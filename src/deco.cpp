#include "deco.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_system.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/md5.h>
#include <mbedtls/rsa.h>

namespace {

constexpr uint32_t DECO_TIMEOUT_MS = 8000;
constexpr size_t SIGN_CHUNK = 53;  // RSA-512 PKCS#1 v1.5: 64 - 11 bytes

// Netlink task only.
char aesKey[17];
char aesIv[17];
char md5Hex[33];
char pwdN[520];
char pwdE[16];
char sigN[520];
char sigE[16];
uint32_t seqBase = 0;
char stok[80];
char sysauth[96];

// Allocated only while decoReadSignal() runs, so the heap is free for TLS otherwise.
struct DecoBuffers {
  char plain[1024];
  char cipherB64[1500];
  char signHex[1100];
  char body[3400];
  char resp[4200];
  uint8_t raw[3100];
  char pwdHex[520];
  char login[640];
};
DecoBuffers *buf = nullptr;
#define plainBuf (buf->plain)
#define cipherB64 (buf->cipherB64)
#define signHex (buf->signHex)
#define bodyBuf (buf->body)
#define respBuf (buf->resp)
#define rawBuf (buf->raw)

int fillRandom(void *, unsigned char *out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

void toHex(const uint8_t *in, size_t len, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = digits[in[i] >> 4];
    out[2 * i + 1] = digits[in[i] & 0x0F];
  }
  out[2 * len] = '\0';
}

// Two quoted strings after "key": ["a","b"].
bool readStringPair(const char *json, const char *key, char *a, size_t aCap, char *b, size_t bCap) {
  const char *p = cs::jsonValueAfterKey(json, key);
  if (!p || *p != '[') {
    return false;
  }
  char *outs[2] = {a, b};
  size_t caps[2] = {aCap, bCap};
  for (int k = 0; k < 2; k++) {
    p = strchr(p, '"');
    if (!p) {
      return false;
    }
    const char *end = strchr(p + 1, '"');
    size_t len = end ? static_cast<size_t>(end - p - 1) : 0;
    if (!end || len == 0 || len >= caps[k]) {
      return false;
    }
    memcpy(outs[k], p + 1, len);
    outs[k][len] = '\0';
    p = end + 1;
  }
  return true;
}

bool readString(const char *json, const char *key, char *out, size_t cap) {
  const char *p = cs::jsonValueAfterKey(json, key);
  if (!p || *p != '"') {
    return false;
  }
  const char *end = strchr(p + 1, '"');
  size_t len = end ? static_cast<size_t>(end - p - 1) : 0;
  if (!end || len == 0 || len >= cap) {
    return false;
  }
  memcpy(out, p + 1, len);
  out[len] = '\0';
  return true;
}

// RSA PKCS#1 v1.5 with a hex public key; appends the hex result (key length) to out.
bool rsaEncryptHex(const char *nHex, const char *eHex, const uint8_t *in, size_t len, char *out, size_t outCap) {
  mbedtls_rsa_context rsa;
  mbedtls_rsa_init(&rsa, MBEDTLS_RSA_PKCS_V15, 0);
  mbedtls_mpi n;
  mbedtls_mpi e;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&e);
  bool ok = mbedtls_mpi_read_string(&n, 16, nHex) == 0 && mbedtls_mpi_read_string(&e, 16, eHex) == 0 &&
            mbedtls_rsa_import(&rsa, &n, nullptr, nullptr, nullptr, &e) == 0 && mbedtls_rsa_complete(&rsa) == 0;
  size_t keyLen = mbedtls_rsa_get_len(&rsa);
  uint8_t enc[256];
  size_t used = strlen(out);
  ok = ok && keyLen <= sizeof(enc) && len + 11 <= keyLen && used + 2 * keyLen < outCap &&
       mbedtls_rsa_pkcs1_encrypt(&rsa, fillRandom, nullptr, MBEDTLS_RSA_PUBLIC, len, in, enc) == 0;
  if (ok) {
    toHex(enc, keyLen, out + used);
  }
  mbedtls_mpi_free(&n);
  mbedtls_mpi_free(&e);
  mbedtls_rsa_free(&rsa);
  return ok;
}

// PKCS7 + AES-128-CBC (ASCII key and IV) + base64 into cipherB64.
bool aesEncrypt(const char *plain) {
  size_t len = strlen(plain);
  size_t padded = (len / 16 + 1) * 16;
  if (padded > sizeof(rawBuf)) {
    return false;
  }
  uint8_t *block = rawBuf;
  memcpy(block, plain, len);
  memset(block + len, static_cast<int>(padded - len), padded - len);
  uint8_t iv[16];
  memcpy(iv, aesIv, 16);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  bool ok = mbedtls_aes_setkey_enc(&aes, reinterpret_cast<const uint8_t *>(aesKey), 128) == 0 &&
            mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, iv, block, block) == 0;
  mbedtls_aes_free(&aes);
  size_t olen = 0;
  return ok &&
         mbedtls_base64_encode(reinterpret_cast<uint8_t *>(cipherB64), sizeof(cipherB64), &olen, block, padded) == 0;
}

// base64 -> AES-128-CBC decrypt -> unpad, into out (text).
bool aesDecrypt(const char *b64, size_t b64Len, char *out, size_t cap) {
  size_t len = 0;
  if (mbedtls_base64_decode(rawBuf, sizeof(rawBuf), &len, reinterpret_cast<const uint8_t *>(b64), b64Len) != 0 ||
      len == 0 || len % 16 != 0) {
    return false;
  }
  uint8_t iv[16];
  memcpy(iv, aesIv, 16);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  bool ok = mbedtls_aes_setkey_dec(&aes, reinterpret_cast<const uint8_t *>(aesKey), 128) == 0 &&
            mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, len, iv, rawBuf, rawBuf) == 0;
  mbedtls_aes_free(&aes);
  if (!ok) {
    return false;
  }
  uint8_t pad = rawBuf[len - 1];
  if (pad == 0 || pad > 16 || pad > len || len - pad >= cap) {
    return false;
  }
  memcpy(out, rawBuf, len - pad);
  out[len - pad] = '\0';
  return true;
}

// POST and keep the response text in respBuf. wantCookie captures sysauth.
int post(const char *url, const char *body, bool wantCookie) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(DECO_TIMEOUT_MS);
  http.setTimeout(DECO_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    return -1;
  }
  http.addHeader("Content-Type", "application/json");
  if (sysauth[0]) {
    char cookie[112];
    snprintf(cookie, sizeof(cookie), "sysauth=%s", sysauth);
    http.addHeader("Cookie", cookie);
  }
  const char *keys[] = {"Set-Cookie"};
  if (wantCookie) {
    http.collectHeaders(keys, 1);
  }
  int code = http.POST(reinterpret_cast<uint8_t *>(const_cast<char *>(body)), strlen(body));
  respBuf[0] = '\0';
  if (code > 0) {
    String text = http.getString();
    snprintf(respBuf, sizeof(respBuf), "%s", text.c_str());
    if (wantCookie) {
      String cookie = http.header("Set-Cookie");
      int start = cookie.indexOf("sysauth=");
      if (start >= 0) {
        start += 8;
        int end = cookie.indexOf(';', start);
        String value = end > start ? cookie.substring(start, end) : cookie.substring(start);
        snprintf(sysauth, sizeof(sysauth), "%s", value.c_str());
      }
    }
  }
  http.end();
  return code;
}

// Encrypts plain, signs it and posts it; decrypted reply text goes to plainBuf.
bool securePost(const char *url, const char *plain, bool login) {
  if (!aesEncrypt(plain)) {
    return false;
  }
  char signText[160];
  if (cs::buildDecoSignText(signText, sizeof(signText), login, aesKey, aesIv, md5Hex, seqBase, strlen(cipherB64)) ==
      0) {
    return false;
  }
  signHex[0] = '\0';
  size_t textLen = strlen(signText);
  for (size_t pos = 0; pos < textLen; pos += SIGN_CHUNK) {
    size_t n = textLen - pos < SIGN_CHUNK ? textLen - pos : SIGN_CHUNK;
    if (!rsaEncryptHex(sigN, sigE, reinterpret_cast<const uint8_t *>(signText + pos), n, signHex, sizeof(signHex))) {
      return false;
    }
  }
  int len = snprintf(bodyBuf, sizeof(bodyBuf), "sign=%s&data=", signHex);
  if (len <= 0 || static_cast<size_t>(len) >= sizeof(bodyBuf) ||
      cs::urlEncode(bodyBuf + len, sizeof(bodyBuf) - len, cipherB64) == 0) {
    return false;
  }
  int code = post(url, bodyBuf, login);
  if (code != 200) {
    Serial.printf("Deco: HTTP %d\n", code);
    return false;
  }
  const char *data = cs::jsonValueAfterKey(respBuf, "data");
  if (!data || *data != '"') {
    return false;
  }
  const char *end = strchr(data + 1, '"');
  return end && aesDecrypt(data + 1, static_cast<size_t>(end - data - 1), plainBuf, sizeof(plainBuf));
}

bool readSignal(const char *host, const char *password, cs::DecoSignal &sig);

}  // namespace

bool decoReadSignal(const char *host, const char *password, cs::DecoSignal &sig) {
  sig = cs::DecoSignal();
  if (!host[0] || !password[0]) {
    return false;
  }
  buf = static_cast<DecoBuffers *>(malloc(sizeof(DecoBuffers)));
  if (!buf) {
    Serial.println("Deco: not enough memory");
    return false;
  }
  bool ok = readSignal(host, password, sig);
  free(buf);
  buf = nullptr;
  return ok;
}

namespace {

bool readSignal(const char *host, const char *password, cs::DecoSignal &sig) {
  sysauth[0] = '\0';
  stok[0] = '\0';
  char url[160];

  snprintf(url, sizeof(url), "http://%s/cgi-bin/luci/;stok=/login?form=keys&operation=read", host);
  if (post(url, "", false) != 200 || !readStringPair(respBuf, "password", pwdN, sizeof(pwdN), pwdE, sizeof(pwdE))) {
    Serial.println("Deco: no password key");
    return false;
  }
  snprintf(url, sizeof(url), "http://%s/cgi-bin/luci/;stok=/login?form=auth&operation=read", host);
  double seq = 0;
  if (post(url, "", false) != 200 || !readStringPair(respBuf, "key", sigN, sizeof(sigN), sigE, sizeof(sigE)) ||
      !cs::jsonReadNumber(cs::jsonValueAfterKey(respBuf, "seq"), seq)) {
    Serial.println("Deco: no auth key");
    return false;
  }
  seqBase = static_cast<uint32_t>(seq);

  uint8_t random[16];
  esp_fill_random(random, sizeof(random));
  toHex(random, 8, aesKey);
  toHex(random + 8, 8, aesIv);
  char hashInput[cs::DECO_PASS_MAX + 8];
  snprintf(hashInput, sizeof(hashInput), "admin%s", password);
  uint8_t digest[16];
  if (mbedtls_md5_ret(reinterpret_cast<const uint8_t *>(hashInput), strlen(hashInput), digest) != 0) {
    return false;
  }
  toHex(digest, 16, md5Hex);

  char *pwdHex = buf->pwdHex;
  char *login = buf->login;
  pwdHex[0] = '\0';
  if (!rsaEncryptHex(pwdN, pwdE, reinterpret_cast<const uint8_t *>(password), strlen(password), pwdHex,
                     sizeof(buf->pwdHex))) {
    Serial.println("Deco: password encryption failed");
    return false;
  }
  snprintf(login, sizeof(buf->login), "{\"params\":{\"password\":\"%s\"},\"operation\":\"login\"}", pwdHex);
  snprintf(url, sizeof(url), "http://%s/cgi-bin/luci/;stok=/login?form=login", host);
  if (!securePost(url, login, true) || !readString(plainBuf, "stok", stok, sizeof(stok)) || !sysauth[0]) {
    Serial.println("Deco: login failed");
    return false;
  }

  snprintf(url, sizeof(url), "http://%s/cgi-bin/luci/;stok=%s/admin/network?form=internet", host, stok);
  bool ok = securePost(url, "{\"operation\":\"read\"}", false) && cs::parseDecoSignal(plainBuf, sig);
  if (!ok) {
    Serial.println("Deco: no mobile signal in reply");
  }

  snprintf(url, sizeof(url), "http://%s/cgi-bin/luci/;stok=%s/admin/system?form=logout", host, stok);
  securePost(url, "{\"operation\":\"logout\"}", false);
  sysauth[0] = '\0';
  return ok;
}

}  // namespace

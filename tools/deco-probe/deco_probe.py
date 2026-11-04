#!/usr/bin/env python3
"""Read-only probe: can a TP-Link Deco (X1500-4G) give us the LTE cell ID?

Run it on a laptop joined to the van's Deco WiFi. It:

1. fetches the Deco's own local web pages and lists every
   ``admin/<module>?form=<name>`` request they reference;
2. logs in with the owner password (typed at the prompt, never stored);
3. sends ``{"operation": "read"}`` to each of those forms plus a few
   known ones, skipping anything that sounds like it changes state;
4. writes every reply to ``deco_dump.json`` (private, may contain IMEI,
   IMSI, client names) and a short ``cell_summary.json`` with only the
   cell and signal fields, which is safe to share.

Nothing is ever written to the router: the only operations sent are
"read" and, at the end, "logout".
"""

import argparse
import getpass
import json
import os
import re
import sys
from json import dumps
from urllib.parse import urljoin

import requests
from tplinkrouterc6u import TPLinkDecoClient

DEFAULT_HOST = "http://192.168.68.1"

# Forms known from the tplinkrouterc6u Deco client (read operations only).
KNOWN_FORMS = [
    "admin/network?form=internet",       # mobile_cpe block: rsrp, rsrq, snr, network_type
    "admin/network?form=wan_ipv4",
    "admin/network?form=performance",
    "admin/device?form=device_list",
]

# Guesses for the 4G pages. Harmless if they don't exist (read only).
GUESSED_FORMS = [
    "admin/network?form=mobile_cpe",
    "admin/network?form=cell_lock",
    "admin/network?form=lte",
    "admin/network?form=mobile",
    "admin/network?form=sim",
    "admin/network?form=signal",
    "admin/mobile?form=status",
    "admin/mobile?form=cell_lock",
    "admin/mobile?form=signal",
    "admin/lte?form=status",
    "admin/cpe?form=status",
]

# Never touch forms whose name suggests an action.
UNSAFE = re.compile(
    r"reboot|reset|restore|factory|upgrade|firmware|update|logout|login|"
    r"backup|delete|remove|write|set_|apply|save|reconnect|dial|sms|ussd|"
    r"pin|puk|unlock|lock_set|scan_start|disconnect",
    re.IGNORECASE,
)

FORM_RE = re.compile(r"(admin/[A-Za-z0-9_]+)\?form=([A-Za-z0-9_]+)")
SCRIPT_RE = re.compile(r"""<script[^>]+src=["']([^"']+)["']""", re.IGNORECASE)
LINK_RE = re.compile(r"""["']([^"']+\.(?:js|html?))(?:\?[^"']*)?["']""", re.IGNORECASE)

# Keys that matter for geolocation (Google wants mcc, mnc, tac, 28-bit ECI).
CELL_KEY = re.compile(
    r"cell|eci|enb|tac|lac|mcc|mnc|plmn|pci|earfcn|arfcn|band|rsrp|rsrq|"
    r"rssi|sinr|snr|network_type|operator|isp|profile|signal",
    re.IGNORECASE,
)
PRIVATE_KEY = re.compile(r"imei|imsi|iccid|msisdn|phone|number|password|pwd|key", re.IGNORECASE)

START_PAGES = ["/", "/webpages/index.html", "/webpages/login.html"]
MAX_FILES = 80


def crawl_forms(host, timeout):
    """Fetch the web UI (no login needed for static files) and collect forms."""
    seen_urls = set()
    queue = [urljoin(host + "/", p.lstrip("/")) for p in START_PAGES]
    forms = set()
    fetched = 0
    while queue and fetched < MAX_FILES:
        url = queue.pop(0)
        if url in seen_urls or not url.startswith(host):
            continue
        seen_urls.add(url)
        try:
            r = requests.get(url, timeout=timeout)
        except requests.RequestException as e:
            print(f"  skip {url}: {e.__class__.__name__}")
            continue
        fetched += 1
        if r.status_code != 200:
            continue
        text = r.text
        for module, form in FORM_RE.findall(text):
            forms.add(f"{module}?form={form}")
        for ref in SCRIPT_RE.findall(text) + LINK_RE.findall(text):
            if ref.startswith(("http://", "https://")) and not ref.startswith(host):
                continue
            queue.append(urljoin(url, ref))
    print(f"  fetched {fetched} web UI files, found {len(forms)} form references")
    return forms


def find_cell_fields(obj, path=""):
    """Yield (path, value) for keys that look cell/signal related."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            p = f"{path}.{k}" if path else k
            if isinstance(v, (dict, list)):
                yield from find_cell_fields(v, p)
            elif CELL_KEY.search(k):
                value = "<redacted>" if PRIVATE_KEY.search(k) else v
                yield p, value
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from find_cell_fields(v, f"{path}[{i}]")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST, help=f"Deco address (default {DEFAULT_HOST})")
    ap.add_argument("--out", default=".", help="folder for deco_dump.json and cell_summary.json")
    ap.add_argument("--timeout", type=int, default=10)
    ap.add_argument("--no-crawl", action="store_true", help="only try the known and guessed forms")
    args = ap.parse_args()
    host = args.host.rstrip("/")
    if not host.startswith(("http://", "https://")):
        host = "http://" + host

    forms = set(KNOWN_FORMS) | set(GUESSED_FORMS)
    if not args.no_crawl:
        print("1. Reading the Deco web UI for form names...")
        forms |= crawl_forms(host, args.timeout)

    skipped = sorted(f for f in forms if UNSAFE.search(f.split("form=")[1]))
    forms = sorted(f for f in forms if f not in skipped)
    if skipped:
        print(f"  not calling {len(skipped)} action-like forms: {', '.join(skipped)}")

    password = os.environ.get("DECO_PASSWORD") or getpass.getpass(
        "2. Deco owner password (TP-Link ID password, not shown): ")
    client = TPLinkDecoClient(host, password, timeout=args.timeout)
    try:
        client.authorize()
    except Exception as e:
        print(f"Login failed: {e}")
        print("Check the address, the password, and that no other admin session is open.")
        return 1
    print("  logged in")

    print(f"3. Reading {len(forms)} forms...")
    dump = {}
    summary = {}
    try:
        for form in forms:
            try:
                data = client.request(form, dumps({"operation": "read"}), ignore_errors=True)
                dump[form] = data
                hits = dict(find_cell_fields(data or {}))
                ok = isinstance(data, dict) and data.get("error_code", 0) == 0
                print(f"  {'ok ' if ok else 'err'} {form}" + (f"  <- {len(hits)} cell/signal fields" if hits else ""))
                if hits:
                    summary[form] = hits
            except Exception as e:
                dump[form] = {"probe_error": str(e)[:300]}
                print(f"  fail {form}: {e.__class__.__name__}")
    finally:
        try:
            client.logout()
        except Exception:
            pass

    os.makedirs(args.out, exist_ok=True)
    dump_path = os.path.join(args.out, "deco_dump.json")
    summary_path = os.path.join(args.out, "cell_summary.json")
    with open(dump_path, "w", encoding="utf-8") as f:
        json.dump(dump, f, indent=2, ensure_ascii=False, default=str)
    with open(summary_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, ensure_ascii=False, default=str)

    print("4. Done.")
    print(f"  full dump (keep private): {dump_path}")
    print(f"  cell summary (safe to share): {summary_path}")
    wanted = ("cell", "eci", "tac", "mcc", "mnc", "plmn")
    found = sorted({p for hits in summary.values() for p in hits if any(w in p.lower() for w in wanted)})
    if found:
        print("  Candidate cell ID fields:")
        for p in found:
            print(f"    {p}")
    else:
        print("  No cell ID / TAC / MCC / MNC field found in any reply.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

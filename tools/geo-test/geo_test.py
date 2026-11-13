#!/usr/bin/env python3
"""Laptop test of ChargeScreen's Wi-Fi positioning.

Reads the Wi-Fi networks Windows can see (netsh), applies the same filters
as the firmware (own networks, locally administered MACs, "_nomap"), keeps
the 15 strongest, and asks Google's Geolocation API where they are, with
considerIp=false like the device. The API key is typed at the prompt (or
taken from GEO_KEY) and only sent to Google.

Needs Windows Location services on (Settings > Privacy & security >
Location), otherwise netsh refuses to list networks.
"""

import argparse
import getpass
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

GEO_URL = "https://www.googleapis.com/geolocation/v1/geolocate?key="
KEEP = 15


def netsh(*args):
    out = subprocess.run(["netsh", "wlan", *args], capture_output=True, text=True, errors="replace")
    return out.stdout + out.stderr


def connected_ssids():
    text = netsh("show", "interfaces")
    return {m.group(1).strip() for m in re.finditer(r"^\s+SSID\s+:\s*(.+)$", text, re.M)}


def scan():
    text = netsh("show", "networks", "mode=bssid")
    if "location permission" in text.lower():
        sys.exit("Windows blocks the Wi-Fi list: turn on Location services in Settings > Privacy & security > "
                 "Location (also 'Let desktop apps access your location'), then run this again.")
    aps = []
    ssid = ""
    current = None
    for line in text.splitlines():
        m = re.match(r"^SSID \d+ : (.*)$", line)
        if m:
            ssid = m.group(1).strip()
            continue
        m = re.search(r"BSSID \d+\s*:\s*([0-9a-fA-F:]{17})", line)
        if m:
            current = {"ssid": ssid, "mac": m.group(1).lower(), "pct": None, "channel": None}
            aps.append(current)
            continue
        if current is None:
            continue
        m = re.search(r"Signal\s*:\s*(\d+)%", line)
        if m:
            current["pct"] = int(m.group(1))
            continue
        m = re.search(r"Channel\s*:\s*(\d+)", line)
        if m:
            current["channel"] = int(m.group(1))
    return aps


def usable(ap, own):
    first = int(ap["mac"].split(":")[0], 16)
    if first & 0x02:
        return False, "locally administered (phone hotspot / random MAC)"
    if ap["ssid"] and ap["ssid"] in own:
        return False, "own network"
    if ap["ssid"].endswith("_nomap"):
        return False, "_nomap"
    if ap["pct"] is None:
        return False, "no signal value"
    return True, ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--own", action="append", default=[],
                    help="extra SSID to ignore, like the device's WiFi 1 / WiFi 2 (the connected one is ignored anyway)")
    args = ap.parse_args()

    own = connected_ssids() | set(args.own)
    found = scan()
    kept, dropped = [], []
    for a in found:
        ok, why = usable(a, own)
        (kept if ok else dropped).append((a, why))
    kept = sorted((a for a, _ in kept), key=lambda a: -a["pct"])[:KEEP]

    print(f"Windows sees {len(found)} access points; {len(kept)} usable after the firmware's filters.")
    for a, why in dropped:
        print(f"  dropped {a['mac']} {a['ssid'][:24]!r}: {why}")
    for a in kept:
        # Windows reports signal quality in %, roughly 2 * (dBm + 100).
        a["dbm"] = round(a["pct"] / 2 - 100)
        print(f"  using   {a['mac']} {a['dbm']:>4} dBm ch {a['channel']}  {a['ssid'][:24]!r}")
    if len(kept) < 2:
        sys.exit("Fewer than 2 usable networks: the device would send 'Position unknown (none)' here.")

    key = os.environ.get("GEO_KEY") or getpass.getpass("Google geolocation key (not shown): ").strip()
    body = {
        "considerIp": False,
        "wifiAccessPoints": [
            {"macAddress": a["mac"], "signalStrength": a["dbm"], **({"channel": a["channel"]} if a["channel"] else {})}
            for a in kept
        ],
    }
    req = urllib.request.Request(GEO_URL + key, data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            code, reply = resp.status, json.loads(resp.read())
    except urllib.error.HTTPError as e:
        code, reply = e.code, json.loads(e.read() or b"{}")

    if code == 200:
        loc = reply["location"]
        print(f"\nHTTP 200: {loc['lat']:.5f}, {loc['lng']:.5f}  accuracy {reply['accuracy']:.0f} m")
        print(f"Map: https://maps.google.com/?q={loc['lat']:.5f},{loc['lng']:.5f}")
    else:
        errors = reply.get("error", {}).get("errors", [{}])
        print(f"\nHTTP {code}: {errors[0].get('reason', '?')} - {reply.get('error', {}).get('message', '')}")
        print("404 notFound = Google doesn't know these networks; 403/400 = key, billing or API not enabled.")


if __name__ == "__main__":
    main()

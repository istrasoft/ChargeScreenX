#!/usr/bin/env python3
"""Passive BLE survey: what is broadcasting inside the van?

Listens to Bluetooth Low Energy advertisements for a few minutes (no
pairing, no connections, nothing sent) and prints one line per device,
labelled when the manufacturer or service is a known sensor type.
Phones, laptops and other personal devices (Apple, Microsoft, Google,
Samsung beacons, exposure notifications) are counted but not listed
unless --all is given, so neighbours' devices are not recorded.

Results also go to ble_survey.json for decoding later.
"""

import argparse
import asyncio
import json
import time

from bleak import BleakScanner

# Bluetooth SIG company identifiers seen in manufacturer data.
MANUFACTURERS = {
    0x02E1: "Victron Energy (Instant Readout)",
    0x0499: "Ruuvi (environment sensor)",
    0x0059: "Nordic Semiconductor (often Mopeka tank sensors, many DIY sensors)",
    0xEC88: "Govee (thermometer / hygrometer)",
    0x0131: "Cypress (often cheap BMS / sensor modules)",
    0x05A7: "Sonos",
    0x0157: "Anhui Huami (Amazfit / Mi Band)",
}
PERSONAL_MANUFACTURERS = {
    0x004C: "Apple",
    0x0006: "Microsoft",
    0x00E0: "Google",
    0x0075: "Samsung",
    0x0087: "Garmin",
}

# 16-bit service UUIDs seen in service data / service lists.
SERVICES = {
    "fcd2": "BTHome (open sensor format, many thermometers)",
    "fe95": "Xiaomi MiBeacon (Mi thermometers and sensors)",
    "181a": "Environmental Sensing (e.g. Xiaomi LYWSD03MMC with custom firmware)",
    "feaa": "Eddystone beacon",
    "ff00": "Vendor service 0xFF00 (often JBD / LiFePO4 BMS)",
    "ffe0": "Vendor service 0xFFE0 (often serial-over-BLE modules, BMS)",
    "fff0": "Vendor service 0xFFF0 (often serial-over-BLE modules, BMS)",
}
PERSONAL_SERVICES = {
    "fd6f": "Exposure notification (phone)",
    "fe9f": "Google Fast Pair (phone / earbuds)",
    "fe2c": "Google Fast Pair (phone / earbuds)",
}


def short_uuid(uuid: str) -> str:
    """'0000fcd2-0000-1000-8000-00805f9b34fb' -> 'fcd2'; longer UUIDs stay as they are."""
    u = uuid.lower()
    if u.startswith("0000") and u.endswith("-0000-1000-8000-00805f9b34fb"):
        return u[4:8]
    return u


def classify(rec) -> tuple[str, bool]:
    """Return (label, personal)."""
    labels = []
    personal = False
    for mid in rec["manufacturer"]:
        mid = int(mid)
        if mid in MANUFACTURERS:
            labels.append(MANUFACTURERS[mid])
        elif mid in PERSONAL_MANUFACTURERS:
            labels.append(PERSONAL_MANUFACTURERS[mid])
            personal = True
        else:
            labels.append(f"manufacturer 0x{mid:04X}")
    for uuid in set(rec["service_data"]) | set(rec["services"]):
        s = short_uuid(uuid)
        if s in SERVICES:
            labels.append(SERVICES[s])
        elif s in PERSONAL_SERVICES:
            labels.append(PERSONAL_SERVICES[s])
            personal = True
    if not labels:
        labels.append("unknown")
    # A named, non-personal manufacturer wins over a personal service hint.
    return "; ".join(dict.fromkeys(labels)), personal


async def survey(seconds: int):
    devices = {}

    def on_advert(device, adv):
        rec = devices.setdefault(device.address, {
            "address": device.address,
            "name": "",
            "count": 0,
            "rssi_min": 0,
            "rssi_max": -200,
            "manufacturer": {},
            "service_data": {},
            "services": [],
            "first_seen": time.time(),
        })
        rec["count"] += 1
        name = adv.local_name or device.name or ""
        if name:
            rec["name"] = name
        if adv.rssi is not None:
            rec["rssi_max"] = max(rec["rssi_max"], adv.rssi)
            rec["rssi_min"] = adv.rssi if rec["count"] == 1 else min(rec["rssi_min"], adv.rssi)
        for mid, data in adv.manufacturer_data.items():
            rec["manufacturer"][str(mid)] = data.hex()
        for uuid, data in adv.service_data.items():
            rec["service_data"][uuid] = data.hex()
        for uuid in adv.service_uuids:
            if uuid not in rec["services"]:
                rec["services"].append(uuid)

    scanner = BleakScanner(detection_callback=on_advert)
    await scanner.start()
    try:
        for left in range(seconds, 0, -10):
            print(f"  listening... {left} s left, {len(devices)} devices so far", flush=True)
            await asyncio.sleep(min(10, left))
    finally:
        await scanner.stop()
    return devices


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=int, default=180, help="how long to listen (default 180)")
    ap.add_argument("--all", action="store_true", help="also list phones, laptops and other personal devices")
    ap.add_argument("--out", default="ble_survey.json")
    args = ap.parse_args()

    print(f"Listening for BLE advertisements for {args.seconds} s. Nothing is sent or paired.")
    devices = asyncio.run(survey(args.seconds))

    rows = []
    hidden = 0
    for rec in devices.values():
        label, personal = classify(rec)
        rec["label"] = label
        rec["personal"] = personal
        if personal and not args.all:
            hidden += 1
            continue
        rows.append(rec)
    rows.sort(key=lambda r: -r["rssi_max"])

    print()
    print(f"{'RSSI':>5}  {'Adverts':>7}  {'Address':<17}  {'Name':<22}  What it looks like")
    for r in rows:
        print(f"{r['rssi_max']:>5}  {r['count']:>7}  {r['address']:<17}  {r['name'][:22]:<22}  {r['label']}")
    if hidden:
        print(f"\n{hidden} phones / laptops / personal devices not listed (use --all to show them).")
    print("Stronger RSSI (closer to 0) means closer. Devices inside the van are usually above -75.")

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump([r for r in devices.values() if args.all or not r["personal"]], f, indent=2)
    print(f"Saved {args.out}")


if __name__ == "__main__":
    main()

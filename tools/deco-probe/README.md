# Deco cell ID probe

A read-only script that checks whether the van's TP-Link Deco X1500-4G gives out the LTE cell details (MCC, MNC, TAC and the 28-bit cell ID) on its local web login. If it does, ChargeScreen can add the cell to its Google geolocation requests. See `design/specs/2026-11-04-parking-position-design.md`.

It uses the open-source [tplinkrouterc6u](https://pypi.org/project/tplinkrouterc6u/) library for the encrypted Deco login. It only sends `read` requests, then `logout`. It never changes a router setting.

## Run it

1. Join a laptop to the van's Deco WiFi.
2. Close the Deco app on your phone (the Deco may allow only one admin session).
3. In this folder:

```bash
python -m venv .venv
```

```bash
.venv/Scripts/python -m pip install -r requirements.txt
```

```bash
.venv/Scripts/python deco_probe.py
```

On macOS or Linux, use `.venv/bin/python` instead.

The password prompt wants the Deco owner's password, the same TP-Link ID password the Deco app uses. It is not stored or written anywhere.

If the Deco is not at `192.168.68.1`, add `--host 192.168.x.1`, using the gateway address shown in your laptop's WiFi details.

## Output

| File | Contents | Share? |
|---|---|---|
| `deco_dump.json` | Every reply, in full | No. It may contain the IMEI, IMSI, WAN IP and the names of connected devices. |
| `cell_summary.json` | Only the cell and signal fields, with private keys redacted | Yes |

The script ends with a list of candidate cell ID fields, or says none were found.

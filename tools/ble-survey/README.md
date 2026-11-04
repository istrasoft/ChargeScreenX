# BLE survey

Lists every Bluetooth Low Energy device broadcasting inside the van, so we can see which sensors ChargeScreen could read passively, the same way it reads the Victron devices. It only listens: no pairing, no connections, nothing sent.

Known types are labelled: Victron, BTHome and Xiaomi thermometers, Govee, Ruuvi, likely Mopeka tank sensors, and BMS-style serial modules. Phones, laptops and other personal devices are counted but not listed, so nearby people's devices aren't recorded.

## Run it

Sit in the van with the laptop's Bluetooth on, and switch on everything you'd normally have running (fridge, heater, camera, inverter). Then, in this folder:

```bash
python -m venv .venv
```

```bash
.venv/Scripts/python -m pip install -r requirements.txt
```

```bash
.venv/Scripts/python ble_survey.py
```

It listens for 3 minutes (`--seconds 300` for longer) and then prints one line per device, strongest signal first. Devices inside the van usually show an RSSI above -75.

`ble_survey.json` holds the raw advertisement bytes for decoding. It is safe to share; personal devices are left out unless you add `--all`.

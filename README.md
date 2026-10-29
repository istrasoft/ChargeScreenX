# Standalone ESP32-C3 Battery Dashboard

Target board: JCZN ESP32-2424S012 with ESP32-C3-MINI-1U, 240x240 GC9A01 round LCD, CST816 touch, 4 MB flash.

Goal: a standalone BLE dashboard for a Victron 300A SmartShunt and an ECO-WORTHY 12V 100Ah LiFePO4 battery with Bluetooth. It should not need Home Assistant, Wi-Fi, MQTT, or a gateway after flashing.

## Best Firmware Route

Use Arduino/PlatformIO for the device firmware.

ESPHome/Home Assistant can help for experiments, but the final device is better as native firmware because:

- Victron Instant Readout is BLE advertising data, usually encrypted with a per-device key.
- ECO-WORTHY batteries vary by model/BMS; many appear to use JBD-like BLE or serial protocols, but that needs confirmation.
- The round display and touch UI need tighter memory control on this ESP32-C3 board, which has no PSRAM.

Victron SmartShunt, BMV, SmartSolar, and similar devices can broadcast Instant Readout data via BLE manufacturer data. The manufacturer ID is `0x02E1`, and Instant Readout records use record type `0x10`.

For the Victron shunt the dashboard should listen passively. It does not need to pair or open a Bluetooth connection for normal Instant Readout data.

## Settings Hotspot

Tap the dots at the bottom of any dashboard screen to open Settings. The WiFi row starts a temporary hotspot named `ChargeScreen`. Connect to it and open `http://192.168.4.1/` to paste Victron Instant Readout keys for the battery monitor, solar controller, and Orion XS.

Keys entered there are stored in the ESP32 flash preferences area. No Victron keys are compiled into the firmware. The setup hotspot turns itself off after 5 minutes without webpage activity.

## Screens

- Battery (always shown), Solar (shown when a solar key is saved), Orion XS DC-DC (shown when an Orion key is saved). Demo mode shows all screens.
- Screen cycle: Off, 10, 20, 30, or 60 seconds, set on settings page 3 or on the setup page. Cycling pauses after a touch.

## Device Lock

Each Victron device kind locks to the first address that decodes correctly three times in a row, so nearby vans or boats with Victron gear are ignored. The lock is released if the device is not heard for 10 minutes or its key is changed. The setup page shows the locked address.

## Online reports

ChargeScreen can join WiFi only when it has something to send, and push to an HTTP endpoint:

- a high-drain alert when the battery discharges faster than a limit over a rolling window (default 4.0 A over 45 minutes), and an all-clear when it is back to normal;
- a morning battery report (default 10:00) with an estimate of the days remaining;
- an evening solar report (default 20:00, after production ends, 23:00 at the latest);
- a silent data batch of 5-minute records (default every 30 minutes).

Set it up in the **Online** section of the setup page: up to two WiFi networks, the URL, and optionally a POSIX time zone such as `CET-1CEST,M3.5.0,M10.5.0/3` (otherwise the time zone is looked up from the IP address once a day). Use an ntfy topic URL such as `https://ntfy.sh/your_topic` and subscribe to it in the ntfy app, or your own endpoint, which receives JSON (`{"v":1,"kind":...,"title":...,"message":...,"priority":...,"time":...,"data":{...}}`). **Send test message** checks the setup once the hotspot turns off. The info screen shows `Online: Off`, `Waiting`, `No WiFi`, `Sent HH:MM` or `Error HH:MM`.

While offline, up to 24 hours of records and 16 alerts or reports are kept on the device and sent later; late messages say when they happened. Battery capacity is learned from the shunt, so the remaining Ah and days estimates appear after a while. Nothing is sent in demo mode, and TLS certificates are not checked, so don't use it with an endpoint that needs a password.

## Flashing

This fork is flashed over USB only (`pio run -t upload`); there is no web firmware update. It uses a single 3 MB app partition. Unit tests for the device-independent logic run with `pio test -e native`.

## License

ChargeScreen firmware is source-available for personal, educational, evaluation, and non-commercial use only. Commercial sale or use is not permitted without written permission from the copyright holder. See [LICENSE](LICENSE).

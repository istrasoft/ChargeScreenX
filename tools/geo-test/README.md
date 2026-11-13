# Wi-Fi positioning test

Checks the Google geolocation key and how accurate Wi-Fi positioning is at the van's current spot, without driving. It uses the same filters and request as the firmware. Only Python's standard library is needed.

1. Turn on Windows Location services: Settings > Privacy & security > Location, including "Let desktop apps access your location". Windows won't list Wi-Fi networks to scripts without it. You can switch it off again afterwards.
2. Run it and type the key at the prompt:

```bash
py -3.12 geo_test.py
```

It ignores the network the laptop is connected to (the Deco). Add `--own "Phone hotspot"` for the device's second WiFi network if it has one.

The result is the position, the accuracy radius and a Google Maps link. `404 notFound` means Google doesn't know the networks around you; `403` or `400` means a problem with the key, billing or the enabled API.

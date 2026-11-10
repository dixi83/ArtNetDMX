# ArtNetDMX for ESP-07S

ESP8266 firmware that receives Art-Net `ArtDMX` packets on UDP port 6454 and continuously transmits DMX512 on UART0 TX, GPIO1. The device uses Wi-Fi station mode when configured. If it cannot connect before the fallback timeout, it starts `ArtNetDMX-Setup` and keeps DMX stopped until a user presses **Run DMX**.

## Build and flash

Use PlatformIO with the `esp07s` environment:

```sh
pio run -e esp07s
pio run -e esp07s -t upload
```

The first boot has no saved Wi-Fi network, so it starts the setup AP immediately. Join `ArtNetDMX-Setup` (password `artnetdmx`) and open `http://192.168.4.1/`. Configure Wi-Fi, then press **Run DMX** to reveal the settings page. Saving settings restarts the device. With saved Wi-Fi credentials, it starts in Run mode by default. Press **Stop DMX** to access settings.

## Settings

- Wi-Fi SSID and password, plus setup AP password.
- Device IP: `0.0.0.0` uses DHCP; gateway and subnet are used for static IP.
- Art-Net sender host filter: `0.0.0.0` accepts any sender.
- Start/end DMX addresses, inclusive, 1–512. Only those source slots are updated; other slots remain at their last value (zero after boot).
- Wi-Fi fallback timeout, 0–3600 seconds. On timeout, Wi-Fi station mode is replaced by the setup AP, Run is stopped and no DMX is transmitted.
- Firmware upload is available at `http://<device-ip>/update` without an OTA password. Stop DMX, open the upload page, and upload `.pio/build/esp07s/firmware.bin`. The device restarts after a successful update. Anyone with access to the device's network can upload firmware, so use this on the trusted network you described.
- DMX output test: while stopped in Wi-Fi mode, enter two channel numbers (1–512) and one shared value (0–255), then start the test. Those two channels are transmitted continuously at that value; all other channels are zero. Press **Stop DMX** to end the test and return to Art-Net mode.

## Wiring

- ESP-07S GPIO1 / TX → DMX transceiver DI (driver input).
- ESP-07S GPIO and transceiver must share ground.
- Connect the transceiver's differential output to the DMX line through a suitable isolated/RS-485 DMX driver, with correct polarity and termination for the line. Tie driver enable (`DE`) high and receiver enable (`/RE`) low if the transceiver has those pins. Do not connect the ESP8266 pin directly to the DMX cable; DMX uses a differential physical layer and an RS-485 transceiver is required.
- The attachment was not available in the project/session. Based on the supplied pin correction, the external RGB status LED uses R=GPIO12, G=GPIO13, B=GPIO14. The firmware assumes a common-cathode LED (active high); each LED channel needs a suitable series resistor. For a common-anode LED, set `RGB_ACTIVE_LOW` to `true` in `src/main.cpp`. Status colors: red = stopped or setup AP, blinking blue = Wi-Fi connecting, paired green pulses = recent Art-Net, slow blue heartbeat = connected without recent Art-Net.

## Notes

UART0 is configured for 250000 baud, 8 data bits, no parity, 2 stop bits. Each frame starts with a 100 µs break and 12 µs mark-after-break. Art-Net universe routing is not filtered: valid ArtDMX packets from the accepted sender update the configured DMX slot range.

OTA uses the ESP8266 updater and the ESP-07's 1 MB flash layout. The current build is about 320 KB; ensure future images continue to fit alongside the running image in the available sketch area. DMX must be stopped before an OTA upload is accepted.

## License

This project is source-available under the [PolyForm Noncommercial License 1.0.0](LICENSE). You may use, modify, and redistribute it for noncommercial purposes, including personal and hobby projects. Commercial use is not permitted by this license; contact the copyright holder for separate commercial licensing.

Because this license restricts commercial use, this project is not open source under the Open Source Definition. See the [OSI's Open Source Definition](https://opensource.org/osd).

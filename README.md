# Sun Sync ESP32-S3 firmware

This PlatformIO project targets the ESP32-S3-DevKitC-1 with 16 MB flash and 8 MB PSRAM. It serves the captive-portal `index.html` page and its static files from LittleFS. The login and admin dashboard are hosted by the Fly.io backend, and the kiosk pushes live state to it with `POST /devices/telemetry` every 15 s. The ESP32 exposes live, read-only battery and panel measurements at `GET /api/sensors`; authentication, settings, sessions, sales, and persistence remain backend responsibilities.

## Hardware defaults

The firmware defaults below support two independently read INA219 sensors and two PCF8574-backed 16x2 LCDs on one shared I2C bus. They remain unverified on physical hardware until the modules are installed and the bus is scanned.

| Module signal | ESP32-S3 connection | Notes |
| --- | --- | --- |
| Shared I2C SDA | GPIO 8 | Connect SDA from both INA219 boards and both LCD backpacks to this bus. |
| Shared I2C SCL | GPIO 9 | Connect SCL from both INA219 boards and both LCD backpacks to this bus. |
| Battery INA219 VCC / GND | 3V3 / GND | Firmware address `0x40`; this sensor measures the battery branch. |
| Panel INA219 VCC / GND | 3V3 / GND | Firmware address `0x41`; set the breakout address pads/jumpers accordingly. |
| Panel PCF8574 LCD SDA / SCL | GPIO 8 / GPIO 9 | Firmware address `0x27`; displays panel voltage, power, and current. |
| Battery PCF8574 LCD SDA / SCL | GPIO 8 / GPIO 9 | Firmware address `0x26`; set its A0-A2 address jumpers so it differs from `0x27`. |
| Router relay input | GPIO 16 | Firmware requests ON during startup and keeps it ON; configure the relay input polarity below. |
| Mobile charging-port relay input | GPIO 17 | Firmware initializes OFF; turns ON only during an active charging session and turns OFF on pause, end, or expiry. |
| LCD module power | Per module specification | If a backpack is powered at 5V, ensure its SDA/SCL pull-ups do not pull the ESP32 pins to 5V. Use a bidirectional I2C level shifter or a verified 3.3V-safe setup. |
| Coin acceptor pulse output | GPIO 5, through an optocoupler/level interface | Keep disabled until the interface and pulse polarity are verified; never connect the acceptor signal directly. |
| Common reference | ESP32 GND to low-voltage module/interface GND | For an isolated optocoupler, follow its output-side wiring requirements; do not defeat isolation by joining grounds unnecessarily. |

All four I2C devices share the same two signal wires; they do **not** each get separate GPIO pins. Each device on the bus must have a unique 7-bit I2C address. The PCF8574 address range is normally `0x20`-`0x27`; `0x26` and `0x27` require different A0-A2 jumper settings. Scan the bus to confirm the actual addresses before connecting every module. If two devices have an address conflict that cannot be changed, use an I2C multiplexer rather than wiring them with the same address directly together.

For each INA219, put its high-side shunt in series with the **positive lead of the individual branch being measured**: source positive to `VIN+`, branch/load positive to `VIN-`. Do not place both sensors in series as if they measured the same branch unless that is specifically intended. Check the breakout's voltage/current rating and the expected load before energizing it.

ESP32-S3 GPIOs are **not 5V tolerant**. Keep I2C pull-ups and the coin-interface output at safe logic levels. Many LCD backpacks are powered from 5V and pull SDA/SCL up to 5V: use a bidirectional I2C level shifter or another verified 3.3V-safe arrangement. The coin input remains disabled by default. Confirm that the replacement hardware matches this pinout before wiring or enabling inputs.

The relay inputs are configured in `include/wifi_config.h`: GPIO 16 is the router channel and GPIO 17 is the charging channel. `SUNSYNC_RELAY_ACTIVE_LEVEL` defaults to `LOW`, common on two-channel relay modules, but the actual board's trigger polarity has not been verified. Check the relay module markings/datasheet and test the logic with the router and charger disconnected before connecting loads; change the setting to `HIGH` if required. Confirm that the relay inputs accept 3.3V logic; do not drive a relay coil directly from an ESP32 GPIO. The firmware initializes the router channel ON and charger channel OFF before mounting LittleFS or starting Wi-Fi. Hardware pull resistors and relay contact wiring determine the state during ESP32 reset/boot, before firmware executes; ensure hardware keeps the charger relay OFF during reset and do not rely on software alone for a fail-safe router power path. Use appropriately rated relay contacts and an isolated supply/wiring arrangement for the router and charger loads.

## Coin payment window

The Pay endpoint opens a 60-second window. Firmware counts debounced falling-edge signal pulses only while that window is open; pulses outside it are ignored. Credits remain associated with the browser's registered device ID after the window closes and are consumed when that device starts a service session. This balance is held in RAM and is lost on restart. Each signal pulse currently equals one credit unit, not a peso amount; configure the acceptor's pulse-to-denomination mapping before using credits to price service.

Coin input is disabled by default (`SUNSYNC_COIN_SIGNAL_ENABLED 0` in `include/wifi_config.h`) because the coin acceptor signal has not been wired to the ESP32. Only enable it after installing and testing a suitable optocoupler/level interface and connecting its 3.3 V logic output to GPIO 5. Never connect a 12 V coin signal directly to an ESP32 pin. The software window does not electrically inhibit the acceptor, so coins may still be physically accepted outside the window even though firmware will not credit them.

`GET /api/payment/status?device_id=<id>` reports the caller's pulse credits and window state. `POST /api/payment/open?device_id=<id>` opens the window; another device cannot take over an active window or unspent balance. Connect and Charge are enabled in the page only when the reported balance is positive and there is no active session. Starting either service creates a timed backend session using that service's configured seconds-per-credit rate; the ESP32 validates and consumes the presented credits. GPIO 17 is enabled only for charging sessions. Pause turns the charger output off and resume turns it on again; expiry turns it off automatically. The Wi-Fi session is recorded, but this firmware build still does not support upstream internet forwarding/NAPT, so a Wi-Fi session does not yet provide internet access.

## Wi-Fi

The firmware runs in AP+station mode: it always starts the local Sun Sync access point and, when `SUNSYNC_WIFI_SSID` and `SUNSYNC_WIFI_PASSWORD` are set, it also connects to that upstream router without blocking the HTTP server. The AP name is `SUNSYNC_AP_SSID-<board-id>` and remains open unless an AP password of at least 8 characters is configured. Connect to the printed AP name and browse to `http://192.168.4.1/`.

The current bundled ESP-IDF build has IPv4 forwarding/NAPT disabled. Therefore, upstream Wi-Fi association can be displayed, but connected AP clients do not yet receive internet access. The firmware reports this explicitly through `GET /api/health`; it will not claim internet sharing is enabled. Enabling NAT requires rebuilding against an ESP-IDF/lwIP configuration with IP forwarding, IPv4 NAPT, and L2-to-L3 copy enabled and validated. Per-client paid internet gating also needs per-client forwarding/firewall enforcement, not just a browser-side button.

`GET /api/wifi/clients` reports AP-associated client MAC addresses, RSSI, and DHCP IP addresses when available. DHCP hostnames are not currently available from this Arduino SDK's AP client-list API. The Wi-Fi Status tab allows friendly names to be assigned locally in the current browser. Per-client downloaded byte counts are explicitly unavailable: this SDK does not expose reliable per-station NAT traffic counters. The existing backend session/download summary remains separate and reflects backend-recorded sessions.

## Build and upload

Install PlatformIO and the Espressif32 platform, then run these commands from this directory:

```powershell
pio run
pio run -t uploadfs
pio run -t upload
pio device monitor
```

The pre-build script `scripts/gzip_web_assets.py` gzips the HTML, CSS and JS in `data/`. Re-upload the filesystem after changing portal assets.

LittleFS must be uploaded before first boot. The custom 16 MB partition table reserves 5 MB for firmware and about 10.9 MiB for LittleFS, plus standard NVS and OTA metadata partitions. HTTP page routes are `/` and `/index.html`.

## Sensor API

`GET /api/sensors` returns two INA219 reading objects: `ina219` is the battery sensor at `0x40`, and `ina219_panel` is the panel sensor at `0x41`. Each object reports `available`, bus voltage, shunt voltage, current, and power. A sensor that was not detected at startup has `available: false` and JSON `null` readings; one missing sensor does not suppress the other. The dashboard uses the battery object for Charging Status and the panel object for its direct panel readings. The backend-powered Solar Status history remains separate. The dashboard polls this endpoint every two seconds.

The panel LCD (`0x27`) and battery LCD (`0x26`) each show the corresponding sensor's voltage and power on the first row and current on the second. A missing LCD is logged at startup and does not prevent the server or the other display from operating. Both LCD backpacks share the ESP32 I2C bus and must use the PCF8574-compatible backpack pin mapping expected by the `LiquidCrystal_PCF8574` driver.

The web pages are served locally without depending on a CDN. HTTP is handled by ESPAsyncWebServer/AsyncTCP; sensor refresh and captive DNS remain lightweight loop/event work. HTML, CSS, and JavaScript are gzip-compressed during the PlatformIO build; the server negotiates gzip with browsers and allows static assets to be cached for an hour while revalidating HTML pages. Backend API base URLs retain the existing localhost setting during local development; device APIs use same-origin paths on the ESP32.

## Configuration

Copy `include/wifi_config.example.h` to `include/wifi_config.h` and fill in Wi-Fi, MQTT, backend URL and device key. `wifi_config.h` is gitignored; never commit it.


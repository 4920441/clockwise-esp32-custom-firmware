# Clockwise ESP32 Custom Firmware

**Custom firmware & soldering-free OTA flash method for the "ClockWise Plus" ESP32 HUB75 64x64 LED matrix pixel clock sold on AliExpress.**

The stock firmware (by topyuan.top) has no user-accessible OTA upload and the USB-C port is **power-only** (no data lines). This project documents how to replace the firmware over WiFi using DNS spoofing of the built-in auto-update mechanism — no soldering, no serial adapter needed.

![OTA update in progress](images/ota-update-in-progress.png)

## Hardware

This is a 64x64 RGB LED matrix panel (HUB75E, P3 pitch) with a custom ESP32 controller board plugged into the back via the standard HUB75 16-pin connector.

| Component | Details |
|-----------|---------|
| MCU | ESP32-WROOM-32E (ESP32-D0WD-V3 rev 3.1), Dual Core, 240MHz |
| Display | 64x64 RGB LED matrix, HUB75E interface, FM6126A driver IC |
| USB-C | **Power only** — no D+/D- data lines connected |
| Serial header | 4-pin pad H2 (3V3, RX, TX, GND) — unpopulated |
| Buttons | BOOT (IO0 / SW5) + RESET (EN / SW4) — may be unpopulated |
| LDR | GPIO 35 (analog, for auto-brightness) |
| Buzzer | GPIO 2 (via transistor) |
| Power | 5V via USB-C, AMS1117-3.3 regulator for ESP32 |

![Panel back with ESP32 board](images/panel-back-overview.png)
![ESP32 board closeup — ESP32-WROOM-32E](images/esp32-board-closeup.png)
![Board back showing HUB75 connector](images/board-back-hub75-connector.png)

### HUB75E Pin Mapping

Extracted from the [PCB schematic](schematic/clockwise-pcb-schematic.pdf) and confirmed by binary analysis of the stock firmware:

| HUB75E Pin | Signal | ESP32 GPIO |
|-----------|--------|------------|
| 1 | R1 | IO25 |
| 2 | G1 | IO26 |
| 3 | B1 | IO27 |
| 4 | GND | - |
| 5 | R2 | IO14 |
| 6 | G2 | IO12 |
| 7 | B2 | IO13 |
| 8 | E | **IO18** |
| 9 | A | IO23 |
| 10 | B | IO19 |
| 11 | C | IO5 |
| 12 | D | IO17 |
| 13 | CLK | IO16 |
| 14 | LAT | IO4 |
| 15 | OE | IO15 |
| 16 | GND | - |

> **Note:** Pins R1 through D and CLK/LAT/OE were confirmed by extracting the `i2s_pins` struct from the stock firmware binary at offset `0x0019c6`. The stock firmware compiles with **E=-1** in the pin struct but sets it to GPIO 18 at runtime. The E pin was determined experimentally by cycling through candidates.

## Stock Firmware: ClockWise Plus

The stock firmware is "ClockWise Plus" by topyuan.top (based on the open-source [Clockwise](https://github.com/jnthas/clockwise) project). It features animated clock faces (Super Mario, Pac Man, Nyan Cat, etc.) configurable via a web interface.

### Stock Firmware API

The clock exposes a web server on port 80:

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/get` | GET | Returns all settings as HTTP **headers** (204 response) |
| `/set` | POST | Save a setting (`key=value` form data) |
| `/restart` | POST | Reboot device |
| `/erase` | POST | Erase WiFi credentials |
| `/read?pin=N` | GET | Read analog pin value |
| `/basic` | GET | Fallback config page |

### Stock OTA Protocol (Reverse-Engineered)

The firmware checks for updates over plain HTTP:

1. **Heartbeat**: `GET http://www.topyuan.top/ledhub75/check?id=<chipid>&ver=<version>&...`
2. **Update check**: `GET http://www.topyuan.top/ledhub75/updatecheck?id=<chipid>&ver=<version>`
   - Server returns the latest version as plain text (e.g., `3.11`)
3. **Firmware download** (if server version > local version):
   `GET http://www.topyuan.top/ledhub75/firmware/<version>.bin`

All communication uses **plain HTTP** (not HTTPS) to `www.topyuan.top` (port 80).

The firmware files are publicly browsable at `https://topyuan.top/ledhub75/firmware/`.

**Security note**: The stock firmware sends your WiFi SSID and password in plain text to topyuan.top on every check-in. The heartbeat URL includes `&ssid=...&pass=...` as query parameters.

## Flashing Custom Firmware (No Soldering)

### Prerequisites

- A machine on the same network as the clock
- Control over your DNS server (or the ability to override DNS for one host)
- Python 3
- nginx (or any HTTP server/reverse proxy on port 80)
- [PlatformIO](https://platformio.org/) (to compile firmware)

### Step 1: Build the Bridge Firmware

The bridge firmware connects to your WiFi, drives the display with a test pattern, and provides a web-based OTA upload at `http://<clock-ip>/update` for all future flashing.

```bash
cd bridge-firmware

# Edit src/main.cpp — set your WiFi SSID and password
# (lines 23-24)

platformio run
```

The compiled binary will be at `.pio/build/esp32/firmware.bin`.

### Step 2: Set Up the OTA Spoof Server

Copy the firmware binary:
```bash
cp bridge-firmware/.pio/build/esp32/firmware.bin ota-spoof/firmware.bin
```

Start the spoof server:
```bash
cd ota-spoof
python3 server.py
```

This listens on port 8088 and serves:
- `/ledhub75/updatecheck` — returns version `99.0` (triggering the update)
- `/ledhub75/firmware/99.0.bin` — serves your firmware binary
- `/ledhub75/check` — responds to heartbeat

### Step 3: Configure nginx

Install the nginx config to route `www.topyuan.top` requests to the spoof server:

```bash
sudo cp ota-spoof/topyuan-spoof.conf /etc/nginx/conf.d/
sudo nginx -t && sudo nginx -s reload
```

### Step 4: DNS Override

Point `www.topyuan.top` to the IP of the machine running the spoof server. How you do this depends on your setup:

- **Router/DNS server**: Add an A record for `www.topyuan.top` pointing to your server's IP
- **dnsmasq**: `address=/www.topyuan.top/192.168.x.x`
- **Pi-hole**: Local DNS record

Make sure the clock can reach this IP (check routing if they're on different subnets).

### Step 5: Trigger the Update

Restart the clock to make it check for updates:
```bash
curl -X POST http://<clock-ip>/restart
```

The clock will:
1. Boot and connect to WiFi
2. Call `www.topyuan.top/ledhub75/updatecheck` (hitting your server)
3. See version `99.0` > `3.11` and download the firmware
4. Flash itself and reboot with the new firmware

You'll see the requests in the spoof server and nginx logs.

**The OTA is safe**: if the firmware binary is invalid, the ESP32's OTA verification rejects it and rolls back to the previous firmware automatically.

### Step 6: Done!

After reboot, the display shows a test pattern and the IP address. From now on, flash any firmware at:

```
http://<clock-ip>/update
```

**Note:** ElegantOTA v3 uses a JavaScript-based upload UI. Use the **browser** at `http://<clock-ip>/update` to upload `.bin` files — `curl`-based uploads are unreliable with this library.

Clean up:
- Remove DNS override for `www.topyuan.top`
- `sudo rm /etc/nginx/conf.d/topyuan-spoof.conf && sudo nginx -s reload`

## Display Issues

### E Pin Mystery (Work in Progress)

The 64x64 panel requires an E address line (HUB75E) for 1/32 scan addressing. However, the stock Clockwise firmware compiles with **E=-1** (disconnected) in the library's pin struct. Despite this, the stock firmware displays correctly on 64x64 panels.

**What we've observed:**
- The stock firmware works perfectly on the 64x64 panel
- Replacing it via OTA (without power cycling) also works — because the stock firmware already initialized the FM6126A panel hardware
- After a **cold boot** (power cycle), custom firmware shows display artifacts: row overlapping, garbled text, and blank lines — symptoms of incorrect E pin or address line configuration

**E pin candidates tested:**

| GPIO | Result |
|------|--------|
| -1 | Rows overlap (top half folds onto bottom half) |
| 32 | Garbled display after power cycle |
| 33 | Garbled display after power cycle |
| **18** | **CORRECT — clean display, all rows working** |
| 22 | Garbled display after power cycle |
| 8 | **BRICKED** — GPIO 8 is SPI flash data line, crashes bootloader |
| 2 | Not tested (buzzer pin) |

**Resolved:** The correct E pin is **GPIO 18**. The stock firmware sets this at runtime (not visible in the compiled pin struct default). Confirmed working after cold boot with FM6126A driver.

### FM6126A Driver

The panel uses FM6126A LED driver ICs which require a special initialization sequence at power-on. Set the driver in the library config:

```cpp
mxconfig.driver = HUB75_I2S_CFG::FM6126A;
```

Without this, the display shows garbled output even with correct pin mapping.

## Serial Recovery

If the device becomes unresponsive (e.g., from a bad GPIO configuration), recovery requires the serial header since USB-C carries no data.

### Dangerous GPIOs — Do NOT Use

**Never configure these GPIOs as HUB75 outputs — they are used for SPI flash and will crash the ESP32:**

| GPIO | Function |
|------|----------|
| 6 | Flash CLK |
| 7 | Flash D0 (SD0) |
| 8 | Flash D1 (SD1) |
| 9 | Flash D2 (SD2) |
| 10 | Flash D3 (SD3) |
| 11 | Flash CMD |

### Serial Header (H2) Wiring

```
Board H2        USB-to-Serial Adapter (CP2102/CH340)
--------        ------------------------------------
RX     -------> TX   (crossed!)
TX     -------> RX   (crossed!)
GND    -------> GND
3V3              (don't connect — power via USB-C)
```

### Entering Download Mode

On the ESP32-WROOM-32E module:
- **IO0** (GPIO 0, pin 25 on module) — must be held LOW during reset
- **EN** (pin 3 on module) — pull briefly LOW to reset

Procedure:
1. Connect serial adapter to H2
2. Hold **BOOT** button (IO0 → GND)
3. While holding BOOT, press and release **RESET** (EN → GND)
4. Release BOOT
5. ESP32 is now in download mode

If BOOT/RESET buttons are not populated, bridge the pins directly on the ESP32-WROOM-32E module.

### NVS Recovery (Erase Bad Settings)

If the device is stuck in a boot loop due to a bad saved setting:

```bash
esptool.py --port /dev/ttyUSB0 erase-region 0x9000 0x5000
```

This erases only the NVS partition (saved preferences), restoring default settings without reflashing firmware.

### Full Reflash

```bash
esptool.py --port /dev/ttyUSB0 --baud 460800 write_flash 0x10000 firmware.bin
```

## Building Your Own Firmware

Use the pin mapping above with the [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) library:

```cpp
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

HUB75_I2S_CFG::i2s_pins pins = {
    25, 26, 27,        // R1, G1, B1
    14, 12, 13,        // R2, G2, B2
    23, 19, 5, 17, 18, // A, B, C, D, E
    4, 15, 16           // LAT, OE, CLK
};

HUB75_I2S_CFG mxconfig(64, 64, 1);
mxconfig.gpio = pins;
mxconfig.clkphase = false;
mxconfig.driver = HUB75_I2S_CFG::FM6126A;
```

Include ElegantOTA in your project so you keep wireless upload capability.

## Panel64 Firmware

The [`panel64-firmware/`](panel64-firmware/) directory contains a complete replacement firmware turning the clock into a self-contained smart dashboard. No Python proxy, no companion app — just the ESP32 talking directly to your existing infrastructure.

### Features

- **Solari split-flap (Fallblattanzeiger) idle display** — date, time, outside temperature, solar yield today (kWh), current power (W)
  - Uses a physical drum model: characters rotate through intermediate glyphs with a split-flap flip animation and asynchronous per-cell timing (the cascading "waterfall" effect)
  - Color-coded watts line: green when producing (exporting), red when importing, white when balanced
- **LDR auto-brightness** — GPIO 35, ambient-light driven, smoothed to avoid flicker
- **NTP clock** — with DST-aware POSIX timezone string
- **Direct InfluxDB queries** — one combined multi-query HTTP request every 30 seconds. No intermediate proxy server needed.
- **MQTT rotating values** — the temperature row cycles through any number of user-configured MQTT topics. Background "sniff" mode discovers both sub-topics and JSON payload fields (for zigbee2mqtt style devices). Configure via `http://<ip>/mqtt`
- **UDP video mode** — receive 64x64 RGB888 frames on UDP port 5005 (12288 bytes/frame, 4-byte chunked protocol). Video and animations play whenever frames arrive; after 5 seconds of no packets, falls back to the split-flap display.
- **Web OTA** — drop a new `firmware.bin` at `http://<ip>/update` (ElegantOTA v3)
- **MQTT debug page** — `/mqtt/debug` shows connection state, configured items, last 10 received topics, and total message count for diagnostics

### Architecture

```
  ┌──────────────┐    WiFi     ┌──────────────┐
  │    Panel64   │◄──────────► │  WiFi router │
  │  (ESP32 +    │             └──────────────┘
  │   HUB75)     │                    │
  └──────┬───────┘                    │
         │                            ▼
         │                   ┌────────────────┐
         │◄──── HTTP ────────│   InfluxDB     │ solar & temperature
         │                   │  :8086         │
         │                   └────────────────┘
         │
         │                   ┌────────────────┐
         │◄──── MQTT ───────►│  MQTT broker   │ rotating values,
         │                   │  :1883         │ zigbee devices, etc.
         │                   └────────────────┘
         │
         │                   ┌────────────────┐
         │◄──── NTP ─────────│  NTP server    │
         │                   └────────────────┘
         │
         │◄──── UDP ─────────   any sender (video, pixel art, scripts)
         │      :5005
         │
         ▼
   HTTP ElegantOTA at :80/update  (future firmware flashes)
```

No Python services, no proxies — the device talks directly to standard services you probably already have. MQTT is optional; if not configured, the firmware still works (just no rotating MQTT items).

### Configuration

Edit `panel64-firmware/src/main.cpp`:

```cpp
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";
const char* NTP_SERVER = "pool.ntp.org";
const char* TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";
const char* INFLUX_HOST = "192.168.1.10";
const int   INFLUX_PORT = 8086;
const char* INFLUX_DB   = "telegraf";
```

If you don't have InfluxDB, comment out the `fetchData()` call and the display will just show the clock and any MQTT values you configure. Adjust the queries in `INFLUX_MULTI_Q` to match your schema (the default queries are tuned for a SolaX + EcoFlow + Panasonic heat pump setup — see comments in the source).

### MQTT rotation items

Visit `http://<ip>/mqtt`:
- Set broker host/port/credentials
- Set rotation interval (seconds)
- Enter a topic prefix (e.g., `panasonic_heat_pump/main` or `zigbee2mqtt/YourDevice`) and click **Start** — the ESP32 subscribes in the background and lists discovered sub-topics AND JSON fields from leaf topics
- Pick an entry from the dropdown, add a 4-char label and 1-2 char unit, click Add
- Works with zigbee2mqtt-style devices that publish JSON payloads: the `temperature`, `humidity`, `battery` fields etc. are discovered automatically

### UDP video sender

The [`ota-spoof/send-video.py`](ota-spoof/send-video.py) script pushes any video/GIF/image to the panel:

```bash
# Play a GIF (loops forever)
python3 send-video.py --host <panel-ip> animation.gif

# Play a video
python3 send-video.py --host <panel-ip> --fps 25 --loop video.mp4

# Pipe from ffmpeg for screen capture, webcam, etc.
ffmpeg -i input.mp4 -vf scale=64:64 -pix_fmt rgb24 -f rawvideo pipe:1 | \
  python3 send-video.py --host <panel-ip> --raw --fps 25
```

## Related Projects

- [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) — The HUB75 display driver library
- [Clockwise](https://github.com/jnthas/clockwise) — The original open-source clock project
- [ClockWise Plus Tutorial](https://topyuan.top/clock/en/) — Stock firmware documentation
- [sjh007/hub75-64-64](https://github.com/sjh007/hub75-64-64) — Similar PCB with schematic
- [ESP32 Trinity](https://esp32trinity.com/) — Open-source ESP32 HUB75 board (different hardware)

## License

MIT

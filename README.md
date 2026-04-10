# Clockwise ESP32 Custom Firmware

**Custom firmware & soldering-free OTA flash method for the "ClockWise Plus" ESP32 HUB75 64x64 LED matrix pixel clock sold on AliExpress.**

The stock firmware (by topyuan.top) has no user-accessible OTA upload and the USB-C port is **power-only** (no data lines). This project documents how to replace the firmware over WiFi using DNS spoofing of the built-in auto-update mechanism — no soldering, no serial adapter needed.

![OTA update in progress](images/ota-update-in-progress.png)

## Hardware

This is a 64x64 RGB LED matrix panel (HUB75E, P3 pitch) with a custom ESP32 controller board plugged into the back via the standard HUB75 16-pin connector.

| Component | Details |
|-----------|---------|
| MCU | ESP32-WROOM-32E (original ESP32, not S2/S3) |
| Display | 64x64 RGB LED matrix, HUB75E interface, 1/32 scan |
| USB-C | **Power only** — no D+/D- data lines connected |
| Serial header | 4-pin pad (3V3, RX, TX, GND) — unpopulated |
| Buttons | BOOT (IO0) + RESET (EN) on the PCB |
| LDR | GPIO 34 (analog, for auto-brightness) |
| Buzzer | GPIO 2 (via transistor) |
| Power | 5V via USB-C, AMS1117-3.3 regulator for ESP32 |

![Panel back with ESP32 board](images/panel-back-overview.png)
![ESP32 board closeup — ESP32-WROOM-32E](images/esp32-board-closeup.png)
![Board back showing HUB75 connector](images/board-back-hub75-connector.png)

### HUB75E Pin Mapping

Extracted from the [PCB schematic](schematic/clockwise-pcb-schematic.pdf):

| HUB75E Pin | Signal | ESP32 GPIO |
|-----------|--------|------------|
| 1 | R1 | IO25 |
| 2 | G1 | IO26 |
| 3 | B1 | IO27 |
| 4 | GND | - |
| 5 | R2 | IO14 |
| 6 | G2 | IO12 |
| 7 | B2 | IO13 |
| 8 | E | IO32 |
| 9 | A | IO23 |
| 10 | B | IO19 |
| 11 | C | IO5 |
| 12 | D | IO17 |
| 13 | CLK | IO16 |
| 14 | LAT | IO4 |
| 15 | OE | IO15 |
| 16 | GND | - |

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

After reboot, the display shows a rainbow test pattern with "BRIDGE FW / OTA ready" and the IP address. From now on, flash any firmware at:

```
http://<clock-ip>/update
```

Clean up:
- Remove DNS override for `www.topyuan.top`
- `sudo rm /etc/nginx/conf.d/topyuan-spoof.conf && sudo nginx -s reload`

## Alternative: Serial Flash

If you prefer a wired approach, the PCB has an unpopulated 4-pin serial header (labeled `3V3 RX TX GND` on the silkscreen). Connect a USB-to-serial adapter (CP2102/CH340), hold BOOT, press RESET, and flash with esptool:

```bash
esptool.py --port /dev/ttyUSB0 --baud 460800 write_flash 0x10000 firmware.bin
```

## Building Your Own Firmware

Use the pin mapping above with the [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) library:

```cpp
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

HUB75_I2S_CFG::i2s_pins pins = {
    25, 26, 27,       // R1, G1, B1
    14, 12, 13,       // R2, G2, B2
    23, 19, 5, 17, 32, // A, B, C, D, E
    4, 15, 16          // LAT, OE, CLK
};

HUB75_I2S_CFG mxconfig(64, 64, 1);
mxconfig.gpio = pins;
mxconfig.clkphase = false;
mxconfig.driver = HUB75_I2S_CFG::FM6126A;
```

Include ElegantOTA in your project so you keep wireless upload capability.

## Related Projects

- [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) — The HUB75 display driver library
- [Clockwise](https://github.com/jnthas/clockwise) — The original open-source clock project
- [ClockWise Plus Tutorial](https://topyuan.top/clock/en/) — Stock firmware documentation
- [sjh007/hub75-64-64](https://github.com/sjh007/hub75-64-64) — Similar PCB with schematic
- [ESP32 Trinity](https://esp32trinity.com/) — Open-source ESP32 HUB75 board (different hardware)

## License

MIT

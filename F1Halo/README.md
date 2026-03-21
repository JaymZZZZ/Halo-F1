# Halo F1

**A Formula 1 desktop companion for the JC4827W543 (ESP32-S3 + 4.3" TFT)**

Halo F1 is a small, always-on display that shows everything you need about the current F1 season — next race session times in your local timezone, the full drivers' and constructors' championship standings, the latest session results, and F1 news headlines. No apps, no browser tabs, no ads. Just a glance at your desk.

The pre-compiled firmware is free to install from the [project website](https://halof1.com/). This repository contains the full source code for reference and personal use.

---

## Features

- **Session Times** — FP1, FP2, FP3, Sprint Qualifying, Sprint Race, Qualifying and Race start times for the upcoming weekend, automatically converted to your local timezone
- **Drivers' Championship** — Full standings table updated throughout the season; includes a pre-season fallback that populates the driver and constructor roster before the first race
- **Session Results** — Qualifying (Q1/Q2/Q3) and race results fetched from the live OpenF1 API, including gap to leader
- **No Spoiler Mode** — No unwanted spoilers when watching live sessions isn't an option!
- **Latest News** — F1 headlines pulled from an RSS feed (currently English only)
- **Night Mode** — Configurable dimming window; set start/stop times and a separate night brightness level
- **8 Languages** — English, Italian, Spanish, French, Dutch, German, Portuguese, Norwegian
- **Captive-portal Wi-Fi setup** — On first boot the device broadcasts an access point (`Halo-F1`); connect from any phone or laptop to enter your home network credentials. No app or computer required after initial flashing
- **Update notifications** — The device checks for new firmware versions on startup and shows an in-app notification if an update is available

---

## Hardware

| Component    | Detail                                |
| ------------ | ------------------------------------- |
| Board        | **JC4827W543**                        |
| SoC          | ESP32-S3                              |
| Display      | 4.3" TFT, 480 × 272 px                |
| Touch        | Capacitive (recommended) or Resistive |
| Connectivity | Wi-Fi (2.4 GHz), Bluetooth            |

The JC4827W543 is available [on Aliexpress at this link](https://s.click.aliexpress.com/e/_c2xQCrDH), get the capacitive touch version for a nicer look. A snap-fit 3D-printable case (no screws, no glue, prints in PLA on any FDM printer) is available free on [MakerWorld](https://makerworld.com/it/models/2492192-halo-your-f1-desktop-companion).

---

## Flashing

The easiest way to install the firmware is via the project website, directly from Chrome or Edge — no software needed:

**[halof1.com](https://halof1.com/)**

If you want to compile from source, see the [Building from Source](#building-from-source) section below.

**Important:** Make sure to install the ipex antenna on the back of the board, this board won't be able to maintain a stable WiFi connection without it.

---

## Building from Source

### Requirements

- [Arduino IDE](https://www.arduino.cc/en/software) 2.x
- ESP32 Arduino core (`esp32` by Espressif, tested on **3.x**)

### Arduino Libraries

Install the following through the Library Manager (Sketch → Include Library → Manage Libraries).
**IMPORTANT:** This project has been developed over an extended period of time, libraries used in it might have introduced breaking changes in latest updates. To ensure full compatibility please make sure to download the correct versions where indicated

| Library             | Author          |
| ------------------- | --------------- |
| `LVGL v9.3`         | LVGL            |
| `WiFiManager`       | tzapu           |
| `ArduinoJson`       | Benoit Blanchon |
| `bb_spi_lcd v2.7.1` | Larry Bank      |

> **Note:** the included `lv_conf.h` is tuned for the default JC4827W543 profile. If you target JC8048W550 and notice rendering or memory issues, adjust `lv_conf.h` to match your board.

### Board Configuration

`F1Halo.ino` now uses a single board selector define.

```cpp
#define HALO_BOARD_JC4827W543 1
#define HALO_BOARD_JC8048W550 2
#define HALO_BOARD_MODEL HALO_BOARD_JC8048W550 // default
```

To build for JC4827W543, change the last line to:

```cpp
#define HALO_BOARD_MODEL HALO_BOARD_JC4827W543
```

The board profile block in the sketch sets `DISPLAY_TYPE`, touch pins and screen size for each model.

### Compiling and Uploading

1. Clone or download this repository
2. Open `F1Halo.ino` in Arduino IDE
3. If your board is resistive-touch, comment out `#define TOUCH_CAPACITIVE` inside the selected board profile block in `F1Halo.ino`
4. Select **ESP32S3 Dev Module** as the target board
5. Set board options based on selected profile:
   - **JC8048W550 (default profile)**: recommended **Flash Size** = **16MB**, **PSRAM** = **OPI PSRAM (8 MB)**, **Partition Scheme** = `16M Flash (3MB APP/9.9MB FATFS)` (or any 3MB-app partition)
   - **JC4827W543 profile**: **Flash Size** = **4MB**, **PSRAM** = **OPI PSRAM (8 MB)**, **Partition Scheme** = `Huge APP (3MB No OTA/1MB SPIFFS)`
6. Set **USB CDC On Boot** to **enabled** to get Serial feed via USB
7. Connect the board via a USB-A to USB-C data cable
8. If the port does not appear, hold `BOOT`, press `RST`, then release `BOOT` to enter flash mode
9. Click **Upload**

At boot, the firmware prints detected Flash/PSRAM sizes and warns over Serial if they are lower than the selected board profile recommendation.

---

## Data Sources

Halo F1 fetches all data over HTTPS. No account or API key is required.

| Data                          | Source                                       |
| ----------------------------- | -------------------------------------------- |
| Driver standings              | [Jolpica / Ergast F1 API](https://jolpi.ca/) |
| Race calendar & session times | [Jolpica / Ergast F1 API](https://jolpi.ca/) |
| Live session results          | [OpenF1 API](https://openf1.org/)            |
| Timezone offset from IP       | [IP API](https://ipapi.co/)                  |
| Weather Forecast              | [Open Meteo](https://open-meteo.com/)        |
| News headlines                | The Race — RSS feed                          |

### Anonymous Statistics

On startup, and periodically, the device sends a minimal anonymous ping to a server at `we-race.it`. This request carries:

- A randomly generated device UUID (generated once, stored in flash)
- The selected display language
- The configured UTC offset
- The current firmware version

No personal data, Wi-Fi credentials, location or network information is ever transmitted. This telemetry is used solely to count active installs, check for firmware updates and deliver in-app notifications such as release announcements. You can verify this behaviour yourself in the `sendStatisticData()` function inside `wifi_handler.h`.

---

## Project Structure

```
F1Halo.ino            — Main sketch: hardware initialisation, setup(), loop()
ui.h                  — LVGL UI construction and all event handlers
wifi_handler.h        — Wi-Fi setup, all API fetch functions, statistics ping
weather.h             — Weather API calls and utils
localized_strings.h   — Translated string tables for all 8 supported languages
utils.h               — Utility functions (UUID generation, time helpers, etc.)
notifications.h       — In-app notification queue and scheduler
audio.h               — Sound playback via I²S (in the works)
touchscreen.h         — Capacitive touch driver wrapper (GT911)
ESP_I2S.cpp / .h      — I²S audio driver (adapted from the Arduino ESP32 core)
lv_conf.h             — LVGL configuration tuned for the JC4827W543 display
```

---

## Future Developments

- [x] No spoiler mode
- [x] Weather forecast for each session
- [ ] Audio notifications when a new article is fetched
  - [x] Library inclusion and set up
  - [ ] Exclude sound during night times
  - [ ] Add Menu config option
  - [ ] 3D Case rework to fit speaker
- [ ] Add constructors standings, let user choose if to display drivers, constructors or both standings
- [ ] Add timezone override in menu
- [ ] Add more RSS feeds

---

## Copyright & Terms of Use

Copyright © 2026 Fabio Rossato. All rights reserved.

This source code is made publicly available for **personal, non-commercial use only**.

**You are free to:**

- Read, study and learn from the code
- Build and flash the firmware for personal use on your own device
- Fork the repository and make private modifications for personal use

**You are not permitted to:**

- Use this code, in whole or in part, in any commercial product, service or paid project
- Redistribute or republish the source code, compiled firmware or any derivative under a different name or project without explicit written permission
- Remove or alter copyright notices in any file

No open-source license is granted. The absence of a license means this code is **not** open source — all rights not explicitly listed above are reserved by the author. If you are unsure whether your intended use is permitted, open an issue or get in touch.

---

## Acknowledgements

- [LVGL](https://lvgl.io/) — Embedded graphics library
- [WiFiManager](https://github.com/tzapu/WiFiManager) — Captive-portal Wi-Fi configuration by tzapu
- [ArduinoJson](https://arduinojson.org/) — JSON parsing by Benoit Blanchon
- [bb_spi_lcd](https://github.com/bitbank2/bb_spi_lcd) — SPI display driver by Larry Bank
- [Jolpica / Ergast F1 API](https://jolpi.ca/) — Race calendar and standings data
- [OpenF1](https://openf1.org/) — Live session result data
- [Open Meteo](https://open-meteo.com/) — Weather forecast data
- [IP API](https://ipapi.co/) — Timezone offset via IP

---

## Links

- 🌐 **Website & firmware installer** — [halof1.com](https://halof1.com/)
- 🖨 **3D-printable case** — [MakerWorld](https://makerworld.com/it/models/2492192-halo-your-f1-desktop-companion)
- 📸 **Instagram** — [@\fabiotechgarage](https://instagram.com/fabiotechgarage)
- 👾 **Discord Server** - [Invite link](https://discord.gg/fMa5KDeFUV)
- ☕ **Support the project** — [paypal.me/rossatof](https://paypal.me/rossatof)

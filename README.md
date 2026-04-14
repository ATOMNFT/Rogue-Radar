<p align="center">
  <img src="Images/header-image.png" alt="Rogue Radar Header" width="100%" />
</p>

<h1 align="center">Rogue Radar</h1>
<p align="center"><strong>ESP32 multi-tool firmware for WiFi, BLE, GPS, and device utilities.</strong></p>
<p align="center">Supports: LilyGO T-Embed ESP32-S3 | CYD-2USB | NM-CYD-C5 (ESP32-C5)</p>

## 🧭 Version Tracker

| Version | Status | Notes |
|--------|--------|-------|
| v1.0.0 | Stable | Initial public release of the Rogue Radar Firmware |

> **Latest Release:** `v1.0.0` — Rogue Radar Firmware
---

## Overview

**Rogue Radar** is a handheld ESP32-S3 firmware built for the **LilyGO T-Embed** that combines multiple wireless and utility tools into one rotary-driven interface.

The firmware uses **LVGL** for the UI, **TFT_eSPI** for the 320x170 ST7789 display, **BLE + WiFi** features from the ESP32 core, **TinyGPS++** for GPS data, and **APA102 LEDs** for visual status feedback.

It is designed around fast menu navigation, onboard scanning tools, live signal data, GPS stats, SD-based update support, and a clean embedded dashboard feel.

## Screenshots

<p align="center">
  <img src="Images/1.jpg" alt="Rogue Radar boot screen" width="30%" />
  <img src="Images/2.jpg" alt="Rogue Radar Screenshot 2" width="30%" />
  <img src="Images/3.jpg" alt="Rogue Radar Screenshot 3" width="30%" />
</p>

<p align="center">
  <img src="Images/4.jpg" alt="Rogue Radar Screenshot 4" width="45%" />
  <img src="Images/5.jpg" alt="Rogue Radar Screenshot 5" width="45%" />
</p>

---

## Current Tool Set

### WiFi Tools
- **Network Scanner** – scans nearby access points and shows SSID, BSSID, RSSI, channel, and security type.
- **Deauth Detector** – monitors for deauthentication activity using promiscuous mode.
- **Channel Analyzer** – surveys channel activity and signal strength across WiFi channels.
- **PineAP Hunter** – watches for BSSIDs cycling through many SSIDs across scans.
- **Pwnagotchi Watch** – looks for Pwnagotchi beacon behavior and parses status data from beacon SSIDs.
- **Flock Detector** – flags WiFi activity associated with networks containing `flock` in the SSID.

### BLE Tools
- **BLE Scanner** – scans nearby Bluetooth Low Energy devices and lists signal details.
- **AirTag Detector** – identifies AirTag-like BLE activity.
- **Flipper Detector** – looks for BLE patterns associated with Flipper-style devices.
- **Skimmer Detector** – checks for HC-03 / HC-05 / HC-06 type modules often used in skimmer-style builds.
- **Meta Detector** – looks for Meta / Ray-Ban smart-glasses related BLE advertisements.

### Misc Tools
- **Device Info** – shows chip, flash, heap, CPU, SDK, and MAC details.
- **SD Update** – supports firmware update flow from SD card.
- **Brightness** – adjusts the TFT backlight with PWM brightness control.

### GPS Tools
- **GPS Stats** – displays live latitude, longitude, speed, altitude, and satellite data.
- **Wiggle Wars** – included as a GPS menu item for expansion / custom use.

---

## Hardware Target

This firmware supports multiple ESP32-based development boards:

### Supported Boards

| Board | MCU | Display | Input | LED | GPS |
|-------|-----|---------|-------|-----|-----|
| **LilyGO T-Embed** | ESP32-S3 | 320x170 | Rotary Encoder | 7x APA102 | Built-in |
| **CYD-2USB** | ESP32 | 320x240 | BOOT Button | None | None |
| **NM-CYD-C5** | **ESP32-C5** (RISC-V) | 320x240 | BOOT Button | None | None |

### LilyGO T-Embed (Original)
- **ESP32-S3** (Xtensa LX7, Wi-Fi 4)
- **ST7789 320x170 display**
- **Rotary encoder + encoder push button**
- **APA102 LED ring**
- **GPS module over UART**
- **MicroSD card on dedicated HSPI bus**

### CYD-2USB / NM-CYD-C5
- **ESP32** or **ESP32-C5** (RISC-V, Wi-Fi 6)
- **ST7789 320x240 display**
- **BOOT button (GPIO0)** for selection
- **1x WS2812 RGB LED**
- **MicroSD card on VSPI bus**
- **GPS** (external module support, configurable)

**Note**: See `rogue-radar/DEVICE_SUPPORT.md` for detailed configuration and wiring information.

---

## Arduino IDE Setup

### Device Selection
Edit `rogue-radar/config.h` and select your device:
```cpp
// #define DEVICE_T_EMBED_S3    // LilyGO T-Embed ESP32-S3
// #define DEVICE_CYD_2USB      // CYD-2USB ESP32
#define DEVICE_NM_CYD_C5     // NM-CYD-C5 ESP32-C5 (RISC-V)
```

### Board Settings

**LilyGO T-Embed:**
- **Board:** `ESP32S3 Dev Module`
- **Partition Scheme:** `Huge APP`

**CYD-2USB:**
- **Board:** `ESP32 Dev Module`
- **Partition Scheme:** `Huge APP`

**NM-CYD-C5:**
- **Board:** `ESP32C5 Dev Module` (requires ESP32 Core >= 3.0.0)
- **Partition Scheme:** `Huge APP`

### Required libraries
- `TFT_eSPI`
- `lvgl` (the sketch notes target **9.0.0**)
- `RotaryEncoder` by mathertel (T-Embed only)
- `APA102` by Pololu (T-Embed only)
- `Adafruit_NeoPixel` (CYD/C5 only - for WS2812 LED)
- `TinyGPSPlus` (T-Embed only, optional for CYD/C5)
- `TFT_Touch` (by Bodmer, url=https://github.com/Bodmer/TFT_Touch) CYD_2USB need

### ESP32 core features used
- `WiFi`
- `esp_wifi`
- `BLEDevice`
- `BLEScan`
- `SD`
- `Update`

---

## LVGL Notes

Make sure your `lv_conf.h` has these enabled:

```cpp
#define LV_COLOR_DEPTH 16
#define LV_USE_LIST    1
#define LV_USE_LABEL   1
#define LV_USE_BTN     1
#define LV_USE_BAR     1
```

---

## TFT / Display Notes

The firmware supports both **320x170** (T-Embed) and **320x240** (CYD) layouts using **TFT_eSPI**.

You will need a correct `User_Setup.h` for your display configuration:
- **T-Embed**: 320x170, specific SPI pins
- **CYD-2USB/NM-CYD-C5**: 320x240, standard CYD SPI pins
- To change the device, need change the `User_Setup.h` file.

See `rogue-radar/DEVICE_SUPPORT.md` for detailed TFT_eSPI configuration.

The sketch also includes a splash screen system using:
- `splash.h`
- `SPLASH_TIME_MS`

---

## Pin Overview

### LilyGO T-Embed

<details>
<summary><strong>GPS</strong></summary>

- `GPS_RX_PIN 44`
- `GPS_TX_PIN 43`

</details>

<details>
<summary><strong>SD Card (HSPI)</strong></summary>

- `SD_CS   39`
- `SD_SCLK 40`
- `SD_MISO 38`
- `SD_MOSI 41`

</details>

<details>
<summary><strong>Device / UI</strong></summary>

- `POWER_PIN   46`
- `LCD_BL_PIN  15`
- `ENCODER_A   1`
- `ENCODER_B   2`
- `ENCODER_BTN 0`

</details>

<details>
<summary><strong>APA102</strong></summary>

- `APA102_DI  42`
- `APA102_CLK 45`

</details>

### CYD-2USB

<details>
<summary><strong>Display (SPI)</strong></summary>

- `TFT_SCLK  14`
- `TFT_MOSI  13`
- `TFT_MISO  12`
- `TFT_CS    15`
- `TFT_DC     2`
- `TFT_BL    21` (CYD-2USB)

</details>

<details>
<summary><strong>GPS</strong></summary>

- `GPS_RX_PIN 3`
- `GPS_TX_PIN 1`

</details>

<details>
<summary><strong>SD Card (VSPI)</strong></summary>

- `SD_CS    5`
- `SD_SCLK  18`
- `SD_MISO  19`
- `SD_MOSI  23`

</details>


<details>
<summary><strong>Input</strong></summary>

- `BOOT_BTN 0` (used as select button)

</details>

### NM-CYD-C5

<details>
<summary><strong>Display (SPI)</strong></summary>

- `TFT_SCLK  6`
- `TFT_MOSI  7`
- `TFT_MISO  2`
- `TFT_CS    23`
- `TFT_DC    24`
- `TFT_BL    25`
</details>

<details>
<summary><strong>GPS</strong></summary>

- `GPS_RX_PIN 4`
- `GPS_TX_PIN 5`

</details>

<details>
<summary><strong>SD Card (VSPI)</strong></summary>

- `SD_CS    10`
- `SD_SCLK  6`
- `SD_MISO  2`
- `SD_MOSI  7`

</details>

<details>
<summary><strong>Input</strong></summary>

- `BOOT_BTN 0` (used as select button)

</details>

---

## UI / Controls

Rogue Radar is built around a **rotary encoder driven interface** using LVGL input groups.

### Controls
- **Rotate encoder** to move through menus and lists
- **Press encoder** to select items
- **Hold encoder button for 5 seconds** to trigger power-off handling

The APA102 LEDs are also used for menu color feedback and scan animations.

---

## Features at a Glance

- Multi-category tool layout
- WiFi scanning and monitoring tools
- BLE scanning and device-type detection
- GPS live stats
- SD card firmware update path
- Adjustable display brightness
- LED ring startup and scanning effects
- Splash screen support
- Embedded dashboard-style UI

---

## Arduino Installation

__METHOD 1__
1. Open the sketch in **Arduino IDE**.
2. Install the required libraries.
3. Make sure `TFT_eSPI` is configured for your **LilyGO T-Embed**.
4. Make sure your `lv_conf.h` options are enabled.
5. Add your `splash.h` file if you are using the splash screen.
6. Select **ESP32S3 Dev Module**.
7. Set partition scheme to **Huge APP**. (3MB NO OTA/1MB SPIFFS); `NM-CYD-C5` use `8MB with spiffs (3MB APP/1.5MB SPIFFS)`
8. Compile and flash.

__METHOD 2__ <br>

## Web Flash Tool

<a href="https://atomnft.github.io/Rogue-Radar/flash0.html" target="_blank" rel="noopener noreferrer">
  <img src="Images/flash-button.png" alt="Flash-Tool" width="450" height="200">
</a>

---

## Roadmap Ideas

- Add logging/export for scan results
- Add themes
- Add richer BLE classification and filtering
- Expand GPS tool set
- Add more SD card utilities
- Add configurable scan timing and thresholds
- Add icon assets and polish for each tool page

---

## Disclaimer

This project is intended for educational, research, and defensive awareness purposes. Be responsible, follow local laws, and only use wireless analysis features where you are authorized to do so.

---

## Credits

---

## TODOs

- [ ] NM-CYD-C5 support high WiFi Channel.
- [ ] More detail Testing.

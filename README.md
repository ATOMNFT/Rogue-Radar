<p align="center">
  <img src="Images/header-image.png" alt="Rogue Radar Header" width="100%" />
</p>

<h1 align="center">Rogue Radar</h1>
<p align="center"><strong>ESP32-S3 multi-tool firmware for WiFi, BLE, GPS, and device utilities on the LilyGO T-Embed (non CC1101).</strong></p>

## 🧭 Version Tracker

| Version | Status | Notes |
|--------|--------|-------|
| v1.0.2 | Stable | Adds Packet Monitor with live graph and hop presets, Flock Hybrid, nyanBOX/Axon/Tesla detectors, improved AirTag/Flipper/Skimmer detection, extra themes, and menu cleanup |
| v1.0.1 | Stable | Adds display/LED dimming controls, scan defaults, rotation toggle, audio feedback, and improved Flock detection |
| v1.0.0 | Stable | Initial public release of the Rogue Radar Firmware |

> **Latest Release:** `v1.0.2` — Rogue Radar Firmware
---

## Overview

**Rogue Radar** is a handheld ESP32-S3 firmware built for the **LilyGO T-Embed** that combines multiple wireless and utility tools into one rotary-driven interface.

The firmware uses **LVGL** for the UI, **TFT_eSPI** for the 320x170 ST7789 display, **BLE + WiFi** features from the ESP32 core, **TinyGPS++** for GPS data, **APA102 LEDs** for visual status feedback, and optional I2S speaker output for alerts and menu feedback.

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
- **Packet Monitor** – live WiFi packet monitor with channel selection, packet rate, packet type counts, average RSSI, optional channel hopping, and a live bar graph.
- **PineAP Hunter** – watches for BSSIDs cycling through many SSIDs across scans.
- **Pwnagotchi Watch** – looks for Pwnagotchi beacon behavior and parses status data from beacon SSIDs.
- **Flock Detector** – flags WiFi activity associated with Flock-related SSID keywords, deduplicates hits by source MAC, and can show the source MAC in the results.
- **Flock Hybrid** – combines BLE and WiFi Flock-style detection into one scanner with merged results and selectable scan presets.

### BLE Tools
- **BLE Scanner** – scans nearby Bluetooth Low Energy devices and lists signal details.
- **AirTag Detector** – identifies AirTag-like BLE activity using manufacturer data, UUID fallback, and passive Apple Find My / AirTag payload pattern detection.
- **Flipper Detector** – detects Flipper-style BLE devices using name matching, OUI fallback, and UUID detection for Black, White, and Transparent variants; results are selectable with a detail page.
- **Skimmer Detector** – checks for suspicious BLE serial/module names including HC-03, HC-05, HC-06, HC-08, BT-HC05, JDY-31, AT-09, HM-10, CC41-A, MLT-BT05, SPP-CA, and FFD0.
- **Meta Detector** – looks for Meta / Ray-Ban smart-glasses related BLE advertisements.
- **nyanBOX Detector** – detects nyanBOX / Nyan Devices BLE badges and shows name, MAC, RSSI, level, version, age, and Locate Mode.
- **Axon Detector** – detects Axon-style BLE devices by configurable MAC/OUI prefix with detail view and Locate Mode.
- **Tesla Detector** – detects Tesla-style BLE name patterns and shows name, MAC, RSSI, age, signal quality, and detail view.

### GPS Tools
- **GPS Stats** – displays live latitude, longitude, speed, altitude, and satellite data.
- **Wiggle Wars** – included as a GPS menu item for expansion / custom use.

### Misc Tools
- **Device Info** – shows firmware version, chip, flash, heap, CPU, and MAC details.
- **SD Update** – supports firmware update flow from SD card.
- **Brightness** – adjusts the TFT backlight with PWM brightness control.
- **Themes** – switches between built-in UI themes including Dark, Flipper, Matrix, Poseidon, Phantom, Amber, and Tron.
- **Scan Defaults** – adjusts BLE scan time, WiFi scan time, WiFi result limit, deauth hop timing, Flock Hybrid presets, Packet Monitor hopping ON/OFF, and Packet Monitor hop timing for the current session.
- **Dimming** – toggles inactivity-based screen and APA102 LED dimming.
- **LEDs** – toggles the APA102 ring on or off at runtime.
- **Rotation** – switches between normal and flipped landscape orientations.
- **Alert Sound** – toggles detection alert chirps.
- **Menu Sounds** – toggles encoder/menu feedback sounds separately from detection alerts.

---

## Hardware Target

This firmware is currently built around the **LilyGO T-Embed ESP32-S3**.

### Main hardware used
- **ESP32-S3**
- **ST7789 320x170 display**
- **Rotary encoder + encoder push button**
- **APA102 LED ring**
- **GPS module over UART**
- **MicroSD card on dedicated HSPI bus**
- **Optional I2S speaker output**

---

## Arduino IDE Setup

### User_Setup Files
Make a backup of your files before replacing any. Drop the corresponding file/s (found above) for your device into `C:\Users\YOURUSERNAME\Documents\Arduino\libraries\TFT_eSPI-master`. And make sure you choose the correct file in the `User_Setup_Select.h` file.

| File Name |
|---|
| `User_Setup_CYD.h` |
| `User_Setup_CYD2USB.h` |
| `User_Setup_LilyGo_T_Embed_S3.h` |
| `User_Setup_nm_cyd_c5.h` |
| `User_Setup_Select.h` |

### Board settings
- **Board:** `ESP32S3 Dev Module`
- **Partition Scheme:** `Huge APP`

### Required libraries
- `TFT_eSPI`
- `lvgl` (the sketch notes target **9.0.0**)
- `RotaryEncoder` by mathertel
- `APA102` by Pololu
- `TinyGPSPlus`

### ESP32 core features used
- `WiFi`
- `esp_wifi`
- `BLEDevice`
- `BLEScan`
- `SD`
- `Update`
- `driver/i2s`

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

The firmware is written for a **320x170** layout and uses **TFT_eSPI**.
You will need a correct `User_Setup.h` for your T-Embed display configuration.

The sketch also includes a splash screen system using:
- `splash.h`
- `SPLASH_TIME_MS`

---

## Pin Overview

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

<details>
<summary><strong>I2S Speaker</strong></summary>

- `SOUND_I2S_BCLK 7`
- `SOUND_I2S_WCLK 5`
- `SOUND_I2S_DOUT 6`

</details>

---

## UI / Controls

Rogue Radar is built around a **rotary encoder driven interface** using LVGL input groups.

### Controls
- **Rotate encoder** to move through menus and lists
- **Press encoder** to select items
- **Hold encoder button for 5 seconds** to trigger power-off handling

The APA102 LEDs are also used for menu color feedback and scan animations. Optional speaker feedback can provide detection chirps and quiet menu tick/click sounds.

---

## Features at a Glance

- Multi-category tool layout
- WiFi scanning and monitoring tools
- BLE scanning and device-type detection
- GPS live stats
- SD card firmware update path
- Adjustable display brightness and inactivity dimming
- Runtime toggles for dimming, LEDs, sound, menu sounds, and display rotation
- Session-adjustable scan defaults
- LED ring startup and scanning effects
- Optional detection alert chirps
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
7. Set partition scheme to **Huge APP**.
8. Compile and flash.

__METHOD 2__ <br>

## Web Flash Tool

<a href="https://atomnft.github.io/Rogue-Radar/flash0.html" target="_blank" rel="noopener noreferrer">
  <img src="Images/flash-button.png" alt="Flash-Tool" width="450" height="200">
</a>

---

## Roadmap Ideas

- Add logging/export for scan results
- Add richer BLE classification and filtering
- Expand GPS tool set
- Add more SD card utilities
- Add icon assets and polish for each tool page

---

## Disclaimer

This project is intended for educational, research, and defensive awareness purposes. Be responsible, follow local laws, and only use wireless analysis features where you are authorized to do so.

---

## Credits



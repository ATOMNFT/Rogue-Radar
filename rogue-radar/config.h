// ============================================================
//  config.h — Rogue Radar T-Embed ESP32-S3
//  Edit this file to customise pins, limits, and behaviour.
//  Do not edit rogue-radar.ino unless you know what you're doing.
// ============================================================
#pragma once

// ─── Device Name ────────────────────────────────────────────────
// Shown in the main menu header
#define DEVICE_NAME  "Rogue Radar"

// ─── Pin Definitions ────────────────────────────────────────────
#define POWER_PIN       46
#define ENCODER_A        1
#define ENCODER_B        2
#define ENCODER_BTN      0
#define APA102_DI       42
#define APA102_CLK      45
#define NUM_LEDS         7

// ─── LCD Backlight (LEDC PWM) ───────────────────────────────────
#define LCD_BL_PIN      15    // IO15 = TFT backlight
#define LCD_BL_CH        0    // LEDC channel (0-7, must be free)
#define LCD_BL_FREQ   5000    // Hz — above audible range
#define LCD_BL_RES       8    // bits (0-255 range)
#define LCD_BL_DEFAULT 255    // startup brightness (0=off, 255=max)

// ─── Display ────────────────────────────────────────────────────
#define SCREEN_W  320
#define SCREEN_H  170

// ─── GPS ────────────────────────────────────────────────────────
#define GPS_RX_PIN  44
#define GPS_TX_PIN  43
#define GPS_BAUD  9600

// ─── SD Card (HSPI — separate bus from TFT) ─────────────────────
#define SD_CS    39
#define SD_SCLK  40
#define SD_MISO  38
#define SD_MOSI  41

// ─── SD OTA ─────────────────────────────────────────────────────
// Filename the SD Update tool looks for on the card root
#define OTA_FILENAME  "/update.bin"

// ─── LVGL ───────────────────────────────────────────────────────
#define LV_BUF_LINES  20   // DMA render buffer height in lines

// ─── APA102 LEDs ────────────────────────────────────────────────
#define LED_BRIGHTNESS  6   // global brightness (0-31 for APA102)

// Per-category ring LED colors  { R, G, B }
#define LED_COLOR_WIFI    {  0, 200,   0 }   // Green
#define LED_COLOR_BLE     {  0,   0, 220 }   // Blue
#define LED_COLOR_MISC    { 220, 220,   0 }   // Yellow
#define LED_COLOR_GPS     { 160,   0, 200 }   // Purple

// ─── Splash Screen ──────────────────────────────────────────────
#define SPLASH_DURATION_MS  3000   // ms to show splash on boot

// ─── Power-off Hold ─────────────────────────────────────────────
#define POWER_HOLD_MS  5000   // hold encoder this long to power off

// ─── WiFi Scanner ───────────────────────────────────────────────
#define MAX_WIFI_RESULTS  30   // max APs stored per scan

// ─── Deauth Detector ────────────────────────────────────────────
#define MAX_DEAUTH        12   // ring-buffer size for deauth events
#define DEAUTH_HOP_MS    200   // channel hop interval in ms

// ─── PineAP Hunter ──────────────────────────────────────────────
#define MAX_PINEAP_BSSIDS   20   // max unique BSSIDs tracked
#define PINEAP_SSID_SLOTS    6   // SSIDs stored per BSSID (display)
#define PINEAP_THRESHOLD     5   // flag after this many unique SSIDs

// ─── Pwnagotchi Detector ────────────────────────────────────────
#define MAX_PWNS       10   // max pwnagotchis tracked
#define PWN_BUF_LEN    33   // SSID pending buffer length

// ─── Flock Safety Detector ──────────────────────────────────────
#define MAX_FLOCK_HITS  20   // max unique SSID hits stored

// ─── BLE Scanner ────────────────────────────────────────────────
#define MAX_BLE_RESULTS   30   // max BLE devices stored per scan
#define BLE_SCAN_SECS      8   // default scan duration in seconds
#define BLE_MIN_SATS       4   // minimum GPS satellites for Wiggle Wars

// ─── Wiggle Wars (WiGLE wardrive) ───────────────────────────────
#define WIGGLE_SCAN_INTERVAL_MS  15000   // ms between WiFi scans

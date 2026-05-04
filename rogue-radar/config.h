// ============================================================
//  config.h — Rogue Radar T-Embed v1.0.1
//  Edit this file to customise pins, limits, themes, and behaviour.
//  Do not edit rogue-radar.ino unless you know what you're doing.
// ============================================================
#pragma once

// ─── Device Name / Firmware Version ─────────────────────────────
#define DEVICE_NAME       "Rogue Radar"
#define FIRMWARE_VERSION  "RR v1.0.1"

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

// ─── Inactivity Backlight Dimmer ────────────────────────────────
// This does NOT put the ESP32-S3 to sleep. It only dims the TFT backlight
// after no encoder movement/button activity for the timeout below.
// Set INACTIVITY_DIM_TIMEOUT_MS to 0 to disable auto-dimming.
#define INACTIVITY_DIM_TIMEOUT_MS  30000  // 30 seconds
#define INACTIVITY_DIM_LEVEL          10  // dim brightness level (0-255)

// Default state for the dimming system at boot.
// 1 = dimming starts enabled | 0 = dimming starts disabled
#define DIMMING_ENABLED_DEFAULT        1

// ─── APA102 LEDs Dimmer ─────────────────────────────────────────
#define LED_BRIGHTNESS      8   // 0-31 for APA102 normal brightness
#define LED_DIM_BRIGHTNESS  1   // 0-31 while inactive/dimmed

// ─── APA102 LEDs Runtime Toggle ────────────────────────────────
// 1 = APA102 ring starts enabled | 0 = APA102 ring starts disabled
#define LEDS_ENABLED_DEFAULT       1

// ─── I2S Speaker / Alert Chirps ────────────────────────────────
// Uses the T-Embed speaker connector / onboard I2S amp pins.
// 1 = alert chirps enabled | 0 = alert chirps disabled.
// I2S is lazy-initialized on first chirp, then shut down after each chirp.
#define SOUND_ENABLED_DEFAULT      1
#define SOUND_I2S_BCLK            7
#define SOUND_I2S_WCLK            5
#define SOUND_I2S_DOUT            6
#define SOUND_SAMPLE_RATE         16000
#define SOUND_VOLUME_PERCENT      35   // 0-100 for detection alert chirps
#define SOUND_ALERT_COOLDOWN_MS   1200 // prevents repeated chirp spam

// ─── Menu Feedback Sounds ──────────────────────────────────────
// Separate from the Misc > Sound ON/OFF alert toggle above.
// Set MENU_FEEDBACK_ENABLED_DEFAULT to 0 to boot with encoder/menu sounds
// disabled while keeping detection alert chirps available. Runtime toggle:
// Misc > Menu Sounds ON/OFF.
#define MENU_FEEDBACK_ENABLED_DEFAULT  1
#define MENU_FEEDBACK_VOLUME_PERCENT  8   // 0-100, kept low on purpose
#define MENU_FEEDBACK_TICK_COOLDOWN_MS 85  // prevents noisy encoder tick spam

// ─── Auto-return Home ──────────────────────────────────────────
// This does NOT sleep or restart the device. It only returns the UI
// back to the main home menu after no encoder movement/button activity.
// Set AUTO_RETURN_HOME_TIMEOUT_MS to 0 to disable auto-return.
#define AUTO_RETURN_HOME_TIMEOUT_MS  120000  // 2 minutes

// ─── Display ────────────────────────────────────────────────────
#define SCREEN_W  320
#define SCREEN_H  170

// ─── Display Rotation ──────────────────────────────────────────
// Landscape-only runtime toggle used by Misc > Rotation.
// This build stays in 320x170 landscape mode, so only rotations 3 and 1
// are used. Normal is the current known-good T-Embed orientation.
#define DISPLAY_ROTATION_NORMAL   3
#define DISPLAY_ROTATION_FLIPPED  1
#define DISPLAY_ROTATION_DEFAULT  DISPLAY_ROTATION_NORMAL


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
#define OTA_FILENAME  "/update.bin"

// ─── LVGL ───────────────────────────────────────────────────────
#define LV_BUF_LINES  20

// Per-category ring LED colors  { R, G, B }
#define LED_COLOR_WIFI    {  0, 200,   0 }
#define LED_COLOR_BLE     {  0,   0, 220 }
#define LED_COLOR_MISC    { 220, 220,   0 }
#define LED_COLOR_GPS     { 160,   0, 200 }

// ─── Splash Screen ──────────────────────────────────────────────
#define SPLASH_DURATION_MS  3000

// ─── Power-off Hold ─────────────────────────────────────────────
#define POWER_HOLD_MS  5000

// ─── WiFi Scanner ───────────────────────────────────────────────
// WiFi scan time is session-adjustable from Misc > Scan Defaults.
// It resets to this value after reboot.
#define WIFI_SCAN_SECS    10
#define MAX_WIFI_RESULTS  30

// ─── Deauth Detector ────────────────────────────────────────────
#define MAX_DEAUTH        12
// Channel hop delay is session-adjustable from Misc > Scan Defaults.
// It resets to this value after reboot.
#define DEAUTH_HOP_MS    200

// ─── PineAP Hunter ──────────────────────────────────────────────
#define MAX_PINEAP_BSSIDS   20
#define PINEAP_SSID_SLOTS    6
#define PINEAP_THRESHOLD     5

// ─── Pwnagotchi Detector ────────────────────────────────────────
#define MAX_PWNS       10
#define PWN_BUF_LEN    33

// ─── Flock Safety Detector ──────────────────────────────────────
#define MAX_FLOCK_HITS  20

// Extra WiFi SSID keywords used by the Flock detector.
// These are checked case-insensitively against beacon/probe SSIDs.
#define FLOCK_KEYWORD_1  "flock"
#define FLOCK_KEYWORD_2  "penguin"
#define FLOCK_KEYWORD_3  "pigvision"
#define FLOCK_KEYWORD_4  "fs ext battery"

// 1 = dedupe Flock hits mainly by source MAC, 0 = dedupe by SSID only.
#define FLOCK_DEDUPE_BY_MAC  1

// 1 = show source MAC under each Flock hit row, 0 = compact one-line rows.
#define FLOCK_SHOW_SOURCE_MAC  1

// ─── BLE Scanner ────────────────────────────────────────────────
#define MAX_BLE_RESULTS   30
// BLE scan time is session-adjustable from Misc > Scan Defaults.
// It resets to this value after reboot.
#define BLE_SCAN_SECS      8

// ─── Flipper Detector ───────────────────────────────────────────
// Number of Flipper name strings checked below. If you add or remove
// entries in FLIPPER_NAME_MATCHES, update this count to match.
#define FLIPPER_NAME_MATCH_COUNT  4

static const char* FLIPPER_NAME_MATCHES[FLIPPER_NAME_MATCH_COUNT] = {
    "flipper",
    "flipperzero",
    "flipper zero",
    "flipper-zero"
};

// ─── Wiggle Wars ────────────────────────────────────────────────
#define WIGGLE_SCAN_INTERVAL_MS  15000

// ─── Default Theme ──────────────────────────────────────────────
// 0 = Dark  |  1 = Flipper  |  2 = Matrix
#define DEFAULT_THEME  0

// ─── UI Themes ──────────────────────────────────────────────────
// Each theme defines 19 values (see UITheme struct in rogue-radar.ino).
// Fields in order:
//   name,
//   bg, card, cardAlt, border, barBg,
//   text, textDim, accent,
//   success, warn, alert,
//   btnDefault, btnFocus, btnPress,
//   actionBg, actionFoc, actionBdr,
//   flashGreen, stopRed
//
// To customise a theme just change the hex values below.
// To add a new theme: add a THEME_XXX macro here, then add
//   { THEME_XXX }, to the THEMES[] array in rogue-radar.ino.

#define THEME_DARK \
    "Dark", \
    0x0d1117, 0x161b22, 0x161b22, 0x30363d, 0x21262d, \
    0xe6edf3, 0x8b949e, 0x58a6ff, \
    0x3fb950, 0xe3b341, 0xf85149, \
    0x21262d, 0x1f4f8f, 0x388bfd, \
    0x1a4a1a, 0x1f6f1f, 0x3fb950, \
    0x238636, 0xb62324

#define THEME_FLIPPER \
    "Flipper", \
    0x111111, 0x1c1c1c, 0x1c1c1c, 0x333333, 0x2a2a2a, \
    0xffffff, 0x888888, 0xff8c00, \
    0xff8c00, 0xffcc00, 0xff3333, \
    0x2a2a2a, 0x7a4000, 0xff8c00, \
    0x3a2000, 0x7a4000, 0xff8c00, \
    0xff8c00, 0xcc2200

#define THEME_MATRIX \
    "Matrix", \
    0x000000, 0x0a0f0a, 0x0a0f0a, 0x1a3a1a, 0x0d1f0d, \
    0x00ff41, 0x007a20, 0x00ff41, \
    0x00ff41, 0x39ff14, 0xff0000, \
    0x0d1f0d, 0x1a5a1a, 0x00ff41, \
    0x001a00, 0x003300, 0x00ff41, \
    0x004000, 0x5a0000

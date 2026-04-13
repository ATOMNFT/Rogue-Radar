// ============================================================
//  config.h — Rogue Radar Firmware
//  Supports: T-Embed ESP32-S3, CYD-2USB, NM-CYD-C5
//  Edit this file to select your device and customise settings.
// ============================================================
#pragma once

// ═══════════════════════════════════════════════════════════════
//  DEVICE SELECTION - Uncomment your device
// ═══════════════════════════════════════════════════════════════

// #define DEVICE_T_EMBED_S3     // LilyGO T-Embed ESP32-S3 (320x170)
// #define DEVICE_CYD_2USB       // CYD-2USB ESP32 (320x240)
#define DEVICE_NM_CYD_C5      // NM-CYD-C5 ESP32 (320x240)

// ═══════════════════════════════════════════════════════════════
//  AUTO-DETECT DEVICE if not manually selected
// ═══════════════════════════════════════════════════════════════
#if !defined(DEVICE_T_EMBED_S3) && !defined(DEVICE_CYD_2USB) && !defined(DEVICE_NM_CYD_C5)
    // Auto-detect based on common board definitions
    #if defined(BOARD_HAS_PSRAM) && defined(CONFIG_IDF_TARGET_ESP32S3)
        #define DEVICE_T_EMBED_S3
    #elif defined(ARDUINO_ESP32_DEV) || defined(ESP32)
        // Default to NM-CYD-C5 for generic ESP32
        #define DEVICE_CYD_2USB
    #elif defined(ARDUINO_ESP32C5_DEV) || defined(ESP32C5)
        #define DEVICE_NM_CYD_C5
    #endif
#endif

// ═══════════════════════════════════════════════════════════════
//  DEVICE CONFIGURATION
// ═══════════════════════════════════════════════════════════════

#if defined(DEVICE_T_EMBED_S3)
    // ─── Device Name ────────────────────────────────────────────
    #define DEVICE_NAME  "Rogue Radar"
    
    // ─── Display ────────────────────────────────────────────────
    #define SCREEN_W  320
    #define SCREEN_H  170
    
    // ─── Pin Definitions ────────────────────────────────────────
    #define POWER_PIN       46
    #define ENCODER_A        1
    #define ENCODER_B        2
    #define ENCODER_BTN      0
    #define APA102_DI       42
    #define APA102_CLK      45
    #define WS2812_PIN      -1    // No WS2812 on T-Embed
    #define NUM_LEDS         7
    #define HAS_APA102_LED   1
    #define HAS_WS2812_LED   0
    #define HAS_ENCODER      1
    
    // ─── LCD Backlight (LEDC PWM) ───────────────────────────────
    #define LCD_BL_PIN      15    // IO15 = TFT backlight
    #define LCD_BL_CH        0    // LEDC channel (0-7, must be free)
    #define LCD_BL_FREQ   5000    // Hz — above audible range
    #define LCD_BL_RES       8    // bits (0-255 range)
    #define LCD_BL_DEFAULT 255    // startup brightness (0=off, 255=max)
    #define TFT_BL_INVERT    0    // 0 = normal, 1 = inverted
    #define TFT_SWAP_BYTES   1    // 1 = true (swap bytes), 0 = false (try 0 if colors reversed)
    
    // ─── Touch Screen ───────────────────────────────────────────
    #define HAS_TOUCH        0    // T-Embed does not have touch screen
    #define TOUCH_CS        -1
    #define TOUCH_IRQ       -1
    
    // ─── GPS ────────────────────────────────────────────────────
    #define GPS_RX_PIN  44
    #define GPS_TX_PIN  43
    #define GPS_BAUD  9600
    #define HAS_GPS        1
    #define GPS_SERIAL_INDEX 1
    
    // ─── SD Card (HSPI — separate bus from TFT) ─────────────────
    #define SD_CS    39
    #define SD_SCLK  40
    #define SD_MISO  38
    #define SD_MOSI  41
    #define HAS_SD_CARD    1

#elif defined(DEVICE_CYD_2USB)
    // ─── Device Name ────────────────────────────────────────────
    #define DEVICE_NAME  "Rogue Radar CYD"
    
    // ─── Display ────────────────────────────────────────────────
    #define SCREEN_W  320
    #define SCREEN_H  240
    
    // ─── Pin Definitions (CYD-2USB ESP32) ───────────────────────
    // CYD-2USB: Cheap Yellow Display with 2 USB ports
    #define POWER_PIN       -1    // No power control pin
    #define ENCODER_A       -1    // No rotary encoder
    #define ENCODER_B       -1
    #define ENCODER_BTN     -1
    #define APA102_DI       -1    // No APA102 LED
    #define APA102_CLK      -1
    #define WS2812_PIN      -1     // WS2812 LED on GPIO27
    #define NUM_LEDS         3     // CYD-2USB has 3 GRB LEDs (IO16, IO4, IO17)
    #define HAS_APA102_LED   0
    #define HAS_WS2812_LED   0
    #define HAS_ENCODER      0
    
    // Note: Most CYD-2USB boards have a single WS2812 RGB LED on GPIO27
    // If your board has the LED on a different pin, change WS2812_PIN above
    
    // ─── LCD Backlight ──────────────────────────────────────────
    #define LCD_BL_PIN      21    // GPIO21 on CYD-2USB
    #define LCD_BL_CH        0
    #define LCD_BL_FREQ   5000
    #define LCD_BL_RES       8
    #define LCD_BL_DEFAULT 255
    #define TFT_BL_INVERT    0    // CYD uses inverted backlight
    #define TFT_SWAP_BYTES   1    // 1 = true (swap bytes), 0 = false (try 0 if colors reversed)
    
    // ─── TFT Pins (for TFT_eSPI User_Setup) ─────────────────────
    #define TFT_MISO        12
    #define TFT_MOSI        13
    #define TFT_SCLK        14
    #define TFT_CS          15
    #define TFT_DC           2
    #define TFT_RST         -1
    
    // ─── Touch Screen (XPT2046) ─────────────────────────────────
    // Touch shares SPI bus with TFT (MISO/MOSI/SCK), separate CS
    #define HAS_TOUCH        1
    #define HAS_CYD_TOUCH    1    // CYD-2USB has a touch screen (XPT2046)
    #define XPT2046_IRQ     36    // Optional: touch interrupt pin
    #define XPT2046_CLK     25
    #define XPT2046_MISO    39
    #define XPT2046_MOSI    32
    #define XPT2046_CS      33

    // ─── GPS (Optional - can be connected externally) ───────────
    // To use external GPS module, connect to any available UART pins
    // and update GPS_RX_PIN / GPS_TX_PIN below
    #define GPS_RX_PIN      3    // Set to actual pin if using external GPS
    #define GPS_TX_PIN      1    // Set to actual pin if using external GPS
    #define GPS_BAUD      9600
    #define HAS_GPS          1    // GPS support enabled (external module)
    #define GPS_SERIAL_INDEX 2    // Use UART2 for GPS on CYD-2USB
    
    // ─── SD Card (VSPI on CYD) ──────────────────────────────────
    #define SD_CS            5
    #define SD_SCLK         18
    #define SD_MISO         19
    #define SD_MOSI         23
    #define HAS_SD_CARD      1

#elif defined(DEVICE_NM_CYD_C5)
    // ─── Device Name ────────────────────────────────────────────
    #define DEVICE_NAME  "Rogue Radar NM-CYD-C5"
    
    // ─── MCU Info ───────────────────────────────────────────────
    // NM-CYD-C5: Based on ESP32-C5 (RISC-V architecture, Wi-Fi 6)
    // Note: Requires Arduino ESP32 Core >= 3.0.0 for C5 support
    
    // ─── Display ────────────────────────────────────────────────
    #define SCREEN_W  320
    #define SCREEN_H  240
    
    // ─── Pin Definitions (NM-CYD-C5 ESP32-C5) ───────────────────
    // ESP32-C5 has different pin mapping than ESP32/ESP32-S3
    // Verify pins with your specific board schematic
    #define POWER_PIN       -1    // No power control pin
    #define ENCODER_A       -1    // No rotary encoder
    #define ENCODER_B       -1
    #define ENCODER_BTN     -1
    #define APA102_DI       -1    // No APA102 LED
    #define APA102_CLK      -1
    #define WS2812_PIN      27     // WS2812 LED on GPIO27
    #define NUM_LEDS         1    // Single WS2812 LED
    #define HAS_APA102_LED   0
    #define HAS_WS2812_LED   1
    #define HAS_ENCODER      0
    
    // Note: Most NM-CYD-C5 boards have a single WS2812 RGB LED on GPIO27
    // Verify with your specific board schematic
    
    // ─── LCD Backlight ──────────────────────────────────────────
    // ESP32-C5 GPIO numbers may differ, verify with schematic
    #define LCD_BL_PIN      25    // GPIO25 on NM-CYD-C5 (verify)
    #define LCD_BL_CH        0
    #define LCD_BL_FREQ   5000
    #define LCD_BL_RES       8
    #define LCD_BL_DEFAULT 255
    #define TFT_BL_INVERT    0    // NM-CYD-C5 uses inverted backlight
    #define TFT_SWAP_BYTES   1    // 1 = true (swap bytes), 0 = false (try 0 if colors reversed)

    // ─── TFT Pins (for TFT_eSPI User_Setup) ─────────────────────
    // SPI pins for ESP32-C5 - verify with board schematic
    #define TFT_MISO        2
    #define TFT_MOSI        7
    #define TFT_SCLK        6
    #define TFT_CS          23
    #define TFT_DC          24
    #define TFT_RST         -1
    
    // ─── Touch Screen (XPT2046) ─────────────────────────────────
    // Touch shares SPI bus with TFT (MISO/MOSI/SCK), separate CS
    #define HAS_TOUCH        1
    #define TOUCH_CS         1    // XPT2046 CS pin (shared SPI with TFT)
    #define TOUCH_IRQ       -1    // Optional: touch interrupt pin (-1 if polling)
    
    // ─── GPS (Optional - can be connected externally) ───────────
    // ESP32-C5 has limited pin availability, verify UART pins
    // To use external GPS, connect to available UART pins and update below
    #define GPS_RX_PIN      4    // Set to actual pin if using external GPS
    #define GPS_TX_PIN      5    // Set to actual pin if using external GPS
    #define GPS_BAUD      9600
    #define HAS_GPS          1    // GPS support enabled (external module)
    #define GPS_SERIAL_INDEX 2    // Use UART2 for GPS on NM-CYD-C5
    
    // ─── SD Card (SPI on NM-CYD-C5) ─────────────────────────────
    // ESP32-C5 SPI pins - verify with board schematic
    #define SD_CS           10
    #define SD_SCLK         6
    #define SD_MISO         2
    #define SD_MOSI         7
    #define HAS_SD_CARD      1

#else
    #error "No device selected! Please define one of: DEVICE_T_EMBED_S3, DEVICE_CYD_2USB, or DEVICE_NM_CYD_C5"
#endif

// ─── BLE Device Classification ──────────────────────────────────
// Declared here so Arduino auto-prototype generation sees the type
enum BLEDeviceType {
    BLE_GENERIC  = 0,
    BLE_AIRTAG   = 1,
    BLE_FLIPPER  = 2,
    BLE_APPLE    = 3,
    BLE_SKIMMER  = 4,
    BLE_META     = 5
};

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

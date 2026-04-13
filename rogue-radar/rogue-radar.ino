// ============================================================
//  Rogue Radar v1.0.0 Firmware
//  Check config.h for adjustable settings
// ============================================================
//
//  Tool Categories:
//  WiFi Tools: Network Scanner | Deauth Detector | Channel Analyzer
//              PineAP Hunter | Pwnagotchi Watch | Flock Detector
//  BLE Tools:  BLE Scanner | AirTag Detector | Flipper Zero Detector
//              Skimmer Detector | Meta Detector
//  GPS Tools:  GPS Stats | Wiggle Wars
//  Misc Tools: Device Info | SD Update | Brightness ADJ
//
//  Display / UI:
//  - ST7789 320x170 display
//  - LVGL-based menu system
//  - Rotary encoder navigation
//  - Splash screen support
//  - APA102 status LED support
//
//  Hardware Target:
//  - LilyGO T-Embed ESP32-S3 / CYD-2USB / NM-CYD-C5
//
//  Arduino IDE Settings:
//  - Board: ESP32S3 Dev Module
//  - Partition Scheme: Huge APP
//
//  Required Libraries:
//  - TFT_eSPI         -> ST7789 display driver (configure User_Setup.h)
//  - lvgl 9.0.0       -> UI framework
//  - RotaryEncoder    -> mathertel/RotaryEncoder
//  - APA102           -> pololu/apa102-arduino
//  - TinyGPSPlus      -> GPS parsing
//  - WiFi / esp_wifi  -> built into ESP32 Arduino core
//  - BLEDevice        -> built into ESP32 Arduino core
//  - SPI / SD         -> for SD card update support
//
//  lv_conf.h notes:
//    #define LV_COLOR_DEPTH        16
//    #define LV_USE_LIST            1
//    #define LV_USE_LABEL           1
//    #define LV_USE_BTN             1
//    #define LV_USE_BAR             1
//
// ============================================================

#include <Arduino.h>
#include "config.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <RotaryEncoder.h>
#include <APA102.h>
#if HAS_WS2812_LED
    #include <Adafruit_NeoPixel.h>
#endif
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <TinyGPS++.h>
#include <SPI.h>
#include <SD.h>
#include <Update.h>
// Optional: CYD 320x240 splash screen
#ifdef DEVICE_T_EMBED_S3
    #include "splash.h"
#elif defined(DEVICE_CYD_2USB) || defined(DEVICE_NM_CYD_C5)
    #include "splash_cyd.h"
#endif

#if HAS_CYD_TOUCH
    #include <TFT_Touch.h>
#endif


// ─── SPI Bus for SD Card ────────────────────────────────────────
#if defined(DEVICE_NM_CYD_C5)  
    static SPIClass sdSPI(SPI); // ESP32-C5 shares SPI bus for TFT and SD
#else
    static SPIClass sdSPI(HSPI);
#endif

// ─── GPS Serial ─────────────────────────────────────────────────
#if HAS_GPS
    #if !CONFIG_IDF_TARGET_ESP32C5
        HardwareSerial Serial2(GPS_SERIAL_INDEX);   // UART1 for S3, UART2 for CYD / NM-CYD-C5
    #endif
#endif

#if HAS_GPS
    static TinyGPSPlus gps;
#endif

// ─── GPS Detection ──────────────────────────────────────────────
// Returns true if GPS is configured (pins are set) and should work
static inline bool gpsIsConfigured() {
    #if HAS_GPS
        return (GPS_RX_PIN >= 0) && (GPS_TX_PIN >= 0);
    #else
        return false;
    #endif
}
static const unsigned long SPLASH_TIME_MS = SPLASH_DURATION_MS;

TFT_eSPI tft = TFT_eSPI();

// ─── LVGL Buffers ───────────────────────────────────────────────
static lv_color_t lvBuf1[SCREEN_W * LV_BUF_LINES];
static lv_color_t lvBuf2[SCREEN_W * LV_BUF_LINES];
static lv_display_t *lvDisp  = nullptr;
static lv_indev_t   *lvIndev = nullptr;

// ─── Rotary Encoder (Conditional) ───────────────────────────────
#if HAS_ENCODER
    RotaryEncoder encoder(ENCODER_A, ENCODER_B, RotaryEncoder::LatchMode::TWO03);
#endif

// ─── APA102 LEDs (Conditional) ──────────────────────────────────
#if HAS_APA102_LED
    APA102<APA102_DI, APA102_CLK> ledStrip;
    rgb_color ledBuf[NUM_LEDS];
#endif

// ─── WS2812 LEDs (Conditional) ──────────────────────────────────
#if HAS_WS2812_LED
    Adafruit_NeoPixel ws2812Strip(NUM_LEDS, WS2812_PIN, NEO_GRB + NEO_KHZ800);
#endif

// ─── Touch Screen (Conditional) ─────────────────────────────────
#if HAS_CYD_TOUCH
    TFT_Touch ts = TFT_Touch(XPT2046_CS, XPT2046_CLK, XPT2046_MOSI, XPT2046_MISO);
#endif

struct MenuLED { uint8_t r, g, b; };
const MenuLED MENU_COLORS[4] = {
    LED_COLOR_WIFI,
    LED_COLOR_BLE,
    LED_COLOR_MISC,
    LED_COLOR_GPS
};

// ─── LED Spinner (FreeRTOS task on core 0) ───────────────────────
struct SpinnerColor { uint8_t r, g, b; };
static volatile bool  spinnerRunning     = false;
static TaskHandle_t   spinnerTaskHandle  = nullptr;
static SpinnerColor   spinnerColor       = {0, 200, 0};

// ─── Global UI State ────────────────────────────────────────────
static int            currentMenu       = 0;
static bool           powerOffTriggered = false;
static unsigned long  btnHoldStart      = 0;
static int            lcdBrightness     = LCD_BL_DEFAULT;  // 0-255, default full

// ─── Screen Pointers ────────────────────────────────────────────
static lv_obj_t *mainScreen       = nullptr;
static lv_obj_t *wifiMenuScreen   = nullptr;
static lv_obj_t *wifiToolScreen   = nullptr;
static lv_obj_t *wifiDetailScreen = nullptr;
static lv_obj_t *bleMenuScreen    = nullptr;
static lv_obj_t *bleToolScreen    = nullptr;
static lv_obj_t *bleDetailScreen  = nullptr;
static lv_obj_t *subScreen        = nullptr;
static lv_obj_t *miscMenuScreen   = nullptr;
static lv_obj_t *miscToolScreen   = nullptr;
static lv_obj_t *gpsMenuScreen    = nullptr;
static lv_obj_t *gpsToolScreen    = nullptr;

// ─── Input Group Pointers ───────────────────────────────────────
static lv_group_t *navGroup        = nullptr;
static lv_group_t *wifiMenuGroup   = nullptr;
static lv_group_t *wifiToolGroup   = nullptr;
static lv_group_t *wifiDetailGroup = nullptr;
static lv_group_t *bleMenuGroup    = nullptr;
static lv_group_t *bleToolGroup    = nullptr;
static lv_group_t *bleDetailGroup  = nullptr;
static lv_group_t *subGroup        = nullptr;
static lv_group_t *miscMenuGroup   = nullptr;
static lv_group_t *miscToolGroup   = nullptr;
static lv_group_t *gpsMenuGroup    = nullptr;
static lv_group_t *gpsToolGroup    = nullptr;

// ════════════════════════════════════════════════════════════════
//  WIFI DATA STRUCTURES
// ════════════════════════════════════════════════════════════════


struct WiFiEntry {
    char    ssid[33];
    char    bssid[18];
    int8_t  rssi;
    uint8_t channel;
    bool    open;
    char    authStr[10];
};

static WiFiEntry wifiEntries[MAX_WIFI_RESULTS];
static int       wifiEntryCount = 0;

// ── Deauth Detector ─────────────────────────────────────────────

struct DeauthEvent {
    char     src[18];
    char     dst[18];
    uint8_t  channel;
    uint16_t reason;
    uint32_t ms;
};

static DeauthEvent   deauthLog[MAX_DEAUTH];
static volatile int  deauthHead    = 0;
static volatile int  deauthTotal   = 0;
static volatile bool deauthActive  = false;
static uint8_t       deauthChannel = 1;

// ── Channel Analyzer ────────────────────────────────────────────
static int    chanNetCount[14];
static int8_t chanMaxRSSI[14];

// LVGL timer handle for deauth refresh
static lv_timer_t *deauthTimer = nullptr;

// ── PineAP Hunter ─────────────────────────────────────────────── Ref. here: https://github.com/n0xa/m5stick-nemo/blob/main/pineap_hunter.h
//
//  Strategy: each WiFi scan returns one SSID per BSSID. A rogue
//  Pineapple / KARMA AP cycles through many SSIDs across scans.
//  We accumulate BSSID->SSID mappings over repeated scans. Any BSSID
//  that exceeds PINEAP_THRESHOLD unique SSIDs is flagged as suspect.
//

struct PineAPEntry {
    char   bssid[18];            // XX:XX:XX:XX:XX:XX
    int8_t lastRSSI;
    int    ssidCount;            // total unique SSIDs observed
    char   ssids[PINEAP_SSID_SLOTS][33]; // first N for display
};

static PineAPEntry pineapEntries[MAX_PINEAP_BSSIDS];
static int         pineapEntryCount = 0;   // how many BSSIDs tracked
static int         pineapScanCount  = 0;   // how many scans performed
static int         pineapFlagged    = 0;   // BSSIDs above threshold

// ── Pwnagotchi Detector ───────────────────────────────────────── Ref. here: https://github.com/justcallmekoko/ESP32Marauder/wiki/detect-pwnagotchi
//
//  Pwnagotchis beacon with source MAC de:ad:be:ef:de:ad and encode
//  their status (name, handshakes captured) as JSON in the SSID field.
//  We run promiscuous mode, sniff beacon frames (0x80), verify the MAC,
//  then extract name + pwnd_tot from the SSID JSON.
//

struct PwnEntry {
    char     name[33];   // parsed from JSON "name" field
    char     bssid[18];  // BSSID from beacon frame
    int      pwnd_tot;   // parsed from JSON "pwnd_tot"
    int8_t   rssi;
    uint32_t lastSeen;   // millis()
};

static PwnEntry        pwnEntries[MAX_PWNS];
static int             pwnCount       = 0;
static volatile bool   pwnActive      = false;
static lv_timer_t     *pwnTimer       = nullptr;

// Ring buffer for passing SSID payloads out of the sniffer ISR
static char            pwnPendingSSID[PWN_BUF_LEN];
static char            pwnPendingBSSID[18];
static volatile int8_t pwnPendingRSSI  = 0;
static volatile bool   pwnPendingReady = false;

// ── Flock Safety Detector ──────────────────────────────────────── Ref. here: https://github.com/justcallmekoko/ESP32Marauder/wiki/flock-sniff
//
//  Flock Safety LPR cameras connect to / advertise networks containing
//  "flock" in the SSID. We watch beacon frames (subtype 8), probe
//  responses (subtype 5), and probe requests (subtype 4) for that
//  substring. Alerts are latching — stays red until tool is exited.
//

struct FlockHit {
    char    ssid[33];
    char    src[18];    // source MAC of the frame
    uint8_t frameType; // 0=beacon/resp, 1=probe req
    int8_t  rssi;
};

static FlockHit       flockHits[MAX_FLOCK_HITS];
static int            flockHitCount   = 0;
static volatile bool  flockActive     = false;
static lv_timer_t    *flockTimer      = nullptr;

// ISR pending slot — one frame at a time into main task
static char            flockPendingSSID[33];
static char            flockPendingSrc[18];
static volatile uint8_t flockPendingType = 0;
static volatile int8_t  flockPendingRSSI = 0;
static volatile bool    flockPendingReady = false;

// ════════════════════════════════════════════════════════════════
//  BLE DATA STRUCTURES
// ════════════════════════════════════════════════════════════════


// Device type flags — defined in config.h so Arduino-generated
// function prototypes can see the type before use.

struct BLEEntry {
    char          name[33];    // advertised local name (or "<unknown>")
    char          mac[18];     // XX:XX:XX:XX:XX:XX
    int8_t        rssi;
    BLEDeviceType type;
    char          mfgHint[14]; // short manufacturer hint for list row
};

static BLEEntry bleEntries[MAX_BLE_RESULTS];
static int      bleEntryCount  = 0;
static bool     bleInitialized = false;  // BLEDevice::init() once only


// ════════════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════
void createMainMenu();
void createWiFiMenu();
void createNetworkScanner();
void createNetworkDetail(int idx);
void createDeauthDetector();
void createChannelAnalyzer();
void createPineAPHunter();
void createPineAPDetail(int idx);
void createPwnagotchiDetector();
void createFlockDetector();
void createSubScreen(int idx);
void createMiscMenu();
void createDeviceInfo();
void createSDUpdate();
void createBrightnessControl();
void createGPSMenu();
void createGPSStats();
void createWiggleWars();

static void cb_wifiToolBack(lv_event_t *e);
static void cb_wifiDetailBack(lv_event_t *e);

void createBLEMenu();
void createBLEScanner();
void createBLEDetail(int idx);
void createAirTagScanner();
void createFlipperScanner();
void createSkimmerScanner();
void createMetaDetector();
static void cb_bleToolBack(lv_event_t *e);
static void cb_bleDetailBack(lv_event_t *e);

// ════════════════════════════════════════════════════════════════
//  LVGL FLUSH + ENCODER CALLBACKS
// ════════════════════════════════════════════════════════════════

static void lvgl_flush_cb(lv_display_t *disp,
                          const lv_area_t *area,
                          uint8_t *px_map)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors(reinterpret_cast<uint16_t *>(px_map), w * h, true);
    tft.endWrite();
    lv_display_flush_ready(disp);
}

static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    #if HAS_ENCODER
        encoder.tick();
        int pos = encoder.getPosition();
        data->enc_diff = (int16_t)pos;
        if (pos != 0) encoder.setPosition(0);
        data->state = (digitalRead(ENCODER_BTN) == LOW)
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
    #else
        // CYD devices: use alternative input method
        // For now, return no movement and check for BOOT button (GPIO0) as select
        data->enc_diff = 0;
        // GPIO0 is the BOOT button on most ESP32 dev boards including CYD
        data->state = (digitalRead(0) == LOW)
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
        (void)indev;  // Suppress unused warning
    #endif
}

// ─── Touch Screen Read Callback ─────────────────────────────────

#ifdef HAS_CYD_TOUCH
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    bool touched = ts.Pressed();
    if(!touched) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    else {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = ts.X();
        data->point.y = ts.Y();
    }
}
#endif

#if HAS_TOUCH
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t touchX, touchY;
    bool touched = tft.getTouch(&touchX, &touchY);
    if (!touched) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    } else {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = touchX;
        data->point.y = touchY;
    }
}
#endif

// ════════════════════════════════════════════════════════════════
//  Splash Screen
// ════════════════════════════════════════════════════════════════
void showSplashScreen() {
    tft.fillScreen(TFT_BLACK);

    // Use configured swap bytes setting (TFT_SWAP_BYTES in config.h)
    // Set to 0 if colors appear reversed (BGR instead of RGB)
    tft.setSwapBytes(TFT_SWAP_BYTES);
    
    #ifdef DEVICE_T_EMBED_S3
        // T-Embed: image matches screen exactly
        tft.pushImage(0, 0, SCREEN_W, SCREEN_H, splash);
    #elif defined(DEVICE_CYD_2USB) || defined(DEVICE_NM_CYD_C5)
        // CYD/NM-CYD-C5 320x240: use full screen splash if available
        tft.pushImage(0, 0, SCREEN_W, SCREEN_H, splash_cyd);
    #endif

    delay(SPLASH_TIME_MS); 
}

// ════════════════════════════════════════════════════════════════
//  LED HELPERS
// ════════════════════════════════════════════════════════════════

void setAllLEDs(uint8_t r, uint8_t g, uint8_t b,
                uint8_t br = LED_BRIGHTNESS)
{
    #if HAS_APA102_LED
        for (int i = 0; i < NUM_LEDS; i++) ledBuf[i] = {r, g, b};
        ledStrip.write(ledBuf, NUM_LEDS, br);
    #elif HAS_WS2812_LED
        // WS2812: apply brightness to color values (0-31 -> 0-255)
        uint8_t scale = (br * 8) > 255 ? 255 : (br * 8);
        uint8_t sr = (r * scale) >> 8;
        uint8_t sg = (g * scale) >> 8;
        uint8_t sb = (b * scale) >> 8;
        for (int i = 0; i < NUM_LEDS; i++) {
            ws2812Strip.setPixelColor(i, ws2812Strip.Color(sr, sg, sb));
        }
        ws2812Strip.show();
    #else
        (void)r; (void)g; (void)b; (void)br;  // Suppress unused warnings
    #endif
}

void ledStartupFlash() {
    #if HAS_APA102_LED || HAS_WS2812_LED
        setAllLEDs(255, 255, 255, 10); delay(300);
        setAllLEDs(0, 0, 0, 0);       delay(150);
        setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
    #endif
}
// ── LED Spinner Task ─────────────────────────────────────────────
//  Runs on core 0 while core 1 blocks on a WiFi scan.
//  Produces a comet-tail chase around the 7-LED ring:
//    Head : full colour
//    Mid  : 35% dimmed
//    Tail : 12% dimmed
//    Rest : off
static void ledSpinnerTask(void *param) {
    #if HAS_APA102_LED
        uint8_t pos = 0;
        rgb_color frame[NUM_LEDS];

        while (spinnerRunning) {
            for (int i = 0; i < NUM_LEDS; i++) frame[i] = {0, 0, 0};

            uint8_t head = pos % NUM_LEDS;
            uint8_t mid  = (pos + NUM_LEDS - 1) % NUM_LEDS;
            uint8_t tail = (pos + NUM_LEDS - 2) % NUM_LEDS;

            frame[head] = { spinnerColor.r,
                            spinnerColor.g,
                            spinnerColor.b };
            frame[mid]  = { (uint8_t)(spinnerColor.r * 35 / 100),
                            (uint8_t)(spinnerColor.g * 35 / 100),
                            (uint8_t)(spinnerColor.b * 35 / 100) };
            frame[tail] = { (uint8_t)(spinnerColor.r * 12 / 100),
                            (uint8_t)(spinnerColor.g * 12 / 100),
                            (uint8_t)(spinnerColor.b * 12 / 100) };

            ledStrip.write(frame, NUM_LEDS, LED_BRIGHTNESS);
            pos = (pos + 1) % NUM_LEDS;
            vTaskDelay(pdMS_TO_TICKS(80));   // ~12 FPS chase speed
        }

        spinnerTaskHandle = nullptr;
        vTaskDelete(nullptr);
    #elif HAS_WS2812_LED
        // Single WS2812 LED: simple blink/pulse effect
        uint8_t pulse = 0;
        int8_t delta = 5;
        
        while (spinnerRunning) {
            // Scale color by pulse brightness
            uint8_t r = (spinnerColor.r * pulse) >> 8;
            uint8_t g = (spinnerColor.g * pulse) >> 8;
            uint8_t b = (spinnerColor.b * pulse) >> 8;
            
            for (int i = 0; i < NUM_LEDS; i++) {
                ws2812Strip.setPixelColor(i, ws2812Strip.Color(r, g, b));
            }
            ws2812Strip.show();
            
            pulse += delta;
            if (pulse >= 200) delta = -5;
            if (pulse <= 20) delta = 5;
            
            vTaskDelay(pdMS_TO_TICKS(30));   // ~30 FPS pulse
        }

        spinnerTaskHandle = nullptr;
        vTaskDelete(nullptr);
    #else
        (void)param;
        spinnerTaskHandle = nullptr;
        vTaskDelete(nullptr);
    #endif
}

// Start the spinner with a given accent colour (call before blocking scan)
void startLEDSpinner(uint8_t r, uint8_t g, uint8_t b) {
    if (spinnerRunning || spinnerTaskHandle != nullptr) return;
    spinnerColor   = {r, g, b};
    spinnerRunning = true;
    xTaskCreatePinnedToCore(
        ledSpinnerTask,    // task function
        "ledSpinner",      // task name
        2048,              // stack size in bytes
        nullptr,           // parameter
        1,                 // priority
        &spinnerTaskHandle,
        0                  // core 0 (Arduino loop runs on core 1)
    );
}

// Stop the spinner and restore a solid colour (call after scan completes)
void stopLEDSpinner(uint8_t r, uint8_t g, uint8_t b) {
    spinnerRunning = false;
    uint32_t deadline = millis() + 400;
    while (spinnerTaskHandle != nullptr && millis() < deadline) delay(10);
    setAllLEDs(r, g, b);
}

// ════════════════════════════════════════════════════════════════
//  UI STYLE HELPERS
// ════════════════════════════════════════════════════════════════

static void applyScreenStyle(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr,   LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *createHeader(lv_obj_t *parent, const char *text) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SCREEN_W, 28);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar,     lv_color_hex(0x161b22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar,       0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x58a6ff), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
    return bar;
}

static lv_obj_t *createBackBtn(lv_obj_t *parent, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 100, 26);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 6, -4);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x21262d), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1f4f8f), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x388bfd), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 5, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xe6edf3), LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t *createActionBtn(lv_obj_t *parent,
                                  const char *label,
                                  lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 110, 26);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a4a1a), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1f6f1f), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3fb950), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x3fb950), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 5, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xe6edf3), LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

static void styleListBtn(lv_obj_t *btn) {
    lv_obj_set_height(btn, 26);
    lv_obj_set_style_bg_color(btn,   lv_color_hex(0x161b22), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn,     LV_OPA_COVER,           LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xe6edf3), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn,   lv_color_hex(0x1f4f8f), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn,     LV_OPA_COVER,           LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x58a6ff), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn,   lv_color_hex(0x388bfd), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(btn, 8, LV_PART_MAIN);
}

static void setGroup(lv_group_t *g)       { lv_indev_set_group(lvIndev, g); }
static void deleteGroup(lv_group_t **g)   { if (*g) { lv_group_delete(*g); *g = nullptr; } }

// ════════════════════════════════════════════════════════════════
//  MAIN MENU
// ════════════════════════════════════════════════════════════════

struct MenuItem { const char *icon; const char *label; const char *subTitle; };
static const MenuItem MENU_ITEMS[4] = {
    { LV_SYMBOL_WIFI,      "WiFi Tools",  LV_SYMBOL_WIFI      "  WiFi Tools" },
    { LV_SYMBOL_BLUETOOTH, "BLE Tools",   LV_SYMBOL_BLUETOOTH "  BLE Tools"  },
    { LV_SYMBOL_SETTINGS,  "Misc Tools",  LV_SYMBOL_SETTINGS  "  Misc Tools" },
    { LV_SYMBOL_GPS,       "GPS Tools",   LV_SYMBOL_GPS       "  GPS Tools"  },
};

static void cb_menuFocused(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    currentMenu = idx;
    setAllLEDs(MENU_COLORS[idx].r, MENU_COLORS[idx].g, MENU_COLORS[idx].b);
}

static void cb_menuClicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if      (idx == 0) createWiFiMenu();
    else if (idx == 1) createBLEMenu();
    else if (idx == 2) createMiscMenu();
    else if (idx == 3) createGPSMenu();
    else               createSubScreen(idx);
}

void createMainMenu() {
    if (mainScreen) { lv_obj_delete(mainScreen); mainScreen = nullptr; }
    mainScreen = lv_obj_create(nullptr);
    applyScreenStyle(mainScreen);
    createHeader(mainScreen, DEVICE_NAME);

    lv_obj_t *list = lv_list_create(mainScreen);
    lv_obj_set_size(list, SCREEN_W, SCREEN_H - 28);
    lv_obj_set_pos(list, 0, 28);
    lv_obj_set_style_bg_color(list,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list,      6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list,      4, LV_PART_MAIN);

    deleteGroup(&navGroup);
    navGroup = lv_group_create();
    setGroup(navGroup);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_list_add_btn(list, MENU_ITEMS[i].icon, MENU_ITEMS[i].label);
        styleListBtn(btn);
        lv_obj_set_height(btn, 30);
        lv_obj_add_event_cb(btn, cb_menuFocused, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, cb_menuClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(navGroup, btn);
    }

    lv_screen_load(mainScreen);
}

// ════════════════════════════════════════════════════════════════
//  PLACEHOLDER SUB-SCREEN  (BLE / Misc / GPS)
// ════════════════════════════════════════════════════════════════

static void cb_subBack(lv_event_t *e) {
    lv_screen_load_anim(mainScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
    deleteGroup(&subGroup);
    setGroup(navGroup);
    setAllLEDs(MENU_COLORS[currentMenu].r,
               MENU_COLORS[currentMenu].g,
               MENU_COLORS[currentMenu].b);
}

void createSubScreen(int idx) {
    if (subScreen) { lv_obj_delete(subScreen); subScreen = nullptr; }
    subScreen = lv_obj_create(nullptr);
    applyScreenStyle(subScreen);
    createHeader(subScreen, MENU_ITEMS[idx].subTitle);

    lv_obj_t *msg = lv_label_create(subScreen);
    lv_label_set_text_fmt(msg, "%s\nComing soon...", MENU_ITEMS[idx].label);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *backBtn = createBackBtn(subScreen, cb_subBack);
    deleteGroup(&subGroup);
    subGroup = lv_group_create();
    lv_group_add_obj(subGroup, backBtn);
    setGroup(subGroup);

    setAllLEDs(MENU_COLORS[idx].r, MENU_COLORS[idx].g, MENU_COLORS[idx].b, 3);
    lv_screen_load_anim(subScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  MISC TOOLS
//
//  Follows the same menu → tool pattern as WiFi, BLE, and GPS.
// ════════════════════════════════════════════════════════════════

static const char *MISC_TOOL_LABELS[3] = {
    LV_SYMBOL_SETTINGS "  Device Info",
    LV_SYMBOL_UPLOAD   "  SD Update",
    LV_SYMBOL_IMAGE    "  Brightness"
};

static void cb_miscMenuBack(lv_event_t *e) {
    miscMenuScreen = nullptr;
    deleteGroup(&miscMenuGroup);
    setGroup(navGroup);
    lv_screen_load_anim(mainScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b);
}

static void cb_miscToolBack(lv_event_t *e) {
    miscToolScreen = nullptr;
    deleteGroup(&miscToolGroup);
    setGroup(miscMenuGroup);
    lv_screen_load_anim(miscMenuScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b, 3);
}

static void cb_miscToolSelected(lv_event_t *e) {
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    switch (t) {
        case 0: createDeviceInfo(); break;
        case 1: createSDUpdate();          break;
        case 2: createBrightnessControl(); break;
    }
}

void createMiscMenu() {
    if (miscMenuScreen) { lv_obj_delete(miscMenuScreen); miscMenuScreen = nullptr; }
    miscMenuScreen = lv_obj_create(nullptr);
    applyScreenStyle(miscMenuScreen);
    createHeader(miscMenuScreen, LV_SYMBOL_SETTINGS "  Misc Tools");

    lv_obj_t *list = lv_list_create(miscMenuScreen);
    lv_obj_set_size(list, SCREEN_W, SCREEN_H - 28 - 34);
    lv_obj_set_pos(list, 0, 28);
    lv_obj_set_style_bg_color(list,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(list,      6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_row(list,      4,                      LV_PART_MAIN);

    deleteGroup(&miscMenuGroup);
    miscMenuGroup = lv_group_create();

    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_list_add_btn(list, nullptr, MISC_TOOL_LABELS[i]);
        styleListBtn(btn);
        lv_obj_set_height(btn, 30);
        lv_obj_add_event_cb(btn, cb_miscToolSelected, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_group_add_obj(miscMenuGroup, btn);
    }

    lv_obj_t *backBtn = createBackBtn(miscMenuScreen, cb_miscMenuBack);
    lv_group_add_obj(miscMenuGroup, backBtn);
    setGroup(miscMenuGroup);

    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b, 3);
    lv_screen_load_anim(miscMenuScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ── Misc Tool 1 — Device Info ────────────────────────────────────

void createDeviceInfo() {
    if (miscToolScreen) { lv_obj_delete(miscToolScreen); miscToolScreen = nullptr; }
    miscToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(miscToolScreen);
    createHeader(miscToolScreen, LV_SYMBOL_SETTINGS "  Device Info");

    // Card
    lv_obj_t *card = lv_obj_create(miscToolScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 38);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,     lv_color_hex(0x161b22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1,                      LV_PART_MAIN);
    lv_obj_set_style_radius(card,       6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,      6,                      LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Gather device info at build time
    uint64_t chipId   = ESP.getEfuseMac();
    uint32_t heap     = ESP.getFreeHeap();
    uint32_t flash    = ESP.getFlashChipSize() / 1024;  // KB
    uint32_t flashSpd = ESP.getFlashChipSpeed() / 1000000; // MHz
    uint8_t cores     = ESP.getChipCores();
    uint32_t cpuMHz   = ESP.getCpuFreqMHz();
    const char *idfVer = ESP.getSdkVersion();

    // Detect chip model at runtime using esp_chip_info()
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    const char* chipModel;
    switch (chip_info.model) {
        case CHIP_ESP32:
            chipModel = "ESP32";
            break;
        case CHIP_ESP32S2:
            chipModel = "ESP32-S2";
            break;
        case CHIP_ESP32S3:
            chipModel = "ESP32-S3";
            break;
        case CHIP_ESP32C2:
            chipModel = "ESP32-C2";
            break;
        case CHIP_ESP32C3:
            chipModel = "ESP32-C3";
            break;
        case CHIP_ESP32C5:
            chipModel = "ESP32-C5";
            break;
        case CHIP_ESP32C6:
            chipModel = "ESP32-C6";
            break;
        case CHIP_ESP32C61:
            chipModel = "ESP32-C61";
            break;
        case CHIP_ESP32H2:
            chipModel = "ESP32-H2";
            break;
        case CHIP_ESP32P4:
            chipModel = "ESP32-P4";
            break;
        default:
            chipModel = "Unknown";
            break;
    }

    // WiFi MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Chip ID (lower 4 bytes of efuse MAC)
    char chipIdStr[12];
    snprintf(chipIdStr, sizeof(chipIdStr), "%08X", (uint32_t)(chipId >> 32));

    char info[320];
    snprintf(info, sizeof(info),
             "Chip : %s  (%d cores)\n"
             "CPU  : %lu MHz\n"
             "Flash: %lu KB @ %lu MHz\n"
             "Heap : %lu B free\n"
             "MAC  : %s\n"
             "ID   : %s\n"
             "IDF  : %s",
             chipModel,
             cores,
             (unsigned long)cpuMHz,
             (unsigned long)flash,
             (unsigned long)flashSpd,
             (unsigned long)heap,
             macStr,
             chipIdStr,
             idfVer);

    lv_obj_t *infoLbl = lv_label_create(card);
    lv_label_set_text(infoLbl, info);
    lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(infoLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(0xe6edf3), LV_PART_MAIN);
    lv_obj_align(infoLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *backBtn = createBackBtn(miscToolScreen, cb_miscToolBack);
    deleteGroup(&miscToolGroup);
    miscToolGroup = lv_group_create();
    lv_group_add_obj(miscToolGroup, backBtn);
    setGroup(miscToolGroup);

    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b, LED_BRIGHTNESS);

    lv_screen_load_anim(miscToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  MISC TOOL 2 – SD UPDATE
//
//  Streams /firmware.bin from SD card into flash using the ESP32
//  built-in Update library. Atomic — bad image leaves current
//  firmware intact. Reboots automatically on success.
//  Workflow: Arduino IDE → Sketch → Export Compiled Binary →
//            rename to firmware.bin → copy to SD root → Flash.
// ════════════════════════════════════════════════════════════════


static lv_obj_t *otaStatusLbl = nullptr;
static lv_obj_t *otaBar       = nullptr;
static lv_obj_t *otaPctLbl    = nullptr;
static lv_obj_t *otaFlashBtn  = nullptr;

static void otaSetStatus(const char *msg, uint32_t color) {
    if (!otaStatusLbl) return;
    lv_label_set_text(otaStatusLbl, msg);
    lv_obj_set_style_text_color(otaStatusLbl, lv_color_hex(color), LV_PART_MAIN);
    lv_timer_handler();
}

static void cb_doFlash(lv_event_t *e) {
    lv_obj_add_state(otaFlashBtn, LV_STATE_DISABLED);

    if (!SD.begin(SD_CS, sdSPI)) {
        otaSetStatus(LV_SYMBOL_CLOSE "  SD card not found!", 0xf85149);
        lv_obj_remove_state(otaFlashBtn, LV_STATE_DISABLED);
        return;
    }

    File f = SD.open(OTA_FILENAME, FILE_READ);
    if (!f) {
        otaSetStatus(LV_SYMBOL_CLOSE "  firmware.bin not found!", 0xf85149);
        SD.end();
        lv_obj_remove_state(otaFlashBtn, LV_STATE_DISABLED);
        return;
    }

    size_t fileSize = f.size();
    if (fileSize == 0) {
        otaSetStatus(LV_SYMBOL_CLOSE "  firmware.bin is empty!", 0xf85149);
        f.close();
        SD.end();
        lv_obj_remove_state(otaFlashBtn, LV_STATE_DISABLED);
        return;
    }

    otaSetStatus(LV_SYMBOL_REFRESH "  Flashing...", 0xe3b341);

    if (!Update.begin(fileSize, U_FLASH)) {
        otaSetStatus(LV_SYMBOL_CLOSE "  Update.begin() failed!", 0xf85149);
        f.close();
        SD.end();
        lv_obj_remove_state(otaFlashBtn, LV_STATE_DISABLED);
        return;
    }

    uint8_t buf[4096];
    size_t  written = 0;
    char    pctBuf[16];

    while (f.available()) {
        size_t toRead = (size_t)f.available();
        if (toRead > sizeof(buf)) toRead = sizeof(buf);
        size_t n = f.read(buf, toRead);
        if (Update.write(buf, n) != n) {
            Update.abort();
            otaSetStatus(LV_SYMBOL_CLOSE "  Write error — aborted!", 0xf85149);
            f.close();
            SD.end();
            lv_obj_remove_state(otaFlashBtn, LV_STATE_DISABLED);
            return;
        }
        written += n;
        int pct = (int)((written * 100UL) / fileSize);
        lv_bar_set_value(otaBar, pct, LV_ANIM_OFF);
        snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
        lv_label_set_text(otaPctLbl, pctBuf);
        lv_timer_handler();
    }

    f.close();
    SD.end();

    if (!Update.end(true)) {
        otaSetStatus(LV_SYMBOL_CLOSE "  Verification failed!", 0xf85149);
        lv_obj_remove_state(otaFlashBtn, LV_STATE_DISABLED);
        return;
    }

    lv_bar_set_value(otaBar, 100, LV_ANIM_OFF);
    lv_label_set_text(otaPctLbl, "100%");
    otaSetStatus(LV_SYMBOL_OK "  Done! Rebooting...", 0x3fb950);
    delay(1500);
    ESP.restart();
}

void createSDUpdate() {
    otaStatusLbl = nullptr;
    otaBar       = nullptr;
    otaPctLbl    = nullptr;
    otaFlashBtn  = nullptr;

    if (miscToolScreen) { lv_obj_delete(miscToolScreen); miscToolScreen = nullptr; }
    miscToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(miscToolScreen);
    createHeader(miscToolScreen, LV_SYMBOL_UPLOAD "  SD Update");

    otaStatusLbl = lv_label_create(miscToolScreen);
    lv_label_set_text(otaStatusLbl,
        "Place firmware.bin in SD root\n"
        "then press Flash.");
    lv_obj_set_style_text_color(otaStatusLbl, lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(otaStatusLbl, 8, 30);

    otaBar = lv_bar_create(miscToolScreen);
    lv_obj_set_size(otaBar, SCREEN_W - 16, 10);
    lv_obj_set_pos(otaBar, 8, 78);
    lv_bar_set_range(otaBar, 0, 100);
    lv_bar_set_value(otaBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(otaBar, lv_color_hex(0x21262d), LV_PART_MAIN);
    lv_obj_set_style_bg_color(otaBar, lv_color_hex(0x3fb950), LV_PART_INDICATOR);
    lv_obj_set_style_radius(otaBar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(otaBar, 3, LV_PART_INDICATOR);

    otaPctLbl = lv_label_create(miscToolScreen);
    lv_label_set_text(otaPctLbl, "");
    lv_obj_set_style_text_color(otaPctLbl, lv_color_hex(0x3fb950), LV_PART_MAIN);
    lv_obj_set_pos(otaPctLbl, 8, 92);

    lv_obj_t *backBtn = createBackBtn(miscToolScreen, cb_miscToolBack);

    otaFlashBtn = lv_btn_create(miscToolScreen);
    lv_obj_set_size(otaFlashBtn, 90, 28);
    lv_obj_align(otaFlashBtn, LV_ALIGN_BOTTOM_MID, 30, -4);
    lv_obj_set_style_bg_color(otaFlashBtn, lv_color_hex(0x238636), LV_PART_MAIN);
    lv_obj_set_style_radius(otaFlashBtn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(otaFlashBtn, cb_doFlash, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *flashLbl = lv_label_create(otaFlashBtn);
    lv_label_set_text(flashLbl, LV_SYMBOL_UPLOAD "  Flash");
    lv_obj_center(flashLbl);

    deleteGroup(&miscToolGroup);
    miscToolGroup = lv_group_create();
    lv_group_add_obj(miscToolGroup, backBtn);
    lv_group_add_obj(miscToolGroup, otaFlashBtn);
    setGroup(miscToolGroup);

    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b, LED_BRIGHTNESS);

    lv_screen_load_anim(miscToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  MISC TOOL 3 – BRIGHTNESS CONTROL
//
//  Controls TFT backlight via LEDC PWM on IO15.
//  Rotary encoder scrolls the bar up/down in 5% steps.
//  Level persists in lcdBrightness global for the session.
// ════════════════════════════════════════════════════════════════

static lv_obj_t *brightBar      = nullptr;
static lv_obj_t *brightPctLbl   = nullptr;
static lv_obj_t *brightDownBtn  = nullptr;
static lv_obj_t *brightUpBtn    = nullptr;

static void applyBrightness() {
    ledcWrite(LCD_BL_PIN, TFT_BL_INVERT ? (255 - lcdBrightness) : lcdBrightness);
    if (!brightBar || !brightPctLbl) return;
    int pct = (lcdBrightness * 100) / 255;
    lv_bar_set_value(brightBar, pct, LV_ANIM_ON);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(brightPctLbl, buf);
}

static void cb_brightDown(lv_event_t *e) {
    lcdBrightness -= 13;   // ~5% of 255
    if (lcdBrightness < 13) lcdBrightness = 13;  // min ~5% — keep display visible
    applyBrightness();
}

static void cb_brightUp(lv_event_t *e) {
    lcdBrightness += 13;
    if (lcdBrightness > 255) lcdBrightness = 255;
    applyBrightness();
}

void createBrightnessControl() {
    brightBar     = nullptr;
    brightPctLbl  = nullptr;
    brightDownBtn = nullptr;
    brightUpBtn   = nullptr;

    if (miscToolScreen) { lv_obj_delete(miscToolScreen); miscToolScreen = nullptr; }
    miscToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(miscToolScreen);
    createHeader(miscToolScreen, LV_SYMBOL_IMAGE "  Brightness");

    // Current percentage label
    brightPctLbl = lv_label_create(miscToolScreen);
    lv_obj_set_style_text_color(brightPctLbl, lv_color_hex(0xe6edf3), LV_PART_MAIN);
    lv_obj_align(brightPctLbl, LV_ALIGN_CENTER, 0, -30);

    // Brightness bar
    brightBar = lv_bar_create(miscToolScreen);
    lv_obj_set_size(brightBar, SCREEN_W - 32, 16);
    lv_obj_align(brightBar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(brightBar, 0, 100);
    lv_obj_set_style_bg_color(brightBar, lv_color_hex(0x21262d), LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightBar, lv_color_hex(0xe3b341), LV_PART_INDICATOR);
    lv_obj_set_style_radius(brightBar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(brightBar, 4, LV_PART_INDICATOR);

    // – button
    brightDownBtn = lv_btn_create(miscToolScreen);
    lv_obj_set_size(brightDownBtn, 52, 30);
    lv_obj_align(brightDownBtn, LV_ALIGN_CENTER, -46, 36);
    lv_obj_set_style_bg_color(brightDownBtn, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_radius(brightDownBtn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(brightDownBtn, cb_brightDown, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *dLbl = lv_label_create(brightDownBtn);
    lv_label_set_text(dLbl, LV_SYMBOL_MINUS);
    lv_obj_center(dLbl);

    // + button
    brightUpBtn = lv_btn_create(miscToolScreen);
    lv_obj_set_size(brightUpBtn, 52, 30);
    lv_obj_align(brightUpBtn, LV_ALIGN_CENTER, 46, 36);
    lv_obj_set_style_bg_color(brightUpBtn, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_radius(brightUpBtn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(brightUpBtn, cb_brightUp, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *uLbl = lv_label_create(brightUpBtn);
    lv_label_set_text(uLbl, LV_SYMBOL_PLUS);
    lv_obj_center(uLbl);

    lv_obj_t *backBtn = createBackBtn(miscToolScreen, cb_miscToolBack);
    deleteGroup(&miscToolGroup);
    miscToolGroup = lv_group_create();
    lv_group_add_obj(miscToolGroup, backBtn);
    lv_group_add_obj(miscToolGroup, brightDownBtn);
    lv_group_add_obj(miscToolGroup, brightUpBtn);
    setGroup(miscToolGroup);

    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b, LED_BRIGHTNESS);

    // Set bar and label to current value
    applyBrightness();

    lv_screen_load_anim(miscToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  GPS TOOLS
//
//  Follows the same menu → tool pattern as WiFi and BLE.
//  GPS data is fed non-blocking in loop() via TinyGPS++.
//  UART1: GPIO44 RX, GPIO43 TX, 9600 baud.
// ════════════════════════════════════════════════════════════════

static const char *GPS_TOOL_LABELS[2] = {
    LV_SYMBOL_GPS     "  GPS Stats",
    LV_SYMBOL_SAVE    "  Wiggle Wars"
};

// ── Shared GPS back callbacks ────────────────────────────────────

static lv_timer_t *gpsTimer = nullptr;

// Forward declarations for Wiggle Wars state — defined later in TOOL 2
static bool          wiggleRunning = false;
static lv_timer_t   *wiggleTimer   = nullptr;

static void cb_gpsMenuBack(lv_event_t *e) {
    gpsMenuScreen = nullptr;
    deleteGroup(&gpsMenuGroup);
    setGroup(navGroup);
    lv_screen_load_anim(mainScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    setAllLEDs(MENU_COLORS[3].r, MENU_COLORS[3].g, MENU_COLORS[3].b);
}

static void cb_gpsToolBack(lv_event_t *e) {
    if (gpsTimer)  { lv_timer_delete(gpsTimer);  gpsTimer  = nullptr; }
    // Clean up wiggle session if active
    if (wiggleRunning) {
        wiggleRunning = false;
        WiFi.scanDelete();
        SD.end();
    }
    if (wiggleTimer) { lv_timer_delete(wiggleTimer); wiggleTimer = nullptr; }
    gpsToolScreen = nullptr;
    deleteGroup(&gpsToolGroup);
    setGroup(gpsMenuGroup);
    lv_screen_load_anim(gpsMenuScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    setAllLEDs(MENU_COLORS[3].r, MENU_COLORS[3].g, MENU_COLORS[3].b, 3);
}

static void cb_gpsToolSelected(lv_event_t *e) {
    #if HAS_GPS
        int t = (int)(intptr_t)lv_event_get_user_data(e);
        switch (t) {
            case 0: createGPSStats();    break;
            case 1: createWiggleWars();  break;
        }
    #else
        (void)e;
    #endif
}

void createGPSMenu() {
    if (gpsMenuScreen) { lv_obj_delete(gpsMenuScreen); gpsMenuScreen = nullptr; }
    gpsMenuScreen = lv_obj_create(nullptr);
    applyScreenStyle(gpsMenuScreen);
    createHeader(gpsMenuScreen, LV_SYMBOL_GPS "  GPS Tools");

    lv_obj_t *list = lv_list_create(gpsMenuScreen);
    lv_obj_set_size(list, SCREEN_W, SCREEN_H - 28 - 34);
    lv_obj_set_pos(list, 0, 28);
    lv_obj_set_style_bg_color(list,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(list,      6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_row(list,      4,                      LV_PART_MAIN);

    deleteGroup(&gpsMenuGroup);
    gpsMenuGroup = lv_group_create();

    #if HAS_GPS
        if (gpsIsConfigured()) {
            // GPS pins are configured, show GPS tools
            for (int i = 0; i < 2; i++) {
                lv_obj_t *btn = lv_list_add_btn(list, nullptr, GPS_TOOL_LABELS[i]);
                styleListBtn(btn);
                lv_obj_set_height(btn, 30);
                lv_obj_add_event_cb(btn, cb_gpsToolSelected, LV_EVENT_CLICKED,
                                    (void *)(intptr_t)i);
                lv_group_add_obj(gpsMenuGroup, btn);
            }
        } else {
            // GPS support compiled but pins not configured
            lv_obj_t *btn = lv_list_add_btn(list, LV_SYMBOL_WARNING, "GPS Not Configured");
            styleListBtn(btn);
            lv_obj_set_height(btn, 30);
            lv_group_add_obj(gpsMenuGroup, btn);
            
            // Add hint label
            lv_obj_t *hint = lv_label_create(list);
            lv_label_set_text(hint, "Set GPS_RX_PIN/GPS_TX_PIN in config.h");
            lv_obj_set_style_text_color(hint, lv_color_hex(0x8b949e), LV_PART_MAIN);
            lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
        }
    #else
        // No GPS support compiled in
        lv_obj_t *btn = lv_list_add_btn(list, LV_SYMBOL_WARNING, "GPS Not Available");
        styleListBtn(btn);
        lv_obj_set_height(btn, 30);
        lv_group_add_obj(gpsMenuGroup, btn);
    #endif

    lv_obj_t *backBtn = createBackBtn(gpsMenuScreen, cb_gpsMenuBack);
    lv_group_add_obj(gpsMenuGroup, backBtn);
    setGroup(gpsMenuGroup);

    setAllLEDs(MENU_COLORS[3].r, MENU_COLORS[3].g, MENU_COLORS[3].b, 3);
    lv_screen_load_anim(gpsMenuScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ── GPS Stats ────────────────────────────────────────────────────

static lv_obj_t *gpsFixLbl = nullptr;
static lv_obj_t *gpsLatLbl = nullptr;
static lv_obj_t *gpsLngLbl = nullptr;
static lv_obj_t *gpsSpdLbl = nullptr;
static lv_obj_t *gpsAltLbl = nullptr;
static lv_obj_t *gpsSatLbl = nullptr;

static void gps_refresh_cb(lv_timer_t *) {
    if (!gpsFixLbl) return;

    bool hasFix = gps.location.isValid() && gps.location.age() < 3000;

    if (!hasFix) {
        lv_label_set_text(gpsFixLbl, LV_SYMBOL_WARNING "  Searching for fix...");
        lv_obj_set_style_text_color(gpsFixLbl, lv_color_hex(0xe3b341), LV_PART_MAIN);
        lv_label_set_text(gpsLatLbl, "Lat:  ---.------");
        lv_label_set_text(gpsLngLbl, "Lng:  ---.------");
        lv_label_set_text(gpsSpdLbl, "Spd:  --- km/h");
        lv_label_set_text(gpsAltLbl, "Alt:  --- m");
        char satBuf[32];
        snprintf(satBuf, sizeof(satBuf), "Sats: %d", (int)gps.satellites.value());
        lv_label_set_text(gpsSatLbl, satBuf);
        return;
    }

    lv_label_set_text(gpsFixLbl, LV_SYMBOL_GPS "  Fix acquired");
    lv_obj_set_style_text_color(gpsFixLbl, lv_color_hex(0x3fb950), LV_PART_MAIN);

    char buf[40];
    snprintf(buf, sizeof(buf), "Lat:  %.6f", gps.location.lat());
    lv_label_set_text(gpsLatLbl, buf);
    snprintf(buf, sizeof(buf), "Lng:  %.6f", gps.location.lng());
    lv_label_set_text(gpsLngLbl, buf);
    snprintf(buf, sizeof(buf), "Spd:  %.1f km/h", gps.speed.kmph());
    lv_label_set_text(gpsSpdLbl, buf);
    snprintf(buf, sizeof(buf), "Alt:  %.0f m", gps.altitude.meters());
    lv_label_set_text(gpsAltLbl, buf);
    snprintf(buf, sizeof(buf), "Sats: %d", (int)gps.satellites.value());
    lv_label_set_text(gpsSatLbl, buf);
}

void createGPSStats() {
    gpsFixLbl = nullptr;
    gpsLatLbl = nullptr;
    gpsLngLbl = nullptr;
    gpsSpdLbl = nullptr;
    gpsAltLbl = nullptr;
    gpsSatLbl = nullptr;

    if (gpsToolScreen) { lv_obj_delete(gpsToolScreen); gpsToolScreen = nullptr; }
    gpsToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(gpsToolScreen);
    createHeader(gpsToolScreen, LV_SYMBOL_GPS "  GPS Stats");

    lv_obj_t *card = lv_obj_create(gpsToolScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 38);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,     lv_color_hex(0x161b22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1,                      LV_PART_MAIN);
    lv_obj_set_style_radius(card,       6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,      6,                      LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    const int rowH = 18;

    gpsFixLbl = lv_label_create(card);
    lv_label_set_text(gpsFixLbl, LV_SYMBOL_WARNING "  Searching for fix...");
    lv_obj_set_style_text_color(gpsFixLbl, lv_color_hex(0xe3b341), LV_PART_MAIN);
    lv_obj_set_pos(gpsFixLbl, 0, 0);

    gpsLatLbl = lv_label_create(card);
    lv_label_set_text(gpsLatLbl, "Lat:  ---.------");
    lv_obj_set_style_text_color(gpsLatLbl, lv_color_hex(0x58a6ff), LV_PART_MAIN);
    lv_obj_set_pos(gpsLatLbl, 0, rowH * 1 + 4);

    gpsLngLbl = lv_label_create(card);
    lv_label_set_text(gpsLngLbl, "Lng:  ---.------");
    lv_obj_set_style_text_color(gpsLngLbl, lv_color_hex(0x58a6ff), LV_PART_MAIN);
    lv_obj_set_pos(gpsLngLbl, 0, rowH * 2 + 4);

    gpsSpdLbl = lv_label_create(card);
    lv_label_set_text(gpsSpdLbl, "Spd:  --- km/h");
    lv_obj_set_style_text_color(gpsSpdLbl, lv_color_hex(0x3fb950), LV_PART_MAIN);
    lv_obj_set_pos(gpsSpdLbl, 0, rowH * 3 + 4);

    gpsAltLbl = lv_label_create(card);
    lv_label_set_text(gpsAltLbl, "Alt:  --- m");
    lv_obj_set_style_text_color(gpsAltLbl, lv_color_hex(0x3fb950), LV_PART_MAIN);
    lv_obj_set_pos(gpsAltLbl, 0, rowH * 4 + 4);

    gpsSatLbl = lv_label_create(card);
    lv_label_set_text(gpsSatLbl, "Sats: 0");
    lv_obj_set_style_text_color(gpsSatLbl, lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(gpsSatLbl, 0, rowH * 5 + 4);

    lv_obj_t *backBtn = createBackBtn(gpsToolScreen, cb_gpsToolBack);
    deleteGroup(&gpsToolGroup);
    gpsToolGroup = lv_group_create();
    lv_group_add_obj(gpsToolGroup, backBtn);
    setGroup(gpsToolGroup);

    setAllLEDs(MENU_COLORS[3].r, MENU_COLORS[3].g, MENU_COLORS[3].b, LED_BRIGHTNESS);

    if (gpsTimer) { lv_timer_delete(gpsTimer); gpsTimer = nullptr; }
    gpsTimer = lv_timer_create(gps_refresh_cb, 1000, nullptr);

    lv_screen_load_anim(gpsToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  GPS TOOL 2 – WIGGLE WARS
//
//  WiGLE-compatible wardrive logger. Scans WiFi networks, tags
//  each with GPS coordinates, and writes a CSV to the SD card
//  in WiGLE format (uploadable to wigle.net).
//
//  SD card on dedicated HSPI bus (IO38-41) — safe with TFT SPI.
//  File named: /wigle_YYYYMMDD_HHMMSS.csv
//  Requires GPS fix before scanning starts.
// ════════════════════════════════════════════════════════════════

static lv_obj_t   *wiggleStatusLbl  = nullptr;
static lv_obj_t   *wiggleScanLbl    = nullptr;
static lv_obj_t   *wiggleNetLbl     = nullptr;
static lv_obj_t   *wiggleFileLbl    = nullptr;
static lv_obj_t   *wiggleStartBtn   = nullptr;
static lv_obj_t   *wiggleStopBtn    = nullptr;
// wiggleTimer and wiggleRunning forward-declared above cb_gpsToolBack

static bool   wiggleSDReady   = false;
static int    wiggleScanCount = 0;
static int    wiggleNetCount  = 0;
static char   wiggleFilename[40];
static File   wiggleFile;

// Write WiGLE CSV header
static bool wiggleOpenFile() {
    if (!gps.date.isValid() || !gps.time.isValid()) return false;

    snprintf(wiggleFilename, sizeof(wiggleFilename),
             "/wigle_%04d%02d%02d_%02d%02d%02d.csv",
             gps.date.year(), gps.date.month(),  gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());

    wiggleFile = SD.open(wiggleFilename, FILE_WRITE);
    if (!wiggleFile) return false;

    wiggleFile.println(
        "WigleWifi-1.4,appRelease=1.0,model=RogueRadar,release=1.0,"
        "device=ESP32-S3,display=TFT,board=ESP32-S3,brand=LilyGO");
    wiggleFile.println(
        "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,"
        "CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
    wiggleFile.close();
    return true;
}

// Append one scan's networks to the CSV
static void wiggleWriteScan() {
    wiggleFile = SD.open(wiggleFilename, FILE_APPEND);
    if (!wiggleFile) return;

    char timestamp[24];
    snprintf(timestamp, sizeof(timestamp),
             "%04d-%02d-%02d %02d:%02d:%02d",
             gps.date.year(), gps.date.month(),  gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());

    double lat = gps.location.lat();
    double lng = gps.location.lng();
    double alt = gps.altitude.meters();
    double acc = gps.hdop.hdop() * 5.0;  // rough accuracy estimate from HDOP

    int nets = WiFi.scanComplete();
    for (int i = 0; i < nets; i++) {
        int ch   = WiFi.channel(i);
        int freq = (ch <= 13) ? (2407 + ch * 5) : (5000 + ch * 5);

        // Auth mode string
        const char *auth;
        switch (WiFi.encryptionType(i)) {
            case WIFI_AUTH_OPEN:         auth = "[ESS]";        break;
            case WIFI_AUTH_WEP:          auth = "[WEP][ESS]";   break;
            case WIFI_AUTH_WPA_PSK:      auth = "[WPA-PSK][ESS]"; break;
            case WIFI_AUTH_WPA2_PSK:     auth = "[WPA2-PSK][ESS]"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth = "[WPA-PSK][WPA2-PSK][ESS]"; break;
            case WIFI_AUTH_WPA3_PSK:     auth = "[WPA3-SAE][ESS]"; break;
            default:                     auth = "[ESS]";        break;
        }

        wiggleFile.print(WiFi.BSSIDstr(i)); wiggleFile.print(',');
        // Escape SSID quotes
        wiggleFile.print('"');
        wiggleFile.print(WiFi.SSID(i));
        wiggleFile.print('"'); wiggleFile.print(',');
        wiggleFile.print(auth);           wiggleFile.print(',');
        wiggleFile.print(timestamp);      wiggleFile.print(',');
        wiggleFile.print(ch);             wiggleFile.print(',');
        wiggleFile.print(freq);           wiggleFile.print(',');
        wiggleFile.print(WiFi.RSSI(i));   wiggleFile.print(',');
        wiggleFile.print(lat, 6);         wiggleFile.print(',');
        wiggleFile.print(lng, 6);         wiggleFile.print(',');
        wiggleFile.print(alt, 1);         wiggleFile.print(',');
        wiggleFile.print(acc, 1);         wiggleFile.print(',');
        wiggleFile.println("WIFI");

        wiggleNetCount++;
    }
    wiggleFile.close();
}

static void wiggle_refresh_cb(lv_timer_t *) {
    if (!wiggleStatusLbl) return;

    if (!wiggleRunning) return;

    // Need GPS fix
    bool hasFix = gps.location.isValid() && gps.location.age() < 3000
                  && gps.satellites.value() >= 4;

    if (!hasFix) {
        lv_label_set_text(wiggleStatusLbl,
            LV_SYMBOL_WARNING "  Waiting for GPS fix...");
        lv_obj_set_style_text_color(wiggleStatusLbl,
            lv_color_hex(0xe3b341), LV_PART_MAIN);
        return;
    }

    // Trigger async WiFi scan (non-blocking)
    int scanResult = WiFi.scanComplete();

    if (scanResult == WIFI_SCAN_FAILED || scanResult == WIFI_SCAN_RUNNING) {
        if (scanResult == WIFI_SCAN_FAILED) {
            WiFi.scanNetworks(true);  // async=true
        }
        lv_label_set_text(wiggleStatusLbl,
            LV_SYMBOL_REFRESH "  Scanning...");
        lv_obj_set_style_text_color(wiggleStatusLbl,
            lv_color_hex(0x58a6ff), LV_PART_MAIN);
        return;
    }

    // Scan complete — write results
    if (scanResult >= 0) {
        wiggleScanCount++;
        wiggleWriteScan();
        WiFi.scanDelete();
        WiFi.scanNetworks(true);  // kick off next async scan

        char buf[40];
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_GPS "  Running  Sats:%d",
                 (int)gps.satellites.value());
        lv_label_set_text(wiggleStatusLbl, buf);
        lv_obj_set_style_text_color(wiggleStatusLbl,
            lv_color_hex(0x3fb950), LV_PART_MAIN);

        snprintf(buf, sizeof(buf), "Scans: %d", wiggleScanCount);
        lv_label_set_text(wiggleScanLbl, buf);

        snprintf(buf, sizeof(buf), "Nets logged: %d", wiggleNetCount);
        lv_label_set_text(wiggleNetLbl, buf);
    }
}

static void cb_wiggleStart(lv_event_t *e) {
    if (wiggleRunning) return;

    // Mount SD
    if (!SD.begin(SD_CS, sdSPI)) {
        lv_label_set_text(wiggleStatusLbl,
            LV_SYMBOL_CLOSE "  SD card not found!");
        lv_obj_set_style_text_color(wiggleStatusLbl,
            lv_color_hex(0xf85149), LV_PART_MAIN);
        return;
    }

    // Need fix to name file with GPS time
    if (!gps.date.isValid() || !gps.time.isValid()) {
        lv_label_set_text(wiggleStatusLbl,
            LV_SYMBOL_WARNING "  Need GPS fix first!");
        lv_obj_set_style_text_color(wiggleStatusLbl,
            lv_color_hex(0xe3b341), LV_PART_MAIN);
        return;
    }

    if (!wiggleOpenFile()) {
        lv_label_set_text(wiggleStatusLbl,
            LV_SYMBOL_CLOSE "  Failed to create file!");
        lv_obj_set_style_text_color(wiggleStatusLbl,
            lv_color_hex(0xf85149), LV_PART_MAIN);
        return;
    }

    wiggleRunning   = true;
    wiggleScanCount = 0;
    wiggleNetCount  = 0;

    // Show filename (strip leading /)
    char fnBuf[42];
    snprintf(fnBuf, sizeof(fnBuf), "%s", wiggleFilename + 1);
    lv_label_set_text(wiggleFileLbl, fnBuf);

    // Kick off first async scan
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.scanNetworks(true);

    // Enable / disable buttons
    lv_obj_add_state(wiggleStartBtn, LV_STATE_DISABLED);
    lv_obj_remove_state(wiggleStopBtn, LV_STATE_DISABLED);

    // Start refresh timer (every 15 s — comfortable scan interval)
    if (wiggleTimer) { lv_timer_delete(wiggleTimer); wiggleTimer = nullptr; }
    wiggleTimer = lv_timer_create(wiggle_refresh_cb, WIGGLE_SCAN_INTERVAL_MS, nullptr);
    // Fire once immediately so status updates right away
    lv_timer_ready(wiggleTimer);
}

static void cb_wiggleStop(lv_event_t *e) {
    if (!wiggleRunning) return;
    wiggleRunning = false;

    WiFi.scanDelete();
    SD.end();

    if (wiggleTimer) { lv_timer_delete(wiggleTimer); wiggleTimer = nullptr; }

    lv_label_set_text(wiggleStatusLbl,
        LV_SYMBOL_OK "  Stopped — file saved");
    lv_obj_set_style_text_color(wiggleStatusLbl,
        lv_color_hex(0x3fb950), LV_PART_MAIN);

    lv_obj_remove_state(wiggleStartBtn, LV_STATE_DISABLED);
    lv_obj_add_state(wiggleStopBtn, LV_STATE_DISABLED);
}

void createWiggleWars() {
    // Reset state
    wiggleRunning   = false;
    wiggleScanCount = 0;
    wiggleNetCount  = 0;
    wiggleStatusLbl = nullptr;
    wiggleScanLbl   = nullptr;
    wiggleNetLbl    = nullptr;
    wiggleFileLbl   = nullptr;
    wiggleStartBtn  = nullptr;
    wiggleStopBtn   = nullptr;

    if (gpsToolScreen) { lv_obj_delete(gpsToolScreen); gpsToolScreen = nullptr; }
    gpsToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(gpsToolScreen);
    createHeader(gpsToolScreen, LV_SYMBOL_SAVE "  Wiggle Wars");

    // Status label
    wiggleStatusLbl = lv_label_create(gpsToolScreen);
    lv_label_set_text(wiggleStatusLbl,
        LV_SYMBOL_GPS "  Press Start to begin");
    lv_obj_set_style_text_color(wiggleStatusLbl,
        lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(wiggleStatusLbl, 8, 30);

    // Stats card
    lv_obj_t *card = lv_obj_create(gpsToolScreen);
    lv_obj_set_size(card, SCREEN_W - 12, 56);
    lv_obj_set_pos(card, 6, 50);
    lv_obj_set_style_bg_color(card,     lv_color_hex(0x161b22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1,                      LV_PART_MAIN);
    lv_obj_set_style_radius(card,       6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,      5,                      LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    wiggleScanLbl = lv_label_create(card);
    lv_label_set_text(wiggleScanLbl, "Scans: 0");
    lv_obj_set_style_text_color(wiggleScanLbl,
        lv_color_hex(0x58a6ff), LV_PART_MAIN);
    lv_obj_set_pos(wiggleScanLbl, 0, 0);

    wiggleNetLbl = lv_label_create(card);
    lv_label_set_text(wiggleNetLbl, "Nets logged: 0");
    lv_obj_set_style_text_color(wiggleNetLbl,
        lv_color_hex(0x3fb950), LV_PART_MAIN);
    lv_obj_set_pos(wiggleNetLbl, 0, 18);

    wiggleFileLbl = lv_label_create(card);
    lv_label_set_text(wiggleFileLbl, "File: none");
    lv_obj_set_style_text_color(wiggleFileLbl,
        lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(wiggleFileLbl, 0, 36);

    // Start / Stop buttons alongside Back
    lv_obj_t *backBtn = createBackBtn(gpsToolScreen, cb_gpsToolBack);

    // Start button
    wiggleStartBtn = lv_btn_create(gpsToolScreen);
    lv_obj_set_size(wiggleStartBtn, 70, 28);
    lv_obj_align(wiggleStartBtn, LV_ALIGN_BOTTOM_MID, -12, -4);
    lv_obj_set_style_bg_color(wiggleStartBtn,
        lv_color_hex(0x238636), LV_PART_MAIN);
    lv_obj_set_style_radius(wiggleStartBtn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(wiggleStartBtn, cb_wiggleStart, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *startLbl = lv_label_create(wiggleStartBtn);
    lv_label_set_text(startLbl, LV_SYMBOL_PLAY "  Start");
    lv_obj_center(startLbl);

    // Stop button
    wiggleStopBtn = lv_btn_create(gpsToolScreen);
    lv_obj_set_size(wiggleStopBtn, 70, 28);
    lv_obj_align(wiggleStopBtn, LV_ALIGN_BOTTOM_MID, 62, -4);
    lv_obj_set_style_bg_color(wiggleStopBtn,
        lv_color_hex(0xb62324), LV_PART_MAIN);
    lv_obj_set_style_radius(wiggleStopBtn, 6, LV_PART_MAIN);
    lv_obj_add_state(wiggleStopBtn, LV_STATE_DISABLED);
    lv_obj_add_event_cb(wiggleStopBtn, cb_wiggleStop, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *stopLbl = lv_label_create(wiggleStopBtn);
    lv_label_set_text(stopLbl, LV_SYMBOL_STOP "  Stop");
    lv_obj_center(stopLbl);

    deleteGroup(&gpsToolGroup);
    gpsToolGroup = lv_group_create();
    lv_group_add_obj(gpsToolGroup, backBtn);
    lv_group_add_obj(gpsToolGroup, wiggleStartBtn);
    lv_group_add_obj(gpsToolGroup, wiggleStopBtn);
    setGroup(gpsToolGroup);

    setAllLEDs(MENU_COLORS[3].r, MENU_COLORS[3].g, MENU_COLORS[3].b, LED_BRIGHTNESS);

    lv_screen_load_anim(gpsToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  WIFI MENU
// ════════════════════════════════════════════════════════════════

static const char *WIFI_TOOL_LABELS[6] = {
    LV_SYMBOL_WIFI     "  Network Scanner",
    LV_SYMBOL_WARNING  "  Deauth Detector",
    LV_SYMBOL_LOOP     "  Channel Analyzer",
    LV_SYMBOL_EYE_OPEN "  PineAP Hunter",
    LV_SYMBOL_EYE_OPEN "  Pwnagotchi Watch",
    LV_SYMBOL_WARNING  "  Flock Detector"
};

static void cb_wifiMenuBack(lv_event_t *e) {
    lv_screen_load_anim(mainScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
    deleteGroup(&wifiMenuGroup);
    setGroup(navGroup);
    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
}

static void cb_wifiToolSelected(lv_event_t *e) {
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    switch (t) {
        case 0: createNetworkScanner();      break;
        case 1: createDeauthDetector();      break;
        case 2: createChannelAnalyzer();     break;
        case 3: createPineAPHunter();        break;
        case 4: createPwnagotchiDetector(); break;
        case 5: createFlockDetector();      break;
    }
}

void createWiFiMenu() {
    if (wifiMenuScreen) { lv_obj_delete(wifiMenuScreen); wifiMenuScreen = nullptr; }
    wifiMenuScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiMenuScreen);
    createHeader(wifiMenuScreen, LV_SYMBOL_WIFI "  WiFi Tools");

    lv_obj_t *list = lv_list_create(wifiMenuScreen);
    lv_obj_set_size(list, SCREEN_W, SCREEN_H - 28 - 34);
    lv_obj_set_pos(list, 0, 28);
    lv_obj_set_style_bg_color(list,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list,      6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list,      4, LV_PART_MAIN);

    deleteGroup(&wifiMenuGroup);
    wifiMenuGroup = lv_group_create();

    for (int i = 0; i < 6; i++) {
        lv_obj_t *btn = lv_list_add_btn(list, nullptr, WIFI_TOOL_LABELS[i]);
        styleListBtn(btn);
        lv_obj_set_height(btn, 30);
        lv_obj_add_event_cb(btn, cb_wifiToolSelected, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_group_add_obj(wifiMenuGroup, btn);
    }

    lv_obj_t *backBtn = createBackBtn(wifiMenuScreen, cb_wifiMenuBack);
    lv_group_add_obj(wifiMenuGroup, backBtn);
    setGroup(wifiMenuGroup);

    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, 3);
    lv_screen_load_anim(wifiMenuScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  WIFI UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════

static const char *authModeStr(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN:          return "Open";
        case WIFI_AUTH_WEP:           return "WEP";
        case WIFI_AUTH_WPA_PSK:       return "WPA";
        case WIFI_AUTH_WPA2_PSK:      return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/2";
        case WIFI_AUTH_WPA3_PSK:      return "WPA3";
        default:                      return "Secured";
    }
}

static lv_color_t rssiColor(int8_t rssi) {
    if (rssi >= -55) return lv_color_hex(0x3fb950);
    if (rssi >= -70) return lv_color_hex(0xe3b341);
    return              lv_color_hex(0xf85149);
}

static const char *rssiQuality(int8_t rssi) {
    if (rssi >= -55) return "Excellent";
    if (rssi >= -65) return "Good";
    if (rssi >= -75) return "Fair";
    return                  "Weak";
}

static void sortByRSSI() {
    for (int i = 0; i < wifiEntryCount - 1; i++)
        for (int j = 0; j < wifiEntryCount - 1 - i; j++)
            if (wifiEntries[j].rssi < wifiEntries[j+1].rssi) {
                WiFiEntry tmp    = wifiEntries[j];
                wifiEntries[j]   = wifiEntries[j+1];
                wifiEntries[j+1] = tmp;
            }
}

static int doWiFiScan() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks(false, true);
    if (n < 0) n = 0;
    if (n > MAX_WIFI_RESULTS) n = MAX_WIFI_RESULTS;
    wifiEntryCount = n;

    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        if (s.length() == 0) s = "<hidden>";
        strncpy(wifiEntries[i].ssid, s.c_str(), 32);
        wifiEntries[i].ssid[32] = '\0';

        String mac = WiFi.BSSIDstr(i);
        strncpy(wifiEntries[i].bssid, mac.c_str(), 17);
        wifiEntries[i].bssid[17] = '\0';

        wifiEntries[i].rssi    = (int8_t)WiFi.RSSI(i);
        wifiEntries[i].channel = (uint8_t)WiFi.channel(i);
        wifiEntries[i].open    = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        strncpy(wifiEntries[i].authStr,
                authModeStr(WiFi.encryptionType(i)), 9);
        wifiEntries[i].authStr[9] = '\0';
    }
    WiFi.scanDelete();
    sortByRSSI();
    return n;
}

// ════════════════════════════════════════════════════════════════
//  SHARED BACK CALLBACKS
// ════════════════════════════════════════════════════════════════

static void cb_wifiToolBack(lv_event_t *e) {
    if (deauthActive) {
        deauthActive = false;
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);
    }
    if (pwnActive) {
        pwnActive = false;
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);
    }
    if (flockActive) {
        flockActive = false;
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);
    }
    if (deauthTimer) { lv_timer_delete(deauthTimer); deauthTimer = nullptr; }
    if (pwnTimer)    { lv_timer_delete(pwnTimer);    pwnTimer    = nullptr; }
    if (flockTimer)  { lv_timer_delete(flockTimer);  flockTimer  = nullptr; }
    // Do NOT lv_obj_delete the outgoing screen — auto_del=true lets LVGL free it
    // after the animation completes.  Deleting before load_anim = use-after-free crash.
    wifiToolScreen = nullptr;
    deleteGroup(&wifiToolGroup);
    setGroup(wifiMenuGroup);
    lv_screen_load_anim(wifiMenuScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, 3);
}

static void cb_wifiDetailBack(lv_event_t *e) {
    wifiDetailScreen = nullptr;
    deleteGroup(&wifiDetailGroup);
    setGroup(wifiToolGroup);
    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
}

// ════════════════════════════════════════════════════════════════
//  TOOL 1 – NETWORK SCANNER
// ════════════════════════════════════════════════════════════════

static lv_obj_t *scanList      = nullptr;
static lv_obj_t *scanStatusLbl = nullptr;
static lv_obj_t *scanBackBtn   = nullptr;   // saved so rebuildScanList can rebuild the group
static lv_obj_t *scanScanBtn   = nullptr;

static void rebuildScanList() {
    // Rebuild group first — lv_obj_clean will invalidate the old list buttons
    // that are still referenced inside wifiToolGroup, causing a crash on back.
    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    if (scanBackBtn) lv_group_add_obj(wifiToolGroup, scanBackBtn);
    if (scanScanBtn) lv_group_add_obj(wifiToolGroup, scanScanBtn);
    setGroup(wifiToolGroup);

    lv_obj_clean(scanList);
    for (int i = 0; i < wifiEntryCount; i++) {
        char row[56];
        const char *lock = wifiEntries[i].open ? " " : LV_SYMBOL_CLOSE;
        char ssidTrunc[17];
        strncpy(ssidTrunc, wifiEntries[i].ssid, 16);
        ssidTrunc[16] = '\0';
        snprintf(row, sizeof(row), "%s %-16s Ch%-2d %ddBm",
                 lock, ssidTrunc, wifiEntries[i].channel, wifiEntries[i].rssi);

        lv_obj_t *btn = lv_list_add_btn(scanList, nullptr, row);
        styleListBtn(btn);
        lv_obj_set_style_text_color(btn, rssiColor(wifiEntries[i].rssi),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            createNetworkDetail((int)(intptr_t)lv_event_get_user_data(ev));
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(wifiToolGroup, btn);
    }
}

static void cb_doScan(lv_event_t *e) {
    lv_label_set_text(scanStatusLbl, LV_SYMBOL_REFRESH "  Scanning...");
    lv_obj_set_style_text_color(scanStatusLbl, lv_color_hex(0xe3b341), LV_PART_MAIN);
    lv_timer_handler();

    // Green spinner while scan blocks core 1
    startLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
    int found = doWiFiScan();
    stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);

    char buf[40];
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  %d network%s found",
             found, found == 1 ? "" : "s");
    lv_label_set_text(scanStatusLbl, buf);
    lv_obj_set_style_text_color(scanStatusLbl,
        found > 0 ? lv_color_hex(0x3fb950) : lv_color_hex(0x8b949e),
        LV_PART_MAIN);

    rebuildScanList();
}

void createNetworkScanner() {
    if (wifiToolScreen) { lv_obj_delete(wifiToolScreen); wifiToolScreen = nullptr; }
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_WIFI "  Network Scanner");

    scanStatusLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(scanStatusLbl, "Press Scan to start");
    lv_obj_set_style_text_color(scanStatusLbl, lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(scanStatusLbl, 8, 30);

    // List: header(28) + status(18) + bottom bar(34) = 80 used; rest for list
    scanList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(scanList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(scanList, 0, 48);
    lv_obj_set_style_bg_color(scanList,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scanList,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(scanList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scanList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scanList,      2, LV_PART_MAIN);

    scanBackBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    scanScanBtn = createActionBtn(wifiToolScreen,
                                        LV_SYMBOL_REFRESH "  Scan", cb_doScan);

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, scanBackBtn);
    lv_group_add_obj(wifiToolGroup, scanScanBtn);
    setGroup(wifiToolGroup);

    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  NETWORK DETAIL SCREEN
// ════════════════════════════════════════════════════════════════

void createNetworkDetail(int idx) {
    if (wifiDetailScreen) { lv_obj_delete(wifiDetailScreen); wifiDetailScreen = nullptr; }
    wifiDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiDetailScreen);

    char hdr[48];
    snprintf(hdr, sizeof(hdr), LV_SYMBOL_WIFI "  %.30s", wifiEntries[idx].ssid);
    createHeader(wifiDetailScreen, hdr);

    // Info card
    lv_obj_t *card = lv_obj_create(wifiDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,      lv_color_hex(0x161b22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,        LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card,  lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(card,  1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,        6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,       6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    int8_t rssi = wifiEntries[idx].rssi;
    char info[220];
    snprintf(info, sizeof(info),
             "SSID     : %s\n"
             "BSSID    : %s\n"
             "Channel  : %d\n"
             "Security : %s\n"
             "RSSI     : %d dBm  (%s)",
             wifiEntries[idx].ssid,
             wifiEntries[idx].bssid,
             wifiEntries[idx].channel,
             wifiEntries[idx].authStr,
             rssi, rssiQuality(rssi));

    lv_obj_t *infoLbl = lv_label_create(card);
    lv_label_set_text(infoLbl, info);
    lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(infoLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(0xe6edf3), LV_PART_MAIN);
    lv_obj_align(infoLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    // RSSI bar (requires LV_USE_BAR 1 in lv_conf.h)
    lv_obj_t *bar = lv_bar_create(wifiDetailScreen);
    lv_obj_set_size(bar, SCREEN_W - 12, 5);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(bar, -100, -30);
    lv_bar_set_value(bar, rssi, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x21262d), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, rssiColor(rssi),        LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar,   3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar,   3, LV_PART_INDICATOR);

    lv_obj_t *backBtn = createBackBtn(wifiDetailScreen, cb_wifiDetailBack);

    deleteGroup(&wifiDetailGroup);
    wifiDetailGroup = lv_group_create();
    lv_group_add_obj(wifiDetailGroup, backBtn);
    setGroup(wifiDetailGroup);

    lv_screen_load_anim(wifiDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  TOOL 2 – DEAUTH DETECTOR
//
//  Uses ESP32 promiscuous mode. 802.11 frame control byte 0:
//    0xC0 = Deauthentication  (Management, subtype 12)
//    0xA0 = Disassociation    (Management, subtype 10)
//  Channel hops 1-13 every 200 ms in loop() below.
// ════════════════════════════════════════════════════════════════

typedef struct {
    uint8_t  frameCtrl[2];
    uint16_t duration;
    uint8_t  dst[6];
    uint8_t  src[6];
    uint8_t  bssid[6];
    uint16_t seqCtrl;
} __attribute__((packed)) Dot11MgmtHdr;

static lv_obj_t *deauthCountLbl  = nullptr;
static lv_obj_t *deauthEventList = nullptr;

static void IRAM_ATTR sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!deauthActive || type != WIFI_PKT_MGMT) return;

    const wifi_promiscuous_pkt_t *pkt =
        reinterpret_cast<const wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *data = pkt->payload;

    if (data[0] != 0xC0 && data[0] != 0xA0) return;
    if (pkt->rx_ctrl.sig_len < (int)sizeof(Dot11MgmtHdr)) return;

    if (deauthTotal < 9999) deauthTotal++;

    const Dot11MgmtHdr *hdr = reinterpret_cast<const Dot11MgmtHdr *>(data);
    int slot = deauthHead % MAX_DEAUTH;

    snprintf(deauthLog[slot].src, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             hdr->src[0],  hdr->src[1],  hdr->src[2],
             hdr->src[3],  hdr->src[4],  hdr->src[5]);
    snprintf(deauthLog[slot].dst, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             hdr->dst[0],  hdr->dst[1],  hdr->dst[2],
             hdr->dst[3],  hdr->dst[4],  hdr->dst[5]);
    deauthLog[slot].channel = pkt->rx_ctrl.channel;
    deauthLog[slot].ms      = (uint32_t)millis();
    deauthLog[slot].reason  = (pkt->rx_ctrl.sig_len >= 26)
                              ? (uint16_t)(data[24] | (data[25] << 8))
                              : 0;
    deauthHead = (deauthHead + 1) % MAX_DEAUTH;
}

static void deauth_refresh_cb(lv_timer_t *) {
    if (!deauthCountLbl || !deauthEventList) return;

    char buf[56];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WARNING "  Deauth frames: %d", deauthTotal);
    lv_label_set_text(deauthCountLbl, buf);
    lv_obj_set_style_text_color(deauthCountLbl,
        deauthTotal > 0 ? lv_color_hex(0xf85149) : lv_color_hex(0x3fb950),
        LV_PART_MAIN);

    lv_obj_clean(deauthEventList);
    int total = (deauthTotal < MAX_DEAUTH) ? deauthTotal : MAX_DEAUTH;

    if (total == 0) {
        lv_obj_t *e = lv_list_add_text(deauthEventList, "No frames detected yet...");
        if (e) lv_obj_set_style_text_color(e, lv_color_hex(0x8b949e), LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < total; i++) {
        int slot = ((deauthHead - 1 - i) + MAX_DEAUTH) % MAX_DEAUTH;
        char row[60];
        snprintf(row, sizeof(row), "Ch%d  %s  [R:%d]",
                 deauthLog[slot].channel,
                 deauthLog[slot].src,
                 deauthLog[slot].reason);
        lv_obj_t *entry = lv_list_add_text(deauthEventList, row);
        if (entry)
            lv_obj_set_style_text_color(entry, lv_color_hex(0xf85149), LV_PART_MAIN);
    }
}

void createDeauthDetector() {
    if (wifiToolScreen) { lv_obj_delete(wifiToolScreen); wifiToolScreen = nullptr; }
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_WARNING "  Deauth Detector");

    deauthCountLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(deauthCountLbl,
                      LV_SYMBOL_WARNING "  Deauth frames: 0");
    lv_obj_set_style_text_color(deauthCountLbl, lv_color_hex(0x3fb950), LV_PART_MAIN);
    lv_obj_set_pos(deauthCountLbl, 8, 30);

    lv_obj_t *hopLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(hopLbl, "Hopping ch 1-13");
    lv_obj_set_style_text_color(hopLbl, lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_align(hopLbl, LV_ALIGN_TOP_RIGHT, -8, 30);

    deauthEventList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(deauthEventList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(deauthEventList, 0, 48);
    lv_obj_set_style_bg_color(deauthEventList, lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(deauthEventList,   LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(deauthEventList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(deauthEventList,  2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(deauthEventList,  1, LV_PART_MAIN);

    lv_obj_t *initLbl =
        lv_list_add_text(deauthEventList, "Monitoring... (no events yet)");
    if (initLbl)
        lv_obj_set_style_text_color(initLbl, lv_color_hex(0x8b949e), LV_PART_MAIN);

    lv_obj_t *backBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, backBtn);
    setGroup(wifiToolGroup);

    // Start sniffer
    deauthTotal  = 0;
    deauthHead   = 0;
    deauthActive = true;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(sniffer_cb);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    deauthChannel = 1;

    if (deauthTimer) { lv_timer_delete(deauthTimer); deauthTimer = nullptr; }
    deauthTimer = lv_timer_create(deauth_refresh_cb, 1000, nullptr);

    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  TOOL 3 – CHANNEL ANALYZER
// ════════════════════════════════════════════════════════════════

static lv_obj_t *chanStatusLbl = nullptr;
static lv_obj_t *chanChartArea = nullptr;

static void buildChannelBars() {
    if (!chanChartArea) return;
    lv_obj_clean(chanChartArea);

    int maxCount = 1;
    for (int ch = 1; ch <= 13; ch++)
        if (chanNetCount[ch] > maxCount) maxCount = chanNetCount[ch];

    // Drawing area inside the panel (accounting for pad_all = 4)
    const int areaW   = SCREEN_W - 12 - 8;
    const int areaH   = SCREEN_H - 80 - 8;
    const int barW    = (areaW / 13) - 2;
    const int maxBarH = areaH - 14;

    for (int ch = 1; ch <= 13; ch++) {
        int x    = (ch - 1) * (barW + 2);
        int barH = (chanNetCount[ch] == 0)
                   ? 2
                   : max(2, (int)((float)chanNetCount[ch] / maxCount * maxBarH));
        int y    = maxBarH - barH;

        lv_color_t col;
        if      (chanNetCount[ch] == 0) col = lv_color_hex(0x21262d);
        else if (chanNetCount[ch] <= 2) col = lv_color_hex(0x3fb950);
        else if (chanNetCount[ch] <= 4) col = lv_color_hex(0xe3b341);
        else                            col = lv_color_hex(0xf85149);

        lv_obj_t *bar = lv_obj_create(chanChartArea);
        lv_obj_set_size(bar, barW, barH);
        lv_obj_set_pos(bar, x, y);
        lv_obj_set_style_bg_color(bar, col, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar,   LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        // Channel label below bar
        lv_obj_t *chLbl = lv_label_create(chanChartArea);
        char cb[4];
        snprintf(cb, sizeof(cb), "%d", ch);
        lv_label_set_text(chLbl, cb);
        lv_obj_set_style_text_color(chLbl, lv_color_hex(0x8b949e), LV_PART_MAIN);
        lv_obj_set_pos(chLbl, x, maxBarH + 2);

        // Count label above bar
        if (chanNetCount[ch] > 0) {
            lv_obj_t *cLbl = lv_label_create(chanChartArea);
            char cnt[4];
            snprintf(cnt, sizeof(cnt), "%d", chanNetCount[ch]);
            lv_label_set_text(cLbl, cnt);
            lv_obj_set_style_text_color(cLbl, lv_color_hex(0xe6edf3), LV_PART_MAIN);
            lv_obj_set_pos(cLbl, x, y > 12 ? y - 12 : 0);
        }
    }
}

static void cb_doChannelScan(lv_event_t *e) {
    lv_label_set_text(chanStatusLbl, LV_SYMBOL_REFRESH "  Scanning channels...");
    lv_obj_set_style_text_color(chanStatusLbl, lv_color_hex(0xe3b341), LV_PART_MAIN);
    if (chanChartArea) lv_obj_clean(chanChartArea);
    lv_timer_handler();

    for (int i = 0; i < 14; i++) { chanNetCount[i] = 0; chanMaxRSSI[i] = -100; }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    // Green spinner while scan blocks core 1
    startLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
    int n = WiFi.scanNetworks(false, true);
    stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
    if (n < 0) n = 0;

    for (int i = 0; i < n; i++) {
        int ch = WiFi.channel(i);
        if (ch >= 1 && ch <= 13) {
            chanNetCount[ch]++;
            int8_t r = (int8_t)WiFi.RSSI(i);
            if (r > chanMaxRSSI[ch]) chanMaxRSSI[ch] = r;
        }
    }
    WiFi.scanDelete();

    // Count how many channels are occupied
    int occupied = 0;
    for (int ch = 1; ch <= 13; ch++) if (chanNetCount[ch] > 0) occupied++;

    char buf[56];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WIFI "  %d nets on %d channel%s",
             n, occupied, occupied == 1 ? "" : "s");
    lv_label_set_text(chanStatusLbl, buf);
    lv_obj_set_style_text_color(chanStatusLbl,
        n > 0 ? lv_color_hex(0x3fb950) : lv_color_hex(0x8b949e),
        LV_PART_MAIN);

    buildChannelBars();
}

void createChannelAnalyzer() {
    if (wifiToolScreen) { lv_obj_delete(wifiToolScreen); wifiToolScreen = nullptr; }
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_LOOP "  Channel Analyzer");

    chanStatusLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(chanStatusLbl, "Press Scan to analyze channels 1-13");
    lv_obj_set_style_text_color(chanStatusLbl, lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(chanStatusLbl, 8, 30);

    chanChartArea = lv_obj_create(wifiToolScreen);
    lv_obj_set_size(chanChartArea, SCREEN_W - 12, SCREEN_H - 80);
    lv_obj_set_pos(chanChartArea, 6, 48);
    lv_obj_set_style_bg_color(chanChartArea, lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chanChartArea,   LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(chanChartArea, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(chanChartArea, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(chanChartArea, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chanChartArea, 4, LV_PART_MAIN);
    lv_obj_clear_flag(chanChartArea, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ph = lv_label_create(chanChartArea);
    lv_label_set_text(ph, "Channels 1-13");
    lv_obj_set_style_text_color(ph, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_align(ph, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *backBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    lv_obj_t *scanBtn = createActionBtn(wifiToolScreen,
                                        LV_SYMBOL_REFRESH "  Scan",
                                        cb_doChannelScan);

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, backBtn);
    lv_group_add_obj(wifiToolGroup, scanBtn);
    setGroup(wifiToolGroup);

    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}


// ════════════════════════════════════════════════════════════════════════════════════════════════
//  TOOL 4 – PINEAP HUNTER  Credit: https://github.com/n0xa
//
//  Detects rogue WiFi Pineapple / KARMA attacks by tracking how many
//  unique SSIDs each BSSID advertises across repeated scans.
//
//  Each WiFi scan returns at most one SSID per BSSID. A legitimate AP
//  will always return the same SSID. A Pineapple running PineAP/KARMA
//  will respond to probe requests by beaconing whatever SSID was probed,
//  so successive scans reveal a new SSID each time for the same BSSID.
//
//  Algorithm:
//    1. Scan and collect BSSID + SSID pairs from every result.
//    2. For each BSSID, check if this SSID has been seen before.
//    3. If new, increment that BSSID's unique-SSID counter and store it
//       (up to PINEAP_SSID_SLOTS entries for display).
//    4. After the scan, count how many BSSIDs have exceeded
//       PINEAP_THRESHOLD unique SSIDs — those are the suspects.
//    5. Display flagged BSSIDs in red; click for SSID list detail.
//
//  Data persists for the lifetime of the tool screen so the list grows
//  with each successive scan. Navigating back clears all state.
// ════════════════════════════════════════════════════════════════════════════════════════════════

static lv_obj_t *pineapStatusLbl = nullptr;
static lv_obj_t *pineapList      = nullptr;
static lv_obj_t *pineapBackBtn   = nullptr;   // saved so rebuildPineAPList can rebuild the group
static lv_obj_t *pineapScanBtn   = nullptr;

// Sort pineapEntries by ssidCount descending
static void sortPineAPBySsidCount() {
    for (int i = 0; i < pineapEntryCount - 1; i++)
        for (int j = 0; j < pineapEntryCount - 1 - i; j++)
            if (pineapEntries[j].ssidCount < pineapEntries[j+1].ssidCount) {
                PineAPEntry tmp    = pineapEntries[j];
                pineapEntries[j]   = pineapEntries[j+1];
                pineapEntries[j+1] = tmp;
            }
}

// Accumulate one WiFi scan into the BSSID table
static void doPineAPScan() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int n = WiFi.scanNetworks(false, true);
    if (n < 0) n = 0;
    pineapScanCount++;

    for (int i = 0; i < n; i++) {
        String bssidStr = WiFi.BSSIDstr(i);
        String ssidStr  = WiFi.SSID(i);
        if (ssidStr.length() == 0) ssidStr = "<hidden>";
        int8_t rssi = (int8_t)WiFi.RSSI(i);

        // Find or create BSSID slot
        int slot = -1;
        for (int j = 0; j < pineapEntryCount; j++) {
            if (bssidStr.equalsIgnoreCase(pineapEntries[j].bssid)) {
                slot = j; break;
            }
        }
        if (slot == -1) {
            if (pineapEntryCount >= MAX_PINEAP_BSSIDS) continue;
            slot = pineapEntryCount++;
            strncpy(pineapEntries[slot].bssid, bssidStr.c_str(), 17);
            pineapEntries[slot].bssid[17] = '\0';
            pineapEntries[slot].ssidCount = 0;
        }
        pineapEntries[slot].lastRSSI = rssi;

        // Check if this SSID is already stored for this BSSID
        int   checkLen = min(pineapEntries[slot].ssidCount, PINEAP_SSID_SLOTS);
        bool  known    = false;
        for (int k = 0; k < checkLen; k++) {
            if (ssidStr.equals(pineapEntries[slot].ssids[k])) { known = true; break; }
        }
        if (!known) {
            if (pineapEntries[slot].ssidCount < PINEAP_SSID_SLOTS) {
                strncpy(pineapEntries[slot].ssids[pineapEntries[slot].ssidCount],
                        ssidStr.c_str(), 32);
                pineapEntries[slot].ssids[pineapEntries[slot].ssidCount][32] = '\0';
            }
            pineapEntries[slot].ssidCount++;
        }
    }
    WiFi.scanDelete();

    // Re-count flagged BSSIDs and sort
    pineapFlagged = 0;
    for (int j = 0; j < pineapEntryCount; j++)
        if (pineapEntries[j].ssidCount >= PINEAP_THRESHOLD) pineapFlagged++;
    sortPineAPBySsidCount();
}

// Rebuild the BSSID result list on screen
static void rebuildPineAPList() {
    if (!pineapList) return;

    // Rebuild the group BEFORE lv_obj_clean so the old list-button
    // references are dropped first.  Stale group pointers are the crash.
    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    if (pineapBackBtn) lv_group_add_obj(wifiToolGroup, pineapBackBtn);
    if (pineapScanBtn) lv_group_add_obj(wifiToolGroup, pineapScanBtn);
    setGroup(wifiToolGroup);

    lv_obj_clean(pineapList);

    if (pineapEntryCount == 0) {
        lv_obj_t *e = lv_list_add_text(pineapList, "No BSSIDs seen yet — press Scan");
        if (e) lv_obj_set_style_text_color(e, lv_color_hex(0x8b949e), LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < pineapEntryCount; i++) {
        bool flagged = (pineapEntries[i].ssidCount >= PINEAP_THRESHOLD);

        // Row: BSSID  Nx  -XXdBm
        char row[56];
        snprintf(row, sizeof(row), "%s  %dx  %ddBm",
                 pineapEntries[i].bssid,
                 pineapEntries[i].ssidCount,
                 pineapEntries[i].lastRSSI);

        lv_obj_t *btn = lv_list_add_btn(pineapList, nullptr, row);
        styleListBtn(btn);

        // Flagged = red (suspect Pineapple), normal = dim grey
        lv_color_t col = flagged ? lv_color_hex(0xf85149)
                                 : lv_color_hex(0x8b949e);
        lv_obj_set_style_text_color(btn, col, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            createPineAPDetail((int)(intptr_t)lv_event_get_user_data(ev));
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(wifiToolGroup, btn);
    }
}

// Scan button callback
static void cb_doPineAPScan(lv_event_t *e) {
    char buf[60];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_REFRESH "  Scanning...  (pass %d)", pineapScanCount + 1);
    lv_label_set_text(pineapStatusLbl, buf);
    lv_obj_set_style_text_color(pineapStatusLbl, lv_color_hex(0xe3b341), LV_PART_MAIN);
    lv_timer_handler();

    // Amber spinner — distinct from generic green WiFi scans
    startLEDSpinner(220, 140, 0);
    doPineAPScan();
    stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);

    if (pineapFlagged > 0) {
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_WARNING "  %d suspect AP%s!  (%d scans)",
                 pineapFlagged, pineapFlagged == 1 ? "" : "s", pineapScanCount);
        lv_obj_set_style_text_color(pineapStatusLbl, lv_color_hex(0xf85149), LV_PART_MAIN);
    } else {
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_WIFI "  %d BSSID%s tracked  (%d scans)",
                 pineapEntryCount, pineapEntryCount == 1 ? "" : "s", pineapScanCount);
        lv_obj_set_style_text_color(pineapStatusLbl,
            pineapEntryCount > 0 ? lv_color_hex(0x3fb950) : lv_color_hex(0x8b949e),
            LV_PART_MAIN);
    }
    lv_label_set_text(pineapStatusLbl, buf);
    rebuildPineAPList();
}

// Main PineAP Hunter screen
void createPineAPHunter() {
    // Reset all state fresh each time the tool is opened
    memset(pineapEntries, 0, sizeof(pineapEntries));
    pineapEntryCount = 0;
    pineapScanCount  = 0;
    pineapFlagged    = 0;
    pineapStatusLbl  = nullptr;
    pineapList       = nullptr;
    pineapBackBtn    = nullptr;
    pineapScanBtn    = nullptr;

    if (wifiToolScreen) { lv_obj_delete(wifiToolScreen); wifiToolScreen = nullptr; }
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_EYE_OPEN "  PineAP Hunter");

    pineapStatusLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(pineapStatusLbl,
        "Scan repeatedly — flags BSSIDs\n"
        "with " LV_SYMBOL_WARNING " 5+ unique SSIDs (KARMA/Pineapple)");
    lv_obj_set_style_text_color(pineapStatusLbl, lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(pineapStatusLbl, 8, 30);

    pineapList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(pineapList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(pineapList, 0, 48);
    lv_obj_set_style_bg_color(pineapList,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pineapList,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(pineapList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pineapList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(pineapList,      2, LV_PART_MAIN);

    pineapBackBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    pineapScanBtn = createActionBtn(wifiToolScreen,
                                        LV_SYMBOL_REFRESH "  Scan",
                                        cb_doPineAPScan);

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, pineapBackBtn);
    lv_group_add_obj(wifiToolGroup, pineapScanBtn);
    setGroup(wifiToolGroup);

    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// PineAP BSSID Detail Screen
//  Reuses wifiDetailScreen/wifiDetailGroup so cb_wifiDetailBack
//  automatically returns to the PineAP list.
void createPineAPDetail(int idx) {
    if (wifiDetailScreen) { lv_obj_delete(wifiDetailScreen); wifiDetailScreen = nullptr; }
    wifiDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiDetailScreen);

    bool flagged = (pineapEntries[idx].ssidCount >= PINEAP_THRESHOLD);
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "%s  %.17s",
             flagged ? LV_SYMBOL_WARNING : LV_SYMBOL_WIFI,
             pineapEntries[idx].bssid);
    createHeader(wifiDetailScreen, hdr);

    // Info card
    lv_obj_t *card = lv_obj_create(wifiDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,     lv_color_hex(0x161b22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,       6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,      6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Badge
    lv_obj_t *badgeLbl = lv_label_create(card);
    if (flagged) {
        lv_label_set_text(badgeLbl, LV_SYMBOL_WARNING "  SUSPECTED PINEAPPLE / KARMA");
        lv_obj_set_style_text_color(badgeLbl, lv_color_hex(0xf85149), LV_PART_MAIN);
    } else {
        lv_label_set_text(badgeLbl, LV_SYMBOL_WIFI "  Normal AP (below threshold)");
        lv_obj_set_style_text_color(badgeLbl, lv_color_hex(0x8b949e), LV_PART_MAIN);
    }
    lv_obj_align(badgeLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    // Stats
    char stats[80];
    snprintf(stats, sizeof(stats),
             "BSSID : %s\n"
             "SSIDs : %d unique  (alert >= %d)\n"
             "RSSI  : %d dBm",
             pineapEntries[idx].bssid,
             pineapEntries[idx].ssidCount,
             PINEAP_THRESHOLD,
             pineapEntries[idx].lastRSSI);

    lv_obj_t *statsLbl = lv_label_create(card);
    lv_label_set_text(statsLbl, stats);
    lv_label_set_long_mode(statsLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(statsLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(statsLbl, lv_color_hex(0xe6edf3), LV_PART_MAIN);
    lv_obj_align(statsLbl, LV_ALIGN_TOP_LEFT, 0, 16);

    // Collected SSIDs
    int stored = min(pineapEntries[idx].ssidCount, PINEAP_SSID_SLOTS);
    char ssidBlock[PINEAP_SSID_SLOTS * 36 + 20] = "SSIDs seen:\n";
    for (int k = 0; k < stored; k++) {
        strncat(ssidBlock, "  ", sizeof(ssidBlock) - strlen(ssidBlock) - 1);
        strncat(ssidBlock, pineapEntries[idx].ssids[k],
                sizeof(ssidBlock) - strlen(ssidBlock) - 1);
        strncat(ssidBlock, "\n", sizeof(ssidBlock) - strlen(ssidBlock) - 1);
    }
    if (pineapEntries[idx].ssidCount > PINEAP_SSID_SLOTS) {
        char extra[24];
        snprintf(extra, sizeof(extra), "  ...+%d more",
                 pineapEntries[idx].ssidCount - PINEAP_SSID_SLOTS);
        strncat(ssidBlock, extra, sizeof(ssidBlock) - strlen(ssidBlock) - 1);
    }

    lv_obj_t *ssidLbl = lv_label_create(card);
    lv_label_set_text(ssidLbl, ssidBlock);
    lv_label_set_long_mode(ssidLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ssidLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(ssidLbl,
        flagged ? lv_color_hex(0xff9900) : lv_color_hex(0x8b949e),
        LV_PART_MAIN);
    lv_obj_align(ssidLbl, LV_ALIGN_TOP_LEFT, 0, 68);

    // RSSI bar
    lv_obj_t *bar = lv_bar_create(wifiDetailScreen);
    lv_obj_set_size(bar, SCREEN_W - 12, 5);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(bar, -100, -30);
    lv_bar_set_value(bar, pineapEntries[idx].lastRSSI, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x21262d), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar,
        flagged ? lv_color_hex(0xf85149) : rssiColor(pineapEntries[idx].lastRSSI),
        LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    lv_obj_t *backBtn = createBackBtn(wifiDetailScreen, cb_wifiDetailBack);

    deleteGroup(&wifiDetailGroup);
    wifiDetailGroup = lv_group_create();
    lv_group_add_obj(wifiDetailGroup, backBtn);
    setGroup(wifiDetailGroup);

    lv_screen_load_anim(wifiDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  BLE UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════

// One-time BLE stack init (safe to call multiple times)
static void ensureBLEInit() {
    if (!bleInitialized) {
        BLEDevice::init("");
        bleInitialized = true;
    }
}

// Sort bleEntries[0..n-1] by RSSI descending
static void sortBLEByRSSI() {
    for (int i = 0; i < bleEntryCount - 1; i++)
        for (int j = 0; j < bleEntryCount - 1 - i; j++)
            if (bleEntries[j].rssi < bleEntries[j+1].rssi) {
                BLEEntry tmp     = bleEntries[j];
                bleEntries[j]    = bleEntries[j+1];
                bleEntries[j+1]  = tmp;
            }
}

// ── Meta / RayBan identifier tables (from Marauder, https://github.com/justcallmekoko/ESP32Marauder/wiki/meta-detect) ──────────
static const uint16_t META_IDENTIFIERS[6] = {
    0xFD5F,   // Meta
    0xFEB7,   // Meta
    0xFEB8,   // Meta
    0x01AB,   // Meta
    0x058E,   // Meta
    0x0D53,   // Luxottica (Ray-Ban parent)
};
// Identifiers that should not be flagged as Meta (Samsung, Apple, Microsoft, etc.)
static const uint16_t BLOCKED_IDENTIFIERS[5] = {
    0xFD5A,   // Samsung
    0xFD69,   // Samsung
    0x004C,   // Apple
    0x0006,   // Microsoft
    0xFEF3,   // generic phone
};

static bool isMetaIdentifier(uint16_t id) {
    for (int i = 0; i < 6; i++)
        if (META_IDENTIFIERS[i] == id) return true;
    return false;
}
static bool isBlockedIdentifier(uint16_t id) {
    for (int i = 0; i < 5; i++)
        if (BLOCKED_IDENTIFIERS[i] == id) return true;
    return false;
}
// Extract the 16-bit short UUID from a 128-bit UUID string
// e.g. "0000fd5f-0000-1000-8000-00805f9b34fb" → 0xFD5F
static uint16_t extract16BitFromUUID(String uuid) {
    if (uuid.length() == 36) {
        String hex = uuid.substring(4, 8);
        return (uint16_t)strtol(hex.c_str(), nullptr, 16);
    }
    return 0;
}

// Meta/RayBan: check manufacturer ID, service UUIDs, and service-data UUID
static bool detectMeta(BLEAdvertisedDevice &dev) {
    // 1. Manufacturer data company ID
    if (dev.haveManufacturerData()) {
        String m = dev.getManufacturerData();
        if (m.length() >= 2) {
            uint16_t companyId = ((uint8_t)m[1] << 8) | (uint8_t)m[0];
            if (isBlockedIdentifier(companyId)) return false;
            if (isMetaIdentifier(companyId))   return true;
        }
    }
    // 2. Service UUIDs
    if (dev.haveServiceUUID()) {
        for (int i = 0; i < (int)dev.getServiceUUIDCount(); i++) {
            String uuidStr = String(dev.getServiceUUID(i).toString().c_str());
            uuidStr.toLowerCase();
            uint16_t id = extract16BitFromUUID(uuidStr);
            if (id != 0) {
                if (isBlockedIdentifier(id)) return false;
                if (isMetaIdentifier(id))    return true;
            }
        }
    }
    // 3. Service data UUID
    if (dev.haveServiceData()) {
        String uuidStr = String(dev.getServiceDataUUID().toString().c_str());
        uuidStr.toLowerCase();
        uint16_t id = extract16BitFromUUID(uuidStr);
        if (id != 0) {
            if (isBlockedIdentifier(id)) return false;
            if (isMetaIdentifier(id))    return true;
        }
    }
    return false;
}

// Apple AirTag: Company ID 0x004C + Find My type byte 0x12 + subtype 0x19
static bool detectAirTag(BLEAdvertisedDevice &dev) {
    if (!dev.haveManufacturerData()) return false;
    String m = dev.getManufacturerData();
    if (m.length() < 4) return false;
    return ((uint8_t)m[0] == 0x4C &&
            (uint8_t)m[1] == 0x00 &&
            (uint8_t)m[2] == 0x12 &&
            (uint8_t)m[3] == 0x19);
}

// Apple device (any): Company ID 0x004C
static bool detectApple(BLEAdvertisedDevice &dev) {
    if (!dev.haveManufacturerData()) return false;
    String m = dev.getManufacturerData();
    if (m.length() < 2) return false;
    return ((uint8_t)m[0] == 0x4C && (uint8_t)m[1] == 0x00);
}

// Flipper Zero detection — three independent signals, any match wins:
//
//  1. Advertised name contains "flipper" (case-insensitive).
//     Works when Flipper is on home screen with BLE enabled.
//
//  2. GATT Service UUID 0x3802 — Flipper Zero's primary serial/RPC
//     service. Present in ad packet regardless of name or MAC.
//     This is the most reliable passive indicator.
//
//  3. OUI prefix 0C:FA:22 — only valid for non-random/static addresses.
//     Kept as a bonus check but Flipper defaults to random private
//     addresses so this rarely fires.
//
static bool detectFlipper(BLEAdvertisedDevice &dev) {

    // ── Method 1: Name check ─────────────────────────────────────
    if (dev.haveName()) {
        String n = String(dev.getName().c_str());
        n.toLowerCase();
        if (n.indexOf("flipper") >= 0) return true;
    }

    // ── Method 2: Service UUID 0x3802 ────────────────────────────
    if (dev.haveServiceUUID()) {
        int uuidCount = dev.getServiceUUIDCount();
        for (int i = 0; i < uuidCount; i++) {
            BLEUUID uuid = dev.getServiceUUID(i);
            // Normalise to 16-bit if possible for comparison
            if (uuid.bitSize() == 16) {
                String uuidCheck = String(uuid.toString().c_str());
                uuidCheck.toLowerCase();
                if (uuidCheck.indexOf("3802") >= 0) return true;
            }
            // Also catch full 128-bit expansion of 0x3802
            String uuidStr = String(uuid.toString().c_str());
            uuidStr.toLowerCase();
            if (uuidStr.indexOf("00003802") >= 0) return true;
        }
    }

    // ── Method 3: OUI check (bonus – rarely fires with random addr) ─
    String mac = String(dev.getAddress().toString().c_str());
    if (mac.length() >= 8) {
        String oui = mac.substring(0, 8);
        oui.toLowerCase();
        if (oui == "0c:fa:22") return true;
    }

    return false;
}

// Short manufacturer hint string for list row display
static const char *mfgHintStr(BLEDeviceType t) {
    switch (t) {
        case BLE_AIRTAG:  return "[AirTag]";
        case BLE_FLIPPER: return "[Flipper]";
        case BLE_SKIMMER: return "[Skimmer?]";
        case BLE_META:    return "[Meta]";
        case BLE_APPLE:   return "[Apple]";
        default:          return "";
    }
}

// RSSI colour (reuse WiFi palette)
static lv_color_t bleRssiColor(int8_t rssi) {
    if (rssi >= -55) return lv_color_hex(0x3fb950);
    if (rssi >= -70) return lv_color_hex(0xe3b341);
    return              lv_color_hex(0xf85149);
}

// Blocking BLE scan — fills bleEntries[], returns count
// Pass filterType = BLE_AIRTAG or BLE_FLIPPER to return only those,
// or BLE_GENERIC (0) to return all devices.
static int doBLEScan(int durationSec, BLEDeviceType filterType) {
    ensureBLEInit();
    WiFi.disconnect();          // avoid coexistence issues during scan
    delay(50);

    BLEScan *pScan = BLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(150);   // was 100 — gives more time for scan responses
    pScan->setWindow(140);     // was 99

    BLEScanResults *results = pScan->start(durationSec, false);
    int total = results->getCount();

    bleEntryCount = 0;
    for (int i = 0; i < total && bleEntryCount < MAX_BLE_RESULTS; i++) {
        BLEAdvertisedDevice dev = results->getDevice(i);

        // Classify device
        BLEDeviceType dtype = BLE_GENERIC;
        if      (detectFlipper(dev)) dtype = BLE_FLIPPER;
        else if (detectAirTag(dev))  dtype = BLE_AIRTAG;
        else if (detectApple(dev))   dtype = BLE_APPLE;
        else if (detectMeta(dev))    dtype = BLE_META;
        else if (dev.haveName()) {
            // Skimmer check — HC-03, HC-05, HC-06 Bluetooth modules
            String nm = dev.getName().c_str();
            if (nm == "HC-03" || nm == "HC-05" || nm == "HC-06")
                dtype = BLE_SKIMMER;
        }

        // If a filter is requested, skip non-matching
        if (filterType != BLE_GENERIC && dtype != filterType) continue;

        // Store entry
        String nm = dev.haveName() ? dev.getName().c_str() : "<unknown>";
        strncpy(bleEntries[bleEntryCount].name, nm.c_str(), 32);
        bleEntries[bleEntryCount].name[32] = '\0';

        String mac = dev.getAddress().toString().c_str();
        strncpy(bleEntries[bleEntryCount].mac, mac.c_str(), 17);
        bleEntries[bleEntryCount].mac[17] = '\0';

        bleEntries[bleEntryCount].rssi = (int8_t)dev.getRSSI();
        bleEntries[bleEntryCount].type = dtype;
        strncpy(bleEntries[bleEntryCount].mfgHint,
                mfgHintStr(dtype), 13);
        bleEntries[bleEntryCount].mfgHint[13] = '\0';

        bleEntryCount++;
    }

    pScan->clearResults();
    sortBLEByRSSI();
    return bleEntryCount;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════
//  TOOL 5 – PWNAGOTCHI WATCH  Credit: https://github.com/justcallmekoko
//
//  Pwnagotchi broadcasts 802.11 beacon frames with a hardcoded
//  source MAC of de:ad:be:ef:de:ad. The SSID field carries a JSON
//  blob: {"name":"pikachu","pwnd_tot":42,...}
//  We run promiscuous mode and sniff for those beacons.
//
//  Beacon frame layout (bytes):
//    [0-1]   Frame Control  (0x80 0x00)
//    [4-9]   Destination    (ff:ff:ff:ff:ff:ff)
//    [10-15] Source MAC     ← must be de:ad:be:ef:de:ad
//    [16-21] BSSID
//    [24-35] Fixed params   (timestamp 8B + interval 2B + caps 2B)
//    [36]    Tag 0x00 = SSID tag
//    [37]    SSID length
//    [38..]  SSID bytes     ← JSON payload
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════

static lv_obj_t *pwnStatusLbl = nullptr;
static lv_obj_t *pwnList      = nullptr;

// ISR-safe: just verify the MAC and copy the raw SSID/BSSID into
// the pending slot. The refresh timer does the JSON parsing.
static void IRAM_ATTR pwn_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!pwnActive || type != WIFI_PKT_MGMT || pwnPendingReady) return;

    const wifi_promiscuous_pkt_t *pkt =
        reinterpret_cast<const wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *d = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    // Must be a beacon (0x80) and long enough to have the SSID tag
    if (d[0] != 0x80 || len < 38) return;

    // Source MAC (offset 10) must be de:ad:be:ef:de:ad
    if (d[10] != 0xDE || d[11] != 0xAD || d[12] != 0xBE ||
        d[13] != 0xEF || d[14] != 0xDE || d[15] != 0xAD) return;

    // SSID tag (byte 36 = 0x00), length at 37
    if (d[36] != 0x00) return;
    uint8_t ssidLen = d[37];
    if (ssidLen == 0 || ssidLen > 32 || (38 + ssidLen) > len) return;

    // Copy into pending slot — refresh timer will parse
    memcpy(pwnPendingSSID, &d[38], ssidLen);
    pwnPendingSSID[ssidLen] = '\0';
    snprintf(pwnPendingBSSID, sizeof(pwnPendingBSSID),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             d[16], d[17], d[18], d[19], d[20], d[21]);
    pwnPendingRSSI  = pkt->rx_ctrl.rssi;
    pwnPendingReady = true;
}

// Parse a pending SSID JSON blob and update pwnEntries[].
// Called from the LVGL timer (main task) — safe to use strstr/atoi.
static void processPwnPending() {
    if (!pwnPendingReady) return;

    // Snapshot and clear the flag first
    char ssid[PWN_BUF_LEN];
    char bssid[18];
    int8_t rssi = pwnPendingRSSI;
    memcpy(ssid,  pwnPendingSSID,  PWN_BUF_LEN);
    memcpy(bssid, pwnPendingBSSID, 18);
    pwnPendingReady = false;

    // Parse "name":"..." from JSON
    char name[33] = "<unknown>";
    char *ns = strstr(ssid, "\"name\":\"");
    if (ns) {
        ns += 8;
        char *ne = strchr(ns, '"');
        if (ne) {
            int nl = (int)(ne - ns);
            if (nl > 32) nl = 32;
            memcpy(name, ns, nl);
            name[nl] = '\0';
        }
    }

    // Parse "pwnd_tot":N
    int pwnd = 0;
    char *ps = strstr(ssid, "\"pwnd_tot\":");
    if (ps) pwnd = atoi(ps + 11);

    // Update existing entry by name, or add new one
    for (int i = 0; i < pwnCount; i++) {
        if (strcmp(pwnEntries[i].name, name) == 0) {
            pwnEntries[i].rssi     = rssi;
            pwnEntries[i].pwnd_tot = pwnd;
            pwnEntries[i].lastSeen = millis();
            return;
        }
    }
    if (pwnCount >= MAX_PWNS) return;
    strncpy(pwnEntries[pwnCount].name,  name,  32);
    strncpy(pwnEntries[pwnCount].bssid, bssid, 17);
    pwnEntries[pwnCount].name[32]  = '\0';
    pwnEntries[pwnCount].bssid[17] = '\0';
    pwnEntries[pwnCount].pwnd_tot  = pwnd;
    pwnEntries[pwnCount].rssi      = rssi;
    pwnEntries[pwnCount].lastSeen  = millis();
    pwnCount++;
}

static void pwn_refresh_cb(lv_timer_t *) {
    if (!pwnStatusLbl || !pwnList) return;

    processPwnPending();

    // Status label
    if (pwnCount == 0) {
        lv_label_set_text(pwnStatusLbl,
            LV_SYMBOL_EYE_OPEN "  Watching... no Pwnagotchi seen");
        lv_obj_set_style_text_color(pwnStatusLbl,
            lv_color_hex(0x8b949e), LV_PART_MAIN);
    } else {
        char buf[56];
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_WARNING "  %d Pwnagotchi%s detected!",
                 pwnCount, pwnCount == 1 ? "" : "s");
        lv_label_set_text(pwnStatusLbl, buf);
        lv_obj_set_style_text_color(pwnStatusLbl,
            lv_color_hex(0xf85149), LV_PART_MAIN);
    }

    // Rebuild list
    lv_obj_clean(pwnList);
    if (pwnCount == 0) {
        lv_obj_t *e = lv_list_add_text(pwnList,
            "Hopping ch 1-13 — waiting for beacon...");
        if (e) lv_obj_set_style_text_color(e,
                    lv_color_hex(0x8b949e), LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < pwnCount; i++) {
        uint32_t ageSec = (millis() - pwnEntries[i].lastSeen) / 1000;
        char row[64];
        snprintf(row, sizeof(row), "%s  %ddBm  %d pwnd  %lus",
                 pwnEntries[i].name,
                 pwnEntries[i].rssi,
                 pwnEntries[i].pwnd_tot,
                 (unsigned long)ageSec);
        lv_obj_t *entry = lv_list_add_text(pwnList, row);
        if (entry)
            lv_obj_set_style_text_color(entry,
                lv_color_hex(0xf85149), LV_PART_MAIN);
    }
}

void createPwnagotchiDetector() {
    // Reset state
    pwnCount        = 0;
    pwnPendingReady = false;
    pwnStatusLbl    = nullptr;
    pwnList         = nullptr;
    memset(pwnEntries, 0, sizeof(pwnEntries));

    if (wifiToolScreen) { lv_obj_delete(wifiToolScreen); wifiToolScreen = nullptr; }
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_EYE_OPEN "  Pwnagotchi Watch");

    pwnStatusLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(pwnStatusLbl,
        "Sniffs beacons from de:ad:be:ef:de:ad\n"
        "Parses name + pwnd_tot from SSID JSON");
    lv_obj_set_style_text_color(pwnStatusLbl,
        lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(pwnStatusLbl, 8, 30);

    pwnList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(pwnList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(pwnList, 0, 48);
    lv_obj_set_style_bg_color(pwnList,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pwnList,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(pwnList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pwnList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(pwnList,      1, LV_PART_MAIN);

    lv_obj_t *initLbl =
        lv_list_add_text(pwnList, "Hopping ch 1-13 — waiting for beacon...");
    if (initLbl)
        lv_obj_set_style_text_color(initLbl,
            lv_color_hex(0x8b949e), LV_PART_MAIN);

    lv_obj_t *backBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, backBtn);
    setGroup(wifiToolGroup);

    // Hot-pink LEDs to distinguish from deauth (green)
    setAllLEDs(220, 0, 150, LED_BRIGHTNESS);

    // Start promiscuous sniffer on channel 1
    pwnActive = true;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(pwn_sniffer_cb);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    deauthChannel = 1;   // reuse hop counter

    if (pwnTimer) { lv_timer_delete(pwnTimer); pwnTimer = nullptr; }
    pwnTimer = lv_timer_create(pwn_refresh_cb, 2000, nullptr);

    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  TOOL 6 – FLOCK SAFETY DETECTOR
//
//  Flock Safety LPR cameras beacon / probe with "flock" in the SSID.
//  We sniff beacon frames (mgmt subtype 8), probe responses (5), and
//  probe requests (4), parse the SSID IE, and alert on any case-
//  insensitive match for "flock". Alerts are latching — stays red
//  until you navigate back. Hops all 2.4 GHz channels (1-13).
//  Reference: github.com/GainSec/Flock-Safety-Trap-Shooter-Sniffer-Alarm
// ════════════════════════════════════════════════════════════════

static lv_obj_t *flockStatusLbl = nullptr;
static lv_obj_t *flockList      = nullptr;

// Case-insensitive substring search (no strstr in IRAM-safe code)
static bool IRAM_ATTR containsFlock(const char *ssid) {
    // "flock" = f,l,o,c,k
    for (int i = 0; ssid[i]; i++) {
        if ((ssid[i]   == 'f' || ssid[i]   == 'F') &&
            (ssid[i+1] == 'l' || ssid[i+1] == 'L') &&
            (ssid[i+2] == 'o' || ssid[i+2] == 'O') &&
            (ssid[i+3] == 'c' || ssid[i+3] == 'C') &&
            (ssid[i+4] == 'k' || ssid[i+4] == 'K'))
            return true;
    }
    return false;
}

static void IRAM_ATTR flock_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!flockActive || type != WIFI_PKT_MGMT || flockPendingReady) return;

    const wifi_promiscuous_pkt_t *pkt =
        reinterpret_cast<const wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *d = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    if (len < 24) return;

    // Frame Control byte 0: type bits [3:2], subtype bits [7:4]
    uint8_t fc0    = d[0];
    uint8_t ftype  = (fc0 >> 2) & 0x03;  // must be 0 (management)
    uint8_t stype  = (fc0 >> 4) & 0x0F;  // 8=beacon, 5=probe resp, 4=probe req

    if (ftype != 0) return;
    if (stype != 8 && stype != 5 && stype != 4) return;

    // IE parsing starts after 24-byte MAC header
    // Beacons/probe-responses have 12 extra fixed bytes; probe-requests don't
    int ieOffset = (stype == 4) ? 24 : 36;
    if (ieOffset >= len) return;

    const uint8_t *ie  = d + ieOffset;
    int            rem = len - ieOffset;

    while (rem >= 2) {
        uint8_t id   = ie[0];
        uint8_t elen = ie[1];
        if (elen + 2 > rem) break;

        if (id == 0 && elen > 0) {   // SSID element
            int n = elen > 32 ? 32 : elen;
            char ssid[33];
            memcpy(ssid, ie + 2, n);
            ssid[n] = '\0';

            if (containsFlock(ssid)) {
                memcpy(flockPendingSSID, ssid, n + 1);
                snprintf(flockPendingSrc, sizeof(flockPendingSrc),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         d[10], d[11], d[12], d[13], d[14], d[15]);
                flockPendingType  = (stype == 4) ? 1 : 0;
                flockPendingRSSI  = pkt->rx_ctrl.rssi;
                flockPendingReady = true;
                return;
            }
        }
        ie  += elen + 2;
        rem -= elen + 2;
    }
}

static void flock_refresh_cb(lv_timer_t *) {
    if (!flockStatusLbl || !flockList) return;

    // Process any pending hit from the ISR
    if (flockPendingReady) {
        char ssid[33], src[18];
        uint8_t ft   = flockPendingType;
        int8_t  rssi = flockPendingRSSI;
        memcpy(ssid, flockPendingSSID, 33);
        memcpy(src,  flockPendingSrc,  18);
        flockPendingReady = false;

        // Deduplicate by SSID
        bool found = false;
        for (int i = 0; i < flockHitCount; i++) {
            if (strcmp(flockHits[i].ssid, ssid) == 0) {
                flockHits[i].rssi = rssi;   // update RSSI
                found = true;
                break;
            }
        }
        if (!found && flockHitCount < MAX_FLOCK_HITS) {
            strncpy(flockHits[flockHitCount].ssid, ssid, 32);
            flockHits[flockHitCount].ssid[32] = '\0';
            strncpy(flockHits[flockHitCount].src, src, 17);
            flockHits[flockHitCount].src[17] = '\0';
            flockHits[flockHitCount].frameType = ft;
            flockHits[flockHitCount].rssi      = rssi;
            flockHitCount++;
        }
    }

    // Update status label
    if (flockHitCount == 0) {
        lv_label_set_text(flockStatusLbl,
            LV_SYMBOL_EYE_OPEN "  Watching... no Flock seen");
        lv_obj_set_style_text_color(flockStatusLbl,
            lv_color_hex(0x8b949e), LV_PART_MAIN);
    } else {
        char buf[56];
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_WARNING "  %d Flock SSID%s detected!",
                 flockHitCount, flockHitCount == 1 ? "" : "s");
        lv_label_set_text(flockStatusLbl, buf);
        lv_obj_set_style_text_color(flockStatusLbl,
            lv_color_hex(0xf85149), LV_PART_MAIN);
    }

    // Rebuild list
    lv_obj_clean(flockList);
    if (flockHitCount == 0) {
        lv_obj_t *e = lv_list_add_text(flockList,
            "Hopping ch 1-13 — watching beacons & probes...");
        if (e) lv_obj_set_style_text_color(e,
                    lv_color_hex(0x8b949e), LV_PART_MAIN);
        return;
    }
    for (int i = 0; i < flockHitCount; i++) {
        char row[64];
        snprintf(row, sizeof(row), "%s  %s  %ddBm",
                 flockHits[i].frameType ? "PROBE" : "BEACON",
                 flockHits[i].ssid,
                 flockHits[i].rssi);
        lv_obj_t *entry = lv_list_add_text(flockList, row);
        if (entry)
            lv_obj_set_style_text_color(entry,
                lv_color_hex(0xf85149), LV_PART_MAIN);
    }
}

void createFlockDetector() {
    // Reset state
    flockHitCount     = 0;
    flockPendingReady = false;
    flockStatusLbl    = nullptr;
    flockList         = nullptr;
    memset(flockHits, 0, sizeof(flockHits));

    if (wifiToolScreen) { lv_obj_delete(wifiToolScreen); wifiToolScreen = nullptr; }
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_WARNING "  Flock Detector");

    flockStatusLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(flockStatusLbl,
        "Sniffs beacons & probes for \"flock\"\n"
        "in SSID — flags Flock Safety cameras");
    lv_obj_set_style_text_color(flockStatusLbl,
        lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(flockStatusLbl, 8, 30);

    flockList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(flockList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(flockList, 0, 48);
    lv_obj_set_style_bg_color(flockList,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(flockList,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(flockList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(flockList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(flockList,      1, LV_PART_MAIN);

    lv_obj_t *initLbl =
        lv_list_add_text(flockList, "Hopping ch 1-13 — watching beacons & probes...");
    if (initLbl)
        lv_obj_set_style_text_color(initLbl,
            lv_color_hex(0x8b949e), LV_PART_MAIN);

    lv_obj_t *backBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, backBtn);
    setGroup(wifiToolGroup);

    // Yellow-orange LEDs — distinct from deauth (green) and pwnagotchi (pink)
    setAllLEDs(220, 120, 0, LED_BRIGHTNESS);

    // Start promiscuous sniffer
    flockActive = true;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(flock_sniffer_cb);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    deauthChannel = 1;

    if (flockTimer) { lv_timer_delete(flockTimer); flockTimer = nullptr; }
    flockTimer = lv_timer_create(flock_refresh_cb, 1500, nullptr);

    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  BLE SHARED BACK CALLBACKS
// ════════════════════════════════════════════════════════════════

static void cb_bleToolBack(lv_event_t *e) {
    bleToolScreen = nullptr;
    deleteGroup(&bleToolGroup);
    setGroup(bleMenuGroup);
    lv_screen_load_anim(bleMenuScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    setAllLEDs(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b, 3);
}

static void cb_bleDetailBack(lv_event_t *e) {
    bleDetailScreen = nullptr;
    deleteGroup(&bleDetailGroup);
    setGroup(bleToolGroup);
    lv_screen_load_anim(bleToolScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
}

// ════════════════════════════════════════════════════════════════
//  BLE MENU
// ════════════════════════════════════════════════════════════════

static const char *BLE_TOOL_LABELS[5] = {
    LV_SYMBOL_BLUETOOTH "  BLE Scanner",
    "\xEF\x80\xA6"      "  AirTag Detector",   // apple-ish symbol fallback
    LV_SYMBOL_WARNING   "  Flipper Detector",
    LV_SYMBOL_WARNING   "  Skimmer Detector",
    LV_SYMBOL_EYE_OPEN  "  Meta Detector"
};

static void cb_bleMenuBack(lv_event_t *e) {
    lv_screen_load_anim(mainScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
    deleteGroup(&bleMenuGroup);
    setGroup(navGroup);
    setAllLEDs(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);
}

static void cb_bleToolSelected(lv_event_t *e) {
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    switch (t) {
        case 0: createBLEScanner();      break;
        case 1: createAirTagScanner();   break;
        case 2: createFlipperScanner();  break;
        case 3: createSkimmerScanner();  break;
        case 4: createMetaDetector();    break;
    }
}

void createBLEMenu() {
    if (bleMenuScreen) { lv_obj_delete(bleMenuScreen); bleMenuScreen = nullptr; }
    bleMenuScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleMenuScreen);
    createHeader(bleMenuScreen, LV_SYMBOL_BLUETOOTH "  BLE Tools");

    lv_obj_t *list = lv_list_create(bleMenuScreen);
    lv_obj_set_size(list, SCREEN_W, SCREEN_H - 28 - 34);
    lv_obj_set_pos(list, 0, 28);
    lv_obj_set_style_bg_color(list,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list,      6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list,      4, LV_PART_MAIN);

    deleteGroup(&bleMenuGroup);
    bleMenuGroup = lv_group_create();

    for (int i = 0; i < 5; i++) {
        lv_obj_t *btn = lv_list_add_btn(list, nullptr, BLE_TOOL_LABELS[i]);
        styleListBtn(btn);
        lv_obj_set_height(btn, 30);
        lv_obj_add_event_cb(btn, cb_bleToolSelected, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_group_add_obj(bleMenuGroup, btn);
    }

    lv_obj_t *backBtn = createBackBtn(bleMenuScreen, cb_bleMenuBack);
    lv_group_add_obj(bleMenuGroup, backBtn);
    setGroup(bleMenuGroup);

    setAllLEDs(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b, 3);
    lv_screen_load_anim(bleMenuScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  TOOL 1 – BLE SCANNER
// ════════════════════════════════════════════════════════════════

static lv_obj_t *bleScanList      = nullptr;
static lv_obj_t *bleScanStatusLbl = nullptr;
static lv_obj_t *bleScanBackBtn   = nullptr;   // saved so rebuildBLEScanList can rebuild the group
static lv_obj_t *bleScanScanBtn   = nullptr;

static void rebuildBLEScanList() {
    // Rebuild group first — lv_obj_clean will invalidate the old list buttons
    // that are still referenced inside bleToolGroup, causing a crash on back.
    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    if (bleScanBackBtn) lv_group_add_obj(bleToolGroup, bleScanBackBtn);
    if (bleScanScanBtn) lv_group_add_obj(bleToolGroup, bleScanScanBtn);
    setGroup(bleToolGroup);

    lv_obj_clean(bleScanList);
    for (int i = 0; i < bleEntryCount; i++) {
        // Row: name (truncated 15) + mfg hint + rssi
        char nameTrunc[16];
        strncpy(nameTrunc, bleEntries[i].name, 15);
        nameTrunc[15] = '\0';

        char row[56];
        snprintf(row, sizeof(row), "%-15s %-9s %ddBm",
                 nameTrunc,
                 bleEntries[i].mfgHint,
                 bleEntries[i].rssi);

        lv_obj_t *btn = lv_list_add_btn(bleScanList, nullptr, row);
        styleListBtn(btn);

        // Colour by device type first, then RSSI for generics
        lv_color_t col;
        switch (bleEntries[i].type) {
            case BLE_AIRTAG:  col = lv_color_hex(0xf0f0f0); break; // white-ish
            case BLE_FLIPPER: col = lv_color_hex(0xff9900); break; // orange
            case BLE_APPLE:   col = lv_color_hex(0x58a6ff); break; // blue accent
            default:          col = bleRssiColor(bleEntries[i].rssi); break;
        }
        lv_obj_set_style_text_color(btn, col, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            createBLEDetail((int)(intptr_t)lv_event_get_user_data(ev));
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(bleToolGroup, btn);
    }
}

static void cb_doBLEScan(lv_event_t *e) {
    lv_label_set_text(bleScanStatusLbl, LV_SYMBOL_REFRESH "  Scanning 5s...");
    lv_obj_set_style_text_color(bleScanStatusLbl, lv_color_hex(0xe3b341), LV_PART_MAIN);
    lv_timer_handler();

    // Blue spinner while BLE scan blocks core 1
    startLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);
    int found = doBLEScan(BLE_SCAN_SECS, BLE_GENERIC);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    char buf[48];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_BLUETOOTH "  %d device%s found",
             found, found == 1 ? "" : "s");
    lv_label_set_text(bleScanStatusLbl, buf);
    lv_obj_set_style_text_color(bleScanStatusLbl,
        found > 0 ? lv_color_hex(0x3fb950) : lv_color_hex(0x8b949e),
        LV_PART_MAIN);

    rebuildBLEScanList();
}

void createBLEScanner() {
    if (bleToolScreen) { lv_obj_delete(bleToolScreen); bleToolScreen = nullptr; }
    bleToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleToolScreen);
    createHeader(bleToolScreen, LV_SYMBOL_BLUETOOTH "  BLE Scanner");

    bleScanStatusLbl = lv_label_create(bleToolScreen);
    lv_label_set_text(bleScanStatusLbl, "Press Scan to start  (5s)");
    lv_obj_set_style_text_color(bleScanStatusLbl, lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(bleScanStatusLbl, 8, 30);

    bleScanList = lv_list_create(bleToolScreen);
    lv_obj_set_size(bleScanList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(bleScanList, 0, 48);
    lv_obj_set_style_bg_color(bleScanList,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bleScanList,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(bleScanList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bleScanList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(bleScanList,      2, LV_PART_MAIN);

    bleScanBackBtn = createBackBtn(bleToolScreen, cb_bleToolBack);
    bleScanScanBtn = createActionBtn(bleToolScreen,
                                        LV_SYMBOL_REFRESH "  Scan",
                                        cb_doBLEScan);

    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    lv_group_add_obj(bleToolGroup, bleScanBackBtn);
    lv_group_add_obj(bleToolGroup, bleScanScanBtn);
    setGroup(bleToolGroup);

    lv_screen_load_anim(bleToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  BLE DEVICE DETAIL SCREEN
// ════════════════════════════════════════════════════════════════

void createBLEDetail(int idx) {
    if (bleDetailScreen) { lv_obj_delete(bleDetailScreen); bleDetailScreen = nullptr; }
    bleDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleDetailScreen);

    // Header shows device name
    char hdr[48];
    snprintf(hdr, sizeof(hdr), LV_SYMBOL_BLUETOOTH "  %.28s", bleEntries[idx].name);
    createHeader(bleDetailScreen, hdr);

    // Determine badge text + colour by type
    const char *badge     = "";
    lv_color_t  badgeCol  = lv_color_hex(0x8b949e);
    switch (bleEntries[idx].type) {
        case BLE_AIRTAG:
            badge    = "  Apple AirTag (Find My)";
            badgeCol = lv_color_hex(0xf0f0f0);
            break;
        case BLE_FLIPPER:
            badge    = "  Flipper Zero";
            badgeCol = lv_color_hex(0xff9900);
            break;
        case BLE_SKIMMER:
            badge    = "  POSSIBLE SKIMMER";
            badgeCol = lv_color_hex(0xf85149);
            break;
        case BLE_META:
            badge    = "  Meta / RayBan Device";
            badgeCol = lv_color_hex(0x58a6ff);
            break;
        case BLE_APPLE:
            badge    = "  Apple Device";
            badgeCol = lv_color_hex(0x58a6ff);
            break;
        default:
            badge    = "  Generic BLE";
            badgeCol = lv_color_hex(0x8b949e);
            break;
    }

    // Info card
    lv_obj_t *card = lv_obj_create(bleDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,      lv_color_hex(0x161b22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,        LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card,  lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(card,  1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,        6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,       6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Badge label (type identification)
    lv_obj_t *badgeLbl = lv_label_create(card);
    lv_label_set_text(badgeLbl, badge);
    lv_obj_set_style_text_color(badgeLbl, badgeCol, LV_PART_MAIN);
    lv_obj_align(badgeLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    // Detail fields
    int8_t rssi = bleEntries[idx].rssi;
    const char *quality =
        rssi >= -55 ? "Excellent" :
        rssi >= -65 ? "Good"      :
        rssi >= -75 ? "Fair"      : "Weak";

    char info[200];
    snprintf(info, sizeof(info),
             "Name  : %s\n"
             "MAC   : %s\n"
             "RSSI  : %d dBm  (%s)",
             bleEntries[idx].name,
             bleEntries[idx].mac,
             rssi, quality);

    lv_obj_t *infoLbl = lv_label_create(card);
    lv_label_set_text(infoLbl, info);
    lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(infoLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(0xe6edf3), LV_PART_MAIN);
    lv_obj_align(infoLbl, LV_ALIGN_TOP_LEFT, 0, 16);

    // RSSI bar
    lv_obj_t *bar = lv_bar_create(bleDetailScreen);
    lv_obj_set_size(bar, SCREEN_W - 12, 5);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(bar, -100, -30);
    lv_bar_set_value(bar, rssi, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x21262d), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, bleRssiColor(rssi),     LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    lv_obj_t *backBtn = createBackBtn(bleDetailScreen, cb_bleDetailBack);

    deleteGroup(&bleDetailGroup);
    bleDetailGroup = lv_group_create();
    lv_group_add_obj(bleDetailGroup, backBtn);
    setGroup(bleDetailGroup);

    lv_screen_load_anim(bleDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════════
//  TOOL 2 – AIRTAG DETECTOR  Credit: https://github.com/justcallmekoko/ESP32Marauder/wiki/airtag-sniff
// ══════════════════════════════════════════════════════════════════════════════════════════════════════

static lv_obj_t *airtagStatusLbl = nullptr;
static lv_obj_t *airtagList      = nullptr;

static void cb_doAirTagScan(lv_event_t *e) {
    lv_label_set_text(airtagStatusLbl, LV_SYMBOL_REFRESH "  Scanning 8s...");
    lv_obj_set_style_text_color(airtagStatusLbl, lv_color_hex(0xe3b341), LV_PART_MAIN);
    lv_obj_clean(airtagList);
    lv_timer_handler();

    // White spinner for AirTag scan (Apple = white/silver)
    startLEDSpinner(200, 200, 200);
    int found = doBLEScan(BLE_SCAN_SECS, BLE_AIRTAG);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(airtagStatusLbl,
                          LV_SYMBOL_BLUETOOTH "  No AirTags detected");
        lv_obj_set_style_text_color(airtagStatusLbl,
                                    lv_color_hex(0x3fb950), LV_PART_MAIN);
        lv_obj_t *empty = lv_list_add_text(airtagList,
                                            "No Apple AirTags in range");
        if (empty)
            lv_obj_set_style_text_color(empty, lv_color_hex(0x8b949e), LV_PART_MAIN);
        return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WARNING "  %d AirTag%s detected!",
             found, found == 1 ? "" : "s");
    lv_label_set_text(airtagStatusLbl, buf);
    lv_obj_set_style_text_color(airtagStatusLbl, lv_color_hex(0xf85149), LV_PART_MAIN);

    for (int i = 0; i < found; i++) {
        char row[52];
        snprintf(row, sizeof(row), "%s  %ddBm",
                 bleEntries[i].mac, bleEntries[i].rssi);
        lv_obj_t *entry = lv_list_add_text(airtagList, row);
        if (entry)
            lv_obj_set_style_text_color(entry, lv_color_hex(0xf0f0f0), LV_PART_MAIN);
    }
}

void createAirTagScanner() {
    if (bleToolScreen) { lv_obj_delete(bleToolScreen); bleToolScreen = nullptr; }
    bleToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleToolScreen);
    createHeader(bleToolScreen, "\xEF\x80\xA6  AirTag Detector");

    airtagStatusLbl = lv_label_create(bleToolScreen);
    lv_label_set_text(airtagStatusLbl,
        "Scans for Apple AirTag Find My\n"
        "advertisements  (Company ID 0x004C\n"
        "type 0x12 subtype 0x19)");
    lv_obj_set_style_text_color(airtagStatusLbl, lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(airtagStatusLbl, 8, 30);

    airtagList = lv_list_create(bleToolScreen);
    lv_obj_set_size(airtagList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(airtagList, 0, 48);
    lv_obj_set_style_bg_color(airtagList,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(airtagList,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(airtagList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(airtagList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(airtagList,      2, LV_PART_MAIN);

    lv_obj_t *backBtn = createBackBtn(bleToolScreen, cb_bleToolBack);
    lv_obj_t *scanBtn = createActionBtn(bleToolScreen,
                                        LV_SYMBOL_REFRESH "  Scan",
                                        cb_doAirTagScan);

    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    lv_group_add_obj(bleToolGroup, backBtn);
    lv_group_add_obj(bleToolGroup, scanBtn);
    setGroup(bleToolGroup);

    lv_screen_load_anim(bleToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════
//  TOOL 3 – FLIPPER ZERO DETECTOR  Credit: https://github.com/justcallmekoko/ESP32Marauder/wiki/flipper-sniff
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════

static lv_obj_t *flipperStatusLbl = nullptr;
static lv_obj_t *flipperList      = nullptr;

static void cb_doFlipperScan(lv_event_t *e) {
    lv_label_set_text(flipperStatusLbl, LV_SYMBOL_REFRESH "  Scanning 8s...");
    lv_obj_set_style_text_color(flipperStatusLbl, lv_color_hex(0xe3b341), LV_PART_MAIN);
    lv_obj_clean(flipperList);
    lv_timer_handler();

    // Orange spinner for Flipper
    startLEDSpinner(220, 100, 0);
    int found = doBLEScan(BLE_SCAN_SECS, BLE_FLIPPER);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(flipperStatusLbl,
                          LV_SYMBOL_BLUETOOTH "  No Flipper Zero detected");
        lv_obj_set_style_text_color(flipperStatusLbl,
                                    lv_color_hex(0x3fb950), LV_PART_MAIN);
        lv_obj_t *empty = lv_list_add_text(flipperList,
                                            "No Flipper Zero in range");
        if (empty)
            lv_obj_set_style_text_color(empty, lv_color_hex(0x8b949e), LV_PART_MAIN);
        return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WARNING "  %d Flipper%s detected!",
             found, found == 1 ? "" : "s");
    lv_label_set_text(flipperStatusLbl, buf);
    lv_obj_set_style_text_color(flipperStatusLbl, lv_color_hex(0xff9900), LV_PART_MAIN);

    for (int i = 0; i < found; i++) {
        char row[52];
        snprintf(row, sizeof(row), "%-20s  %ddBm",
                 bleEntries[i].name, bleEntries[i].rssi);
        lv_obj_t *entry = lv_list_add_text(flipperList, row);
        if (entry)
            lv_obj_set_style_text_color(entry, lv_color_hex(0xff9900), LV_PART_MAIN);

        // Second line with MAC
        char macRow[28];
        snprintf(macRow, sizeof(macRow), "  %s", bleEntries[i].mac);
        lv_obj_t *macEntry = lv_list_add_text(flipperList, macRow);
        if (macEntry)
            lv_obj_set_style_text_color(macEntry, lv_color_hex(0x8b949e), LV_PART_MAIN);
    }
}

void createFlipperScanner() {
    if (bleToolScreen) { lv_obj_delete(bleToolScreen); bleToolScreen = nullptr; }
    bleToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleToolScreen);
    createHeader(bleToolScreen, LV_SYMBOL_WARNING "  Flipper Detector");

    flipperStatusLbl = lv_label_create(bleToolScreen);
    lv_label_set_text(flipperStatusLbl,
        "OUI: 0C:FA:22 (IEEE prefix)\n"
        "or name containing \"Flipper\"");
    lv_obj_set_style_text_color(flipperStatusLbl, lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(flipperStatusLbl, 8, 30);

    flipperList = lv_list_create(bleToolScreen);
    lv_obj_set_size(flipperList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(flipperList, 0, 48);
    lv_obj_set_style_bg_color(flipperList,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(flipperList,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(flipperList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(flipperList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(flipperList,      2, LV_PART_MAIN);

    lv_obj_t *backBtn = createBackBtn(bleToolScreen, cb_bleToolBack);
    lv_obj_t *scanBtn = createActionBtn(bleToolScreen,
                                        LV_SYMBOL_REFRESH "  Scan",
                                        cb_doFlipperScan);

    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    lv_group_add_obj(bleToolGroup, backBtn);
    lv_group_add_obj(bleToolGroup, scanBtn);
    setGroup(bleToolGroup);

    lv_screen_load_anim(bleToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
//  BLE TOOL 4 – SKIMMER DETECTOR  Credit: https://github.com/justcallmekoko/ESP32Marauder/wiki/detect-card-skimmers
//
//  Scans for Bluetooth devices with names matching known skimmer
//  modules: HC-03, HC-05, HC-06. These cheap hobbyist boards are
//  commonly found inside gas pump card skimmers.
//  Reference: github.com/sparkfunX/Skimmer_Scanner
//             github.com/justcallmekoko/ESP32Marauder
//  ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

static lv_obj_t *skimmerStatusLbl = nullptr;
static lv_obj_t *skimmerList      = nullptr;

static void cb_doSkimmerScan(lv_event_t *e) {
    lv_label_set_text(skimmerStatusLbl, LV_SYMBOL_REFRESH "  Scanning 8s...");
    lv_obj_set_style_text_color(skimmerStatusLbl, lv_color_hex(0xe3b341), LV_PART_MAIN);
    lv_obj_clean(skimmerList);
    lv_timer_handler();

    // Red spinner — danger colour for skimmers
    startLEDSpinner(220, 0, 0);
    int found = doBLEScan(BLE_SCAN_SECS, BLE_SKIMMER);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(skimmerStatusLbl,
            LV_SYMBOL_OK "  No skimmer modules detected");
        lv_obj_set_style_text_color(skimmerStatusLbl,
            lv_color_hex(0x3fb950), LV_PART_MAIN);
        lv_obj_t *empty = lv_list_add_text(skimmerList,
            "No HC-03 / HC-05 / HC-06 in range");
        if (empty)
            lv_obj_set_style_text_color(empty,
                lv_color_hex(0x8b949e), LV_PART_MAIN);
        return;
    }

    char buf[56];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WARNING "  %d suspect device%s found!",
             found, found == 1 ? "" : "s");
    lv_label_set_text(skimmerStatusLbl, buf);
    lv_obj_set_style_text_color(skimmerStatusLbl,
        lv_color_hex(0xf85149), LV_PART_MAIN);

    for (int i = 0; i < found; i++) {
        char row[52];
        snprintf(row, sizeof(row), "%s  %s  %ddBm",
                 bleEntries[i].name,
                 bleEntries[i].mac,
                 bleEntries[i].rssi);
        lv_obj_t *entry = lv_list_add_text(skimmerList, row);
        if (entry)
            lv_obj_set_style_text_color(entry,
                lv_color_hex(0xf85149), LV_PART_MAIN);
    }
}

void createSkimmerScanner() {
    skimmerStatusLbl = nullptr;
    skimmerList      = nullptr;

    if (bleToolScreen) { lv_obj_delete(bleToolScreen); bleToolScreen = nullptr; }
    bleToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleToolScreen);
    createHeader(bleToolScreen, LV_SYMBOL_WARNING "  Skimmer Detector");

    skimmerStatusLbl = lv_label_create(bleToolScreen);
    lv_label_set_text(skimmerStatusLbl,
        "Scans for HC-03 / HC-05 / HC-06\n"
        "Bluetooth modules used in skimmers");
    lv_obj_set_style_text_color(skimmerStatusLbl,
        lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(skimmerStatusLbl, 8, 30);

    skimmerList = lv_list_create(bleToolScreen);
    lv_obj_set_size(skimmerList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(skimmerList, 0, 48);
    lv_obj_set_style_bg_color(skimmerList,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(skimmerList,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(skimmerList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(skimmerList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(skimmerList,      2, LV_PART_MAIN);

    lv_obj_t *backBtn = createBackBtn(bleToolScreen, cb_bleToolBack);
    lv_obj_t *scanBtn = createActionBtn(bleToolScreen,
                                        LV_SYMBOL_REFRESH "  Scan",
                                        cb_doSkimmerScan);

    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    lv_group_add_obj(bleToolGroup, backBtn);
    lv_group_add_obj(bleToolGroup, scanBtn);
    setGroup(bleToolGroup);

    lv_screen_load_anim(bleToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ══════════════════════════════════════════════════════════════════════════════════════════════
//  BLE TOOL 5 – META / RAYBAN DETECTOR
//
//  Detects Meta smart glasses (Ray-Ban Meta, Quest, etc.) by
//  checking BLE advertisements for Meta/Luxottica manufacturer IDs,
//  service UUIDs, and service-data UUIDs, while filtering out
//  common non-Meta identifiers (Apple, Samsung, Microsoft).
//
//  Identifier tables credit: https://github.com/justcallmekoko/ESP32Marauder/wiki/meta-detect
//  Key IDs: 0xFD5F, 0xFEB7, 0xFEB8, 0x01AB, 0x058E, 0x0D53
// ══════════════════════════════════════════════════════════════════════════════════════════════

static lv_obj_t *metaStatusLbl = nullptr;
static lv_obj_t *metaList      = nullptr;

static void cb_doMetaScan(lv_event_t *e) {
    lv_label_set_text(metaStatusLbl, LV_SYMBOL_REFRESH "  Scanning 8s...");
    lv_obj_set_style_text_color(metaStatusLbl, lv_color_hex(0xe3b341), LV_PART_MAIN);
    lv_obj_clean(metaList);
    lv_timer_handler();

    // Blue spinner for Meta (brand colour)
    startLEDSpinner(0, 100, 255);
    int found = doBLEScan(BLE_SCAN_SECS, BLE_META);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(metaStatusLbl,
            LV_SYMBOL_OK "  No Meta devices detected");
        lv_obj_set_style_text_color(metaStatusLbl,
            lv_color_hex(0x3fb950), LV_PART_MAIN);
        lv_obj_t *empty = lv_list_add_text(metaList,
            "No Ray-Ban / Quest in range");
        if (empty)
            lv_obj_set_style_text_color(empty,
                lv_color_hex(0x8b949e), LV_PART_MAIN);
        return;
    }

    char buf[56];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WARNING "  %d Meta device%s found!",
             found, found == 1 ? "" : "s");
    lv_label_set_text(metaStatusLbl, buf);
    lv_obj_set_style_text_color(metaStatusLbl,
        lv_color_hex(0x58a6ff), LV_PART_MAIN);

    for (int i = 0; i < found; i++) {
        char row[52];
        snprintf(row, sizeof(row), "%s  %s  %ddBm",
                 bleEntries[i].name[0] ? bleEntries[i].name : "<unknown>",
                 bleEntries[i].mac,
                 bleEntries[i].rssi);
        lv_obj_t *entry = lv_list_add_text(metaList, row);
        if (entry)
            lv_obj_set_style_text_color(entry,
                lv_color_hex(0x58a6ff), LV_PART_MAIN);
    }
}

void createMetaDetector() {
    metaStatusLbl = nullptr;
    metaList      = nullptr;

    if (bleToolScreen) { lv_obj_delete(bleToolScreen); bleToolScreen = nullptr; }
    bleToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleToolScreen);
    createHeader(bleToolScreen, LV_SYMBOL_EYE_OPEN "  Meta Detector");

    metaStatusLbl = lv_label_create(bleToolScreen);
    lv_label_set_text(metaStatusLbl,
        "Detects Ray-Ban Meta / Quest via\n"
        "BLE manufacturer & service UUIDs");
    lv_obj_set_style_text_color(metaStatusLbl,
        lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_pos(metaStatusLbl, 8, 30);

    metaList = lv_list_create(bleToolScreen);
    lv_obj_set_size(metaList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(metaList, 0, 48);
    lv_obj_set_style_bg_color(metaList,     lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(metaList,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(metaList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(metaList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(metaList,      2, LV_PART_MAIN);

    lv_obj_t *backBtn = createBackBtn(bleToolScreen, cb_bleToolBack);
    lv_obj_t *scanBtn = createActionBtn(bleToolScreen,
                                        LV_SYMBOL_REFRESH "  Scan",
                                        cb_doMetaScan);

    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    lv_group_add_obj(bleToolGroup, backBtn);
    lv_group_add_obj(bleToolGroup, scanBtn);
    setGroup(bleToolGroup);

    lv_screen_load_anim(bleToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    // Initialize GPS only if pins are configured
    #if HAS_GPS
        if (gpsIsConfigured()) {
            Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
            Serial.println("[GPS] External GPS module enabled");
        } else {
            Serial.println("[GPS] GPS support compiled but pins not configured");
        }
    #endif
    // SD card on dedicated HSPI bus — must not share with TFT
    sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    #if defined(POWER_PIN) && (POWER_PIN >= 0)
        pinMode(POWER_PIN, OUTPUT);
        digitalWrite(POWER_PIN, HIGH);
    #endif
    #if HAS_ENCODER
        pinMode(ENCODER_BTN, INPUT_PULLUP);
    #else
        // CYD devices: use BOOT button (GPIO0) as select
        pinMode(0, INPUT_PULLUP);
    #endif

    tft.begin();
    tft.writecommand(0x11);
    delay(120);
    tft.setRotation(1);
    // Backlight PWM via LEDC — allows smooth brightness control
    ledcAttach(LCD_BL_PIN, LCD_BL_FREQ, LCD_BL_RES);
    ledcWrite(LCD_BL_PIN, TFT_BL_INVERT ? (255 - lcdBrightness) : lcdBrightness);
    tft.fillScreen(TFT_BLACK);
#if HAS_CYD_TOUCH
    ts.setCal(495, 3398, 721, 3448, 320, 240, 1);
#endif
#if HAS_TOUCH
    uint16_t calData[5] = { 225, 3413, 403, 3334, 1 };
    tft.setTouch(calData);
#endif

    showSplashScreen();  // Splash Screen Call

    // Initialize WS2812 LED if present
    #if HAS_WS2812_LED
        ws2812Strip.begin();
        ws2812Strip.show();  // Initialize all pixels to 'off'
    #endif

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)millis(); });

    lvDisp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_flush_cb(lvDisp, lvgl_flush_cb);
    lv_display_set_buffers(lvDisp, lvBuf1, lvBuf2,
                           sizeof(lvBuf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lvIndev = lv_indev_create();
    #if HAS_TOUCH || HAS_CYD_TOUCH
        // Touch screen input
        lv_indev_set_type(lvIndev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(lvIndev, touch_read_cb);
        Serial.println("[Input] Touch screen enabled");
    #else
        // Encoder or button input
        lv_indev_set_type(lvIndev, LV_INDEV_TYPE_ENCODER);
        lv_indev_set_read_cb(lvIndev, encoder_read_cb);
        Serial.println("[Input] Encoder enabled");
    #endif

    ledStartupFlash();
    createMainMenu();

    Serial.println("[Rogue Radar] Boot complete.");
}

// ════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════

void loop() {
    // Feed all available GPS bytes into TinyGPS++ — non-blocking
    #if HAS_GPS
        if (gpsIsConfigured()) {
            while (Serial2.available())
                gps.encode(Serial2.read());
        }
    #endif

    lv_timer_handler();

    // Channel hopping: deauth detector and pwnagotchi watch both need it
    static unsigned long lastHop = 0;
    if ((deauthActive || pwnActive || flockActive) && (millis() - lastHop >= DEAUTH_HOP_MS)) {
        lastHop       = millis();
        deauthChannel = (deauthChannel % 13) + 1;
        esp_wifi_set_channel(deauthChannel, WIFI_SECOND_CHAN_NONE);
    }

    // Long-press power-off (hold encoder 5 s) - only for T-Embed
    #if HAS_ENCODER
    if (!powerOffTriggered) {
        if (digitalRead(ENCODER_BTN) == LOW) {
            if (btnHoldStart == 0) btnHoldStart = millis();
            if ((millis() - btnHoldStart) > POWER_HOLD_MS) {
                powerOffTriggered = true;
                if (deauthActive) {
                    deauthActive = false;
                    esp_wifi_set_promiscuous(false);
                }
                if (deauthTimer) { lv_timer_delete(deauthTimer); deauthTimer = nullptr; }

                lv_obj_t *offScr = lv_obj_create(nullptr);
                lv_obj_set_style_bg_color(offScr, lv_color_hex(0x000000), LV_PART_MAIN);
                lv_obj_t *msg = lv_label_create(offScr);
                lv_label_set_text(msg, LV_SYMBOL_POWER "  Powering off...");
                lv_obj_set_style_text_color(msg, lv_color_hex(0xff4444), LV_PART_MAIN);
                lv_obj_center(msg);
                lv_screen_load(offScr);
                lv_timer_handler();

                #if HAS_APA102_LED
                    setAllLEDs(0, 0, 0, 0);
                #endif
                delay(1500);
                #if defined(POWER_PIN) && (POWER_PIN >= 0)
                    digitalWrite(POWER_PIN, LOW);
                #endif
            }
        } else {
            btnHoldStart = 0;
        }
    }
    #endif

    delay(5);
}

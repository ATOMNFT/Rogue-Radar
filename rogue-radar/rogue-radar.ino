// ============================================================
//  Rogue Radar v1.0.2 Firmware
//  Check config.h for adjustable settings
// ============================================================
//
//  Tool Categories:
//  WiFi Tools: Network Scanner | Deauth Detector | Channel Analyzer
//              Packet Monitor | PineAP Hunter | Pwnagotchi Watch | Flock Detector | Flock Hybrid
//  BLE Tools:  BLE Scanner | AirTag Detector | Flipper Zero Detector | Tesla BLE Detector
//              nyanBOX Detector | Axon Detector | Skimmer Detector | Meta Detector
//  GPS Tools:  GPS Stats | Wiggle Wars
//  Misc Tools: Device Info | SD Update | Brightness ADJ | Themes | Dimming | Scan Times | LEDs | Detection Sounds | Menu Sounds | Rotation
//              Packet Monitor chan hopping |
//
//  Display / UI:
//  - ST7789 320x170 display
//  - LVGL-based menu system
//  - Rotary encoder navigation
//  - Splash screen support
//  - APA102 status LED support
//
//  Hardware Target:
//  - LilyGO T-Embed ESP32-S3
//
//  Arduino IDE Settings:
//  - Board: ESP32S3 Dev Module
//  - USB CDC On Boot: Disabled
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
//  Notes:
//  - GPS uses UART on pins defined in this sketch
//  - SD card uses HSPI pins defined in this sketch
//  - Encoder button and rotary input drive menu navigation
// ============================================================

#include <Arduino.h>
#include "config.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <RotaryEncoder.h>
#include <APA102.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <TinyGPS++.h>
#include <SPI.h>
#include <SD.h>
#include <Update.h>
#include <driver/i2s.h>
#include "splash.h"

static SPIClass sdSPI(HSPI);

static HardwareSerial gpsSerial(1);   // UART1 — free since BLE uses its own stack
static TinyGPSPlus    gps;

// ─── UI Themes ──────────────────────────────────────────────────
struct UITheme {
    const char *name;
    uint32_t bg;         // screen background
    uint32_t card;       // card / panel background
    uint32_t cardAlt;    // slightly lighter card (list buttons default)
    uint32_t border;     // borders
    uint32_t barBg;      // progress/RSSI bar track
    uint32_t text;       // primary text
    uint32_t textDim;    // secondary / inactive text
    uint32_t accent;     // header labels, data highlights
    uint32_t success;    // good/found state
    uint32_t warn;       // scanning / amber warning
    uint32_t alert;      // red alert
    uint32_t btnDefault; // back button default bg
    uint32_t btnFocus;   // focused button bg
    uint32_t btnPress;   // pressed button bg
    uint32_t actionBg;   // action button default bg
    uint32_t actionFoc;  // action button focused bg
    uint32_t actionBdr;  // action button border
    uint32_t flashGreen; // flash/confirm button
    uint32_t stopRed;    // stop button
};

static const UITheme THEMES[] = {
    { THEME_DARK     },
    { THEME_FLIPPER  },
    { THEME_MATRIX   },
    { THEME_POSEIDON },
    { THEME_PHANTOM  },
    { THEME_AMBER    },
    { THEME_TRON     },
};
static int currentTheme = DEFAULT_THEME;

// Convenience macros so tool code reads cleanly
#define TH       (THEMES[currentTheme])
#define TC(x)    lv_color_hex(TH.x)

// ─── Splash Duration — defined in config.h ──────────────────────
static const unsigned long SPLASH_TIME_MS = SPLASH_DURATION_MS;

TFT_eSPI tft = TFT_eSPI();

// ─── LVGL Buffers ───────────────────────────────────────────────
static lv_color_t lvBuf1[SCREEN_W * LV_BUF_LINES];
static lv_color_t lvBuf2[SCREEN_W * LV_BUF_LINES];
static lv_display_t *lvDisp  = nullptr;
static lv_indev_t   *lvIndev = nullptr;

// ─── Rotary Encoder ─────────────────────────────────────────────
RotaryEncoder encoder(ENCODER_A, ENCODER_B, RotaryEncoder::LatchMode::TWO03);

// ─── APA102 LEDs ────────────────────────────────────────────────
APA102<APA102_DI, APA102_CLK> ledStrip;
rgb_color ledBuf[NUM_LEDS];

struct MenuLED { uint8_t r, g, b; };
const MenuLED MENU_COLORS[4] = {
    LED_COLOR_WIFI,
    LED_COLOR_BLE,
    LED_COLOR_GPS,
    LED_COLOR_MISC
};

// ─── LED Spinner (FreeRTOS task on core 0) ───────────────────────
struct SpinnerColor { uint8_t r, g, b; };
static volatile bool  spinnerRunning     = false;
static TaskHandle_t   spinnerTaskHandle  = nullptr;
static SpinnerColor   spinnerColor       = {0, 200, 0};
static volatile uint16_t spinnerDelayMs   = 80;

// ─── Global UI State ────────────────────────────────────────────
static int            currentMenu       = 0;
static bool           powerOffTriggered = false;
static bool           powerButtonReleasedAfterBoot = false;  // safety: power-off hold is ignored until button is released once after boot
static unsigned long  btnHoldStart      = 0;
static int            lcdBrightness     = LCD_BL_DEFAULT;

// ─── Inactivity Dimmer State ───────────────────────────────────
// Safe first step for sleep-timer behavior: no ESP32 sleep modes yet.
// We only dim the TFT backlight and APA102 LED brightness after no
// encoder/button activity.
static unsigned long  lastActivityMs    = 0;
static bool           backlightDimmed   = false;
static bool           dimmingEnabled    = (DIMMING_ENABLED_DEFAULT != 0);
static bool           ledsEnabled       = (LEDS_ENABLED_DEFAULT != 0);
static lv_obj_t      *miscDimmingBtn    = nullptr;
static lv_obj_t      *miscLedsBtn       = nullptr;
static lv_obj_t      *miscSoundBtn      = nullptr;
static lv_obj_t      *miscMenuSoundBtn  = nullptr;
static lv_obj_t      *miscRotationBtn   = nullptr;

// ─── I2S Speaker / Alert Chirp State ───────────────────────────
static bool           soundEnabled      = (SOUND_ENABLED_DEFAULT != 0);
static bool           soundReady        = false;
static bool           menuFeedbackEnabled = (MENU_FEEDBACK_ENABLED_DEFAULT != 0);
static unsigned long  lastDeauthSoundMs = 0;
static unsigned long  lastFlockSoundMs  = 0;
static unsigned long  lastPwnSoundMs    = 0;
static unsigned long  lastFlipSoundMs   = 0;
static unsigned long  lastBleSusSoundMs = 0;
static unsigned long  lastMenuTickMs    = 0;
static unsigned long  lastMenuClickMs   = 0;

// Runtime display rotation. This is reset-after-reboot and only toggles
// between the two landscape orientations so the 320x170 LVGL layout stays safe.
static uint8_t displayRotation = DISPLAY_ROTATION_DEFAULT;

// ─── Runtime Scan Defaults ─────────────────────────────────────
// These start from config.h on each boot and reset after reboot.
static int bleScanSeconds  = BLE_SCAN_SECS;
static int wifiScanSeconds = WIFI_SCAN_SECS;
static int wifiMaxResults  = MAX_WIFI_RESULTS;
static int deauthHopMs     = DEAUTH_HOP_MS;

// Flock Hybrid runtime scan defaults. These reset after reboot.
static int flockHybridPresetIdx = FLOCK_HYBRID_PRESET_DEFAULT;
static int flockHybridBleSecs   = FLOCK_HYBRID_BLE_SECS;
static int flockHybridWifiSecs  = FLOCK_HYBRID_WIFI_SECS;
static int flockHybridHopMs     = FLOCK_HYBRID_WIFI_HOP_MS;

// Packet Monitor runtime state. Display-only first pass: no PCAP writes.
static bool          packetMonitorActive  = false;
static bool          packetMonitorHopEnabled = (PACKET_MONITOR_HOP_ENABLED_DEFAULT != 0);
static int           packetMonitorHopMs = PACKET_MONITOR_HOP_MS;
static uint8_t       packetMonitorChannel = PACKET_MONITOR_DEFAULT_CH;
static uint32_t      packetMonitorLastHopMs = 0;
static lv_timer_t   *packetMonitorTimer   = nullptr;
static lv_obj_t     *packetMonStatusLbl   = nullptr;
static lv_obj_t     *packetMonStatsLbl    = nullptr;
static lv_obj_t     *packetMonGraphArea   = nullptr;
static lv_obj_t     *packetMonStartBtn    = nullptr;
static lv_obj_t     *packetMonStartLbl    = nullptr;

static volatile uint32_t packetMonTotalPackets = 0;
static volatile uint32_t packetMonMgmtPackets  = 0;
static volatile uint32_t packetMonDataPackets  = 0;
static volatile uint32_t packetMonCtrlPackets  = 0;
static volatile int32_t  packetMonRssiSum      = 0;

static uint32_t packetMonLastTotal   = 0;
static uint32_t packetMonLastRssiCnt = 0;
static int32_t  packetMonLastRssiSum = 0;
static uint32_t packetMonLastUpdate  = 0;
static uint16_t packetMonRateSamples[PACKET_MONITOR_GRAPH_BARS] = {0};
static uint8_t  packetMonSampleHead = 0;

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
static int           deauthSoundedTotal = 0;
static volatile bool deauthActive  = false;
static uint8_t       deauthChannel = 1;

// ── Channel Analyzer ────────────────────────────────────────────
static int    chanNetCount[14];
static int8_t chanMaxRSSI[14];

// LVGL timer handle for deauth refresh
static lv_timer_t *deauthTimer = nullptr;

// ── PineAP Hunter ───────────────────────────────────────────────
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

// ── Pwnagotchi Detector ─────────────────────────────────────────
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

// ── Flock Safety Detector ────────────────────────────────────────
//
//  Flock/Penguin-style devices may advertise WiFi SSIDs containing
//  keywords such as "flock", "penguin", "pigvision", or
//  "fs ext battery". We watch beacon frames (subtype 8), probe
//  responses (subtype 5), and probe requests (subtype 4) for those
//  substrings. Alerts are latching — stays red until tool is exited.
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

// ── Flock Hybrid Scanner ───────────────────────────────────────
//  Combined BLE + WiFi Flock scan. This intentionally alternates phases
//  instead of running BLE and WiFi promiscuous mode at the same time.
struct FlockHybridHit {
    char     source[6];   // "BLE" or "WiFi"
    char     name[33];    // BLE name or WiFi SSID
    char     mac[18];
    char     reason[24];
    int8_t   rssi;
    uint32_t lastSeen;
};

static FlockHybridHit hybridHits[MAX_FLOCK_HYBRID_HITS];
static int            hybridHitCount = 0;
static volatile bool  hybridWifiActive = false;
static lv_obj_t      *hybridStatusLbl = nullptr;
static lv_obj_t      *hybridList      = nullptr;
static lv_timer_t    *hybridStartTimer = nullptr;

// ISR pending slot for the WiFi half of the hybrid scanner
static char             hybridPendingName[33];
static char             hybridPendingMac[18];
static char             hybridPendingReason[24];
static volatile int8_t  hybridPendingRSSI = 0;
static volatile bool    hybridPendingReady = false;

// ════════════════════════════════════════════════════════════════
//  BLE DATA STRUCTURES
// ════════════════════════════════════════════════════════════════
// Device type flags for scanner result colouring / filtering
enum BLEDeviceType {
    BLE_GENERIC  = 0,
    BLE_AIRTAG   = 1,
    BLE_FLIPPER  = 2,
    BLE_APPLE    = 3,    // Apple but not an AirTag
    BLE_SKIMMER  = 4,    // HC-03/HC-05/HC-06 skimmer module
    BLE_META     = 5,    // Meta/RayBan smart glasses
    BLE_NYANBOX  = 6,    // nyanBOX / Nyan Devices badge
    BLE_AXON     = 7,    // Axon-style BLE device by MAC/OUI prefix
    BLE_TESLA    = 8     // Tesla BLE name-pattern detector
};

struct BLEEntry {
    char          name[33];    // advertised local name (or "<unknown>")
    char          mac[18];     // XX:XX:XX:XX:XX:XX
    int8_t        rssi;
    BLEDeviceType type;
    char          mfgHint[14]; // short manufacturer hint for list row
    char          flipperColor[13]; // Black / White / Transparent / Unknown
};

static BLEEntry bleEntries[MAX_BLE_RESULTS];
static int      bleEntryCount  = 0;
static bool     bleInitialized = false;  // BLEDevice::init() once only

// nyanBOX detector entries. Fixed array keeps memory predictable on ESP32-S3.
struct NyanBoxEntry {
    char     name[33];
    char     mac[18];
    int8_t   rssi;
    uint16_t level;
    char     version[16];
    uint32_t lastSeen;
};

static NyanBoxEntry nyanEntries[MAX_NYANBOX_RESULTS];
static int          nyanEntryCount = 0;

// Axon detector entries. Fixed array keeps memory predictable on ESP32-S3.
struct AxonEntry {
    char     name[33];
    char     mac[18];
    int8_t   rssi;
    uint32_t lastSeen;
};

static AxonEntry axonEntries[MAX_AXON_RESULTS];
static int       axonEntryCount = 0;

// Tesla detector entries. Fixed array keeps memory predictable on ESP32-S3.
struct TeslaEntry {
    char     name[33];
    char     mac[18];
    int8_t   rssi;
    uint32_t lastSeen;
};

static TeslaEntry teslaEntries[MAX_TESLA_RESULTS];
static int        teslaEntryCount = 0;


// ════════════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════
void createMainMenu();
void createWiFiMenu();
void createNetworkScanner();
void createNetworkDetail(int idx);
void createDeauthDetector();
void createChannelAnalyzer();
void createPacketMonitor();
void createPineAPHunter();
void createPineAPDetail(int idx);
void createPwnagotchiDetector();
void createFlockDetector();
void createFlockHybridScanner();
void createSubScreen(int idx);
void createMiscMenu();
void createDeviceInfo();
void createSDUpdate();
void createBrightnessControl();
void createThemePicker();
void createScanDefaults();
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
void createNyanBoxDetector();
void createNyanBoxDetail(int idx);
void createNyanBoxLocate(int idx);
void createAxonDetector();
void createAxonDetail(int idx);
void createAxonLocate(int idx);
void createTeslaDetector();
void createTeslaDetail(int idx);
void createSkimmerScanner();
void createMetaDetector();
static void cb_bleToolBack(lv_event_t *e);
static void cb_bleDetailBack(lv_event_t *e);

// ════════════════════════════════════════════════════════════════
//  INACTIVITY BACKLIGHT + APA102 LED DIMMER
// ════════════════════════════════════════════════════════════════
static void applyBacklightLevel(uint8_t level) {
    ledcWrite(LCD_BL_CH, level);
}

static uint8_t activeLedBrightness(uint8_t requestedBrightness = LED_BRIGHTNESS) {
    // Runtime LED toggle: keep the APA102 ring dark while preserving
    // the last requested colour/status internally.
    if (!ledsEnabled) {
        return 0;
    }

    // Keep explicit OFF requests off, but cap normal/status brightness while dimming is enabled and active.
    if (dimmingEnabled && backlightDimmed && requestedBrightness > LED_DIM_BRIGHTNESS) {
        return LED_DIM_BRIGHTNESS;
    }
    return requestedBrightness;
}

static void refreshCurrentLEDs(uint8_t requestedBrightness = LED_BRIGHTNESS) {
    ledStrip.write(ledBuf, NUM_LEDS, activeLedBrightness(requestedBrightness));
}

static void resetInactivityTimer() {
    lastActivityMs = millis();

    // If the display/LEDs were dimmed, restore them on the first encoder
    // movement or button press.
    if (backlightDimmed) {
        backlightDimmed = false;
        applyBacklightLevel((uint8_t)lcdBrightness);
        refreshCurrentLEDs(LED_BRIGHTNESS);
    }
}

static void updateInactivityDimmer() {
    // Runtime OFF means no dimming at all. If the device was already dimmed, wake it back up.
    if (!dimmingEnabled) {
        if (backlightDimmed) {
            backlightDimmed = false;
            applyBacklightLevel((uint8_t)lcdBrightness);
            refreshCurrentLEDs(LED_BRIGHTNESS);
        }
        return;
    }

#if INACTIVITY_DIM_TIMEOUT_MS > 0
    if (!backlightDimmed && (millis() - lastActivityMs >= INACTIVITY_DIM_TIMEOUT_MS)) {
        backlightDimmed = true;
        applyBacklightLevel((uint8_t)INACTIVITY_DIM_LEVEL);
        refreshCurrentLEDs(LED_BRIGHTNESS);
    }
#endif
}


// ════════════════════════════════════════════════════════════════
//  I2S SPEAKER ALERT CHIRPS
//
//  T-Embed uses an I2S speaker path, so these short chirps are generated
//  directly as small PCM tone bursts. No audio files, SD access, or ESP32
//  sleep modes are involved.
// ════════════════════════════════════════════════════════════════
#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S I2S_COMM_FORMAT_I2S
#endif

static bool soundCooldownReady(unsigned long &lastMs) {
    unsigned long now = millis();
    if (now - lastMs < SOUND_ALERT_COOLDOWN_MS) return false;
    lastMs = now;
    return true;
}

static void initSound() {
    if (soundReady) return;

    i2s_config_t i2sConfig = {};
    i2sConfig.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2sConfig.sample_rate = SOUND_SAMPLE_RATE;
    i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2sConfig.intr_alloc_flags = 0;
    i2sConfig.dma_buf_count = 4;
    i2sConfig.dma_buf_len = 64;
    i2sConfig.use_apll = false;
    i2sConfig.tx_desc_auto_clear = true;
    i2sConfig.fixed_mclk = 0;

    if (i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, nullptr) != ESP_OK) {
        Serial.println("[Sound] I2S driver install failed");
        soundReady = false;
        return;
    }

    i2s_pin_config_t pinConfig = {};
    pinConfig.bck_io_num = SOUND_I2S_BCLK;
    pinConfig.ws_io_num = SOUND_I2S_WCLK;
    pinConfig.data_out_num = SOUND_I2S_DOUT;
    pinConfig.data_in_num = I2S_PIN_NO_CHANGE;

    if (i2s_set_pin(I2S_NUM_0, &pinConfig) != ESP_OK) {
        Serial.println("[Sound] I2S pin setup failed");
        i2s_driver_uninstall(I2S_NUM_0);
        soundReady = false;
        return;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
    soundReady = true;
    Serial.println("[Sound] I2S alert chirps ready");
}

static bool ensureSoundReady(bool requireAlertSoundEnabled = true) {
    // Lazy init: do not start I2S during boot.
    // Detection alert chirps obey Misc > Sound ON/OFF.
    // Menu feedback has its own config setting and can initialize I2S independently.
    if (requireAlertSoundEnabled && !soundEnabled) return false;
    if (!soundReady) initSound();
    return soundReady;
}

static void soundTone(uint16_t freqHz, uint16_t durationMs, uint8_t volumePct = SOUND_VOLUME_PERCENT, bool requireAlertSoundEnabled = true) {
    if (freqHz == 0 || durationMs == 0) return;
    if (!ensureSoundReady(requireAlertSoundEnabled)) return;

    if (volumePct > 100) volumePct = 100;
    int16_t amp = (int16_t)((12000L * volumePct) / 100L);
    if (amp < 300) amp = 300;

    const uint16_t framesPerChunk = 64;
    int16_t samples[framesPerChunk * 2]; // stereo: L/R
    uint32_t totalFrames = ((uint32_t)SOUND_SAMPLE_RATE * durationMs) / 1000UL;
    uint32_t halfPeriod = SOUND_SAMPLE_RATE / ((uint32_t)freqHz * 2UL);
    if (halfPeriod == 0) halfPeriod = 1;

    uint32_t frameIndex = 0;
    while (frameIndex < totalFrames) {
        uint16_t frames = framesPerChunk;
        if (totalFrames - frameIndex < frames) frames = totalFrames - frameIndex;

        for (uint16_t i = 0; i < frames; i++) {
            int16_t sample = (((frameIndex + i) / halfPeriod) & 1) ? amp : -amp;
            samples[i * 2]     = sample;
            samples[i * 2 + 1] = sample;
        }

        size_t bytesWritten = 0;
        i2s_write(I2S_NUM_0, samples, frames * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
        frameIndex += frames;
    }
}

static void soundSilence(uint16_t durationMs) {
    if (!soundReady || durationMs == 0) return;
    int16_t zeros[64 * 2] = {0};
    uint32_t totalFrames = ((uint32_t)SOUND_SAMPLE_RATE * durationMs) / 1000UL;
    while (totalFrames > 0) {
        uint16_t frames = totalFrames > 64 ? 64 : totalFrames;
        size_t bytesWritten = 0;
        i2s_write(I2S_NUM_0, zeros, frames * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
        totalFrames -= frames;
    }
}

static void stopSoundDriverAfterChirp() {
    if (!soundReady) return;

    // The T-Embed encoder button uses GPIO0. On this board, leaving the
    // I2S driver active after a chirp can make the button read as held,
    // which triggers the existing long-press power-off path. Since these
    // are only short alert chirps, shut I2S back down after each chirp.
    soundSilence(25);
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    soundReady = false;

    // Re-assert the encoder button input mode and require a fresh release
    // before any long-press power-off can start counting again.
    pinMode(ENCODER_BTN, INPUT_PULLUP);
    btnHoldStart = 0;
    powerButtonReleasedAfterBoot = false;
}

static void playDeauthChirp() {
    if (!soundCooldownReady(lastDeauthSoundMs)) return;
    soundTone(2100, 65, SOUND_VOLUME_PERCENT);
    soundSilence(45);
    soundTone(2100, 65, SOUND_VOLUME_PERCENT);
    stopSoundDriverAfterChirp();
}

static void playFlockChirp() {
    if (!soundCooldownReady(lastFlockSoundMs)) return;
    soundTone(520, 170, SOUND_VOLUME_PERCENT);
    soundSilence(50);
    soundTone(390, 210, SOUND_VOLUME_PERCENT);
    stopSoundDriverAfterChirp();
}

static void playPwnagotchiChirp() {
    if (!soundCooldownReady(lastPwnSoundMs)) return;
    soundTone(880, 80, SOUND_VOLUME_PERCENT);
    soundSilence(35);
    soundTone(1175, 80, SOUND_VOLUME_PERCENT);
    soundSilence(35);
    soundTone(1568, 110, SOUND_VOLUME_PERCENT);
    stopSoundDriverAfterChirp();
}

static void playFlipperChirp() {
    if (!soundCooldownReady(lastFlipSoundMs)) return;
    soundTone(1200, 55, SOUND_VOLUME_PERCENT);
    soundSilence(30);
    soundTone(1600, 55, SOUND_VOLUME_PERCENT);
    soundSilence(30);
    soundTone(1000, 70, SOUND_VOLUME_PERCENT);
    stopSoundDriverAfterChirp();
}

static void playBLESuspiciousChirp() {
    if (!soundCooldownReady(lastBleSusSoundMs)) return;
    soundTone(430, 120, SOUND_VOLUME_PERCENT);
    soundSilence(80);
    soundTone(430, 120, SOUND_VOLUME_PERCENT);
    stopSoundDriverAfterChirp();
}

static bool menuFeedbackCooldownReady(unsigned long &lastMs, uint16_t cooldownMs) {
    unsigned long now = millis();
    if (now - lastMs < cooldownMs) return false;
    lastMs = now;
    return true;
}

static void playMenuTickFeedback() {
    if (!menuFeedbackEnabled) return;
    if (!menuFeedbackCooldownReady(lastMenuTickMs, MENU_FEEDBACK_TICK_COOLDOWN_MS)) return;

    // Very short, quiet tick for encoder movement.
    // This intentionally does not obey Misc > Sound ON/OFF; use
    // Misc > Menu Sounds ON/OFF or MENU_FEEDBACK_ENABLED_DEFAULT to disable it.
    soundTone(1450, 8, MENU_FEEDBACK_VOLUME_PERCENT, false);
    stopSoundDriverAfterChirp();
}

static void playMenuClickFeedback() {
    if (!menuFeedbackEnabled) return;
    if (!menuFeedbackCooldownReady(lastMenuClickMs, 120)) return;

    // Short click for button/select actions.
    soundTone(950, 18, MENU_FEEDBACK_VOLUME_PERCENT, false);
    stopSoundDriverAfterChirp();
}

static const char *getSoundMenuLabel() {
    static char label[40];
    snprintf(label, sizeof(label), LV_SYMBOL_BELL " Alert Sound: %s",
             soundEnabled ? "ON" : "OFF");
    return label;
}

static void updateSoundMenuLabel() {
    if (!miscSoundBtn) return;

    lv_obj_t *label = lv_obj_get_child(miscSoundBtn, 0);
    if (label) {
        lv_label_set_text(label, getSoundMenuLabel());
    }
}

static void toggleSoundEnabled() {
    soundEnabled = !soundEnabled;
    resetInactivityTimer();

    if (!soundEnabled && soundReady) {
        stopSoundDriverAfterChirp();
    }
    // Do not call initSound() here. I2S is intentionally lazy-initialized
    // by the first alert chirp so boot/menu input stays stable.

    updateSoundMenuLabel();
}

static const char *getMenuSoundMenuLabel() {
    static char label[44];
    snprintf(label, sizeof(label), LV_SYMBOL_AUDIO "  Menu Sounds: %s",
             menuFeedbackEnabled ? "ON" : "OFF");
    return label;
}

static void updateMenuSoundMenuLabel() {
    if (!miscMenuSoundBtn) return;

    lv_obj_t *label = lv_obj_get_child(miscMenuSoundBtn, 0);
    if (label) {
        lv_label_set_text(label, getMenuSoundMenuLabel());
    }
}

static void toggleMenuFeedbackEnabled() {
    menuFeedbackEnabled = !menuFeedbackEnabled;
    resetInactivityTimer();

    // If a feedback tone somehow left the I2S driver active, shut it down
    // before leaving the toggle. This keeps the GPIO0 power button stable.
    if (!menuFeedbackEnabled && soundReady) {
        stopSoundDriverAfterChirp();
    }

    updateMenuSoundMenuLabel();
}

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
    encoder.tick();
    int pos = encoder.getPosition();
    bool pressed = (digitalRead(ENCODER_BTN) == LOW);

    // Any encoder movement or button press counts as activity.
    // This wakes the backlight before the UI action continues.
    if (pos != 0 || pressed) {
        resetInactivityTimer();
    }

    // Optional menu feedback sounds. These are intentionally short and
    // shut I2S back down after each tick/click to avoid the GPIO0
    // false power-off issue seen when I2S stayed active.
    static bool wasPressed = false;
    if (pos != 0) {
        playMenuTickFeedback();
    }
    if (pressed && !wasPressed) {
        playMenuClickFeedback();
    }
    wasPressed = pressed;

    data->enc_diff = (int16_t)pos;
    if (pos != 0) encoder.setPosition(0);
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ════════════════════════════════════════════════════════════════
//  Splash Screen
// ════════════════════════════════════════════════════════════════
void showSplashScreen() {
    tft.fillScreen(TFT_BLACK);

    tft.setSwapBytes(true);
    // If image matches screen exactly:
    tft.pushImage(0, 0, SCREEN_W, SCREEN_H, splash);

    // Or centered if smaller
    // tft.pushImage((SCREEN_W - SPLASH_W)/2, (SCREEN_H - SPLASH_H)/2, SPLASH_W, SPLASH_H, splash);

    delay(SPLASH_TIME_MS); 
}

// ════════════════════════════════════════════════════════════════
//  LED HELPERS
// ════════════════════════════════════════════════════════════════
void setAllLEDs(uint8_t r, uint8_t g, uint8_t b,
                uint8_t br = LED_BRIGHTNESS)
{
    for (int i = 0; i < NUM_LEDS; i++) ledBuf[i] = {r, g, b};
    refreshCurrentLEDs(br);
}

void ledStartupFlash() {
    setAllLEDs(255, 255, 255, 10); delay(300);
    setAllLEDs(0, 0, 0, 0);       delay(150);
    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
}
// ── LED Spinner Task ─────────────────────────────────────────────
//  Runs on core 0 while core 1 blocks on a WiFi scan.
//  Produces a comet-tail chase around the 7-LED ring:
//    Head : full colour
//    Mid  : 35% dimmed
//    Tail : 12% dimmed
//    Rest : off
static void ledSpinnerTask(void *param) {
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

        ledStrip.write(frame, NUM_LEDS, activeLedBrightness(LED_BRIGHTNESS));
        pos = (pos + 1) % NUM_LEDS;
        vTaskDelay(pdMS_TO_TICKS(spinnerDelayMs));
    }

    spinnerTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

// Start the spinner with a given accent colour (call before blocking scan)
void startLEDSpinner(uint8_t r, uint8_t g, uint8_t b, uint16_t delayMs = 80) {
    if (spinnerRunning || spinnerTaskHandle != nullptr) return;
    spinnerColor   = {r, g, b};
    spinnerDelayMs = delayMs;
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
    lv_obj_set_style_bg_color(scr, TC(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr,   LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *createHeader(lv_obj_t *parent, const char *text) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SCREEN_W, 28);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar,     TC(card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar,       LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar,       0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, TC(accent), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
    return bar;
}

static lv_obj_t *createBackBtn(lv_obj_t *parent, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 100, 26);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 6, -4);
    lv_obj_set_style_bg_color(btn, TC(btnDefault), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, TC(btnFocus),   LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, TC(btnPress),   LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, TC(border), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 5, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_color(lbl, TC(text), LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t *createActionBtn(lv_obj_t *parent,
                                  const char *label,
                                  lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 110, 26);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
    lv_obj_set_style_bg_color(btn, TC(actionBg),  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, TC(actionFoc), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, TC(success),   LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, TC(actionBdr), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 5, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, TC(text), LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

static void styleListBtn(lv_obj_t *btn) {
    lv_obj_set_height(btn, 26);
    lv_obj_set_style_bg_color(btn,   TC(cardAlt),  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn,     LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, TC(text),     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn,   TC(btnFocus), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn,     LV_OPA_COVER, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(btn, TC(accent), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn, 1,           LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn,   TC(btnPress), LV_PART_MAIN | LV_STATE_PRESSED);
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
    { LV_SYMBOL_GPS,       "GPS Tools",   LV_SYMBOL_GPS       "  GPS Tools"  },
    { LV_SYMBOL_SETTINGS,  "Misc Tools",  LV_SYMBOL_SETTINGS  "  Misc Tools" },
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
    else if (idx == 2) createGPSMenu();
    else if (idx == 3) createMiscMenu();
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
    lv_obj_set_style_bg_color(list,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list,      6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list,      4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(TH.accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);

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
    lv_obj_set_style_text_color(msg, lv_color_hex(TH.textDim), LV_PART_MAIN);
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
static const char *MISC_TOOL_LABELS[5] = {
    LV_SYMBOL_SETTINGS "  Device Info",
    LV_SYMBOL_UPLOAD   "  SD Update",
    LV_SYMBOL_IMAGE    "  Brightness",
    LV_SYMBOL_EDIT     "  Themes",
    LV_SYMBOL_SETTINGS     "  Scan Defaults"
};


static bool isDisplayRotationFlipped() {
    return displayRotation == DISPLAY_ROTATION_FLIPPED;
}

static const char *getRotationMenuLabel() {
    static char label[40];
    snprintf(label, sizeof(label), LV_SYMBOL_REFRESH "  Rotation: %s",
             isDisplayRotationFlipped() ? "Flipped" : "Normal");
    return label;
}

static void updateRotationMenuLabel() {
    if (!miscRotationBtn) return;

    lv_obj_t *label = lv_obj_get_child(miscRotationBtn, 0);
    if (label) {
        lv_label_set_text(label, getRotationMenuLabel());
    }
}

static void applyDisplayRotation(bool redrawNow) {
    // Keep LVGL's resolution fixed at SCREEN_W x SCREEN_H. We only allow
    // landscape rotations, so the layout dimensions remain unchanged.
    tft.setRotation(displayRotation);

    if (lvDisp) {
        lv_display_set_resolution(lvDisp, SCREEN_W, SCREEN_H);

        lv_obj_t *active = lv_screen_active();
        if (active) {
            lv_obj_invalidate(active);
        }

        if (redrawNow) {
            tft.fillScreen(TFT_BLACK);
            lv_refr_now(lvDisp);
        }
    }
}

static void toggleDisplayRotation() {
    displayRotation = isDisplayRotationFlipped()
                    ? DISPLAY_ROTATION_NORMAL
                    : DISPLAY_ROTATION_FLIPPED;

    resetInactivityTimer();
    applyDisplayRotation(true);
    updateRotationMenuLabel();
}

static const char *getDimmingMenuLabel() {
    static char label[32];
    snprintf(label, sizeof(label), LV_SYMBOL_IMAGE "  Dimming: %s",
             dimmingEnabled ? "ON" : "OFF");
    return label;
}

static void updateDimmingMenuLabel() {
    if (!miscDimmingBtn) return;

    lv_obj_t *label = lv_obj_get_child(miscDimmingBtn, 0);
    if (label) {
        lv_label_set_text(label, getDimmingMenuLabel());
    }
}

static void toggleDimmingEnabled() {
    dimmingEnabled = !dimmingEnabled;

    // Toggling counts as activity. If dimming was turned off while already dimmed,
    // this immediately restores the TFT backlight and APA102 LED brightness.
    resetInactivityTimer();
    updateDimmingMenuLabel();
}

static const char *getLedsMenuLabel() {
    static char label[32];
    snprintf(label, sizeof(label), LV_SYMBOL_IMAGE "  LEDs: %s",
             ledsEnabled ? "ON" : "OFF");
    return label;
}

static void updateLedsMenuLabel() {
    if (!miscLedsBtn) return;

    lv_obj_t *label = lv_obj_get_child(miscLedsBtn, 0);
    if (label) {
        lv_label_set_text(label, getLedsMenuLabel());
    }
}

static void toggleLedsEnabled() {
    ledsEnabled = !ledsEnabled;

    // Toggling counts as user activity and immediately applies the new
    // APA102 visibility state to the current stored ring colour.
    resetInactivityTimer();
    refreshCurrentLEDs(LED_BRIGHTNESS);
    updateLedsMenuLabel();
}

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
        case 0: createDeviceInfo();        break;
        case 1: createSDUpdate();          break;
        case 2: createBrightnessControl(); break;
        case 3: createThemePicker();       break;
        case 4: createScanDefaults();      break;
        case 5: toggleDimmingEnabled();    break;
        case 6: toggleLedsEnabled();       break;
        case 7: toggleSoundEnabled();      break;
        case 8: toggleMenuFeedbackEnabled(); break;
        case 9: toggleDisplayRotation();   break;
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
    lv_obj_set_style_bg_color(list,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(list,      6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_row(list,      4,                      LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(TH.accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);

    deleteGroup(&miscMenuGroup);
    miscMenuGroup = lv_group_create();
    miscDimmingBtn = nullptr;
    miscLedsBtn = nullptr;
    miscSoundBtn = nullptr;
    miscMenuSoundBtn = nullptr;
    miscRotationBtn = nullptr;

    for (int i = 0; i < 10; i++) {
        const char *label = nullptr;
        if (i < 5) {
            label = MISC_TOOL_LABELS[i];
        } else if (i == 5) {
            label = getDimmingMenuLabel();
        } else if (i == 6) {
            label = getLedsMenuLabel();
        } else if (i == 7) {
            label = getSoundMenuLabel();
        } else if (i == 8) {
            label = getMenuSoundMenuLabel();
        } else {
            label = getRotationMenuLabel();
        }
        lv_obj_t *btn = lv_list_add_btn(list, nullptr, label);
        styleListBtn(btn);
        lv_obj_set_height(btn, 30);
        lv_obj_add_event_cb(btn, cb_miscToolSelected, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_group_add_obj(miscMenuGroup, btn);

        if (i == 5) {
            miscDimmingBtn = btn;
        } else if (i == 6) {
            miscLedsBtn = btn;
        } else if (i == 7) {
            miscSoundBtn = btn;
        } else if (i == 8) {
            miscMenuSoundBtn = btn;
        } else if (i == 9) {
            miscRotationBtn = btn;
        }
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
    lv_obj_set_style_bg_color(card,     lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1,                      LV_PART_MAIN);
    lv_obj_set_style_radius(card,       6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,      6,                      LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Gather device info at run time
    uint32_t heap     = ESP.getFreeHeap();
    uint32_t flash    = ESP.getFlashChipSize() / 1024;  // KB
    uint32_t flashSpd = ESP.getFlashChipSpeed() / 1000000; // MHz
    uint8_t cores     = ESP.getChipCores();
    uint32_t cpuMHz   = ESP.getCpuFreqMHz();
    const char *firmwareVer = FIRMWARE_VERSION;

    // WiFi MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char info[320];
    snprintf(info, sizeof(info),
             "FW   : %s\n"
             "Chip : ESP32-S3  (%d cores)\n"
             "CPU  : %lu MHz\n"
             "Flash: %lu KB @ %lu MHz\n"
             "Heap : %lu B free\n"
             "MAC  : %s",
             firmwareVer,
             cores,
             (unsigned long)cpuMHz,
             (unsigned long)flash,
             (unsigned long)flashSpd,
             (unsigned long)heap,
             macStr);
    lv_obj_t *infoLbl = lv_label_create(card);
    lv_label_set_text(infoLbl, info);
    lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(infoLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(TH.text), LV_PART_MAIN);
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
        otaSetStatus(LV_SYMBOL_CLOSE "  SD card not found!", TH.alert);
        lv_obj_remove_state(otaFlashBtn, LV_STATE_DISABLED);
        return;
    }

    File f = SD.open(OTA_FILENAME, FILE_READ);
    if (!f) {
        otaSetStatus(LV_SYMBOL_CLOSE "  firmware.bin not found!", TH.alert);
        SD.end();
        lv_obj_remove_state(otaFlashBtn, LV_STATE_DISABLED);
        return;
    }

    size_t fileSize = f.size();
    if (fileSize == 0) {
        otaSetStatus(LV_SYMBOL_CLOSE "  firmware.bin is empty!", TH.alert);
        f.close();
        SD.end();
        lv_obj_remove_state(otaFlashBtn, LV_STATE_DISABLED);
        return;
    }

    otaSetStatus(LV_SYMBOL_REFRESH "  Flashing...", TH.warn);

    if (!Update.begin(fileSize, U_FLASH)) {
        otaSetStatus(LV_SYMBOL_CLOSE "  Update.begin() failed!", TH.alert);
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
            otaSetStatus(LV_SYMBOL_CLOSE "  Write error — aborted!", TH.alert);
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
        otaSetStatus(LV_SYMBOL_CLOSE "  Verification failed!", TH.alert);
        lv_obj_remove_state(otaFlashBtn, LV_STATE_DISABLED);
        return;
    }

    lv_bar_set_value(otaBar, 100, LV_ANIM_OFF);
    lv_label_set_text(otaPctLbl, "100%");
    otaSetStatus(LV_SYMBOL_OK "  Done! Rebooting...", TH.success);
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
        "Place update.bin in SD root\n"
        "then press Flash.");
    lv_obj_set_style_text_color(otaStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(otaStatusLbl, 8, 30);

    otaBar = lv_bar_create(miscToolScreen);
    lv_obj_set_size(otaBar, SCREEN_W - 16, 10);
    lv_obj_set_pos(otaBar, 8, 78);
    lv_bar_set_range(otaBar, 0, 100);
    lv_bar_set_value(otaBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(otaBar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(otaBar, lv_color_hex(TH.success), LV_PART_INDICATOR);
    lv_obj_set_style_radius(otaBar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(otaBar, 3, LV_PART_INDICATOR);

    otaPctLbl = lv_label_create(miscToolScreen);
    lv_label_set_text(otaPctLbl, "");
    lv_obj_set_style_text_color(otaPctLbl, lv_color_hex(TH.success), LV_PART_MAIN);
    lv_obj_set_pos(otaPctLbl, 8, 92);

    lv_obj_t *backBtn = createBackBtn(miscToolScreen, cb_miscToolBack);

    otaFlashBtn = lv_btn_create(miscToolScreen);
    lv_obj_set_size(otaFlashBtn, 90, 28);
    lv_obj_align(otaFlashBtn, LV_ALIGN_BOTTOM_MID, 30, -4);
    lv_obj_set_style_bg_color(otaFlashBtn, lv_color_hex(TH.flashGreen), LV_PART_MAIN);
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
//  MISC TOOL – SCAN DEFAULTS
//
//  Session-only settings. Values start from config.h on boot and reset
//  after reboot. No Preferences/NVS writes are used here.
// ════════════════════════════════════════════════════════════════
static lv_obj_t *scanDefaultsBleBtn    = nullptr;
static lv_obj_t *scanDefaultsWifiBtn   = nullptr;
static lv_obj_t *scanDefaultsResultBtn = nullptr;
static lv_obj_t *scanDefaultsHopBtn       = nullptr;
static lv_obj_t *scanDefaultsHybridBtn    = nullptr;
static lv_obj_t *scanDefaultsPacketHopBtn = nullptr;
static lv_obj_t *scanDefaultsPacketMsBtn  = nullptr;

struct FlockHybridPreset {
    const char *name;
    int bleSecs;
    int wifiSecs;
    int hopMs;
};

static const FlockHybridPreset FLOCK_HYBRID_PRESETS[] = {
    { FLOCK_HYBRID_PRESET_0_NAME, FLOCK_HYBRID_PRESET_0_BLE, FLOCK_HYBRID_PRESET_0_WIFI, FLOCK_HYBRID_PRESET_0_HOP },
    { FLOCK_HYBRID_PRESET_1_NAME, FLOCK_HYBRID_PRESET_1_BLE, FLOCK_HYBRID_PRESET_1_WIFI, FLOCK_HYBRID_PRESET_1_HOP },
    { FLOCK_HYBRID_PRESET_2_NAME, FLOCK_HYBRID_PRESET_2_BLE, FLOCK_HYBRID_PRESET_2_WIFI, FLOCK_HYBRID_PRESET_2_HOP },
    { FLOCK_HYBRID_PRESET_3_NAME, FLOCK_HYBRID_PRESET_3_BLE, FLOCK_HYBRID_PRESET_3_WIFI, FLOCK_HYBRID_PRESET_3_HOP },
    { FLOCK_HYBRID_PRESET_4_NAME, FLOCK_HYBRID_PRESET_4_BLE, FLOCK_HYBRID_PRESET_4_WIFI, FLOCK_HYBRID_PRESET_4_HOP }
};

static const int FLOCK_HYBRID_PRESET_COUNT = sizeof(FLOCK_HYBRID_PRESETS) / sizeof(FLOCK_HYBRID_PRESETS[0]);

static void applyFlockHybridPreset() {
    if (flockHybridPresetIdx < 0 || flockHybridPresetIdx >= FLOCK_HYBRID_PRESET_COUNT) {
        flockHybridPresetIdx = 0;
    }

    flockHybridBleSecs  = FLOCK_HYBRID_PRESETS[flockHybridPresetIdx].bleSecs;
    flockHybridWifiSecs = FLOCK_HYBRID_PRESETS[flockHybridPresetIdx].wifiSecs;
    flockHybridHopMs    = FLOCK_HYBRID_PRESETS[flockHybridPresetIdx].hopMs;
}

static const int BLE_TIME_OPTIONS[]    = {5, 8, 10, 15, 20};
static const int WIFI_TIME_OPTIONS[]   = {5, 10, 15, 20};
static const int WIFI_RESULT_OPTIONS[] = {10, 20, 30};
static const int DEAUTH_HOP_OPTIONS[]  = {100, 200, 500, 1000};
static const int PACKET_HOP_OPTIONS[]  = {
    PACKET_MONITOR_HOP_PRESET_0_MS,
    PACKET_MONITOR_HOP_PRESET_1_MS,
    PACKET_MONITOR_HOP_PRESET_2_MS,
    PACKET_MONITOR_HOP_PRESET_3_MS,
    PACKET_MONITOR_HOP_PRESET_4_MS
};

static int nextOptionValue(const int *options, int count, int currentValue) {
    for (int i = 0; i < count; i++) {
        if (options[i] == currentValue) return options[(i + 1) % count];
    }
    return options[0];
}

static void setBtnText(lv_obj_t *btn, const char *txt) {
    if (!btn) return;
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label) lv_label_set_text(label, txt);
}

static void updateScanDefaultsLabels() {
    char buf[48];
    snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH "  BLE Time: %d sec", bleScanSeconds);
    setBtnText(scanDefaultsBleBtn, buf);
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  WiFi Time: %d sec", wifiScanSeconds);
    setBtnText(scanDefaultsWifiBtn, buf);
    snprintf(buf, sizeof(buf), LV_SYMBOL_SETTINGS "  WiFi Results: %d", wifiMaxResults);
    setBtnText(scanDefaultsResultBtn, buf);
    snprintf(buf, sizeof(buf), LV_SYMBOL_REFRESH "  Deauth Hop: %d ms", deauthHopMs);
    setBtnText(scanDefaultsHopBtn, buf);
    snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  Hybrid: %s", FLOCK_HYBRID_PRESETS[flockHybridPresetIdx].name);
    setBtnText(scanDefaultsHybridBtn, buf);
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  Packet Hop: %s", packetMonitorHopEnabled ? "ON" : "OFF");
    setBtnText(scanDefaultsPacketHopBtn, buf);
    snprintf(buf, sizeof(buf), LV_SYMBOL_REFRESH "  Packet Hop: %d ms", packetMonitorHopMs);
    setBtnText(scanDefaultsPacketMsBtn, buf);
}

static void cb_scanDefaultsBle(lv_event_t *e) {
    bleScanSeconds = nextOptionValue(BLE_TIME_OPTIONS, sizeof(BLE_TIME_OPTIONS) / sizeof(BLE_TIME_OPTIONS[0]), bleScanSeconds);
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

static void cb_scanDefaultsWifiTime(lv_event_t *e) {
    wifiScanSeconds = nextOptionValue(WIFI_TIME_OPTIONS, sizeof(WIFI_TIME_OPTIONS) / sizeof(WIFI_TIME_OPTIONS[0]), wifiScanSeconds);
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

static void cb_scanDefaultsWifiResults(lv_event_t *e) {
    wifiMaxResults = nextOptionValue(WIFI_RESULT_OPTIONS, sizeof(WIFI_RESULT_OPTIONS) / sizeof(WIFI_RESULT_OPTIONS[0]), wifiMaxResults);
    if (wifiMaxResults > MAX_WIFI_RESULTS) wifiMaxResults = MAX_WIFI_RESULTS;
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

static void cb_scanDefaultsDeauthHop(lv_event_t *e) {
    deauthHopMs = nextOptionValue(DEAUTH_HOP_OPTIONS, sizeof(DEAUTH_HOP_OPTIONS) / sizeof(DEAUTH_HOP_OPTIONS[0]), deauthHopMs);
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

static void cb_scanDefaultsFlockHybrid(lv_event_t *e) {
    flockHybridPresetIdx = (flockHybridPresetIdx + 1) % FLOCK_HYBRID_PRESET_COUNT;
    applyFlockHybridPreset();
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

static void cb_scanDefaultsPacketHopToggle(lv_event_t *e) {
    packetMonitorHopEnabled = !packetMonitorHopEnabled;
    packetMonitorLastHopMs = millis();
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

static void cb_scanDefaultsPacketHopMs(lv_event_t *e) {
    packetMonitorHopMs = nextOptionValue(PACKET_HOP_OPTIONS, sizeof(PACKET_HOP_OPTIONS) / sizeof(PACKET_HOP_OPTIONS[0]), packetMonitorHopMs);
    packetMonitorLastHopMs = millis();
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

void createScanDefaults() {
    if (miscToolScreen) { lv_obj_delete(miscToolScreen); miscToolScreen = nullptr; }
    miscToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(miscToolScreen);
    createHeader(miscToolScreen, LV_SYMBOL_SETTINGS "  Scan Defaults");

    lv_obj_t *list = lv_list_create(miscToolScreen);
    lv_obj_set_size(list, SCREEN_W, SCREEN_H - 28 - 34);
    lv_obj_set_pos(list, 0, 28);
    lv_obj_set_style_bg_color(list,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(list,      6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_row(list,      4,                      LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(TH.accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);

    scanDefaultsBleBtn = lv_list_add_btn(list, nullptr, "");
    styleListBtn(scanDefaultsBleBtn);
    lv_obj_set_height(scanDefaultsBleBtn, 30);
    lv_obj_add_event_cb(scanDefaultsBleBtn, cb_scanDefaultsBle, LV_EVENT_CLICKED, nullptr);

    scanDefaultsWifiBtn = lv_list_add_btn(list, nullptr, "");
    styleListBtn(scanDefaultsWifiBtn);
    lv_obj_set_height(scanDefaultsWifiBtn, 30);
    lv_obj_add_event_cb(scanDefaultsWifiBtn, cb_scanDefaultsWifiTime, LV_EVENT_CLICKED, nullptr);

    scanDefaultsResultBtn = lv_list_add_btn(list, nullptr, "");
    styleListBtn(scanDefaultsResultBtn);
    lv_obj_set_height(scanDefaultsResultBtn, 30);
    lv_obj_add_event_cb(scanDefaultsResultBtn, cb_scanDefaultsWifiResults, LV_EVENT_CLICKED, nullptr);

    scanDefaultsHopBtn = lv_list_add_btn(list, nullptr, "");
    styleListBtn(scanDefaultsHopBtn);
    lv_obj_set_height(scanDefaultsHopBtn, 30);
    lv_obj_add_event_cb(scanDefaultsHopBtn, cb_scanDefaultsDeauthHop, LV_EVENT_CLICKED, nullptr);

    scanDefaultsHybridBtn = lv_list_add_btn(list, nullptr, "");
    styleListBtn(scanDefaultsHybridBtn);
    lv_obj_set_height(scanDefaultsHybridBtn, 30);
    lv_obj_add_event_cb(scanDefaultsHybridBtn, cb_scanDefaultsFlockHybrid, LV_EVENT_CLICKED, nullptr);

    scanDefaultsPacketHopBtn = lv_list_add_btn(list, nullptr, "");
    styleListBtn(scanDefaultsPacketHopBtn);
    lv_obj_set_height(scanDefaultsPacketHopBtn, 30);
    lv_obj_add_event_cb(scanDefaultsPacketHopBtn, cb_scanDefaultsPacketHopToggle, LV_EVENT_CLICKED, nullptr);

    scanDefaultsPacketMsBtn = lv_list_add_btn(list, nullptr, "");
    styleListBtn(scanDefaultsPacketMsBtn);
    lv_obj_set_height(scanDefaultsPacketMsBtn, 30);
    lv_obj_add_event_cb(scanDefaultsPacketMsBtn, cb_scanDefaultsPacketHopMs, LV_EVENT_CLICKED, nullptr);

    updateScanDefaultsLabels();

    lv_obj_t *backBtn = createBackBtn(miscToolScreen, cb_miscToolBack);

    deleteGroup(&miscToolGroup);
    miscToolGroup = lv_group_create();
    lv_group_add_obj(miscToolGroup, scanDefaultsBleBtn);
    lv_group_add_obj(miscToolGroup, scanDefaultsWifiBtn);
    lv_group_add_obj(miscToolGroup, scanDefaultsResultBtn);
    lv_group_add_obj(miscToolGroup, scanDefaultsHopBtn);
    lv_group_add_obj(miscToolGroup, scanDefaultsHybridBtn);
    lv_group_add_obj(miscToolGroup, scanDefaultsPacketHopBtn);
    lv_group_add_obj(miscToolGroup, scanDefaultsPacketMsBtn);
    lv_group_add_obj(miscToolGroup, backBtn);
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
    // A manual brightness change also counts as activity and should restore
    // the screen if it was auto-dimmed.
    resetInactivityTimer();
    applyBacklightLevel((uint8_t)lcdBrightness);
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
    lv_obj_set_style_text_color(brightPctLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(brightPctLbl, LV_ALIGN_CENTER, 0, -30);

    // Brightness bar
    brightBar = lv_bar_create(miscToolScreen);
    lv_obj_set_size(brightBar, SCREEN_W - 32, 16);
    lv_obj_align(brightBar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(brightBar, 0, 100);
    lv_obj_set_style_bg_color(brightBar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightBar, lv_color_hex(TH.warn), LV_PART_INDICATOR);
    lv_obj_set_style_radius(brightBar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(brightBar, 4, LV_PART_INDICATOR);

    // – button
    brightDownBtn = lv_btn_create(miscToolScreen);
    lv_obj_set_size(brightDownBtn, 52, 30);
    lv_obj_align(brightDownBtn, LV_ALIGN_CENTER, -46, 36);
    lv_obj_set_style_bg_color(brightDownBtn, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_radius(brightDownBtn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(brightDownBtn, cb_brightDown, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *dLbl = lv_label_create(brightDownBtn);
    lv_label_set_text(dLbl, LV_SYMBOL_MINUS);
    lv_obj_center(dLbl);

    // + button
    brightUpBtn = lv_btn_create(miscToolScreen);
    lv_obj_set_size(brightUpBtn, 52, 30);
    lv_obj_align(brightUpBtn, LV_ALIGN_CENTER, 46, 36);
    lv_obj_set_style_bg_color(brightUpBtn, lv_color_hex(TH.border), LV_PART_MAIN);
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
//  MISC TOOL 4 – THEME PICKER
//
//  Displays the 3 available themes as a list. Selecting one sets
//  currentTheme and navigates back to the misc menu, which redraws
//  using the new theme. All subsequent screens pick up the new
//  colors since they are recreated fresh on every navigation.
// ════════════════════════════════════════════════════════════════
void createThemePicker() {
    if (miscToolScreen) { lv_obj_delete(miscToolScreen); miscToolScreen = nullptr; }
    miscToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(miscToolScreen);
    createHeader(miscToolScreen, LV_SYMBOL_EDIT "  Themes");

    lv_obj_t *list = lv_list_create(miscToolScreen);
    lv_obj_set_size(list, SCREEN_W, SCREEN_H - 28 - 34);
    lv_obj_set_pos(list, 0, 28);
    lv_obj_set_style_bg_color(list,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list,       LV_OPA_COVER,        LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list,      6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list,      4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(TH.accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);

    deleteGroup(&miscToolGroup);
    miscToolGroup = lv_group_create();

    int numThemes = (int)(sizeof(THEMES) / sizeof(THEMES[0]));
    for (int i = 0; i < numThemes; i++) {
        // Build label — mark active theme with a checkmark
        char label[32];
        snprintf(label, sizeof(label), "%s%s",
                 (i == currentTheme) ? LV_SYMBOL_OK "  " : "    ",
                 THEMES[i].name);

        lv_obj_t *btn = lv_list_add_btn(list, nullptr, label);
        styleListBtn(btn);
        lv_obj_set_height(btn, 30);

        // Active theme in accent color, others in dim
        lv_obj_set_style_text_color(btn,
            (i == currentTheme) ? lv_color_hex(TH.accent) : lv_color_hex(TH.textDim),
            LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            int idx = (int)(intptr_t)lv_event_get_user_data(ev);
            currentTheme = idx;
            miscToolScreen = nullptr;
            deleteGroup(&miscToolGroup);
            setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b, 3);
            // Rebuild main menu with new theme so it recolors too
            // (createMainMenu calls lv_screen_load so mainScreen is refreshed)
            if (mainScreen) { lv_obj_delete(mainScreen); mainScreen = nullptr; }
            createMainMenu();
            // Then navigate to the freshly themed misc menu
            createMiscMenu();
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_group_add_obj(miscToolGroup, btn);
    }

    lv_obj_t *backBtn = createBackBtn(miscToolScreen, cb_miscToolBack);
    lv_group_add_obj(miscToolGroup, backBtn);
    setGroup(miscToolGroup);

    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b, LED_BRIGHTNESS);
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
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    switch (t) {
        case 0: createGPSStats();    break;
        case 1: createWiggleWars();  break;
    }
}

void createGPSMenu() {
    if (gpsMenuScreen) { lv_obj_delete(gpsMenuScreen); gpsMenuScreen = nullptr; }
    gpsMenuScreen = lv_obj_create(nullptr);
    applyScreenStyle(gpsMenuScreen);
    createHeader(gpsMenuScreen, LV_SYMBOL_GPS "  GPS Tools");

    lv_obj_t *list = lv_list_create(gpsMenuScreen);
    lv_obj_set_size(list, SCREEN_W, SCREEN_H - 28 - 34);
    lv_obj_set_pos(list, 0, 28);
    lv_obj_set_style_bg_color(list,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(list,      6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_row(list,      4,                      LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(TH.accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);

    deleteGroup(&gpsMenuGroup);
    gpsMenuGroup = lv_group_create();

    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn = lv_list_add_btn(list, nullptr, GPS_TOOL_LABELS[i]);
        styleListBtn(btn);
        lv_obj_set_height(btn, 30);
        lv_obj_add_event_cb(btn, cb_gpsToolSelected, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_group_add_obj(gpsMenuGroup, btn);
    }

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
        lv_obj_set_style_text_color(gpsFixLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
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
    lv_obj_set_style_text_color(gpsFixLbl, lv_color_hex(TH.success), LV_PART_MAIN);

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
    lv_obj_set_style_bg_color(card,     lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1,                      LV_PART_MAIN);
    lv_obj_set_style_radius(card,       6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,      6,                      LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    const int rowH = 18;

    gpsFixLbl = lv_label_create(card);
    lv_label_set_text(gpsFixLbl, LV_SYMBOL_WARNING "  Searching for fix...");
    lv_obj_set_style_text_color(gpsFixLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_obj_set_pos(gpsFixLbl, 0, 0);

    gpsLatLbl = lv_label_create(card);
    lv_label_set_text(gpsLatLbl, "Lat:  ---.------");
    lv_obj_set_style_text_color(gpsLatLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
    lv_obj_set_pos(gpsLatLbl, 0, rowH * 1 + 4);

    gpsLngLbl = lv_label_create(card);
    lv_label_set_text(gpsLngLbl, "Lng:  ---.------");
    lv_obj_set_style_text_color(gpsLngLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
    lv_obj_set_pos(gpsLngLbl, 0, rowH * 2 + 4);

    gpsSpdLbl = lv_label_create(card);
    lv_label_set_text(gpsSpdLbl, "Spd:  --- km/h");
    lv_obj_set_style_text_color(gpsSpdLbl, lv_color_hex(TH.success), LV_PART_MAIN);
    lv_obj_set_pos(gpsSpdLbl, 0, rowH * 3 + 4);

    gpsAltLbl = lv_label_create(card);
    lv_label_set_text(gpsAltLbl, "Alt:  --- m");
    lv_obj_set_style_text_color(gpsAltLbl, lv_color_hex(TH.success), LV_PART_MAIN);
    lv_obj_set_pos(gpsAltLbl, 0, rowH * 4 + 4);

    gpsSatLbl = lv_label_create(card);
    lv_label_set_text(gpsSatLbl, "Sats: 0");
    lv_obj_set_style_text_color(gpsSatLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
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
        "WigleWifi-1.4,appRelease=1.0,model=T-Embed,release=1.0,"
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
            lv_color_hex(TH.warn), LV_PART_MAIN);
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
            lv_color_hex(TH.accent), LV_PART_MAIN);
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
            lv_color_hex(TH.success), LV_PART_MAIN);

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
            lv_color_hex(TH.alert), LV_PART_MAIN);
        return;
    }

    // Need fix to name file with GPS time
    if (!gps.date.isValid() || !gps.time.isValid()) {
        lv_label_set_text(wiggleStatusLbl,
            LV_SYMBOL_WARNING "  Need GPS fix first!");
        lv_obj_set_style_text_color(wiggleStatusLbl,
            lv_color_hex(TH.warn), LV_PART_MAIN);
        return;
    }

    if (!wiggleOpenFile()) {
        lv_label_set_text(wiggleStatusLbl,
            LV_SYMBOL_CLOSE "  Failed to create file!");
        lv_obj_set_style_text_color(wiggleStatusLbl,
            lv_color_hex(TH.alert), LV_PART_MAIN);
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
        lv_color_hex(TH.success), LV_PART_MAIN);

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
        lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(wiggleStatusLbl, 8, 30);

    // Stats card
    lv_obj_t *card = lv_obj_create(gpsToolScreen);
    lv_obj_set_size(card, SCREEN_W - 12, 56);
    lv_obj_set_pos(card, 6, 50);
    lv_obj_set_style_bg_color(card,     lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1,                      LV_PART_MAIN);
    lv_obj_set_style_radius(card,       6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,      5,                      LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    wiggleScanLbl = lv_label_create(card);
    lv_label_set_text(wiggleScanLbl, "Scans: 0");
    lv_obj_set_style_text_color(wiggleScanLbl,
        lv_color_hex(TH.accent), LV_PART_MAIN);
    lv_obj_set_pos(wiggleScanLbl, 0, 0);

    wiggleNetLbl = lv_label_create(card);
    lv_label_set_text(wiggleNetLbl, "Nets logged: 0");
    lv_obj_set_style_text_color(wiggleNetLbl,
        lv_color_hex(TH.success), LV_PART_MAIN);
    lv_obj_set_pos(wiggleNetLbl, 0, 18);

    wiggleFileLbl = lv_label_create(card);
    lv_label_set_text(wiggleFileLbl, "File: none");
    lv_obj_set_style_text_color(wiggleFileLbl,
        lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(wiggleFileLbl, 0, 36);

    // Start / Stop buttons alongside Back
    lv_obj_t *backBtn = createBackBtn(gpsToolScreen, cb_gpsToolBack);

    // Start button
    wiggleStartBtn = lv_btn_create(gpsToolScreen);
    lv_obj_set_size(wiggleStartBtn, 70, 28);
    lv_obj_align(wiggleStartBtn, LV_ALIGN_BOTTOM_MID, -12, -4);
    lv_obj_set_style_bg_color(wiggleStartBtn,
        lv_color_hex(TH.flashGreen), LV_PART_MAIN);
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
        lv_color_hex(TH.stopRed), LV_PART_MAIN);
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
static const char *WIFI_TOOL_LABELS[8] = {
    LV_SYMBOL_WIFI     "  Network Scanner",
    LV_SYMBOL_WARNING  "  Deauth Detector",
    LV_SYMBOL_LOOP     "  Channel Analyzer",
    LV_SYMBOL_EYE_OPEN "  Packet Monitor",
    LV_SYMBOL_EYE_OPEN "  PineAP Hunter",
    LV_SYMBOL_EYE_OPEN "  Pwnagotchi Watch",
    LV_SYMBOL_WARNING  "  Flock Detector",
    LV_SYMBOL_BLUETOOTH "  Flock Hybrid"
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
        case 3: createPacketMonitor();      break;
        case 4: createPineAPHunter();       break;
        case 5: createPwnagotchiDetector(); break;
        case 6: createFlockDetector();      break;
        case 7: createFlockHybridScanner(); break;
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
    lv_obj_set_style_bg_color(list,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list,      6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list,      4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(TH.accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);

    deleteGroup(&wifiMenuGroup);
    wifiMenuGroup = lv_group_create();

    for (int i = 0; i < 8; i++) {
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
    if (rssi >= -55) return lv_color_hex(TH.success);
    if (rssi >= -70) return lv_color_hex(TH.warn);
    return              lv_color_hex(TH.alert);
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

    // Runtime WiFi scan time is reset-after-reboot and controlled from Misc > Scan Defaults.
    // Arduino WiFi scanning is async here so the configured time can act as a soft timeout.
    WiFi.scanDelete();
    WiFi.scanNetworks(true, true);  // async=true, show_hidden=true

    int n = WIFI_SCAN_RUNNING;
    unsigned long startMs = millis();
    unsigned long timeoutMs = (unsigned long)wifiScanSeconds * 1000UL;
    while ((millis() - startMs) < timeoutMs) {
        n = WiFi.scanComplete();
        if (n >= 0) break;
        lv_timer_handler();
        delay(25);
    }

    if (n == WIFI_SCAN_RUNNING) {
        esp_wifi_scan_stop();
        delay(25);
        n = WiFi.scanComplete();
    }

    if (n < 0) n = 0;

    // wifiEntries[] is compiled to MAX_WIFI_RESULTS, so runtime value is safely capped.
    int resultLimit = wifiMaxResults;
    if (resultLimit > MAX_WIFI_RESULTS) resultLimit = MAX_WIFI_RESULTS;
    if (n > resultLimit) n = resultLimit;
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
    if (hybridWifiActive) {
        hybridWifiActive = false;
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);
    }
    if (packetMonitorActive) {
        packetMonitorActive = false;
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);
    }
    if (spinnerRunning) {
        stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
    }
    if (deauthTimer) { lv_timer_delete(deauthTimer); deauthTimer = nullptr; }
    if (pwnTimer)    { lv_timer_delete(pwnTimer);    pwnTimer    = nullptr; }
    if (flockTimer)  { lv_timer_delete(flockTimer);  flockTimer  = nullptr; }
    if (hybridStartTimer) { lv_timer_delete(hybridStartTimer); hybridStartTimer = nullptr; }
    if (packetMonitorTimer) { lv_timer_delete(packetMonitorTimer); packetMonitorTimer = nullptr; }
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
    lv_obj_set_style_text_color(scanStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
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
        found > 0 ? lv_color_hex(TH.success) : lv_color_hex(TH.textDim),
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
    lv_obj_set_style_text_color(scanStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(scanStatusLbl, 8, 30);

    // List: header(28) + status(18) + bottom bar(34) = 80 used; rest for list
    scanList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(scanList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(scanList, 0, 48);
    lv_obj_set_style_bg_color(scanList,     lv_color_hex(TH.bg), LV_PART_MAIN);
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
    lv_obj_set_style_bg_color(card,      lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,        LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card,  lv_color_hex(TH.border), LV_PART_MAIN);
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
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(infoLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    // RSSI bar (requires LV_USE_BAR 1 in lv_conf.h)
    lv_obj_t *bar = lv_bar_create(wifiDetailScreen);
    lv_obj_set_size(bar, SCREEN_W - 12, 5);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(bar, -100, -30);
    lv_bar_set_value(bar, rssi, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TH.barBg), LV_PART_MAIN);
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

    if (deauthTotal > deauthSoundedTotal) {
        playDeauthChirp();
        deauthSoundedTotal = deauthTotal;
    }

    char buf[56];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WARNING "  Deauth frames: %d", deauthTotal);
    lv_label_set_text(deauthCountLbl, buf);
    lv_obj_set_style_text_color(deauthCountLbl,
        deauthTotal > 0 ? lv_color_hex(TH.alert) : lv_color_hex(TH.success),
        LV_PART_MAIN);

    lv_obj_clean(deauthEventList);
    int total = (deauthTotal < MAX_DEAUTH) ? deauthTotal : MAX_DEAUTH;

    if (total == 0) {
        lv_obj_t *e = lv_list_add_text(deauthEventList, "No frames detected yet...");
        if (e) lv_obj_set_style_text_color(e, lv_color_hex(TH.textDim), LV_PART_MAIN);
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
            lv_obj_set_style_text_color(entry, lv_color_hex(TH.alert), LV_PART_MAIN);
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
    lv_obj_set_style_text_color(deauthCountLbl, lv_color_hex(TH.success), LV_PART_MAIN);
    lv_obj_set_pos(deauthCountLbl, 8, 30);

    lv_obj_t *hopLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(hopLbl, "Hopping ch 1-13");
    lv_obj_set_style_text_color(hopLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_align(hopLbl, LV_ALIGN_TOP_RIGHT, -8, 30);

    deauthEventList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(deauthEventList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(deauthEventList, 0, 48);
    lv_obj_set_style_bg_color(deauthEventList, lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(deauthEventList,   LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(deauthEventList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(deauthEventList,  2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(deauthEventList,  1, LV_PART_MAIN);

    lv_obj_t *initLbl =
        lv_list_add_text(deauthEventList, "Monitoring... (no events yet)");
    if (initLbl)
        lv_obj_set_style_text_color(initLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);

    lv_obj_t *backBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, backBtn);
    setGroup(wifiToolGroup);

    // Start sniffer
    deauthTotal  = 0;
    deauthHead   = 0;
    deauthSoundedTotal = 0;
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
        if      (chanNetCount[ch] == 0) col = lv_color_hex(TH.barBg);
        else if (chanNetCount[ch] <= 2) col = lv_color_hex(TH.success);
        else if (chanNetCount[ch] <= 4) col = lv_color_hex(TH.warn);
        else                            col = lv_color_hex(TH.alert);

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
        lv_obj_set_style_text_color(chLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
        lv_obj_set_pos(chLbl, x, maxBarH + 2);

        // Count label above bar
        if (chanNetCount[ch] > 0) {
            lv_obj_t *cLbl = lv_label_create(chanChartArea);
            char cnt[4];
            snprintf(cnt, sizeof(cnt), "%d", chanNetCount[ch]);
            lv_label_set_text(cLbl, cnt);
            lv_obj_set_style_text_color(cLbl, lv_color_hex(TH.text), LV_PART_MAIN);
            lv_obj_set_pos(cLbl, x, y > 12 ? y - 12 : 0);
        }
    }
}

static void cb_doChannelScan(lv_event_t *e) {
    lv_label_set_text(chanStatusLbl, LV_SYMBOL_REFRESH "  Scanning channels...");
    lv_obj_set_style_text_color(chanStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
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
        n > 0 ? lv_color_hex(TH.success) : lv_color_hex(TH.textDim),
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
    lv_obj_set_style_text_color(chanStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(chanStatusLbl, 8, 30);

    chanChartArea = lv_obj_create(wifiToolScreen);
    lv_obj_set_size(chanChartArea, SCREEN_W - 12, SCREEN_H - 80);
    lv_obj_set_pos(chanChartArea, 6, 48);
    lv_obj_set_style_bg_color(chanChartArea, lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chanChartArea,   LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(chanChartArea, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(chanChartArea, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(chanChartArea, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chanChartArea, 4, LV_PART_MAIN);
    lv_obj_clear_flag(chanChartArea, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ph = lv_label_create(chanChartArea);
    lv_label_set_text(ph, "Channels 1-13");
    lv_obj_set_style_text_color(ph, lv_color_hex(TH.border), LV_PART_MAIN);
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



// ════════════════════════════════════════════════════════════════
//  TOOL 4 – PACKET MONITOR
//
//  Display-only first pass inspired by PacketMonitor32. Uses WiFi
//  promiscuous mode to count packet activity on the selected channel.
//  No PCAP / SD writes yet, so it stays lighter and safer.
// ════════════════════════════════════════════════════════════════

static void IRAM_ATTR packet_monitor_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!packetMonitorActive) return;

    const wifi_promiscuous_pkt_t *pkt = reinterpret_cast<const wifi_promiscuous_pkt_t *>(buf);
    packetMonTotalPackets++;
    packetMonRssiSum += pkt->rx_ctrl.rssi;

    if      (type == WIFI_PKT_MGMT) packetMonMgmtPackets++;
    else if (type == WIFI_PKT_DATA) packetMonDataPackets++;
    else if (type == WIFI_PKT_CTRL) packetMonCtrlPackets++;
}

static const char *packetSignalLabel(int avgRssi) {
    if (avgRssi >= -55) return "Strong";
    if (avgRssi >= -70) return "Good";
    if (avgRssi >= -82) return "Weak";
    return "Far";
}

static void packetMonResetCounters() {
    packetMonTotalPackets = 0;
    packetMonMgmtPackets  = 0;
    packetMonDataPackets  = 0;
    packetMonCtrlPackets  = 0;
    packetMonRssiSum      = 0;
    packetMonLastTotal    = 0;
    packetMonLastRssiCnt  = 0;
    packetMonLastRssiSum  = 0;
    packetMonLastUpdate   = millis();
    packetMonSampleHead   = 0;
    memset(packetMonRateSamples, 0, sizeof(packetMonRateSamples));
}

static void packetMonDrawGraph() {
    if (!packetMonGraphArea) return;
    lv_obj_clean(packetMonGraphArea);

    const int areaW   = SCREEN_W - 12 - 8;
    const int areaH   = 58;
    const int barGap  = 1;
    const int barW    = max(2, (areaW / PACKET_MONITOR_GRAPH_BARS) - barGap);
    const int maxBarH = areaH - 10;

    uint16_t maxRate = 1;
    for (int i = 0; i < PACKET_MONITOR_GRAPH_BARS; i++) {
        if (packetMonRateSamples[i] > maxRate) maxRate = packetMonRateSamples[i];
    }
#if PACKET_MONITOR_GRAPH_MAX_RATE > 0
    if (maxRate < PACKET_MONITOR_GRAPH_MAX_RATE) maxRate = PACKET_MONITOR_GRAPH_MAX_RATE;
#endif

    for (int i = 0; i < PACKET_MONITOR_GRAPH_BARS; i++) {
        int srcIdx = (packetMonSampleHead + i) % PACKET_MONITOR_GRAPH_BARS;
        uint16_t rate = packetMonRateSamples[srcIdx];
        int barH = rate == 0 ? 2 : max(2, (int)((float)rate / maxRate * maxBarH));
        int x = i * (barW + barGap);
        int y = maxBarH - barH;

        lv_color_t col = lv_color_hex(TH.accent);
        if (rate == 0) col = lv_color_hex(TH.barBg);
        else if (rate > (maxRate * 75 / 100)) col = lv_color_hex(TH.alert);
        else if (rate > (maxRate * 45 / 100)) col = lv_color_hex(TH.warn);
        else col = lv_color_hex(TH.success);

        lv_obj_t *bar = lv_obj_create(packetMonGraphArea);
        lv_obj_set_size(bar, barW, barH);
        lv_obj_set_pos(bar, x, y);
        lv_obj_set_style_bg_color(bar, col, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *base = lv_obj_create(packetMonGraphArea);
    lv_obj_set_size(base, areaW, 1);
    lv_obj_set_pos(base, 0, maxBarH + 2);
    lv_obj_set_style_bg_color(base, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(base, 0, LV_PART_MAIN);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);
}

static void packetMonUpdateUI() {
    if (!packetMonStatsLbl || !packetMonStatusLbl) return;

    uint32_t now = millis();

    if (packetMonitorActive && packetMonitorHopEnabled &&
        (now - packetMonitorLastHopMs >= (uint32_t)packetMonitorHopMs)) {
        packetMonitorChannel++;
        if (packetMonitorChannel > 13) packetMonitorChannel = 1;
        esp_wifi_set_channel(packetMonitorChannel, WIFI_SECOND_CHAN_NONE);
        packetMonitorLastHopMs = now;
    }

    uint32_t total = packetMonTotalPackets;
    uint32_t mgmt  = packetMonMgmtPackets;
    uint32_t data  = packetMonDataPackets;
    uint32_t ctrl  = packetMonCtrlPackets;
    int32_t  rssiS = packetMonRssiSum;

    uint32_t elapsed = now - packetMonLastUpdate;
    if (elapsed == 0) elapsed = 1;
    uint32_t deltaPackets = total - packetMonLastTotal;

    // Keep both sides of the math as uint32_t to avoid Arduino/C++ min()
    // template type conflicts on ESP32 core 2.0.10.
    uint32_t rawRate = (uint32_t)((deltaPackets * 1000UL) / elapsed);
    if (rawRate > 65535UL) rawRate = 65535UL;
    uint16_t rate = (uint16_t)rawRate;

    int avgRssi = -99;
    uint32_t deltaRssiCount = total - packetMonLastRssiCnt;
    if (deltaRssiCount > 0) {
        avgRssi = (int)((rssiS - (int32_t)packetMonLastRssiSum) / (int32_t)deltaRssiCount);
    }

    packetMonRateSamples[packetMonSampleHead] = rate;
    packetMonSampleHead = (packetMonSampleHead + 1) % PACKET_MONITOR_GRAPH_BARS;
    packetMonLastTotal   = total;
    packetMonLastRssiCnt = total;
    packetMonLastRssiSum = rssiS;
    packetMonLastUpdate  = now;

    char status[72];
    snprintf(status, sizeof(status), "%s CH:%u %s  Rate:%u/s  RSSI:%ddBm %s",
             packetMonitorActive ? LV_SYMBOL_PLAY : LV_SYMBOL_STOP,
             packetMonitorChannel,
             packetMonitorHopEnabled ? "HOP" : "MAN",
             rate,
             avgRssi,
             deltaRssiCount ? packetSignalLabel(avgRssi) : "--");
    lv_label_set_text(packetMonStatusLbl, status);
    lv_obj_set_style_text_color(packetMonStatusLbl,
        packetMonitorActive ? lv_color_hex(TH.accent) : lv_color_hex(TH.textDim),
        LV_PART_MAIN);

    char stats[96];
    snprintf(stats, sizeof(stats), "Packets:%lu   Mgmt:%lu   Data:%lu   Ctrl:%lu",
             (unsigned long)total,
             (unsigned long)mgmt,
             (unsigned long)data,
             (unsigned long)ctrl);
    lv_label_set_text(packetMonStatsLbl, stats);

    packetMonDrawGraph();
}

static void packetMonTimerCb(lv_timer_t *t) {
    packetMonUpdateUI();
}

static void packetMonStop() {
    if (!packetMonitorActive) return;
    packetMonitorActive = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
    if (packetMonStartLbl) lv_label_set_text(packetMonStartLbl, LV_SYMBOL_PLAY "  Start");
    packetMonUpdateUI();
}

static void packetMonStart() {
    packetMonResetCounters();

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_channel(packetMonitorChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous_rx_cb(packet_monitor_cb);
    esp_wifi_set_promiscuous(true);
    packetMonitorActive = true;
    packetMonitorLastHopMs = millis();

    startLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, 120);
    if (packetMonStartLbl) lv_label_set_text(packetMonStartLbl, LV_SYMBOL_STOP "  Stop");
    packetMonUpdateUI();
}

static void cb_packetMonStartStop(lv_event_t *e) {
    if (packetMonitorActive) packetMonStop();
    else packetMonStart();
}

static void packetMonSetChannel(uint8_t ch) {
    if (ch < 1) ch = 13;
    if (ch > 13) ch = 1;
    packetMonitorChannel = ch;
    packetMonitorLastHopMs = millis();
    if (packetMonitorActive) {
        esp_wifi_set_channel(packetMonitorChannel, WIFI_SECOND_CHAN_NONE);
        packetMonResetCounters();
    }
    packetMonUpdateUI();
}

static void cb_packetMonChMinus(lv_event_t *e) {
    packetMonSetChannel(packetMonitorChannel == 1 ? 13 : packetMonitorChannel - 1);
}

static void cb_packetMonChPlus(lv_event_t *e) {
    packetMonSetChannel(packetMonitorChannel == 13 ? 1 : packetMonitorChannel + 1);
}

static lv_obj_t *createSmallPacketBtn(lv_obj_t *parent, const char *text, int x, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 50, 26);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, x, -4);
    lv_obj_set_style_bg_color(btn, TC(actionBg),  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, TC(actionFoc), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, TC(success),   LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, TC(actionBdr), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 5, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, TC(text), LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

void createPacketMonitor() {
    packetMonitorActive = false;
    packetMonStatusLbl = nullptr;
    packetMonStatsLbl  = nullptr;
    packetMonGraphArea = nullptr;
    packetMonStartBtn  = nullptr;
    packetMonStartLbl  = nullptr;
    packetMonResetCounters();

    if (wifiToolScreen) { lv_obj_delete(wifiToolScreen); wifiToolScreen = nullptr; }
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_EYE_OPEN "  Packet Monitor");

    packetMonStatusLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(packetMonStatusLbl, packetMonitorHopEnabled ?
        "Ready. Packet Hop ON. Press Start." :
        "Ready. Choose channel and press Start.");
    lv_obj_set_style_text_color(packetMonStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(packetMonStatusLbl, 8, 30);

    packetMonStatsLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(packetMonStatsLbl, "Packets:0   Mgmt:0   Data:0   Ctrl:0");
    lv_obj_set_style_text_color(packetMonStatsLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_set_pos(packetMonStatsLbl, 8, 46);

    packetMonGraphArea = lv_obj_create(wifiToolScreen);
    lv_obj_set_size(packetMonGraphArea, SCREEN_W - 12, 66);
    lv_obj_set_pos(packetMonGraphArea, 6, 66);
    lv_obj_set_style_bg_color(packetMonGraphArea, lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(packetMonGraphArea, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(packetMonGraphArea, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(packetMonGraphArea, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(packetMonGraphArea, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(packetMonGraphArea, 4, LV_PART_MAIN);
    lv_obj_clear_flag(packetMonGraphArea, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint = lv_label_create(packetMonGraphArea);
    lv_label_set_text(hint, "Live packets / second graph");
    lv_obj_set_style_text_color(hint, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *backBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    lv_obj_t *chMinus = createSmallPacketBtn(wifiToolScreen, "CH-", 112, cb_packetMonChMinus);
    lv_obj_t *chPlus  = createSmallPacketBtn(wifiToolScreen, "CH+", 166, cb_packetMonChPlus);

    packetMonStartBtn = lv_btn_create(wifiToolScreen);
    lv_obj_set_size(packetMonStartBtn, 96, 26);
    lv_obj_align(packetMonStartBtn, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
    lv_obj_set_style_bg_color(packetMonStartBtn, TC(actionBg),  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(packetMonStartBtn, TC(actionFoc), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(packetMonStartBtn, TC(success),   LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(packetMonStartBtn, TC(actionBdr), LV_PART_MAIN);
    lv_obj_set_style_border_width(packetMonStartBtn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(packetMonStartBtn, 5, LV_PART_MAIN);
    lv_obj_add_event_cb(packetMonStartBtn, cb_packetMonStartStop, LV_EVENT_CLICKED, nullptr);
    packetMonStartLbl = lv_label_create(packetMonStartBtn);
    lv_label_set_text(packetMonStartLbl, LV_SYMBOL_PLAY "  Start");
    lv_obj_set_style_text_color(packetMonStartLbl, TC(text), LV_PART_MAIN);
    lv_obj_center(packetMonStartLbl);

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, backBtn);
    lv_group_add_obj(wifiToolGroup, chMinus);
    lv_group_add_obj(wifiToolGroup, chPlus);
    lv_group_add_obj(wifiToolGroup, packetMonStartBtn);
    setGroup(wifiToolGroup);

    if (packetMonitorTimer) { lv_timer_delete(packetMonitorTimer); packetMonitorTimer = nullptr; }
    packetMonitorTimer = lv_timer_create(packetMonTimerCb, PACKET_MONITOR_UPDATE_MS, nullptr);

    packetMonUpdateUI();
    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, LED_BRIGHTNESS);
    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  TOOL 5 – PINEAP HUNTER
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
// ════════════════════════════════════════════════════════════════
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
        if (e) lv_obj_set_style_text_color(e, lv_color_hex(TH.textDim), LV_PART_MAIN);
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
        lv_color_t col = flagged ? lv_color_hex(TH.alert)
                                 : lv_color_hex(TH.textDim);
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
    lv_obj_set_style_text_color(pineapStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_timer_handler();

    // Amber spinner — distinct from generic green WiFi scans
    startLEDSpinner(220, 140, 0);
    doPineAPScan();
    stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);

    if (pineapFlagged > 0) {
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_WARNING "  %d suspect AP%s!  (%d scans)",
                 pineapFlagged, pineapFlagged == 1 ? "" : "s", pineapScanCount);
        lv_obj_set_style_text_color(pineapStatusLbl, lv_color_hex(TH.alert), LV_PART_MAIN);
    } else {
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_WIFI "  %d BSSID%s tracked  (%d scans)",
                 pineapEntryCount, pineapEntryCount == 1 ? "" : "s", pineapScanCount);
        lv_obj_set_style_text_color(pineapStatusLbl,
            pineapEntryCount > 0 ? lv_color_hex(TH.success) : lv_color_hex(TH.textDim),
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
    lv_obj_set_style_text_color(pineapStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(pineapStatusLbl, 8, 30);

    pineapList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(pineapList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(pineapList, 0, 48);
    lv_obj_set_style_bg_color(pineapList,     lv_color_hex(TH.bg), LV_PART_MAIN);
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
    lv_obj_set_style_bg_color(card,     lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,       6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,      6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Badge
    lv_obj_t *badgeLbl = lv_label_create(card);
    if (flagged) {
        lv_label_set_text(badgeLbl, LV_SYMBOL_WARNING "  SUSPECTED PINEAPPLE / KARMA");
        lv_obj_set_style_text_color(badgeLbl, lv_color_hex(TH.alert), LV_PART_MAIN);
    } else {
        lv_label_set_text(badgeLbl, LV_SYMBOL_WIFI "  Normal AP (below threshold)");
        lv_obj_set_style_text_color(badgeLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
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
    lv_obj_set_style_text_color(statsLbl, lv_color_hex(TH.text), LV_PART_MAIN);
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
        flagged ? lv_color_hex(0xff9900) : lv_color_hex(TH.textDim),
        LV_PART_MAIN);
    lv_obj_align(ssidLbl, LV_ALIGN_TOP_LEFT, 0, 68);

    // RSSI bar
    lv_obj_t *bar = lv_bar_create(wifiDetailScreen);
    lv_obj_set_size(bar, SCREEN_W - 12, 5);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(bar, -100, -30);
    lv_bar_set_value(bar, pineapEntries[idx].lastRSSI, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar,
        flagged ? lv_color_hex(TH.alert) : rssiColor(pineapEntries[idx].lastRSSI),
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

// ── Meta / RayBan identifier tables (from Marauder, credit: NullPxl) ──────────
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
        std::string m = dev.getManufacturerData();
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

// Apple AirTag detection — catches both operating states:
//
//  STATE 1 — Offline / Lost mode (separated from owner):
//    Broadcasts a 31-byte Find My payload. NimBLE getManufacturerData()
//    returns bytes starting from the company ID, so the pattern is:
//    [0x4C 0x00 0x12 0x19 ...]
//    We also do a sliding memcmp across the raw payload in case the AD
//    structure is presented differently by the BLE stack (Marauder approach).
//
//  STATE 2 — Nearby / Paired mode (near owner's iPhone):
//    Broadcasts a short nearby-interaction packet. Pattern:
//    [0x4C 0x00 0x07 0x05 ...] — type 0x07 = nearby interaction
//    This is what most "close by" AirTags will actually be broadcasting.
//
//  UUID fallback — AirTags advertise service UUID 0xFD44 (Apple's
//    proprietary continuity service) in both modes.
//
// GhostESP-style raw BLE payload pattern check for Apple Find My / AirTag.
// This catches full advertising payloads like:
//   1E FF 4C 00 ...       (manufacturer AD structure with Apple company ID)
//   4C 00 12 19 ...       (Find My manufacturer payload)
// and the shorter Apple Nearby packet:
//   4C 00 07 ...          (nearby interaction)
static bool detectAirTagPayloadPattern(const uint8_t *payload, size_t len) {
#if AIRTAG_PAYLOAD_DETECT_ENABLED
    if (!payload || len < 4) return false;

    for (size_t i = 0; i <= len - 4; i++) {
        // Full AD structure form: len, type=0xFF, Apple company ID 0x004C.
        if (payload[i] == 0x1E && payload[i + 1] == 0xFF &&
            payload[i + 2] == AIRTAG_APPLE_COMPANY_LE_0 &&
            payload[i + 3] == AIRTAG_APPLE_COMPANY_LE_1) {
            return true;
        }

        // Manufacturer data form: Apple company ID + Find My type/subtype.
        if (payload[i] == AIRTAG_APPLE_COMPANY_LE_0 &&
            payload[i + 1] == AIRTAG_APPLE_COMPANY_LE_1 &&
            payload[i + 2] == AIRTAG_FINDMY_TYPE &&
            payload[i + 3] == AIRTAG_FINDMY_SUBTYPE) {
            return true;
        }

        // Manufacturer data form: Apple company ID + nearby interaction type.
        if (payload[i] == AIRTAG_APPLE_COMPANY_LE_0 &&
            payload[i + 1] == AIRTAG_APPLE_COMPANY_LE_1 &&
            payload[i + 2] == AIRTAG_NEARBY_TYPE) {
            return true;
        }
    }
#endif
    return false;
}

static bool detectAirTag(BLEAdvertisedDevice &dev) {
    // ── Check 0: GhostESP-style raw advertisement payload pattern ─
    // Arduino-ESP32 exposes the raw payload here; use it when available
    // because it can contain the complete AD structure, not only the
    // manufacturer-data slice.
#if AIRTAG_PAYLOAD_DETECT_ENABLED
    {
        uint8_t *rawPayload = dev.getPayload();
        size_t rawLen = dev.getPayloadLength();
        if (rawPayload && rawLen > 0 && detectAirTagPayloadPattern(rawPayload, rawLen)) {
            return true;
        }
    }
#endif

    // ── Check 1: manufacturer data patterns ─────────────────────
    if (dev.haveManufacturerData()) {
        std::string m = dev.getManufacturerData();
        size_t len = m.length();

        if (len >= 4) {
            uint8_t b0 = (uint8_t)m[0];
            uint8_t b1 = (uint8_t)m[1];
            uint8_t b2 = (uint8_t)m[2];

            // Must be Apple company ID (0x004C little-endian)
            if (b0 == 0x4C && b1 == 0x00) {
                // Lost/offline mode: type 0x12 = Find My network
                if (b2 == 0x12) return true;
                // Nearby/paired mode: type 0x07 = nearby interaction
                // AirTag uses subtype 0x05, but match type only to
                // be safe against minor firmware variations
                if (b2 == 0x07) return true;
            }
        }

        // Sliding window search for the raw Find My signature
        // [0x4C 0x00 0x12 0x19] anywhere in the payload —
        // catches cases where NimBLE presents the full AD frame
        if (len >= 4) {
            for (size_t i = 0; i <= len - 4; i++) {
                if ((uint8_t)m[i]   == 0x4C &&
                    (uint8_t)m[i+1] == 0x00 &&
                    (uint8_t)m[i+2] == 0x12 &&
                    (uint8_t)m[i+3] == 0x19) return true;
            }
        }
    }

    // ── Check 2: AirTag service UUID 0xFD44 ─────────────────────
    // AirTags advertise this UUID in both lost and paired modes.
    // It is Apple's proprietary "Continuity" service used by Find My.
    if (dev.haveServiceUUID()) {
        int count = dev.getServiceUUIDCount();
        for (int i = 0; i < count; i++) {
            String uuid = dev.getServiceUUID(i).toString().c_str();
            uuid.toLowerCase();
            // Short UUID form: "fd44" — look for it in the string
            if (uuid.indexOf("fd44") != -1) return true;
        }
    }

    return false;
}

// Apple device (any): Company ID 0x004C
static bool detectApple(BLEAdvertisedDevice &dev) {
    if (!dev.haveManufacturerData()) return false;
    std::string m = dev.getManufacturerData();
    if (m.length() < 2) return false;
    return ((uint8_t)m[0] == 0x4C && (uint8_t)m[1] == 0x00);
}


// GhostESP-style Flipper UUID helpers.
// These scan the raw BLE advertising payload for 16-bit, 32-bit, and 128-bit
// UUID AD fields that contain the known Flipper values.
static uint16_t rrReadU16LE(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rrReadU32LE(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool isFlipperUuidValue(uint16_t uuid) {
    return uuid == FLIPPER_UUID_BLACK ||
           uuid == FLIPPER_UUID_WHITE ||
           uuid == FLIPPER_UUID_TRANSPARENT;
}

static bool scanFlipperUuidList(const uint8_t *data, size_t len, size_t step) {
    if (!data || len < step) return false;

    for (size_t i = 0; i + step <= len; i += step) {
        uint16_t uuid = 0;

        if (step == 2) {
            uuid = rrReadU16LE(data + i);
        } else if (step == 4) {
            uuid = (uint16_t)(rrReadU32LE(data + i) & 0xFFFF);
        } else {
            continue;
        }

        if (isFlipperUuidValue(uuid)) return true;
    }

    return false;
}

static bool scan128BitForFlipperUuid(const uint8_t *data, size_t len) {
    if (!data || len < 16) return false;

    // GhostESP scans inside each 128-bit UUID because the Flipper value may
    // be embedded in the base UUID instead of presented as a simple 16-bit UUID.
    for (size_t i = 0; i + 2 <= len; i++) {
        if (isFlipperUuidValue(rrReadU16LE(data + i))) return true;
    }

    return false;
}

static bool detectFlipperUuidFromAdvPayload(const uint8_t *data, size_t len) {
    if (!data || len < 2) return false;

    const uint8_t *p = data;
    size_t remaining = len;

    while (remaining > 1) {
        uint8_t fieldLen = p[0];
        if (fieldLen == 0 || (size_t)(fieldLen + 1) > remaining) break;

        uint8_t fieldType = p[1];
        const uint8_t *payload = p + 2;
        uint8_t payloadLen = (fieldLen >= 1) ? (uint8_t)(fieldLen - 1) : 0;

        // 0x02 / 0x03 = partial / complete 16-bit UUID list
        if ((fieldType == 0x02 || fieldType == 0x03) && payloadLen >= 2) {
            if (scanFlipperUuidList(payload, payloadLen, 2)) return true;
        }
        // 0x04 / 0x05 = partial / complete 32-bit UUID list
        else if ((fieldType == 0x04 || fieldType == 0x05) && payloadLen >= 4) {
            if (scanFlipperUuidList(payload, payloadLen, 4)) return true;
        }
        // 0x06 / 0x07 = partial / complete 128-bit UUID list
        else if ((fieldType == 0x06 || fieldType == 0x07) && payloadLen >= 16) {
            for (size_t i = 0; i + 16 <= payloadLen; i += 16) {
                if (scan128BitForFlipperUuid(payload + i, 16)) return true;
            }
        }

        remaining -= (size_t)(fieldLen + 1);
        p += (size_t)(fieldLen + 1);
    }

    return false;
}
static uint16_t firstFlipperUuidFromList(const uint8_t *data, size_t len, size_t step) {
    if (!data || len < step) return 0;

    for (size_t i = 0; i + step <= len; i += step) {
        uint16_t uuid = 0;
        if (step == 2) {
            uuid = rrReadU16LE(data + i);
        } else if (step == 4) {
            uuid = (uint16_t)(rrReadU32LE(data + i) & 0xFFFF);
        }

        if (isFlipperUuidValue(uuid)) return uuid;
    }

    return 0;
}

static uint16_t firstFlipperUuidFrom128(const uint8_t *data, size_t len) {
    if (!data || len < 16) return 0;

    for (size_t i = 0; i + 2 <= len; i++) {
        uint16_t uuid = rrReadU16LE(data + i);
        if (isFlipperUuidValue(uuid)) return uuid;
    }

    return 0;
}

static uint16_t getFlipperUuidFromAdvPayload(const uint8_t *data, size_t len) {
    if (!data || len < 2) return 0;

    const uint8_t *p = data;
    size_t remaining = len;

    while (remaining > 1) {
        uint8_t fieldLen = p[0];
        if (fieldLen == 0 || (size_t)(fieldLen + 1) > remaining) break;

        uint8_t fieldType = p[1];
        const uint8_t *payload = p + 2;
        uint8_t payloadLen = (fieldLen >= 1) ? (uint8_t)(fieldLen - 1) : 0;
        uint16_t uuid = 0;

        if ((fieldType == 0x02 || fieldType == 0x03) && payloadLen >= 2) {
            uuid = firstFlipperUuidFromList(payload, payloadLen, 2);
        } else if ((fieldType == 0x04 || fieldType == 0x05) && payloadLen >= 4) {
            uuid = firstFlipperUuidFromList(payload, payloadLen, 4);
        } else if ((fieldType == 0x06 || fieldType == 0x07) && payloadLen >= 16) {
            for (size_t i = 0; i + 16 <= payloadLen; i += 16) {
                uuid = firstFlipperUuidFrom128(payload + i, 16);
                if (uuid) break;
            }
        }

        if (uuid) return uuid;

        remaining -= (size_t)(fieldLen + 1);
        p += (size_t)(fieldLen + 1);
    }

    return 0;
}

static uint16_t getFlipperUuidFromServiceUUIDs(BLEAdvertisedDevice &dev) {
    if (!dev.haveServiceUUID()) return 0;

    int uuidCount = dev.getServiceUUIDCount();
    for (int i = 0; i < uuidCount; i++) {
        BLEUUID uuid = dev.getServiceUUID(i);

        if (uuid.bitSize() == 16) {
            uint16_t u16 = uuid.getNative()->uuid.uuid16;
            if (isFlipperUuidValue(u16)) return u16;
        }

        String uuidStr = String(uuid.toString().c_str());
        uuidStr.toLowerCase();
        if (uuidStr.indexOf("3081") >= 0) return FLIPPER_UUID_BLACK;
        if (uuidStr.indexOf("3082") >= 0) return FLIPPER_UUID_WHITE;
        if (uuidStr.indexOf("3083") >= 0) return FLIPPER_UUID_TRANSPARENT;
    }

    return 0;
}

static const char *flipperColorFromUuid(uint16_t uuid) {
    if (uuid == FLIPPER_UUID_BLACK)       return "Black";
    if (uuid == FLIPPER_UUID_WHITE)       return "White";
    if (uuid == FLIPPER_UUID_TRANSPARENT) return "Transparent";
    return "Unknown";
}

static const char *detectFlipperColor(BLEAdvertisedDevice &dev) {
    if (FLIPPER_UUID_DETECT_ENABLED) {
        uint8_t *payload = dev.getPayload();
        size_t payloadLen = dev.getPayloadLength();
        uint16_t uuid = getFlipperUuidFromAdvPayload(payload, payloadLen);
        if (uuid) return flipperColorFromUuid(uuid);
    }

    uint16_t svcUuid = getFlipperUuidFromServiceUUIDs(dev);
    if (svcUuid) return flipperColorFromUuid(svcUuid);

    return "Unknown";
}


// Flipper Zero detection — multiple independent signals, any match wins:
//
//  1. Advertised name contains one of the configured Flipper strings
//     from config.h (case-insensitive).
//
//  2. Raw advertisement UUID fields match GhostESP-style Flipper UUID
//     detection: 0x3081, 0x3082, or 0x3083.
//
//  3. Advertised service UUID matches one of the common Flipper BLE
//     UUIDs: 0x3081, 0x3082, or 0x3083.
//
//  4. OUI prefix 0C:FA:22 — only valid for non-random/static addresses.
//     Kept as a weak fallback because Flipper often uses private BLE
//     addresses, so this may not always fire.
//
static bool detectFlipper(BLEAdvertisedDevice &dev) {

    // ── Method 1: Configurable name check ────────────────────────
    if (dev.haveName()) {
        String n = String(dev.getName().c_str());
        n.toLowerCase();

        for (int i = 0; i < FLIPPER_NAME_MATCH_COUNT; i++) {
            String token = String(FLIPPER_NAME_MATCHES[i]);
            token.toLowerCase();
            if (token.length() && n.indexOf(token) >= 0) return true;
        }
    }

    // ── Method 2: GhostESP-style raw advertisement UUID fields ───
    // This catches UUIDs in partial/complete 16-bit, 32-bit, and 128-bit
    // AD fields, even when the Arduino BLE wrapper does not expose them
    // through haveServiceUUID()/getServiceUUID().
    if (FLIPPER_UUID_DETECT_ENABLED) {
        uint8_t *payload = dev.getPayload();
        size_t payloadLen = dev.getPayloadLength();
        if (payload && payloadLen > 0 && detectFlipperUuidFromAdvPayload(payload, payloadLen)) {
            return true;
        }
    }

    // ── Method 3: Common Flipper service UUIDs ───────────────────
    if (dev.haveServiceUUID()) {
        int uuidCount = dev.getServiceUUIDCount();
        for (int i = 0; i < uuidCount; i++) {
            BLEUUID uuid = dev.getServiceUUID(i);

            if (uuid.bitSize() == 16) {
                uint16_t u16 = uuid.getNative()->uuid.uuid16;
                if (u16 == 0x3081 || u16 == 0x3082 || u16 == 0x3083) return true;
            }

            String uuidStr = String(uuid.toString().c_str());
            uuidStr.toLowerCase();
            if (uuidStr == "00003081-0000-1000-8000-00805f9b34fb") return true;
            if (uuidStr == "00003082-0000-1000-8000-00805f9b34fb") return true;
            if (uuidStr == "00003083-0000-1000-8000-00805f9b34fb") return true;

            // Fallback in case the UUID string is shortened differently
            if (uuidStr.indexOf("3081") >= 0) return true;
            if (uuidStr.indexOf("3082") >= 0) return true;
            if (uuidStr.indexOf("3083") >= 0) return true;
        }
    }

    // ── Method 4: OUI check (bonus – rarely fires with random addr) ─
    std::string mac = dev.getAddress().toString();
    if (mac.length() >= 8) {
        std::string oui = mac.substr(0, 8);
        for (char &c : oui) c = tolower(c);
        if (oui == "0c:fa:22") return true;
    }

    return false;
}


// nyanBOX / Nyan Devices badge detection.
// Reference logic adapted from nyanBOX detector: service UUID + optional
// manufacturer data where FF FF is followed by level and packed version.
static bool detectNyanBox(BLEAdvertisedDevice &dev) {
    if (!dev.haveServiceUUID()) return false;

    String target = String(NYANBOX_SERVICE_UUID);
    target.toLowerCase();

    int uuidCount = dev.getServiceUUIDCount();
    for (int i = 0; i < uuidCount; i++) {
        String uuidStr = String(dev.getServiceUUID(i).toString().c_str());
        uuidStr.toLowerCase();
        if (uuidStr == target) return true;
        if (uuidStr.indexOf("6e79616e") >= 0 && uuidStr.indexOf("636521") >= 0) return true;
    }
    return false;
}

// Axon-style BLE detection. Reference logic checks for devices whose
// advertised BLE address starts with the configured OUI/MAC prefix.
static bool detectAxon(BLEAdvertisedDevice &dev) {
    String mac = dev.getAddress().toString().c_str();
    mac.toLowerCase();

    String prefix = String(AXON_MAC_PREFIX);
    prefix.toLowerCase();

    return mac.startsWith(prefix);
}

// Tesla BLE name-pattern detector. Inspired by TeslaScanner, but guarded so
// index 17 is only read when the advertised name is long enough.
static bool detectTeslaName(BLEAdvertisedDevice &dev) {
    if (!dev.haveName()) return false;

    String nm = dev.getName().c_str();
    nm.trim();

    if (nm.length() <= TESLA_NAME_END_INDEX) return false;

    return (nm.charAt(0) == TESLA_NAME_START_CHAR &&
            nm.charAt(TESLA_NAME_END_INDEX) == TESLA_NAME_END_CHAR);
}

static void parseNyanBoxManufacturer(BLEAdvertisedDevice &dev, uint16_t &level, char *version, size_t versionLen) {
    level = 0;
    if (version && versionLen) {
        strncpy(version, "Unknown", versionLen - 1);
        version[versionLen - 1] = '\0';
    }

    if (!dev.haveManufacturerData()) return;
    std::string m = dev.getManufacturerData();
    if (m.length() < 8) return;

    const uint8_t *b = (const uint8_t *)m.data();
    if (b[0] != 0xFF || b[1] != 0xFF) return;

    level = ((uint16_t)b[2] << 8) | b[3];
    uint32_t versionNum = ((uint32_t)b[4] << 24) |
                          ((uint32_t)b[5] << 16) |
                          ((uint32_t)b[6] << 8)  |
                           (uint32_t)b[7];

    int major = versionNum / 10000;
    int minor = (versionNum / 100) % 100;
    int patch = versionNum % 100;

    if (!version || !versionLen) return;
    if (minor == 0 && patch == 0) {
        snprintf(version, versionLen, "v%d", major);
    } else if (patch == 0) {
        snprintf(version, versionLen, "v%d.%d", major, minor);
    } else {
        snprintf(version, versionLen, "v%d.%d.%d", major, minor, patch);
    }
}

static int findNyanBoxByMac(const char *mac) {
    for (int i = 0; i < nyanEntryCount; i++) {
        if (strcmp(nyanEntries[i].mac, mac) == 0) return i;
    }
    return -1;
}

static void sortNyanBoxByRSSI() {
    for (int i = 0; i < nyanEntryCount - 1; i++) {
        for (int j = 0; j < nyanEntryCount - 1 - i; j++) {
            if (nyanEntries[j].rssi < nyanEntries[j + 1].rssi) {
                NyanBoxEntry tmp = nyanEntries[j];
                nyanEntries[j] = nyanEntries[j + 1];
                nyanEntries[j + 1] = tmp;
            }
        }
    }
}

static void upsertNyanBoxDevice(BLEAdvertisedDevice &dev) {
    String macStr = dev.getAddress().toString().c_str();
    int idx = findNyanBoxByMac(macStr.c_str());
    if (idx < 0) {
        if (nyanEntryCount >= MAX_NYANBOX_RESULTS) return;
        idx = nyanEntryCount++;
        memset(&nyanEntries[idx], 0, sizeof(NyanBoxEntry));
        strncpy(nyanEntries[idx].mac, macStr.c_str(), sizeof(nyanEntries[idx].mac) - 1);
        strncpy(nyanEntries[idx].name, "Unknown", sizeof(nyanEntries[idx].name) - 1);
        strncpy(nyanEntries[idx].version, "Unknown", sizeof(nyanEntries[idx].version) - 1);
    }

    String nm = dev.haveName() ? dev.getName().c_str() : "Unknown";
    strncpy(nyanEntries[idx].name, nm.c_str(), sizeof(nyanEntries[idx].name) - 1);
    nyanEntries[idx].name[sizeof(nyanEntries[idx].name) - 1] = '\0';

    nyanEntries[idx].rssi = (int8_t)dev.getRSSI();
    nyanEntries[idx].lastSeen = millis();
    parseNyanBoxManufacturer(dev, nyanEntries[idx].level,
                             nyanEntries[idx].version,
                             sizeof(nyanEntries[idx].version));
}

static int doNyanBoxScan(int durationSec, const char *targetMac = nullptr) {
    ensureBLEInit();
    WiFi.disconnect();
    delay(50);

    BLEScan *pScan = BLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(150);
    pScan->setWindow(140);

    BLEScanResults results = pScan->start(durationSec, false);
    int total = results.getCount();

    for (int i = 0; i < total; i++) {
        BLEAdvertisedDevice dev = results.getDevice(i);
        String macStr = dev.getAddress().toString().c_str();

        if (targetMac && targetMac[0]) {
            if (macStr.equalsIgnoreCase(String(targetMac))) {
                upsertNyanBoxDevice(dev);
            }
            continue;
        }

        if (detectNyanBox(dev)) {
            upsertNyanBoxDevice(dev);
        }
    }

    pScan->clearResults();
    sortNyanBoxByRSSI();
    return nyanEntryCount;
}

static const char *nyanSignalQuality(int8_t rssi) {
    if (rssi >= -50) return "EXCELLENT";
    if (rssi >= -60) return "VERY GOOD";
    if (rssi >= -70) return "GOOD";
    if (rssi >= -80) return "FAIR";
    return "WEAK";
}

static const char *axonSignalQuality(int8_t rssi) {
    if (rssi >= -50) return "EXCELLENT";
    if (rssi >= -60) return "VERY GOOD";
    if (rssi >= -70) return "GOOD";
    if (rssi >= -80) return "FAIR";
    return "WEAK";
}

static int findAxonByMac(const char *mac) {
    for (int i = 0; i < axonEntryCount; i++) {
        if (strcmp(axonEntries[i].mac, mac) == 0) return i;
    }
    return -1;
}

static void sortAxonByRSSI() {
    for (int i = 0; i < axonEntryCount - 1; i++) {
        for (int j = 0; j < axonEntryCount - 1 - i; j++) {
            if (axonEntries[j].rssi < axonEntries[j + 1].rssi) {
                AxonEntry tmp = axonEntries[j];
                axonEntries[j] = axonEntries[j + 1];
                axonEntries[j + 1] = tmp;
            }
        }
    }
}

static void upsertAxonDevice(BLEAdvertisedDevice &dev) {
    String macStr = dev.getAddress().toString().c_str();
    int idx = findAxonByMac(macStr.c_str());
    if (idx < 0) {
        if (axonEntryCount >= MAX_AXON_RESULTS) return;
        idx = axonEntryCount++;
        memset(&axonEntries[idx], 0, sizeof(AxonEntry));
        strncpy(axonEntries[idx].mac, macStr.c_str(), sizeof(axonEntries[idx].mac) - 1);
        strncpy(axonEntries[idx].name, "Axon Device", sizeof(axonEntries[idx].name) - 1);
    }

    String nm = dev.haveName() ? dev.getName().c_str() : "Axon Device";
    strncpy(axonEntries[idx].name, nm.c_str(), sizeof(axonEntries[idx].name) - 1);
    axonEntries[idx].name[sizeof(axonEntries[idx].name) - 1] = '\0';

    axonEntries[idx].rssi = (int8_t)dev.getRSSI();
    axonEntries[idx].lastSeen = millis();
}

static int doAxonScan(int durationSec, const char *targetMac = nullptr) {
    ensureBLEInit();
    WiFi.disconnect();
    delay(50);

    BLEScan *pScan = BLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(150);
    pScan->setWindow(140);

    BLEScanResults results = pScan->start(durationSec, false);
    int total = results.getCount();

    for (int i = 0; i < total; i++) {
        BLEAdvertisedDevice dev = results.getDevice(i);
        String macStr = dev.getAddress().toString().c_str();

        if (targetMac && targetMac[0]) {
            if (macStr.equalsIgnoreCase(String(targetMac))) {
                upsertAxonDevice(dev);
            }
            continue;
        }

        if (detectAxon(dev)) {
            upsertAxonDevice(dev);
        }
    }

    pScan->clearResults();
    sortAxonByRSSI();
    return axonEntryCount;
}

// Short manufacturer hint string for list row display
static const char *mfgHintStr(BLEDeviceType t) {
    switch (t) {
        case BLE_AIRTAG:  return "[AirTag]";
        case BLE_FLIPPER: return "[Flipper]";
        case BLE_SKIMMER: return "[Skimmer?]";
        case BLE_META:    return "[Meta]";
        case BLE_NYANBOX: return "[nyanBOX]";
        case BLE_AXON:    return "[Axon]";
        case BLE_TESLA:   return "[Tesla]";
        case BLE_APPLE:   return "[Apple]";
        default:          return "";
    }
}

// RSSI colour (reuse WiFi palette)
static lv_color_t bleRssiColor(int8_t rssi) {
    if (rssi >= -55) return lv_color_hex(TH.success);
    if (rssi >= -70) return lv_color_hex(TH.warn);
    return              lv_color_hex(TH.alert);
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

    BLEScanResults results = pScan->start(durationSec, false);
    int total = results.getCount();

    bleEntryCount = 0;
    for (int i = 0; i < total && bleEntryCount < MAX_BLE_RESULTS; i++) {
        BLEAdvertisedDevice dev = results.getDevice(i);

        // Classify device
        BLEDeviceType dtype = BLE_GENERIC;
        if      (detectFlipper(dev)) dtype = BLE_FLIPPER;
        else if (detectNyanBox(dev)) dtype = BLE_NYANBOX;
        else if (detectAxon(dev))    dtype = BLE_AXON;
        else if (detectTeslaName(dev)) dtype = BLE_TESLA;
        else if (detectAirTag(dev))  dtype = BLE_AIRTAG;
        else if (detectApple(dev))   dtype = BLE_APPLE;
        else if (detectMeta(dev))    dtype = BLE_META;
        else if (dev.haveName()) {
            // Skimmer check — suspicious serial/BLE module names from config.h.
            String nm = dev.getName().c_str();
            nm.trim();

            for (int s = 0; s < SKIMMER_NAME_MATCH_COUNT; s++) {
                if (nm.equalsIgnoreCase(SKIMMER_NAME_MATCHES[s])) {
                    dtype = BLE_SKIMMER;
                    break;
                }
            }
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

        strncpy(bleEntries[bleEntryCount].flipperColor,
                dtype == BLE_FLIPPER ? detectFlipperColor(dev) : "",
                sizeof(bleEntries[bleEntryCount].flipperColor) - 1);
        bleEntries[bleEntryCount].flipperColor[sizeof(bleEntries[bleEntryCount].flipperColor) - 1] = '\0';

        bleEntryCount++;
    }

    pScan->clearResults();
    sortBLEByRSSI();
    return bleEntryCount;
}

// ════════════════════════════════════════════════════════════════
//  TOOL 5 – PWNAGOTCHI WATCH
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
// ════════════════════════════════════════════════════════════════
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
    playPwnagotchiChirp();
}

static void pwn_refresh_cb(lv_timer_t *) {
    if (!pwnStatusLbl || !pwnList) return;

    processPwnPending();

    // Status label
    if (pwnCount == 0) {
        lv_label_set_text(pwnStatusLbl,
            LV_SYMBOL_EYE_OPEN "  Watching... no Pwnagotchi seen");
        lv_obj_set_style_text_color(pwnStatusLbl,
            lv_color_hex(TH.textDim), LV_PART_MAIN);
    } else {
        char buf[56];
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_WARNING "  %d Pwnagotchi%s detected!",
                 pwnCount, pwnCount == 1 ? "" : "s");
        lv_label_set_text(pwnStatusLbl, buf);
        lv_obj_set_style_text_color(pwnStatusLbl,
            lv_color_hex(TH.alert), LV_PART_MAIN);
    }

    // Rebuild list
    lv_obj_clean(pwnList);
    if (pwnCount == 0) {
        lv_obj_t *e = lv_list_add_text(pwnList,
            "Hopping ch 1-13 — waiting for beacon...");
        if (e) lv_obj_set_style_text_color(e,
                    lv_color_hex(TH.textDim), LV_PART_MAIN);
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
                lv_color_hex(TH.alert), LV_PART_MAIN);
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
        lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(pwnStatusLbl, 8, 30);

    pwnList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(pwnList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(pwnList, 0, 48);
    lv_obj_set_style_bg_color(pwnList,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pwnList,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(pwnList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pwnList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(pwnList,      1, LV_PART_MAIN);

    lv_obj_t *initLbl =
        lv_list_add_text(pwnList, "Hopping ch 1-13 — waiting for beacon...");
    if (initLbl)
        lv_obj_set_style_text_color(initLbl,
            lv_color_hex(TH.textDim), LV_PART_MAIN);

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
//  TOOL 7 – FLOCK SAFETY DETECTOR
//
//  Flock/Penguin-style devices can beacon / probe with SSIDs containing
//  Flock-related keywords. We sniff beacon frames (mgmt subtype 8),
//  probe responses (5), and probe requests (4), parse the SSID IE,
//  and alert on configured case-insensitive keyword matches. Alerts are
//  latching — stays red until you navigate back. Hops all 2.4 GHz channels.
//  Reference: github.com/GainSec/Flock-Safety-Trap-Shooter-Sniffer-Alarm
// ════════════════════════════════════════════════════════════════
static lv_obj_t *flockStatusLbl = nullptr;
static lv_obj_t *flockList      = nullptr;

// Small IRAM-safe helpers for case-insensitive keyword matching.
// Avoids strstr/strcasecmp inside the promiscuous sniffer callback.
static char IRAM_ATTR flockToLower(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static bool IRAM_ATTR flockContainsKeyword(const char *ssid, const char *keyword) {
    if (!ssid || !keyword || !keyword[0]) return false;

    for (int i = 0; ssid[i]; i++) {
        int j = 0;
        while (keyword[j] && ssid[i + j] &&
               flockToLower(ssid[i + j]) == flockToLower(keyword[j])) {
            j++;
        }
        if (keyword[j] == '\0') return true;
    }
    return false;
}

static bool IRAM_ATTR containsFlockKeyword(const char *ssid) {
    return flockContainsKeyword(ssid, FLOCK_KEYWORD_1) ||
           flockContainsKeyword(ssid, FLOCK_KEYWORD_2) ||
           flockContainsKeyword(ssid, FLOCK_KEYWORD_3) ||
           flockContainsKeyword(ssid, FLOCK_KEYWORD_4);
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

            if (containsFlockKeyword(ssid)) {
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

        // Deduplicate hits. MAC-based dedupe prevents repeated frames from
        // the same device from inflating the count, while still allowing
        // multiple devices that share the same SSID to be listed separately.
        bool found = false;
        for (int i = 0; i < flockHitCount; i++) {
#if FLOCK_DEDUPE_BY_MAC
            bool sameHit = (strcmp(flockHits[i].src, src) == 0);
#else
            bool sameHit = (strcmp(flockHits[i].ssid, ssid) == 0);
#endif
            if (sameHit) {
                strncpy(flockHits[i].ssid, ssid, 32);
                flockHits[i].ssid[32] = '\0';
                flockHits[i].frameType = ft;
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
            playFlockChirp();
        }
    }

    // Update status label
    if (flockHitCount == 0) {
        lv_label_set_text(flockStatusLbl,
            LV_SYMBOL_EYE_OPEN "  Watching... no Flock seen");
        lv_obj_set_style_text_color(flockStatusLbl,
            lv_color_hex(TH.textDim), LV_PART_MAIN);
    } else {
        char buf[56];
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_WARNING "  %d Flock hit%s detected!",
                 flockHitCount, flockHitCount == 1 ? "" : "s");
        lv_label_set_text(flockStatusLbl, buf);
        lv_obj_set_style_text_color(flockStatusLbl,
            lv_color_hex(TH.alert), LV_PART_MAIN);
    }

    // Rebuild list
    lv_obj_clean(flockList);
    if (flockHitCount == 0) {
        lv_obj_t *e = lv_list_add_text(flockList,
            "Hopping ch 1-13 — watching beacons & probes...");
        if (e) lv_obj_set_style_text_color(e,
                    lv_color_hex(TH.textDim), LV_PART_MAIN);
        return;
    }
    for (int i = 0; i < flockHitCount; i++) {
        char row[104];
#if FLOCK_SHOW_SOURCE_MAC
        snprintf(row, sizeof(row), "%s  %s  %ddBm\n%s",
                 flockHits[i].frameType ? "PROBE" : "BEACON",
                 flockHits[i].ssid,
                 flockHits[i].rssi,
                 flockHits[i].src);
#else
        snprintf(row, sizeof(row), "%s  %s  %ddBm",
                 flockHits[i].frameType ? "PROBE" : "BEACON",
                 flockHits[i].ssid,
                 flockHits[i].rssi);
#endif
        lv_obj_t *entry = lv_list_add_text(flockList, row);
        if (entry)
            lv_obj_set_style_text_color(entry,
                lv_color_hex(TH.alert), LV_PART_MAIN);
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
        "Sniffs beacons & probes for Flock keywords\n"
        "flock / penguin / pigvision / fs battery");
    lv_obj_set_style_text_color(flockStatusLbl,
        lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(flockStatusLbl, 8, 30);

    flockList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(flockList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(flockList, 0, 48);
    lv_obj_set_style_bg_color(flockList,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(flockList,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(flockList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(flockList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(flockList,      1, LV_PART_MAIN);

    lv_obj_t *initLbl =
        lv_list_add_text(flockList, "Hopping ch 1-13 — watching beacons & probes...");
    if (initLbl)
        lv_obj_set_style_text_color(initLbl,
            lv_color_hex(TH.textDim), LV_PART_MAIN);

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
//  FLOCK HYBRID SCANNER — BLE phase + WiFi phase, merged list
// ════════════════════════════════════════════════════════════════
static const char *hybridSignalQuality(int8_t rssi) {
    if (rssi >= -50) return "VERY GOOD";
    if (rssi >= -65) return "GOOD";
    if (rssi >= -80) return "FAIR";
    return "WEAK";
}

static void hybridUpsertHit(const char *source, const char *name, const char *mac,
                            int8_t rssi, const char *reason) {
    if (!source || !name || !mac || !reason) return;

    for (int i = 0; i < hybridHitCount; i++) {
        if (strcmp(hybridHits[i].source, source) == 0 &&
            strcmp(hybridHits[i].mac, mac) == 0) {
            strncpy(hybridHits[i].name, name, sizeof(hybridHits[i].name) - 1);
            hybridHits[i].name[sizeof(hybridHits[i].name) - 1] = '\0';
            strncpy(hybridHits[i].reason, reason, sizeof(hybridHits[i].reason) - 1);
            hybridHits[i].reason[sizeof(hybridHits[i].reason) - 1] = '\0';
            hybridHits[i].rssi = rssi;
            hybridHits[i].lastSeen = millis();
            return;
        }
    }

    if (hybridHitCount >= MAX_FLOCK_HYBRID_HITS) return;
    FlockHybridHit &h = hybridHits[hybridHitCount++];
    memset(&h, 0, sizeof(h));
    strncpy(h.source, source, sizeof(h.source) - 1);
    strncpy(h.name, name, sizeof(h.name) - 1);
    strncpy(h.mac, mac, sizeof(h.mac) - 1);
    strncpy(h.reason, reason, sizeof(h.reason) - 1);
    h.rssi = rssi;
    h.lastSeen = millis();
    playFlockChirp();
}

static void hybridSortByRSSI() {
    for (int i = 0; i < hybridHitCount - 1; i++) {
        for (int j = 0; j < hybridHitCount - 1 - i; j++) {
            if (hybridHits[j].rssi < hybridHits[j + 1].rssi) {
                FlockHybridHit tmp = hybridHits[j];
                hybridHits[j] = hybridHits[j + 1];
                hybridHits[j + 1] = tmp;
            }
        }
    }
}

static void hybridProcessPendingWifi() {
    if (!hybridPendingReady) return;
    char name[33], mac[18], reason[24];
    int8_t rssi = hybridPendingRSSI;
    memcpy(name, hybridPendingName, sizeof(name));
    memcpy(mac, hybridPendingMac, sizeof(mac));
    memcpy(reason, hybridPendingReason, sizeof(reason));
    hybridPendingReady = false;
    hybridUpsertHit("WiFi", name, mac, rssi, reason);
}

static void hybridRebuildList() {
    if (!hybridList) return;
    hybridSortByRSSI();
    lv_obj_clean(hybridList);

    if (hybridHitCount == 0) {
        lv_obj_t *e = lv_list_add_text(hybridList,
            "No combined Flock hits yet. Press Start Scan.");
        if (e) lv_obj_set_style_text_color(e, lv_color_hex(TH.textDim), LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < hybridHitCount; i++) {
        uint32_t age = (millis() - hybridHits[i].lastSeen) / 1000UL;
        char row[132];
#if FLOCK_HYBRID_SHOW_MAC
        snprintf(row, sizeof(row), "%s  %s  %ddBm\n%s  %s  %lus",
                 hybridHits[i].source,
                 hybridHits[i].name,
                 hybridHits[i].rssi,
                 hybridHits[i].mac,
                 hybridHits[i].reason,
                 (unsigned long)age);
#else
        snprintf(row, sizeof(row), "%s  %s  %ddBm  %s",
                 hybridHits[i].source,
                 hybridHits[i].name,
                 hybridHits[i].rssi,
                 hybridHits[i].reason);
#endif
        lv_obj_t *entry = lv_list_add_text(hybridList, row);
        if (entry) {
            lv_obj_set_style_text_color(entry,
                strcmp(hybridHits[i].source, "BLE") == 0 ? lv_color_hex(TH.accent) : lv_color_hex(TH.alert),
                LV_PART_MAIN);
        }
    }
}

static const char *hybridKeywordReason(const char *name) {
    if (flockContainsKeyword(name, FLOCK_KEYWORD_1)) return FLOCK_KEYWORD_1;
    if (flockContainsKeyword(name, FLOCK_KEYWORD_2)) return FLOCK_KEYWORD_2;
    if (flockContainsKeyword(name, FLOCK_KEYWORD_3)) return FLOCK_KEYWORD_3;
    if (flockContainsKeyword(name, FLOCK_KEYWORD_4)) return "fs battery";
    return "keyword";
}

static bool hybridNameIsTenDigits(const char *name) {
    if (!name) return false;
    int len = 0;
    for (; name[len]; len++) {
        if (name[len] < '0' || name[len] > '9') return false;
    }
    return len == 10;
}

static bool detectFlockBLE(BLEAdvertisedDevice &dev, char *reason, size_t reasonLen) {
    if (reason && reasonLen) reason[0] = '\0';

    if (dev.haveName()) {
        String nm = String(dev.getName().c_str());
        char nbuf[33];
        strncpy(nbuf, nm.c_str(), sizeof(nbuf) - 1);
        nbuf[sizeof(nbuf) - 1] = '\0';

        if (containsFlockKeyword(nbuf)) {
            snprintf(reason, reasonLen, "Name:%s", hybridKeywordReason(nbuf));
            return true;
        }
        if (hybridNameIsTenDigits(nbuf)) {
            snprintf(reason, reasonLen, "10-digit name");
            return true;
        }
    }

    if (dev.haveManufacturerData()) {
        std::string md = dev.getManufacturerData();
        if (md.length() >= 2) {
            uint8_t b0 = (uint8_t)md[0];
            uint8_t b1 = (uint8_t)md[1];
            // XUNTONG 0x09C8 may appear little-endian in manufacturer data.
            if ((b0 == 0xC8 && b1 == 0x09) || (b0 == 0x09 && b1 == 0xC8)) {
                snprintf(reason, reasonLen, "MFG 09C8");
                return true;
            }
        }
    }

    return false;
}

static void hybrid_wifi_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!hybridWifiActive || type != WIFI_PKT_MGMT || hybridPendingReady) return;

    const wifi_promiscuous_pkt_t *pkt = reinterpret_cast<const wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *d = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;

    uint8_t fc0 = d[0];
    uint8_t ftype = (fc0 >> 2) & 0x03;
    uint8_t stype = (fc0 >> 4) & 0x0F;
    if (ftype != 0) return;
    if (stype != 8 && stype != 5 && stype != 4) return;

    int ieOffset = (stype == 4) ? 24 : 36;
    if (ieOffset >= len) return;

    const uint8_t *ie = d + ieOffset;
    int rem = len - ieOffset;
    while (rem >= 2) {
        uint8_t id = ie[0];
        uint8_t elen = ie[1];
        if (elen + 2 > rem) break;

        if (id == 0 && elen > 0) {
            int n = elen > 32 ? 32 : elen;
            char ssid[33];
            memcpy(ssid, ie + 2, n);
            ssid[n] = '\0';

            if (containsFlockKeyword(ssid)) {
                memcpy(hybridPendingName, ssid, n + 1);
                snprintf(hybridPendingMac, sizeof(hybridPendingMac),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         d[10], d[11], d[12], d[13], d[14], d[15]);
                snprintf(hybridPendingReason, sizeof(hybridPendingReason),
                         "%s", (stype == 4) ? "Probe" : "Beacon");
                hybridPendingRSSI = pkt->rx_ctrl.rssi;
                hybridPendingReady = true;
                return;
            }
        }
        ie += elen + 2;
        rem -= elen + 2;
    }
}

static void runFlockHybridCycle() {
    if (!hybridStatusLbl || !hybridList) return;

    startLEDSpinner(FLOCK_HYBRID_LED_R, FLOCK_HYBRID_LED_G, FLOCK_HYBRID_LED_B, FLOCK_HYBRID_LED_SPIN_MS);

    lv_label_set_text(hybridStatusLbl, LV_SYMBOL_BLUETOOTH "  BLE phase running...");
    lv_obj_set_style_text_color(hybridStatusLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
    lv_timer_handler();

    ensureBLEInit();
    WiFi.disconnect();
    delay(50);
    BLEScan *pScan = BLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(150);
    pScan->setWindow(140);
    BLEScanResults results = pScan->start(flockHybridBleSecs, false);
    int total = results.getCount();
    for (int i = 0; i < total; i++) {
        BLEAdvertisedDevice dev = results.getDevice(i);
        char reason[24];
        if (detectFlockBLE(dev, reason, sizeof(reason))) {
            String nm = dev.haveName() ? dev.getName().c_str() : "<unknown>";
            String mac = dev.getAddress().toString().c_str();
            hybridUpsertHit("BLE", nm.c_str(), mac.c_str(), (int8_t)dev.getRSSI(), reason);
        }
    }
    pScan->clearResults();
    hybridRebuildList();

    lv_label_set_text(hybridStatusLbl, LV_SYMBOL_WIFI "  WiFi phase running...");
    lv_obj_set_style_text_color(hybridStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_timer_handler();

    hybridPendingReady = false;
    hybridWifiActive = true;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(hybrid_wifi_sniffer_cb);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    uint8_t ch = 1;
    unsigned long startMs = millis();
    unsigned long lastHopMs = 0;
    unsigned long wifiMs = (unsigned long)flockHybridWifiSecs * 1000UL;

    while ((millis() - startMs) < wifiMs && hybridWifiActive) {
        if (hybridPendingReady) {
            hybridProcessPendingWifi();
            hybridRebuildList();
        }
        if (millis() - lastHopMs >= (unsigned long)flockHybridHopMs) {
            lastHopMs = millis();
            ch = (ch % 13) + 1;
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        }
        lv_timer_handler();
        delay(15);
    }

    hybridWifiActive = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    hybridProcessPendingWifi();
    hybridRebuildList();

    char buf[64];
    snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  Hybrid complete — %d hit%s",
             hybridHitCount, hybridHitCount == 1 ? "" : "s");
    stopLEDSpinner(FLOCK_HYBRID_LED_R, FLOCK_HYBRID_LED_G, FLOCK_HYBRID_LED_B);

    lv_label_set_text(hybridStatusLbl, buf);
    lv_obj_set_style_text_color(hybridStatusLbl,
        hybridHitCount ? lv_color_hex(TH.alert) : lv_color_hex(TH.textDim), LV_PART_MAIN);
}

static void cb_runFlockHybrid(lv_event_t *e) {
    runFlockHybridCycle();
}

static void cb_startFlockHybridTimer(lv_timer_t *t) {
    if (hybridStartTimer) { lv_timer_delete(hybridStartTimer); hybridStartTimer = nullptr; }
    runFlockHybridCycle();
}

void createFlockHybridScanner() {
    hybridHitCount = 0;
    hybridPendingReady = false;
    hybridWifiActive = false;
    hybridStatusLbl = nullptr;
    hybridList = nullptr;
    memset(hybridHits, 0, sizeof(hybridHits));

    if (wifiToolScreen) { lv_obj_delete(wifiToolScreen); wifiToolScreen = nullptr; }
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_WARNING "  Flock Hybrid");

    hybridStatusLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(hybridStatusLbl,
        "Ready. Press Start Scan to run BLE + WiFi.\n"
        "Merged hits show source, RSSI, reason.");
    lv_obj_set_style_text_color(hybridStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(hybridStatusLbl, 8, 30);

    hybridList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(hybridList, SCREEN_W, SCREEN_H - 82);
    lv_obj_set_pos(hybridList, 0, 50);
    lv_obj_set_style_bg_color(hybridList,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hybridList,       LV_OPA_COVER,        LV_PART_MAIN);
    lv_obj_set_style_border_width(hybridList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hybridList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(hybridList,      1, LV_PART_MAIN);
    hybridRebuildList();

    lv_obj_t *backBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    lv_obj_t *scanBtn = createActionBtn(wifiToolScreen, LV_SYMBOL_REFRESH "  Start Scan", cb_runFlockHybrid);

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, backBtn);
    lv_group_add_obj(wifiToolGroup, scanBtn);
    setGroup(wifiToolGroup);

    setAllLEDs(FLOCK_HYBRID_LED_R, FLOCK_HYBRID_LED_G, FLOCK_HYBRID_LED_B, LED_BRIGHTNESS);
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
static const char *BLE_TOOL_LABELS[] = {
    LV_SYMBOL_BLUETOOTH "  BLE Scanner",
    "\xEF\x80\xA6"      "  AirTag Detector",   // apple-ish symbol fallback
    LV_SYMBOL_WARNING   "  Flipper Detector",
    LV_SYMBOL_BLUETOOTH "  nyanBOX Detector",
    LV_SYMBOL_BLUETOOTH "  Axon Detector",
    LV_SYMBOL_BLUETOOTH "  Tesla Detector",
    LV_SYMBOL_WARNING   "  Skimmer Detector",
    LV_SYMBOL_EYE_OPEN  "  Meta Detector"
};
static const int BLE_TOOL_COUNT = sizeof(BLE_TOOL_LABELS) / sizeof(BLE_TOOL_LABELS[0]);

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
        case 3: createNyanBoxDetector(); break;
        case 4: createAxonDetector();    break;
        case 5: createTeslaDetector();   break;
        case 6: createSkimmerScanner();  break;
        case 7: createMetaDetector();    break;
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
    lv_obj_set_style_bg_color(list,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list,      6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list,      4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(TH.accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);

    deleteGroup(&bleMenuGroup);
    bleMenuGroup = lv_group_create();

    for (int i = 0; i < BLE_TOOL_COUNT; i++) {
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
            case BLE_APPLE:   col = lv_color_hex(TH.accent); break; // blue accent
            case BLE_TESLA:   col = lv_color_hex(0x58a6ff); break; // blue Tesla detector accent
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
    char scanMsg[40];
    snprintf(scanMsg, sizeof(scanMsg), LV_SYMBOL_REFRESH "  Scanning %ds...", bleScanSeconds);
    lv_label_set_text(bleScanStatusLbl, scanMsg);
    lv_obj_set_style_text_color(bleScanStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_timer_handler();

    // Blue spinner while BLE scan blocks core 1
    startLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);
    int found = doBLEScan(bleScanSeconds, BLE_GENERIC);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    for (int i = 0; i < found; i++) {
        if (bleEntries[i].type == BLE_SKIMMER) {
            playBLESuspiciousChirp();
            break;
        }
    }

    char buf[48];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_BLUETOOTH "  %d device%s found",
             found, found == 1 ? "" : "s");
    lv_label_set_text(bleScanStatusLbl, buf);
    lv_obj_set_style_text_color(bleScanStatusLbl,
        found > 0 ? lv_color_hex(TH.success) : lv_color_hex(TH.textDim),
        LV_PART_MAIN);

    rebuildBLEScanList();
}

void createBLEScanner() {
    if (bleToolScreen) { lv_obj_delete(bleToolScreen); bleToolScreen = nullptr; }
    bleToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleToolScreen);
    createHeader(bleToolScreen, LV_SYMBOL_BLUETOOTH "  BLE Scanner");

    bleScanStatusLbl = lv_label_create(bleToolScreen);
    char readyMsg[48];
    snprintf(readyMsg, sizeof(readyMsg), "Press Scan to start  (%ds)", bleScanSeconds);
    lv_label_set_text(bleScanStatusLbl, readyMsg);
    lv_obj_set_style_text_color(bleScanStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(bleScanStatusLbl, 8, 30);

    bleScanList = lv_list_create(bleToolScreen);
    lv_obj_set_size(bleScanList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(bleScanList, 0, 48);
    lv_obj_set_style_bg_color(bleScanList,     lv_color_hex(TH.bg), LV_PART_MAIN);
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
    lv_color_t  badgeCol  = lv_color_hex(TH.textDim);
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
            badgeCol = lv_color_hex(TH.alert);
            break;
        case BLE_META:
            badge    = "  Meta / RayBan Device";
            badgeCol = lv_color_hex(TH.accent);
            break;
        case BLE_TESLA:
            badge    = "  Tesla BLE Pattern";
            badgeCol = lv_color_hex(0x58a6ff);
            break;
        case BLE_APPLE:
            badge    = "  Apple Device";
            badgeCol = lv_color_hex(TH.accent);
            break;
        default:
            badge    = "  Generic BLE";
            badgeCol = lv_color_hex(TH.textDim);
            break;
    }

    // Info card
    lv_obj_t *card = lv_obj_create(bleDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,      lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,        LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card,  lv_color_hex(TH.border), LV_PART_MAIN);
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

    char info[240];
    if (bleEntries[idx].type == BLE_FLIPPER) {
        snprintf(info, sizeof(info),
                 "Name  : %s\n"
                 "Color : %s\n"
                 "MAC   : %s\n"
                 "RSSI  : %d dBm  (%s)",
                 bleEntries[idx].name,
                 bleEntries[idx].flipperColor[0] ? bleEntries[idx].flipperColor : "Unknown",
                 bleEntries[idx].mac,
                 rssi, quality);
    } else {
        snprintf(info, sizeof(info),
                 "Name  : %s\n"
                 "MAC   : %s\n"
                 "RSSI  : %d dBm  (%s)",
                 bleEntries[idx].name,
                 bleEntries[idx].mac,
                 rssi, quality);
    }

    lv_obj_t *infoLbl = lv_label_create(card);
    lv_label_set_text(infoLbl, info);
    lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(infoLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(infoLbl, LV_ALIGN_TOP_LEFT, 0, 16);

    // RSSI bar
    lv_obj_t *bar = lv_bar_create(bleDetailScreen);
    lv_obj_set_size(bar, SCREEN_W - 12, 5);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(bar, -100, -30);
    lv_bar_set_value(bar, rssi, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TH.barBg), LV_PART_MAIN);
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

// ════════════════════════════════════════════════════════════════
//  TOOL 2 – AIRTAG DETECTOR
// ════════════════════════════════════════════════════════════════
static lv_obj_t *airtagStatusLbl = nullptr;
static lv_obj_t *airtagList      = nullptr;

static void cb_doAirTagScan(lv_event_t *e) {
    lv_label_set_text(airtagStatusLbl, LV_SYMBOL_REFRESH "  Scanning 8s...");
    lv_obj_set_style_text_color(airtagStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_obj_clean(airtagList);
    lv_timer_handler();

    // White spinner for AirTag scan (Apple = white/silver)
    startLEDSpinner(200, 200, 200);
    int found = doBLEScan(bleScanSeconds, BLE_AIRTAG);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(airtagStatusLbl,
                          LV_SYMBOL_BLUETOOTH "  No AirTags detected");
        lv_obj_set_style_text_color(airtagStatusLbl,
                                    lv_color_hex(TH.success), LV_PART_MAIN);
        lv_obj_t *empty = lv_list_add_text(airtagList,
                                            "No Apple AirTags in range");
        if (empty)
            lv_obj_set_style_text_color(empty, lv_color_hex(TH.textDim), LV_PART_MAIN);
        return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WARNING "  %d AirTag%s detected!",
             found, found == 1 ? "" : "s");
    lv_label_set_text(airtagStatusLbl, buf);
    lv_obj_set_style_text_color(airtagStatusLbl, lv_color_hex(TH.alert), LV_PART_MAIN);

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
        "Detects Apple AirTags\n");
    lv_obj_set_style_text_color(airtagStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(airtagStatusLbl, 8, 30);

    airtagList = lv_list_create(bleToolScreen);
    lv_obj_set_size(airtagList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(airtagList, 0, 48);
    lv_obj_set_style_bg_color(airtagList,     lv_color_hex(TH.bg), LV_PART_MAIN);
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

// ════════════════════════════════════════════════════════════════
//  TOOL 3 – FLIPPER ZERO DETECTOR
// ════════════════════════════════════════════════════════════════
static lv_obj_t *flipperStatusLbl = nullptr;
static lv_obj_t *flipperList      = nullptr;
static lv_obj_t *flipperBackBtn   = nullptr;
static lv_obj_t *flipperScanBtn   = nullptr;

static void resetFlipperToolGroup() {
    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    if (flipperBackBtn) lv_group_add_obj(bleToolGroup, flipperBackBtn);
    if (flipperScanBtn) lv_group_add_obj(bleToolGroup, flipperScanBtn);
    setGroup(bleToolGroup);
}

static void cb_doFlipperScan(lv_event_t *e) {
    lv_label_set_text(flipperStatusLbl, LV_SYMBOL_REFRESH "  Scanning 8s...");
    lv_obj_set_style_text_color(flipperStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    resetFlipperToolGroup();
    lv_obj_clean(flipperList);
    lv_timer_handler();

    // Orange spinner for Flipper
    startLEDSpinner(220, 100, 0);
    int found = doBLEScan(bleScanSeconds, BLE_FLIPPER);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(flipperStatusLbl,
                          LV_SYMBOL_BLUETOOTH "  No Flipper Zero detected");
        lv_obj_set_style_text_color(flipperStatusLbl,
                                    lv_color_hex(TH.success), LV_PART_MAIN);
        lv_obj_t *empty = lv_list_add_text(flipperList,
                                            "No Flipper Zero in range");
        if (empty)
            lv_obj_set_style_text_color(empty, lv_color_hex(TH.textDim), LV_PART_MAIN);
        return;
    }

    playFlipperChirp();

    char buf[48];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WARNING "  %d Flipper%s detected!",
             found, found == 1 ? "" : "s");
    lv_label_set_text(flipperStatusLbl, buf);
    lv_obj_set_style_text_color(flipperStatusLbl, lv_color_hex(0xff9900), LV_PART_MAIN);

    resetFlipperToolGroup();

    for (int i = 0; i < found; i++) {
        char row[64];
        snprintf(row, sizeof(row), "%-14s %-11s %ddBm",
                 bleEntries[i].name,
                 bleEntries[i].flipperColor[0] ? bleEntries[i].flipperColor : "Unknown",
                 bleEntries[i].rssi);

        lv_obj_t *btn = lv_list_add_btn(flipperList, nullptr, row);
        if (btn) {
            styleListBtn(btn);
            lv_obj_set_style_text_color(btn, lv_color_hex(0xff9900), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
                createBLEDetail((int)(intptr_t)lv_event_get_user_data(ev));
            }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_group_add_obj(bleToolGroup, btn);
        }
    }
}

void createFlipperScanner() {
    if (bleToolScreen) { lv_obj_delete(bleToolScreen); bleToolScreen = nullptr; }
    bleToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleToolScreen);
    createHeader(bleToolScreen, LV_SYMBOL_WARNING "  Flipper Detector");

    flipperStatusLbl = lv_label_create(bleToolScreen);
    lv_label_set_text(flipperStatusLbl,
        "Detects Fipper Zeros");
    lv_obj_set_style_text_color(flipperStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(flipperStatusLbl, 8, 30);

    flipperList = lv_list_create(bleToolScreen);
    lv_obj_set_size(flipperList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(flipperList, 0, 48);
    lv_obj_set_style_bg_color(flipperList,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(flipperList,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_width(flipperList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(flipperList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(flipperList,      2, LV_PART_MAIN);

    flipperBackBtn = createBackBtn(bleToolScreen, cb_bleToolBack);
    flipperScanBtn = createActionBtn(bleToolScreen,
                                     LV_SYMBOL_REFRESH "  Scan",
                                     cb_doFlipperScan);

    resetFlipperToolGroup();

    lv_screen_load_anim(bleToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}


// ════════════════════════════════════════════════════════════════
//  BLE TOOL 4 – NYANBOX DETECTOR
//
//  BLE-only detector for nyanBOX / Nyan Devices badges. It checks
//  for the configured 128-bit service UUID and parses optional
//  manufacturer data for level and firmware version. Locate Mode
//  refreshes RSSI for the selected MAC on demand.
// ════════════════════════════════════════════════════════════════
static lv_obj_t *nyanStatusLbl = nullptr;
static lv_obj_t *nyanList      = nullptr;
static lv_obj_t *nyanBackBtn   = nullptr;
static lv_obj_t *nyanScanBtn   = nullptr;
static lv_obj_t *nyanLocateLbl = nullptr;
static int       nyanLocateIdx = -1;
static int       nyanDetailIdxForLocate = -1;

static void rebuildNyanBoxList() {
    if (!nyanList) return;

    // Rebuild focus group after lv_obj_clean() because list children are destroyed.
    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    if (nyanBackBtn) lv_group_add_obj(bleToolGroup, nyanBackBtn);
    if (nyanScanBtn) lv_group_add_obj(bleToolGroup, nyanScanBtn);

    lv_obj_clean(nyanList);

    if (nyanEntryCount == 0) {
        lv_obj_t *empty = lv_list_add_text(nyanList, "No nyanBOX devices found yet");
        if (empty) lv_obj_set_style_text_color(empty, lv_color_hex(TH.textDim), LV_PART_MAIN);
        setGroup(bleToolGroup);
        return;
    }

    for (int i = 0; i < nyanEntryCount; i++) {
        char nameTrunc[12];
        strncpy(nameTrunc, nyanEntries[i].name[0] ? nyanEntries[i].name : "Unknown", 11);
        nameTrunc[11] = '\0';

        char row[64];
        if (nyanEntries[i].level > 0) {
            snprintf(row, sizeof(row), "%s  L%u  %ddBm",
                     nameTrunc, nyanEntries[i].level, nyanEntries[i].rssi);
        } else {
            snprintf(row, sizeof(row), "%s  L?  %ddBm",
                     nameTrunc, nyanEntries[i].rssi);
        }

        lv_obj_t *btn = lv_list_add_btn(nyanList, nullptr, row);
        styleListBtn(btn);
        lv_obj_set_style_text_color(btn, bleRssiColor(nyanEntries[i].rssi), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            createNyanBoxDetail((int)(intptr_t)lv_event_get_user_data(ev));
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(bleToolGroup, btn);
    }

    setGroup(bleToolGroup);
}

static void cb_doNyanBoxScan(lv_event_t *e) {
    char msg[48];
    snprintf(msg, sizeof(msg), LV_SYMBOL_REFRESH "  Scanning %ds...", NYANBOX_SCAN_SECS);
    lv_label_set_text(nyanStatusLbl, msg);
    lv_obj_set_style_text_color(nyanStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_obj_clean(nyanList);
    lv_timer_handler();

    startLEDSpinner(120, 0, 220);
    nyanEntryCount = 0;
    memset(nyanEntries, 0, sizeof(nyanEntries));
    int found = doNyanBoxScan(NYANBOX_SCAN_SECS);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(nyanStatusLbl, LV_SYMBOL_BLUETOOTH "  No nyanBOX devices detected");
        lv_obj_set_style_text_color(nyanStatusLbl, lv_color_hex(TH.success), LV_PART_MAIN);
    } else {
        snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING "  %d nyanBOX device%s found!",
                 found, found == 1 ? "" : "s");
        lv_label_set_text(nyanStatusLbl, msg);
        lv_obj_set_style_text_color(nyanStatusLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
    }

    rebuildNyanBoxList();
}

void createNyanBoxDetector() {
    nyanStatusLbl = nullptr;
    nyanList      = nullptr;
    nyanBackBtn   = nullptr;
    nyanScanBtn   = nullptr;
    nyanLocateLbl = nullptr;
    nyanLocateIdx = -1;
    nyanDetailIdxForLocate = -1;

    if (bleToolScreen) { lv_obj_delete(bleToolScreen); bleToolScreen = nullptr; }
    bleToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleToolScreen);
    createHeader(bleToolScreen, LV_SYMBOL_BLUETOOTH "  nyanBOX Detector");

    nyanStatusLbl = lv_label_create(bleToolScreen);
    char readyMsg[64];
    snprintf(readyMsg, sizeof(readyMsg),
             "Detects NyanBOX Devices", NYANBOX_SCAN_SECS);
    lv_label_set_text(nyanStatusLbl, readyMsg);
    lv_obj_set_style_text_color(nyanStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(nyanStatusLbl, 8, 30);

    nyanList = lv_list_create(bleToolScreen);
    lv_obj_set_size(nyanList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(nyanList, 0, 48);
    lv_obj_set_style_bg_color(nyanList,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nyanList,       LV_OPA_COVER,        LV_PART_MAIN);
    lv_obj_set_style_border_width(nyanList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nyanList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(nyanList,      2, LV_PART_MAIN);

    lv_obj_t *empty = lv_list_add_text(nyanList, "Ready to scan for nyanBOX badges");
    if (empty) lv_obj_set_style_text_color(empty, lv_color_hex(TH.textDim), LV_PART_MAIN);

    nyanBackBtn = createBackBtn(bleToolScreen, cb_bleToolBack);
    nyanScanBtn = createActionBtn(bleToolScreen,
                                  LV_SYMBOL_REFRESH "  Scan",
                                  cb_doNyanBoxScan);

    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    lv_group_add_obj(bleToolGroup, nyanBackBtn);
    lv_group_add_obj(bleToolGroup, nyanScanBtn);
    setGroup(bleToolGroup);

    if (nyanEntryCount > 0) {
        char msg[56];
        snprintf(msg, sizeof(msg), LV_SYMBOL_BLUETOOTH "  %d saved nyanBOX result%s",
                 nyanEntryCount, nyanEntryCount == 1 ? "" : "s");
        lv_label_set_text(nyanStatusLbl, msg);
        lv_obj_set_style_text_color(nyanStatusLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
        rebuildNyanBoxList();
    }

    setAllLEDs(120, 0, 220, LED_BRIGHTNESS);
    lv_screen_load_anim(bleToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

static void cb_nyanDetailBack(lv_event_t *e) {
    cb_bleDetailBack(e);
}

static void cb_nyanLocate(lv_event_t *e) {
    if (nyanDetailIdxForLocate >= 0) createNyanBoxLocate(nyanDetailIdxForLocate);
}

void createNyanBoxDetail(int idx) {
    if (idx < 0 || idx >= nyanEntryCount) return;
    nyanDetailIdxForLocate = idx;

    if (bleDetailScreen) { lv_obj_delete(bleDetailScreen); bleDetailScreen = nullptr; }
    bleDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleDetailScreen);

    char hdr[48];
    snprintf(hdr, sizeof(hdr), LV_SYMBOL_BLUETOOTH "  %.24s", nyanEntries[idx].name);
    createHeader(bleDetailScreen, hdr);

    lv_obj_t *card = lv_obj_create(bleDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,      lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,        LV_OPA_COVER,          LV_PART_MAIN);
    lv_obj_set_style_border_color(card,  lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card,  1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,        6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,       6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    uint32_t ageSec = (millis() - nyanEntries[idx].lastSeen) / 1000;
    char info[220];
    snprintf(info, sizeof(info),
             "nyanBOX / Nyan Device\n"
             "Name : %s\n"
             "MAC  : %s\n"
             "RSSI : %d dBm (%s)\n"
             "Level: %s%u\n"
             "FW   : %s\n"
             "Age  : %lus",
             nyanEntries[idx].name,
             nyanEntries[idx].mac,
             nyanEntries[idx].rssi,
             nyanSignalQuality(nyanEntries[idx].rssi),
             nyanEntries[idx].level > 0 ? "" : "?",
             nyanEntries[idx].level,
             nyanEntries[idx].version,
             (unsigned long)ageSec);

    lv_obj_t *infoLbl = lv_label_create(card);
    lv_label_set_text(infoLbl, info);
    lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(infoLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(infoLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *backBtn = createBackBtn(bleDetailScreen, cb_nyanDetailBack);
    lv_obj_t *locateBtn = createActionBtn(bleDetailScreen,
                                          LV_SYMBOL_EYE_OPEN "  Locate",
                                          cb_nyanLocate);

    deleteGroup(&bleDetailGroup);
    bleDetailGroup = lv_group_create();
    lv_group_add_obj(bleDetailGroup, backBtn);
    lv_group_add_obj(bleDetailGroup, locateBtn);
    setGroup(bleDetailGroup);

    lv_screen_load_anim(bleDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

static void updateNyanLocateLabel() {
    if (!nyanLocateLbl || nyanLocateIdx < 0 || nyanLocateIdx >= nyanEntryCount) return;

    int8_t rssi = nyanEntries[nyanLocateIdx].rssi;
    int signalLevel = map(constrain(rssi, -100, -40), -100, -40, 0, 5);
    char bars[8] = "";
    for (int i = 0; i < 5; i++) strcat(bars, i < signalLevel ? "|" : ".");

    char buf[220];
    snprintf(buf, sizeof(buf),
             "Locate nyanBOX\n"
             "%s\n"
             "%s\n\n"
             "RSSI  : %d dBm\n"
             "Signal: %s\n"
             "Bars  : %s\n\n"
             "Press Refresh to update",
             nyanEntries[nyanLocateIdx].name,
             nyanEntries[nyanLocateIdx].mac,
             rssi,
             nyanSignalQuality(rssi),
             bars);
    lv_label_set_text(nyanLocateLbl, buf);
}

static void cb_nyanLocateRefresh(lv_event_t *e) {
    if (nyanLocateIdx < 0 || nyanLocateIdx >= nyanEntryCount) return;

    lv_label_set_text(nyanLocateLbl, LV_SYMBOL_REFRESH "  Refreshing RSSI...");
    lv_timer_handler();

    char mac[18];
    strncpy(mac, nyanEntries[nyanLocateIdx].mac, sizeof(mac) - 1);
    mac[sizeof(mac) - 1] = '\0';

    startLEDSpinner(120, 0, 220);
    doNyanBoxScan(NYANBOX_LOCATE_SCAN_SECS, mac);
    stopLEDSpinner(120, 0, 220);

    // MAC-based upsert may have shifted sort order. Re-find target.
    int newIdx = findNyanBoxByMac(mac);
    if (newIdx >= 0) nyanLocateIdx = newIdx;
    updateNyanLocateLabel();
}

static void cb_nyanLocateBack(lv_event_t *e) {
    int idx = nyanLocateIdx;
    if (bleDetailScreen) { lv_obj_delete(bleDetailScreen); bleDetailScreen = nullptr; }
    if (idx >= 0 && idx < nyanEntryCount) createNyanBoxDetail(idx);
    else createNyanBoxDetector();
}

void createNyanBoxLocate(int idx) {
    if (idx < 0 || idx >= nyanEntryCount) return;
    nyanLocateIdx = idx;

    if (bleDetailScreen) { lv_obj_delete(bleDetailScreen); bleDetailScreen = nullptr; }
    bleDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleDetailScreen);
    createHeader(bleDetailScreen, LV_SYMBOL_EYE_OPEN "  Locate nyanBOX");

    lv_obj_t *card = lv_obj_create(bleDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,      lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,        LV_OPA_COVER,          LV_PART_MAIN);
    lv_obj_set_style_border_color(card,  lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card,  1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,        6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,       6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    nyanLocateLbl = lv_label_create(card);
    lv_label_set_long_mode(nyanLocateLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(nyanLocateLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(nyanLocateLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(nyanLocateLbl, LV_ALIGN_TOP_LEFT, 0, 0);
    updateNyanLocateLabel();

    lv_obj_t *backBtn = createBackBtn(bleDetailScreen, cb_nyanLocateBack);
    lv_obj_t *refreshBtn = createActionBtn(bleDetailScreen,
                                           LV_SYMBOL_REFRESH "  Refresh",
                                           cb_nyanLocateRefresh);

    deleteGroup(&bleDetailGroup);
    bleDetailGroup = lv_group_create();
    lv_group_add_obj(bleDetailGroup, backBtn);
    lv_group_add_obj(bleDetailGroup, refreshBtn);
    setGroup(bleDetailGroup);

    lv_screen_load_anim(bleDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  BLE TOOL 5 – AXON DETECTOR
//
//  BLE-only detector for Axon-style devices. It checks the configured
//  MAC/OUI prefix and stores matches in a fixed-size array. Locate Mode
//  refreshes RSSI for the selected MAC on demand.
// ════════════════════════════════════════════════════════════════
static lv_obj_t *axonStatusLbl = nullptr;
static lv_obj_t *axonList      = nullptr;
static lv_obj_t *axonBackBtn   = nullptr;
static lv_obj_t *axonScanBtn   = nullptr;
static lv_obj_t *axonLocateLbl = nullptr;
static int       axonLocateIdx = -1;
static int       axonDetailIdxForLocate = -1;

static void rebuildAxonList() {
    if (!axonList) return;

    // Rebuild focus group after lv_obj_clean() because list children are destroyed.
    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    if (axonBackBtn) lv_group_add_obj(bleToolGroup, axonBackBtn);
    if (axonScanBtn) lv_group_add_obj(bleToolGroup, axonScanBtn);

    lv_obj_clean(axonList);

    if (axonEntryCount == 0) {
        lv_obj_t *empty = lv_list_add_text(axonList, "No Axon devices found yet");
        if (empty) lv_obj_set_style_text_color(empty, lv_color_hex(TH.textDim), LV_PART_MAIN);
        setGroup(bleToolGroup);
        return;
    }

    for (int i = 0; i < axonEntryCount; i++) {
        char nameTrunc[12];
        strncpy(nameTrunc, axonEntries[i].name[0] ? axonEntries[i].name : "Axon", 11);
        nameTrunc[11] = '\0';

        char row[64];
#if AXON_SHOW_FULL_MAC
        snprintf(row, sizeof(row), "%s  %ddBm", nameTrunc, axonEntries[i].rssi);
#else
        snprintf(row, sizeof(row), "%s  %ddBm", nameTrunc, axonEntries[i].rssi);
#endif

        lv_obj_t *btn = lv_list_add_btn(axonList, nullptr, row);
        styleListBtn(btn);
        lv_obj_set_style_text_color(btn, bleRssiColor(axonEntries[i].rssi), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            createAxonDetail((int)(intptr_t)lv_event_get_user_data(ev));
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(bleToolGroup, btn);

#if AXON_SHOW_FULL_MAC
        lv_obj_t *macTxt = lv_list_add_text(axonList, axonEntries[i].mac);
        if (macTxt) lv_obj_set_style_text_color(macTxt, lv_color_hex(TH.textDim), LV_PART_MAIN);
#endif
    }

    setGroup(bleToolGroup);
}

static void cb_doAxonScan(lv_event_t *e) {
    char msg[48];
    snprintf(msg, sizeof(msg), LV_SYMBOL_REFRESH "  Scanning %ds...", AXON_SCAN_SECS);
    lv_label_set_text(axonStatusLbl, msg);
    lv_obj_set_style_text_color(axonStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_obj_clean(axonList);
    lv_timer_handler();

    startLEDSpinner(0, 120, 220);
    axonEntryCount = 0;
    memset(axonEntries, 0, sizeof(axonEntries));
    int found = doAxonScan(AXON_SCAN_SECS);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(axonStatusLbl, LV_SYMBOL_BLUETOOTH "  No Axon devices detected");
        lv_obj_set_style_text_color(axonStatusLbl, lv_color_hex(TH.success), LV_PART_MAIN);
    } else {
        snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING "  %d Axon device%s found!",
                 found, found == 1 ? "" : "s");
        lv_label_set_text(axonStatusLbl, msg);
        lv_obj_set_style_text_color(axonStatusLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
    }

    rebuildAxonList();
}

void createAxonDetector() {
    axonStatusLbl = nullptr;
    axonList      = nullptr;
    axonBackBtn   = nullptr;
    axonScanBtn   = nullptr;
    axonLocateLbl = nullptr;
    axonLocateIdx = -1;
    axonDetailIdxForLocate = -1;

    if (bleToolScreen) { lv_obj_delete(bleToolScreen); bleToolScreen = nullptr; }
    bleToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleToolScreen);
    createHeader(bleToolScreen, LV_SYMBOL_BLUETOOTH "  Axon Detector");

    axonStatusLbl = lv_label_create(bleToolScreen);
    char readyMsg[80];
    snprintf(readyMsg, sizeof(readyMsg),
             "Detects Axon Cameras", AXON_MAC_PREFIX, AXON_SCAN_SECS);
    lv_label_set_text(axonStatusLbl, readyMsg);
    lv_obj_set_style_text_color(axonStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(axonStatusLbl, 8, 30);

    axonList = lv_list_create(bleToolScreen);
    lv_obj_set_size(axonList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(axonList, 0, 48);
    lv_obj_set_style_bg_color(axonList,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(axonList,       LV_OPA_COVER,        LV_PART_MAIN);
    lv_obj_set_style_border_width(axonList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(axonList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(axonList,      2, LV_PART_MAIN);

    lv_obj_t *empty = lv_list_add_text(axonList, "Ready to scan for Axon devices");
    if (empty) lv_obj_set_style_text_color(empty, lv_color_hex(TH.textDim), LV_PART_MAIN);

    axonBackBtn = createBackBtn(bleToolScreen, cb_bleToolBack);
    axonScanBtn = createActionBtn(bleToolScreen,
                                  LV_SYMBOL_REFRESH "  Scan",
                                  cb_doAxonScan);

    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    lv_group_add_obj(bleToolGroup, axonBackBtn);
    lv_group_add_obj(bleToolGroup, axonScanBtn);
    setGroup(bleToolGroup);

    if (axonEntryCount > 0) {
        char msg[56];
        snprintf(msg, sizeof(msg), LV_SYMBOL_BLUETOOTH "  %d saved Axon result%s",
                 axonEntryCount, axonEntryCount == 1 ? "" : "s");
        lv_label_set_text(axonStatusLbl, msg);
        lv_obj_set_style_text_color(axonStatusLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
        rebuildAxonList();
    }

    setAllLEDs(0, 120, 220, LED_BRIGHTNESS);
    lv_screen_load_anim(bleToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

static void cb_axonDetailBack(lv_event_t *e) {
    cb_bleDetailBack(e);
}

static void cb_axonLocate(lv_event_t *e) {
    if (axonDetailIdxForLocate >= 0) createAxonLocate(axonDetailIdxForLocate);
}

void createAxonDetail(int idx) {
    if (idx < 0 || idx >= axonEntryCount) return;
    axonDetailIdxForLocate = idx;

    if (bleDetailScreen) { lv_obj_delete(bleDetailScreen); bleDetailScreen = nullptr; }
    bleDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleDetailScreen);

    char hdr[48];
    snprintf(hdr, sizeof(hdr), LV_SYMBOL_BLUETOOTH "  %.24s", axonEntries[idx].name);
    createHeader(bleDetailScreen, hdr);

    lv_obj_t *card = lv_obj_create(bleDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,      lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,        LV_OPA_COVER,          LV_PART_MAIN);
    lv_obj_set_style_border_color(card,  lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card,  1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,        6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,       6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    uint32_t ageSec = (millis() - axonEntries[idx].lastSeen) / 1000;
    char info[200];
    snprintf(info, sizeof(info),
             "Axon-style BLE Device\n"
             "Name : %s\n"
             "MAC  : %s\n"
             "RSSI : %d dBm (%s)\n"
             "Age  : %lus\n"
             "Prefix: %s",
             axonEntries[idx].name,
             axonEntries[idx].mac,
             axonEntries[idx].rssi,
             axonSignalQuality(axonEntries[idx].rssi),
             (unsigned long)ageSec,
             AXON_MAC_PREFIX);

    lv_obj_t *infoLbl = lv_label_create(card);
    lv_label_set_text(infoLbl, info);
    lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(infoLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(infoLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *backBtn = createBackBtn(bleDetailScreen, cb_axonDetailBack);
    lv_obj_t *locateBtn = createActionBtn(bleDetailScreen,
                                          LV_SYMBOL_EYE_OPEN "  Locate",
                                          cb_axonLocate);

    deleteGroup(&bleDetailGroup);
    bleDetailGroup = lv_group_create();
    lv_group_add_obj(bleDetailGroup, backBtn);
    lv_group_add_obj(bleDetailGroup, locateBtn);
    setGroup(bleDetailGroup);

    lv_screen_load_anim(bleDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

static void updateAxonLocateLabel() {
    if (!axonLocateLbl || axonLocateIdx < 0 || axonLocateIdx >= axonEntryCount) return;

    int8_t rssi = axonEntries[axonLocateIdx].rssi;
    int signalLevel = map(constrain(rssi, -100, -40), -100, -40, 0, 5);
    char bars[8] = "";
    for (int i = 0; i < 5; i++) strcat(bars, i < signalLevel ? "|" : ".");

    char buf[220];
    snprintf(buf, sizeof(buf),
             "Locate Axon\n"
             "%s\n"
             "%s\n\n"
             "RSSI  : %d dBm\n"
             "Signal: %s\n"
             "Bars  : %s\n\n"
             "Press Refresh to update",
             axonEntries[axonLocateIdx].name,
             axonEntries[axonLocateIdx].mac,
             rssi,
             axonSignalQuality(rssi),
             bars);
    lv_label_set_text(axonLocateLbl, buf);
}

static void cb_axonLocateRefresh(lv_event_t *e) {
    if (axonLocateIdx < 0 || axonLocateIdx >= axonEntryCount) return;

    lv_label_set_text(axonLocateLbl, LV_SYMBOL_REFRESH "  Refreshing RSSI...");
    lv_timer_handler();

    char mac[18];
    strncpy(mac, axonEntries[axonLocateIdx].mac, sizeof(mac) - 1);
    mac[sizeof(mac) - 1] = '\0';

    startLEDSpinner(0, 120, 220);
    doAxonScan(AXON_LOCATE_SCAN_SECS, mac);
    stopLEDSpinner(0, 120, 220);

    // MAC-based upsert may have shifted sort order. Re-find target.
    int newIdx = findAxonByMac(mac);
    if (newIdx >= 0) axonLocateIdx = newIdx;
    updateAxonLocateLabel();
}

static void cb_axonLocateBack(lv_event_t *e) {
    int idx = axonLocateIdx;
    if (bleDetailScreen) { lv_obj_delete(bleDetailScreen); bleDetailScreen = nullptr; }
    if (idx >= 0 && idx < axonEntryCount) createAxonDetail(idx);
    else createAxonDetector();
}

void createAxonLocate(int idx) {
    if (idx < 0 || idx >= axonEntryCount) return;
    axonLocateIdx = idx;

    if (bleDetailScreen) { lv_obj_delete(bleDetailScreen); bleDetailScreen = nullptr; }
    bleDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleDetailScreen);
    createHeader(bleDetailScreen, LV_SYMBOL_EYE_OPEN "  Locate Axon");

    lv_obj_t *card = lv_obj_create(bleDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,      lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,        LV_OPA_COVER,          LV_PART_MAIN);
    lv_obj_set_style_border_color(card,  lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card,  1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,        6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,       6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    axonLocateLbl = lv_label_create(card);
    lv_label_set_long_mode(axonLocateLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(axonLocateLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(axonLocateLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(axonLocateLbl, LV_ALIGN_TOP_LEFT, 0, 0);
    updateAxonLocateLabel();

    lv_obj_t *backBtn = createBackBtn(bleDetailScreen, cb_axonLocateBack);
    lv_obj_t *refreshBtn = createActionBtn(bleDetailScreen,
                                           LV_SYMBOL_REFRESH "  Refresh",
                                           cb_axonLocateRefresh);

    deleteGroup(&bleDetailGroup);
    bleDetailGroup = lv_group_create();
    lv_group_add_obj(bleDetailGroup, backBtn);
    lv_group_add_obj(bleDetailGroup, refreshBtn);
    setGroup(bleDetailGroup);

    lv_screen_load_anim(bleDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}


// ════════════════════════════════════════════════════════════════
//  BLE TOOL 6 – TESLA DETECTOR
//
//  Passive BLE name-pattern detector inspired by TeslaScanner.
//  It checks names only when the advertised name is long enough:
//     name[0] == TESLA_NAME_START_CHAR
//     name[TESLA_NAME_END_INDEX] == TESLA_NAME_END_CHAR
// ════════════════════════════════════════════════════════════════
static lv_obj_t *teslaStatusLbl = nullptr;
static lv_obj_t *teslaList      = nullptr;
static lv_obj_t *teslaBackBtn   = nullptr;
static lv_obj_t *teslaScanBtn   = nullptr;

static const char *teslaSignalQuality(int8_t rssi) {
    if (rssi >= -50) return "EXCELLENT";
    if (rssi >= -60) return "VERY GOOD";
    if (rssi >= -70) return "GOOD";
    if (rssi >= -80) return "FAIR";
    return "WEAK";
}

static int findTeslaByMac(const char *mac) {
    for (int i = 0; i < teslaEntryCount; i++) {
        if (strcmp(teslaEntries[i].mac, mac) == 0) return i;
    }
    return -1;
}

static void sortTeslaByRSSI() {
    for (int i = 0; i < teslaEntryCount - 1; i++) {
        for (int j = 0; j < teslaEntryCount - 1 - i; j++) {
            if (teslaEntries[j].rssi < teslaEntries[j + 1].rssi) {
                TeslaEntry tmp = teslaEntries[j];
                teslaEntries[j] = teslaEntries[j + 1];
                teslaEntries[j + 1] = tmp;
            }
        }
    }
}

static void upsertTeslaDevice(BLEAdvertisedDevice &dev) {
    String macStr = dev.getAddress().toString().c_str();
    int idx = findTeslaByMac(macStr.c_str());
    if (idx < 0) {
        if (teslaEntryCount >= MAX_TESLA_RESULTS) return;
        idx = teslaEntryCount++;
        memset(&teslaEntries[idx], 0, sizeof(TeslaEntry));
        strncpy(teslaEntries[idx].mac, macStr.c_str(), sizeof(teslaEntries[idx].mac) - 1);
        strncpy(teslaEntries[idx].name, "Tesla BLE", sizeof(teslaEntries[idx].name) - 1);
    }

    String nm = dev.haveName() ? dev.getName().c_str() : "Tesla BLE";
    strncpy(teslaEntries[idx].name, nm.c_str(), sizeof(teslaEntries[idx].name) - 1);
    teslaEntries[idx].name[sizeof(teslaEntries[idx].name) - 1] = '\0';
    teslaEntries[idx].rssi = (int8_t)dev.getRSSI();
    teslaEntries[idx].lastSeen = millis();
}

static int doTeslaScan(int durationSec) {
    ensureBLEInit();
    WiFi.disconnect();
    delay(50);

    BLEScan *pScan = BLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(150);
    pScan->setWindow(140);

    BLEScanResults results = pScan->start(durationSec, false);
    int total = results.getCount();

    teslaEntryCount = 0;
    memset(teslaEntries, 0, sizeof(teslaEntries));

    for (int i = 0; i < total && teslaEntryCount < MAX_TESLA_RESULTS; i++) {
        BLEAdvertisedDevice dev = results.getDevice(i);
        if (detectTeslaName(dev)) {
            upsertTeslaDevice(dev);
        }
    }

    pScan->clearResults();
    sortTeslaByRSSI();
    return teslaEntryCount;
}

static void rebuildTeslaList() {
    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    if (teslaBackBtn) lv_group_add_obj(bleToolGroup, teslaBackBtn);
    if (teslaScanBtn) lv_group_add_obj(bleToolGroup, teslaScanBtn);
    setGroup(bleToolGroup);

    lv_obj_clean(teslaList);
    if (teslaEntryCount == 0) {
        lv_obj_t *empty = lv_list_add_text(teslaList, "No Tesla BLE patterns found yet");
        if (empty) lv_obj_set_style_text_color(empty, lv_color_hex(TH.textDim), LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < teslaEntryCount; i++) {
        char nameTrunc[19];
        strncpy(nameTrunc, teslaEntries[i].name, 18);
        nameTrunc[18] = '\0';

        char row[64];
        snprintf(row, sizeof(row), "%-18s %ddBm", nameTrunc, teslaEntries[i].rssi);

        lv_obj_t *btn = lv_list_add_btn(teslaList, nullptr, row);
        styleListBtn(btn);
        lv_obj_set_style_text_color(btn, bleRssiColor(teslaEntries[i].rssi), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            createTeslaDetail((int)(intptr_t)lv_event_get_user_data(ev));
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(bleToolGroup, btn);

#if TESLA_SHOW_FULL_MAC
        lv_obj_t *macTxt = lv_list_add_text(teslaList, teslaEntries[i].mac);
        if (macTxt) lv_obj_set_style_text_color(macTxt, lv_color_hex(TH.textDim), LV_PART_MAIN);
#endif
    }

    setGroup(bleToolGroup);
}

static void cb_doTeslaScan(lv_event_t *e) {
    char msg[48];
    snprintf(msg, sizeof(msg), LV_SYMBOL_REFRESH "  Scanning %ds...", TESLA_SCAN_SECS);
    lv_label_set_text(teslaStatusLbl, msg);
    lv_obj_set_style_text_color(teslaStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_obj_clean(teslaList);
    lv_timer_handler();

    startLEDSpinner(0, 90, 220);
    int found = doTeslaScan(TESLA_SCAN_SECS);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(teslaStatusLbl, LV_SYMBOL_BLUETOOTH "  No Tesla BLE patterns detected");
        lv_obj_set_style_text_color(teslaStatusLbl, lv_color_hex(TH.success), LV_PART_MAIN);
    } else {
        snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING "  %d Tesla-like BLE device%s found!",
                 found, found == 1 ? "" : "s");
        lv_label_set_text(teslaStatusLbl, msg);
        lv_obj_set_style_text_color(teslaStatusLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
    }

    rebuildTeslaList();
}

void createTeslaDetector() {
    teslaStatusLbl = nullptr;
    teslaList      = nullptr;
    teslaBackBtn   = nullptr;
    teslaScanBtn   = nullptr;

    if (bleToolScreen) { lv_obj_delete(bleToolScreen); bleToolScreen = nullptr; }
    bleToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleToolScreen);
    createHeader(bleToolScreen, LV_SYMBOL_BLUETOOTH "  Tesla Detector");

    teslaStatusLbl = lv_label_create(bleToolScreen);
    char readyMsg[96];
    snprintf(readyMsg, sizeof(readyMsg),
             "Detects Tesla-style BLE names\nPattern: %c...%c at index %d  (%ds)",
             TESLA_NAME_START_CHAR, TESLA_NAME_END_CHAR, TESLA_NAME_END_INDEX, TESLA_SCAN_SECS);
    lv_label_set_text(teslaStatusLbl, readyMsg);
    lv_obj_set_style_text_color(teslaStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(teslaStatusLbl, 8, 30);

    teslaList = lv_list_create(bleToolScreen);
    lv_obj_set_size(teslaList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(teslaList, 0, 48);
    lv_obj_set_style_bg_color(teslaList,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(teslaList,       LV_OPA_COVER,        LV_PART_MAIN);
    lv_obj_set_style_border_width(teslaList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(teslaList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(teslaList,      2, LV_PART_MAIN);

    lv_obj_t *empty = lv_list_add_text(teslaList, "Ready to scan for Tesla BLE names");
    if (empty) lv_obj_set_style_text_color(empty, lv_color_hex(TH.textDim), LV_PART_MAIN);

    teslaBackBtn = createBackBtn(bleToolScreen, cb_bleToolBack);
    teslaScanBtn = createActionBtn(bleToolScreen,
                                   LV_SYMBOL_REFRESH "  Scan",
                                   cb_doTeslaScan);

    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    lv_group_add_obj(bleToolGroup, teslaBackBtn);
    lv_group_add_obj(bleToolGroup, teslaScanBtn);
    setGroup(bleToolGroup);

    if (teslaEntryCount > 0) {
        char msg[56];
        snprintf(msg, sizeof(msg), LV_SYMBOL_BLUETOOTH "  %d saved Tesla result%s",
                 teslaEntryCount, teslaEntryCount == 1 ? "" : "s");
        lv_label_set_text(teslaStatusLbl, msg);
        lv_obj_set_style_text_color(teslaStatusLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
        rebuildTeslaList();
    }

    setAllLEDs(0, 90, 220, LED_BRIGHTNESS);
    lv_screen_load_anim(bleToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

static void cb_teslaDetailBack(lv_event_t *e) {
    cb_bleDetailBack(e);
}

void createTeslaDetail(int idx) {
    if (idx < 0 || idx >= teslaEntryCount) return;

    if (bleDetailScreen) { lv_obj_delete(bleDetailScreen); bleDetailScreen = nullptr; }
    bleDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleDetailScreen);

    char hdr[48];
    snprintf(hdr, sizeof(hdr), LV_SYMBOL_BLUETOOTH "  %.24s", teslaEntries[idx].name);
    createHeader(bleDetailScreen, hdr);

    lv_obj_t *card = lv_obj_create(bleDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,      lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,        LV_OPA_COVER,          LV_PART_MAIN);
    lv_obj_set_style_border_color(card,  lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card,  1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,        6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,       6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    uint32_t ageSec = (millis() - teslaEntries[idx].lastSeen) / 1000;
    char info[220];
    snprintf(info, sizeof(info),
             "Tesla-style BLE Pattern\n"
             "Name : %s\n"
             "MAC  : %s\n"
             "RSSI : %d dBm (%s)\n"
             "Age  : %lus\n"
             "Rule : name[0]=%c, name[%d]=%c",
             teslaEntries[idx].name,
             teslaEntries[idx].mac,
             teslaEntries[idx].rssi,
             teslaSignalQuality(teslaEntries[idx].rssi),
             (unsigned long)ageSec,
             TESLA_NAME_START_CHAR,
             TESLA_NAME_END_INDEX,
             TESLA_NAME_END_CHAR);

    lv_obj_t *infoLbl = lv_label_create(card);
    lv_label_set_text(infoLbl, info);
    lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(infoLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(infoLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *bar = lv_bar_create(bleDetailScreen);
    lv_obj_set_size(bar, SCREEN_W - 12, 5);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(bar, -100, -30);
    lv_bar_set_value(bar, teslaEntries[idx].rssi, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, bleRssiColor(teslaEntries[idx].rssi), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    lv_obj_t *backBtn = createBackBtn(bleDetailScreen, cb_teslaDetailBack);

    deleteGroup(&bleDetailGroup);
    bleDetailGroup = lv_group_create();
    lv_group_add_obj(bleDetailGroup, backBtn);
    setGroup(bleDetailGroup);

    lv_screen_load_anim(bleDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  BLE TOOL 7 – SKIMMER DETECTOR
//
//  Scans for Bluetooth devices with names matching suspicious serial/BLE
//  module names from config.h. These cheap hobbyist boards are sometimes
//  found inside gas pump card skimmers or similar suspicious builds.
//  Reference: github.com/sparkfunX/Skimmer_Scanner
//             github.com/justcallmekoko/ESP32Marauder
// ════════════════════════════════════════════════════════════════
static lv_obj_t *skimmerStatusLbl = nullptr;
static lv_obj_t *skimmerList      = nullptr;

static void cb_doSkimmerScan(lv_event_t *e) {
    lv_label_set_text(skimmerStatusLbl, LV_SYMBOL_REFRESH "  Scanning 8s...");
    lv_obj_set_style_text_color(skimmerStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_obj_clean(skimmerList);
    lv_timer_handler();

    // Red spinner — danger colour for skimmers
    startLEDSpinner(220, 0, 0);
    int found = doBLEScan(bleScanSeconds, BLE_SKIMMER);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(skimmerStatusLbl,
            LV_SYMBOL_OK "  No skimmer modules detected");
        lv_obj_set_style_text_color(skimmerStatusLbl,
            lv_color_hex(TH.success), LV_PART_MAIN);
        lv_obj_t *empty = lv_list_add_text(skimmerList,
            "No configured skimmer names in range");
        if (empty)
            lv_obj_set_style_text_color(empty,
                lv_color_hex(TH.textDim), LV_PART_MAIN);
        return;
    }

    playBLESuspiciousChirp();

    char buf[56];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WARNING "  %d suspect device%s found!",
             found, found == 1 ? "" : "s");
    lv_label_set_text(skimmerStatusLbl, buf);
    lv_obj_set_style_text_color(skimmerStatusLbl,
        lv_color_hex(TH.alert), LV_PART_MAIN);

    for (int i = 0; i < found; i++) {
        char row[52];
        snprintf(row, sizeof(row), "%s  %s  %ddBm",
                 bleEntries[i].name,
                 bleEntries[i].mac,
                 bleEntries[i].rssi);
        lv_obj_t *entry = lv_list_add_text(skimmerList, row);
        if (entry)
            lv_obj_set_style_text_color(entry,
                lv_color_hex(TH.alert), LV_PART_MAIN);
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
        "Scans for configured suspicious BLE\n"
        "serial modules used in skimmers");
    lv_obj_set_style_text_color(skimmerStatusLbl,
        lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(skimmerStatusLbl, 8, 30);

    skimmerList = lv_list_create(bleToolScreen);
    lv_obj_set_size(skimmerList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(skimmerList, 0, 48);
    lv_obj_set_style_bg_color(skimmerList,     lv_color_hex(TH.bg), LV_PART_MAIN);
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

// ════════════════════════════════════════════════════════════════
//  BLE TOOL 8 – META / RAYBAN DETECTOR
//
//  Detects Meta smart glasses (Ray-Ban Meta, Quest, etc.) by
//  checking BLE advertisements for Meta/Luxottica manufacturer IDs,
//  service UUIDs, and service-data UUIDs, while filtering out
//  common non-Meta identifiers (Apple, Samsung, Microsoft).
//
//  Identifier tables credit: NullPxl / justcallmekoko Marauder
//  Key IDs: 0xFD5F, 0xFEB7, 0xFEB8, 0x01AB, 0x058E, 0x0D53
// ════════════════════════════════════════════════════════════════
static lv_obj_t *metaStatusLbl = nullptr;
static lv_obj_t *metaList      = nullptr;

static void cb_doMetaScan(lv_event_t *e) {
    lv_label_set_text(metaStatusLbl, LV_SYMBOL_REFRESH "  Scanning 8s...");
    lv_obj_set_style_text_color(metaStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_obj_clean(metaList);
    lv_timer_handler();

    // Blue spinner for Meta (brand colour)
    startLEDSpinner(0, 100, 255);
    int found = doBLEScan(bleScanSeconds, BLE_META);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(metaStatusLbl,
            LV_SYMBOL_OK "  No Meta devices detected");
        lv_obj_set_style_text_color(metaStatusLbl,
            lv_color_hex(TH.success), LV_PART_MAIN);
        lv_obj_t *empty = lv_list_add_text(metaList,
            "No Ray-Ban / Quest in range");
        if (empty)
            lv_obj_set_style_text_color(empty,
                lv_color_hex(TH.textDim), LV_PART_MAIN);
        return;
    }

    char buf[56];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WARNING "  %d Meta device%s found!",
             found, found == 1 ? "" : "s");
    lv_label_set_text(metaStatusLbl, buf);
    lv_obj_set_style_text_color(metaStatusLbl,
        lv_color_hex(TH.accent), LV_PART_MAIN);

    for (int i = 0; i < found; i++) {
        char row[52];
        snprintf(row, sizeof(row), "%s  %s  %ddBm",
                 bleEntries[i].name[0] ? bleEntries[i].name : "<unknown>",
                 bleEntries[i].mac,
                 bleEntries[i].rssi);
        lv_obj_t *entry = lv_list_add_text(metaList, row);
        if (entry)
            lv_obj_set_style_text_color(entry,
                lv_color_hex(TH.accent), LV_PART_MAIN);
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
        "Detects Ray-Ban Meta / Quest");
    lv_obj_set_style_text_color(metaStatusLbl,
        lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(metaStatusLbl, 8, 30);

    metaList = lv_list_create(bleToolScreen);
    lv_obj_set_size(metaList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(metaList, 0, 48);
    lv_obj_set_style_bg_color(metaList,     lv_color_hex(TH.bg), LV_PART_MAIN);
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
//  AUTO-RETURN HOME
// ════════════════════════════════════════════════════════════════
static void deleteIfInactiveScreen(lv_obj_t *&scr, lv_obj_t *activeScr) {
    // Only delete screens that are not currently active. The active screen is
    // handled by lv_screen_load_anim(..., auto_del=true) when returning home.
    if (scr && scr != activeScr && scr != mainScreen) {
        lv_obj_delete(scr);
        scr = nullptr;
    }
}

static void cleanupForAutoReturnHome(lv_obj_t *activeScr) {
    // Stop any WiFi promiscuous tools cleanly before jumping home.
    if (deauthActive || pwnActive || flockActive || hybridWifiActive || packetMonitorActive) {
        deauthActive = false;
        pwnActive    = false;
        flockActive  = false;
        hybridWifiActive = false;
        packetMonitorActive = false;
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);
    }

    if (spinnerRunning) {
        stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
    }

    if (deauthTimer) { lv_timer_delete(deauthTimer); deauthTimer = nullptr; }
    if (pwnTimer)    { lv_timer_delete(pwnTimer);    pwnTimer    = nullptr; }
    if (flockTimer)  { lv_timer_delete(flockTimer);  flockTimer  = nullptr; }
    if (hybridStartTimer) { lv_timer_delete(hybridStartTimer); hybridStartTimer = nullptr; }
    if (packetMonitorTimer) { lv_timer_delete(packetMonitorTimer); packetMonitorTimer = nullptr; }

    // Stop GPS/Wiggle Wars timers safely if one of those tools is open.
    if (gpsTimer) { lv_timer_delete(gpsTimer); gpsTimer = nullptr; }
    if (wiggleRunning) {
        wiggleRunning = false;
        WiFi.scanDelete();
        SD.end();
    }
    if (wiggleTimer) { lv_timer_delete(wiggleTimer); wiggleTimer = nullptr; }

    // Delete inactive screens that would otherwise stay hidden in memory.
    deleteIfInactiveScreen(subScreen,        activeScr);
    deleteIfInactiveScreen(wifiMenuScreen,   activeScr);
    deleteIfInactiveScreen(wifiToolScreen,   activeScr);
    deleteIfInactiveScreen(wifiDetailScreen, activeScr);
    deleteIfInactiveScreen(bleMenuScreen,    activeScr);
    deleteIfInactiveScreen(bleToolScreen,    activeScr);
    deleteIfInactiveScreen(bleDetailScreen,  activeScr);
    deleteIfInactiveScreen(miscMenuScreen,   activeScr);
    deleteIfInactiveScreen(miscToolScreen,   activeScr);
    deleteIfInactiveScreen(gpsMenuScreen,    activeScr);
    deleteIfInactiveScreen(gpsToolScreen,    activeScr);

    // Drop non-home input groups. The main menu group is kept.
    deleteGroup(&subGroup);
    deleteGroup(&wifiMenuGroup);
    deleteGroup(&wifiToolGroup);
    deleteGroup(&wifiDetailGroup);
    deleteGroup(&bleMenuGroup);
    deleteGroup(&bleToolGroup);
    deleteGroup(&bleDetailGroup);
    deleteGroup(&miscMenuGroup);
    deleteGroup(&miscToolGroup);
    deleteGroup(&gpsMenuGroup);
    deleteGroup(&gpsToolGroup);
}

static void updateAutoReturnHome() {
#if AUTO_RETURN_HOME_TIMEOUT_MS > 0
    if (powerOffTriggered || !mainScreen) return;

    lv_obj_t *activeScr = lv_screen_active();
    if (activeScr == mainScreen) return;

    // Do not auto-return in the middle of a long-press power action.
    if (digitalRead(ENCODER_BTN) == LOW) return;

    if (millis() - lastActivityMs < AUTO_RETURN_HOME_TIMEOUT_MS) return;

    cleanupForAutoReturnHome(activeScr);
    setGroup(navGroup);
    setAllLEDs(MENU_COLORS[currentMenu].r, MENU_COLORS[currentMenu].g, MENU_COLORS[currentMenu].b, LED_BRIGHTNESS);

    // Loading home with auto_del=true lets LVGL safely free the active screen
    // after the animation finishes.
    lv_screen_load_anim(mainScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);

    // Count the auto-return as fresh activity so it does not keep retriggering.
    resetInactivityTimer();
#endif
}

// ════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    // SD card on dedicated HSPI bus — must not share with TFT
    sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    pinMode(POWER_PIN, OUTPUT);
    digitalWrite(POWER_PIN, HIGH);
    pinMode(ENCODER_BTN, INPUT_PULLUP);
    // Sound uses lazy init. Do not start I2S during boot.
    // initSound();

    tft.begin();
    tft.writecommand(0x11);
    delay(120);
    applyDisplayRotation(false);
    // Backlight PWM via LEDC — allows smooth brightness control
    ledcSetup(LCD_BL_CH, LCD_BL_FREQ, LCD_BL_RES);
    ledcAttachPin(LCD_BL_PIN, LCD_BL_CH);
    applyBacklightLevel((uint8_t)lcdBrightness);
    resetInactivityTimer();
    applyFlockHybridPreset();
    tft.fillScreen(TFT_BLACK);
    showSplashScreen();  // Splash Screen Call

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)millis(); });

    lvDisp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_flush_cb(lvDisp, lvgl_flush_cb);
    lv_display_set_buffers(lvDisp, lvBuf1, lvBuf2,
                           sizeof(lvBuf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lvIndev = lv_indev_create();
    lv_indev_set_type(lvIndev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(lvIndev, encoder_read_cb);

    ledStartupFlash();
    createMainMenu();

    Serial.println("[T-Embed] Boot complete.");
}

// ════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════
void loop() {
    // Feed all available GPS bytes into TinyGPS++ — non-blocking
    while (gpsSerial.available())
        gps.encode(gpsSerial.read());

    lv_timer_handler();

    // Safe inactivity behavior: dim backlight + APA102 brightness only, no ESP32 sleep yet.
    updateInactivityDimmer();

    // Optional UI auto-return: after inactivity, jump back to the main home menu.
    updateAutoReturnHome();

    // Channel hopping: deauth detector and pwnagotchi watch both need it
    static unsigned long lastHop = 0;
    if ((deauthActive || pwnActive || flockActive || hybridWifiActive) && (millis() - lastHop >= (unsigned long)deauthHopMs)) {
        lastHop       = millis();
        deauthChannel = (deauthChannel % 13) + 1;
        esp_wifi_set_channel(deauthChannel, WIFI_SECOND_CHAN_NONE);
    }

    // Long-press power-off (hold encoder 5 s)
    // Safety latch: ignore any boot-time LOW reading until the button has been
    // seen released once. This prevents accidental shutdowns after peripheral init.
    if (!powerOffTriggered) {
        int encoderBtnState = digitalRead(ENCODER_BTN);

        if (encoderBtnState == HIGH) {
            powerButtonReleasedAfterBoot = true;
            btnHoldStart = 0;
        } else if (powerButtonReleasedAfterBoot) {
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

                setAllLEDs(0, 0, 0, 0);
                if (soundReady) {
                    stopSoundDriverAfterChirp();
                }
                delay(1500);
                digitalWrite(POWER_PIN, LOW);
            }
        } else {
            // Button appears LOW before first release after boot; ignore it.
            btnHoldStart = 0;
        }
    }

    delay(5);
}

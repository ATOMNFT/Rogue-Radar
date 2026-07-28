// ============================================================
//  Rogue Radar v1.0.5 Firmware
//  Check config.h for adjustable settings
// ============================================================
//
//  Tool Categories:
//  WiFi Tools: Network Scanner | Connect to AP | LAN Host Discovery (simple) | Gateway Info | Station Scanner | Deauth Detector | Channel Analyzer 
//              Packet Monitor | WiFi Mapper | PineAP Hunter | Pwnagotchi Watch | Flock Detector | Flock Hybrid
//  BLE Tools:  BLE Scanner | AirTag Detector | Flipper Zero Detector | Tesla BLE Detector
//              nyanBOX Detector | Axon Detector | Raven Detector | Smart Charger Monitor
//              Skimmer Detector | Meta Detector
//  GPS Tools:  GPS Stats | Wiggle Wars
//  Audio Tools: Sound Recorder
//  Misc Tools: Device Info | SD Update | Brightness ADJ | Themes | Dimming | Scan Times | LEDs | Detection Sounds | Menu Sounds | Rotation | Power Off
//              | Battery Gauge | Volume Adjustments |
//
//  Display / UI:
//  - ST7789 320x170 display
//  - LVGL-based menu system
//  - Rotary encoder navigation
//  - Splash screen support
//  - APA102 status LED support
//
//  Hardware Target:
//  - LilyGO T-Embed ESP32-S3 (Non CC1101)
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


#ifndef AUDIO_RECORD_STOP_POLL_MS
#define AUDIO_RECORD_STOP_POLL_MS 20
#endif

#ifndef AUDIO_RECORD_STOP_RESTART_GUARD_MS
#define AUDIO_RECORD_STOP_RESTART_GUARD_MS 900
#endif


#ifndef AUDIO_RECORD_SD_SAVE_ENABLED
#define AUDIO_RECORD_SD_SAVE_ENABLED 1
#endif
#ifndef AUDIO_RECORD_SD_FOLDER
#define AUDIO_RECORD_SD_FOLDER "/rr_audio"
#endif
#ifndef AUDIO_RECORD_SD_PREFIX
#define AUDIO_RECORD_SD_PREFIX "REC"
#endif
#ifndef AUDIO_RECORD_SD_MAX_FILES
#define AUDIO_RECORD_SD_MAX_FILES 24
#endif
#ifndef AUDIO_RECORD_SD_ROOT_FALLBACK
#define AUDIO_RECORD_SD_ROOT_FALLBACK 1
#endif
#ifndef AUDIO_RECORD_SD_DEBUG_STATUS
#define AUDIO_RECORD_SD_DEBUG_STATUS 1
#endif

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
#include <Preferences.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <Wire.h>
#include "splash.h"

static SPIClass sdSPI(HSPI);
static Preferences settingsPrefs;

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
    { THEME_TYPER    },
    { THEME_JOKER    },
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
const MenuLED MENU_COLORS[5] = {
    LED_COLOR_WIFI,
    LED_COLOR_BLE,
    LED_COLOR_GPS,
    LED_COLOR_AUDIO,
    LED_COLOR_MISC
};

// ─── LED Spinner (FreeRTOS task on core 0) ───────────────────────
struct SpinnerColor { uint8_t r, g, b; };
static volatile bool  spinnerRunning     = false;
static TaskHandle_t   spinnerTaskHandle  = nullptr;
static SpinnerColor   spinnerColor       = {0, 200, 0};
static volatile uint16_t spinnerDelayMs   = 80;
static volatile bool     rainbowSpinnerRunning = false;
static TaskHandle_t      rainbowSpinnerTaskHandle = nullptr;
static volatile uint16_t rainbowSpinnerDelayMs = 70;

// ─── Global UI State ────────────────────────────────────────────
static int            currentMenu       = 0;
static bool           powerOffTriggered = false;
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
static lv_obj_t      *miscAlertVolumeBtn = nullptr;
static lv_obj_t      *miscRotationBtn   = nullptr;

// Menu volume auto-set state. Declared here so the common Misc Back handler
// can safely delete the timer from any Misc tool page.
static lv_timer_t    *menuVolAutoTimer = nullptr;
static bool           menuVolPendingAutoSet = false;
static lv_timer_t    *alertVolAutoTimer = nullptr;
static bool           alertVolPendingAutoSet = false;

// ─── I2S Speaker / Alert Chirp State ───────────────────────────
static bool           soundEnabled      = (SOUND_ENABLED_DEFAULT != 0);
static bool           soundReady        = false;
static bool           menuFeedbackEnabled = (MENU_FEEDBACK_ENABLED_DEFAULT != 0);
static int            menuFeedbackVolumePercent = MENU_FEEDBACK_VOLUME_PERCENT;
static int            alertSoundVolumePercent = SOUND_VOLUME_PERCENT;
static unsigned long  lastDeauthSoundMs = 0;
static unsigned long  lastFlockSoundMs  = 0;
static unsigned long  lastPwnSoundMs    = 0;
static unsigned long  lastFlipSoundMs   = 0;
static unsigned long  lastTeslaSoundMs  = 0;
static unsigned long  lastBleSusSoundMs = 0;
static unsigned long  lastMenuTickMs    = 0;
static unsigned long  lastMenuClickMs   = 0;

// Runtime display rotation. This is reset-after-reboot and only toggles
// between the two landscape orientations so the 320x170 LVGL layout stays safe.
static uint8_t displayRotation = DISPLAY_ROTATION_DEFAULT;

// ════════════════════════════════════════════════════════════════
//  PERSISTENT SETTINGS / NVS — v1.0.4 PASS 1 + PASS 2 + PASS 3
//
//  Saves UI, sound, and scan-default settings so they survive reboot:
//  Pass 1: theme, brightness, rotation, LEDs ON/OFF, dimming ON/OFF.
//  Pass 2: Alert Sound ON/OFF, Alert Volume, Menu Sounds ON/OFF, Menu Volume.
//  Pass 3: Scan Defaults, Packet Monitor settings, Flock Hybrid preset.
//  Config.h still provides the defaults when no saved value exists.
// ════════════════════════════════════════════════════════════════
static int themeCount() {
    return (int)(sizeof(THEMES) / sizeof(THEMES[0]));
}

static int clampIntValue(int value, int minValue, int maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

static bool isValidDisplayRotation(uint8_t rot) {
    return (rot == DISPLAY_ROTATION_NORMAL || rot == DISPLAY_ROTATION_FLIPPED);
}

static void loadPersistentSettings() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, true);

    int savedTheme = settingsPrefs.getUChar("theme", DEFAULT_THEME);
    if (savedTheme >= 0 && savedTheme < themeCount()) {
        currentTheme = savedTheme;
    } else {
        currentTheme = DEFAULT_THEME;
    }

    lcdBrightness = settingsPrefs.getUChar("bright", LCD_BL_DEFAULT);
    lcdBrightness = clampIntValue(lcdBrightness, 13, 255);

    uint8_t savedRotation = settingsPrefs.getUChar("rot", DISPLAY_ROTATION_DEFAULT);
    displayRotation = isValidDisplayRotation(savedRotation)
                    ? savedRotation
                    : DISPLAY_ROTATION_DEFAULT;

    ledsEnabled = settingsPrefs.getBool("leds", (LEDS_ENABLED_DEFAULT != 0));
    dimmingEnabled = settingsPrefs.getBool("dim", (DIMMING_ENABLED_DEFAULT != 0));

    soundEnabled = settingsPrefs.getBool("alertOn", (SOUND_ENABLED_DEFAULT != 0));
    alertSoundVolumePercent = settingsPrefs.getInt("alertVol", SOUND_VOLUME_PERCENT);
    alertSoundVolumePercent = clampIntValue(alertSoundVolumePercent,
                                           SOUND_VOLUME_MIN_PERCENT,
                                           SOUND_VOLUME_MAX_PERCENT);

    menuFeedbackEnabled = settingsPrefs.getBool("menuOn", (MENU_FEEDBACK_ENABLED_DEFAULT != 0));
    menuFeedbackVolumePercent = settingsPrefs.getInt("menuVol", MENU_FEEDBACK_VOLUME_PERCENT);
    menuFeedbackVolumePercent = clampIntValue(menuFeedbackVolumePercent,
                                             MENU_FEEDBACK_VOLUME_MIN_PERCENT,
                                             MENU_FEEDBACK_VOLUME_MAX_PERCENT);

    settingsPrefs.end();

    Serial.println("[Rogue-Radar] NVS settings loaded.");
#endif
}

static void savePersistentThemeSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putUChar("theme", (uint8_t)currentTheme);
    settingsPrefs.end();
#endif
}

static void savePersistentBrightnessSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putUChar("bright", (uint8_t)clampIntValue(lcdBrightness, 13, 255));
    settingsPrefs.end();
#endif
}

static void savePersistentRotationSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putUChar("rot", displayRotation);
    settingsPrefs.end();
#endif
}

static void savePersistentLedsSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putBool("leds", ledsEnabled);
    settingsPrefs.end();
#endif
}

static void savePersistentDimmingSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putBool("dim", dimmingEnabled);
    settingsPrefs.end();
#endif
}

static void savePersistentAlertSoundSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putBool("alertOn", soundEnabled);
    settingsPrefs.end();
#endif
}

static void savePersistentAlertVolumeSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putInt("alertVol", clampIntValue(alertSoundVolumePercent,
                                                   SOUND_VOLUME_MIN_PERCENT,
                                                   SOUND_VOLUME_MAX_PERCENT));
    settingsPrefs.end();
#endif
}

static void savePersistentMenuSoundSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putBool("menuOn", menuFeedbackEnabled);
    settingsPrefs.end();
#endif
}

static void savePersistentMenuVolumeSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putInt("menuVol", clampIntValue(menuFeedbackVolumePercent,
                                                  MENU_FEEDBACK_VOLUME_MIN_PERCENT,
                                                  MENU_FEEDBACK_VOLUME_MAX_PERCENT));
    settingsPrefs.end();
#endif
}

static void resetPersistentSettings() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.clear();
    settingsPrefs.end();
    Serial.println("[Rogue-Radar] NVS settings cleared. Restarting...");
#endif
}

// ─── Runtime Scan Defaults ─────────────────────────────────────
// These start from config.h unless a saved NVS value exists.
static int bleScanSeconds  = BLE_SCAN_SECS;
static int wifiScanSeconds = WIFI_SCAN_SECS;
static int wifiMaxResults  = MAX_WIFI_RESULTS;
static int deauthHopMs     = DEAUTH_HOP_MS;

// Flock Hybrid runtime scan defaults. These persist after v1.0.4 Pass 3.
static int flockHybridPresetIdx = FLOCK_HYBRID_PRESET_DEFAULT;
static int flockHybridBleSecs   = FLOCK_HYBRID_BLE_SECS;
static int flockHybridWifiSecs  = FLOCK_HYBRID_WIFI_SECS;
static int flockHybridHopMs     = FLOCK_HYBRID_WIFI_HOP_MS;

// Packet Monitor runtime state. Hop settings persist after v1.0.4 Pass 3.
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

// WiFi Mapper runtime state.
// This is a visual packet map: X = channel 1-13, Y = RSSI -90 to -10 dBm.
struct WiFiMapperPoint {
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  pktType;   // 0=MGMT, 1=DATA, 2=CTRL, 3=OTHER
    uint32_t ms;
};

static volatile bool     wifiMapperActive = false;
static uint8_t           wifiMapperChannel = 1;
static uint8_t           wifiMapperSpeedIdx = WIFI_MAPPER_DEFAULT_SPEED;
static uint32_t          wifiMapperLastHopMs = 0;
static uint32_t          wifiMapperTotalPackets = 0;
static volatile uint16_t wifiMapperHead = 0;
static volatile uint16_t wifiMapperCount = 0;
static WiFiMapperPoint   wifiMapperPoints[WIFI_MAPPER_MAX_POINTS];
static volatile int8_t   wifiMapperLastRSSI = -127;
static volatile uint8_t  wifiMapperLastType = 3;

static lv_timer_t *wifiMapperTimer = nullptr;
static lv_obj_t   *wifiMapperStatusLbl = nullptr;
static lv_obj_t   *wifiMapperDetailLbl = nullptr;
static lv_obj_t   *wifiMapperGridArea = nullptr;
static lv_obj_t   *wifiMapperPauseBtn = nullptr;
static lv_obj_t   *wifiMapperPauseLbl = nullptr;
static lv_obj_t   *wifiMapperSpeedBtn = nullptr;
static lv_obj_t   *wifiMapperSpeedLbl = nullptr;

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
static lv_obj_t *audioMenuScreen  = nullptr;
static lv_obj_t *audioToolScreen  = nullptr;

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
static lv_group_t *audioMenuGroup  = nullptr;
static lv_group_t *audioToolGroup  = nullptr;
static lv_group_t *keyboardGroup   = nullptr;

// ─── LVGL Keyboard State ───────────────────────────────────────
typedef void (*RogueKeyboardCallback)(const char *text, bool accepted);
static lv_obj_t *keyboardScreen     = nullptr;
static lv_obj_t *keyboardTextLabel  = nullptr;
static lv_obj_t *keyboardCountLabel = nullptr;
static lv_obj_t *keyboardCapsLabel  = nullptr;
static lv_obj_t *keyboardKeyLabels[48];
static RogueKeyboardCallback keyboardDoneCb = nullptr;
static String keyboardCurrentText;
static int keyboardMaxLen = 64;
static bool keyboardCaps = false;
static bool keyboardActive = false;  // True while LVGL keyboard is open; keeps input/audio/battery refresh stable.
static bool keyboardFinishPending = false;
static bool keyboardFinishAccepted = false;
static lv_timer_t *keyboardFinishTimer = nullptr;
static uint32_t keyboardFinishReadyAtMs = 0;  // Wait until encoder button is released before closing keyboard.
static uint32_t keyboardButtonReleasedAtMs = 0;  // Debounced release time before finishing OK/Esc.
static bool keyboardClickFeedbackPending = false;  // Delayed keyboard key click; plays only after GPIO0 is released.
static uint32_t keyboardClickFeedbackReadyAtMs = 0;
static uint32_t keyboardClickFeedbackReleaseAtMs = 0;


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

// ── Connect to AP Tool ─────────────────────────────────────────
// Uses the shared WiFiEntry scan buffer, then keeps WiFi in STA mode
// once connected so future safe LAN tools can reuse the connection.
static lv_obj_t *connectApStatusLbl = nullptr;
static lv_obj_t *connectApList      = nullptr;
static lv_obj_t *connectApBackBtn   = nullptr;
static lv_obj_t *connectApScanBtn   = nullptr;
static lv_obj_t *connectApDiscBtn   = nullptr;

// ── LAN Host Discovery Tool ────────────────────────────────────
// Connected-only LAN utility. Uses the active STA connection created by
// Connect to AP, then probes the local subnet for common TCP services.
static lv_obj_t *lanDiscoveryStatusLbl = nullptr;
static lv_obj_t *lanDiscoveryList      = nullptr;
static lv_obj_t *lanDiscoveryBackBtn   = nullptr;
static lv_obj_t *lanDiscoveryScanBtn   = nullptr;

// ── Gateway Info / Router Check Tool ───────────────────────────
// Connected-only LAN utility. Shows router/gateway-focused connection details
// and lets the user refresh the status without leaving the page.
static lv_obj_t *gatewayInfoStatusLbl = nullptr;
static lv_obj_t *gatewayInfoList      = nullptr;
static lv_obj_t *gatewayInfoBackBtn   = nullptr;
static lv_obj_t *gatewayInfoRefreshBtn = nullptr;

static int       connectApSelectedIdx = -1;
static char      connectApTargetSsid[33] = {0};
static char      connectApTargetBssid[18] = {0};
static char      connectApTargetAuth[10] = {0};
static bool      connectApTargetOpen = false;
static int8_t    connectApTargetRssi = 0;
static uint8_t   connectApTargetChannel = 0;

// True only when Connect to AP is using a previously saved/config password.
// This prevents a reconnect from immediately re-saving the same credential
// while the WiFi/LVGL screen transition is happening.
static bool      connectApUsingCachedPassword = false;

// WiFi connection watchdog used by Connect to AP.
// It lets Rogue Radar play the disconnect event tone when a connected AP drops
// unexpectedly (out of range, router reboot, weak signal, etc.).
// Manual disconnects and intentional reconnect attempts suppress the watchdog
// briefly so the tone does not double-play.
static bool      connectApWatchInitialized = false;
static bool      connectApWasConnected = false;
static uint32_t  connectApSuppressDropToneUntilMs = 0;

// ── Station Scanner ────────────────────────────────────────────
struct StationEntry {
    char     stationMac[18];
    char     apBssid[18];
    char     frameType[18];
    int8_t   rssi;
    uint8_t  channel;
    uint16_t packets;
    uint16_t eapolPackets;
    bool     eapolSeen;
    uint32_t lastSeenMs;
};

struct StationApEntry {
    char     bssid[18];
    char     ssid[33];
    char     authStr[12];
    int8_t   rssi;
    uint8_t  channel;
    uint16_t clientCount;
    uint16_t eapolPackets;
    uint32_t lastSeenMs;
};

static StationEntry   stationEntries[MAX_STATION_RESULTS];
static StationApEntry stationAps[MAX_STATION_APS];
static volatile int   stationEntryCount = 0;
static volatile int   stationApCount = 0;
static volatile uint16_t stationEapolTotal = 0;
static volatile bool stationScanActive = false;
static uint8_t stationScanChannel = 1;
static lv_timer_t *stationScanTimer = nullptr;
static lv_obj_t *stationStatusLbl = nullptr;
static lv_obj_t *stationList = nullptr;
static lv_obj_t *stationStartBtn = nullptr;
static lv_obj_t *stationStartLbl = nullptr;
static lv_obj_t *stationBackBtn = nullptr;

// Station result rows are kept alive and updated in-place while scanning.
// This prevents encoder focus from jumping back to Back during list refreshes.
static lv_obj_t *stationRowBtns[MAX_STATION_RESULTS] = {nullptr};
static lv_obj_t *stationRowLabels[MAX_STATION_RESULTS] = {nullptr};
static int       stationRowCount = 0;
static lv_obj_t *stationEmptyLbl = nullptr;

// ── Deauth Detector ─────────────────────────────────────────────
struct DeauthEvent {
    char     src[18];
    char     dst[18];
    uint8_t  channel;
    uint16_t reason;
    int8_t   rssi;
    uint32_t ms;
};

static DeauthEvent   deauthLog[MAX_DEAUTH];
static volatile int  deauthHead    = 0;
static volatile int  deauthTotal   = 0;
static int           deauthSoundedTotal = 0;
static volatile bool deauthActive  = false;
static uint8_t       deauthChannel = 1;

// Deauth stats are lightweight and mirror the existing detector logic.
// They are shown in the detector list and in the Deauth Stats page.
static volatile int32_t deauthRssiSum      = 0;
static volatile int     deauthRssiCount    = 0;
static volatile int8_t  deauthStrongestRSSI = -127;
static int              deauthCurrentRate  = 0;
static uint32_t         deauthLastRateMs   = 0;
static int              deauthLastRateTotal = 0;

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
    char     name[33];       // parsed from JSON "name" field
    char     bssid[18];      // BSSID from beacon frame
    char     rawJson[97];    // shortened SSID JSON preview
    int      pwnd_tot;       // parsed from JSON "pwnd_tot"
    bool     pal;            // parsed from JSON "pal"
    bool     minigotchi;     // parsed from JSON "minigotchi"
    uint8_t  channel;        // channel seen on
    int8_t   rssi;
    uint32_t lastSeen;       // millis()
};

static PwnEntry        pwnEntries[MAX_PWNS];
static int             pwnCount       = 0;
static volatile bool   pwnActive      = false;
static lv_timer_t     *pwnTimer       = nullptr;

// Ring buffer for passing SSID payloads out of the sniffer ISR
static char            pwnPendingSSID[PWN_BUF_LEN];
static char            pwnPendingBSSID[18];
static volatile int8_t pwnPendingRSSI  = 0;
static volatile uint8_t pwnPendingChannel = 1;
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
    char     ssid[33];
    char     src[18];       // source MAC of the frame
    char     method[18];    // Name / MAC Prefix / MFR Prefix / SoundThinking
    char     confidence[8]; // High / Medium / Low
    char     type[20];      // Flock / Flock MFR / SoundThinking
    uint8_t  frameType;     // 0=beacon/resp, 1=probe req
    int8_t   rssi;
    uint16_t count;
    uint32_t firstSeen;
    uint32_t lastSeen;
};

static FlockHit       flockHits[MAX_FLOCK_HITS];
static int            flockHitCount   = 0;
static volatile bool  flockActive     = false;
static lv_timer_t    *flockTimer      = nullptr;

// ISR pending slot — one frame at a time into main task
static char            flockPendingSSID[33];
static char            flockPendingSrc[18];
static char            flockPendingMethod[18];
static char            flockPendingConfidence[8];
static char            flockPendingDeviceType[20];
static volatile uint8_t flockPendingType = 0;
static volatile int8_t  flockPendingRSSI = 0;
static volatile bool    flockPendingReady = false;

// ── Flock Hybrid Scanner ───────────────────────────────────────
//  Combined BLE + WiFi Flock scan. This intentionally alternates phases
//  instead of running BLE and WiFi promiscuous mode at the same time.
struct FlockHybridHit {
    char     source[6];      // "BLE" or "WiFi"
    char     name[33];       // BLE name or WiFi SSID
    char     mac[18];
    char     reason[24];     // Short match reason shown in rows
    char     method[24];     // Name / MAC Prefix / MFR Prefix / BLE MFR ID
    char     confidence[8];  // High / Medium / Low
    char     type[20];       // Flock / Flock MFR / SoundThinking
    int8_t   rssi;
    uint16_t count;
    uint32_t firstSeen;
    uint32_t lastSeen;
};

static FlockHybridHit hybridHits[MAX_FLOCK_HYBRID_HITS];
static int            hybridHitCount = 0;
static volatile bool  hybridWifiActive = false;
static lv_obj_t      *hybridStatusLbl = nullptr;
static lv_obj_t      *hybridList      = nullptr;
static lv_obj_t      *hybridBackBtn   = nullptr;
static lv_obj_t      *hybridScanBtn   = nullptr;
static lv_timer_t    *hybridStartTimer = nullptr;
static uint32_t       hybridBleHeardCount = 0;

// ISR pending slot for the WiFi half of the hybrid scanner
static char             hybridPendingName[33];
static char             hybridPendingMac[18];
static char             hybridPendingReason[24];
static char             hybridPendingMethod[24];
static char             hybridPendingConfidence[8];
static char             hybridPendingType[20];
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

// ─── Manual prototypes for functions using custom types ─────────
static const char *mfgHintStr(BLEDeviceType t);
static int doBLEScan(int durationSec, BLEDeviceType filterType);
static const char *pwnDeviceType(const PwnEntry &e);

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

// Raven detector entries. Fixed array keeps memory predictable on ESP32-S3.
struct RavenEntry {
    char     name[33];
    char     mac[18];
    int8_t   rssi;
    char     matchedUuid[41];
    char     fwEstimate[16];
    uint8_t  uuidHitCount;
    uint32_t lastSeen;
};

static RavenEntry ravenEntries[MAX_RAVEN_RESULTS];
static int        ravenEntryCount = 0;


// Smart Charger Monitor entries. First pass is passive only: no GATT connection,
// no notify subscription, and no writes. It matches the charger by advertised
// name, optional MAC prefix, and FFF0 service UUID if the UUID is advertised.
struct SmartChargerEntry {
    char     name[33];
    char     mac[18];
    int8_t   rssi;
    char     matchMethod[24];
    char     advUuid[41];
    char     mfgPreview[33];
    uint8_t  confidence;  // 3=High, 2=Medium, 1=Low
    uint32_t lastSeen;
};

static SmartChargerEntry chargerEntries[MAX_CHARGER_RESULTS];
static int               chargerEntryCount = 0;


// ════════════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════
void createMainMenu();
void createWiFiMenu();
void createNetworkScanner();
void createNetworkDetail(int idx);
void createConnectAPTool();
void createLANHostDiscovery();
void createGatewayInfo();
void createStationScanner();
void createStationDetail(int idx);
void createDeauthDetector();
void createDeauthStats();
void createChannelAnalyzer();
void createPacketMonitor();
void createWiFiMapper();
void createPineAPHunter();
void createPineAPDetail(int idx);
void createPwnagotchiDetector();
void createPwnagotchiDetail(int idx);
void createFlockDetector();
void createFlockHybridScanner();
void createFlockHybridDetail(int idx);
void createSubScreen(int idx);
void createMiscMenu();
void createDeviceInfo();
void createSDUpdate();
void createBrightnessControl();
void createMenuFeedbackVolumeControl();
void createAlertSoundVolumeControl();
void createThemePicker();
void createScanDefaults();
void createResetSettings();
void createPowerOffConfirm();
void createGPSMenu();
void createGPSStats();
void createWiggleWars();
void createAudioMenu();
void createSoundRecorder();
static void createSoundRecorderFileMenu();

// Reusable Rogue Radar LVGL keyboard.
// Callback receives (text, true) on OK and ("", false) on Back/Cancel.
void createKeyboardScreen(const char *title,
                          const char *currentText,
                          int maxLen,
                          RogueKeyboardCallback cb);

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
void createRavenDetector();
void createRavenDetail(int idx);
void createSmartChargerMonitor();
void createSmartChargerDetail(int idx);
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
    //Serial.println("[Sound] I2S alert chirps ready");  // Added for testing.------------------------------------------------------------------------------------
}

static bool ensureSoundReady(bool requireAlertSoundEnabled = true) {
    // Lazy init: do not start I2S during boot.
    // Detection alert chirps obey Misc > Sound ON/OFF.
    // Menu feedback has its own config setting and can initialize I2S independently.
    if (requireAlertSoundEnabled && !soundEnabled) return false;
    if (!soundReady) initSound();
    return soundReady;
}

static void soundTone(uint16_t freqHz, uint16_t durationMs, uint8_t volumePct = 255, bool requireAlertSoundEnabled = true) {
    if (freqHz == 0 || durationMs == 0) return;
    if (!ensureSoundReady(requireAlertSoundEnabled)) return;

    if (volumePct == 255) volumePct = (uint8_t)alertSoundVolumePercent;
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

    // These are only short alert/menu chirps, so shut I2S back down after
    // each chirp. GPIO0 remains a normal encoder/select input only; the old
    // background GPIO0 long-hold shutdown watcher has been removed.
    soundSilence(25);
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    soundReady = false;

    // Re-assert the encoder button input mode after I2S shuts down.
    pinMode(ENCODER_BTN, INPUT_PULLUP);
}

static void playDeauthChirp() {
    if (!soundCooldownReady(lastDeauthSoundMs)) return;
    soundTone(2100, 65, (uint8_t)alertSoundVolumePercent);
    soundSilence(45);
    soundTone(2100, 65, (uint8_t)alertSoundVolumePercent);
    stopSoundDriverAfterChirp();
}

static void playFlockChirp() {
    if (!soundCooldownReady(lastFlockSoundMs)) return;
    soundTone(520, 170, (uint8_t)alertSoundVolumePercent);
    soundSilence(50);
    soundTone(390, 210, (uint8_t)alertSoundVolumePercent);
    stopSoundDriverAfterChirp();
}

static void playPwnagotchiChirp() {
    if (!soundCooldownReady(lastPwnSoundMs)) return;
    soundTone(880, 80, (uint8_t)alertSoundVolumePercent);
    soundSilence(35);
    soundTone(1175, 80, (uint8_t)alertSoundVolumePercent);
    soundSilence(35);
    soundTone(1568, 110, (uint8_t)alertSoundVolumePercent);
    stopSoundDriverAfterChirp();
}

static void playFlipperChirp() {
    if (!soundCooldownReady(lastFlipSoundMs)) return;
    soundTone(1200, 55, (uint8_t)alertSoundVolumePercent);
    soundSilence(30);
    soundTone(1600, 55, (uint8_t)alertSoundVolumePercent);
    soundSilence(30);
    soundTone(1000, 70, (uint8_t)alertSoundVolumePercent);
    stopSoundDriverAfterChirp();
}

// Tesla Detector uses the same alert-style tone family as Flipper,
// but keeps its own cooldown so a Tesla hit is not blocked by a
// recent Flipper alert.
static void playTeslaChirp() {
    if (!soundCooldownReady(lastTeslaSoundMs)) return;
    soundTone(1200, 55, (uint8_t)alertSoundVolumePercent);
    soundSilence(30);
    soundTone(1600, 55, (uint8_t)alertSoundVolumePercent);
    soundSilence(30);
    soundTone(1000, 70, (uint8_t)alertSoundVolumePercent);
    stopSoundDriverAfterChirp();
}

static void playBLESuspiciousChirp() {
    if (!soundCooldownReady(lastBleSusSoundMs)) return;
    soundTone(430, 120, (uint8_t)alertSoundVolumePercent);
    soundSilence(80);
    soundTone(430, 120, (uint8_t)alertSoundVolumePercent);
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
    soundTone(1450, 8, (uint8_t)menuFeedbackVolumePercent, false);
    stopSoundDriverAfterChirp();
}

static void playMenuClickFeedback() {
    if (!menuFeedbackEnabled) return;
    if (!menuFeedbackCooldownReady(lastMenuClickMs, 120)) return;

    // Short click for button/select actions.
    soundTone(950, 18, (uint8_t)menuFeedbackVolumePercent, false);
    stopSoundDriverAfterChirp();
}

static void playConnectApConnectedTone() {
#if CONNECT_AP_EVENT_SOUNDS_ENABLED
    if (!menuFeedbackEnabled) return;

    // Distinct rising 3-note "connected" tone.
    // This follows Misc > Menu Sounds ON/OFF and uses its own config volume.
    soundTone(660, 55, (uint8_t)CONNECT_AP_EVENT_VOLUME_PERCENT, false);
    soundSilence(30);
    soundTone(880, 65, (uint8_t)CONNECT_AP_EVENT_VOLUME_PERCENT, false);
    soundSilence(30);
    soundTone(1320, 90, (uint8_t)CONNECT_AP_EVENT_VOLUME_PERCENT, false);
    stopSoundDriverAfterChirp();
#endif
}

static void playConnectApDisconnectedTone() {
#if CONNECT_AP_EVENT_SOUNDS_ENABLED
    if (!menuFeedbackEnabled) return;

    // Distinct falling 2-note "disconnected" tone.
    // Kept short so it does not feel like an alert chirp.
    soundTone(520, 80, (uint8_t)CONNECT_AP_EVENT_VOLUME_PERCENT, false);
    soundSilence(40);
    soundTone(330, 110, (uint8_t)CONNECT_AP_EVENT_VOLUME_PERCENT, false);
    stopSoundDriverAfterChirp();
#endif
}

static void playLanDiscoveryDoneTone() {
#if LAN_DISCOVERY_DONE_SOUND_ENABLED
    if (!menuFeedbackEnabled) return;

    // Dedicated "LAN scan complete" tone.
    // Uses a quick low-high-low chirp pattern so it is distinct from
    // Connect AP connect/disconnect and detector alert sounds.
    soundTone(740, 45, (uint8_t)LAN_DISCOVERY_DONE_SOUND_VOLUME_PERCENT, false);
    soundSilence(25);
    soundTone(1175, 55, (uint8_t)LAN_DISCOVERY_DONE_SOUND_VOLUME_PERCENT, false);
    soundSilence(25);
    soundTone(930, 85, (uint8_t)LAN_DISCOVERY_DONE_SOUND_VOLUME_PERCENT, false);
    stopSoundDriverAfterChirp();
#endif
}

static void connectApSuppressDropTone(uint32_t ms) {
    // Used for manual disconnects and intentional reconnect attempts.
    // Unexpected AP loss still plays the disconnect tone.
    connectApSuppressDropToneUntilMs = millis() + ms;
}

static void connectApMarkConnectedState(bool connected) {
    connectApWatchInitialized = true;
    connectApWasConnected = connected;
}

static void processConnectApConnectionWatchdog() {
#if CONNECT_AP_EVENT_SOUNDS_ENABLED
    bool nowConnected = (WiFi.status() == WL_CONNECTED);

    if (!connectApWatchInitialized) {
        connectApWatchInitialized = true;
        connectApWasConnected = nowConnected;
        return;
    }

    // Connected -> disconnected without a manual/suppressed operation:
    // play the AP disconnect tone one time.
    if (connectApWasConnected && !nowConnected) {
        bool suppressed = ((int32_t)(millis() - connectApSuppressDropToneUntilMs) < 0);
        if (!suppressed) {
            Serial.println("[Connect AP] WiFi connection dropped unexpectedly.");
            playConnectApDisconnectedTone();
        }
    }

    connectApWasConnected = nowConnected;
#endif
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
    savePersistentAlertSoundSetting();
}


static const char *getAlertVolumeMenuLabel() {
    static char label[44];
    snprintf(label, sizeof(label), LV_SYMBOL_AUDIO "  Alert Volume: %d%%",
             alertSoundVolumePercent);
    return label;
}

static void updateAlertVolumeMenuLabel() {
    if (!miscAlertVolumeBtn) return;

    lv_obj_t *label = lv_obj_get_child(miscAlertVolumeBtn, 0);
    if (label) {
        lv_label_set_text(label, getAlertVolumeMenuLabel());
    }
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
    savePersistentMenuSoundSetting();
}

static const char *getMenuVolumeMenuLabel() {
    static char label[44];
    snprintf(label, sizeof(label), LV_SYMBOL_AUDIO "  Menu Volume: %d%%",
             menuFeedbackVolumePercent);
    return label;
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

    // Once OK/Esc is selected, freeze LVGL input until loop() safely finishes
    // the keyboard close after the physical button has been released. This
    // prevents the disabled button matrix from receiving extra pressed/release
    // events while the close is pending.
    if (keyboardFinishPending) {
        if (pos != 0) encoder.setPosition(0);
        wasPressed = false;
        data->enc_diff = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (pos != 0) {
        // Encoder movement uses the normal quiet menu tick everywhere,
        // including the keyboard, so scrolling across keys has feedback again.
        playMenuTickFeedback();
    }

    if (!keyboardActive && pressed && !wasPressed) {
        // Normal menus can click immediately. Keyboard key clicks are delayed
        // from cb_keyboardMatrix() until GPIO0 is released, which avoids the
        // earlier keyboard OK/Back reboot behavior.
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

// Draw one boot spinner frame directly to the APA102 ring.
// This intentionally avoids the FreeRTOS spinner task because setup() is
// still booting and we want a simple, predictable startup animation.
static void writeBootSpinnerFrame(uint8_t pos, uint8_t r, uint8_t g, uint8_t b) {
    rgb_color frame[NUM_LEDS];

    for (int i = 0; i < NUM_LEDS; i++) frame[i] = {0, 0, 0};

    uint8_t head = pos % NUM_LEDS;
    uint8_t tail = (pos + NUM_LEDS - 1) % NUM_LEDS;

    frame[head] = {r, g, b};
    frame[tail] = {
        (uint8_t)(r * BOOT_LIGHTS_TAIL_PERCENT / 100),
        (uint8_t)(g * BOOT_LIGHTS_TAIL_PERCENT / 100),
        (uint8_t)(b * BOOT_LIGHTS_TAIL_PERCENT / 100)
    };

    // Boot lights have their own brightness setting. If desired, config can
    // still make them respect the runtime LEDs ON/OFF toggle.
#if BOOT_LIGHTS_RESPECT_LED_TOGGLE
    ledStrip.write(frame, NUM_LEDS, activeLedBrightness(BOOT_LIGHTS_BRIGHTNESS));
#else
    ledStrip.write(frame, NUM_LEDS, BOOT_LIGHTS_BRIGHTNESS);
#endif
}

// Spin one color for the configured boot duration.
// It always completes at least BOOT_LIGHTS_MIN_REVOLUTIONS full circles so
// each color is visible even if the duration/step settings are tuned short.
static void runBootColorSpin(uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t minFrames = NUM_LEDS * BOOT_LIGHTS_MIN_REVOLUTIONS;
    const uint16_t timeFrames = (BOOT_LIGHTS_COLOR_MS + BOOT_LIGHTS_STEP_MS - 1) / BOOT_LIGHTS_STEP_MS;
    const uint16_t totalFrames = (timeFrames > minFrames) ? timeFrames : minFrames;

    for (uint16_t frame = 0; frame < totalFrames; frame++) {
        writeBootSpinnerFrame(frame % NUM_LEDS, r, g, b);
        delay(BOOT_LIGHTS_STEP_MS);
    }

#if BOOT_LIGHTS_COLOR_GAP_MS > 0
    // Briefly clear the ring between colors so red/green/blue are distinct.
    rgb_color darkFrame[NUM_LEDS];
    for (int i = 0; i < NUM_LEDS; i++) darkFrame[i] = {0, 0, 0};
    ledStrip.write(darkFrame, NUM_LEDS, 0);
    delay(BOOT_LIGHTS_COLOR_GAP_MS);
#endif
}

// Optional boot animation: red circle, green circle, blue circle.
// This runs before the splash screen is drawn so the LCD image can display
// cleanly afterward for the full SPLASH_DURATION_MS.
void runBootLightsAnimation() {
#if BOOT_LIGHTS_ENABLED
#if BOOT_LIGHTS_RESPECT_LED_TOGGLE
    if (!ledsEnabled) {
        return;
    }
#endif

    runBootColorSpin(255, 0, 0);
    runBootColorSpin(0, 255, 0);
    runBootColorSpin(0, 0, 255);

#if BOOT_LIGHTS_RESTORE_MENU_COLOR
    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
#else
    setAllLEDs(0, 0, 0, 0);
#endif

#if BOOT_LIGHTS_END_HOLD_MS > 0
    delay(BOOT_LIGHTS_END_HOLD_MS);
#endif
#endif
}

void ledStartupFlash() {
#if BOOT_LIGHTS_ENABLED
    // When boot lights are enabled, avoid adding the older white flash after
    // the splash screen. Just restore the normal WiFi/menu LED color.
    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
#else
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

// Small color wheel used by the LAN Host Discovery rainbow spinner.
static rgb_color wheelColor(uint8_t pos) {
    pos = 255 - pos;
    if (pos < 85) {
        return { (uint8_t)(255 - pos * 3), 0, (uint8_t)(pos * 3) };
    }
    if (pos < 170) {
        pos -= 85;
        return { 0, (uint8_t)(pos * 3), (uint8_t)(255 - pos * 3) };
    }
    pos -= 170;
    return { (uint8_t)(pos * 3), (uint8_t)(255 - pos * 3), 0 };
}

// Rainbow chase spinner for connected LAN tools that perform blocking TCP probes.
static void ledRainbowSpinnerTask(void *param) {
    uint8_t pos = 0;
    rgb_color frame[NUM_LEDS];

    while (rainbowSpinnerRunning) {
        for (int i = 0; i < NUM_LEDS; i++) frame[i] = {0, 0, 0};

        uint8_t head = pos % NUM_LEDS;
        uint8_t mid  = (pos + NUM_LEDS - 1) % NUM_LEDS;
        uint8_t tail = (pos + NUM_LEDS - 2) % NUM_LEDS;

        rgb_color c = wheelColor((uint8_t)(pos * 32));
        frame[head] = c;
        frame[mid]  = { (uint8_t)(c.red * 35 / 100),
                        (uint8_t)(c.green * 35 / 100),
                        (uint8_t)(c.blue * 35 / 100) };
        frame[tail] = { (uint8_t)(c.red * 12 / 100),
                        (uint8_t)(c.green * 12 / 100),
                        (uint8_t)(c.blue * 12 / 100) };

        ledStrip.write(frame, NUM_LEDS, activeLedBrightness(LED_BRIGHTNESS));
        pos = (pos + 1) % NUM_LEDS;
        vTaskDelay(pdMS_TO_TICKS(rainbowSpinnerDelayMs));
    }

    rainbowSpinnerTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

static void startLEDRainbowSpinner(uint16_t delayMs = 70) {
    if (rainbowSpinnerRunning || rainbowSpinnerTaskHandle != nullptr) return;
    rainbowSpinnerDelayMs = delayMs;
    rainbowSpinnerRunning = true;
    xTaskCreatePinnedToCore(
        ledRainbowSpinnerTask,
        "ledRainbowSpin",
        2048,
        nullptr,
        1,
        &rainbowSpinnerTaskHandle,
        0
    );
}

static void stopLEDRainbowSpinner(uint8_t r, uint8_t g, uint8_t b, uint8_t br = LED_BRIGHTNESS) {
    rainbowSpinnerRunning = false;
    uint32_t deadline = millis() + 400;
    while (rainbowSpinnerTaskHandle != nullptr && millis() < deadline) delay(10);
    setAllLEDs(r, g, b, br);
}

// ════════════════════════════════════════════════════════════════
//  UI STYLE HELPERS
// ════════════════════════════════════════════════════════════════
static void applyScreenStyle(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, TC(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr,   LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

// ── Battery Meter Helpers ───────────────────────────────────────
// Reads the T-Embed LiPo battery voltage from the configured ADC pin
// and displays a compact percentage in the right side of the top bar.
static lv_obj_t *batteryTopLabel = nullptr;

// Cached battery values prevent rapid menu changes from causing jumpy readings
// and keep ADC work out of frequent UI redraw paths.
static int      batteryCachedRaw     = 0;
static float    batteryCachedVolts   = 0.0f;
static int      batteryCachedPercent = 0;
static uint32_t batteryLastReadMs    = 0;
static bool     batteryCacheValid    = false;

// Shared top-bar battery display value.
// This prevents Home / WiFi / BLE / Misc headers from showing different
// percentages just because each screen was created at a different time.
static int      batteryDisplayPercent      = 0;
static uint32_t batteryDisplayLastUpdateMs = 0;
static bool     batteryDisplayValid        = false;

#if BATTERY_METER_ENABLED
static int batteryVoltageToPercent(float volts) {
    // Approximate single-cell LiPo curve. This is intentionally conservative
    // and can be calibrated later after comparing against a charger/multimeter.
    if (volts >= 4.20f) return 100;
    if (volts <= 3.30f) return 0;

    struct BatteryPoint { float v; int p; };
    static const BatteryPoint curve[] = {
        {4.20f, 100}, {4.10f, 90}, {4.00f, 80}, {3.92f, 70},
        {3.85f, 60},  {3.79f, 50}, {3.75f, 40}, {3.70f, 30},
        {3.65f, 20},  {3.55f, 10}, {3.30f, 0}
    };

    for (size_t i = 0; i < (sizeof(curve) / sizeof(curve[0])) - 1; i++) {
        if (volts <= curve[i].v && volts >= curve[i + 1].v) {
            float spanV = curve[i].v - curve[i + 1].v;
            float spanP = (float)(curve[i].p - curve[i + 1].p);
            float frac  = (volts - curve[i + 1].v) / spanV;
            int pct = curve[i + 1].p + (int)(frac * spanP + 0.5f);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            return pct;
        }
    }
    return 0;
}

static void readBatterySnapshot(int *rawOut, float *voltsOut, int *percentOut) {
    uint32_t now = millis();

    // Only sample the ADC when the cache is stale. This avoids doing analogRead()
    // repeatedly during fast menu navigation / encoder use.
    if (!batteryCacheValid || (now - batteryLastReadMs >= (uint32_t)BATTERY_UPDATE_MS)) {
        uint32_t rawSum = 0;
        const uint8_t samples = BATTERY_AVG_SAMPLES;
        for (uint8_t i = 0; i < samples; i++) {
            rawSum += (uint32_t)analogRead(BATTERY_ADC_PIN);
            delay(1);
        }

        batteryCachedRaw = (int)(rawSum / samples);
        float adcVoltage = ((float)batteryCachedRaw / BATTERY_ADC_RESOLUTION) * BATTERY_ADC_REF_VOLTAGE;
        batteryCachedVolts = adcVoltage * BATTERY_DIVIDER_RATIO;
        batteryCachedPercent = batteryVoltageToPercent(batteryCachedVolts);
        batteryLastReadMs = now;
        batteryCacheValid = true;
    }

    if (rawOut)     *rawOut = batteryCachedRaw;
    if (voltsOut)   *voltsOut = batteryCachedVolts;
    if (percentOut) *percentOut = batteryCachedPercent;
}

static void refreshBatteryDisplayPercent(bool force = false) {
    uint32_t now = millis();

    // Top-bar percent is intentionally stickier than raw ADC reads.
    // All headers use this one shared value so each menu shows the same
    // battery percent instead of re-sampling differently per screen.
    if (force || !batteryDisplayValid ||
        (now - batteryDisplayLastUpdateMs >= (uint32_t)BATTERY_DISPLAY_UPDATE_MS)) {
        int raw = 0;
        float volts = 0.0f;
        int percent = 0;

        readBatterySnapshot(&raw, &volts, &percent);

        batteryDisplayPercent = percent;
        batteryDisplayLastUpdateMs = now;
        batteryDisplayValid = true;
    }
}

static void updateBatteryTopLabel() {
    // Important: this is only called while creating a fresh header. Avoid using
    // an LVGL timer here because screen changes delete old header objects and a
    // timer can randomly update a stale label pointer during encoder navigation.
    if (!batteryTopLabel) return;

    // Normal screens may refresh the shared battery value when it is stale.
    // The keyboard screen keeps the existing shared value to avoid an ADC read
    // during heavy LVGL matrix drawing, which caused an occasional 0% top-bar
    // reading while plugged in.
    if (!batteryDisplayValid) {
        refreshBatteryDisplayPercent(true);
    } else if (!keyboardActive) {
        refreshBatteryDisplayPercent(false);
    }

    int percent = batteryDisplayPercent;

    char text[20];
    snprintf(text, sizeof(text), "BAT %d%%", percent);
    lv_label_set_text(batteryTopLabel, text);

    uint32_t col = TH.success;
    if (percent <= BATTERY_CRITICAL_PERCENT) col = TH.alert;
    else if (percent <= BATTERY_WARN_PERCENT) col = TH.warn;
    lv_obj_set_style_text_color(batteryTopLabel, lv_color_hex(col), LV_PART_MAIN);
}
#endif

static const char *getTopbarWifiIconText() {
#if defined(TOPBAR_WIFI_ICON_CUSTOM_TEXT)
    // Config override: leave blank to use LVGL's built-in WiFi symbol.
    if (strlen(TOPBAR_WIFI_ICON_CUSTOM_TEXT) > 0) {
        return TOPBAR_WIFI_ICON_CUSTOM_TEXT;
    }
#endif
    return LV_SYMBOL_WIFI;
}

static lv_obj_t *topbarWifiOverlayLabel = nullptr;
static wl_status_t topbarWifiLastStatus = WL_IDLE_STATUS;
static int topbarWifiLastTheme = -1;

// Persistent top-layer WiFi indicator.
// Unlike the per-screen header label, this stays visible across every page
// while the device remains connected to an AP.
static void updateTopbarWifiOverlay(bool force = false) {
#if TOPBAR_WIFI_ICON_ENABLED
    if (!lvDisp) return;

    wl_status_t status = WiFi.status();
    bool connected = (status == WL_CONNECTED);

    if (!topbarWifiOverlayLabel) {
        topbarWifiOverlayLabel = lv_label_create(lv_layer_top());
        lv_obj_set_width(topbarWifiOverlayLabel, 22);
        lv_label_set_long_mode(topbarWifiOverlayLabel, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(topbarWifiOverlayLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(topbarWifiOverlayLabel, TC(accent), LV_PART_MAIN);
#if BATTERY_METER_ENABLED
        lv_obj_align(topbarWifiOverlayLabel, LV_ALIGN_TOP_RIGHT, -88, 5);
#else
        lv_obj_align(topbarWifiOverlayLabel, LV_ALIGN_TOP_RIGHT, -8, 5);
#endif
        lv_obj_add_flag(topbarWifiOverlayLabel, LV_OBJ_FLAG_HIDDEN);
    }

    if (force || status != topbarWifiLastStatus || currentTheme != topbarWifiLastTheme) {
        lv_label_set_text(topbarWifiOverlayLabel, getTopbarWifiIconText());
        lv_obj_set_style_text_color(topbarWifiOverlayLabel, TC(accent), LV_PART_MAIN);
#if BATTERY_METER_ENABLED
        lv_obj_align(topbarWifiOverlayLabel, LV_ALIGN_TOP_RIGHT, -88, 5);
#else
        lv_obj_align(topbarWifiOverlayLabel, LV_ALIGN_TOP_RIGHT, -8, 5);
#endif

        if (connected) {
            lv_obj_clear_flag(topbarWifiOverlayLabel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(topbarWifiOverlayLabel);
        } else {
            lv_obj_add_flag(topbarWifiOverlayLabel, LV_OBJ_FLAG_HIDDEN);
        }

        topbarWifiLastStatus = status;
        topbarWifiLastTheme = currentTheme;
    }
#else
    if (topbarWifiOverlayLabel) {
        lv_obj_add_flag(topbarWifiOverlayLabel, LV_OBJ_FLAG_HIDDEN);
    }
#endif
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

#if BATTERY_METER_ENABLED
    // Compact battery percentage on the opposite side of the top bar.
    batteryTopLabel = nullptr;
    batteryTopLabel = lv_label_create(bar);
    lv_obj_set_width(batteryTopLabel, 78);
    lv_label_set_long_mode(batteryTopLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(batteryTopLabel, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(batteryTopLabel, LV_ALIGN_RIGHT_MID, -8, 0);
    updateBatteryTopLabel();
#endif

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


// ════════════════════════════════════════════════════════════════
//  REUSABLE LVGL KEYBOARD
// ════════════════════════════════════════════════════════════════
// Inspired by the Launcher keyboard layout concept, but rewritten for
// Rogue Radar's LVGL + encoder UI instead of direct TFT drawing.
//
// IMPORTANT:
// This version uses ONE lv_btnmatrix object instead of 48 separate LVGL
// buttons. An early standalone keyboard validation build froze on-device right before the
// keyboard loaded, most likely because the old test created too many LVGL
// button/label objects at once from a menu callback. The button-matrix
// version is much lighter and should be safer on the T-Embed.
//
// Future tools can call:
//
//   createKeyboardScreen("SSID", "", 32, myCallback);
//
// The callback receives:
//   accepted=true  -> text contains the entered value
//   accepted=false -> user cancelled with Back/Esc

static lv_obj_t *keyboardMatrix = nullptr;
static lv_obj_t *keyboardScreenDeletePending = nullptr;
static lv_timer_t *keyboardScreenDeleteTimer = nullptr;

static const char *RR_KB_MAP_LOWER[] = {
    "OK", "caps", "Del", "Space", "Esc", "\n",
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", ";", "'", "\\", "\n",
    "z", "x", "c", "v", "b", "n", "m", ",", ".", "/", "@", ".",
    ""
};

static const char *RR_KB_MAP_UPPER[] = {
    "OK", "CAPS", "Del", "Space", "Esc", "\n",
    "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "_", "+", "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "{", "}", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", ":", "\"", "|", "\n",
    "Z", "X", "C", "V", "B", "N", "M", "<", ">", "?", "@", ".",
    ""
};

static void keyboardRefreshText() {
    if (keyboardTextLabel) {
        lv_label_set_text(keyboardTextLabel,
                          keyboardCurrentText.length() ? keyboardCurrentText.c_str() : "_");
    }

    if (keyboardCountLabel) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%u/%d",
                 (unsigned)keyboardCurrentText.length(), keyboardMaxLen);
        lv_label_set_text(keyboardCountLabel, buf);
    }
}

static void keyboardRefreshCaps() {
    if (keyboardCapsLabel) {
        lv_label_set_text(keyboardCapsLabel, keyboardCaps ? "CAPS" : "caps");
    }

    if (keyboardMatrix) {
        lv_btnmatrix_set_map(keyboardMatrix, keyboardCaps ? RR_KB_MAP_UPPER : RR_KB_MAP_LOWER);
    }
}

// Keyboard close helper:
// Do not delete the active keyboard screen before the next screen has loaded.
// Deleting the active screen while LVGL is still processing the encoder/button
// event can cause a LoadProhibited-style reboot on the T-Embed.
static void keyboardDeleteOldScreenTimerCb(lv_timer_t *timer) {
    if (timer) {
        lv_timer_delete(timer);
    }
    keyboardScreenDeleteTimer = nullptr;

    if (!keyboardScreenDeletePending) return;

    // If the old keyboard screen is still active, leave it alone rather than
    // deleting the currently loaded screen. This keeps the fail-safe behavior
    // as "minor memory leak during testing" instead of "reboot".
    if (keyboardScreenDeletePending != lv_screen_active()) {
        lv_obj_delete(keyboardScreenDeletePending);
    }

    keyboardScreenDeletePending = nullptr;
}

static void keyboardQueueOldScreenDelete(lv_obj_t *oldScreen, uint32_t delayMs) {
    if (!oldScreen) return;

    if (keyboardScreenDeleteTimer) {
        lv_timer_delete(keyboardScreenDeleteTimer);
        keyboardScreenDeleteTimer = nullptr;
    }

    // Clean up any older pending screen first, but never delete the active one.
    if (keyboardScreenDeletePending &&
        keyboardScreenDeletePending != oldScreen &&
        keyboardScreenDeletePending != lv_screen_active()) {
        lv_obj_delete(keyboardScreenDeletePending);
    }

    keyboardScreenDeletePending = oldScreen;
    keyboardScreenDeleteTimer = lv_timer_create(keyboardDeleteOldScreenTimerCb, delayMs, nullptr);
}

static void keyboardFinish(bool accepted) {
    RogueKeyboardCallback cb = keyboardDoneCb;
    String out = keyboardCurrentText;
    lv_obj_t *oldKeyboardScreen = keyboardScreen;

    if (keyboardFinishTimer) {
        lv_timer_delete(keyboardFinishTimer);
        keyboardFinishTimer = nullptr;
    }

    // Important: detach the encoder indev before deleting the keyboard group.
    // Otherwise LVGL can keep an input-device pointer to a group that no
    // longer exists, which can reboot when OK/Esc closes the keyboard.
    if (lvIndev) {
        lv_indev_set_group(lvIndev, nullptr);
    }
    deleteGroup(&keyboardGroup);

    // Clear keyboard object pointers now. The old screen is deleted later,
    // after the callback has loaded the next screen.
    keyboardScreen = nullptr;
    keyboardMatrix = nullptr;
    keyboardTextLabel = nullptr;
    keyboardCountLabel = nullptr;
    keyboardCapsLabel = nullptr;
    memset(keyboardKeyLabels, 0, sizeof(keyboardKeyLabels));
    keyboardDoneCb = nullptr;
    keyboardActive = false;
    keyboardFinishPending = false;
    keyboardFinishReadyAtMs = 0;
    keyboardButtonReleasedAtMs = 0;

    // GPIO0 is the encoder button. The old background long-hold shutdown
    // watcher has been removed, so keyboard close only needs the normal
    // deferred release flow below.

    if (cb) {
        cb(accepted ? out.c_str() : "", accepted);
    }

    // Delete the old keyboard screen only after the callback has had a chance
    // to load the next tool/result page. This avoids deleting
    // the active screen from inside the close flow.
    keyboardQueueOldScreenDelete(oldKeyboardScreen, 450);
}

static void queueKeyboardClickFeedback() {
    // Keyboard key clicks are delayed until after the encoder button is released.
    // This keeps the I2S feedback sound away from the GPIO0 LOW period that caused
    // earlier keyboard OK/Back instability on the T-Embed.
    if (!menuFeedbackEnabled) return;
    if (keyboardFinishPending) return;

    keyboardClickFeedbackPending = true;
    keyboardClickFeedbackReadyAtMs = millis() + 40;
    keyboardClickFeedbackReleaseAtMs = 0;
}

static void processKeyboardClickFeedback() {
    if (!keyboardClickFeedbackPending) return;

    if ((int32_t)(millis() - keyboardClickFeedbackReadyAtMs) < 0) return;

    // Wait for the encoder button to be released and stable briefly before
    // starting/stopping the I2S driver for the click sound.
    if (digitalRead(ENCODER_BTN) == LOW) {
        keyboardClickFeedbackReleaseAtMs = 0;
        return;
    }

    if (keyboardClickFeedbackReleaseAtMs == 0) {
        keyboardClickFeedbackReleaseAtMs = millis();
        return;
    }

    if ((millis() - keyboardClickFeedbackReleaseAtMs) < 80) return;

    keyboardClickFeedbackPending = false;
    keyboardClickFeedbackReadyAtMs = 0;
    keyboardClickFeedbackReleaseAtMs = 0;
    playMenuClickFeedback();
}

static void keyboardFinishTimerCb(lv_timer_t *timer) {
    // Legacy safety stub: the keyboard no longer closes from an LVGL timer.
    // OK/Back now queue the finish, then loop() closes the keyboard only after
    // the encoder button has been released. That avoids deleting/loading screens
    // while GPIO0 is still held LOW.
    if (timer) {
        lv_timer_delete(timer);
    }
    keyboardFinishTimer = nullptr;
}

static void keyboardRequestFinish(bool accepted) {
    // Do NOT delete/load screens directly inside the button-matrix event.
    // Also do NOT close immediately while the encoder button is still held.
    // GPIO0 is the encoder button on T-Embed and is also boot/power sensitive.
    // We queue the finish, disable the matrix, then loop() waits for button
    // release before calling keyboardFinish().
    if (keyboardFinishPending) return;

    keyboardFinishPending = true;
    keyboardFinishAccepted = accepted;

    // Do not allow a queued keyboard click to fire during OK/Esc shutdown.
    // The close path already has its own GPIO0 guard/debounce flow.
    keyboardClickFeedbackPending = false;
    keyboardClickFeedbackReadyAtMs = 0;
    keyboardClickFeedbackReleaseAtMs = 0;
    keyboardFinishReadyAtMs = millis() + 120;
    keyboardButtonReleasedAtMs = 0;

    if (keyboardMatrix) {
        lv_obj_add_state(keyboardMatrix, LV_STATE_DISABLED);
    }

    Serial.printf("[Keyboard] Finish queued: %s\n", accepted ? "OK" : "Esc");
}

static void processKeyboardDeferredFinish() {
    if (!keyboardFinishPending) return;

    // Keep the keyboard finish flow deferred until the button is released.

    if ((int32_t)(millis() - keyboardFinishReadyAtMs) < 0) return;

    // Wait until the physical encoder button is released and stays released
    // briefly before changing screens. GPIO0 can bounce on release, and on the
    // T-Embed it is also boot/power-sensitive.
    if (digitalRead(ENCODER_BTN) == LOW) {
        keyboardButtonReleasedAtMs = 0;
        return;
    }

    if (keyboardButtonReleasedAtMs == 0) {
        keyboardButtonReleasedAtMs = millis();
        return;
    }

    if ((millis() - keyboardButtonReleasedAtMs) < 300) {
        return;
    }

    Serial.printf("[Keyboard] Finish running: %s\n", keyboardFinishAccepted ? "OK" : "Esc");
    keyboardFinish(keyboardFinishAccepted);
}

static void cb_keyboardMatrix(lv_event_t *e) {
    lv_obj_t *matrix = (lv_obj_t *)lv_event_get_target(e);
    uint16_t selected = lv_btnmatrix_get_selected_btn(matrix);

    if (selected == LV_BTNMATRIX_BTN_NONE) return;

    const char *txt = lv_btnmatrix_get_button_text(matrix, selected);
    if (!txt || !txt[0]) return;
    if (keyboardFinishPending) return;

    resetInactivityTimer();

    if (strcmp(txt, "OK") == 0) {
        keyboardRequestFinish(true);
        return;
    }

    if (strcmp(txt, "Esc") == 0 || strcmp(txt, "Back") == 0) {
        keyboardRequestFinish(false);
        return;
    }

    // Normal keyboard keys get the same quiet menu/key click sound, but the
    // click is queued and played only after GPIO0 is released for stability.
    queueKeyboardClickFeedback();

    if (strcmp(txt, "caps") == 0 || strcmp(txt, "CAPS") == 0) {
        keyboardCaps = !keyboardCaps;
        keyboardRefreshCaps();
        return;
    }

    if (strcmp(txt, "Del") == 0) {
        if (keyboardCurrentText.length() > 0) {
            keyboardCurrentText.remove(keyboardCurrentText.length() - 1);
            keyboardRefreshText();
        }
        return;
    }

    if (strcmp(txt, "Space") == 0) {
        if ((int)keyboardCurrentText.length() < keyboardMaxLen) {
            keyboardCurrentText += ' ';
            keyboardRefreshText();
        }
        return;
    }

    // Normal key: each key label is a one-character string.
    if ((int)keyboardCurrentText.length() < keyboardMaxLen) {
        keyboardCurrentText += txt[0];
        keyboardRefreshText();
    }
}

void createKeyboardScreen(const char *title,
                          const char *currentText,
                          int maxLen,
                          RogueKeyboardCallback cb) {
    Serial.println("[Keyboard] Opening LVGL keyboard...");

    if (keyboardScreen) {
        lv_obj_delete(keyboardScreen);
        keyboardScreen = nullptr;
    }

    keyboardActive = true;
    keyboardFinishPending = false;
    keyboardFinishAccepted = false;
    keyboardFinishReadyAtMs = 0;
    keyboardButtonReleasedAtMs = 0;
    keyboardClickFeedbackPending = false;
    keyboardClickFeedbackReadyAtMs = 0;
    keyboardClickFeedbackReleaseAtMs = 0;

    if (keyboardFinishTimer) {
        lv_timer_delete(keyboardFinishTimer);
        keyboardFinishTimer = nullptr;
    }

    keyboardCurrentText = currentText ? currentText : "";
    keyboardMaxLen = maxLen > 0 ? maxLen : 64;
    keyboardCaps = false;
    keyboardDoneCb = cb;
    keyboardMatrix = nullptr;
    memset(keyboardKeyLabels, 0, sizeof(keyboardKeyLabels));

    keyboardScreen = lv_obj_create(nullptr);
    applyScreenStyle(keyboardScreen);
    createHeader(keyboardScreen, title ? title : "Keyboard");

    deleteGroup(&keyboardGroup);
    keyboardGroup = lv_group_create();

    // Text preview area
    lv_obj_t *box = lv_obj_create(keyboardScreen);
    lv_obj_set_size(box, SCREEN_W - 10, 28);
    lv_obj_set_pos(box, 5, 30);
    lv_obj_set_style_bg_color(box, TC(card), LV_PART_MAIN);
    lv_obj_set_style_border_color(box, TC(border), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 4, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    keyboardTextLabel = lv_label_create(box);
    lv_obj_set_width(keyboardTextLabel, SCREEN_W - 65);
    lv_label_set_long_mode(keyboardTextLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(keyboardTextLabel, TC(text), LV_PART_MAIN);
    lv_obj_align(keyboardTextLabel, LV_ALIGN_LEFT_MID, 2, 0);

    keyboardCountLabel = lv_label_create(box);
    lv_obj_set_style_text_color(keyboardCountLabel, TC(textDim), LV_PART_MAIN);
    lv_obj_align(keyboardCountLabel, LV_ALIGN_RIGHT_MID, -2, 0);

    keyboardMatrix = lv_btnmatrix_create(keyboardScreen);
    lv_btnmatrix_set_map(keyboardMatrix, RR_KB_MAP_LOWER);
    lv_obj_set_size(keyboardMatrix, SCREEN_W - 8, SCREEN_H - 64);
    lv_obj_set_pos(keyboardMatrix, 4, 62);

    // Keyboard stability helpers:
    // - no scrollbars/scroll-on-focus inside the matrix
    // - no style animations during encoder movement
    // These reduce flicker on the small T-Embed LVGL display.
    lv_obj_clear_flag(keyboardMatrix, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(keyboardMatrix, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scrollbar_mode(keyboardMatrix, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_anim_duration(keyboardMatrix, 0, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(keyboardMatrix, 0, LV_PART_ITEMS);

    // Compact styling for the 320x170 T-Embed screen.
    lv_obj_set_style_bg_color(keyboardMatrix, TC(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(keyboardMatrix, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(keyboardMatrix, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(keyboardMatrix, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_row(keyboardMatrix, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_column(keyboardMatrix, 1, LV_PART_MAIN);

    lv_obj_set_style_bg_color(keyboardMatrix, TC(cardAlt), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(keyboardMatrix, TC(btnFocus), LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(keyboardMatrix, TC(btnPress), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keyboardMatrix, TC(text), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(keyboardMatrix, TC(text), LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(keyboardMatrix, TC(border), LV_PART_ITEMS);
    lv_obj_set_style_border_width(keyboardMatrix, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboardMatrix, 3, LV_PART_ITEMS);

    lv_obj_add_event_cb(keyboardMatrix, cb_keyboardMatrix, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_group_add_obj(keyboardGroup, keyboardMatrix);

    keyboardRefreshText();
    keyboardRefreshCaps();

    setGroup(keyboardGroup);

    // Button matrices need encoder edit mode so rotating the encoder moves
    // between keys instead of trying to leave the matrix object.
    lv_group_set_editing(keyboardGroup, true);

    Serial.printf("[Keyboard] Created. Free heap: %u bytes\n", ESP.getFreeHeap());

    // Use a direct load for the keyboard test screen.
    // The slide animation looked nice, but on-device it added visible flicker
    // around the large button matrix. Direct load is cleaner/stabler here.
    lv_screen_load(keyboardScreen);
}

static void styleListBtn(lv_obj_t *btn) {
    // Do NOT use lv_obj_remove_style_all(btn) here.
    // It breaks the built-in list/button spacing and can stack text/icons.

    lv_obj_set_height(btn, 26);

    // Default row style
    lv_obj_set_style_bg_color(btn,   TC(cardAlt),  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn,     LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, TC(text),     LV_PART_MAIN | LV_STATE_DEFAULT);

    // Focused row style
    lv_obj_set_style_bg_color(btn,   TC(btnFocus), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn,     LV_OPA_COVER, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(btn, TC(accent), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn, 1,           LV_PART_MAIN | LV_STATE_FOCUSED);

    // Encoder/key focus state — helps override LVGL's default blue focus bar
    lv_obj_set_style_bg_color(btn,   TC(btnFocus), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(btn,     LV_OPA_COVER, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(btn, TC(accent), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(btn, 1,           LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    // Extra-specific focused + key-focused state
    lv_obj_set_style_bg_color(btn,   TC(btnFocus), LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(btn,     LV_OPA_COVER, LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(btn, TC(accent), LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(btn, 1,           LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);

    // Pressed/selected style
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
static const MenuItem MENU_ITEMS[5] = {
    { LV_SYMBOL_WIFI,      "WiFi Tools",   LV_SYMBOL_WIFI      "  WiFi Tools"   },
    { LV_SYMBOL_BLUETOOTH, "BLE Tools",    LV_SYMBOL_BLUETOOTH "  BLE Tools"    },
    { LV_SYMBOL_GPS,       "GPS Tools",    LV_SYMBOL_GPS       "  GPS Tools"    },
    { LV_SYMBOL_AUDIO,     "Audio Tools",  LV_SYMBOL_AUDIO     "  Audio Tools"  },
    { LV_SYMBOL_SETTINGS,  "Misc Tools",   LV_SYMBOL_SETTINGS  "  Misc Tools"   },
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
    else if (idx == 3) createAudioMenu();
    else if (idx == 4) createMiscMenu();
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

    for (int i = 0; i < 5; i++) {
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
    savePersistentRotationSetting();
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
    savePersistentDimmingSetting();
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
    savePersistentLedsSetting();
}

static void cb_miscMenuBack(lv_event_t *e) {
    miscMenuScreen = nullptr;
    deleteGroup(&miscMenuGroup);
    setGroup(navGroup);
    lv_screen_load_anim(mainScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b);
}

static void cb_miscToolBack(lv_event_t *e) {
    // If user backs out before the auto-set timer fires, keep the latest
    // chosen volume value instead of losing it.
    if (menuVolPendingAutoSet) {
        savePersistentMenuVolumeSetting();
    }
    if (alertVolPendingAutoSet) {
        savePersistentAlertVolumeSetting();
    }

    if (menuVolAutoTimer) {
        lv_timer_delete(menuVolAutoTimer);
        menuVolAutoTimer = nullptr;
    }
    menuVolPendingAutoSet = false;
    if (alertVolAutoTimer) {
        lv_timer_delete(alertVolAutoTimer);
        alertVolAutoTimer = nullptr;
    }
    alertVolPendingAutoSet = false;
    miscToolScreen = nullptr;
    deleteGroup(&miscToolGroup);
    setGroup(miscMenuGroup);
    lv_screen_load_anim(miscMenuScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b, 3);
}


// ── Misc Tool — Power Off ───────────────────────────────────────
static void performSoftwarePowerOff() {
    if (powerOffTriggered) return;
    powerOffTriggered = true;

    // Stop active WiFi promiscuous modes cleanly before pulling the latch low.
    if (deauthActive) {
        deauthActive = false;
        esp_wifi_set_promiscuous(false);
    }
    if (deauthTimer) {
        lv_timer_delete(deauthTimer);
        deauthTimer = nullptr;
    }

    // Show a simple final screen so the user gets clear feedback.
    lv_obj_t *offScr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(offScr, lv_color_hex(0x000000), LV_PART_MAIN);

    lv_obj_t *msg = lv_label_create(offScr);
    lv_label_set_text(msg, LV_SYMBOL_POWER "  Powering off...");
    lv_obj_set_style_text_color(msg, lv_color_hex(0xff4444), LV_PART_MAIN);
    lv_obj_center(msg);

    lv_screen_load(offScr);
    lv_timer_handler();

    // Silence lights/sound before shutting down.
    setAllLEDs(0, 0, 0, 0);
    if (soundReady) {
        stopSoundDriverAfterChirp();
    }

    delay(POWER_OFF_DELAY_MS);

    // T-Embed power latch: pulling POWER_PIN low turns the board off.
    digitalWrite(POWER_PIN, LOW);
}

static void cb_powerOffConfirm(lv_event_t *e) {
    (void)e;
    performSoftwarePowerOff();
}

void createPowerOffConfirm() {
    if (miscToolScreen) { lv_obj_delete(miscToolScreen); miscToolScreen = nullptr; }
    miscToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(miscToolScreen);
    createHeader(miscToolScreen, LV_SYMBOL_POWER "  Power Off");

    lv_obj_t *card = lv_obj_create(miscToolScreen);
    lv_obj_set_size(card, SCREEN_W - 18, 82);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_bg_color(card, TC(card), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, TC(border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg,
                      "Power off Rogue Radar?\n"
                      "This puts the T-Embed to sleep.\n (Not actual power off)");
    lv_obj_set_style_text_color(msg, TC(text), LV_PART_MAIN);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(msg, SCREEN_W - 42);
    lv_obj_center(msg);

    lv_obj_t *backBtn = createBackBtn(miscToolScreen, cb_miscToolBack);
    lv_obj_t *powerBtn = createActionBtn(miscToolScreen, LV_SYMBOL_POWER "  Power Off", cb_powerOffConfirm);
    lv_obj_set_style_bg_color(powerBtn, lv_color_hex(TH.stopRed), LV_PART_MAIN);
    lv_obj_set_style_border_color(powerBtn, lv_color_hex(TH.alert), LV_PART_MAIN);

    deleteGroup(&miscToolGroup);
    miscToolGroup = lv_group_create();
    lv_group_add_obj(miscToolGroup, backBtn);
    lv_group_add_obj(miscToolGroup, powerBtn);
    setGroup(miscToolGroup);

    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b, 3);
    lv_screen_load_anim(miscToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}


// ── Misc Tool — Reset Settings ─────────────────────────────────
static lv_obj_t *resetSettingsStatusLbl = nullptr;

static void cb_restartAfterReset(lv_timer_t *timer) {
    if (timer) lv_timer_delete(timer);
    ESP.restart();
}

static void cb_resetSettingsConfirm(lv_event_t *e) {
    (void)e;
    resetPersistentSettings();

    if (resetSettingsStatusLbl) {
        lv_label_set_text(resetSettingsStatusLbl,
                          "Saved settings cleared.\nRestarting with config.h defaults...");
        lv_obj_set_style_text_color(resetSettingsStatusLbl, TC(success), LV_PART_MAIN);
    }

    lv_timer_create(cb_restartAfterReset, 1200, nullptr);
}

void createResetSettings() {
    if (miscToolScreen) { lv_obj_delete(miscToolScreen); miscToolScreen = nullptr; }
    miscToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(miscToolScreen);
    createHeader(miscToolScreen, LV_SYMBOL_SETTINGS "  Reset Settings");

    lv_obj_t *card = lv_obj_create(miscToolScreen);
    lv_obj_set_size(card, SCREEN_W - 18, 92);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(card, TC(card), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, TC(border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);

    resetSettingsStatusLbl = lv_label_create(card);
    lv_label_set_text(resetSettingsStatusLbl,
                      "Clear all saved NVS settings?\n"
                      "Theme, brightness, rotation, LEDs, dimming,\n"
                      "sound volumes, and scan defaults will reset.");
    lv_obj_set_style_text_color(resetSettingsStatusLbl, TC(text), LV_PART_MAIN);
    lv_obj_set_style_text_align(resetSettingsStatusLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(resetSettingsStatusLbl, SCREEN_W - 40);
    lv_obj_center(resetSettingsStatusLbl);

    lv_obj_t *backBtn = createBackBtn(miscToolScreen, cb_miscToolBack);
    lv_obj_t *resetBtn = createActionBtn(miscToolScreen, "Clear & Restart", cb_resetSettingsConfirm);

    deleteGroup(&miscToolGroup);
    miscToolGroup = lv_group_create();
    lv_group_add_obj(miscToolGroup, backBtn);
    lv_group_add_obj(miscToolGroup, resetBtn);
    setGroup(miscToolGroup);

    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b, 3);
    lv_screen_load_anim(miscToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
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
        case 8: createAlertSoundVolumeControl(); break;
        case 9: toggleMenuFeedbackEnabled(); break;
        case 10: createMenuFeedbackVolumeControl(); break;
        case 11: toggleDisplayRotation();  break;
        case 12: createResetSettings();    break;
        case 13: createPowerOffConfirm();  break;
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
    miscAlertVolumeBtn = nullptr;
    miscRotationBtn = nullptr;

    for (int i = 0; i < 14; i++) {
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
            label = getAlertVolumeMenuLabel();
        } else if (i == 9) {
            label = getMenuSoundMenuLabel();
        } else if (i == 10) {
            label = getMenuVolumeMenuLabel();
        } else if (i == 11) {
            label = getRotationMenuLabel();
        } else if (i == 12) {
            // Reset Settings icon: use LVGL new-line symbol as requested.
            label = LV_SYMBOL_NEW_LINE "  Reset Settings";
        } else if (i == 13) {
            label = LV_SYMBOL_POWER "  Power Off";
        } else {
            label = "  Unknown";
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
            miscAlertVolumeBtn = btn;
        } else if (i == 9) {
            miscMenuSoundBtn = btn;
        } else if (i == 11) {
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

    // Device Info can grow as we add useful diagnostics, so keep this card scrollable.
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);

    // Gather device info at run time
    uint32_t heap     = ESP.getFreeHeap();
    uint32_t flash    = ESP.getFlashChipSize() / 1024;  // KB
    uint32_t flashSpd = ESP.getFlashChipSpeed() / 1000000; // MHz
    uint8_t cores     = ESP.getChipCores();
    uint32_t cpuMHz   = ESP.getCpuFreqMHz();
    const char *firmwareVer = FIRMWARE_VERSION;

#if BATTERY_METER_ENABLED
    int battRaw = 0;
    float battVolts = 0.0f;
    int battPct = 0;
    readBatterySnapshot(&battRaw, &battVolts, &battPct);
#endif

    // Device MAC addresses
    uint8_t staMac[6];
    uint8_t apMac[6];
    uint8_t btMac[6];
    esp_read_mac(staMac, ESP_MAC_WIFI_STA);
    esp_read_mac(apMac,  ESP_MAC_WIFI_SOFTAP);
    esp_read_mac(btMac,  ESP_MAC_BT);

    char staMacStr[18];
    char apMacStr[18];
    char btMacStr[18];
    snprintf(staMacStr, sizeof(staMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             staMac[0], staMac[1], staMac[2], staMac[3], staMac[4], staMac[5]);
    snprintf(apMacStr, sizeof(apMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             apMac[0], apMac[1], apMac[2], apMac[3], apMac[4], apMac[5]);
    snprintf(btMacStr, sizeof(btMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             btMac[0], btMac[1], btMac[2], btMac[3], btMac[4], btMac[5]);

    uint64_t efuseMac = ESP.getEfuseMac();

    char info[1000];
#if BATTERY_METER_ENABLED
    snprintf(info, sizeof(info),
             "HW       : %s\n"
             "FW       : %s\n"             
             "Chip     : ESP32-S3  (%d cores)\n"
             "CPU      : %lu MHz\n"
             "Flash    : %lu KB @ %lu MHz\n"
             "Heap     : %lu B free\n"
             "Battery  : %.2fV  %d%%  raw:%d\n"
             "Device MAC: %s\n"
             "WiFi AP  : %s\n"
             "BLE MAC  : %s\n"
             "eFuse ID : %04X%08lX",
             DEVICE_TYPE,
             firmwareVer,
             cores,
             (unsigned long)cpuMHz,
             (unsigned long)flash,
             (unsigned long)flashSpd,
             (unsigned long)heap,
             battVolts,
             battPct,
             battRaw,
             staMacStr,
             apMacStr,
             btMacStr,
             (uint16_t)(efuseMac >> 32),
             (unsigned long)(efuseMac & 0xFFFFFFFF));
#else
    snprintf(info, sizeof(info),
             "HW       : %s\n"
             "FW       : %s\n" 
             "Chip     : ESP32-S3  (%d cores)\n"
             "CPU      : %lu MHz\n"
             "Flash    : %lu KB @ %lu MHz\n"
             "Heap     : %lu B free\n"
             "Device MAC: %s\n"
             "WiFi AP  : %s\n"
             "BLE MAC  : %s\n"
             "eFuse ID : %04X%08lX",
             DEVICE_TYPE,
             firmwareVer,
             cores,
             (unsigned long)cpuMHz,
             (unsigned long)flash,
             (unsigned long)flashSpd,
             (unsigned long)heap,
             staMacStr,
             apMacStr,
             btMacStr,
             (uint16_t)(efuseMac >> 32),
             (unsigned long)(efuseMac & 0xFFFFFFFF));
#endif
    lv_obj_t *infoLbl = lv_label_create(card);
    lv_label_set_text(infoLbl, info);
    lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(infoLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(infoLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *backBtn = createBackBtn(miscToolScreen, cb_miscToolBack);
    deleteGroup(&miscToolGroup);
    miscToolGroup = lv_group_create();
    lv_group_add_obj(miscToolGroup, card);     // Focus card first so encoder can scroll Device Info.
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
//  Persistent v1.0.4 Pass 3 settings. Values start from config.h when no
//  saved NVS value exists, then save when changed from this page.
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

static bool isValidOptionValue(const int *options, int count, int value) {
    for (int i = 0; i < count; i++) {
        if (options[i] == value) return true;
    }
    return false;
}

static void loadPersistentScanSettings() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, true);

    int savedBleSecs = settingsPrefs.getInt("bleSec", BLE_SCAN_SECS);
    bleScanSeconds = isValidOptionValue(BLE_TIME_OPTIONS,
                                        sizeof(BLE_TIME_OPTIONS) / sizeof(BLE_TIME_OPTIONS[0]),
                                        savedBleSecs) ? savedBleSecs : BLE_SCAN_SECS;

    int savedWifiSecs = settingsPrefs.getInt("wifiSec", WIFI_SCAN_SECS);
    wifiScanSeconds = isValidOptionValue(WIFI_TIME_OPTIONS,
                                         sizeof(WIFI_TIME_OPTIONS) / sizeof(WIFI_TIME_OPTIONS[0]),
                                         savedWifiSecs) ? savedWifiSecs : WIFI_SCAN_SECS;

    int savedWifiResults = settingsPrefs.getInt("wifiMax", MAX_WIFI_RESULTS);
    wifiMaxResults = isValidOptionValue(WIFI_RESULT_OPTIONS,
                                        sizeof(WIFI_RESULT_OPTIONS) / sizeof(WIFI_RESULT_OPTIONS[0]),
                                        savedWifiResults) ? savedWifiResults : MAX_WIFI_RESULTS;
    if (wifiMaxResults > MAX_WIFI_RESULTS) wifiMaxResults = MAX_WIFI_RESULTS;

    int savedDeauthHop = settingsPrefs.getInt("deHop", DEAUTH_HOP_MS);
    deauthHopMs = isValidOptionValue(DEAUTH_HOP_OPTIONS,
                                     sizeof(DEAUTH_HOP_OPTIONS) / sizeof(DEAUTH_HOP_OPTIONS[0]),
                                     savedDeauthHop) ? savedDeauthHop : DEAUTH_HOP_MS;

    int savedFlockPreset = settingsPrefs.getUChar("flockPre", FLOCK_HYBRID_PRESET_DEFAULT);
    flockHybridPresetIdx = (savedFlockPreset >= 0 && savedFlockPreset < FLOCK_HYBRID_PRESET_COUNT)
                         ? savedFlockPreset
                         : FLOCK_HYBRID_PRESET_DEFAULT;
    applyFlockHybridPreset();

    packetMonitorHopEnabled = settingsPrefs.getBool("pktHopOn", (PACKET_MONITOR_HOP_ENABLED_DEFAULT != 0));

    int savedPacketHopMs = settingsPrefs.getInt("pktHopMs", PACKET_MONITOR_HOP_MS);
    packetMonitorHopMs = isValidOptionValue(PACKET_HOP_OPTIONS,
                                            sizeof(PACKET_HOP_OPTIONS) / sizeof(PACKET_HOP_OPTIONS[0]),
                                            savedPacketHopMs) ? savedPacketHopMs : PACKET_MONITOR_HOP_MS;
    packetMonitorLastHopMs = millis();

    settingsPrefs.end();

    Serial.println("[Rogue-Radar] NVS scan defaults loaded.");
#endif
}

static void savePersistentBleScanSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putInt("bleSec", bleScanSeconds);
    settingsPrefs.end();
#endif
}

static void savePersistentWifiScanSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putInt("wifiSec", wifiScanSeconds);
    settingsPrefs.end();
#endif
}

static void savePersistentWifiResultsSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putInt("wifiMax", wifiMaxResults);
    settingsPrefs.end();
#endif
}

static void savePersistentDeauthHopSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putInt("deHop", deauthHopMs);
    settingsPrefs.end();
#endif
}

static void savePersistentFlockHybridSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putUChar("flockPre", (uint8_t)flockHybridPresetIdx);
    settingsPrefs.end();
#endif
}

static void savePersistentPacketHopEnabledSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putBool("pktHopOn", packetMonitorHopEnabled);
    settingsPrefs.end();
#endif
}

static void savePersistentPacketHopMsSetting() {
#if PERSISTENT_SETTINGS_ENABLED
    settingsPrefs.begin(PREFS_NAMESPACE, false);
    settingsPrefs.putInt("pktHopMs", packetMonitorHopMs);
    settingsPrefs.end();
#endif
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
    savePersistentBleScanSetting();
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

static void cb_scanDefaultsWifiTime(lv_event_t *e) {
    wifiScanSeconds = nextOptionValue(WIFI_TIME_OPTIONS, sizeof(WIFI_TIME_OPTIONS) / sizeof(WIFI_TIME_OPTIONS[0]), wifiScanSeconds);
    savePersistentWifiScanSetting();
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

static void cb_scanDefaultsWifiResults(lv_event_t *e) {
    wifiMaxResults = nextOptionValue(WIFI_RESULT_OPTIONS, sizeof(WIFI_RESULT_OPTIONS) / sizeof(WIFI_RESULT_OPTIONS[0]), wifiMaxResults);
    if (wifiMaxResults > MAX_WIFI_RESULTS) wifiMaxResults = MAX_WIFI_RESULTS;
    savePersistentWifiResultsSetting();
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

static void cb_scanDefaultsDeauthHop(lv_event_t *e) {
    deauthHopMs = nextOptionValue(DEAUTH_HOP_OPTIONS, sizeof(DEAUTH_HOP_OPTIONS) / sizeof(DEAUTH_HOP_OPTIONS[0]), deauthHopMs);
    savePersistentDeauthHopSetting();
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

static void cb_scanDefaultsFlockHybrid(lv_event_t *e) {
    flockHybridPresetIdx = (flockHybridPresetIdx + 1) % FLOCK_HYBRID_PRESET_COUNT;
    applyFlockHybridPreset();
    savePersistentFlockHybridSetting();
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

static void cb_scanDefaultsPacketHopToggle(lv_event_t *e) {
    packetMonitorHopEnabled = !packetMonitorHopEnabled;
    packetMonitorLastHopMs = millis();
    savePersistentPacketHopEnabledSetting();
    resetInactivityTimer();
    updateScanDefaultsLabels();
}

static void cb_scanDefaultsPacketHopMs(lv_event_t *e) {
    packetMonitorHopMs = nextOptionValue(PACKET_HOP_OPTIONS, sizeof(PACKET_HOP_OPTIONS) / sizeof(PACKET_HOP_OPTIONS[0]), packetMonitorHopMs);
    packetMonitorLastHopMs = millis();
    savePersistentPacketHopMsSetting();
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
    savePersistentBrightnessSetting();
}

static void cb_brightUp(lv_event_t *e) {
    lcdBrightness += 13;
    if (lcdBrightness > 255) lcdBrightness = 255;
    applyBrightness();
    savePersistentBrightnessSetting();
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
//  MISC TOOL 3A – ALERT SOUND VOLUME
//
//  Controls detection alert chirp volume at runtime.
//  Menu feedback volume remains separate.
//  Level resets after reboot and starts from SOUND_VOLUME_PERCENT.
// ════════════════════════════════════════════════════════════════
static lv_obj_t *alertVolBar       = nullptr;
static lv_obj_t *alertVolPctLbl    = nullptr;
static lv_obj_t *alertVolHintLbl   = nullptr;
static lv_obj_t *alertVolDownBtn   = nullptr;
static lv_obj_t *alertVolUpBtn     = nullptr;
static lv_obj_t *alertVolBackBtn   = nullptr;
static unsigned long alertVolLastAdjustMs = 0;

static void applyAlertSoundVolume(bool previewTone = false) {
    resetInactivityTimer();

    if (alertSoundVolumePercent < SOUND_VOLUME_MIN_PERCENT) {
        alertSoundVolumePercent = SOUND_VOLUME_MIN_PERCENT;
    }
    if (alertSoundVolumePercent > SOUND_VOLUME_MAX_PERCENT) {
        alertSoundVolumePercent = SOUND_VOLUME_MAX_PERCENT;
    }

    if (alertVolBar) {
        lv_bar_set_value(alertVolBar, alertSoundVolumePercent, LV_ANIM_ON);
    }

    if (alertVolPctLbl) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d%%", alertSoundVolumePercent);
        lv_label_set_text(alertVolPctLbl, buf);
    }

    updateAlertVolumeMenuLabel();

    // Safety note:
    // Do NOT play a preview tone from this page. Earlier testing showed that
    // extra I2S activity during volume adjustment could make the T-Embed think
    // the power/encoder button was being held and trigger the power-off screen.
    (void)previewTone;
}

static void updateAlertVolumeHint(const char *msg) {
    if (!alertVolHintLbl) return;
    lv_label_set_text(alertVolHintLbl, msg);
}

static void armAlertVolumeAutoSet() {
    alertVolPendingAutoSet = true;
    alertVolLastAdjustMs = millis();
    updateAlertVolumeHint("Release to set...");
}

static void alertVolumeAutoSetTimerCb(lv_timer_t *timer) {
    (void)timer;
    if (!alertVolPendingAutoSet) return;

    unsigned long now = millis();
    if (now - alertVolLastAdjustMs < SOUND_VOLUME_AUTO_SET_MS) return;

    alertVolPendingAutoSet = false;
    savePersistentAlertVolumeSetting();
    updateAlertVolumeHint("Set. Back highlighted.");

    // Highlight/focus Back after the chosen value sits for a moment.
    if (miscToolGroup && alertVolBackBtn) {
        lv_group_focus_obj(alertVolBackBtn);
    }
}

static void cb_alertVolDown(lv_event_t *e) {
    (void)e;
    alertSoundVolumePercent -= SOUND_VOLUME_STEP_PERCENT;
    applyAlertSoundVolume(false);
    armAlertVolumeAutoSet();
}

static void cb_alertVolUp(lv_event_t *e) {
    (void)e;
    alertSoundVolumePercent += SOUND_VOLUME_STEP_PERCENT;
    applyAlertSoundVolume(false);
    armAlertVolumeAutoSet();
}

void createAlertSoundVolumeControl() {
    alertVolBar       = nullptr;
    alertVolPctLbl    = nullptr;
    alertVolHintLbl   = nullptr;
    alertVolDownBtn   = nullptr;
    alertVolUpBtn     = nullptr;
    alertVolBackBtn   = nullptr;
    alertVolPendingAutoSet = false;

    if (alertVolAutoTimer) {
        lv_timer_delete(alertVolAutoTimer);
        alertVolAutoTimer = nullptr;
    }

    if (miscToolScreen) { lv_obj_delete(miscToolScreen); miscToolScreen = nullptr; }
    miscToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(miscToolScreen);
    createHeader(miscToolScreen, LV_SYMBOL_AUDIO "  Alert Volume");

    // Current percentage label
    alertVolPctLbl = lv_label_create(miscToolScreen);
    lv_obj_set_style_text_color(alertVolPctLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(alertVolPctLbl, LV_ALIGN_CENTER, 0, -30);

    // Small status/instruction label
    alertVolHintLbl = lv_label_create(miscToolScreen);
    lv_label_set_text(alertVolHintLbl, "Use -/+ then wait to set");
    lv_obj_set_style_text_color(alertVolHintLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_align(alertVolHintLbl, LV_ALIGN_CENTER, 0, -14);

    // Detection alert volume bar
    alertVolBar = lv_bar_create(miscToolScreen);
    lv_obj_set_size(alertVolBar, SCREEN_W - 32, 16);
    lv_obj_align(alertVolBar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(alertVolBar, SOUND_VOLUME_MIN_PERCENT, SOUND_VOLUME_MAX_PERCENT);
    lv_obj_set_style_bg_color(alertVolBar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(alertVolBar, lv_color_hex(TH.accent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(alertVolBar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(alertVolBar, 4, LV_PART_INDICATOR);

    // – button
    alertVolDownBtn = lv_btn_create(miscToolScreen);
    lv_obj_set_size(alertVolDownBtn, 52, 30);
    lv_obj_align(alertVolDownBtn, LV_ALIGN_CENTER, -46, 36);
    lv_obj_set_style_bg_color(alertVolDownBtn, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_radius(alertVolDownBtn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(alertVolDownBtn, cb_alertVolDown, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *dLbl = lv_label_create(alertVolDownBtn);
    lv_label_set_text(dLbl, LV_SYMBOL_MINUS);
    lv_obj_center(dLbl);

    // + button
    alertVolUpBtn = lv_btn_create(miscToolScreen);
    lv_obj_set_size(alertVolUpBtn, 52, 30);
    lv_obj_align(alertVolUpBtn, LV_ALIGN_CENTER, 46, 36);
    lv_obj_set_style_bg_color(alertVolUpBtn, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_radius(alertVolUpBtn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(alertVolUpBtn, cb_alertVolUp, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *uLbl = lv_label_create(alertVolUpBtn);
    lv_label_set_text(uLbl, LV_SYMBOL_PLUS);
    lv_obj_center(uLbl);

    alertVolBackBtn = createBackBtn(miscToolScreen, cb_miscToolBack);
    deleteGroup(&miscToolGroup);
    miscToolGroup = lv_group_create();
    lv_group_add_obj(miscToolGroup, alertVolDownBtn);
    lv_group_add_obj(miscToolGroup, alertVolUpBtn);
    lv_group_add_obj(miscToolGroup, alertVolBackBtn);
    setGroup(miscToolGroup);
    lv_group_focus_obj(alertVolDownBtn);

    alertVolAutoTimer = lv_timer_create(alertVolumeAutoSetTimerCb, 100, nullptr);

    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b, LED_BRIGHTNESS);

    // Set bar and label to current value
    applyAlertSoundVolume(false);

    lv_screen_load_anim(miscToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  MISC TOOL 3B – MENU FEEDBACK VOLUME
//
//  Controls encoder/menu feedback volume at runtime.
//  Detection alert chirp volume still uses SOUND_VOLUME_PERCENT.
//  Level resets after reboot and starts from MENU_FEEDBACK_VOLUME_PERCENT.
// ════════════════════════════════════════════════════════════════
static lv_obj_t *menuVolBar       = nullptr;
static lv_obj_t *menuVolPctLbl    = nullptr;
static lv_obj_t *menuVolHintLbl   = nullptr;
static lv_obj_t *menuVolDownBtn   = nullptr;
static lv_obj_t *menuVolUpBtn     = nullptr;
static lv_obj_t *menuVolBackBtn   = nullptr;
static unsigned long menuVolLastAdjustMs = 0;

static void applyMenuFeedbackVolume(bool previewTone = false) {
    resetInactivityTimer();

    if (menuFeedbackVolumePercent < MENU_FEEDBACK_VOLUME_MIN_PERCENT) {
        menuFeedbackVolumePercent = MENU_FEEDBACK_VOLUME_MIN_PERCENT;
    }
    if (menuFeedbackVolumePercent > MENU_FEEDBACK_VOLUME_MAX_PERCENT) {
        menuFeedbackVolumePercent = MENU_FEEDBACK_VOLUME_MAX_PERCENT;
    }

    if (menuVolBar) {
        lv_bar_set_value(menuVolBar, menuFeedbackVolumePercent, LV_ANIM_ON);
    }

    if (menuVolPctLbl) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d%%", menuFeedbackVolumePercent);
        lv_label_set_text(menuVolPctLbl, buf);
    }

    // Safety note:
    // Do NOT play a preview tone from this page. Earlier testing showed that
    // extra I2S activity during volume adjustment could make the T-Embed think
    // the power/encoder button was being held and trigger the power-off screen.
    (void)previewTone;
}

static void updateMenuVolumeHint(const char *msg) {
    if (!menuVolHintLbl) return;
    lv_label_set_text(menuVolHintLbl, msg);
}

static void armMenuVolumeAutoSet() {
    menuVolPendingAutoSet = true;
    menuVolLastAdjustMs = millis();
    updateMenuVolumeHint("Release to set...");
}

static void menuVolumeAutoSetTimerCb(lv_timer_t *timer) {
    (void)timer;
    if (!menuVolPendingAutoSet) return;

    unsigned long now = millis();
    if (now - menuVolLastAdjustMs < MENU_FEEDBACK_VOLUME_AUTO_SET_MS) return;

    menuVolPendingAutoSet = false;
    savePersistentMenuVolumeSetting();
    updateMenuVolumeHint("Set. Back highlighted.");

    // Highlight/focus Back after the chosen value sits for a moment.
    if (miscToolGroup && menuVolBackBtn) {
        lv_group_focus_obj(menuVolBackBtn);
    }
}

static void cb_menuVolDown(lv_event_t *e) {
    (void)e;
    menuFeedbackVolumePercent -= MENU_FEEDBACK_VOLUME_STEP_PERCENT;
    applyMenuFeedbackVolume(false);
    armMenuVolumeAutoSet();
}

static void cb_menuVolUp(lv_event_t *e) {
    (void)e;
    menuFeedbackVolumePercent += MENU_FEEDBACK_VOLUME_STEP_PERCENT;
    applyMenuFeedbackVolume(false);
    armMenuVolumeAutoSet();
}

void createMenuFeedbackVolumeControl() {
    menuVolBar       = nullptr;
    menuVolPctLbl    = nullptr;
    menuVolHintLbl   = nullptr;
    menuVolDownBtn   = nullptr;
    menuVolUpBtn     = nullptr;
    menuVolBackBtn   = nullptr;
    menuVolPendingAutoSet = false;

    if (menuVolAutoTimer) {
        lv_timer_delete(menuVolAutoTimer);
        menuVolAutoTimer = nullptr;
    }

    if (miscToolScreen) { lv_obj_delete(miscToolScreen); miscToolScreen = nullptr; }
    miscToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(miscToolScreen);
    createHeader(miscToolScreen, LV_SYMBOL_AUDIO "  Menu Volume");

    // Current percentage label
    menuVolPctLbl = lv_label_create(miscToolScreen);
    lv_obj_set_style_text_color(menuVolPctLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(menuVolPctLbl, LV_ALIGN_CENTER, 0, -30);

    // Small status/instruction label
    menuVolHintLbl = lv_label_create(miscToolScreen);
    lv_label_set_text(menuVolHintLbl, "Use -/+ then wait to set");
    lv_obj_set_style_text_color(menuVolHintLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_align(menuVolHintLbl, LV_ALIGN_CENTER, 0, -14);

    // Menu feedback volume bar
    menuVolBar = lv_bar_create(miscToolScreen);
    lv_obj_set_size(menuVolBar, SCREEN_W - 32, 16);
    lv_obj_align(menuVolBar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(menuVolBar, MENU_FEEDBACK_VOLUME_MIN_PERCENT, MENU_FEEDBACK_VOLUME_MAX_PERCENT);
    lv_obj_set_style_bg_color(menuVolBar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(menuVolBar, lv_color_hex(TH.accent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(menuVolBar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(menuVolBar, 4, LV_PART_INDICATOR);

    // – button
    menuVolDownBtn = lv_btn_create(miscToolScreen);
    lv_obj_set_size(menuVolDownBtn, 52, 30);
    lv_obj_align(menuVolDownBtn, LV_ALIGN_CENTER, -46, 36);
    lv_obj_set_style_bg_color(menuVolDownBtn, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_radius(menuVolDownBtn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(menuVolDownBtn, cb_menuVolDown, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *dLbl = lv_label_create(menuVolDownBtn);
    lv_label_set_text(dLbl, LV_SYMBOL_MINUS);
    lv_obj_center(dLbl);

    // + button
    menuVolUpBtn = lv_btn_create(miscToolScreen);
    lv_obj_set_size(menuVolUpBtn, 52, 30);
    lv_obj_align(menuVolUpBtn, LV_ALIGN_CENTER, 46, 36);
    lv_obj_set_style_bg_color(menuVolUpBtn, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_radius(menuVolUpBtn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(menuVolUpBtn, cb_menuVolUp, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *uLbl = lv_label_create(menuVolUpBtn);
    lv_label_set_text(uLbl, LV_SYMBOL_PLUS);
    lv_obj_center(uLbl);

    menuVolBackBtn = createBackBtn(miscToolScreen, cb_miscToolBack);
    deleteGroup(&miscToolGroup);
    miscToolGroup = lv_group_create();
    lv_group_add_obj(miscToolGroup, menuVolDownBtn);
    lv_group_add_obj(miscToolGroup, menuVolUpBtn);
    lv_group_add_obj(miscToolGroup, menuVolBackBtn);
    setGroup(miscToolGroup);
    lv_group_focus_obj(menuVolDownBtn);

    menuVolAutoTimer = lv_timer_create(menuVolumeAutoSetTimerCb, 100, nullptr);

    setAllLEDs(MENU_COLORS[2].r, MENU_COLORS[2].g, MENU_COLORS[2].b, LED_BRIGHTNESS);

    // Set bar and label to current value
    applyMenuFeedbackVolume(false);

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
            savePersistentThemeSetting();
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
//  AUDIO TOOLS MENU + SOUND RECORDER
//
//  First-pass recorder for the T-Embed ES7210 microphone path.
//  This keeps audio work isolated from WiFi/BLE/LAN tools. The mic uses
//  I2S_NUM_1 for input, while the existing speaker chirp path uses I2S_NUM_0
//  for output.
// ════════════════════════════════════════════════════════════════
static const char *AUDIO_TOOL_LABELS[] = {
    LV_SYMBOL_AUDIO "  Sound Recorder"
};

static lv_obj_t *soundRecorderStatusLbl = nullptr;
static lv_obj_t *soundRecorderInfoLbl   = nullptr;
static lv_obj_t *soundRecorderMeterLbl  = nullptr;
static lv_obj_t *soundRecorderRecordBtn = nullptr;
static lv_obj_t *soundRecorderPlayBtn   = nullptr;
static lv_obj_t *soundRecorderRecordBtnLbl = nullptr;
static lv_obj_t *soundRecorderPlayBtnLbl   = nullptr;
static lv_obj_t *soundRecorderFilesInfoLbl = nullptr;
static lv_obj_t *soundRecorderFilesList    = nullptr;
static lv_obj_t *soundRecorderFileMenuScreen = nullptr;
static lv_obj_t *soundRecorderRecordPanel  = nullptr;

// One-shot rebuild helper used by the SD file action popup.
// Rebuilding from a timer keeps us from deleting/recreating the active
// Sound Recorder screen while LVGL is still inside a button/list callback.
static lv_timer_t *soundRecorderRebuildTimer = nullptr;

// SD recording browser state.
// File buttons store only the visible filename, while these arrays keep the
// full path used by the playback callback.
static char soundRecorderFilePaths[AUDIO_RECORD_SD_MAX_FILES][80];
static char soundRecorderFileNames[AUDIO_RECORD_SD_MAX_FILES][32];
static int  soundRecorderFileCount = 0;
static int  soundRecorderSelectedFile = -1;
static char soundRecorderSelectedPath[80] = {0};
static char soundRecorderSelectedName[32] = {0};

static int16_t *soundRecorderBuffer = nullptr;
static size_t   soundRecorderSamples = 0;
static size_t   soundRecorderCapacitySamples = 0;
static bool     soundRecorderInputReady = false;
// When recording is active, pressing the same Record button again requests
// a clean stop. The blocking I2S read loop checks this flag between chunks.
static volatile bool soundRecorderRecording = false;
static volatile bool soundRecorderStopRequested = false;

// When Stop is detected by polling the physical encoder button inside the
// blocking record loop, LVGL can still deliver one leftover click after the
// button is released. This guard ignores that stale click so recording does
// not immediately start again.
static volatile bool soundRecorderIgnoreNextRecordClick = false;
static volatile uint32_t soundRecorderIgnoreRecordClickUntilMs = 0;

static void soundRecorderRefreshFileList();
static bool soundRecorderPlayWavFile(const char *path, const char *name);
static bool soundRecorderDeleteSelectedFile();
static void cb_soundRecorderFileMenuBack(lv_event_t *e);
static void cb_soundRecorderFileMenuPlay(lv_event_t *e);
static void cb_soundRecorderFileMenuDelete(lv_event_t *e);
static void soundRecorderRebuildTimerCb(lv_timer_t *timer);
static void soundRecorderQueueRebuild(uint32_t delayMs = 30);

static void soundRecorderSetStatus(const char *text, uint32_t color) {
    if (!soundRecorderStatusLbl) return;
    lv_label_set_text(soundRecorderStatusLbl, text);
    lv_obj_set_style_text_color(soundRecorderStatusLbl, lv_color_hex(color), LV_PART_MAIN);
    lv_refr_now(lvDisp);
}

static void soundRecorderUpdateInfo(const char *extra = nullptr) {
    if (!soundRecorderInfoLbl) return;
    char buf[96];

    // Keep the left panel simple and short.
    // Status messages stay on the Status line above, while this area only
    // shows timing/playback info so it does not clip inside the compact box.
    uint32_t recordedMs = (AUDIO_RECORD_SAMPLE_RATE > 0)
                        ? (uint32_t)((soundRecorderSamples * 1000ULL) / AUDIO_RECORD_SAMPLE_RATE)
                        : 0;
    uint32_t capacityMs = (AUDIO_RECORD_SAMPLE_RATE > 0)
                        ? (uint32_t)((soundRecorderCapacitySamples * 1000ULL) / AUDIO_RECORD_SAMPLE_RATE)
                        : 0;
    if (capacityMs == 0) capacityMs = (uint32_t)AUDIO_RECORD_SECONDS * 1000UL;

    (void)extra; // Extra text is intentionally not shown here to keep the panel clean.
    snprintf(buf, sizeof(buf), "Time: %.1f / %.1fs\nPlay: %u%%",
             recordedMs / 1000.0f,
             capacityMs / 1000.0f,
             (unsigned)AUDIO_RECORD_PLAYBACK_SPEED_PERCENT);
    lv_label_set_text(soundRecorderInfoLbl, buf);
}

static void soundRecorderUpdateMeter(int32_t peak) {
    if (!soundRecorderMeterLbl) return;
    if (peak < 0) peak = -peak;
    int bars = peak / 2200;
    if (bars > 10) bars = 10;
    char line[28];
    int p = 0;
    p += snprintf(line + p, sizeof(line) - p, "Lvl:[");
    for (int i = 0; i < 10 && p < (int)sizeof(line)-2; i++) line[p++] = (i < bars) ? '#' : '-';
    snprintf(line + p, sizeof(line) - p, "]");
    lv_label_set_text(soundRecorderMeterLbl, line);
}

static void soundRecorderFreeBuffer() {
    if (soundRecorderBuffer) {
        free(soundRecorderBuffer);
        soundRecorderBuffer = nullptr;
    }
    soundRecorderSamples = 0;
    soundRecorderCapacitySamples = 0;
}

static bool soundRecorderAllocBuffer() {
    soundRecorderFreeBuffer();

    const size_t requestedSamples = (size_t)AUDIO_RECORD_SAMPLE_RATE * (size_t)AUDIO_RECORD_SECONDS;
    if (requestedSamples == 0) return false;

    const size_t requestedBytes = requestedSamples * sizeof(int16_t);
    size_t bytesToAlloc = requestedBytes;

    // Prefer PSRAM when available. The 5-second 16 kHz buffer is about 160 KB,
    // which can fail from normal heap fragmentation after WiFi/BLE/LVGL use.
    if (psramFound()) {
        soundRecorderBuffer = (int16_t *)heap_caps_malloc(bytesToAlloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (soundRecorderBuffer) {
            soundRecorderCapacitySamples = bytesToAlloc / sizeof(int16_t);
            memset(soundRecorderBuffer, 0, bytesToAlloc);
            return true;
        }
    }

    // Try normal 8-bit heap for the full requested buffer.
    soundRecorderBuffer = (int16_t *)heap_caps_malloc(bytesToAlloc, MALLOC_CAP_8BIT);
    if (!soundRecorderBuffer) {
#if AUDIO_RECORD_ALLOW_SHORT_BUFFER
        // Last-resort fallback: make the recorder usable even when there is not
        // enough contiguous RAM for the full requested clip length.
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        const size_t safetyBytes = AUDIO_RECORD_BUFFER_SAFETY_BYTES;  // Keep heap free for LVGL/buttons while recording.
        const size_t minBytes = (size_t)AUDIO_RECORD_SAMPLE_RATE * sizeof(int16_t); // 1 second minimum

        if (largest > (safetyBytes + minBytes)) {
            bytesToAlloc = largest - safetyBytes;
            if (bytesToAlloc > requestedBytes) bytesToAlloc = requestedBytes;
            bytesToAlloc &= ~(sizeof(int16_t) - 1); // sample-align
            soundRecorderBuffer = (int16_t *)heap_caps_malloc(bytesToAlloc, MALLOC_CAP_8BIT);
        }
#endif
    }

    if (!soundRecorderBuffer) {
        soundRecorderCapacitySamples = 0;
        return false;
    }

    soundRecorderCapacitySamples = bytesToAlloc / sizeof(int16_t);
    memset(soundRecorderBuffer, 0, bytesToAlloc);
    return true;
}


static void soundRecorderSetFilesText(const char *text) {
    if (!soundRecorderFilesInfoLbl) return;
    lv_label_set_text(soundRecorderFilesInfoLbl, text ? text : "");
    lv_refr_now(lvDisp);
}

static void soundRecorderSelectFile(int idx) {
    if (idx < 0 || idx >= soundRecorderFileCount) return;
    soundRecorderSelectedFile = idx;
    strncpy(soundRecorderSelectedPath, soundRecorderFilePaths[idx], sizeof(soundRecorderSelectedPath) - 1);
    soundRecorderSelectedPath[sizeof(soundRecorderSelectedPath) - 1] = '\0';
    strncpy(soundRecorderSelectedName, soundRecorderFileNames[idx], sizeof(soundRecorderSelectedName) - 1);
    soundRecorderSelectedName[sizeof(soundRecorderSelectedName) - 1] = '\0';

    char status[64];
    snprintf(status, sizeof(status), LV_SYMBOL_PLAY "  %s", soundRecorderSelectedName);
    soundRecorderSetStatus(status, TH.warn);
}

static void cb_soundRecorderFileClicked(lv_event_t *e) {
    resetInactivityTimer();
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    soundRecorderSelectFile(idx);

    // Stable file actions:
    // Earlier popup-based builds could reboot on some ESP32-S3/LVGL setups
    // when Back/Delete changed focus or rebuilt UI objects from inside the
    // popup callback. Keep actions on the existing Sound Recorder page instead:
    // select a file, then use Play to play it or Delete to remove it.
    if (soundRecorderRecordBtnLbl) lv_label_set_text(soundRecorderRecordBtnLbl, "Delete");
    if (soundRecorderPlayBtnLbl)   lv_label_set_text(soundRecorderPlayBtnLbl, LV_SYMBOL_PLAY "  Play");
    soundRecorderSetFilesText("Selected. Play or Delete.");
}

static bool soundRecorderEnsureSD(char *diag, size_t diagLen) {
#if AUDIO_RECORD_SD_SAVE_ENABLED
    if (diag && diagLen) snprintf(diag, diagLen, "Mounting SD...");
    if (!SD.begin(SD_CS, sdSPI)) {
        if (diag && diagLen) snprintf(diag, diagLen, "SD mount failed");
        return false;
    }
    if (diag && diagLen) snprintf(diag, diagLen, "SD mounted");
    return true;
#else
    if (diag && diagLen) snprintf(diag, diagLen, "SD save disabled");
    return false;
#endif
}

static void soundRecorderWriteLE16(File &f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF) };
    f.write(b, 2);
}

static void soundRecorderWriteLE32(File &f, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v & 0xFF),
        (uint8_t)((v >> 8) & 0xFF),
        (uint8_t)((v >> 16) & 0xFF),
        (uint8_t)((v >> 24) & 0xFF)
    };
    f.write(b, 4);
}

static bool soundRecorderWriteWavFile(const char *path, char *diag, size_t diagLen) {
#if AUDIO_RECORD_SD_SAVE_ENABLED
    if (!path || !soundRecorderBuffer || soundRecorderSamples == 0) {
        if (diag && diagLen) snprintf(diag, diagLen, "No audio data");
        return false;
    }

    if (diag && diagLen) snprintf(diag, diagLen, "Opening %s", path);
    soundRecorderSetFilesText(diag);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        if (diag && diagLen) snprintf(diag, diagLen, "Open failed:\n%s", path);
        return false;
    }

    const uint32_t dataBytes = (uint32_t)(soundRecorderSamples * sizeof(int16_t));
    const uint32_t byteRate = (uint32_t)AUDIO_RECORD_SAMPLE_RATE * 1U * 16U / 8U;
    const uint16_t blockAlign = 1U * 16U / 8U;

    f.write((const uint8_t *)"RIFF", 4);
    soundRecorderWriteLE32(f, 36U + dataBytes);
    f.write((const uint8_t *)"WAVE", 4);
    f.write((const uint8_t *)"fmt ", 4);
    soundRecorderWriteLE32(f, 16);       // PCM fmt chunk size
    soundRecorderWriteLE16(f, 1);        // PCM
    soundRecorderWriteLE16(f, 1);        // mono
    soundRecorderWriteLE32(f, AUDIO_RECORD_SAMPLE_RATE);
    soundRecorderWriteLE32(f, byteRate);
    soundRecorderWriteLE16(f, blockAlign);
    soundRecorderWriteLE16(f, 16);       // 16-bit
    f.write((const uint8_t *)"data", 4);
    soundRecorderWriteLE32(f, dataBytes);

    size_t bytesWritten = f.write((const uint8_t *)soundRecorderBuffer, dataBytes);
    f.flush();
    size_t finalSize = f.size();
    f.close();

    const size_t expectedSize = (size_t)dataBytes + 44U;
    if (bytesWritten != dataBytes || finalSize < expectedSize) {
        if (diag && diagLen) {
            snprintf(diag, diagLen,
                     "Write failed:\n%s\nWrote %u/%u\nSize %u/%u",
                     path,
                     (unsigned)bytesWritten,
                     (unsigned)dataBytes,
                     (unsigned)finalSize,
                     (unsigned)expectedSize);
        }
        return false;
    }

    if (diag && diagLen) {
        snprintf(diag, diagLen, "Saved:\n%s\n%u bytes", path, (unsigned)finalSize);
    }
    return true;
#else
    (void)path; (void)diag; (void)diagLen;
    return false;
#endif
}

static bool soundRecorderMakeFolder(char *diag, size_t diagLen) {
#if AUDIO_RECORD_SD_SAVE_ENABLED
    if (SD.exists(AUDIO_RECORD_SD_FOLDER)) {
        if (diag && diagLen) snprintf(diag, diagLen, "Folder OK:\n%s", AUDIO_RECORD_SD_FOLDER);
        return true;
    }
    if (diag && diagLen) snprintf(diag, diagLen, "Creating folder:\n%s", AUDIO_RECORD_SD_FOLDER);
    soundRecorderSetFilesText(diag);
    if (!SD.mkdir(AUDIO_RECORD_SD_FOLDER)) {
        if (diag && diagLen) snprintf(diag, diagLen, "Folder create failed:\n%s", AUDIO_RECORD_SD_FOLDER);
        return false;
    }
    if (diag && diagLen) snprintf(diag, diagLen, "Folder created:\n%s", AUDIO_RECORD_SD_FOLDER);
    return true;
#else
    (void)diag; (void)diagLen;
    return false;
#endif
}

static bool soundRecorderNextPath(char *path, size_t pathLen, bool inFolder) {
#if AUDIO_RECORD_SD_SAVE_ENABLED
    if (!path || pathLen == 0) return false;
    for (int i = 1; i <= 9999; i++) {
        if (inFolder) {
            snprintf(path, pathLen, "%s/%s%04d.WAV", AUDIO_RECORD_SD_FOLDER, AUDIO_RECORD_SD_PREFIX, i);
        } else {
            snprintf(path, pathLen, "/%s%04d.WAV", AUDIO_RECORD_SD_PREFIX, i);
        }
        if (!SD.exists(path)) return true;
    }
#endif
    return false;
}

static bool soundRecorderSaveToSD() {
#if AUDIO_RECORD_SD_SAVE_ENABLED
    char diag[192];
    char path[80];

    if (!soundRecorderEnsureSD(diag, sizeof(diag))) {
        soundRecorderSetFilesText(diag);
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  SD mount failed", TH.alert);
        return false;
    }
    soundRecorderSetFilesText(diag);

    bool folderReady = soundRecorderMakeFolder(diag, sizeof(diag));
    soundRecorderSetFilesText(diag);

    if (folderReady && soundRecorderNextPath(path, sizeof(path), true)) {
        if (soundRecorderWriteWavFile(path, diag, sizeof(diag))) {
            soundRecorderSetFilesText(diag);
            soundRecorderSetStatus(LV_SYMBOL_OK "  Saved SD", TH.success);
            return true;
        }
        soundRecorderSetFilesText(diag);
#if AUDIO_RECORD_SD_ROOT_FALLBACK
        delay(250);
#endif
    }

#if AUDIO_RECORD_SD_ROOT_FALLBACK
    if (soundRecorderNextPath(path, sizeof(path), false)) {
        snprintf(diag, sizeof(diag), "Trying root fallback...");
        soundRecorderSetFilesText(diag);
        if (soundRecorderWriteWavFile(path, diag, sizeof(diag))) {
            soundRecorderSetFilesText(diag);
            soundRecorderSetStatus(LV_SYMBOL_OK "  Saved root", TH.success);
            return true;
        }
        soundRecorderSetFilesText(diag);
    }
#endif

    soundRecorderSetStatus(LV_SYMBOL_WARNING "  SD write failed", TH.alert);
    return false;
#else
    return false;
#endif
}

static void soundRecorderRefreshFileList() {
#if AUDIO_RECORD_SD_SAVE_ENABLED
    soundRecorderFileCount = 0;

    if (soundRecorderFilesList) {
        lv_obj_clean(soundRecorderFilesList);
    }

    char diag[80];
    if (!soundRecorderEnsureSD(diag, sizeof(diag))) {
        soundRecorderSetFilesText("SD not mounted");
        return;
    }

    File dir = SD.open(AUDIO_RECORD_SD_FOLDER);
    if (dir && dir.isDirectory()) {
        File entry = dir.openNextFile();
        while (entry && soundRecorderFileCount < AUDIO_RECORD_SD_MAX_FILES) {
            if (!entry.isDirectory()) {
                const char *fullName = entry.name();
                const char *name = strrchr(fullName, '/');
                name = name ? name + 1 : fullName;

                // Only show WAV files in the browser.
                String upper = String(name);
                upper.toUpperCase();
                if (upper.endsWith(".WAV")) {
                    snprintf(soundRecorderFileNames[soundRecorderFileCount],
                             sizeof(soundRecorderFileNames[soundRecorderFileCount]),
                             "%s", name);
                    snprintf(soundRecorderFilePaths[soundRecorderFileCount],
                             sizeof(soundRecorderFilePaths[soundRecorderFileCount]),
                             "%s/%s", AUDIO_RECORD_SD_FOLDER, name);

                    if (soundRecorderFilesList) {
                        lv_obj_t *btn = lv_list_add_btn(soundRecorderFilesList, nullptr,
                                                        soundRecorderFileNames[soundRecorderFileCount]);
                        styleListBtn(btn);
                        lv_obj_set_height(btn, 24);
                        lv_obj_add_event_cb(btn, cb_soundRecorderFileClicked, LV_EVENT_CLICKED,
                                            (void *)(intptr_t)soundRecorderFileCount);

                        // Add file buttons to the encoder group as selectable items.
                        // They are added before the bottom controls during initial page build.
                        if (audioToolGroup) lv_group_add_obj(audioToolGroup, btn);
                    }

                    soundRecorderFileCount++;
                }
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();
    }

    if (soundRecorderFileCount == 0) {
        soundRecorderSelectedFile = -1;
        soundRecorderSelectedPath[0] = '\0';
        soundRecorderSelectedName[0] = '\0';
        if (soundRecorderRecordBtnLbl) lv_label_set_text(soundRecorderRecordBtnLbl, LV_SYMBOL_AUDIO "  Record");
        if (soundRecorderPlayBtnLbl)   lv_label_set_text(soundRecorderPlayBtnLbl, LV_SYMBOL_PLAY "  Play");
        soundRecorderSetFilesText("No WAV files yet");
    } else {
        soundRecorderSetFilesText("");
    }
#else
    soundRecorderSetFilesText("SD save disabled");
#endif
}


// ─── Sound Recorder selected-file action popup ──────────────────
// NOTE: Earlier builds opened a separate LVGL screen from inside the list
// click callback. On some ESP32-S3/LVGL builds, deleting/loading screens
// while the focused list button is still processing can cause a reboot.
// This version uses an in-place popup and defers any page rebuild to a
// one-shot timer after the event callback has returned.
static bool soundRecorderDeleteSelectedFile() {
#if AUDIO_RECORD_SD_SAVE_ENABLED
    if (!soundRecorderSelectedPath[0]) {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  No file selected", TH.warn);
        return false;
    }

    char diag[80];
    if (!soundRecorderEnsureSD(diag, sizeof(diag))) {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  SD mount failed", TH.alert);
        return false;
    }

    if (!SD.exists(soundRecorderSelectedPath)) {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  File missing", TH.alert);
        return false;
    }

    if (!SD.remove(soundRecorderSelectedPath)) {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  Delete failed", TH.alert);
        return false;
    }

    soundRecorderSetStatus(LV_SYMBOL_OK "  Deleted", TH.success);
    soundRecorderSelectedFile = -1;
    soundRecorderSelectedPath[0] = '\0';
    soundRecorderSelectedName[0] = '\0';
    return true;
#else
    soundRecorderSetStatus(LV_SYMBOL_WARNING "  SD save disabled", TH.warn);
    return false;
#endif
}

static void soundRecorderRebuildTimerCb(lv_timer_t *timer) {
    if (timer) lv_timer_delete(timer);
    soundRecorderRebuildTimer = nullptr;
    createSoundRecorder();
}

static void soundRecorderQueueRebuild(uint32_t delayMs) {
    if (soundRecorderRebuildTimer) {
        lv_timer_delete(soundRecorderRebuildTimer);
        soundRecorderRebuildTimer = nullptr;
    }
    if (delayMs < 10) delayMs = 10;
    soundRecorderRebuildTimer = lv_timer_create(soundRecorderRebuildTimerCb, delayMs, nullptr);
}

static void cb_soundRecorderFileMenuBack(lv_event_t *e) {
    (void)e;
    resetInactivityTimer();

    // Close the popup by rebuilding after LVGL finishes this button event.
    soundRecorderQueueRebuild(30);
}

static void cb_soundRecorderFileMenuPlay(lv_event_t *e) {
    (void)e;
    resetInactivityTimer();

    // Keep the popup visible while playing so the selected filename remains clear.
    if (soundRecorderSelectedPath[0]) {
        soundRecorderPlayWavFile(soundRecorderSelectedPath, soundRecorderSelectedName);
    } else {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  No file selected", TH.warn);
    }
}

static void cb_soundRecorderFileMenuDelete(lv_event_t *e) {
    (void)e;
    resetInactivityTimer();

    // Delete from SD, then rebuild after the callback returns so the list refreshes
    // without deleting the active screen during the current LVGL event.
    if (soundRecorderDeleteSelectedFile()) {
        soundRecorderQueueRebuild(250);
    }
}

static lv_obj_t *soundRecorderMakePopupButton(lv_obj_t *parent,
                                              int x,
                                              int y,
                                              int w,
                                              int h,
                                              const char *label,
                                              lv_event_cb_t cb,
                                              bool danger = false) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, danger ? TC(stopRed) : TC(actionBg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, danger ? TC(alert)   : TC(actionFoc), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, TC(btnPress), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, danger ? TC(alert) : TC(actionBdr), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, TC(text), LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

static void createSoundRecorderFileMenu() {
    if (!soundRecorderSelectedName[0] || !soundRecorderSelectedPath[0]) {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  No file selected", TH.warn);
        return;
    }

    if (!audioToolScreen) return;

    // Remove any stale popup object only. Do not delete/load the whole screen here.
    if (soundRecorderFileMenuScreen) {
        lv_obj_delete(soundRecorderFileMenuScreen);
        soundRecorderFileMenuScreen = nullptr;
    }

    // Full-screen dimmed popup container over the existing Sound Recorder page.
    soundRecorderFileMenuScreen = lv_obj_create(audioToolScreen);
    lv_obj_remove_style_all(soundRecorderFileMenuScreen);
    lv_obj_set_size(soundRecorderFileMenuScreen, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(soundRecorderFileMenuScreen, 0, 0);
    lv_obj_set_style_bg_color(soundRecorderFileMenuScreen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(soundRecorderFileMenuScreen, LV_OPA_70, LV_PART_MAIN);
    lv_obj_add_flag(soundRecorderFileMenuScreen, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *card = lv_obj_create(soundRecorderFileMenuScreen);
    lv_obj_set_size(card, SCREEN_W - 16, SCREEN_H - 18);
    lv_obj_set_pos(card, 8, 9);
    lv_obj_set_style_bg_color(card,     lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,       LV_OPA_COVER,          LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,       8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,      8, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_AUDIO "  Recording");
    lv_obj_set_style_text_color(title, lv_color_hex(TH.accent), LV_PART_MAIN);
    lv_obj_set_pos(title, 0, 0);

    lv_obj_t *nameLbl = lv_label_create(card);
    lv_label_set_text(nameLbl, soundRecorderSelectedName);
    lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(nameLbl, SCREEN_W - 40);
    lv_obj_set_style_text_color(nameLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_set_pos(nameLbl, 0, 28);

    lv_obj_t *hintLbl = lv_label_create(card);
    lv_label_set_text(hintLbl, "Choose an action for this saved WAV.");
    lv_label_set_long_mode(hintLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hintLbl, SCREEN_W - 40);
    lv_obj_set_style_text_color(hintLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(hintLbl, 0, 52);

    const int btnY = SCREEN_H - 43;
    const int btnW = 88;
    const int btnH = 27;

    lv_obj_t *backBtn = soundRecorderMakePopupButton(soundRecorderFileMenuScreen, 13,  btnY, btnW, btnH,
                                                     LV_SYMBOL_LEFT "  Back",
                                                     cb_soundRecorderFileMenuBack, false);
    lv_obj_t *playBtn = soundRecorderMakePopupButton(soundRecorderFileMenuScreen, 116, btnY, btnW, btnH,
                                                     LV_SYMBOL_PLAY "  Play",
                                                     cb_soundRecorderFileMenuPlay, false);
    lv_obj_t *deleteBtn = soundRecorderMakePopupButton(soundRecorderFileMenuScreen, 219, btnY, btnW, btnH,
                                                       "Delete",
                                                       cb_soundRecorderFileMenuDelete, true);

    deleteGroup(&audioToolGroup);
    audioToolGroup = lv_group_create();
    lv_group_add_obj(audioToolGroup, backBtn);
    lv_group_add_obj(audioToolGroup, playBtn);
    lv_group_add_obj(audioToolGroup, deleteBtn);
    setGroup(audioToolGroup);
    lv_group_focus_obj(playBtn);

    setAllLEDs(MENU_COLORS[3].r, MENU_COLORS[3].g, MENU_COLORS[3].b, LED_BRIGHTNESS);
    lv_obj_move_foreground(soundRecorderFileMenuScreen);
    lv_refr_now(lvDisp);
}

// ─── WAV playback from SD browser ────────────────────────────────
static uint16_t soundRecorderReadLE16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t soundRecorderReadLE32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static bool soundRecorderPlayWavFile(const char *path, const char *name) {
#if AUDIO_RECORD_SD_SAVE_ENABLED
    if (!path || !path[0]) {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  No file selected", TH.warn);
        return false;
    }

    char diag[96];
    if (!soundRecorderEnsureSD(diag, sizeof(diag))) {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  SD mount failed", TH.alert);
        soundRecorderSetFilesText(diag);
        return false;
    }

    File f = SD.open(path, FILE_READ);
    if (!f) {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  File open failed", TH.alert);
        return false;
    }

    uint8_t header[44];
    if (f.read(header, sizeof(header)) != sizeof(header)) {
        f.close();
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  Bad WAV", TH.alert);
        return false;
    }

    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        f.close();
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  Not WAV", TH.alert);
        return false;
    }

    uint16_t channels = soundRecorderReadLE16(header + 22);
    uint32_t sampleRate = soundRecorderReadLE32(header + 24);
    uint16_t bitsPerSample = soundRecorderReadLE16(header + 34);
    uint32_t dataBytes = soundRecorderReadLE32(header + 40);

    if (channels != 1 || bitsPerSample != 16 || sampleRate == 0) {
        f.close();
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  WAV format unsupported", TH.alert);
        return false;
    }

    if (soundReady) stopSoundDriverAfterChirp();
    if (!ensureSoundReady(false)) {
        f.close();
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  Speaker init failed", TH.alert);
        return false;
    }

    uint32_t playbackRate = ((uint32_t)sampleRate * (uint32_t)AUDIO_RECORD_PLAYBACK_SPEED_PERCENT) / 100U;
    if (playbackRate < 8000U) playbackRate = 8000U;
    if (playbackRate > 48000U) playbackRate = 48000U;
    i2s_set_clk(I2S_NUM_0, playbackRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

    if (soundRecorderPlayBtnLbl) lv_label_set_text(soundRecorderPlayBtnLbl, LV_SYMBOL_STOP "  Playing");
    char status[80];
    snprintf(status, sizeof(status), LV_SYMBOL_PLAY "  %s", name ? name : "Playing file");
    soundRecorderSetStatus(status, TH.warn);

    const size_t framesPerChunk = 128;
    int16_t mono[framesPerChunk];
    int16_t stereo[framesPerChunk * 2];
    uint8_t volumePct = AUDIO_RECORD_PLAYBACK_VOLUME_PERCENT;
    if (volumePct > 100) volumePct = 100;

    uint32_t remaining = dataBytes;
    while (remaining > 0) {
        size_t want = sizeof(mono);
        if (want > remaining) want = remaining;
        size_t got = f.read((uint8_t *)mono, want);
        if (got == 0) break;
        remaining -= got;

        size_t frames = got / sizeof(int16_t);
        for (size_t i = 0; i < frames; i++) {
            int32_t s = mono[i];
            s = (s * volumePct) / 100;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            stereo[i * 2]     = (int16_t)s;
            stereo[i * 2 + 1] = (int16_t)s;
        }

        size_t bytesWritten = 0;
        i2s_write(I2S_NUM_0, stereo, frames * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
        lv_timer_handler();
    }

    f.close();
    stopSoundDriverAfterChirp();
    if (soundRecorderPlayBtnLbl) lv_label_set_text(soundRecorderPlayBtnLbl, LV_SYMBOL_PLAY "  Play");
    soundRecorderSetStatus(LV_SYMBOL_OK "  File done", TH.success);
    pinMode(ENCODER_BTN, INPUT_PULLUP);
    return true;
#else
    (void)path; (void)name;
    soundRecorderSetStatus(LV_SYMBOL_WARNING "  SD save disabled", TH.warn);
    return false;
#endif
}

// ─── Minimal ES7210 ADC init for the T-Embed microphone ───────────────
// Based on LilyGO's T-Embed mic example, but kept self-contained so the
// Rogue Radar sketch does not need extra ES7210 library files in the folder.
static bool soundRecorderEs7210Write(uint8_t reg, uint8_t value) {
#if AUDIO_RECORDER_USE_ES7210_I2C
    Wire.beginTransmission(AUDIO_RECORDER_ES7210_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return (Wire.endTransmission() == 0);
#else
    (void)reg; (void)value;
    return true;
#endif
}

static int soundRecorderEs7210Read(uint8_t reg) {
#if AUDIO_RECORDER_USE_ES7210_I2C
    Wire.beginTransmission(AUDIO_RECORDER_ES7210_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((uint8_t)AUDIO_RECORDER_ES7210_ADDR, (uint8_t)1) != 1) return -1;
    return Wire.read();
#else
    (void)reg;
    return 0;
#endif
}

static bool soundRecorderEs7210Update(uint8_t reg, uint8_t mask, uint8_t value) {
    int current = soundRecorderEs7210Read(reg);
    if (current < 0) return false;
    uint8_t next = ((uint8_t)current & ~mask) | (value & mask);
    return soundRecorderEs7210Write(reg, next);
}

static bool soundRecorderEs7210Probe() {
#if AUDIO_RECORDER_USE_ES7210_I2C
    Wire.begin(AUDIO_RECORDER_I2C_SDA, AUDIO_RECORDER_I2C_SCL);
    Wire.beginTransmission(AUDIO_RECORDER_ES7210_ADDR);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        Serial.printf("[Audio] ES7210 probe miss SDA=%d SCL=%d addr=0x%02X err=%u\n",
                      AUDIO_RECORDER_I2C_SDA,
                      AUDIO_RECORDER_I2C_SCL,
                      AUDIO_RECORDER_ES7210_ADDR,
                      err);
        return false;
    }
    return true;
#else
    return true;
#endif
}

static bool soundRecorderEs7210SelectAllMics() {
    bool ok = true;
    // Clear mic enable bits, power both mic groups, then enable all four ADC paths.
    for (uint8_t r = 0x43; r <= 0x46; r++) ok &= soundRecorderEs7210Update(r, 0x10, 0x00);
    ok &= soundRecorderEs7210Write(0x4B, 0xFF);
    ok &= soundRecorderEs7210Write(0x4C, 0xFF);

    // MIC1 + MIC2 group
    ok &= soundRecorderEs7210Update(0x01, 0x0B, 0x00);
    ok &= soundRecorderEs7210Write(0x4B, 0x00);
    ok &= soundRecorderEs7210Update(0x43, 0x10, 0x10);
    ok &= soundRecorderEs7210Update(0x44, 0x10, 0x10);

    // MIC3 + MIC4 group
    ok &= soundRecorderEs7210Update(0x01, 0x15, 0x00);
    ok &= soundRecorderEs7210Write(0x4C, 0x00);
    ok &= soundRecorderEs7210Update(0x45, 0x10, 0x10);
    ok &= soundRecorderEs7210Update(0x46, 0x10, 0x10);
    return ok;
}

static bool soundRecorderEs7210SetGain(uint8_t reg, uint8_t gain) {
    if (gain > 14) gain = 14;
    return soundRecorderEs7210Update(reg, 0x0F, gain);
}

static bool soundRecorderInitEs7210() {
#if AUDIO_RECORDER_USE_ES7210_I2C
    if (!soundRecorderEs7210Probe()) return false;

    bool ok = true;
    Serial.printf("[Audio] ES7210 found at 0x%02X on SDA=%d SCL=%d\n",
                  AUDIO_RECORDER_ES7210_ADDR,
                  AUDIO_RECORDER_I2C_SDA,
                  AUDIO_RECORDER_I2C_SCL);

    // Reset and basic timing/power setup from LilyGO's official mic example.
    ok &= soundRecorderEs7210Write(0x00, 0xFF); // reset
    delay(2);
    ok &= soundRecorderEs7210Write(0x00, 0x41);
    ok &= soundRecorderEs7210Write(0x01, 0x1F); // clocks off during setup
    ok &= soundRecorderEs7210Write(0x09, 0x30);
    ok &= soundRecorderEs7210Write(0x0A, 0x30);

    // Slave mode, analog power/bias, and 16 kHz / 16-bit normal I2S.
    ok &= soundRecorderEs7210Write(0x40, 0xC3);
    ok &= soundRecorderEs7210Write(0x41, 0x70);
    ok &= soundRecorderEs7210Write(0x42, 0x70);
    ok &= soundRecorderEs7210Write(0x02, 0xC1); // 16 kHz coefficient using 4.096 MHz MCLK
    ok &= soundRecorderEs7210Write(0x07, 0x20);
    ok &= soundRecorderEs7210Write(0x04, 0x01);
    ok &= soundRecorderEs7210Write(0x05, 0x00);
    ok &= soundRecorderEs7210Write(0x11, 0x60); // 16-bit, normal I2S
    ok &= soundRecorderEs7210Write(0x12, 0x00); // ADC1/2 to SDOUT1, ADC3/4 to SDOUT2

    ok &= soundRecorderEs7210SelectAllMics();

    // LilyGO's example uses 0dB on MIC1/2 and 37.5dB on MIC3/4.
    ok &= soundRecorderEs7210SetGain(0x43, AUDIO_RECORDER_ES7210_GAIN_MIC12);
    ok &= soundRecorderEs7210SetGain(0x44, AUDIO_RECORDER_ES7210_GAIN_MIC12);
    ok &= soundRecorderEs7210SetGain(0x45, AUDIO_RECORDER_ES7210_GAIN_MIC34);
    ok &= soundRecorderEs7210SetGain(0x46, AUDIO_RECORDER_ES7210_GAIN_MIC34);

    // Start ADC clocks and power up all mic inputs.
    ok &= soundRecorderEs7210Write(0x01, 0x00);
    ok &= soundRecorderEs7210Write(0x06, 0x00);
    ok &= soundRecorderEs7210Write(0x47, 0x00);
    ok &= soundRecorderEs7210Write(0x48, 0x00);
    ok &= soundRecorderEs7210Write(0x49, 0x00);
    ok &= soundRecorderEs7210Write(0x4A, 0x00);
    ok &= soundRecorderEs7210SelectAllMics();

    Serial.printf("[Audio] ES7210 init %s\n", ok ? "OK" : "had I2C write misses");
    return ok;
#else
    return true;
#endif
}

static void soundRecorderStopInput() {
    if (!soundRecorderInputReady) return;
    i2s_zero_dma_buffer(I2S_NUM_1);
    i2s_driver_uninstall(I2S_NUM_1);
    soundRecorderInputReady = false;
}

static bool soundRecorderInitInput() {
    soundRecorderStopInput();

    // The ES7210 must be configured over I2C before the I2S data line
    // carries real microphone audio. LilyGO's official T-Embed mic example
    // uses SDA=18/SCL=8, then starts I2S RX on the mic pins below.
    bool es7210Ready = soundRecorderInitEs7210();
    if (!es7210Ready) {
#if AUDIO_RECORDER_REQUIRE_ES7210_I2C
        Serial.println("[Audio] ES7210 init failed; recording stopped");
        return false;
#else
        Serial.println("[Audio] ES7210 init failed; trying raw I2S RX anyway");
#endif
    }

    Serial.printf("[Audio] I2S mic pins BCLK=%d LRCK=%d DIN=%d MCLK=%d\n",
                  AUDIO_RECORDER_MIC_BCLK,
                  AUDIO_RECORDER_MIC_LRCK,
                  AUDIO_RECORDER_MIC_DIN,
                  AUDIO_RECORDER_MIC_MCLK);

    i2s_config_t i2sConfig = {};
    i2sConfig.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    i2sConfig.sample_rate = AUDIO_RECORD_SAMPLE_RATE;
    i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2sConfig.channel_format = I2S_CHANNEL_FMT_ALL_LEFT;
    i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2sConfig.intr_alloc_flags = 0;
    i2sConfig.dma_buf_count = 4;
    i2sConfig.dma_buf_len = 256;
    i2sConfig.use_apll = false;
    i2sConfig.tx_desc_auto_clear = false;
    i2sConfig.fixed_mclk = 0;
#if defined(I2S_MCLK_MULTIPLE_256)
    i2sConfig.mclk_multiple = I2S_MCLK_MULTIPLE_256;
#endif
#if defined(I2S_BITS_PER_CHAN_16BIT)
    i2sConfig.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
#endif
#if defined(I2S_TDM_ACTIVE_CH0) && defined(I2S_TDM_ACTIVE_CH1)
    i2sConfig.chan_mask = (i2s_channel_t)(I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1);
#endif

    if (i2s_driver_install(I2S_NUM_1, &i2sConfig, 0, nullptr) != ESP_OK) {
        Serial.println("[Audio] I2S RX driver install failed");
        return false;
    }

    i2s_pin_config_t pinConfig = {};
#if defined(SOC_I2S_SUPPORTS_MCLK) || defined(CONFIG_IDF_TARGET_ESP32S3)
    pinConfig.mck_io_num = AUDIO_RECORDER_MIC_MCLK;
#endif
    pinConfig.bck_io_num = AUDIO_RECORDER_MIC_BCLK;
    pinConfig.ws_io_num = AUDIO_RECORDER_MIC_LRCK;
    pinConfig.data_out_num = I2S_PIN_NO_CHANGE;
    pinConfig.data_in_num = AUDIO_RECORDER_MIC_DIN;

    if (i2s_set_pin(I2S_NUM_1, &pinConfig) != ESP_OK) {
        Serial.println("[Audio] I2S RX pin setup failed");
        i2s_driver_uninstall(I2S_NUM_1);
        return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_1);
    soundRecorderInputReady = true;
    return true;
}

static void cb_soundRecorderRecord(lv_event_t *) {
    resetInactivityTimer();

    // If a saved WAV is selected, the center button becomes Delete.
    // This avoids the popup delete path that was causing reboots on hardware.
    if (!soundRecorderRecording && soundRecorderSelectedPath[0]) {
        if (soundRecorderDeleteSelectedFile()) {
            soundRecorderRefreshFileList();
            if (soundRecorderRecordBtnLbl) lv_label_set_text(soundRecorderRecordBtnLbl, LV_SYMBOL_AUDIO "  Record");
            if (soundRecorderPlayBtnLbl)   lv_label_set_text(soundRecorderPlayBtnLbl, LV_SYMBOL_PLAY "  Play");
            soundRecorderSetFilesText("Deleted. List refreshed.");
        }
        return;
    }

    // Debounce/restart guard after a manual Stop press.
    // The recorder loop polls the physical encoder button while LVGL is busy.
    // Without this guard, the same release/click can be delivered after the
    // loop exits and instantly start a new recording.
    uint32_t nowMs = millis();
    if (soundRecorderIgnoreNextRecordClick) {
        if (nowMs < soundRecorderIgnoreRecordClickUntilMs) {
            if (digitalRead(ENCODER_BTN) == HIGH) {
                // Keep ignoring until the guard window expires, but release
                // detection confirms the button is no longer physically held.
            }
            return;
        }
        soundRecorderIgnoreNextRecordClick = false;
        soundRecorderIgnoreRecordClickUntilMs = 0;
    }

    // If we are already recording, this button becomes Stop. Do not restart
    // or reallocate the buffer; just ask the active recording loop to exit.
    if (soundRecorderRecording) {
        soundRecorderStopRequested = true;
        if (soundRecorderRecordBtnLbl) lv_label_set_text(soundRecorderRecordBtnLbl, LV_SYMBOL_STOP "  Stopping");
        soundRecorderSetStatus(LV_SYMBOL_STOP "  Stopping...", TH.warn);
        return;
    }

    if (!soundRecorderAllocBuffer()) {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  No buffer", TH.alert);
        soundRecorderUpdateInfo("Try shorter seconds in config.h");
        return;
    }

    const size_t requestedSamples = (size_t)AUDIO_RECORD_SAMPLE_RATE * (size_t)AUDIO_RECORD_SECONDS;
    bool usingShortBuffer = (soundRecorderCapacitySamples < requestedSamples);

    if (!soundRecorderInitInput()) {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  Mic failed", TH.alert);
        soundRecorderUpdateInfo("Check ES7210 I2C pins/config");
        return;
    }

    soundRecorderStopRequested = false;
    soundRecorderRecording = true;
    if (soundRecorderRecordBtnLbl) lv_label_set_text(soundRecorderRecordBtnLbl, LV_SYMBOL_STOP "  Stop");
    soundRecorderSetStatus(LV_SYMBOL_AUDIO "  Recording...", TH.warn);
    soundRecorderUpdateMeter(0);

    const size_t chunkSamples = 256;
    int16_t chunk[chunkSamples];
    soundRecorderSamples = 0;
    uint32_t lastUiMs = millis();
    int32_t peak = 0;

    // This callback records in a tight loop, so LVGL cannot dispatch a second
    // normal button-click event until recording finishes. To make the same
    // Record/Stop button responsive during recording, we also watch the
    // physical encoder button directly. The stop press is only armed after the
    // original Record press has been released, so it will not instantly cancel.
    bool stopButtonArmed = false;
    uint32_t lastStopPollMs = 0;

    while (soundRecorderSamples < soundRecorderCapacitySamples && !soundRecorderStopRequested) {
        // Manual stop while the blocking recorder loop is active.
        // LOW = pressed because ENCODER_BTN is configured with INPUT_PULLUP.
        if (millis() - lastStopPollMs > AUDIO_RECORD_STOP_POLL_MS) {
            lastStopPollMs = millis();
            bool encoderPressed = (digitalRead(ENCODER_BTN) == LOW);
            if (!stopButtonArmed) {
                if (!encoderPressed) stopButtonArmed = true;
            } else if (encoderPressed) {
                soundRecorderStopRequested = true;
                if (soundRecorderRecordBtnLbl) lv_label_set_text(soundRecorderRecordBtnLbl, LV_SYMBOL_STOP "  Stopping");
                soundRecorderSetStatus(LV_SYMBOL_STOP "  Stopping...", TH.warn);
                lv_timer_handler();
                break;
            }
        }

        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_NUM_1, chunk, sizeof(chunk), &bytesRead, pdMS_TO_TICKS(35));
        if (err != ESP_OK) break;
        size_t got = bytesRead / sizeof(int16_t);
        if (got == 0) {
            lv_timer_handler();
            continue;
        }

        size_t remain = soundRecorderCapacitySamples - soundRecorderSamples;
        if (got > remain) got = remain;
        memcpy(soundRecorderBuffer + soundRecorderSamples, chunk, got * sizeof(int16_t));
        soundRecorderSamples += got;

        for (size_t i = 0; i < got; i++) {
            int32_t v = chunk[i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }

        if (millis() - lastUiMs > 250) {
            soundRecorderUpdateMeter(peak);
            soundRecorderUpdateInfo();
            lv_timer_handler();
            lastUiMs = millis();
            peak = 0;
        }
    }

    bool stoppedManually = soundRecorderStopRequested;
    if (stoppedManually) {
        // Ignore one stale Record/Stop click caused by button bounce or by the
        // release event that LVGL receives after the blocking record loop ends.
        soundRecorderIgnoreNextRecordClick = true;
        soundRecorderIgnoreRecordClickUntilMs = millis() + AUDIO_RECORD_STOP_RESTART_GUARD_MS;
    }
    soundRecorderRecording = false;
    soundRecorderStopRequested = false;
    soundRecorderStopInput();
    if (soundRecorderRecordBtnLbl) lv_label_set_text(soundRecorderRecordBtnLbl, LV_SYMBOL_AUDIO "  Record");
    soundRecorderUpdateMeter(peak);

    if (stoppedManually) {
        soundRecorderUpdateInfo();
        if (soundRecorderSamples > 0) {
            bool savedToSD = soundRecorderSaveToSD();
            if (!savedToSD) {
                soundRecorderSetStatus(LV_SYMBOL_OK "  Stopped RAM", TH.success);
            }
            soundRecorderRefreshFileList();
        } else {
            soundRecorderSetStatus(LV_SYMBOL_OK "  Stopped", TH.success);
        }
    } else {
        soundRecorderUpdateInfo();
        bool savedToSD = soundRecorderSaveToSD();
        if (!savedToSD) {
            soundRecorderSetStatus(usingShortBuffer ? LV_SYMBOL_OK "  Short RAM" : LV_SYMBOL_OK "  Saved RAM", TH.success);
        }
        soundRecorderRefreshFileList();
    }
    pinMode(ENCODER_BTN, INPUT_PULLUP);
}

static void cb_soundRecorderPlay(lv_event_t *) {
    resetInactivityTimer();

    // If a recording in the right-side browser is selected, the bottom Play
    // button plays that file. Otherwise it falls back to the most recent RAM clip.
    if (soundRecorderSelectedPath[0]) {
        soundRecorderPlayWavFile(soundRecorderSelectedPath, soundRecorderSelectedName);
        return;
    }

    if (!soundRecorderBuffer || soundRecorderSamples == 0) {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  Select file or record", TH.warn);
        return;
    }

    if (soundReady) stopSoundDriverAfterChirp();
    if (!ensureSoundReady(false)) {
        soundRecorderSetStatus(LV_SYMBOL_WARNING "  Speaker init failed", TH.alert);
        return;
    }

    // Playback speed tuner:
    // If ES7210/I2S RX provides duplicated/interleaved samples, playback can sound
    // slow-motion. Raising the TX sample rate is the safest first correction
    // because it does not rewrite the recording buffer.
    uint32_t playbackRate = ((uint32_t)AUDIO_RECORD_SAMPLE_RATE * (uint32_t)AUDIO_RECORD_PLAYBACK_SPEED_PERCENT) / 100U;
    if (playbackRate < 8000U) playbackRate = 8000U;
    if (playbackRate > 48000U) playbackRate = 48000U;
    esp_err_t clkErr = i2s_set_clk(I2S_NUM_0, playbackRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    Serial.printf("[Audio] Playback rate=%u Hz speed=%u%% clk=%s\n",
                  (unsigned)playbackRate,
                  (unsigned)AUDIO_RECORD_PLAYBACK_SPEED_PERCENT,
                  clkErr == ESP_OK ? "OK" : "ERR");

    if (soundRecorderPlayBtnLbl) lv_label_set_text(soundRecorderPlayBtnLbl, LV_SYMBOL_STOP "  Playing");
    char playStatus[64];
    snprintf(playStatus, sizeof(playStatus), LV_SYMBOL_PLAY "  Playing at %u%%...", (unsigned)AUDIO_RECORD_PLAYBACK_SPEED_PERCENT);
    soundRecorderSetStatus(playStatus, TH.warn);

    const size_t framesPerChunk = 128;
    int16_t stereo[framesPerChunk * 2];
    size_t pos = 0;
    uint8_t volumePct = AUDIO_RECORD_PLAYBACK_VOLUME_PERCENT;
    if (volumePct > 100) volumePct = 100;

    while (pos < soundRecorderSamples) {
        size_t frames = soundRecorderSamples - pos;
        if (frames > framesPerChunk) frames = framesPerChunk;
        for (size_t i = 0; i < frames; i++) {
            int32_t s = soundRecorderBuffer[pos + i];
            s = (s * volumePct) / 100;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            stereo[i * 2]     = (int16_t)s;
            stereo[i * 2 + 1] = (int16_t)s;
        }
        size_t bytesWritten = 0;
        i2s_write(I2S_NUM_0, stereo, frames * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
        pos += frames;
        if ((pos % (framesPerChunk * 8)) == 0) lv_timer_handler();
    }

    stopSoundDriverAfterChirp();
    if (soundRecorderPlayBtnLbl) lv_label_set_text(soundRecorderPlayBtnLbl, LV_SYMBOL_PLAY "  Play");
    soundRecorderSetStatus(LV_SYMBOL_OK "  Playback done", TH.success);
    pinMode(ENCODER_BTN, INPUT_PULLUP);
}

static void cb_audioMenuBack(lv_event_t *) {
    audioMenuScreen = nullptr;
    deleteGroup(&audioMenuGroup);
    setGroup(navGroup);
    lv_screen_load_anim(mainScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    setAllLEDs(MENU_COLORS[3].r, MENU_COLORS[3].g, MENU_COLORS[3].b);
}

static void cb_audioToolBack(lv_event_t *) {
    // If Back is used while recording, request a clean stop before leaving.
    soundRecorderStopRequested = true;
    soundRecorderRecording = false;
    soundRecorderStopInput();
    audioToolScreen = nullptr;
    deleteGroup(&audioToolGroup);
    setGroup(audioMenuGroup);
    lv_screen_load_anim(audioMenuScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    setAllLEDs(MENU_COLORS[3].r, MENU_COLORS[3].g, MENU_COLORS[3].b, 3);
}

static void cb_audioToolSelected(lv_event_t *e) {
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    switch (t) {
        case 0: createSoundRecorder(); break;
    }
}

void createAudioMenu() {
    if (audioMenuScreen) { lv_obj_delete(audioMenuScreen); audioMenuScreen = nullptr; }
    audioMenuScreen = lv_obj_create(nullptr);
    applyScreenStyle(audioMenuScreen);
    createHeader(audioMenuScreen, LV_SYMBOL_AUDIO "  Audio Tools");

    lv_obj_t *list = lv_list_create(audioMenuScreen);
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

    deleteGroup(&audioMenuGroup);
    audioMenuGroup = lv_group_create();

    for (int i = 0; i < 1; i++) {
        lv_obj_t *btn = lv_list_add_btn(list, nullptr, AUDIO_TOOL_LABELS[i]);
        styleListBtn(btn);
        lv_obj_set_height(btn, 30);
        lv_obj_add_event_cb(btn, cb_audioToolSelected, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_group_add_obj(audioMenuGroup, btn);
    }

    lv_obj_t *backBtn = createBackBtn(audioMenuScreen, cb_audioMenuBack);
    lv_group_add_obj(audioMenuGroup, backBtn);
    setGroup(audioMenuGroup);

    setAllLEDs(MENU_COLORS[3].r, MENU_COLORS[3].g, MENU_COLORS[3].b, 3);
    lv_screen_load_anim(audioMenuScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

void createSoundRecorder() {
    soundRecorderStatusLbl = nullptr;
    soundRecorderInfoLbl = nullptr;
    soundRecorderMeterLbl = nullptr;
    soundRecorderRecordBtn = nullptr;
    soundRecorderPlayBtn = nullptr;
    soundRecorderRecordBtnLbl = nullptr;
    soundRecorderPlayBtnLbl = nullptr;
    soundRecorderFilesList = nullptr;
    soundRecorderFileMenuScreen = nullptr;
    soundRecorderRecordPanel = nullptr;
    soundRecorderSelectedFile = -1;
    soundRecorderSelectedPath[0] = '\0';
    soundRecorderSelectedName[0] = '\0';

    if (audioToolScreen) { lv_obj_delete(audioToolScreen); audioToolScreen = nullptr; }
    audioToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(audioToolScreen);
    createHeader(audioToolScreen, LV_SYMBOL_AUDIO "  Sound Recorder");

    // Sound Recorder uses its own compact layout so the buttons do not
    // overlap the shared Back/Action buttons used by other pages.
    const int panelY = 28;
    const int panelH = 105;  // Taller panels so the recorder info fits cleanly.
    const int panelGap = 6;
    const int panelW = (SCREEN_W - 18) / 2;  // two even side-by-side boxes
    const int leftX  = 6;
    const int rightX = leftX + panelW + panelGap;

    lv_obj_t *recordPanel = lv_obj_create(audioToolScreen);
    soundRecorderRecordPanel = recordPanel;
    lv_obj_set_size(recordPanel, panelW, panelH);
    lv_obj_set_pos(recordPanel, leftX, panelY);
    lv_obj_set_style_bg_color(recordPanel,     lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(recordPanel,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(recordPanel, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(recordPanel, 1,                      LV_PART_MAIN);
    lv_obj_set_style_radius(recordPanel,       6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(recordPanel,      6,                      LV_PART_MAIN);
    // Allow vertical scroll as a safety net if future recorder details grow.
    lv_obj_add_flag(recordPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(recordPanel, LV_OBJ_FLAG_CLICKABLE);  // lets the encoder focus this scroll area
    lv_obj_set_scroll_dir(recordPanel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(recordPanel, LV_SCROLLBAR_MODE_AUTO);
    // Focus styling makes it obvious when the left scroll panel is selected.
    lv_obj_set_style_border_color(recordPanel, TC(accent), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(recordPanel, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(recordPanel, TC(accent), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(recordPanel, 2, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    lv_obj_t *recordTitle = lv_label_create(recordPanel);
    lv_label_set_text(recordTitle, LV_SYMBOL_AUDIO "  Recorder");
    lv_obj_set_style_text_color(recordTitle, lv_color_hex(TH.accent), LV_PART_MAIN);
    lv_obj_set_pos(recordTitle, 0, 0);

    soundRecorderStatusLbl = lv_label_create(recordPanel);
    lv_label_set_text(soundRecorderStatusLbl, LV_SYMBOL_AUDIO "  Ready");
    lv_label_set_long_mode(soundRecorderStatusLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(soundRecorderStatusLbl, panelW - 14);
    lv_obj_set_style_text_color(soundRecorderStatusLbl, lv_color_hex(TH.success), LV_PART_MAIN);
    lv_obj_set_pos(soundRecorderStatusLbl, 0, 19);

    soundRecorderMeterLbl = lv_label_create(recordPanel);
    lv_label_set_text(soundRecorderMeterLbl, "Lvl:[----------]");
    lv_label_set_long_mode(soundRecorderMeterLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(soundRecorderMeterLbl, panelW - 14);
    lv_obj_set_style_text_color(soundRecorderMeterLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_obj_set_pos(soundRecorderMeterLbl, 0, 39);

    soundRecorderInfoLbl = lv_label_create(recordPanel);
    lv_label_set_text(soundRecorderInfoLbl, "Time: 0.0 / 0.0s\nPlay: 200%");
    lv_label_set_long_mode(soundRecorderInfoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(soundRecorderInfoLbl, panelW - 14);
    lv_obj_set_style_text_color(soundRecorderInfoLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_set_pos(soundRecorderInfoLbl, 0, 59);

    lv_obj_t *filesPanel = lv_obj_create(audioToolScreen);
    lv_obj_set_size(filesPanel, panelW, panelH);
    lv_obj_set_pos(filesPanel, rightX, panelY);
    lv_obj_set_style_bg_color(filesPanel,     lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(filesPanel,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(filesPanel, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(filesPanel, 1,                      LV_PART_MAIN);
    lv_obj_set_style_radius(filesPanel,       6,                      LV_PART_MAIN);
    lv_obj_set_style_pad_all(filesPanel,      6,                      LV_PART_MAIN);
    // Keep this panel scroll-ready now so the future recordings/file browser
    // can grow vertically without cutting off filenames or details.
    lv_obj_add_flag(filesPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(filesPanel, LV_OBJ_FLAG_CLICKABLE);  // lets the encoder focus this future file browser
    lv_obj_set_scroll_dir(filesPanel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(filesPanel, LV_SCROLLBAR_MODE_AUTO);
    // Focus styling makes it obvious when the right scroll panel is selected.
    lv_obj_set_style_border_color(filesPanel, TC(accent), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(filesPanel, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(filesPanel, TC(accent), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(filesPanel, 2, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    lv_obj_t *filesTitle = lv_label_create(filesPanel);
    lv_label_set_text(filesTitle, LV_SYMBOL_DIRECTORY "  Recordings");
    lv_obj_set_style_text_color(filesTitle, lv_color_hex(TH.accent), LV_PART_MAIN);
    lv_obj_set_pos(filesTitle, 0, 0);

    soundRecorderFilesList = lv_list_create(filesPanel);
    // Leave a little more room for the footer/status label so the bottom
    // status text is not clipped by the recordings panel edge.
    lv_obj_set_size(soundRecorderFilesList, panelW - 12, panelH - 44);
    lv_obj_set_pos(soundRecorderFilesList, 0, 22);
    lv_obj_set_style_bg_opa(soundRecorderFilesList, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(soundRecorderFilesList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(soundRecorderFilesList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(soundRecorderFilesList, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(soundRecorderFilesList, lv_color_hex(TH.accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(soundRecorderFilesList, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(soundRecorderFilesList, 4, LV_PART_SCROLLBAR);

    soundRecorderFilesInfoLbl = lv_label_create(filesPanel);
    lv_label_set_text(soundRecorderFilesInfoLbl, "Checking SD...");
    lv_label_set_long_mode(soundRecorderFilesInfoLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(soundRecorderFilesInfoLbl, panelW - 14);
    lv_obj_set_style_text_color(soundRecorderFilesInfoLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    // Lift the footer/status text slightly so the full letters remain visible.
    lv_obj_set_pos(soundRecorderFilesInfoLbl, 0, panelH - 22);

    // Page-specific button row. These do not use createBackBtn() or
    // createActionBtn(), so their size and placement can be tuned here only.
    const int btnY = SCREEN_H - 30;
    const int btnW = 94;
    const int btnH = 27;

    lv_obj_t *backBtn = lv_btn_create(audioToolScreen);
    lv_obj_set_size(backBtn, btnW, btnH);
    lv_obj_set_pos(backBtn, 6, btnY);
    lv_obj_set_style_bg_color(backBtn, TC(btnDefault), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(backBtn, TC(btnFocus),   LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(backBtn, TC(btnPress),   LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(backBtn, TC(border), LV_PART_MAIN);
    lv_obj_set_style_border_width(backBtn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(backBtn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(backBtn, cb_audioToolBack, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_color(backLbl, TC(text), LV_PART_MAIN);
    lv_obj_center(backLbl);

    soundRecorderRecordBtn = lv_btn_create(audioToolScreen);
    lv_obj_set_size(soundRecorderRecordBtn, btnW, btnH);
    lv_obj_set_pos(soundRecorderRecordBtn, 113, btnY);
    lv_obj_set_style_bg_color(soundRecorderRecordBtn, TC(actionBg),  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(soundRecorderRecordBtn, TC(actionFoc), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(soundRecorderRecordBtn, TC(stopRed),   LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(soundRecorderRecordBtn, TC(actionBdr), LV_PART_MAIN);
    lv_obj_set_style_border_width(soundRecorderRecordBtn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(soundRecorderRecordBtn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(soundRecorderRecordBtn, cb_soundRecorderRecord, LV_EVENT_CLICKED, nullptr);
    soundRecorderRecordBtnLbl = lv_label_create(soundRecorderRecordBtn);
    lv_label_set_text(soundRecorderRecordBtnLbl, LV_SYMBOL_AUDIO "  Record");
    lv_obj_set_style_text_color(soundRecorderRecordBtnLbl, TC(text), LV_PART_MAIN);
    lv_obj_center(soundRecorderRecordBtnLbl);

    soundRecorderPlayBtn = lv_btn_create(audioToolScreen);
    lv_obj_set_size(soundRecorderPlayBtn, btnW, btnH);
    lv_obj_set_pos(soundRecorderPlayBtn, 220, btnY);
    lv_obj_set_style_bg_color(soundRecorderPlayBtn, TC(actionBg),  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(soundRecorderPlayBtn, TC(actionFoc), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(soundRecorderPlayBtn, TC(btnPress),  LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(soundRecorderPlayBtn, TC(actionBdr), LV_PART_MAIN);
    lv_obj_set_style_border_width(soundRecorderPlayBtn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(soundRecorderPlayBtn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(soundRecorderPlayBtn, cb_soundRecorderPlay, LV_EVENT_CLICKED, nullptr);
    soundRecorderPlayBtnLbl = lv_label_create(soundRecorderPlayBtn);
    lv_label_set_text(soundRecorderPlayBtnLbl, LV_SYMBOL_PLAY "  Play");
    lv_obj_set_style_text_color(soundRecorderPlayBtnLbl, TC(text), LV_PART_MAIN);
    lv_obj_center(soundRecorderPlayBtnLbl);

    deleteGroup(&audioToolGroup);
    audioToolGroup = lv_group_create();

    // Encoder focus order for this page:
    // Recorder panel -> saved WAV file buttons -> Back -> Record -> Play.
    // Clicking a saved WAV now opens a Play/Delete action screen.
    lv_group_add_obj(audioToolGroup, recordPanel);

    // Refresh now so the generated file buttons get inserted into the group
    // before the bottom controls.
    soundRecorderUpdateInfo();
    soundRecorderRefreshFileList();

    lv_group_add_obj(audioToolGroup, backBtn);
    lv_group_add_obj(audioToolGroup, soundRecorderRecordBtn);
    lv_group_add_obj(audioToolGroup, soundRecorderPlayBtn);
    setGroup(audioToolGroup);
    setAllLEDs(MENU_COLORS[3].r, MENU_COLORS[3].g, MENU_COLORS[3].b, LED_BRIGHTNESS);
    lv_screen_load_anim(audioToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
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

    // Device Info can grow as we add useful diagnostics, so keep this card scrollable.
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);

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
static const char *WIFI_TOOL_LABELS[13] = {
    LV_SYMBOL_WIFI     "  Network Scanner",
    LV_SYMBOL_WIFI     "  Connect to AP",
    LV_SYMBOL_EYE_OPEN "  LAN Host Discovery",
    LV_SYMBOL_HOME     "  Gateway Info",
    LV_SYMBOL_EYE_OPEN "  Station Scanner",
    LV_SYMBOL_WARNING  "  Deauth Detector",
    LV_SYMBOL_LOOP     "  Channel Analyzer",
    LV_SYMBOL_EYE_OPEN "  Packet Monitor",
    LV_SYMBOL_EYE_OPEN "  WiFi Mapper",
    LV_SYMBOL_EYE_OPEN "  PineAP Hunter",
    LV_SYMBOL_EYE_OPEN "  Pwnagotchi Watch",
    LV_SYMBOL_WARNING  "  Flock Detector",
    LV_SYMBOL_BLUETOOTH "  Flock Hybrid"
};

static const int WIFI_TOOL_LAN_DISCOVERY_INDEX = 2;
static const int WIFI_TOOL_GATEWAY_INFO_INDEX  = 3;

static void cb_wifiToolSelected(lv_event_t *e);

// Connected-only WiFi Tools row state.
// LAN Host Discovery starts dimmed when there is no AP connection, but after
// Connect to AP succeeds we update this row in place instead of rebuilding the
// whole WiFi Tools screen. Rebuilding from inside a Back/event path was causing
// LVGL object invalidation and LoadProhibited reboots.
static lv_obj_t *wifiLanDiscoveryBtn = nullptr;
static bool wifiLanDiscoveryBtnHasEvent = false;
static bool wifiLanDiscoveryBtnEnabled = false;
static lv_obj_t *wifiGatewayInfoBtn = nullptr;
static bool wifiGatewayInfoBtnHasEvent = false;
static bool wifiGatewayInfoBtnEnabled = false;

static void setListBtnText(lv_obj_t *btn, const char *txt) {
    if (!btn || !txt) return;
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label) {
        lv_label_set_text(label, txt);
    }
}

static void refreshWiFiMenuLanDiscoveryItem() {
#if LAN_DISCOVERY_ENABLED
    bool connected = (WiFi.status() == WL_CONNECTED);

    if (wifiLanDiscoveryBtn) {
        // Keep the LAN Host Discovery row in the encoder group at all times.
        // The click handler still checks connection state, so the row can safely
        // stay selectable while showing "connect first" when no AP is active.
        lv_obj_clear_state(wifiLanDiscoveryBtn, LV_STATE_DISABLED);
        lv_obj_add_flag(wifiLanDiscoveryBtn, LV_OBJ_FLAG_CLICKABLE);

        if (!wifiLanDiscoveryBtnHasEvent) {
            lv_obj_add_event_cb(wifiLanDiscoveryBtn, cb_wifiToolSelected, LV_EVENT_CLICKED,
                                (void *)(intptr_t)WIFI_TOOL_LAN_DISCOVERY_INDEX);
            wifiLanDiscoveryBtnHasEvent = true;
        }

        if (connected) {
            setListBtnText(wifiLanDiscoveryBtn, WIFI_TOOL_LABELS[WIFI_TOOL_LAN_DISCOVERY_INDEX]);
            lv_obj_set_style_text_color(wifiLanDiscoveryBtn, TC(text), LV_PART_MAIN);
            lv_obj_set_style_bg_color(wifiLanDiscoveryBtn, TC(card), LV_PART_MAIN);
        } else {
            setListBtnText(wifiLanDiscoveryBtn, LV_SYMBOL_EYE_OPEN "  LAN Host Discovery (connect first)");
            lv_obj_set_style_text_color(wifiLanDiscoveryBtn, TC(textDim), LV_PART_MAIN);
            lv_obj_set_style_bg_color(wifiLanDiscoveryBtn, TC(card), LV_PART_MAIN);
        }
        wifiLanDiscoveryBtnEnabled = true;
    }
#endif

#if GATEWAY_INFO_ENABLED
    if (wifiGatewayInfoBtn) {
        // Gateway Info follows the same connected-only UX as LAN Host Discovery:
        // visible in WiFi Tools, dimmed before Connect to AP, and usable once connected.
        lv_obj_clear_state(wifiGatewayInfoBtn, LV_STATE_DISABLED);
        lv_obj_add_flag(wifiGatewayInfoBtn, LV_OBJ_FLAG_CLICKABLE);

        if (!wifiGatewayInfoBtnHasEvent) {
            lv_obj_add_event_cb(wifiGatewayInfoBtn, cb_wifiToolSelected, LV_EVENT_CLICKED,
                                (void *)(intptr_t)WIFI_TOOL_GATEWAY_INFO_INDEX);
            wifiGatewayInfoBtnHasEvent = true;
        }

        if (connected) {
            setListBtnText(wifiGatewayInfoBtn, WIFI_TOOL_LABELS[WIFI_TOOL_GATEWAY_INFO_INDEX]);
            lv_obj_set_style_text_color(wifiGatewayInfoBtn, TC(text), LV_PART_MAIN);
            lv_obj_set_style_bg_color(wifiGatewayInfoBtn, TC(card), LV_PART_MAIN);
        } else {
            setListBtnText(wifiGatewayInfoBtn, LV_SYMBOL_HOME "  Gateway Info (connect first)");
            lv_obj_set_style_text_color(wifiGatewayInfoBtn, TC(textDim), LV_PART_MAIN);
            lv_obj_set_style_bg_color(wifiGatewayInfoBtn, TC(card), LV_PART_MAIN);
        }
        wifiGatewayInfoBtnEnabled = true;
    }
#endif
}

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
        case 1: createConnectAPTool();       break;
        case 2: createLANHostDiscovery();    break;
        case 3: createGatewayInfo();         break;
        case 4: createStationScanner();      break;
        case 5: createDeauthDetector();      break;
        case 6: createChannelAnalyzer();     break;
        case 7: createPacketMonitor();       break;
        case 8: createWiFiMapper();          break;
        case 9: createPineAPHunter();        break;
        case 10: createPwnagotchiDetector(); break;
        case 11: createFlockDetector();      break;
        case 12: createFlockHybridScanner(); break;
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

    wifiLanDiscoveryBtn = nullptr;
    wifiLanDiscoveryBtnHasEvent = false;
    wifiLanDiscoveryBtnEnabled = false;
    wifiGatewayInfoBtn = nullptr;
    wifiGatewayInfoBtnHasEvent = false;
    wifiGatewayInfoBtnEnabled = false;

    bool lanToolsReady = (WiFi.status() == WL_CONNECTED);

    for (int i = 0; i < (int)(sizeof(WIFI_TOOL_LABELS) / sizeof(WIFI_TOOL_LABELS[0])); i++) {
        bool isLanDiscovery = (i == WIFI_TOOL_LAN_DISCOVERY_INDEX);
        bool isGatewayInfo  = (i == WIFI_TOOL_GATEWAY_INFO_INDEX);
        bool connectedToolWaiting = (isLanDiscovery || isGatewayInfo) && !lanToolsReady;

        const char *rowText = WIFI_TOOL_LABELS[i];
        if (connectedToolWaiting) {
            rowText = isLanDiscovery
                          ? LV_SYMBOL_EYE_OPEN "  LAN Host Discovery (connect first)"
                          : LV_SYMBOL_HOME "  Gateway Info (connect first)";
        }

        lv_obj_t *btn = lv_list_add_btn(list, nullptr, rowText);
        styleListBtn(btn);
        lv_obj_set_height(btn, 30);

        lv_obj_add_event_cb(btn, cb_wifiToolSelected, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_group_add_obj(wifiMenuGroup, btn);

        if (isLanDiscovery) {
            wifiLanDiscoveryBtn = btn;
            wifiLanDiscoveryBtnHasEvent = true;
            wifiLanDiscoveryBtnEnabled = true;
        }
        if (isGatewayInfo) {
            wifiGatewayInfoBtn = btn;
            wifiGatewayInfoBtnHasEvent = true;
            wifiGatewayInfoBtnEnabled = true;
        }

        // Visual-only dim state. Connected-only rows stay in the encoder group
        // so they can become usable immediately after Connect to AP without
        // rebuilding the whole WiFi Tools menu or re-adding LVGL group objects.
        if (connectedToolWaiting) {
            lv_obj_set_style_text_color(btn, TC(textDim), LV_PART_MAIN);
            lv_obj_set_style_bg_color(btn, TC(card), LV_PART_MAIN);
        }
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

// Deauth Detector UI object pointers.
// Kept above shared back/cleanup callbacks so those callbacks can safely clear them.
static lv_obj_t *deauthCountLbl  = nullptr;
static lv_obj_t *deauthEventList = nullptr;
static lv_obj_t *deauthStatsLbl  = nullptr;
static lv_obj_t *deauthStatsBar  = nullptr;

// ════════════════════════════════════════════════════════════════
//  SHARED BACK CALLBACKS
// ════════════════════════════════════════════════════════════════
// WiFi Tools can change while a tool is open; for example, Connect to AP can
// make LAN Host Discovery become available. Rebuild the WiFi Tools menu after
// the current LVGL event finishes so we do not delete/rebuild screens while
// the Back button event is still unwinding.
static lv_timer_t *wifiMenuRefreshTimer = nullptr;
static lv_obj_t  *wifiToolScreenPendingDelete = nullptr;

static void wifiMenuRefreshTimerCb(lv_timer_t *timer) {
    lv_timer_delete(timer);
    wifiMenuRefreshTimer = nullptr;

    lv_obj_t *oldToolScreen = wifiToolScreenPendingDelete;
    wifiToolScreenPendingDelete = nullptr;

    createWiFiMenu();

    if (oldToolScreen) {
        lv_obj_delete(oldToolScreen);
    }

    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, 3);
}

static void scheduleWiFiMenuRefresh(lv_obj_t *oldToolScreen) {
    wifiToolScreenPendingDelete = oldToolScreen;

    if (wifiMenuRefreshTimer) {
        lv_timer_delete(wifiMenuRefreshTimer);
        wifiMenuRefreshTimer = nullptr;
    }

    wifiMenuRefreshTimer = lv_timer_create(wifiMenuRefreshTimerCb, 75, nullptr);
}

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
    if (stationScanActive) {
        stationScanActive = false;
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);
    }
    if (wifiMapperActive) {
        wifiMapperActive = false;
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);
    }
    if (spinnerRunning) {
        stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
    }
    if (deauthTimer) { lv_timer_delete(deauthTimer); deauthTimer = nullptr; }
    deauthCountLbl = nullptr;
    deauthStatsLbl = nullptr;
    deauthStatsBar = nullptr;
    deauthEventList = nullptr;
    if (pwnTimer)    { lv_timer_delete(pwnTimer);    pwnTimer    = nullptr; }
    if (flockTimer)  { lv_timer_delete(flockTimer);  flockTimer  = nullptr; }
    if (hybridStartTimer) { lv_timer_delete(hybridStartTimer); hybridStartTimer = nullptr; }
    hybridStatusLbl = nullptr;
    hybridList = nullptr;
    hybridBackBtn = nullptr;
    hybridScanBtn = nullptr;
    if (packetMonitorTimer) { lv_timer_delete(packetMonitorTimer); packetMonitorTimer = nullptr; }
    if (wifiMapperTimer) { lv_timer_delete(wifiMapperTimer); wifiMapperTimer = nullptr; }
    if (stationScanTimer) { lv_timer_delete(stationScanTimer); stationScanTimer = nullptr; }
    stationStatusLbl = nullptr;
    stationList = nullptr;
    stationStartBtn = nullptr;
    stationStartLbl = nullptr;
    stationBackBtn = nullptr;
    wifiMapperStatusLbl = nullptr;
    wifiMapperDetailLbl = nullptr;
    wifiMapperGridArea = nullptr;
    wifiMapperPauseBtn = nullptr;
    wifiMapperPauseLbl = nullptr;
    wifiMapperSpeedBtn = nullptr;
    wifiMapperSpeedLbl = nullptr;
    connectApStatusLbl = nullptr;
    connectApList = nullptr;
    connectApBackBtn = nullptr;
    connectApScanBtn = nullptr;
    connectApDiscBtn = nullptr;
    lanDiscoveryStatusLbl = nullptr;
    lanDiscoveryList = nullptr;
    lanDiscoveryBackBtn = nullptr;
    lanDiscoveryScanBtn = nullptr;
    gatewayInfoStatusLbl = nullptr;
    gatewayInfoList = nullptr;
    gatewayInfoBackBtn = nullptr;
    gatewayInfoRefreshBtn = nullptr;
    // Return to the existing WiFi Tools menu, then update only the connected-only
    // LAN Host Discovery / Gateway Info rows in place. Do not rebuild/delete the whole WiFi menu
    // from this Back path; that previously invalidated LVGL objects and caused
    // LoadProhibited reboots after Connect to AP.
    deleteGroup(&wifiToolGroup);
    setGroup(wifiMenuGroup);
    refreshWiFiMenuLanDiscoveryItem();

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
    lv_group_add_obj(wifiDetailGroup, card);     // Focus card first so encoder can scroll Station Detail.
    lv_group_add_obj(wifiDetailGroup, backBtn);
    setGroup(wifiDetailGroup);

    lv_screen_load_anim(wifiDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}


// ════════════════════════════════════════════════════════════════
//  TOOL 2 – CONNECT TO AP
//
//  Scans nearby access points, lets you pick one with the encoder,
//  opens the reusable LVGL keyboard for the password, then connects
//  the ESP32-S3 in STA mode. The connection is intentionally left
//  active for future safe LAN tools such as ping/gateway checks,
//  simple port checks, or SSH banner checks.
// ════════════════════════════════════════════════════════════════
static void connectApSetStatus(const char *msg, uint32_t colorHex) {
    if (!connectApStatusLbl) return;
    lv_label_set_text(connectApStatusLbl, msg);
    lv_obj_set_style_text_color(connectApStatusLbl, lv_color_hex(colorHex), LV_PART_MAIN);
}

static void connectApCaptureTarget(int idx) {
    if (idx < 0 || idx >= wifiEntryCount) return;
    connectApSelectedIdx = idx;
    strncpy(connectApTargetSsid, wifiEntries[idx].ssid, sizeof(connectApTargetSsid) - 1);
    connectApTargetSsid[sizeof(connectApTargetSsid) - 1] = '\0';
    strncpy(connectApTargetBssid, wifiEntries[idx].bssid, sizeof(connectApTargetBssid) - 1);
    connectApTargetBssid[sizeof(connectApTargetBssid) - 1] = '\0';
    strncpy(connectApTargetAuth, wifiEntries[idx].authStr, sizeof(connectApTargetAuth) - 1);
    connectApTargetAuth[sizeof(connectApTargetAuth) - 1] = '\0';
    connectApTargetOpen = wifiEntries[idx].open;
    connectApTargetRssi = wifiEntries[idx].rssi;
    connectApTargetChannel = wifiEntries[idx].channel;
}

// Saved AP credentials are stored in a few simple NVS slots instead of
// using the SSID as a raw NVS key. NVS keys are short, and SSIDs may contain
// characters that are not ideal for key names.
static bool connectApLoadSavedPassword(const char *ssid, char *out, size_t outLen) {
    if (!ssid || !ssid[0] || !out || outLen == 0) return false;
    out[0] = '\0';

#if PERSISTENT_SETTINGS_ENABLED && CONNECT_AP_SAVE_PASSWORDS
    // Use a short-lived local Preferences handle for AP credentials.
    // This avoids sharing the main settingsPrefs handle during WiFi/LVGL flows.
    Preferences apPrefs;
    if (apPrefs.begin(PREFS_NAMESPACE, true)) {
        for (int i = 0; i < CONNECT_AP_SAVED_SLOT_COUNT; i++) {
            char ssidKey[8];
            char passKey[8];
            snprintf(ssidKey, sizeof(ssidKey), "apS%d", i);
            snprintf(passKey, sizeof(passKey), "apP%d", i);

            String savedSsid = apPrefs.getString(ssidKey, "");
            if (savedSsid == ssid) {
                String savedPass = apPrefs.getString(passKey, "");
                apPrefs.end();

                if (savedPass.length() > 0) {
                    savedPass.toCharArray(out, outLen);
                    Serial.printf("[Connect AP] Loaded saved password slot %d for SSID: %s\n", i, ssid);
                    return true;
                }
                return false;
            }
        }
        apPrefs.end();
    }
#endif

#if CONNECT_AP_USE_CONFIG_CREDENTIALS
    for (int i = 0; i < CONNECT_AP_CONFIG_CRED_COUNT; i++) {
        const char *cfgSsid = CONNECT_AP_CONFIG_SSIDS[i];
        const char *cfgPass = CONNECT_AP_CONFIG_PASSWORDS[i];
        if (cfgSsid && cfgPass && cfgSsid[0] && cfgPass[0] && strcmp(cfgSsid, ssid) == 0) {
            strncpy(out, cfgPass, outLen - 1);
            out[outLen - 1] = '\0';
            Serial.printf("[Connect AP] Loaded config password slot %d for SSID: %s\n", i, ssid);
            return true;
        }
    }
#endif

    return false;
}

static void connectApSavePassword(const char *ssid, const char *password) {
    if (!ssid || !ssid[0] || !password || !password[0]) return;

#if PERSISTENT_SETTINGS_ENABLED && CONNECT_AP_SAVE_PASSWORDS
    // Use a local Preferences object instead of the shared settingsPrefs handle.
    // This keeps AP credential writes isolated from the normal settings system.
    Preferences apPrefs;
    if (!apPrefs.begin(PREFS_NAMESPACE, false)) {
        Serial.println("[Connect AP] Password save skipped: NVS open failed.");
        return;
    }

    int targetSlot = -1;
    int emptySlot = -1;
    for (int i = 0; i < CONNECT_AP_SAVED_SLOT_COUNT; i++) {
        char ssidKey[8];
        snprintf(ssidKey, sizeof(ssidKey), "apS%d", i);

        String savedSsid = apPrefs.getString(ssidKey, "");
        if (savedSsid == ssid) {
            targetSlot = i;
            break;
        }
        if (emptySlot < 0 && savedSsid.length() == 0) {
            emptySlot = i;
        }
    }

    if (targetSlot < 0) {
        targetSlot = (emptySlot >= 0) ? emptySlot : 0;  // overwrite oldest/basic slot 0 if full
    }

    char ssidKey[8];
    char passKey[8];
    snprintf(ssidKey, sizeof(ssidKey), "apS%d", targetSlot);
    snprintf(passKey, sizeof(passKey), "apP%d", targetSlot);

    bool ssidOk = apPrefs.putString(ssidKey, ssid) > 0;
    bool passOk = apPrefs.putString(passKey, password) > 0;
    apPrefs.end();

    Serial.printf("[Connect AP] Saved password slot %d for SSID: %s (%s/%s)\n",
                  targetSlot,
                  ssid,
                  ssidOk ? "ssid ok" : "ssid fail",
                  passOk ? "pass ok" : "pass fail");
#endif
}


static const char* connectApInternetStatusText(bool *reachableOut) {
#if CONNECT_AP_INTERNET_CHECK_ENABLED
    if (reachableOut) *reachableOut = false;
    if (WiFi.status() != WL_CONNECTED) return "No";

    WiFiClient client;
    client.setTimeout(CONNECT_AP_INTERNET_CHECK_TIMEOUT_MS);

    bool reachable = client.connect(CONNECT_AP_INTERNET_CHECK_HOST,
                                    CONNECT_AP_INTERNET_CHECK_PORT);
    if (reachable) {
        client.stop();
        if (reachableOut) *reachableOut = true;
        return "Yes";
    }

    client.stop();
    return "No";
#else
    if (reachableOut) *reachableOut = false;
    return "Skipped";
#endif
}

static void connectApDisconnectNow() {
    // Manual disconnect should play from the button handler only, not again from
    // the background connection watchdog.
    connectApSuppressDropTone(1500);
    WiFi.disconnect(false, false);
    delay(60);
    connectApMarkConnectedState(false);
}

static void connectApShowStatusPage(const char *titleText, const char *bodyText, bool connected) {
    // Do not delete the currently active WiFi tool screen before the new
    // status page is loaded. This mirrors the keyboard close fix and avoids
    // LVGL deleting an active screen during an event/callback chain.
    lv_obj_t *oldWifiScreen = wifiToolScreen;
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, titleText);

    lv_obj_t *card = lv_obj_create(wifiToolScreen);
    lv_obj_set_size(card, SCREEN_W - 14, SCREEN_H - 28 - 38);
    lv_obj_set_pos(card, 7, 31);
    lv_obj_set_style_bg_color(card, TC(card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, connected ? TC(success) : TC(border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 7, LV_PART_MAIN);

    // The connected router/status page has more details than the older result page,
    // so keep the card scrollable on the compact 320x170 T-Embed screen.
    if (connected) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(card, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
    } else {
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *info = lv_label_create(card);
    lv_label_set_text(info, bodyText);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, SCREEN_W - 36);
    lv_obj_set_style_text_color(info, connected ? TC(success) : TC(text), LV_PART_MAIN);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 0, 0);

    // Result hint removed: future WiFi-connected tools will live in their own pages.
    // This also keeps the status/details text from overlapping on the compact T-Embed screen.

    lv_obj_t *backBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    lv_obj_set_size(backBtn, 86, 26);
    lv_obj_align(backBtn, LV_ALIGN_BOTTOM_LEFT, 6, -4);

    lv_obj_t *scanBtn = createActionBtn(wifiToolScreen, LV_SYMBOL_REFRESH " Scan", [](lv_event_t *e) {
        createConnectAPTool();
    });
    lv_obj_set_size(scanBtn, 86, 26);
    lv_obj_align(scanBtn, LV_ALIGN_BOTTOM_RIGHT, -6, -4);

    // Add a center Disconnect button only when connected. Width/positions are
    // tuned for the 320x170 T-Embed screen so Back/Disconnect/Scan do not overlap.
    lv_obj_t *discBtn = nullptr;
    if (connected) {
        discBtn = lv_btn_create(wifiToolScreen);
        lv_obj_set_size(discBtn, 108, 26);
        lv_obj_align(discBtn, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_set_style_bg_color(discBtn, TC(btnDefault), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(discBtn, TC(btnFocus), LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(discBtn, TC(alert), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_color(discBtn, TC(border), LV_PART_MAIN);
        lv_obj_set_style_border_width(discBtn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(discBtn, 5, LV_PART_MAIN);
        lv_obj_add_event_cb(discBtn, [](lv_event_t *e) {
            connectApDisconnectNow();
            char body[180];
            snprintf(body, sizeof(body),
                     "Disconnected from:\n%s\n\nWiFi status: %d",
                     connectApTargetSsid[0] ? connectApTargetSsid : "AP",
                     (int)WiFi.status());
            connectApShowStatusPage(LV_SYMBOL_WIFI "  Disconnected", body, false);
            playConnectApDisconnectedTone();
        }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *lbl = lv_label_create(discBtn);
        lv_label_set_text(lbl, "Disconnect");
        lv_obj_set_style_text_color(lbl, TC(text), LV_PART_MAIN);
        lv_obj_center(lbl);
    }

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    if (connected) lv_group_add_obj(wifiToolGroup, card);  // Focus card first so encoder can scroll status details.
    lv_group_add_obj(wifiToolGroup, backBtn);
    if (discBtn) lv_group_add_obj(wifiToolGroup, discBtn);
    lv_group_add_obj(wifiToolGroup, scanBtn);
    setGroup(wifiToolGroup);

    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, 3);
    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);

    if (oldWifiScreen && oldWifiScreen != wifiToolScreen) {
        keyboardQueueOldScreenDelete(oldWifiScreen, 500);
    }
}

static void connectApAttempt(const char *password) {
    char heading[52];
    snprintf(heading, sizeof(heading), LV_SYMBOL_WIFI "  %.26s", connectApTargetSsid[0] ? connectApTargetSsid : "Connect");

    // Load the temporary Connecting page before deleting the old AP list/status page.
    lv_obj_t *oldWifiScreen = wifiToolScreen;
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, heading);

    lv_obj_t *status = lv_label_create(wifiToolScreen);
    lv_label_set_text_fmt(status, LV_SYMBOL_REFRESH "  Connecting to:\n%s", connectApTargetSsid);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status, SCREEN_W - 24);
    lv_obj_set_style_text_color(status, TC(warn), LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, -10);
    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 120, 0, false);
    if (oldWifiScreen && oldWifiScreen != wifiToolScreen) {
        keyboardQueueOldScreenDelete(oldWifiScreen, 500);
    }
    lv_timer_handler();

    startLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
    WiFi.mode(WIFI_STA);

    // If we were already connected and are changing/retrying APs, suppress only the
    // temporary disconnect/drop tone caused by WiFi.disconnect() below.
    // The successful connection/reconnect tone still plays when WL_CONNECTED is reached.
    connectApSuppressDropTone(CONNECT_AP_TIMEOUT_MS + 2500);
    WiFi.disconnect(false, false);
    delay(120);
    connectApMarkConnectedState(false);

    if (connectApTargetOpen) {
        WiFi.begin(connectApTargetSsid);
    } else {
        WiFi.begin(connectApTargetSsid, password ? password : "");
    }

    uint32_t startMs = millis();
    wl_status_t st = WiFi.status();
    while ((millis() - startMs) < CONNECT_AP_TIMEOUT_MS) {
        st = WiFi.status();
        if (st == WL_CONNECTED) break;
        lv_timer_handler();
        delay(80);
    }
    stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);

    if (WiFi.status() == WL_CONNECTED) {
        bool savedPassword = false;
        if (!connectApTargetOpen && password && password[0]) {
            if (connectApUsingCachedPassword) {
                // This password already came from NVS/config, so do not write it again.
                savedPassword = true;
            } else {
                connectApSavePassword(connectApTargetSsid, password);
                savedPassword = true;
            }
        }

        String ssid = WiFi.SSID();
        String bssid = WiFi.BSSIDstr();
        String ip = WiFi.localIP().toString();
        String gw = WiFi.gatewayIP().toString();
        String dns = WiFi.dnsIP(0).toString();
        String mac = WiFi.macAddress();
        int rssi = WiFi.RSSI();
        int channel = WiFi.channel();

        bool internetReachable = false;
        const char *internetText = connectApInternetStatusText(&internetReachable);

        char body[560];
        snprintf(body, sizeof(body),
                 LV_SYMBOL_OK "  Connected Status\n\n"
                 "SSID    : %s\n"
                 "BSSID   : %s\n"
                 "RSSI    : %d dBm\n"
                 "Channel : %d\n"
                 "Local  %s\n"
                 "Gateway : %s\n"
                 "DNS     : %s\n"
                 "MAC     : %s\n"
                 "Net    %s",
                 ssid.c_str(),
                 bssid.c_str(),
                 rssi,
                 channel,
                 ip.c_str(),
                 gw.c_str(),
                 dns.c_str(),
                 mac.c_str(),
                 internetText);
        connectApShowStatusPage(LV_SYMBOL_WIFI "  Connected Status", body, true);

        // Re-enable the drop watchdog as soon as the new connection is complete.
        // This keeps the intentional switch/reconnect quiet, but still plays the
        // connected tone every time a connection succeeds.
        connectApSuppressDropToneUntilMs = 0;
        connectApMarkConnectedState(true);
        playConnectApConnectedTone();
        connectApUsingCachedPassword = false;
    } else {
        char body[260];
        snprintf(body, sizeof(body),
                 LV_SYMBOL_CLOSE "  Connection failed\n\n"
                 "SSID   : %s\n"
                 "Security: %s\n"
                 "Status : %d\n\n"
                 "Check the password or signal strength.",
                 connectApTargetSsid,
                 connectApTargetAuth[0] ? connectApTargetAuth : "Secured",
                 (int)WiFi.status());
        connectApShowStatusPage(LV_SYMBOL_WIFI "  Connect Failed", body, false);
        connectApMarkConnectedState(false);
        connectApUsingCachedPassword = false;
    }
}

static void cb_connectApPasswordDone(const char *text, bool accepted) {
    if (!accepted) {
        connectApUsingCachedPassword = false;
        createConnectAPTool();
        return;
    }
    connectApUsingCachedPassword = false;
    connectApAttempt(text ? text : "");
}

static void connectApSelect(int idx) {
    if (idx < 0 || idx >= wifiEntryCount) return;
    connectApCaptureTarget(idx);
    connectApUsingCachedPassword = false;

    if (connectApTargetOpen) {
        connectApAttempt("");
    } else {
        char savedPass[65];
        if (connectApLoadSavedPassword(connectApTargetSsid, savedPass, sizeof(savedPass))) {
            connectApUsingCachedPassword = true;
            connectApSetStatus(LV_SYMBOL_OK "  Saved password found. Connecting...", TH.success);
            lv_timer_handler();
            connectApAttempt(savedPass);
            return;
        }

        char title[48];
        snprintf(title, sizeof(title), "Password: %.27s", connectApTargetSsid);
        createKeyboardScreen(title, "", 64, cb_connectApPasswordDone);
    }
}

static void rebuildConnectApList() {
    if (!connectApList) return;

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    if (connectApBackBtn) lv_group_add_obj(wifiToolGroup, connectApBackBtn);
    if (connectApDiscBtn) lv_group_add_obj(wifiToolGroup, connectApDiscBtn);
    if (connectApScanBtn) lv_group_add_obj(wifiToolGroup, connectApScanBtn);
    setGroup(wifiToolGroup);

    lv_obj_clean(connectApList);

    if (wifiEntryCount <= 0) {
        lv_obj_t *empty = lv_label_create(connectApList);
        lv_label_set_text(empty, "No APs listed yet. Press Scan.");
        lv_obj_set_style_text_color(empty, TC(textDim), LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < wifiEntryCount; i++) {
        char ssidTrunc[17];
        strncpy(ssidTrunc, wifiEntries[i].ssid, 16);
        ssidTrunc[16] = '\0';

        char row[76];
        const char *lock = wifiEntries[i].open ? "Open" : wifiEntries[i].authStr;
        snprintf(row, sizeof(row), "%-16s %4d %s", ssidTrunc, wifiEntries[i].rssi, lock);

        lv_obj_t *btn = lv_list_add_btn(connectApList, nullptr, row);
        styleListBtn(btn);
        lv_obj_set_style_text_color(btn, rssiColor(wifiEntries[i].rssi),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            connectApSelect((int)(intptr_t)lv_event_get_user_data(ev));
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(wifiToolGroup, btn);
    }
}

static void cb_connectApScan(lv_event_t *e) {
    connectApSetStatus(LV_SYMBOL_REFRESH "  Scanning APs...", TH.warn);
    lv_timer_handler();

    startLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
    int found = doWiFiScan();
    stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);

    char status[72];
    snprintf(status, sizeof(status), LV_SYMBOL_WIFI "  %d AP%s found. Select one.",
             found, found == 1 ? "" : "s");
    connectApSetStatus(status, found > 0 ? TH.success : TH.textDim);
    rebuildConnectApList();
}

static void cb_connectApDisconnect(lv_event_t *e) {
    connectApDisconnectNow();
    connectApSetStatus(LV_SYMBOL_CLOSE "  Disconnected", TH.warn);
    rebuildConnectApList();
    playConnectApDisconnectedTone();
}


// ════════════════════════════════════════════════════════════════
//  WIFI TOOL — GATEWAY INFO / ROUTER CHECK
// ════════════════════════════════════════════════════════════════
static bool gatewayInfoConnected() {
    return (WiFi.status() == WL_CONNECTED);
}

static const char *gatewayInfoPortName(uint16_t port) {
    switch (port) {
        case 53:  return "DNS";
        case 80:  return "HTTP";
        case 443: return "HTTPS";
        default:  return "TCP";
    }
}

static void gatewayInfoSetStatus(const char *msg, uint32_t colorHex) {
    if (!gatewayInfoStatusLbl) return;
    lv_label_set_text(gatewayInfoStatusLbl, msg);
    lv_obj_set_style_text_color(gatewayInfoStatusLbl, lv_color_hex(colorHex), LV_PART_MAIN);
}

static bool gatewayInfoProbeGateway(uint16_t *openPortOut) {
#if GATEWAY_INFO_ENABLED
    if (!gatewayInfoConnected()) return false;

    IPAddress gateway = WiFi.gatewayIP();
    if (gateway == IPAddress(0, 0, 0, 0)) return false;

    WiFiClient client;
    client.setTimeout(GATEWAY_INFO_TCP_TIMEOUT_MS);

    for (int p = 0; p < GATEWAY_INFO_PORT_COUNT; p++) {
        uint16_t port = GATEWAY_INFO_PORTS[p];
        if (client.connect(gateway, port, GATEWAY_INFO_TCP_TIMEOUT_MS)) {
            client.stop();
            if (openPortOut) *openPortOut = port;
            return true;
        }
        client.stop();
        lv_timer_handler();
        delay(1);
    }
#endif
    return false;
}

static void gatewayInfoAddRow(const char *text, uint32_t colorHex = 0) {
    if (!gatewayInfoList || !text) return;
    lv_obj_t *row = lv_list_add_btn(gatewayInfoList, nullptr, text);
    styleListBtn(row);

    // Gateway Info uses a compact list because the T-Embed display is short.
    // Keep each row tight so the Back/Refresh buttons never cover the content.
    lv_obj_set_height(row, 20);
    lv_obj_set_style_pad_top(row, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, 6, LV_PART_MAIN);

    if (colorHex) {
        lv_obj_set_style_text_color(row, lv_color_hex(colorHex), LV_PART_MAIN);
    }
}

static void gatewayInfoRefresh() {
#if !GATEWAY_INFO_ENABLED
    gatewayInfoSetStatus("Gateway Info disabled in config.", TH.warn);
    return;
#else
    resetInactivityTimer();

    if (!gatewayInfoList) return;
    lv_obj_clean(gatewayInfoList);

    if (!gatewayInfoConnected()) {
        gatewayInfoSetStatus("Connect to an AP first. Gateway Info uses the active WiFi connection.", TH.warn);
        gatewayInfoAddRow("Connect to an AP first.", TH.textDim);
        if (gatewayInfoRefreshBtn) lv_obj_add_state(gatewayInfoRefreshBtn, LV_STATE_DISABLED);
        return;
    }

    if (gatewayInfoRefreshBtn) lv_obj_clear_state(gatewayInfoRefreshBtn, LV_STATE_DISABLED);

    gatewayInfoSetStatus(LV_SYMBOL_REFRESH "  Checking gateway/router...", TH.warn);
    lv_timer_handler();

    String ssid = WiFi.SSID();
    String bssid = WiFi.BSSIDstr();
    IPAddress local = WiFi.localIP();
    IPAddress gateway = WiFi.gatewayIP();
    IPAddress subnet = WiFi.subnetMask();
    IPAddress dns1 = WiFi.dnsIP(0);
    IPAddress dns2 = WiFi.dnsIP(1);
    int32_t rssi = WiFi.RSSI();
    int32_t channel = WiFi.channel();

    uint16_t gatewayPort = 0;
    bool gatewayReachable = gatewayInfoProbeGateway(&gatewayPort);
    bool internetReachable = false;
    const char *internetText = connectApInternetStatusText(&internetReachable);

    char status[192];
    // One-line horizontal marquee. Avoid newlines here so the status scrolls
    // sideways instead of vertically on the short T-Embed display.
    // Clearer one-line marquee format:
    //   TCP: OK Port 80 HTTP
    // instead of the shorter "TCP: OK HTTP" wording.
    if (gatewayReachable) {
        snprintf(status, sizeof(status),
                 LV_SYMBOL_HOME " Gateway %s  |  TCP: OK Port %u %s  |  Net: %s",
                 gateway.toString().c_str(),
                 gatewayPort,
                 gatewayInfoPortName(gatewayPort),
                 internetText);
    } else {
        snprintf(status, sizeof(status),
                 LV_SYMBOL_HOME " Gateway %s  |  TCP: No Response  |  Net: %s",
                 gateway.toString().c_str(),
                 internetText);
    }
    gatewayInfoSetStatus(status, gatewayReachable ? TH.success : TH.warn);

    char row[128];

    snprintf(row, sizeof(row), "SSID   %.24s", ssid.length() ? ssid.c_str() : "<none>");
    gatewayInfoAddRow(row, TH.text);

    snprintf(row, sizeof(row), "BSSID  %s", bssid.length() ? bssid.c_str() : "--");
    gatewayInfoAddRow(row, TH.textDim);

    snprintf(row, sizeof(row), "Signal %ld dBm %s", (long)rssi, rssiQuality((int8_t)rssi));
    gatewayInfoAddRow(row, TH.text);

    snprintf(row, sizeof(row), "Ch     %ld", (long)channel);
    gatewayInfoAddRow(row, TH.textDim);

    snprintf(row, sizeof(row), "Local  %s", local.toString().c_str());
    gatewayInfoAddRow(row, TH.text);

    snprintf(row, sizeof(row), "Gateway %s", gateway.toString().c_str());
    gatewayInfoAddRow(row, gatewayReachable ? TH.success : TH.warn);

    snprintf(row, sizeof(row), "Subnet %s", subnet.toString().c_str());
    gatewayInfoAddRow(row, TH.textDim);

    // Highlight DNS rows in yellow so router/DNS details stand out.
    snprintf(row, sizeof(row), "DNS1   %s", dns1.toString().c_str());
    gatewayInfoAddRow(row, TH.warn);

    snprintf(row, sizeof(row), "DNS2   %s", dns2.toString().c_str());
    gatewayInfoAddRow(row, TH.warn);

    if (gatewayReachable) {
        snprintf(row, sizeof(row), "Router TCP Port %u %s", gatewayPort, gatewayInfoPortName(gatewayPort));
        gatewayInfoAddRow(row, TH.success);
    } else {
        gatewayInfoAddRow("Router TCP no response", TH.warn);
    }

    snprintf(row, sizeof(row), "Net    %s", internetText);
    gatewayInfoAddRow(row, internetReachable ? TH.success : TH.warn);

    snprintf(row, sizeof(row), "STA MAC %s", WiFi.macAddress().c_str());
    gatewayInfoAddRow(row, TH.textDim);

    lv_timer_handler();
#endif
}

static void cb_gatewayInfoRefresh(lv_event_t *e) {
    playMenuClickFeedback();
    gatewayInfoRefresh();
}

void createGatewayInfo() {
#if !GATEWAY_INFO_ENABLED
    createSubScreen(0);
    return;
#else
    lv_obj_t *oldWifiScreen = wifiToolScreen;
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_HOME "  Gateway Info");

    gatewayInfoStatusLbl = lv_label_create(wifiToolScreen);
    lv_label_set_long_mode(gatewayInfoStatusLbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(gatewayInfoStatusLbl, SCREEN_W - 14);
    lv_obj_set_height(gatewayInfoStatusLbl, 18);
    // LVGL on this build supports animation time, not animation speed.
    // Keep the config value acting like a speed: higher = faster scroll.
    uint32_t gwScrollReduceMs = (uint32_t)GATEWAY_INFO_STATUS_SCROLL_SPEED * 120UL;
    if (gwScrollReduceMs > 8200UL) gwScrollReduceMs = 8200UL;
    uint32_t gwScrollTimeMs = 9000UL - gwScrollReduceMs;
    lv_obj_set_style_anim_time(gatewayInfoStatusLbl, gwScrollTimeMs, LV_PART_MAIN);
    lv_obj_set_pos(gatewayInfoStatusLbl, 7, 30);

    gatewayInfoList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(gatewayInfoList, SCREEN_W - 8, SCREEN_H - 88);
    lv_obj_set_pos(gatewayInfoList, 4, 50);
    lv_obj_set_style_bg_color(gatewayInfoList,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(gatewayInfoList,       LV_OPA_COVER,        LV_PART_MAIN);
    lv_obj_set_style_border_width(gatewayInfoList, 0,                   LV_PART_MAIN);
    lv_obj_set_style_pad_all(gatewayInfoList,      2,                   LV_PART_MAIN);
    lv_obj_set_style_pad_row(gatewayInfoList,      2,                   LV_PART_MAIN);
    lv_obj_set_style_bg_color(gatewayInfoList, lv_color_hex(TH.accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(gatewayInfoList, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(gatewayInfoList, 4, LV_PART_SCROLLBAR);

    gatewayInfoBackBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    lv_obj_set_size(gatewayInfoBackBtn, 100, 26);
    lv_obj_align(gatewayInfoBackBtn, LV_ALIGN_BOTTOM_LEFT, 6, -4);

    gatewayInfoRefreshBtn = createActionBtn(wifiToolScreen, LV_SYMBOL_REFRESH " Refresh", cb_gatewayInfoRefresh);
    lv_obj_set_size(gatewayInfoRefreshBtn, 112, 26);
    lv_obj_align(gatewayInfoRefreshBtn, LV_ALIGN_BOTTOM_RIGHT, -6, -4);

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, gatewayInfoBackBtn);
    lv_group_add_obj(wifiToolGroup, gatewayInfoRefreshBtn);
    lv_group_add_obj(wifiToolGroup, gatewayInfoList);
    setGroup(wifiToolGroup);

    if (!gatewayInfoConnected()) {
        lv_obj_add_state(gatewayInfoRefreshBtn, LV_STATE_DISABLED);
    }

    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, 3);
    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);

    gatewayInfoRefresh();

    if (oldWifiScreen && oldWifiScreen != wifiToolScreen) {
        keyboardQueueOldScreenDelete(oldWifiScreen, 500);
    }

    lv_timer_handler();
#endif
}


// ════════════════════════════════════════════════════════════════
//  WIFI TOOL — LAN HOST DISCOVERY
// ════════════════════════════════════════════════════════════════
static bool lanDiscoveryConnected() {
    return (WiFi.status() == WL_CONNECTED);
}

static uint32_t lanDiscoveryIpToU32(IPAddress ip) {
    return ((uint32_t)ip[0] << 24) |
           ((uint32_t)ip[1] << 16) |
           ((uint32_t)ip[2] << 8)  |
           (uint32_t)ip[3];
}

static IPAddress lanDiscoveryU32ToIp(uint32_t value) {
    return IPAddress((uint8_t)((value >> 24) & 0xFF),
                     (uint8_t)((value >> 16) & 0xFF),
                     (uint8_t)((value >> 8) & 0xFF),
                     (uint8_t)(value & 0xFF));
}

static IPAddress lanDiscoveryNetworkBase(IPAddress ip, IPAddress mask) {
    return lanDiscoveryU32ToIp(lanDiscoveryIpToU32(ip) & lanDiscoveryIpToU32(mask));
}

static bool lanDiscoveryBuildRange(IPAddress local,
                                   IPAddress mask,
                                   uint32_t *firstOut,
                                   uint32_t *lastOut,
                                   uint32_t *networkOut,
                                   uint32_t *broadcastOut) {
    uint32_t ipNum = lanDiscoveryIpToU32(local);
    uint32_t maskNum = lanDiscoveryIpToU32(mask);

    // If DHCP did not give us a useful subnet mask, fall back to a /24
    // around the connected local IP instead of assuming the last octet only.
    if (maskNum == 0 || maskNum == 0xFFFFFFFFUL) {
        maskNum = 0xFFFFFF00UL;
    }

    uint32_t networkNum = ipNum & maskNum;
    uint32_t broadcastNum = networkNum | (~maskNum);

    // Need at least one usable host between network and broadcast.
    if (broadcastNum <= (networkNum + 1)) {
        return false;
    }

    uint32_t usableFirst = networkNum + 1;
    uint32_t usableLast = broadcastNum - 1;

    int startHost = LAN_DISCOVERY_START_HOST;
    if (startHost < 1) startHost = 1;

    // Keep the existing config meaning, but apply it relative to the actual
    // network base calculated from WiFi.localIP() and WiFi.subnetMask().
    uint32_t configuredFirst = networkNum + (uint32_t)startHost;
    if (configuredFirst > usableFirst && configuredFirst <= usableLast) {
        usableFirst = configuredFirst;
    }

    int maxHosts = LAN_DISCOVERY_MAX_HOSTS;
    if (maxHosts < 1) maxHosts = 1;

    uint32_t configuredLast = usableFirst + (uint32_t)maxHosts - 1;
    if (configuredLast < usableFirst || configuredLast > usableLast) {
        configuredLast = usableLast;
    }

    if (usableFirst > configuredLast) {
        return false;
    }

    if (firstOut) *firstOut = usableFirst;
    if (lastOut) *lastOut = configuredLast;
    if (networkOut) *networkOut = networkNum;
    if (broadcastOut) *broadcastOut = broadcastNum;
    return true;
}

static const char *lanDiscoveryPortName(uint16_t port) {
    switch (port) {
        case 22:  return "SSH";
        case 53:  return "DNS";
        case 80:  return "HTTP";
        case 443: return "HTTPS";
        case 8080:return "HTTP-Alt";
        default:  return "TCP";
    }
}

static bool lanDiscoveryProbe(IPAddress ip, uint16_t *openPortOut) {
    WiFiClient client;

    for (int p = 0; p < LAN_DISCOVERY_PORT_COUNT; p++) {
        uint16_t port = LAN_DISCOVERY_PORTS[p];

        if (client.connect(ip, port, LAN_DISCOVERY_TCP_TIMEOUT_MS)) {
            client.stop();
            if (openPortOut) *openPortOut = port;
            return true;
        }

        client.stop();
        lv_timer_handler();
        delay(1);
    }

    return false;
}

static void lanDiscoverySetStatus(const char *msg, uint32_t colorHex) {
    if (!lanDiscoveryStatusLbl) return;
    lv_label_set_text(lanDiscoveryStatusLbl, msg);
    lv_obj_set_style_text_color(lanDiscoveryStatusLbl, lv_color_hex(colorHex), LV_PART_MAIN);
}

static void lanDiscoveryRunScan() {
#if !LAN_DISCOVERY_ENABLED
    lanDiscoverySetStatus("LAN Host Discovery disabled in config.", TH.warn);
    return;
#else
    if (!lanDiscoveryConnected()) {
        lanDiscoverySetStatus("Connect to an AP first, then run LAN Host Discovery.", TH.warn);
        return;
    }

    if (!lanDiscoveryList) return;

    resetInactivityTimer();

    // Make the page feel alive while the TCP probes are running. The scan is
    // still a blocking loop, but the LED task runs on core 0 and the UI gets
    // periodic lv_timer_handler() calls below.
#if LAN_DISCOVERY_RAINBOW_LED_ENABLED
    startLEDRainbowSpinner(LAN_DISCOVERY_RAINBOW_LED_DELAY_MS);
#else
    startLEDSpinner(0, 200, 0, 80);
#endif

    if (lanDiscoveryScanBtn) lv_obj_add_state(lanDiscoveryScanBtn, LV_STATE_DISABLED);
    if (lanDiscoveryBackBtn) lv_obj_add_state(lanDiscoveryBackBtn, LV_STATE_DISABLED);

    lv_obj_clean(lanDiscoveryList);

    IPAddress local = WiFi.localIP();
    IPAddress gateway = WiFi.gatewayIP();
    IPAddress mask = WiFi.subnetMask();

    uint32_t firstHostNum = 0;
    uint32_t lastHostNum = 0;
    uint32_t networkNum = 0;
    uint32_t broadcastNum = 0;

    if (!lanDiscoveryBuildRange(local, mask, &firstHostNum, &lastHostNum, &networkNum, &broadcastNum)) {
        lanDiscoverySetStatus("Could not build LAN range from IP/subnet.", TH.alert);
#if LAN_DISCOVERY_RAINBOW_LED_ENABLED
        stopLEDRainbowSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, 3);
#else
        stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
#endif
        if (lanDiscoveryBackBtn) lv_obj_clear_state(lanDiscoveryBackBtn, LV_STATE_DISABLED);
        if (lanDiscoveryScanBtn) lv_obj_clear_state(lanDiscoveryScanBtn, LV_STATE_DISABLED);
        return;
    }

    IPAddress firstIp = lanDiscoveryU32ToIp(firstHostNum);
    IPAddress lastIp = lanDiscoveryU32ToIp(lastHostNum);
    IPAddress networkIp = lanDiscoveryU32ToIp(networkNum);

    char status[192];
    snprintf(status, sizeof(status),
             "Scanning LAN...\nLocal: %s\nRange: %s-%s",
             local.toString().c_str(),
             firstIp.toString().c_str(),
             lastIp.toString().c_str());
    lanDiscoverySetStatus(status, TH.warn);
    lv_timer_handler();

    int found = 0;
    int scanned = 0;

    // Probe gateway first because it is the most useful/likely live target.
    bool gatewayWasProbed = false;
    if (gateway != IPAddress(0, 0, 0, 0) && gateway != local) {
        uint32_t gatewayNum = lanDiscoveryIpToU32(gateway);
        if (gatewayNum >= networkNum && gatewayNum <= broadcastNum) {
            gatewayWasProbed = true;
            uint16_t openPort = 0;
            if (lanDiscoveryProbe(gateway, &openPort)) {
                char row[96];
                snprintf(row, sizeof(row), "%s   gateway   %u/%s",
                         gateway.toString().c_str(),
                         openPort,
                         lanDiscoveryPortName(openPort));
                lv_obj_t *btn = lv_list_add_btn(lanDiscoveryList, nullptr, row);
                styleListBtn(btn);
                lv_obj_set_height(btn, 28);
                lv_obj_set_style_text_color(btn, TC(success), LV_PART_MAIN);
                found++;
            }
        }
    }

    for (uint32_t ipNum = firstHostNum; ipNum <= lastHostNum && found < LAN_DISCOVERY_MAX_RESULTS; ipNum++) {
        IPAddress target = lanDiscoveryU32ToIp(ipNum);

        if (target == local ||
            target == IPAddress(0, 0, 0, 0) ||
            target == IPAddress(255, 255, 255, 255) ||
            (gatewayWasProbed && target == gateway)) {
            continue;
        }

        scanned++;

        uint16_t openPort = 0;
        if (lanDiscoveryProbe(target, &openPort)) {
            char row[96];
            snprintf(row, sizeof(row), "%s   %u/%s",
                     target.toString().c_str(),
                     openPort,
                     lanDiscoveryPortName(openPort));
            lv_obj_t *btn = lv_list_add_btn(lanDiscoveryList, nullptr, row);
            styleListBtn(btn);
            lv_obj_set_height(btn, 28);
            lv_obj_set_style_text_color(btn, TC(text), LV_PART_MAIN);
            found++;
        }

        if ((scanned % 8) == 0) {
            snprintf(status, sizeof(status),
                     "Scanning LAN... %d checked\nFound: %d\nNetwork: %s/%s",
                     scanned,
                     found,
                     networkIp.toString().c_str(),
                     mask.toString().c_str());
            lanDiscoverySetStatus(status, TH.warn);
            lv_timer_handler();
        }
    }

    if (found == 0) {
        lv_obj_t *row = lv_list_add_btn(lanDiscoveryList, nullptr,
                                        "No hosts found by TCP probe.");
        styleListBtn(row);
        lv_obj_set_height(row, 28);
        lv_obj_set_style_text_color(row, TC(textDim), LV_PART_MAIN);
    }

    snprintf(status, sizeof(status),
             LV_SYMBOL_OK "  LAN scan complete\nChecked: %d hosts\nFound: %d",
             scanned,
             found);
    lanDiscoverySetStatus(status, found > 0 ? TH.success : TH.textDim);

#if LAN_DISCOVERY_RAINBOW_LED_ENABLED
    stopLEDRainbowSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, 3);
#else
    stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
#endif

    playLanDiscoveryDoneTone();

    if (lanDiscoveryBackBtn) lv_obj_clear_state(lanDiscoveryBackBtn, LV_STATE_DISABLED);
    if (lanDiscoveryScanBtn) lv_obj_clear_state(lanDiscoveryScanBtn, LV_STATE_DISABLED);
    lv_timer_handler();
#endif
}

static void cb_lanDiscoveryRescan(lv_event_t *e) {
    resetInactivityTimer();
    playMenuClickFeedback();
    lanDiscoveryRunScan();
}

void createLANHostDiscovery() {
#if !LAN_DISCOVERY_ENABLED
    createSubScreen(0);
    return;
#else
    lv_obj_t *oldWifiScreen = wifiToolScreen;
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_EYE_OPEN "  LAN Host Discovery");

    lanDiscoveryStatusLbl = lv_label_create(wifiToolScreen);
    lv_label_set_long_mode(lanDiscoveryStatusLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lanDiscoveryStatusLbl, SCREEN_W - 14);
    lv_obj_set_pos(lanDiscoveryStatusLbl, 7, 30);

    if (lanDiscoveryConnected()) {
        char msg[150];
        snprintf(msg, sizeof(msg),
                 "Ready. Local: %s\nGateway %s",
                 WiFi.localIP().toString().c_str(),
                 WiFi.gatewayIP().toString().c_str());
        lanDiscoverySetStatus(msg, TH.text);
    } else {
        lanDiscoverySetStatus("Connect to an AP first. This tool uses the active LAN connection.", TH.warn);
    }

    lanDiscoveryList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(lanDiscoveryList, SCREEN_W, SCREEN_H - 98);
    lv_obj_set_pos(lanDiscoveryList, 0, 64);
    lv_obj_set_style_bg_color(lanDiscoveryList,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lanDiscoveryList,       LV_OPA_COVER,        LV_PART_MAIN);
    lv_obj_set_style_border_width(lanDiscoveryList, 0,                   LV_PART_MAIN);
    lv_obj_set_style_pad_all(lanDiscoveryList,      3,                   LV_PART_MAIN);
    lv_obj_set_style_pad_row(lanDiscoveryList,      3,                   LV_PART_MAIN);
    lv_obj_set_style_bg_color(lanDiscoveryList, lv_color_hex(TH.accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(lanDiscoveryList, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(lanDiscoveryList, 4, LV_PART_SCROLLBAR);

    lv_obj_t *hintRow = lv_list_add_btn(lanDiscoveryList, nullptr,
                                        lanDiscoveryConnected()
                                            ? "Press Scan to start LAN discovery."
                                            : "Connect to an AP first.");
    styleListBtn(hintRow);
    lv_obj_set_height(hintRow, 28);
    lv_obj_set_style_text_color(hintRow, TC(textDim), LV_PART_MAIN);

    lanDiscoveryBackBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    lv_obj_set_size(lanDiscoveryBackBtn, 100, 26);
    lv_obj_align(lanDiscoveryBackBtn, LV_ALIGN_BOTTOM_LEFT, 6, -4);

    lanDiscoveryScanBtn = createActionBtn(wifiToolScreen, LV_SYMBOL_REFRESH " Scan", cb_lanDiscoveryRescan);
    lv_obj_set_size(lanDiscoveryScanBtn, 100, 26);
    lv_obj_align(lanDiscoveryScanBtn, LV_ALIGN_BOTTOM_RIGHT, -6, -4);

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, lanDiscoveryBackBtn);
    lv_group_add_obj(wifiToolGroup, lanDiscoveryScanBtn);
    lv_group_add_obj(wifiToolGroup, lanDiscoveryList);
    setGroup(wifiToolGroup);

    if (!lanDiscoveryConnected()) {
        lv_obj_add_state(lanDiscoveryScanBtn, LV_STATE_DISABLED);
    }

    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, 3);
    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);

    if (oldWifiScreen && oldWifiScreen != wifiToolScreen) {
        keyboardQueueOldScreenDelete(oldWifiScreen, 500);
    }

    lv_timer_handler();
#endif
}


void createConnectAPTool() {
    lv_obj_t *oldWifiScreen = wifiToolScreen;
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_WIFI "  Connect to AP");

    connectApStatusLbl = lv_label_create(wifiToolScreen);
    if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        char status[96];
        snprintf(status, sizeof(status), LV_SYMBOL_OK "  Connected: %.20s  %s",
                 WiFi.SSID().c_str(), ip.c_str());
        lv_label_set_text(connectApStatusLbl, status);
        lv_obj_set_style_text_color(connectApStatusLbl, TC(success), LV_PART_MAIN);
    } else {
        lv_label_set_text(connectApStatusLbl, "Press Scan, select AP, then enter password.");
        lv_obj_set_style_text_color(connectApStatusLbl, TC(textDim), LV_PART_MAIN);
    }
    lv_label_set_long_mode(connectApStatusLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(connectApStatusLbl, SCREEN_W - 14);
    lv_obj_set_pos(connectApStatusLbl, 7, 30);

    connectApList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(connectApList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(connectApList, 0, 48);
    lv_obj_set_style_bg_color(connectApList,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(connectApList,       LV_OPA_COVER,        LV_PART_MAIN);
    lv_obj_set_style_border_width(connectApList, 0,                   LV_PART_MAIN);
    lv_obj_set_style_pad_all(connectApList,      2,                   LV_PART_MAIN);
    lv_obj_set_style_pad_row(connectApList,      2,                   LV_PART_MAIN);
    lv_obj_set_style_bg_color(connectApList, lv_color_hex(TH.accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(connectApList, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(connectApList, 4, LV_PART_SCROLLBAR);

    connectApBackBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    lv_obj_set_size(connectApBackBtn, 86, 26);
    lv_obj_align(connectApBackBtn, LV_ALIGN_BOTTOM_LEFT, 6, -4);

    connectApScanBtn = createActionBtn(wifiToolScreen, LV_SYMBOL_REFRESH " Scan", cb_connectApScan);
    lv_obj_set_size(connectApScanBtn, 86, 26);
    lv_obj_align(connectApScanBtn, LV_ALIGN_BOTTOM_RIGHT, -6, -4);

    connectApDiscBtn = lv_btn_create(wifiToolScreen);
    lv_obj_set_size(connectApDiscBtn, 108, 26);
    lv_obj_align(connectApDiscBtn, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(connectApDiscBtn, TC(btnDefault), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(connectApDiscBtn, TC(btnFocus), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(connectApDiscBtn, TC(alert), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(connectApDiscBtn, TC(border), LV_PART_MAIN);
    lv_obj_set_style_border_width(connectApDiscBtn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(connectApDiscBtn, 5, LV_PART_MAIN);
    lv_obj_add_event_cb(connectApDiscBtn, cb_connectApDisconnect, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *discLbl = lv_label_create(connectApDiscBtn);
    lv_label_set_text(discLbl, "Disconnect");
    lv_obj_set_style_text_color(discLbl, TC(text), LV_PART_MAIN);
    lv_obj_center(discLbl);

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, connectApBackBtn);
    lv_group_add_obj(wifiToolGroup, connectApDiscBtn);
    lv_group_add_obj(wifiToolGroup, connectApScanBtn);
    setGroup(wifiToolGroup);

    rebuildConnectApList();

    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, 3);
    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);

    if (oldWifiScreen && oldWifiScreen != wifiToolScreen) {
        keyboardQueueOldScreenDelete(oldWifiScreen, 500);
    }
}


// ════════════════════════════════════════════════════════════════
//  TOOL 3 – STATION SCANNER
//
//  Passive client/station scanner inspired by GhostESP station scan logic.
//  It watches management + data frames, deduplicates station/AP pairs,
//  and displays station MAC, RSSI, channel, age, frame type, AP/BSSID,
//  and packet count. No deauth/injection; monitor-only.
// ════════════════════════════════════════════════════════════════
static bool stationIsBroadcastOrMulticast(const uint8_t *mac) {
    if (!mac) return true;
    if (mac[0] & 0x01) return true;
    static const uint8_t ff[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    return memcmp(mac, ff, 6) == 0;
}

static void stationMacToStr(const uint8_t *mac, char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char *stationMgmtSubtypeName(uint8_t subtype) {
    switch (subtype) {
        case 0x0: return "Assoc Req";
        case 0x1: return "Assoc Resp";
        case 0x2: return "Reassoc Req";
        case 0x3: return "Reassoc Resp";
        case 0xA: return "Disassoc";
        case 0xB: return "Auth";
        case 0xC: return "Deauth";
        case 0xD: return "Action";
        default:  return "Mgmt";
    }
}

static bool stationRelevantMgmtSubtype(uint8_t subtype) {
    switch (subtype) {
        case 0x0: // Assoc Request
        case 0x1: // Assoc Response
        case 0x2: // Reassoc Request
        case 0x3: // Reassoc Response
        case 0xA: // Disassociation
        case 0xB: // Authentication
        case 0xC: // Deauthentication
        case 0xD: // Action
            return true;
        default:
            return false;
    }
}


static int stationFindApByBssid(const char *bssid) {
    if (!bssid || strcmp(bssid, "--") == 0) return -1;
    int count = stationApCount;
    if (count < 0) count = 0;
    if (count > MAX_STATION_APS) count = MAX_STATION_APS;
    for (int i = 0; i < count; i++) {
        if (strncmp(stationAps[i].bssid, bssid, 18) == 0) return i;
    }
    return -1;
}

static void stationAddOrUpdateAp(const uint8_t *bssid, const char *ssid,
                                 const char *auth, int8_t rssi, uint8_t channel) {
    if (!bssid || stationIsBroadcastOrMulticast(bssid)) return;

    char bssidStr[18];
    stationMacToStr(bssid, bssidStr);

    int idx = stationFindApByBssid(bssidStr);
    if (idx < 0) {
        int count = stationApCount;
        if (count >= MAX_STATION_APS) return;
        idx = count;
        strncpy(stationAps[idx].bssid, bssidStr, sizeof(stationAps[idx].bssid) - 1);
        stationAps[idx].bssid[sizeof(stationAps[idx].bssid) - 1] = '\0';
        stationAps[idx].clientCount = 0;
        stationAps[idx].eapolPackets = 0;
        stationApCount = count + 1;
    }

    if (ssid && ssid[0]) {
        strncpy(stationAps[idx].ssid, ssid, sizeof(stationAps[idx].ssid) - 1);
        stationAps[idx].ssid[sizeof(stationAps[idx].ssid) - 1] = '\0';
    } else if (stationAps[idx].ssid[0] == '\0') {
        strncpy(stationAps[idx].ssid, "<hidden>", sizeof(stationAps[idx].ssid) - 1);
        stationAps[idx].ssid[sizeof(stationAps[idx].ssid) - 1] = '\0';
    }

    if (auth && auth[0]) {
        strncpy(stationAps[idx].authStr, auth, sizeof(stationAps[idx].authStr) - 1);
        stationAps[idx].authStr[sizeof(stationAps[idx].authStr) - 1] = '\0';
    } else if (stationAps[idx].authStr[0] == '\0') {
        strncpy(stationAps[idx].authStr, "Unknown", sizeof(stationAps[idx].authStr) - 1);
        stationAps[idx].authStr[sizeof(stationAps[idx].authStr) - 1] = '\0';
    }

    stationAps[idx].rssi = rssi;
    stationAps[idx].channel = channel;
    stationAps[idx].lastSeenMs = millis();
}

static void stationParseApFrame(const uint8_t *d, uint16_t len, int8_t rssi, uint8_t channel) {
    // Beacon / probe response layout: 24-byte MAC header, 12-byte fixed params, then tagged params.
    if (!d || len < 36) return;

    const uint8_t *bssid = d + 16;  // addr3 is the BSSID for beacon/probe response.
    if (stationIsBroadcastOrMulticast(bssid)) return;

    uint16_t cap = (uint16_t)d[34] | ((uint16_t)d[35] << 8);
    bool privacy = (cap & 0x0010) != 0;
    bool sawRsn = false;
    bool sawWpa = false;
    bool sawSae = false;
    char ssid[33];
    ssid[0] = '\0';

    uint16_t pos = 36;
    while ((pos + 2) <= len) {
        uint8_t tag = d[pos++];
        uint8_t tlvLen = d[pos++];
        if ((pos + tlvLen) > len) break;

        if (tag == 0) { // SSID
            uint8_t copyLen = tlvLen;
            if (copyLen > 32) copyLen = 32;
            if (copyLen > 0) {
                memcpy(ssid, d + pos, copyLen);
                ssid[copyLen] = '\0';
                for (uint8_t i = 0; i < copyLen; i++) {
                    if ((uint8_t)ssid[i] < 32 || (uint8_t)ssid[i] > 126) ssid[i] = '?';
                }
            }
        } else if (tag == 48) { // RSN
            sawRsn = true;
            // Look for SAE auth suite selector 00-0F-AC:08 as a WPA3 hint.
            for (uint8_t i = 0; (i + 3) < tlvLen; i++) {
                if (d[pos + i] == 0x00 && d[pos + i + 1] == 0x0F &&
                    d[pos + i + 2] == 0xAC && d[pos + i + 3] == 0x08) {
                    sawSae = true;
                    break;
                }
            }
        } else if (tag == 221 && tlvLen >= 4) { // Vendor specific
            // WPA vendor OUI: 00:50:F2:01
            if (d[pos] == 0x00 && d[pos + 1] == 0x50 &&
                d[pos + 2] == 0xF2 && d[pos + 3] == 0x01) {
                sawWpa = true;
            }
        }

        pos += tlvLen;
    }

    const char *auth = "Open";
    if (sawSae) auth = "WPA3";
    else if (sawRsn && sawWpa) auth = "WPA/2";
    else if (sawRsn) auth = "WPA2";
    else if (sawWpa) auth = "WPA";
    else if (privacy) auth = "WEP/Sec";

    if (ssid[0] == '\0') strncpy(ssid, "<hidden>", sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';

    stationAddOrUpdateAp(bssid, ssid, auth, rssi, channel);
}

static bool stationIsEapolFrame(const uint8_t *d, uint16_t len, uint8_t subtype,
                                bool toDS, bool fromDS) {
    if (!d || len < 32) return false;

    uint16_t hdrLen = 24;
    if (toDS && fromDS) hdrLen = 30;          // Four-address WDS frame.
    if (subtype & 0x08) hdrLen += 2;          // QoS data adds control field.

    if ((hdrLen + 8) > len) return false;

    const uint8_t *llc = d + hdrLen;
    return (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
            llc[6] == 0x88 && llc[7] == 0x8E);
}

static void stationRefreshApStats() {
    int apCount = stationApCount;
    if (apCount < 0) apCount = 0;
    if (apCount > MAX_STATION_APS) apCount = MAX_STATION_APS;

    for (int i = 0; i < apCount; i++) {
        stationAps[i].clientCount = 0;
        stationAps[i].eapolPackets = 0;
    }

    int stCount = stationEntryCount;
    if (stCount < 0) stCount = 0;
    if (stCount > MAX_STATION_RESULTS) stCount = MAX_STATION_RESULTS;

    for (int i = 0; i < stCount; i++) {
        int apIdx = stationFindApByBssid(stationEntries[i].apBssid);
        if (apIdx >= 0) {
            if (stationAps[apIdx].clientCount < 65535) stationAps[apIdx].clientCount++;
            if (stationAps[apIdx].eapolPackets < 65535 - stationEntries[i].eapolPackets) {
                stationAps[apIdx].eapolPackets += stationEntries[i].eapolPackets;
            } else {
                stationAps[apIdx].eapolPackets = 65535;
            }
        }
    }
}

static const char *stationApSsidForBssid(const char *bssid) {
    int idx = stationFindApByBssid(bssid);
    if (idx < 0 || stationAps[idx].ssid[0] == '\0') return "Unknown";
    return stationAps[idx].ssid;
}

static const char *stationApAuthForBssid(const char *bssid) {
    int idx = stationFindApByBssid(bssid);
    if (idx < 0 || stationAps[idx].authStr[0] == '\0') return "Unknown";
    return stationAps[idx].authStr;
}


static void stationAddOrUpdate(const uint8_t *sta, const uint8_t *ap,
                               const char *frameName, int8_t rssi, uint8_t channel,
                               bool eapolSeen) {
    if (!sta || stationIsBroadcastOrMulticast(sta)) return;

    char staStr[18];
    char apStr[18];
    stationMacToStr(sta, staStr);

    bool apKnown = (ap && !stationIsBroadcastOrMulticast(ap) && memcmp(sta, ap, 6) != 0);
    if (apKnown) stationMacToStr(ap, apStr);
    else strncpy(apStr, "--", sizeof(apStr));
    apStr[sizeof(apStr)-1] = '\0';

    int count = stationEntryCount;
    for (int i = 0; i < count; i++) {
        if (strncmp(stationEntries[i].stationMac, staStr, 18) == 0 &&
            strncmp(stationEntries[i].apBssid, apStr, 18) == 0) {
            stationEntries[i].rssi = rssi;
            stationEntries[i].channel = channel;
            stationEntries[i].lastSeenMs = millis();
            if (stationEntries[i].packets < 65535) stationEntries[i].packets++;
            if (eapolSeen) {
                stationEntries[i].eapolSeen = true;
                if (stationEntries[i].eapolPackets < 65535) stationEntries[i].eapolPackets++;
                if (stationEapolTotal < 65535) stationEapolTotal++;
            }
            strncpy(stationEntries[i].frameType, frameName, sizeof(stationEntries[i].frameType) - 1);
            stationEntries[i].frameType[sizeof(stationEntries[i].frameType) - 1] = '\0';
            return;
        }
    }

    if (count >= MAX_STATION_RESULTS) return;
    int idx = count;
    strncpy(stationEntries[idx].stationMac, staStr, sizeof(stationEntries[idx].stationMac) - 1);
    stationEntries[idx].stationMac[sizeof(stationEntries[idx].stationMac) - 1] = '\0';
    strncpy(stationEntries[idx].apBssid, apStr, sizeof(stationEntries[idx].apBssid) - 1);
    stationEntries[idx].apBssid[sizeof(stationEntries[idx].apBssid) - 1] = '\0';
    strncpy(stationEntries[idx].frameType, frameName, sizeof(stationEntries[idx].frameType) - 1);
    stationEntries[idx].frameType[sizeof(stationEntries[idx].frameType) - 1] = '\0';
    stationEntries[idx].rssi = rssi;
    stationEntries[idx].channel = channel;
    stationEntries[idx].packets = 1;
    stationEntries[idx].eapolPackets = eapolSeen ? 1 : 0;
    stationEntries[idx].eapolSeen = eapolSeen;
    if (eapolSeen && stationEapolTotal < 65535) stationEapolTotal++;
    stationEntries[idx].lastSeenMs = millis();
    stationEntryCount = count + 1;
}

static void station_scan_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!stationScanActive) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    if (!buf) return;

    const wifi_promiscuous_pkt_t *pkt = reinterpret_cast<const wifi_promiscuous_pkt_t *>(buf);
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;

    const uint8_t *d = pkt->payload;
    uint16_t fc = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
    uint8_t frameType = (fc >> 2) & 0x03;
    uint8_t subtype   = (fc >> 4) & 0x0F;
    bool toDS         = (fc & 0x0100) != 0;
    bool fromDS       = (fc & 0x0200) != 0;

    const uint8_t *addr1 = d + 4;
    const uint8_t *addr2 = d + 10;
    const uint8_t *addr3 = d + 16;
    const uint8_t *sta = nullptr;
    const uint8_t *ap  = nullptr;
    const char *frameName = "Other";
    bool eapolSeen = false;

    if (frameType == 2) { // Data frame
        eapolSeen = stationIsEapolFrame(d, len, subtype, toDS, fromDS);

        if (toDS && !fromDS) {
            sta = addr2; ap = addr1; frameName = eapolSeen ? "EAPOL STA>AP" : "Data STA>AP";
        } else if (!toDS && fromDS) {
            sta = addr1; ap = addr2; frameName = eapolSeen ? "EAPOL AP>STA" : "Data AP>STA";
        } else if (!toDS && !fromDS) {
            sta = addr2; ap = addr3; frameName = eapolSeen ? "EAPOL" : "Data";
        } else {
            return; // WDS / four-address data path; skip for first pass
        }
    } else if (frameType == 0) { // Management frame
        // Track APs from beacon/probe response frames so Station Detail can show SSID/security.
        if (subtype == 0x8 || subtype == 0x5) {
            stationParseApFrame(d, len, pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel);
            return;
        }

        if (!stationRelevantMgmtSubtype(subtype)) return;
        frameName = stationMgmtSubtypeName(subtype);

        // AP -> Station management responses often have addr2 == addr3.
        // Station -> AP requests often have addr2 as station and addr3 as BSSID.
        if (!stationIsBroadcastOrMulticast(addr1) && memcmp(addr2, addr3, 6) == 0) {
            sta = addr1; ap = addr2;
        } else {
            sta = addr2; ap = addr3;
        }
    } else {
        return;
    }

    if (!sta || stationIsBroadcastOrMulticast(sta)) return;
    if (ap && memcmp(sta, ap, 6) == 0) ap = nullptr;

    stationAddOrUpdate(sta, ap, frameName, pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel, eapolSeen);
}

static int stationFocusedRowIndex() {
    if (!wifiToolGroup) return -1;
    lv_obj_t *focused = lv_group_get_focused(wifiToolGroup);
    if (!focused) return -1;
    for (int i = 0; i < stationRowCount; i++) {
        if (stationRowBtns[i] == focused) return i;
    }
    return -1;
}

static void stationClearRows() {
    for (int i = 0; i < MAX_STATION_RESULTS; i++) {
        if (stationRowBtns[i]) {
            lv_obj_delete(stationRowBtns[i]);
            stationRowBtns[i] = nullptr;
            stationRowLabels[i] = nullptr;
        }
    }
    stationRowCount = 0;
    if (stationEmptyLbl) {
        lv_obj_delete(stationEmptyLbl);
        stationEmptyLbl = nullptr;
    }
}

static void stationSetEmptyMessage(bool show) {
    if (!stationList) return;
    if (show) {
        if (!stationEmptyLbl) {
            stationEmptyLbl = lv_list_add_text(stationList, "No stations yet...");
            if (stationEmptyLbl) {
                lv_obj_set_style_text_color(stationEmptyLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
            }
        }
    } else if (stationEmptyLbl) {
        lv_obj_delete(stationEmptyLbl);
        stationEmptyLbl = nullptr;
    }
}

static void stationRefreshList() {
    if (!stationList) return;
    stationRefreshApStats();

    // Preserve station-row focus while updating the visible text in-place.
    int focusedRow = stationFocusedRowIndex();
    int count = stationEntryCount;
    if (count < 0) count = 0;
    if (count > MAX_STATION_RESULTS) count = MAX_STATION_RESULTS;

    if (count <= 0) {
        if (stationRowCount > 0) stationClearRows();
        stationSetEmptyMessage(true);
        return;
    }

    stationSetEmptyMessage(false);

    // Shrink only when needed. Deleting rows can disturb focus, so do it only
    // when the result count actually drops, such as when a new scan starts.
    while (stationRowCount > count) {
        int i = stationRowCount - 1;
        if (stationRowBtns[i]) lv_obj_delete(stationRowBtns[i]);
        stationRowBtns[i] = nullptr;
        stationRowLabels[i] = nullptr;
        stationRowCount--;
    }

    // Grow only when new stations appear. Existing buttons stay alive so the
    // encoder can remain focused on the same result across refresh ticks.
    while (stationRowCount < count) {
        int i = stationRowCount;
        lv_obj_t *btn = lv_list_add_btn(stationList, nullptr, "");
        styleListBtn(btn);
        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            createStationDetail((int)(intptr_t)lv_event_get_user_data(ev));
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        stationRowBtns[i] = btn;
        stationRowLabels[i] = lv_obj_get_child(btn, 0);
        if (wifiToolGroup) lv_group_add_obj(wifiToolGroup, btn);
        stationRowCount++;
    }

    for (int i = 0; i < count; i++) {
        char row[96];
        uint32_t age = (millis() - stationEntries[i].lastSeenMs) / 1000UL;
        snprintf(row, sizeof(row), "%s Ch%-2u %ddBm %us #%u",
                 stationEntries[i].stationMac,
                 stationEntries[i].channel,
                 stationEntries[i].rssi,
                 (unsigned)age,
                 stationEntries[i].packets);

        if (stationRowLabels[i]) lv_label_set_text(stationRowLabels[i], row);
        if (stationRowBtns[i]) {
            lv_obj_set_style_text_color(stationRowBtns[i], rssiColor(stationEntries[i].rssi),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }

    if (focusedRow >= 0 && focusedRow < stationRowCount && stationRowBtns[focusedRow]) {
        lv_group_focus_obj(stationRowBtns[focusedRow]);
    }
}

static void stationScanStop() {
    if (!stationScanActive) return;
    stationScanActive = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
    if (stationStartLbl) lv_label_set_text(stationStartLbl, LV_SYMBOL_PLAY "  Start");
}

static void stationScanStart() {
    stationEntryCount = 0;
    stationApCount = 0;
    stationEapolTotal = 0;
    memset(stationEntries, 0, sizeof(stationEntries));
    memset(stationAps, 0, sizeof(stationAps));
    stationClearRows();
    stationScanChannel = 1;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);

    wifi_promiscuous_filter_t filt;
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_channel(stationScanChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous_rx_cb(station_scan_cb);
    esp_wifi_set_promiscuous(true);
    stationScanActive = true;

    startLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, 150);
    if (stationStartLbl) lv_label_set_text(stationStartLbl, LV_SYMBOL_STOP "  Stop");
}

static void stationScanTimerCb(lv_timer_t *t) {
    stationRefreshApStats();
    if (stationScanActive) {
        static uint32_t lastHop = 0;
        uint32_t now = millis();
        if (now - lastHop >= (uint32_t)STATION_SCAN_HOP_MS) {
            stationScanChannel++;
            if (stationScanChannel > STATION_SCAN_MAX_CHANNEL) stationScanChannel = 1;
            esp_wifi_set_channel(stationScanChannel, WIFI_SECOND_CHAN_NONE);
            lastHop = now;
        }

        if (stationStatusLbl) {
            char s[72];
            snprintf(s, sizeof(s), LV_SYMBOL_PLAY "  CH:%u  STA:%d AP:%d EAPOL:%u",
                     stationScanChannel, stationEntryCount, stationApCount, stationEapolTotal);
            lv_label_set_text(stationStatusLbl, s);
            lv_obj_set_style_text_color(stationStatusLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
        }
    } else if (stationStatusLbl) {
        char s[72];
        snprintf(s, sizeof(s), LV_SYMBOL_STOP "  Ready  STA:%d AP:%d EAPOL:%u",
                 stationEntryCount, stationApCount, stationEapolTotal);
        lv_label_set_text(stationStatusLbl, s);
        lv_obj_set_style_text_color(stationStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    }

    stationRefreshList();
}

static void cb_stationScanToggle(lv_event_t *e) {
    if (stationScanActive) stationScanStop();
    else stationScanStart();
    stationScanTimerCb(nullptr);
}

void createStationScanner() {
    if (wifiToolScreen) { lv_obj_delete(wifiToolScreen); wifiToolScreen = nullptr; }
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_EYE_OPEN "  Station Scanner");

    stationStatusLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(stationStatusLbl, "Ready. Press Start to scan stations.");
    lv_obj_set_style_text_color(stationStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(stationStatusLbl, 8, 30);

    lv_obj_t *hint = lv_label_create(wifiToolScreen);
    lv_label_set_text(hint, "MAC  Ch  RSSI  Age  #Packets  AP/EAPOL in detail");
    lv_obj_set_style_text_color(hint, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(hint, 8, 48);

    stationList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(stationList, SCREEN_W, SCREEN_H - 104);
    lv_obj_set_pos(stationList, 0, 66);
    lv_obj_set_style_bg_color(stationList, lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(stationList, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(stationList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(stationList, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(stationList, 2, LV_PART_MAIN);

    // New screen instance: any previous row objects were deleted with the old screen.
    memset(stationRowBtns, 0, sizeof(stationRowBtns));
    memset(stationRowLabels, 0, sizeof(stationRowLabels));
    stationRowCount = 0;
    stationEmptyLbl = nullptr;

    stationBackBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    stationStartBtn = createActionBtn(wifiToolScreen, LV_SYMBOL_PLAY "  Start", cb_stationScanToggle);
    stationStartLbl = lv_obj_get_child(stationStartBtn, 0);

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();

    // Start/Stop is the primary control on this page, so focus it first.
    // Back remains available as the next encoder step.
    lv_group_add_obj(wifiToolGroup, stationStartBtn);
    lv_group_add_obj(wifiToolGroup, stationBackBtn);
    setGroup(wifiToolGroup);
    lv_group_focus_obj(stationStartBtn);

    if (stationScanTimer) { lv_timer_delete(stationScanTimer); stationScanTimer = nullptr; }
    stationScanTimer = lv_timer_create(stationScanTimerCb, 1000, nullptr);
    stationRefreshList();

    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

void createStationDetail(int idx) {
    if (idx < 0 || idx >= stationEntryCount) return;
    if (wifiDetailScreen) { lv_obj_delete(wifiDetailScreen); wifiDetailScreen = nullptr; }
    wifiDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiDetailScreen);
    createHeader(wifiDetailScreen, LV_SYMBOL_EYE_OPEN "  Station Detail");

    StationEntry e = stationEntries[idx];
    stationRefreshApStats();
    uint32_t age = (millis() - e.lastSeenMs) / 1000UL;
    int apIdx = stationFindApByBssid(e.apBssid);
    const char *apSsid = (apIdx >= 0 && stationAps[apIdx].ssid[0]) ? stationAps[apIdx].ssid : "Unknown";
    const char *apAuth = (apIdx >= 0 && stationAps[apIdx].authStr[0]) ? stationAps[apIdx].authStr : "Unknown";
    uint16_t apClients = (apIdx >= 0) ? stationAps[apIdx].clientCount : 0;
    uint16_t apEapol = (apIdx >= 0) ? stationAps[apIdx].eapolPackets : 0;

    lv_obj_t *card = lv_obj_create(wifiDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 6, LV_PART_MAIN);

    // Station details can run taller than the visible card area,
    // so keep the card scrollable and add it to the encoder group below.
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);

    char info[720];
    snprintf(info, sizeof(info),
             "Station MAC : %s\n"
             "RSSI        : %d dBm (%s)\n"
             "Channel     : %u\n"
             "Last Seen   : %lus ago\n"
             "Frame Type  : %s\n"
             "Packets     : %u\n"
             "\n"
             "AP / BSSID  : %s\n"
             "AP SSID     : %s\n"
             "AP Security : %s\n"
             "AP Clients  : %u\n"
             "\n"
             "EAPOL Seen  : %s\n"
             "EAPOL Count : %u\n"
             "AP EAPOL    : %u",
             e.stationMac,
             e.rssi, rssiQuality(e.rssi),
             e.channel,
             (unsigned long)age,
             e.frameType,
             e.packets,
             e.apBssid,
             apSsid,
             apAuth,
             apClients,
             e.eapolSeen ? "Yes" : "No",
             e.eapolPackets,
             apEapol);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, info);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *bar = lv_bar_create(wifiDetailScreen);
    lv_obj_set_size(bar, SCREEN_W - 12, 5);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(bar, -100, -30);
    lv_bar_set_value(bar, e.rssi, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, rssiColor(e.rssi), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    lv_obj_t *backBtn = createBackBtn(wifiDetailScreen, cb_wifiDetailBack);
    deleteGroup(&wifiDetailGroup);
    wifiDetailGroup = lv_group_create();

    // Focus the scrollable info card first so the encoder can scroll Station Detail.
    // Back remains the next encoder item.
    lv_group_add_obj(wifiDetailGroup, card);
    lv_group_add_obj(wifiDetailGroup, backBtn);
    setGroup(wifiDetailGroup);
    lv_group_focus_obj(card);

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
    deauthLog[slot].rssi    = pkt->rx_ctrl.rssi;

    deauthRssiSum += pkt->rx_ctrl.rssi;
    if (deauthRssiCount < 9999) deauthRssiCount++;
    if (pkt->rx_ctrl.rssi > deauthStrongestRSSI) deauthStrongestRSSI = pkt->rx_ctrl.rssi;

    deauthHead = (deauthHead + 1) % MAX_DEAUTH;
}


static int deauthUniqueRecentSources(int total) {
    int unique = 0;
    char seen[MAX_DEAUTH][18];
    memset(seen, 0, sizeof(seen));

    for (int i = 0; i < total; i++) {
        int slot = ((deauthHead - 1 - i) + MAX_DEAUTH) % MAX_DEAUTH;
        bool exists = false;
        for (int j = 0; j < unique; j++) {
            if (strncmp(seen[j], deauthLog[slot].src, 18) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists && unique < MAX_DEAUTH) {
            strncpy(seen[unique], deauthLog[slot].src, 17);
            seen[unique][17] = '\0';
            unique++;
        }
    }
    return unique;
}

static int deauthAverageRSSI() {
    int count = deauthRssiCount;
    if (count <= 0) return -100;
    return (int)(deauthRssiSum / count);
}

static const char *deauthLastSource() {
    static char lastSrc[18];
    if (deauthTotal <= 0) {
        snprintf(lastSrc, sizeof(lastSrc), "--:--:--:--:--:--");
        return lastSrc;
    }
    int slot = ((deauthHead - 1) + MAX_DEAUTH) % MAX_DEAUTH;
    snprintf(lastSrc, sizeof(lastSrc), "%s", deauthLog[slot].src);
    return lastSrc;
}

static void updateDeauthRate() {
    uint32_t now = millis();
    if (deauthLastRateMs == 0) {
        deauthLastRateMs = now;
        deauthLastRateTotal = deauthTotal;
        deauthCurrentRate = 0;
        return;
    }

    uint32_t elapsed = now - deauthLastRateMs;
    if (elapsed < 250) return;

    int delta = deauthTotal - deauthLastRateTotal;
    if (delta < 0) delta = 0;
    deauthCurrentRate = (int)((delta * 1000UL) / elapsed);
    deauthLastRateMs = now;
    deauthLastRateTotal = deauthTotal;
}

static void formatDeauthStats(char *buf, size_t len, bool compact) {
    int totalRecent = (deauthTotal < MAX_DEAUTH) ? deauthTotal : MAX_DEAUTH;
    int avgRssi = deauthAverageRSSI();
    int strongest = (deauthRssiCount > 0) ? deauthStrongestRSSI : -100;
    int uniqueSources = deauthUniqueRecentSources(totalRecent);

    if (compact) {
        snprintf(buf, len,
                 "Rate:%d/s  Ch:%d  Avg:%d  Strong:%d\nUnique:%d  Last:%s",
                 deauthCurrentRate,
                 deauthChannel,
                 avgRssi,
                 strongest,
                 uniqueSources,
                 deauthLastSource());
    } else {
        snprintf(buf, len,
                 "Total Frames : %d\n"
                 "Packet/sec   : %d\n"
                 "Current Ch   : %d\n"
                 "Avg RSSI     : %d dBm\n"
                 "Strongest    : %d dBm\n"
                 "Unique Src   : %d recent\n"
                 "Last Source  : %s",
                 deauthTotal,
                 deauthCurrentRate,
                 deauthChannel,
                 avgRssi,
                 strongest,
                 uniqueSources,
                 deauthLastSource());
    }
}

static void deauth_refresh_cb(lv_timer_t *) {
    if (!deauthCountLbl || !deauthEventList) return;

    updateDeauthRate();

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

    if (deauthStatsLbl) {
        char stats[128];
        formatDeauthStats(stats, sizeof(stats), true);
        lv_label_set_text(deauthStatsLbl, stats);
        lv_obj_set_style_text_color(deauthStatsLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    }
    if (deauthStatsBar) {
        int barRssi = (deauthRssiCount > 0) ? deauthStrongestRSSI : -100;
        lv_bar_set_value(deauthStatsBar, barRssi, LV_ANIM_ON);
        lv_obj_set_style_bg_color(deauthStatsBar, rssiColor(barRssi), LV_PART_INDICATOR);
    }

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
        snprintf(row, sizeof(row), "Ch%d  %s  %ddBm [R:%d]",
                 deauthLog[slot].channel,
                 deauthLog[slot].src,
                 deauthLog[slot].rssi,
                 deauthLog[slot].reason);
        lv_obj_t *entry = lv_list_add_text(deauthEventList, row);
        if (entry)
            lv_obj_set_style_text_color(entry, lv_color_hex(TH.alert), LV_PART_MAIN);
    }
}


static void cb_deauthStatsBack(lv_event_t *e) {
    wifiDetailScreen = nullptr;
    setGroup(wifiToolGroup);
    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
}

static void cb_deauthStatsOpen(lv_event_t *e) {
    createDeauthStats();
}

void createDeauthStats() {
    if (wifiDetailScreen) { lv_obj_delete(wifiDetailScreen); wifiDetailScreen = nullptr; }
    wifiDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiDetailScreen);
    createHeader(wifiDetailScreen, LV_SYMBOL_WARNING "  Deauth Stats");

    updateDeauthRate();

    lv_obj_t *card = lv_obj_create(wifiDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 40);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,      lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,        LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card,  lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card,  1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,        6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,       6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    char info[260];
    formatDeauthStats(info, sizeof(info), false);

    lv_obj_t *infoLbl = lv_label_create(card);
    lv_label_set_text(infoLbl, info);
    lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(infoLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(infoLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    int barRssi = (deauthRssiCount > 0) ? deauthStrongestRSSI : -100;
    lv_obj_t *bar = lv_bar_create(wifiDetailScreen);
    lv_obj_set_size(bar, SCREEN_W - 12, 5);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(bar, -100, -30);
    lv_bar_set_value(bar, barRssi, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, rssiColor(barRssi), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar,   3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar,   3, LV_PART_INDICATOR);

    lv_obj_t *backBtn = createBackBtn(wifiDetailScreen, cb_deauthStatsBack);

    deleteGroup(&wifiDetailGroup);
    wifiDetailGroup = lv_group_create();
    lv_group_add_obj(wifiDetailGroup, backBtn);
    setGroup(wifiDetailGroup);

    lv_screen_load_anim(wifiDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
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

    deauthStatsLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(deauthStatsLbl, "Rate:0/s  Ch:1  Avg:--  Strong:--\nUnique:0  Last:--:--:--:--:--:--");
    lv_label_set_long_mode(deauthStatsLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(deauthStatsLbl, SCREEN_W - 16);
    lv_obj_set_style_text_color(deauthStatsLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(deauthStatsLbl, 8, 48);

    deauthStatsBar = lv_bar_create(wifiToolScreen);
    lv_obj_set_size(deauthStatsBar, SCREEN_W - 16, 5);
    lv_obj_set_pos(deauthStatsBar, 8, 83);
    lv_bar_set_range(deauthStatsBar, -100, -30);
    lv_bar_set_value(deauthStatsBar, -100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(deauthStatsBar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(deauthStatsBar, lv_color_hex(TH.textDim), LV_PART_INDICATOR);
    lv_obj_set_style_radius(deauthStatsBar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(deauthStatsBar, 3, LV_PART_INDICATOR);

    deauthEventList = lv_list_create(wifiToolScreen);
    lv_obj_set_size(deauthEventList, SCREEN_W, SCREEN_H - 128);
    lv_obj_set_pos(deauthEventList, 0, 93);
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
    lv_obj_t *statsBtn = createActionBtn(wifiToolScreen, "Stats", cb_deauthStatsOpen);

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, backBtn);
    lv_group_add_obj(wifiToolGroup, statsBtn);
    setGroup(wifiToolGroup);

    // Start sniffer
    deauthTotal  = 0;
    deauthHead   = 0;
    deauthSoundedTotal = 0;
    deauthRssiSum = 0;
    deauthRssiCount = 0;
    deauthStrongestRSSI = -127;
    deauthCurrentRate = 0;
    deauthLastRateMs = 0;
    deauthLastRateTotal = 0;
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
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl, 42);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
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
    if (stationScanTimer) { lv_timer_delete(stationScanTimer); stationScanTimer = nullptr; }
    stationStatusLbl = nullptr;
    stationList = nullptr;
    stationStartBtn = nullptr;
    stationStartLbl = nullptr;
    stationBackBtn = nullptr;
    packetMonitorTimer = lv_timer_create(packetMonTimerCb, PACKET_MONITOR_UPDATE_MS, nullptr);

    packetMonUpdateUI();
    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, LED_BRIGHTNESS);
    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}


// ════════════════════════════════════════════════════════════════
//  TOOL 5 – WIFI MAPPER
//
//  Safe first pass inspired by Raymond-exe/wifi-mapper.
//  X-axis = WiFi channel 1-13. Y-axis = packet RSSI.
//  This stays display-only: no SD writes and no packet injection.
// ════════════════════════════════════════════════════════════════

static const char *wifiMapperSpeedName() {
    if (wifiMapperSpeedIdx == 0) return "Slow";
    if (wifiMapperSpeedIdx == 2) return "Fast";
    return "Normal";
}

static uint16_t wifiMapperHopMs() {
    if (wifiMapperSpeedIdx == 0) return WIFI_MAPPER_HOP_SLOW_MS;
    if (wifiMapperSpeedIdx == 2) return WIFI_MAPPER_HOP_FAST_MS;
    return WIFI_MAPPER_HOP_NORMAL_MS;
}

static uint16_t wifiMapperFreqMHz(uint8_t ch) {
    if (ch == 14) return 2484;
    return (uint16_t)(2407 + (ch * 5));
}

static int wifiMapperClampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void wifiMapperResetPoints() {
    wifiMapperHead = 0;
    wifiMapperCount = 0;
    wifiMapperTotalPackets = 0;
    wifiMapperLastRSSI = -127;
    wifiMapperLastType = 3;
    memset(wifiMapperPoints, 0, sizeof(wifiMapperPoints));
}

static void IRAM_ATTR wifi_mapper_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!wifiMapperActive || !buf) return;

    const wifi_promiscuous_pkt_t *pkt = reinterpret_cast<const wifi_promiscuous_pkt_t *>(buf);
    int8_t rssi = pkt->rx_ctrl.rssi;

    uint8_t t = 3;
    if      (type == WIFI_PKT_MGMT) t = 0;
    else if (type == WIFI_PKT_DATA) t = 1;
    else if (type == WIFI_PKT_CTRL) t = 2;

    uint16_t idx = wifiMapperHead;
    wifiMapperPoints[idx].channel = wifiMapperChannel;
    wifiMapperPoints[idx].rssi = rssi;
    wifiMapperPoints[idx].pktType = t;
    wifiMapperPoints[idx].ms = millis();

    wifiMapperHead = (uint16_t)((wifiMapperHead + 1) % WIFI_MAPPER_MAX_POINTS);
    if (wifiMapperCount < WIFI_MAPPER_MAX_POINTS) wifiMapperCount++;

    wifiMapperTotalPackets++;
    wifiMapperLastRSSI = rssi;
    wifiMapperLastType = t;
}

static lv_color_t wifiMapperPointColor(uint8_t pktType, int8_t rssi) {
    if (rssi >= -50) return lv_color_hex(TH.alert);
    if (rssi >= -65) return lv_color_hex(TH.warn);
    if (pktType == 1) return lv_color_hex(TH.success);  // data
    if (pktType == 2) return lv_color_hex(TH.accent);   // control
    return lv_color_hex(TH.text);                       // management/other
}

static void wifiMapperDrawGrid() {
    if (!wifiMapperGridArea) return;
    lv_obj_clean(wifiMapperGridArea);

    const int areaW = SCREEN_W - 12;
    const int areaH = 82;
    const int leftPad = 20;
    const int topPad = 5;
    const int plotW = areaW - 30;
    const int plotH = areaH - 18;

    // Highlight current hopping channel.
    int chX = leftPad + ((int)(wifiMapperChannel - 1) * plotW) / 12;
    lv_obj_t *hilite = lv_obj_create(wifiMapperGridArea);
    lv_obj_set_size(hilite, 3, plotH);
    lv_obj_set_pos(hilite, chX - 1, topPad);
    lv_obj_set_style_bg_color(hilite, lv_color_hex(TH.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hilite, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(hilite, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hilite, 1, LV_PART_MAIN);
    lv_obj_clear_flag(hilite, LV_OBJ_FLAG_SCROLLABLE);

    // Vertical channel grid.
    for (int ch = 1; ch <= 13; ch++) {
        int x = leftPad + ((ch - 1) * plotW) / 12;
        lv_obj_t *line = lv_obj_create(wifiMapperGridArea);
        lv_obj_set_size(line, 1, plotH);
        lv_obj_set_pos(line, x, topPad);
        lv_obj_set_style_bg_color(line, lv_color_hex((ch == wifiMapperChannel) ? TH.accent : TH.border), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(line, (ch == wifiMapperChannel) ? LV_OPA_70 : LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

        if (ch == 1 || ch == 6 || ch == 11 || ch == 13) {
            lv_obj_t *lbl = lv_label_create(wifiMapperGridArea);
            char s[4];
            snprintf(s, sizeof(s), "%d", ch);
            lv_label_set_text(lbl, s);
            lv_obj_set_style_text_color(lbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_pos(lbl, x - 3, topPad + plotH + 1);
        }
    }

    // Horizontal RSSI grid: -90, -70, -50, -30, -10.
    for (int r = WIFI_MAPPER_RSSI_MIN; r <= WIFI_MAPPER_RSSI_MAX; r += 20) {
        int y = topPad + ((WIFI_MAPPER_RSSI_MAX - r) * plotH) /
                (WIFI_MAPPER_RSSI_MAX - WIFI_MAPPER_RSSI_MIN);

        lv_obj_t *line = lv_obj_create(wifiMapperGridArea);
        lv_obj_set_size(line, plotW, 1);
        lv_obj_set_pos(line, leftPad, y);
        lv_obj_set_style_bg_color(line, lv_color_hex(TH.border), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(line, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

        if (r == -90 || r == -50 || r == -10) {
            lv_obj_t *lbl = lv_label_create(wifiMapperGridArea);
            char s[8];
            snprintf(s, sizeof(s), "%d", r);
            lv_label_set_text(lbl, s);
            lv_obj_set_style_text_color(lbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_pos(lbl, 1, y - 6);
        }
    }

    // Snapshot the ring indexes. It is fine if a packet arrives while drawing;
    // the next timer tick will catch it.
    uint16_t count = wifiMapperCount;
    uint16_t head = wifiMapperHead;

    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (head + WIFI_MAPPER_MAX_POINTS - count + i) % WIFI_MAPPER_MAX_POINTS;
        WiFiMapperPoint p = wifiMapperPoints[idx];
        if (p.channel < 1 || p.channel > 13) continue;

        int rssi = wifiMapperClampInt(p.rssi, WIFI_MAPPER_RSSI_MIN, WIFI_MAPPER_RSSI_MAX);
        int x = leftPad + ((int)(p.channel - 1) * plotW) / 12;
        int y = topPad + ((WIFI_MAPPER_RSSI_MAX - rssi) * plotH) /
                (WIFI_MAPPER_RSSI_MAX - WIFI_MAPPER_RSSI_MIN);

        lv_obj_t *dot = lv_obj_create(wifiMapperGridArea);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_pos(dot, x - 2, y - 2);
        lv_obj_set_style_bg_color(dot, wifiMapperPointColor(p.pktType, p.rssi), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void wifiMapperUpdateUI() {
    if (!wifiMapperStatusLbl || !wifiMapperDetailLbl) return;

    uint32_t now = millis();

    if (wifiMapperActive && (now - wifiMapperLastHopMs >= wifiMapperHopMs())) {
        wifiMapperChannel++;
        if (wifiMapperChannel > 13) wifiMapperChannel = 1;
        esp_wifi_set_channel(wifiMapperChannel, WIFI_SECOND_CHAN_NONE);
        wifiMapperLastHopMs = now;
    }

    char status[72];
    // Compact status line so it stays inside the 320px T-Embed width.
    snprintf(status, sizeof(status), "%s  CH:%u  %s  Pts:%u",
             wifiMapperActive ? "Map" : "Pause",
             wifiMapperChannel,
             wifiMapperSpeedName(),
             (unsigned)wifiMapperCount);
    lv_label_set_text(wifiMapperStatusLbl, status);
    lv_obj_set_style_text_color(wifiMapperStatusLbl,
                                lv_color_hex(wifiMapperActive ? TH.accent : TH.textDim),
                                LV_PART_MAIN);

    const char *typeName = "Other";
    if (wifiMapperLastType == 0) typeName = "Mgmt";
    else if (wifiMapperLastType == 1) typeName = "Data";
    else if (wifiMapperLastType == 2) typeName = "Ctrl";

    char detail[80];
    if (wifiMapperLastRSSI <= -126) {
        snprintf(detail, sizeof(detail), "Waiting for packets...");
    } else {
        // Compact detail line: channel, RSSI, frequency, bandwidth, packet type.
        snprintf(detail, sizeof(detail), "Ch:%u  %ddBm  %uMHz  20M  %s",
                 wifiMapperChannel,
                 (int)wifiMapperLastRSSI,
                 wifiMapperFreqMHz(wifiMapperChannel),
                 typeName);
    }
    lv_label_set_text(wifiMapperDetailLbl, detail);

    wifiMapperDrawGrid();
}

static void wifiMapperTimerCb(lv_timer_t *t) {
    wifiMapperUpdateUI();
}

static void wifiMapperStop() {
    if (!wifiMapperActive) return;
    wifiMapperActive = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    stopLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b);
    if (wifiMapperPauseLbl) lv_label_set_text(wifiMapperPauseLbl, "Resume");
    wifiMapperUpdateUI();
}

static void wifiMapperStart() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);

    wifi_promiscuous_filter_t filt;
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_CTRL;

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_channel(wifiMapperChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous_rx_cb(wifi_mapper_cb);
    esp_wifi_set_promiscuous(true);

    wifiMapperActive = true;
    wifiMapperLastHopMs = millis();

    startLEDSpinner(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, 140);
    if (wifiMapperPauseLbl) lv_label_set_text(wifiMapperPauseLbl, "Pause");
    wifiMapperUpdateUI();
}

static void cb_wifiMapperPauseResume(lv_event_t *e) {
    if (wifiMapperActive) wifiMapperStop();
    else wifiMapperStart();
}

static void cb_wifiMapperClear(lv_event_t *e) {
    wifiMapperResetPoints();
    wifiMapperUpdateUI();
}

static void cb_wifiMapperSpeed(lv_event_t *e) {
    wifiMapperSpeedIdx++;
    if (wifiMapperSpeedIdx > 2) wifiMapperSpeedIdx = 0;
    wifiMapperLastHopMs = millis();
    if (wifiMapperSpeedLbl) {
        char s[20];
        snprintf(s, sizeof(s), "Speed:%s", wifiMapperSpeedName());
        lv_label_set_text(wifiMapperSpeedLbl, s);
    }
    wifiMapperUpdateUI();
}

static lv_obj_t *createMapperBtn(lv_obj_t *parent, const char *text, int x, int w, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    // Compact WiFi Mapper button bar. Keeps all controls inside 320x170.
    lv_obj_set_size(btn, w, 20);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, x, -2);
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

void createWiFiMapper() {
    wifiMapperActive = false;
    wifiMapperChannel = 1;
    wifiMapperLastHopMs = millis();
    wifiMapperStatusLbl = nullptr;
    wifiMapperDetailLbl = nullptr;
    wifiMapperGridArea = nullptr;
    wifiMapperPauseBtn = nullptr;
    wifiMapperPauseLbl = nullptr;
    wifiMapperSpeedBtn = nullptr;
    wifiMapperSpeedLbl = nullptr;
    wifiMapperResetPoints();

    if (wifiToolScreen) { lv_obj_delete(wifiToolScreen); wifiToolScreen = nullptr; }
    wifiToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiToolScreen);
    createHeader(wifiToolScreen, LV_SYMBOL_EYE_OPEN "  WiFi Mapper");

    wifiMapperStatusLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(wifiMapperStatusLbl, "Map  CH:1  Normal  Pts:0");
    lv_obj_set_style_text_color(wifiMapperStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(wifiMapperStatusLbl, 8, 27);

    wifiMapperGridArea = lv_obj_create(wifiToolScreen);
    lv_obj_set_size(wifiMapperGridArea, SCREEN_W - 12, 82);
    lv_obj_set_pos(wifiMapperGridArea, 6, 42);
    lv_obj_set_style_bg_color(wifiMapperGridArea, lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wifiMapperGridArea, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(wifiMapperGridArea, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(wifiMapperGridArea, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(wifiMapperGridArea, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wifiMapperGridArea, 0, LV_PART_MAIN);
    lv_obj_clear_flag(wifiMapperGridArea, LV_OBJ_FLAG_SCROLLABLE);

    wifiMapperDetailLbl = lv_label_create(wifiToolScreen);
    lv_label_set_text(wifiMapperDetailLbl, "Waiting for packets...");
    lv_obj_set_style_text_color(wifiMapperDetailLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_label_set_long_mode(wifiMapperDetailLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(wifiMapperDetailLbl, SCREEN_W - 16);
    lv_obj_set_pos(wifiMapperDetailLbl, 8, 126);

    // Single compact control bar: no overlap with Back button.
    wifiMapperPauseBtn = createMapperBtn(wifiToolScreen, "Pause", 6, 58, cb_wifiMapperPauseResume);
    wifiMapperPauseLbl = lv_obj_get_child(wifiMapperPauseBtn, 0);
    lv_obj_t *clearBtn = createMapperBtn(wifiToolScreen, "Clear", 68, 54, cb_wifiMapperClear);

    wifiMapperSpeedBtn = createMapperBtn(wifiToolScreen, "Speed:Normal", 126, 104, cb_wifiMapperSpeed);
    wifiMapperSpeedLbl = lv_obj_get_child(wifiMapperSpeedBtn, 0);

    lv_obj_t *backBtn = createMapperBtn(wifiToolScreen, "Back", 234, 80, cb_wifiToolBack);

    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, wifiMapperPauseBtn);
    lv_group_add_obj(wifiToolGroup, clearBtn);
    lv_group_add_obj(wifiToolGroup, wifiMapperSpeedBtn);
    lv_group_add_obj(wifiToolGroup, backBtn);
    setGroup(wifiToolGroup);

    if (packetMonitorTimer) { lv_timer_delete(packetMonitorTimer); packetMonitorTimer = nullptr; }
    if (stationScanTimer) { lv_timer_delete(stationScanTimer); stationScanTimer = nullptr; }
    if (wifiMapperTimer) { lv_timer_delete(wifiMapperTimer); wifiMapperTimer = nullptr; }
    wifiMapperTimer = lv_timer_create(wifiMapperTimerCb, 500, nullptr);

    wifiMapperDrawGrid();
    wifiMapperStart();

    setAllLEDs(MENU_COLORS[0].r, MENU_COLORS[0].g, MENU_COLORS[0].b, LED_BRIGHTNESS);
    lv_screen_load_anim(wifiToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// ════════════════════════════════════════════════════════════════
//  TOOL 6 – PINEAP HUNTER

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


// Raven / SoundThinking-style BLE service UUID detection.
// Reference: 0xXyc/flock-you-wifi-recon Raven UUID patterns.
static const char *RAVEN_UUID_MATCHES[] = {
    RAVEN_DEVICE_INFO_SERVICE,
    RAVEN_GPS_SERVICE,
    RAVEN_POWER_SERVICE,
    RAVEN_NETWORK_SERVICE,
    RAVEN_UPLOAD_SERVICE,
    RAVEN_ERROR_SERVICE,
    RAVEN_OLD_HEALTH_SERVICE,
    RAVEN_OLD_LOCATION_SERVICE
};

static const char *ravenUuidLabel(const char *uuid) {
    if (!uuid) return "Unknown";
    if (strcasecmp(uuid, RAVEN_DEVICE_INFO_SERVICE) == 0) return "Device Info";
    if (strcasecmp(uuid, RAVEN_GPS_SERVICE) == 0) return "GPS";
    if (strcasecmp(uuid, RAVEN_POWER_SERVICE) == 0) return "Power";
    if (strcasecmp(uuid, RAVEN_NETWORK_SERVICE) == 0) return "Network";
    if (strcasecmp(uuid, RAVEN_UPLOAD_SERVICE) == 0) return "Upload";
    if (strcasecmp(uuid, RAVEN_ERROR_SERVICE) == 0) return "Error";
    if (strcasecmp(uuid, RAVEN_OLD_HEALTH_SERVICE) == 0) return "Old Health";
    if (strcasecmp(uuid, RAVEN_OLD_LOCATION_SERVICE) == 0) return "Old Location";
    return "Raven UUID";
}

static bool ravenUuidIsMatch(const String &uuidStr) {
    for (size_t i = 0; i < sizeof(RAVEN_UUID_MATCHES) / sizeof(RAVEN_UUID_MATCHES[0]); i++) {
        if (uuidStr.equalsIgnoreCase(RAVEN_UUID_MATCHES[i])) return true;
    }
    return false;
}

static uint8_t ravenCountUuidHits(BLEAdvertisedDevice &dev, char *firstUuid, size_t firstUuidLen) {
    if (firstUuid && firstUuidLen) firstUuid[0] = '\0';
    if (!dev.haveServiceUUID()) return 0;

    uint8_t hits = 0;
    int uuidCount = dev.getServiceUUIDCount();

    for (int i = 0; i < uuidCount; i++) {
        String uuidStr = String(dev.getServiceUUID(i).toString().c_str());
        uuidStr.toLowerCase();

        if (ravenUuidIsMatch(uuidStr)) {
            hits++;
            if (firstUuid && firstUuidLen && firstUuid[0] == '\0') {
                strncpy(firstUuid, uuidStr.c_str(), firstUuidLen - 1);
                firstUuid[firstUuidLen - 1] = '\0';
            }
        }
    }

    return hits;
}

static bool detectRaven(BLEAdvertisedDevice &dev) {
    char firstUuid[41];
    return ravenCountUuidHits(dev, firstUuid, sizeof(firstUuid)) > 0;
}

static const char *estimateRavenFW(BLEAdvertisedDevice &dev) {
    if (!dev.haveServiceUUID()) return "?";

    bool hasNewGps = false;
    bool hasOldLoc = false;
    bool hasPower  = false;

    int uuidCount = dev.getServiceUUIDCount();
    for (int i = 0; i < uuidCount; i++) {
        String uuidStr = String(dev.getServiceUUID(i).toString().c_str());
        uuidStr.toLowerCase();

        if (uuidStr.equalsIgnoreCase(RAVEN_GPS_SERVICE)) hasNewGps = true;
        if (uuidStr.equalsIgnoreCase(RAVEN_OLD_LOCATION_SERVICE)) hasOldLoc = true;
        if (uuidStr.equalsIgnoreCase(RAVEN_POWER_SERVICE)) hasPower = true;
    }

    if (hasOldLoc && !hasNewGps) return "1.1.x";
    if (hasNewGps && !hasPower)  return "1.2.x";
    if (hasNewGps && hasPower)   return "1.3.x";
    return "?";
}


// Smart Charger passive detector helpers.
// The nRF Connect logs for the test charger showed the name "Smart Charger"
// and the FFF0 GATT service with FFF1/FFF3 write and FFF4/FFF6/FFF7/FFF8 notify
// characteristics. This first-pass tool stays passive and only uses advertising data.
static bool chargerUuidIsMatch(const String &uuidStr) {
    return uuidStr.equalsIgnoreCase(CHARGER_SERVICE_UUID);
}

static bool chargerHasServiceUuid(BLEAdvertisedDevice &dev, char *firstUuid, size_t firstUuidLen) {
    if (firstUuid && firstUuidLen) firstUuid[0] = '\0';
    if (!dev.haveServiceUUID()) return false;

    int uuidCount = dev.getServiceUUIDCount();
    for (int i = 0; i < uuidCount; i++) {
        String uuidStr = String(dev.getServiceUUID(i).toString().c_str());
        uuidStr.toLowerCase();
        if (chargerUuidIsMatch(uuidStr)) {
            if (firstUuid && firstUuidLen) {
                strncpy(firstUuid, uuidStr.c_str(), firstUuidLen - 1);
                firstUuid[firstUuidLen - 1] = '\0';
            }
            return true;
        }
    }
    return false;
}

static bool chargerNameMatches(BLEAdvertisedDevice &dev) {
    if (!dev.haveName()) return false;
    String n = String(dev.getName().c_str());
    n.toLowerCase();

    for (int i = 0; i < CHARGER_NAME_MATCH_COUNT; i++) {
        String token = String(CHARGER_NAME_MATCHES[i]);
        token.toLowerCase();
        if (token.length() > 0 && n.indexOf(token) >= 0) return true;
    }
    return false;
}

static bool chargerMacPrefixMatches(const String &macStr) {
    String prefix = String(CHARGER_MAC_PREFIX);
    prefix.trim();
    if (prefix.length() == 0) return false;

    String macLower = macStr;
    String prefixLower = prefix;
    macLower.toLowerCase();
    prefixLower.toLowerCase();
    return macLower.startsWith(prefixLower);
}

static void chargerManufacturerPreview(BLEAdvertisedDevice &dev, char *out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';

    if (!dev.haveManufacturerData()) {
        strncpy(out, "none", outLen - 1);
        out[outLen - 1] = '\0';
        return;
    }

    std::string m = dev.getManufacturerData();
    size_t maxBytes = m.length();
    if (maxBytes > 8) maxBytes = 8;

    char tmp[4];
    for (size_t i = 0; i < maxBytes; i++) {
        snprintf(tmp, sizeof(tmp), "%02X", (uint8_t)m[i]);
        strncat(out, tmp, outLen - strlen(out) - 1);
        if (i + 1 < maxBytes) strncat(out, "-", outLen - strlen(out) - 1);
    }

    if (m.length() > maxBytes) {
        strncat(out, "...", outLen - strlen(out) - 1);
    }
}

static bool detectSmartCharger(BLEAdvertisedDevice &dev, char *method, size_t methodLen,
                               char *advUuid, size_t advUuidLen, uint8_t *confidence) {
    if (method && methodLen) method[0] = '\0';
    if (advUuid && advUuidLen) advUuid[0] = '\0';
    if (confidence) *confidence = 0;

    String macStr = dev.getAddress().toString().c_str();
    bool nameHit = chargerNameMatches(dev);
    bool uuidHit = chargerHasServiceUuid(dev, advUuid, advUuidLen);
    bool macHit  = chargerMacPrefixMatches(macStr);

    if (!nameHit && !uuidHit && !macHit) return false;

    const char *m = "Unknown";
    uint8_t conf = 1;
    if (nameHit && uuidHit) {
        m = "Name+UUID";
        conf = 3;
    } else if (nameHit && macHit) {
        m = "Name+MAC";
        conf = 3;
    } else if (uuidHit) {
        m = "FFF0 UUID";
        conf = 2;
    } else if (nameHit) {
        m = "Name";
        conf = 2;
    } else if (macHit) {
        m = "MAC Prefix";
        conf = 1;
    }

    if (method && methodLen) {
        strncpy(method, m, methodLen - 1);
        method[methodLen - 1] = '\0';
    }
    if (confidence) *confidence = conf;
    return true;
}

static const char *chargerConfidenceLabel(uint8_t c) {
    if (c >= 3) return "High";
    if (c == 2) return "Medium";
    return "Low";
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
//  We run promiscuous mode and sniff for those beacons. This parser
//  also pulls Minigotchi-style fields such as pal/minigotchi when present.
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
static lv_obj_t *pwnBackBtn   = nullptr;

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
    pwnPendingRSSI    = pkt->rx_ctrl.rssi;
    pwnPendingChannel = deauthChannel;
    pwnPendingReady   = true;
}

// Tiny JSON helpers for the simple Pwnagotchi beacon payload.
// These avoid adding ArduinoJson just for a few fields.
static bool pwnJsonString(const char *json, const char *key, char *out, size_t outLen) {
    if (!json || !key || !out || outLen == 0) return false;
    out[0] = '\0';

    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);

    char *start = strstr((char *)json, needle);
    if (!start) return false;
    start += strlen(needle);

    char *end = strchr(start, '"');
    if (!end) return false;

    size_t n = (size_t)(end - start);
    if (n >= outLen) n = outLen - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

static bool pwnJsonInt(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return false;

    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":", key);

    char *start = strstr((char *)json, needle);
    if (!start) return false;
    start += strlen(needle);

    *out = atoi(start);
    return true;
}

static bool pwnJsonBool(const char *json, const char *key) {
    if (!json || !key) return false;

    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":", key);

    char *start = strstr((char *)json, needle);
    if (!start) return false;
    start += strlen(needle);
    while (*start == ' ' || *start == '\t') start++;

    return (strncmp(start, "true", 4) == 0 || strncmp(start, "1", 1) == 0);
}

static const char *pwnDeviceType(const PwnEntry &e) {
    if (e.minigotchi) return "Minigotchi";
    if (e.pal)        return "Palnagotchi";
    return "Pwnagotchi";
}

// Parse a pending SSID JSON blob and update pwnEntries[].
// Called from the LVGL timer (main task) — safe to use strstr/atoi.
static void processPwnPending() {
    if (!pwnPendingReady) return;

    // Snapshot and clear the flag first
    char ssid[PWN_BUF_LEN];
    char bssid[18];
    int8_t rssi = pwnPendingRSSI;
    uint8_t ch = pwnPendingChannel;
    memcpy(ssid,  pwnPendingSSID,  PWN_BUF_LEN);
    memcpy(bssid, pwnPendingBSSID, 18);
    pwnPendingReady = false;

    // Parse useful Pwnagotchi / Minigotchi fields.
    char name[33] = "<unknown>";
    pwnJsonString(ssid, "name", name, sizeof(name));

    int pwnd = 0;
    pwnJsonInt(ssid, "pwnd_tot", &pwnd);

    bool pal = pwnJsonBool(ssid, "pal");
    bool minigotchi = pwnJsonBool(ssid, "minigotchi");

    // Update existing entry by name, or add new one
    for (int i = 0; i < pwnCount; i++) {
        if (strcmp(pwnEntries[i].name, name) == 0) {
            pwnEntries[i].rssi       = rssi;
            pwnEntries[i].channel    = ch;
            pwnEntries[i].pwnd_tot   = pwnd;
            pwnEntries[i].pal        = pal;
            pwnEntries[i].minigotchi = minigotchi;
            strncpy(pwnEntries[i].rawJson, ssid, sizeof(pwnEntries[i].rawJson) - 1);
            pwnEntries[i].rawJson[sizeof(pwnEntries[i].rawJson) - 1] = '\0';
            pwnEntries[i].lastSeen   = millis();
            return;
        }
    }
    if (pwnCount >= MAX_PWNS) return;
    strncpy(pwnEntries[pwnCount].name,  name,  32);
    strncpy(pwnEntries[pwnCount].bssid, bssid, 17);
    pwnEntries[pwnCount].name[32]  = '\0';
    pwnEntries[pwnCount].bssid[17] = '\0';
    pwnEntries[pwnCount].pwnd_tot   = pwnd;
    pwnEntries[pwnCount].pal        = pal;
    pwnEntries[pwnCount].minigotchi = minigotchi;
    pwnEntries[pwnCount].channel    = ch;
    pwnEntries[pwnCount].rssi       = rssi;
    strncpy(pwnEntries[pwnCount].rawJson, ssid, sizeof(pwnEntries[pwnCount].rawJson) - 1);
    pwnEntries[pwnCount].rawJson[sizeof(pwnEntries[pwnCount].rawJson) - 1] = '\0';
    pwnEntries[pwnCount].lastSeen   = millis();
    pwnCount++;
    playPwnagotchiChirp();
}

static void pwn_refresh_cb(lv_timer_t *) {
    if (!pwnStatusLbl || !pwnList) return;

    processPwnPending();

    // Keep parsing while the detail page is open, but do not rebuild the
    // hidden list or steal encoder focus away from the detail screen.
    if (lv_screen_active() != wifiToolScreen) return;

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

    // Rebuild list and encoder group so results are selectable.
    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    if (pwnBackBtn) lv_group_add_obj(wifiToolGroup, pwnBackBtn);

    lv_obj_clean(pwnList);
    if (pwnCount == 0) {
        lv_obj_t *e = lv_list_add_text(pwnList,
            "Hopping ch 1-13 — waiting for beacon...");
        if (e) lv_obj_set_style_text_color(e,
                    lv_color_hex(TH.textDim), LV_PART_MAIN);
        setGroup(wifiToolGroup);
        return;
    }

    for (int i = 0; i < pwnCount; i++) {
        uint32_t ageSec = (millis() - pwnEntries[i].lastSeen) / 1000;
        char row[80];
        snprintf(row, sizeof(row), "%s  %s  Ch%d  %ddBm  %d pwnd  %lus",
                 pwnDeviceType(pwnEntries[i]),
                 pwnEntries[i].name,
                 pwnEntries[i].channel,
                 pwnEntries[i].rssi,
                 pwnEntries[i].pwnd_tot,
                 (unsigned long)ageSec);
        lv_obj_t *btn = lv_list_add_btn(pwnList, nullptr, row);
        styleListBtn(btn);
        lv_obj_set_style_text_color(btn, lv_color_hex(TH.alert),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            createPwnagotchiDetail((int)(intptr_t)lv_event_get_user_data(ev));
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(wifiToolGroup, btn);
    }
    setGroup(wifiToolGroup);
}

void createPwnagotchiDetail(int idx) {
    if (idx < 0 || idx >= pwnCount) return;

    if (wifiDetailScreen) { lv_obj_delete(wifiDetailScreen); wifiDetailScreen = nullptr; }
    wifiDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiDetailScreen);

    char hdr[64];
    snprintf(hdr, sizeof(hdr), LV_SYMBOL_EYE_OPEN "  %.20s", pwnEntries[idx].name);
    createHeader(wifiDetailScreen, hdr);

    lv_obj_t *card = lv_obj_create(wifiDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,     lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,       6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,      6, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);

    uint32_t ageSec = (millis() - pwnEntries[idx].lastSeen) / 1000;
    char info[520];
    snprintf(info, sizeof(info),
             "Type      : %s\n"
             "Name      : %s\n"
             "Pwnd Tot  : %d\n"
             "RSSI      : %d dBm\n"
             "Channel   : %d\n"
             "BSSID     : %s\n"
             "Source    : DE:AD:BE:EF:DE:AD\n"
             "Pal       : %s\n"
             "Minigotchi: %s\n"
             "Age       : %lus\n\n"
             "Raw JSON / SSID:\n%s",
             pwnDeviceType(pwnEntries[idx]),
             pwnEntries[idx].name,
             pwnEntries[idx].pwnd_tot,
             pwnEntries[idx].rssi,
             pwnEntries[idx].channel,
             pwnEntries[idx].bssid,
             pwnEntries[idx].pal ? "yes" : "no",
             pwnEntries[idx].minigotchi ? "yes" : "no",
             (unsigned long)ageSec,
             pwnEntries[idx].rawJson[0] ? pwnEntries[idx].rawJson : "<not available>");

    lv_obj_t *infoLbl = lv_label_create(card);
    lv_label_set_text(infoLbl, info);
    lv_label_set_long_mode(infoLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(infoLbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(infoLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *bar = lv_bar_create(wifiDetailScreen);
    lv_obj_set_size(bar, SCREEN_W - 12, 5);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(bar, -100, -30);
    lv_bar_set_value(bar, pwnEntries[idx].rssi, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, rssiColor(pwnEntries[idx].rssi), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    lv_obj_t *backBtn = createBackBtn(wifiDetailScreen, cb_wifiDetailBack);

    deleteGroup(&wifiDetailGroup);
    wifiDetailGroup = lv_group_create();
    lv_group_add_obj(wifiDetailGroup, card);
    lv_group_add_obj(wifiDetailGroup, backBtn);
    setGroup(wifiDetailGroup);

    lv_screen_load_anim(wifiDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

void createPwnagotchiDetector() {
    // Reset state
    pwnCount        = 0;
    pwnPendingReady = false;
    pwnStatusLbl    = nullptr;
    pwnList         = nullptr;
    pwnBackBtn      = nullptr;
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

    pwnBackBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    lv_group_add_obj(wifiToolGroup, pwnBackBtn);
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

static bool IRAM_ATTR flockStartsWithNoCase(const char *text, const char *prefix) {
    if (!text || !prefix) return false;
    for (int i = 0; prefix[i]; i++) {
        if (!text[i]) return false;
        if (flockToLower(text[i]) != flockToLower(prefix[i])) return false;
    }
    return true;
}

static bool IRAM_ATTR flockIsHexChar(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

// Strong Flock SSID/name pattern: Flock-XXXX where XXXX are hex-style ID chars.
// Kept intentionally simple/fast so it is safe to call from sniffer paths.
static bool IRAM_ATTR flockHasStrictIdPattern(const char *ssid) {
#if FLOCK_STRICT_ID_PATTERN_ENABLED
    if (!ssid) return false;
    if (!flockStartsWithNoCase(ssid, "Flock-")) return false;

    int count = 0;
    const char *p = ssid + 6;
    while (*p && flockIsHexChar(*p)) {
        count++;
        p++;
    }
    return count >= FLOCK_STRICT_ID_MIN_HEX_CHARS;
#else
    return false;
#endif
}

static bool IRAM_ATTR containsFlockKeyword(const char *ssid) {
    return flockHasStrictIdPattern(ssid) ||
           flockContainsKeyword(ssid, FLOCK_KEYWORD_1) ||
           flockContainsKeyword(ssid, FLOCK_KEYWORD_2) ||
           flockContainsKeyword(ssid, FLOCK_KEYWORD_3) ||
           flockContainsKeyword(ssid, FLOCK_KEYWORD_4) ||
           flockContainsKeyword(ssid, FLOCK_KEYWORD_5) ||
           flockContainsKeyword(ssid, FLOCK_KEYWORD_6) ||
           flockContainsKeyword(ssid, FLOCK_KEYWORD_7) ||
           flockContainsKeyword(ssid, FLOCK_KEYWORD_8) ||
           flockContainsKeyword(ssid, FLOCK_KEYWORD_9);
}


static uint16_t flockAdaptiveDwellMs(uint8_t channel, uint16_t baseMs) {
#if FLOCK_ADAPTIVE_DWELL_ENABLED
    if (channel == 1 || channel == 6 || channel == 11) {
        return FLOCK_DWELL_MAIN_MS;
    }
    return FLOCK_DWELL_OTHER_MS;
#else
    return baseMs;
#endif
}

// Expanded Flock / SoundThinking MAC-prefix helpers.
// Prefixes are lowercase 3-byte OUIs in "xx:xx:xx" form.
static const char *FLOCK_HIGH_MAC_PREFIXES[] = {
    "58:8e:81", "cc:cc:cc", "ec:1b:bd", "90:35:ea", "04:0d:84",
    "f0:82:c0", "1c:34:f1", "38:5b:44", "94:34:69", "b4:e3:f9",
    "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "14:5a:fc",
    "74:4c:a1", "08:3a:88", "9c:2f:9d", "94:08:53", "e4:aa:ea",
    "b4:1e:52"
};
static const char *FLOCK_MFR_MAC_PREFIXES[] = {
    "f4:6a:dd", "f8:a2:d6", "e0:0a:f6", "00:f4:8d", "d0:39:57", "e8:d0:fc"
};
static const char *SOUNDTHINKING_MAC_PREFIXES[] = {
    "d4:11:d6"
};

static char flockLowerAscii(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static bool flockPrefixEquals(const char *mac, const char *prefix) {
    if (!mac || !prefix) return false;
    for (int i = 0; i < 8; i++) {
        if (!mac[i] || !prefix[i]) return false;
        if (flockLowerAscii(mac[i]) != flockLowerAscii(prefix[i])) return false;
    }
    return true;
}

static bool flockMacInList(const char *mac, const char **list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (flockPrefixEquals(mac, list[i])) return true;
    }
    return false;
}

static bool classifyFlockMac(const char *mac, char *method, size_t methodLen,
                             char *confidence, size_t confidenceLen,
                             char *type, size_t typeLen) {
#if FLOCK_MAC_PREFIX_MATCH_ENABLED
    if (flockMacInList(mac, FLOCK_HIGH_MAC_PREFIXES, sizeof(FLOCK_HIGH_MAC_PREFIXES) / sizeof(FLOCK_HIGH_MAC_PREFIXES[0]))) {
        snprintf(method, methodLen, "MAC Prefix");
        snprintf(confidence, confidenceLen, "High");
        snprintf(type, typeLen, "Flock");
        return true;
    }
    if (flockMacInList(mac, SOUNDTHINKING_MAC_PREFIXES, sizeof(SOUNDTHINKING_MAC_PREFIXES) / sizeof(SOUNDTHINKING_MAC_PREFIXES[0]))) {
        snprintf(method, methodLen, "MAC Prefix");
        snprintf(confidence, confidenceLen, "High");
        snprintf(type, typeLen, "SoundThinking");
        return true;
    }
    if (flockMacInList(mac, FLOCK_MFR_MAC_PREFIXES, sizeof(FLOCK_MFR_MAC_PREFIXES) / sizeof(FLOCK_MFR_MAC_PREFIXES[0]))) {
        snprintf(method, methodLen, "MFR Prefix");
        snprintf(confidence, confidenceLen, "Low");
        snprintf(type, typeLen, "Flock MFR");
        return true;
    }
#endif
    return false;
}

static void classifyFlockNameHit(const char *ssid,
                                 char *method, size_t methodLen,
                                 char *confidence, size_t confidenceLen,
                                 char *type, size_t typeLen) {
    if (flockHasStrictIdPattern(ssid)) {
        snprintf(method, methodLen, "Flock-ID");
        snprintf(confidence, confidenceLen, "High");
        snprintf(type, typeLen, "Flock");
        return;
    }

    snprintf(method, methodLen, "Name");
    snprintf(confidence, confidenceLen, "High");
    snprintf(type, typeLen, "Flock");
}

static void IRAM_ATTR flock_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!flockActive || type != WIFI_PKT_MGMT || flockPendingReady) return;

    const wifi_promiscuous_pkt_t *pkt =
        reinterpret_cast<const wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *d = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    if (len < 24) return;

    uint8_t fc0    = d[0];
    uint8_t ftype  = (fc0 >> 2) & 0x03;
    uint8_t stype  = (fc0 >> 4) & 0x0F;

    if (ftype != 0) return;
    if (stype != 8 && stype != 5 && stype != 4) return;

    char src[18];
    snprintf(src, sizeof(src), "%02X:%02X:%02X:%02X:%02X:%02X",
             d[10], d[11], d[12], d[13], d[14], d[15]);

    char method[18] = "";
    char confidence[8] = "";
    char deviceType[20] = "";
    bool macHit = classifyFlockMac(src, method, sizeof(method), confidence, sizeof(confidence), deviceType, sizeof(deviceType));

    char ssid[33];
    ssid[0] = '\0';
    bool nameHit = false;

    int ieOffset = (stype == 4) ? 24 : 36;
    if (ieOffset < len) {
        const uint8_t *ie  = d + ieOffset;
        int            rem = len - ieOffset;

        while (rem >= 2) {
            uint8_t id   = ie[0];
            uint8_t elen = ie[1];
            if (elen + 2 > rem) break;

            if (id == 0 && elen > 0) {
                int n = elen > 32 ? 32 : elen;
                memcpy(ssid, ie + 2, n);
                ssid[n] = '\0';
                if (containsFlockKeyword(ssid)) {
                    nameHit = true;
                    classifyFlockNameHit(ssid, method, sizeof(method), confidence, sizeof(confidence), deviceType, sizeof(deviceType));
                    break;
                }
            }
            ie  += elen + 2;
            rem -= elen + 2;
        }
    }

    if (!nameHit && !macHit) return;
    if (!ssid[0]) snprintf(ssid, sizeof(ssid), "<hidden/none>");

    strncpy(flockPendingSSID, ssid, sizeof(flockPendingSSID) - 1);
    flockPendingSSID[sizeof(flockPendingSSID) - 1] = '\0';
    strncpy(flockPendingSrc, src, sizeof(flockPendingSrc) - 1);
    flockPendingSrc[sizeof(flockPendingSrc) - 1] = '\0';
    strncpy(flockPendingMethod, method, sizeof(flockPendingMethod) - 1);
    flockPendingMethod[sizeof(flockPendingMethod) - 1] = '\0';
    strncpy(flockPendingConfidence, confidence, sizeof(flockPendingConfidence) - 1);
    flockPendingConfidence[sizeof(flockPendingConfidence) - 1] = '\0';
    strncpy(flockPendingDeviceType, deviceType, sizeof(flockPendingDeviceType) - 1);
    flockPendingDeviceType[sizeof(flockPendingDeviceType) - 1] = '\0';
    flockPendingType  = (stype == 4) ? 1 : 0;
    flockPendingRSSI  = pkt->rx_ctrl.rssi;
    flockPendingReady = true;
}

static void flock_refresh_cb(lv_timer_t *) {
    if (!flockStatusLbl || !flockList) return;

    // Process any pending hit from the ISR
    if (flockPendingReady) {
        char ssid[33], src[18], method[18], confidence[8], deviceType[20];
        uint8_t ft   = flockPendingType;
        int8_t  rssi = flockPendingRSSI;
        memcpy(ssid, flockPendingSSID, 33);
        memcpy(src,  flockPendingSrc,  18);
        memcpy(method, flockPendingMethod, 18);
        memcpy(confidence, flockPendingConfidence, 8);
        memcpy(deviceType, flockPendingDeviceType, 20);
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
                strncpy(flockHits[i].method, method, sizeof(flockHits[i].method) - 1);
                strncpy(flockHits[i].confidence, confidence, sizeof(flockHits[i].confidence) - 1);
                strncpy(flockHits[i].type, deviceType, sizeof(flockHits[i].type) - 1);
                flockHits[i].frameType = ft;
                flockHits[i].rssi = rssi;   // update RSSI
                flockHits[i].count++;
                flockHits[i].lastSeen = millis();
                found = true;
                break;
            }
        }
        if (!found && flockHitCount < MAX_FLOCK_HITS) {
            strncpy(flockHits[flockHitCount].ssid, ssid, 32);
            flockHits[flockHitCount].ssid[32] = '\0';
            strncpy(flockHits[flockHitCount].src, src, 17);
            flockHits[flockHitCount].src[17] = '\0';
            strncpy(flockHits[flockHitCount].method, method, sizeof(flockHits[flockHitCount].method) - 1);
            strncpy(flockHits[flockHitCount].confidence, confidence, sizeof(flockHits[flockHitCount].confidence) - 1);
            strncpy(flockHits[flockHitCount].type, deviceType, sizeof(flockHits[flockHitCount].type) - 1);
            flockHits[flockHitCount].frameType = ft;
            flockHits[flockHitCount].rssi      = rssi;
            flockHits[flockHitCount].count     = 1;
            flockHits[flockHitCount].firstSeen = millis();
            flockHits[flockHitCount].lastSeen  = millis();
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
        uint32_t age = (millis() - flockHits[i].lastSeen) / 1000UL;
        char row[150];
#if FLOCK_SHOW_SOURCE_MAC
        snprintf(row, sizeof(row), "%s %s [%s] %ddBm x%u\n%s  %s  %lus",
                 flockHits[i].frameType ? "PROBE" : "BEACON",
                 flockHits[i].type[0] ? flockHits[i].type : "Flock",
                 flockHits[i].confidence[0] ? flockHits[i].confidence : "High",
                 flockHits[i].rssi,
                 flockHits[i].count,
                 flockHits[i].src,
                 flockHits[i].method[0] ? flockHits[i].method : "Name",
                 (unsigned long)age);
#else
        snprintf(row, sizeof(row), "%s %s [%s] %ddBm x%u",
                 flockHits[i].frameType ? "PROBE" : "BEACON",
                 flockHits[i].type[0] ? flockHits[i].type : "Flock",
                 flockHits[i].confidence[0] ? flockHits[i].confidence : "High",
                 flockHits[i].rssi,
                 flockHits[i].count);
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
        "Sniffs beacons & probes for Flock patterns\n"
        "Flock-XXXX / FS_ / FS- / FlockOS / FlockCam");
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
                            int8_t rssi, const char *reason,
                            const char *method = "Name",
                            const char *confidence = "High",
                            const char *type = "Flock") {
    if (!source || !name || !mac || !reason) return;

    for (int i = 0; i < hybridHitCount; i++) {
        if (strcmp(hybridHits[i].source, source) == 0 &&
            strcmp(hybridHits[i].mac, mac) == 0) {
            strncpy(hybridHits[i].name, name, sizeof(hybridHits[i].name) - 1);
            hybridHits[i].name[sizeof(hybridHits[i].name) - 1] = '\0';
            strncpy(hybridHits[i].reason, reason, sizeof(hybridHits[i].reason) - 1);
            hybridHits[i].reason[sizeof(hybridHits[i].reason) - 1] = '\0';
            strncpy(hybridHits[i].method, method, sizeof(hybridHits[i].method) - 1);
            hybridHits[i].method[sizeof(hybridHits[i].method) - 1] = '\0';
            strncpy(hybridHits[i].confidence, confidence, sizeof(hybridHits[i].confidence) - 1);
            hybridHits[i].confidence[sizeof(hybridHits[i].confidence) - 1] = '\0';
            strncpy(hybridHits[i].type, type, sizeof(hybridHits[i].type) - 1);
            hybridHits[i].type[sizeof(hybridHits[i].type) - 1] = '\0';
            hybridHits[i].rssi = rssi;
            hybridHits[i].count++;
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
    strncpy(h.method, method, sizeof(h.method) - 1);
    strncpy(h.confidence, confidence, sizeof(h.confidence) - 1);
    strncpy(h.type, type, sizeof(h.type) - 1);
    h.rssi = rssi;
    h.count = 1;
    h.firstSeen = millis();
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
    char name[33], mac[18], reason[24], method[24], confidence[8], type[20];
    int8_t rssi = hybridPendingRSSI;
    memcpy(name, hybridPendingName, sizeof(name));
    memcpy(mac, hybridPendingMac, sizeof(mac));
    memcpy(reason, hybridPendingReason, sizeof(reason));
    memcpy(method, hybridPendingMethod, sizeof(method));
    memcpy(confidence, hybridPendingConfidence, sizeof(confidence));
    memcpy(type, hybridPendingType, sizeof(type));
    hybridPendingReady = false;
    hybridUpsertHit("WiFi", name, mac, rssi, reason, method, confidence, type);
}

static void hybridRebuildList() {
    if (!hybridList) return;
    hybridSortByRSSI();

    // Rebuild group so results are selectable without losing Back/Start Scan.
    deleteGroup(&wifiToolGroup);
    wifiToolGroup = lv_group_create();
    if (hybridBackBtn) lv_group_add_obj(wifiToolGroup, hybridBackBtn);
    if (hybridScanBtn) lv_group_add_obj(wifiToolGroup, hybridScanBtn);

    lv_obj_clean(hybridList);

    if (hybridHitCount == 0) {
        lv_obj_t *e = lv_list_add_text(hybridList,
            "No combined Flock hits yet. Press Start Scan.");
        if (e) lv_obj_set_style_text_color(e, lv_color_hex(TH.textDim), LV_PART_MAIN);
        setGroup(wifiToolGroup);
        return;
    }

    for (int i = 0; i < hybridHitCount; i++) {
        uint32_t age = (millis() - hybridHits[i].lastSeen) / 1000UL;
        char row[156];
#if FLOCK_HYBRID_SHOW_MAC
        snprintf(row, sizeof(row), "%s %s [%s] %ddBm x%u\n%s  %s  %lus",
                 hybridHits[i].source,
                 hybridHits[i].type[0] ? hybridHits[i].type : "Flock",
                 hybridHits[i].confidence[0] ? hybridHits[i].confidence : "High",
                 hybridHits[i].rssi,
                 hybridHits[i].count,
                 hybridHits[i].mac,
                 hybridHits[i].method[0] ? hybridHits[i].method : hybridHits[i].reason,
                 (unsigned long)age);
#else
        snprintf(row, sizeof(row), "%s %s [%s] %ddBm x%u",
                 hybridHits[i].source,
                 hybridHits[i].type[0] ? hybridHits[i].type : "Flock",
                 hybridHits[i].confidence[0] ? hybridHits[i].confidence : "High",
                 hybridHits[i].rssi,
                 hybridHits[i].count);
#endif
        lv_obj_t *btn = lv_list_add_btn(hybridList, nullptr, row);
        if (btn) {
            styleListBtn(btn);
            lv_obj_set_style_text_color(btn,
                strcmp(hybridHits[i].source, "BLE") == 0 ? lv_color_hex(TH.accent) : lv_color_hex(TH.alert),
                LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
                createFlockHybridDetail((int)(intptr_t)lv_event_get_user_data(ev));
            }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_group_add_obj(wifiToolGroup, btn);
        }
    }
    setGroup(wifiToolGroup);
}

static const char *hybridKeywordReason(const char *name) {
    if (flockHasStrictIdPattern(name)) return "Flock-ID";
    if (flockContainsKeyword(name, FLOCK_KEYWORD_1)) return FLOCK_KEYWORD_1;
    if (flockContainsKeyword(name, FLOCK_KEYWORD_2)) return FLOCK_KEYWORD_2;
    if (flockContainsKeyword(name, FLOCK_KEYWORD_3)) return FLOCK_KEYWORD_3;
    if (flockContainsKeyword(name, FLOCK_KEYWORD_4)) return "fs battery";
    if (flockContainsKeyword(name, FLOCK_KEYWORD_5)) return FLOCK_KEYWORD_5;
    if (flockContainsKeyword(name, FLOCK_KEYWORD_6)) return FLOCK_KEYWORD_6;
    if (flockContainsKeyword(name, FLOCK_KEYWORD_7)) return "flocksafety";
    if (flockContainsKeyword(name, FLOCK_KEYWORD_8)) return FLOCK_KEYWORD_8;
    if (flockContainsKeyword(name, FLOCK_KEYWORD_9)) return FLOCK_KEYWORD_9;
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

static bool detectFlockBLE(BLEAdvertisedDevice &dev, char *reason, size_t reasonLen,
                            char *method, size_t methodLen,
                            char *confidence, size_t confidenceLen,
                            char *type, size_t typeLen) {
    if (reason && reasonLen) reason[0] = '\0';
    if (method && methodLen) method[0] = '\0';
    if (confidence && confidenceLen) confidence[0] = '\0';
    if (type && typeLen) type[0] = '\0';

    String macStr = dev.getAddress().toString().c_str();
    if (classifyFlockMac(macStr.c_str(), method, methodLen, confidence, confidenceLen, type, typeLen)) {
        snprintf(reason, reasonLen, "%s", method);
        return true;
    }

    if (dev.haveName()) {
        String nm = String(dev.getName().c_str());
        char nbuf[33];
        strncpy(nbuf, nm.c_str(), sizeof(nbuf) - 1);
        nbuf[sizeof(nbuf) - 1] = '\0';

        if (containsFlockKeyword(nbuf)) {
            snprintf(reason, reasonLen, "Name:%s", hybridKeywordReason(nbuf));
            if (flockHasStrictIdPattern(nbuf)) {
                snprintf(method, methodLen, "Flock-ID");
            } else {
                snprintf(method, methodLen, "Name");
            }
            snprintf(confidence, confidenceLen, "High");
            snprintf(type, typeLen, "Flock");
            return true;
        }
        if (hybridNameIsTenDigits(nbuf)) {
            snprintf(reason, reasonLen, "10-digit name");
            snprintf(method, methodLen, "Name Pattern");
            snprintf(confidence, confidenceLen, "Medium");
            snprintf(type, typeLen, "Flock?");
            return true;
        }
    }

#if FLOCK_BLE_MFR_ID_MATCH_ENABLED
    if (dev.haveManufacturerData()) {
        std::string md = dev.getManufacturerData();
        if (md.length() >= 2) {
            uint16_t code = ((uint16_t)(uint8_t)md[1] << 8) | (uint16_t)(uint8_t)md[0];
            if (code == FLOCK_BLE_MFR_ID_XUNTONG) {
                snprintf(reason, reasonLen, "MFG 09C8");
                snprintf(method, methodLen, "BLE MFR ID");
                snprintf(confidence, confidenceLen, "High");
                snprintf(type, typeLen, "Flock/XUNTONG");
                return true;
            }
        }
    }
#endif

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

    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             d[10], d[11], d[12], d[13], d[14], d[15]);

    char reason[24] = "";
    char method[24] = "";
    char confidence[8] = "";
    char deviceType[20] = "";
    bool macHit = classifyFlockMac(mac, method, sizeof(method), confidence, sizeof(confidence), deviceType, sizeof(deviceType));

    char ssid[33];
    ssid[0] = '\0';
    bool nameHit = false;

    int ieOffset = (stype == 4) ? 24 : 36;
    if (ieOffset < len) {
        const uint8_t *ie = d + ieOffset;
        int rem = len - ieOffset;
        while (rem >= 2) {
            uint8_t id = ie[0];
            uint8_t elen = ie[1];
            if (elen + 2 > rem) break;

            if (id == 0 && elen > 0) {
                int n = elen > 32 ? 32 : elen;
                memcpy(ssid, ie + 2, n);
                ssid[n] = '\0';

                if (containsFlockKeyword(ssid)) {
                    nameHit = true;
                    snprintf(reason, sizeof(reason), "%s:%s",
                             (stype == 4) ? "Probe" : "Beacon",
                             hybridKeywordReason(ssid));
                    if (flockHasStrictIdPattern(ssid)) {
                        snprintf(method, sizeof(method), "Flock-ID");
                    } else {
                        snprintf(method, sizeof(method), "Name");
                    }
                    snprintf(confidence, sizeof(confidence), "High");
                    snprintf(deviceType, sizeof(deviceType), "Flock");
                    break;
                }
            }
            ie += elen + 2;
            rem -= elen + 2;
        }
    }

    if (!nameHit && !macHit) return;
    if (!reason[0]) snprintf(reason, sizeof(reason), "%s", (stype == 4) ? "Probe MAC" : "Beacon MAC");
    if (!ssid[0]) snprintf(ssid, sizeof(ssid), "<hidden/none>");

    strncpy(hybridPendingName, ssid, sizeof(hybridPendingName) - 1);
    hybridPendingName[sizeof(hybridPendingName) - 1] = '\0';
    strncpy(hybridPendingMac, mac, sizeof(hybridPendingMac) - 1);
    hybridPendingMac[sizeof(hybridPendingMac) - 1] = '\0';
    strncpy(hybridPendingReason, reason, sizeof(hybridPendingReason) - 1);
    hybridPendingReason[sizeof(hybridPendingReason) - 1] = '\0';
    strncpy(hybridPendingMethod, method, sizeof(hybridPendingMethod) - 1);
    hybridPendingMethod[sizeof(hybridPendingMethod) - 1] = '\0';
    strncpy(hybridPendingConfidence, confidence, sizeof(hybridPendingConfidence) - 1);
    hybridPendingConfidence[sizeof(hybridPendingConfidence) - 1] = '\0';
    strncpy(hybridPendingType, deviceType, sizeof(hybridPendingType) - 1);
    hybridPendingType[sizeof(hybridPendingType) - 1] = '\0';
    hybridPendingRSSI = pkt->rx_ctrl.rssi;
    hybridPendingReady = true;
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
    hybridBleHeardCount += (uint32_t)total;
    for (int i = 0; i < total; i++) {
        BLEAdvertisedDevice dev = results.getDevice(i);
        char reason[24];
        char method[24];
        char confidence[8];
        char deviceType[20];
        if (detectFlockBLE(dev, reason, sizeof(reason),
                           method, sizeof(method),
                           confidence, sizeof(confidence),
                           deviceType, sizeof(deviceType))) {
            String nm = dev.haveName() ? dev.getName().c_str() : "<unknown>";
            String mac = dev.getAddress().toString().c_str();
            hybridUpsertHit("BLE", nm.c_str(), mac.c_str(), (int8_t)dev.getRSSI(), reason,
                            method, confidence, deviceType);
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
    deauthChannel = ch;
    unsigned long startMs = millis();
    unsigned long lastHopMs = 0;
    unsigned long wifiMs = (unsigned long)flockHybridWifiSecs * 1000UL;

    while ((millis() - startMs) < wifiMs && hybridWifiActive) {
        if (hybridPendingReady) {
            hybridProcessPendingWifi();
            hybridRebuildList();
        }
        uint16_t hybridHopMs = flockAdaptiveDwellMs(ch, flockHybridHopMs);
        if (millis() - lastHopMs >= (unsigned long)hybridHopMs) {
            lastHopMs = millis();
            ch = (ch % 13) + 1;
            deauthChannel = ch;  // keep shared top-level hop state aligned
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
    snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  Hybrid complete — %d hit%s / %lu heard",
             hybridHitCount, hybridHitCount == 1 ? "" : "s", (unsigned long)hybridBleHeardCount);
    stopLEDSpinner(FLOCK_HYBRID_LED_R, FLOCK_HYBRID_LED_G, FLOCK_HYBRID_LED_B);

    lv_label_set_text(hybridStatusLbl, buf);
    lv_obj_set_style_text_color(hybridStatusLbl,
        hybridHitCount ? lv_color_hex(TH.alert) : lv_color_hex(TH.textDim), LV_PART_MAIN);
}


void createFlockHybridDetail(int idx) {
    if (idx < 0 || idx >= hybridHitCount) return;

    if (wifiDetailScreen) { lv_obj_delete(wifiDetailScreen); wifiDetailScreen = nullptr; }
    wifiDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(wifiDetailScreen);

    char hdr[64];
    snprintf(hdr, sizeof(hdr), LV_SYMBOL_EYE_OPEN "  %.20s", hybridHits[idx].type[0] ? hybridHits[idx].type : "Flock Hit");
    createHeader(wifiDetailScreen, hdr);

    FlockHybridHit h = hybridHits[idx];
    uint32_t age = (millis() - h.lastSeen) / 1000UL;
    uint32_t firstAge = (millis() - h.firstSeen) / 1000UL;

    lv_obj_t *card = lv_obj_create(wifiDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,     lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,       LV_OPA_COVER,           LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,       6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,      6, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);

    char info[760];
    snprintf(info, sizeof(info),
             "Source      : %s\n"
             "Type        : %s\n"
             "Confidence  : %s\n"
             "Method      : %s\n"
             "Reason      : %s\n"
             "Name / SSID : %s\n"
             "MAC         : %s\n"
             "RSSI        : %d dBm (%s)\n"
             "Seen Count  : %u\n"
             "Last Seen   : %lus ago\n"
             "First Seen  : %lus ago\n"
             "BLE Heard   : %lu advs\n\n"
             "Note: Low confidence manufacturer-prefix hits can be false positives.\n"
             "Raven UUID detection is intentionally left for a separate Raven Detector tool.",
             h.source,
             h.type[0] ? h.type : "Flock",
             h.confidence[0] ? h.confidence : "High",
             h.method[0] ? h.method : h.reason,
             h.reason,
             h.name,
             h.mac,
             h.rssi, hybridSignalQuality(h.rssi),
             h.count,
             (unsigned long)age,
             (unsigned long)firstAge,
             (unsigned long)hybridBleHeardCount);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, info);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, SCREEN_W - 28);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH.text), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *bar = lv_bar_create(wifiDetailScreen);
    lv_obj_set_size(bar, SCREEN_W - 12, 5);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_bar_set_range(bar, -100, -30);
    lv_bar_set_value(bar, h.rssi, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, rssiColor(h.rssi), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    lv_obj_t *backBtn = createBackBtn(wifiDetailScreen, cb_wifiDetailBack);
    deleteGroup(&wifiDetailGroup);
    wifiDetailGroup = lv_group_create();
    lv_group_add_obj(wifiDetailGroup, card);
    lv_group_add_obj(wifiDetailGroup, backBtn);
    setGroup(wifiDetailGroup);
    lv_group_focus_obj(card);

    lv_screen_load_anim(wifiDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
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
    hybridBackBtn = nullptr;
    hybridScanBtn = nullptr;
    hybridBleHeardCount = 0;
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

    hybridBackBtn = createBackBtn(wifiToolScreen, cb_wifiToolBack);
    hybridScanBtn = createActionBtn(wifiToolScreen, LV_SYMBOL_REFRESH "  Start Scan", cb_runFlockHybrid);

    hybridRebuildList();

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
    LV_SYMBOL_BLUETOOTH "  Raven Detector",
    LV_SYMBOL_BLUETOOTH "  Smart Charger",
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
        case 5: createRavenDetector();          break;
        case 6: createSmartChargerMonitor();    break;
        case 7: createTeslaDetector();          break;
        case 8: createSkimmerScanner();         break;
        case 9: createMetaDetector();           break;
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
    lv_group_add_obj(bleDetailGroup, card);
    lv_group_add_obj(bleDetailGroup, backBtn);
    lv_group_focus_obj(card);
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
//  BLE TOOL – RAVEN DETECTOR
//
//  Passive BLE detector for Raven / SoundThinking-style gunshot sensors.
//  It checks advertised service UUIDs only; it does not connect to devices.
//  UUID patterns and firmware-estimate idea are based on
//  0xXyc/flock-you-wifi-recon.
// ════════════════════════════════════════════════════════════════
static lv_obj_t *ravenStatusLbl = nullptr;
static lv_obj_t *ravenList      = nullptr;
static lv_obj_t *ravenBackBtn   = nullptr;
static lv_obj_t *ravenScanBtn   = nullptr;

static const char *ravenSignalQuality(int8_t rssi) {
    if (rssi >= -50) return "EXCELLENT";
    if (rssi >= -60) return "VERY GOOD";
    if (rssi >= -70) return "GOOD";
    if (rssi >= -80) return "FAIR";
    return "WEAK";
}

static int findRavenByMac(const char *mac) {
    for (int i = 0; i < ravenEntryCount; i++) {
        if (strcmp(ravenEntries[i].mac, mac) == 0) return i;
    }
    return -1;
}

static void sortRavenByRSSI() {
    for (int i = 0; i < ravenEntryCount - 1; i++) {
        for (int j = 0; j < ravenEntryCount - 1 - i; j++) {
            if (ravenEntries[j].rssi < ravenEntries[j + 1].rssi) {
                RavenEntry tmp = ravenEntries[j];
                ravenEntries[j] = ravenEntries[j + 1];
                ravenEntries[j + 1] = tmp;
            }
        }
    }
}

static void upsertRavenDevice(BLEAdvertisedDevice &dev) {
    String macStr = dev.getAddress().toString().c_str();
    int idx = findRavenByMac(macStr.c_str());

    if (idx < 0) {
        if (ravenEntryCount >= MAX_RAVEN_RESULTS) return;
        idx = ravenEntryCount++;
        memset(&ravenEntries[idx], 0, sizeof(RavenEntry));
        strncpy(ravenEntries[idx].mac, macStr.c_str(), sizeof(ravenEntries[idx].mac) - 1);
        strncpy(ravenEntries[idx].name, "Raven Device", sizeof(ravenEntries[idx].name) - 1);
    }

    String nm = dev.haveName() ? dev.getName().c_str() : "Raven Device";
    strncpy(ravenEntries[idx].name, nm.c_str(), sizeof(ravenEntries[idx].name) - 1);
    ravenEntries[idx].name[sizeof(ravenEntries[idx].name) - 1] = '\0';

    char firstUuid[41];
    uint8_t hitCount = ravenCountUuidHits(dev, firstUuid, sizeof(firstUuid));
    strncpy(ravenEntries[idx].matchedUuid,
            firstUuid[0] ? firstUuid : "unknown",
            sizeof(ravenEntries[idx].matchedUuid) - 1);
    ravenEntries[idx].matchedUuid[sizeof(ravenEntries[idx].matchedUuid) - 1] = '\0';

    strncpy(ravenEntries[idx].fwEstimate,
            estimateRavenFW(dev),
            sizeof(ravenEntries[idx].fwEstimate) - 1);
    ravenEntries[idx].fwEstimate[sizeof(ravenEntries[idx].fwEstimate) - 1] = '\0';

    ravenEntries[idx].uuidHitCount = hitCount;
    ravenEntries[idx].rssi = (int8_t)dev.getRSSI();
    ravenEntries[idx].lastSeen = millis();
}

static int doRavenScan(int durationSec) {
    ensureBLEInit();
    WiFi.disconnect();
    delay(50);

    BLEScan *pScan = BLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(150);
    pScan->setWindow(140);

    BLEScanResults results = pScan->start(durationSec, false);
    int total = results.getCount();

    ravenEntryCount = 0;
    memset(ravenEntries, 0, sizeof(ravenEntries));

    for (int i = 0; i < total && ravenEntryCount < MAX_RAVEN_RESULTS; i++) {
        BLEAdvertisedDevice dev = results.getDevice(i);
        if (detectRaven(dev)) {
            upsertRavenDevice(dev);
        }
    }

    pScan->clearResults();
    sortRavenByRSSI();
    return ravenEntryCount;
}

static void rebuildRavenList() {
    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    if (ravenBackBtn) lv_group_add_obj(bleToolGroup, ravenBackBtn);
    if (ravenScanBtn) lv_group_add_obj(bleToolGroup, ravenScanBtn);
    setGroup(bleToolGroup);

    lv_obj_clean(ravenList);
    if (ravenEntryCount == 0) {
        lv_obj_t *empty = lv_list_add_text(ravenList, "No Raven UUID patterns found yet");
        if (empty) lv_obj_set_style_text_color(empty, lv_color_hex(TH.textDim), LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < ravenEntryCount; i++) {
        char row[96];
        const char *uuidLabel = ravenUuidLabel(ravenEntries[i].matchedUuid);
        snprintf(row, sizeof(row), "%s  %ddBm  FW:%s",
                 uuidLabel,
                 ravenEntries[i].rssi,
                 ravenEntries[i].fwEstimate);

        lv_obj_t *btn = lv_list_add_btn(ravenList, nullptr, row);
        styleListBtn(btn);
        lv_obj_set_style_text_color(btn, bleRssiColor(ravenEntries[i].rssi),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            createRavenDetail((int)(intptr_t)lv_event_get_user_data(ev));
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(bleToolGroup, btn);

#if RAVEN_SHOW_FULL_MAC
        char sub[64];
        snprintf(sub, sizeof(sub), "%s  UUID hits:%u",
                 ravenEntries[i].mac,
                 ravenEntries[i].uuidHitCount);
        lv_obj_t *macTxt = lv_list_add_text(ravenList, sub);
        if (macTxt) lv_obj_set_style_text_color(macTxt, lv_color_hex(TH.textDim), LV_PART_MAIN);
#endif
    }

    setGroup(bleToolGroup);
}

static void cb_doRavenScan(lv_event_t *e) {
    char msg[48];
    snprintf(msg, sizeof(msg), LV_SYMBOL_REFRESH "  Scanning %ds...", RAVEN_SCAN_SECS);
    lv_label_set_text(ravenStatusLbl, msg);
    lv_obj_set_style_text_color(ravenStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_obj_clean(ravenList);
    lv_timer_handler();

    startLEDSpinner(170, 0, 220);
    int found = doRavenScan(RAVEN_SCAN_SECS);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(ravenStatusLbl, LV_SYMBOL_OK "  No Raven UUID patterns detected");
        lv_obj_set_style_text_color(ravenStatusLbl, lv_color_hex(TH.success), LV_PART_MAIN);
    } else {
        snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING "  %d Raven-like BLE device%s found!",
                 found, found == 1 ? "" : "s");
        lv_label_set_text(ravenStatusLbl, msg);
        lv_obj_set_style_text_color(ravenStatusLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
        playBLESuspiciousChirp();
    }

    rebuildRavenList();
}

void createRavenDetector() {
    ravenStatusLbl = nullptr;
    ravenList      = nullptr;
    ravenBackBtn   = nullptr;
    ravenScanBtn   = nullptr;

    if (bleToolScreen) { lv_obj_delete(bleToolScreen); bleToolScreen = nullptr; }
    bleToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleToolScreen);
    createHeader(bleToolScreen, LV_SYMBOL_BLUETOOTH "  Raven Detector");

    ravenStatusLbl = lv_label_create(bleToolScreen);
    lv_label_set_text(ravenStatusLbl,
                      "Detects Raven/SoundThinking BLE UUIDs\nPassive scan only; no connection");
    lv_obj_set_style_text_color(ravenStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(ravenStatusLbl, 8, 30);

    ravenList = lv_list_create(bleToolScreen);
    lv_obj_set_size(ravenList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(ravenList, 0, 48);
    lv_obj_set_style_bg_color(ravenList,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ravenList,       LV_OPA_COVER,        LV_PART_MAIN);
    lv_obj_set_style_border_width(ravenList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ravenList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ravenList,      2, LV_PART_MAIN);

    lv_obj_t *empty = lv_list_add_text(ravenList, "Ready to scan for Raven UUID patterns");
    if (empty) lv_obj_set_style_text_color(empty, lv_color_hex(TH.textDim), LV_PART_MAIN);

    ravenBackBtn = createBackBtn(bleToolScreen, cb_bleToolBack);
    ravenScanBtn = createActionBtn(bleToolScreen,
                                   LV_SYMBOL_REFRESH "  Scan",
                                   cb_doRavenScan);

    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    lv_group_add_obj(bleToolGroup, ravenBackBtn);
    lv_group_add_obj(bleToolGroup, ravenScanBtn);
    setGroup(bleToolGroup);

    if (ravenEntryCount > 0) {
        char msg[56];
        snprintf(msg, sizeof(msg), LV_SYMBOL_BLUETOOTH "  %d saved Raven result%s",
                 ravenEntryCount, ravenEntryCount == 1 ? "" : "s");
        lv_label_set_text(ravenStatusLbl, msg);
        lv_obj_set_style_text_color(ravenStatusLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
        rebuildRavenList();
    }

    setAllLEDs(170, 0, 220, LED_BRIGHTNESS);
    lv_screen_load_anim(bleToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

static void cb_ravenDetailBack(lv_event_t *e) {
    cb_bleDetailBack(e);
}

void createRavenDetail(int idx) {
    if (idx < 0 || idx >= ravenEntryCount) return;

    if (bleDetailScreen) { lv_obj_delete(bleDetailScreen); bleDetailScreen = nullptr; }
    bleDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleDetailScreen);

    char hdr[48];
    snprintf(hdr, sizeof(hdr), LV_SYMBOL_BLUETOOTH "  Raven Detail");
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

    uint32_t ageSec = (millis() - ravenEntries[idx].lastSeen) / 1000;
    const char *uuidLabel = ravenUuidLabel(ravenEntries[idx].matchedUuid);

    char info[360];
    snprintf(info, sizeof(info),
             "Raven / SoundThinking-like BLE\n"
             "Name : %s\n"
             "MAC  : %s\n"
             "RSSI : %d dBm (%s)\n"
             "UUID : %s\n"
             "Svc  : %s\n"
             "Hits : %u\n"
             "FW   : %s\n"
             "Age  : %lus\n"
             "Mode : passive BLE UUID scan",
             ravenEntries[idx].name,
             ravenEntries[idx].mac,
             ravenEntries[idx].rssi,
             ravenSignalQuality(ravenEntries[idx].rssi),
             uuidLabel,
             ravenEntries[idx].matchedUuid,
             ravenEntries[idx].uuidHitCount,
             ravenEntries[idx].fwEstimate,
             (unsigned long)ageSec);

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
    lv_bar_set_value(bar, ravenEntries[idx].rssi, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, bleRssiColor(ravenEntries[idx].rssi), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    lv_obj_t *backBtn = createBackBtn(bleDetailScreen, cb_ravenDetailBack);

    deleteGroup(&bleDetailGroup);
    bleDetailGroup = lv_group_create();
    lv_group_add_obj(bleDetailGroup, backBtn);
    setGroup(bleDetailGroup);

    lv_screen_load_anim(bleDetailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}


// ════════════════════════════════════════════════════════════════
//  BLE TOOL – SMART CHARGER MONITOR
//
//  First-pass passive scanner for BLE smart car USB/cigarette-lighter chargers.
//  It matches "Smart Charger", optional MAC prefix, and the FFF0 service UUID
//  when the charger advertises it. No BLE connection or writes are performed.
// ════════════════════════════════════════════════════════════════
static lv_obj_t *chargerStatusLbl = nullptr;
static lv_obj_t *chargerList      = nullptr;
static lv_obj_t *chargerBackBtn   = nullptr;
static lv_obj_t *chargerScanBtn   = nullptr;

static const char *chargerSignalQuality(int8_t rssi) {
    if (rssi >= -50) return "EXCELLENT";
    if (rssi >= -60) return "VERY GOOD";
    if (rssi >= -70) return "GOOD";
    if (rssi >= -80) return "FAIR";
    return "WEAK";
}

static int findChargerByMac(const char *mac) {
    for (int i = 0; i < chargerEntryCount; i++) {
        if (strcmp(chargerEntries[i].mac, mac) == 0) return i;
    }
    return -1;
}

static void sortChargersByRSSI() {
    for (int i = 0; i < chargerEntryCount - 1; i++) {
        for (int j = 0; j < chargerEntryCount - 1 - i; j++) {
            if (chargerEntries[j].rssi < chargerEntries[j + 1].rssi) {
                SmartChargerEntry tmp = chargerEntries[j];
                chargerEntries[j] = chargerEntries[j + 1];
                chargerEntries[j + 1] = tmp;
            }
        }
    }
}

static void upsertSmartCharger(BLEAdvertisedDevice &dev) {
    String macStr = dev.getAddress().toString().c_str();
    int idx = findChargerByMac(macStr.c_str());

    if (idx < 0) {
        if (chargerEntryCount >= MAX_CHARGER_RESULTS) return;
        idx = chargerEntryCount++;
        memset(&chargerEntries[idx], 0, sizeof(SmartChargerEntry));
        strncpy(chargerEntries[idx].mac, macStr.c_str(), sizeof(chargerEntries[idx].mac) - 1);
        strncpy(chargerEntries[idx].name, "Smart Charger", sizeof(chargerEntries[idx].name) - 1);
    }

    String nm = dev.haveName() ? dev.getName().c_str() : "Smart Charger";
    strncpy(chargerEntries[idx].name, nm.c_str(), sizeof(chargerEntries[idx].name) - 1);
    chargerEntries[idx].name[sizeof(chargerEntries[idx].name) - 1] = '\0';

    char method[24];
    char advUuid[41];
    uint8_t conf = 0;
    detectSmartCharger(dev, method, sizeof(method), advUuid, sizeof(advUuid), &conf);

    strncpy(chargerEntries[idx].matchMethod, method[0] ? method : "Unknown",
            sizeof(chargerEntries[idx].matchMethod) - 1);
    chargerEntries[idx].matchMethod[sizeof(chargerEntries[idx].matchMethod) - 1] = '\0';

    strncpy(chargerEntries[idx].advUuid, advUuid[0] ? advUuid : "not advertised",
            sizeof(chargerEntries[idx].advUuid) - 1);
    chargerEntries[idx].advUuid[sizeof(chargerEntries[idx].advUuid) - 1] = '\0';

    chargerManufacturerPreview(dev, chargerEntries[idx].mfgPreview,
                               sizeof(chargerEntries[idx].mfgPreview));

    chargerEntries[idx].confidence = conf;
    chargerEntries[idx].rssi = (int8_t)dev.getRSSI();
    chargerEntries[idx].lastSeen = millis();
}

static int doSmartChargerScan(int durationSec) {
    ensureBLEInit();
    WiFi.disconnect();
    delay(50);

    BLEScan *pScan = BLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(150);
    pScan->setWindow(140);

    BLEScanResults results = pScan->start(durationSec, false);
    int total = results.getCount();

    chargerEntryCount = 0;
    memset(chargerEntries, 0, sizeof(chargerEntries));

    for (int i = 0; i < total && chargerEntryCount < MAX_CHARGER_RESULTS; i++) {
        BLEAdvertisedDevice dev = results.getDevice(i);
        char method[24];
        char advUuid[41];
        uint8_t conf = 0;
        if (detectSmartCharger(dev, method, sizeof(method), advUuid, sizeof(advUuid), &conf)) {
            upsertSmartCharger(dev);
        }
    }

    pScan->clearResults();
    sortChargersByRSSI();
    return chargerEntryCount;
}

static void rebuildSmartChargerList() {
    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    if (chargerBackBtn) lv_group_add_obj(bleToolGroup, chargerBackBtn);
    if (chargerScanBtn) lv_group_add_obj(bleToolGroup, chargerScanBtn);
    setGroup(bleToolGroup);

    lv_obj_clean(chargerList);
    if (chargerEntryCount == 0) {
        lv_obj_t *empty = lv_list_add_text(chargerList, "No smart charger matches found yet");
        if (empty) lv_obj_set_style_text_color(empty, lv_color_hex(TH.textDim), LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < chargerEntryCount; i++) {
        char row[112];
        snprintf(row, sizeof(row), "%s  %ddBm  %s",
                 chargerEntries[i].name,
                 chargerEntries[i].rssi,
                 chargerConfidenceLabel(chargerEntries[i].confidence));

        lv_obj_t *btn = lv_list_add_btn(chargerList, nullptr, row);
        styleListBtn(btn);
        lv_obj_set_style_text_color(btn, bleRssiColor(chargerEntries[i].rssi),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, [](lv_event_t *ev) {
            createSmartChargerDetail((int)(intptr_t)lv_event_get_user_data(ev));
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(bleToolGroup, btn);

#if CHARGER_SHOW_FULL_MAC
        char sub[88];
        snprintf(sub, sizeof(sub), "%s  Match:%s",
                 chargerEntries[i].mac,
                 chargerEntries[i].matchMethod);
        lv_obj_t *macTxt = lv_list_add_text(chargerList, sub);
        if (macTxt) lv_obj_set_style_text_color(macTxt, lv_color_hex(TH.textDim), LV_PART_MAIN);
#endif
    }

    setGroup(bleToolGroup);
}

static void cb_doSmartChargerScan(lv_event_t *e) {
    char msg[56];
    snprintf(msg, sizeof(msg), LV_SYMBOL_REFRESH "  Scanning %ds...", CHARGER_SCAN_SECS);
    lv_label_set_text(chargerStatusLbl, msg);
    lv_obj_set_style_text_color(chargerStatusLbl, lv_color_hex(TH.warn), LV_PART_MAIN);
    lv_obj_clean(chargerList);
    lv_timer_handler();

    startLEDSpinner(80, 80, 255);
    int found = doSmartChargerScan(CHARGER_SCAN_SECS);
    stopLEDSpinner(MENU_COLORS[1].r, MENU_COLORS[1].g, MENU_COLORS[1].b);

    if (found == 0) {
        lv_label_set_text(chargerStatusLbl, LV_SYMBOL_OK "  No smart charger matches detected");
        lv_obj_set_style_text_color(chargerStatusLbl, lv_color_hex(TH.success), LV_PART_MAIN);
    } else {
        snprintf(msg, sizeof(msg), LV_SYMBOL_BLUETOOTH "  %d smart charger match%s found",
                 found, found == 1 ? "" : "es");
        lv_label_set_text(chargerStatusLbl, msg);
        lv_obj_set_style_text_color(chargerStatusLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
        playBLESuspiciousChirp();
    }

    rebuildSmartChargerList();
}

void createSmartChargerMonitor() {
    chargerStatusLbl = nullptr;
    chargerList      = nullptr;
    chargerBackBtn   = nullptr;
    chargerScanBtn   = nullptr;

    if (bleToolScreen) { lv_obj_delete(bleToolScreen); bleToolScreen = nullptr; }
    bleToolScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleToolScreen);
    createHeader(bleToolScreen, LV_SYMBOL_BLUETOOTH "  Smart Charger");

    chargerStatusLbl = lv_label_create(bleToolScreen);
    lv_label_set_text(chargerStatusLbl,
                      "Passive scan for Smart Charger / FFF0");
    lv_obj_set_style_text_color(chargerStatusLbl, lv_color_hex(TH.textDim), LV_PART_MAIN);
    lv_obj_set_pos(chargerStatusLbl, 8, 30);

    chargerList = lv_list_create(bleToolScreen);
    lv_obj_set_size(chargerList, SCREEN_W, SCREEN_H - 80);
    lv_obj_set_pos(chargerList, 0, 48);
    lv_obj_set_style_bg_color(chargerList,     lv_color_hex(TH.bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chargerList,       LV_OPA_COVER,        LV_PART_MAIN);
    lv_obj_set_style_border_width(chargerList, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chargerList,      2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(chargerList,      2, LV_PART_MAIN);

    lv_obj_t *empty = lv_list_add_text(chargerList, "Ready to scan for smart charger advertisements");
    if (empty) lv_obj_set_style_text_color(empty, lv_color_hex(TH.textDim), LV_PART_MAIN);

    chargerBackBtn = createBackBtn(bleToolScreen, cb_bleToolBack);
    chargerScanBtn = createActionBtn(bleToolScreen,
                                     LV_SYMBOL_REFRESH "  Scan",
                                     cb_doSmartChargerScan);

    deleteGroup(&bleToolGroup);
    bleToolGroup = lv_group_create();
    lv_group_add_obj(bleToolGroup, chargerBackBtn);
    lv_group_add_obj(bleToolGroup, chargerScanBtn);
    setGroup(bleToolGroup);

    if (chargerEntryCount > 0) {
        char msg[60];
        snprintf(msg, sizeof(msg), LV_SYMBOL_BLUETOOTH "  %d saved charger result%s",
                 chargerEntryCount, chargerEntryCount == 1 ? "" : "s");
        lv_label_set_text(chargerStatusLbl, msg);
        lv_obj_set_style_text_color(chargerStatusLbl, lv_color_hex(TH.accent), LV_PART_MAIN);
        rebuildSmartChargerList();
    }

    setAllLEDs(80, 80, 255, LED_BRIGHTNESS);
    lv_screen_load_anim(bleToolScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

static void cb_smartChargerDetailBack(lv_event_t *e) {
    cb_bleDetailBack(e);
}

void createSmartChargerDetail(int idx) {
    if (idx < 0 || idx >= chargerEntryCount) return;

    if (bleDetailScreen) { lv_obj_delete(bleDetailScreen); bleDetailScreen = nullptr; }
    bleDetailScreen = lv_obj_create(nullptr);
    applyScreenStyle(bleDetailScreen);
    createHeader(bleDetailScreen, LV_SYMBOL_BLUETOOTH "  Charger Detail");

    lv_obj_t *card = lv_obj_create(bleDetailScreen);
    lv_obj_set_size(card, SCREEN_W - 12, SCREEN_H - 28 - 14 - 34);
    lv_obj_set_pos(card, 6, 30);
    lv_obj_set_style_bg_color(card,      lv_color_hex(TH.card), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card,        LV_OPA_COVER,          LV_PART_MAIN);
    lv_obj_set_style_border_color(card,  lv_color_hex(TH.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(card,  1, LV_PART_MAIN);
    lv_obj_set_style_radius(card,        6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card,       6, LV_PART_MAIN);

    // Smart Charger details can run taller than the compact 320x170 screen,
    // so keep this card vertically scrollable and focusable by the encoder.
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_border_color(card, lv_color_hex(TH.accent), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN | LV_STATE_FOCUSED);

    uint32_t ageSec = (millis() - chargerEntries[idx].lastSeen) / 1000;

    char info[520];
    snprintf(info, sizeof(info),
             "Smart Charger BLE Match\n"
             "Name : %s\n"
             "MAC  : %s\n"
             "RSSI : %d dBm (%s)\n"
             "Conf : %s\n"
             "Match: %s\n"
             "Svc  : %s\n"
             "MFR  : %s\n"
             "Age  : %lus\n\n"
             "Known GATT from nRF logs:\n"
             "FFF0 service\n"
             "FFF1 Write | FFF3 WNR\n"
             "FFF4/FFF6/FFF7/FFF8 Notify\n\n"
             "Mode : passive scan only",
             chargerEntries[idx].name,
             chargerEntries[idx].mac,
             chargerEntries[idx].rssi,
             chargerSignalQuality(chargerEntries[idx].rssi),
             chargerConfidenceLabel(chargerEntries[idx].confidence),
             chargerEntries[idx].matchMethod,
             chargerEntries[idx].advUuid,
             chargerEntries[idx].mfgPreview,
             (unsigned long)ageSec);

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
    lv_bar_set_value(bar, chargerEntries[idx].rssi, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TH.barBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, bleRssiColor(chargerEntries[idx].rssi), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    lv_obj_t *backBtn = createBackBtn(bleDetailScreen, cb_smartChargerDetailBack);

    deleteGroup(&bleDetailGroup);
    bleDetailGroup = lv_group_create();

    // Focus the scrollable info card first so the encoder scrolls Charger Detail.
    // Back remains the next encoder item. This matches the other scrollable
    // detail pages and gives the card its focused border/highlight.
    lv_group_add_obj(bleDetailGroup, card);
    lv_group_add_obj(bleDetailGroup, backBtn);
    setGroup(bleDetailGroup);
    lv_group_focus_obj(card);

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

        // Play the same style of alert tone used by the other BLE finders
        // when a Tesla-like BLE pattern is found.
        playTeslaChirp();
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
    deauthCountLbl = nullptr;
    deauthStatsLbl = nullptr;
    deauthStatsBar = nullptr;
    deauthEventList = nullptr;
    if (pwnTimer)    { lv_timer_delete(pwnTimer);    pwnTimer    = nullptr; }
    if (flockTimer)  { lv_timer_delete(flockTimer);  flockTimer  = nullptr; }
    if (hybridStartTimer) { lv_timer_delete(hybridStartTimer); hybridStartTimer = nullptr; }
    if (packetMonitorTimer) { lv_timer_delete(packetMonitorTimer); packetMonitorTimer = nullptr; }
    if (stationScanTimer) { lv_timer_delete(stationScanTimer); stationScanTimer = nullptr; }
    stationStatusLbl = nullptr;
    stationList = nullptr;
    stationStartBtn = nullptr;
    stationStartLbl = nullptr;
    stationBackBtn = nullptr;

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
    delay(300);

    Serial.println();
    Serial.println("====================================");
    Serial.println("[Rogue-Radar] Boot sequence started...");
    Serial.printf("[Rogue-Radar] Reset reason: %d\n", esp_reset_reason());

    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

    // SD card on dedicated HSPI bus — must not share with TFT
    sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

    pinMode(POWER_PIN, OUTPUT);
    delay(10);
    digitalWrite(POWER_PIN, HIGH);
    delay(10);

    pinMode(ENCODER_BTN, INPUT_PULLUP);

#if BATTERY_METER_ENABLED
    analogReadResolution(12);
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);

    // Prime the shared top-bar battery value before the first screen/header is created.
    // This prevents early screens from briefly showing BAT 0% before the first cached
    // ADC read has been copied into batteryDisplayPercent.
    int bootPrimeBattRaw = 0;
    float bootPrimeBattVolts = 0.0f;
    int bootPrimeBattPercent = 0;

    readBatterySnapshot(&bootPrimeBattRaw, &bootPrimeBattVolts, &bootPrimeBattPercent);

    batteryDisplayPercent = bootPrimeBattPercent;
    batteryDisplayLastUpdateMs = millis();
    batteryDisplayValid = true;
#endif

    loadPersistentSettings();
    loadPersistentScanSettings();

    // I2S sound is lazy-initialized only when a chirp plays.

    // Boot-only APA102 ring animation. It runs before the TFT splash so
    // the splash image is not competing with the LED boot sequence.
    // Config controls on/off, speed, duration, brightness, and restore behavior.
    // No Serial logging here; the animation is intentionally quiet during boot.
    runBootLightsAnimation();

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
    lv_tick_set_cb([]() -> uint32_t {
        return (uint32_t)millis();
    });

    lvDisp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_flush_cb(lvDisp, lvgl_flush_cb);
    lv_display_set_buffers(lvDisp, lvBuf1, lvBuf2,
                           sizeof(lvBuf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lvIndev = lv_indev_create();
    lv_indev_set_type(lvIndev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(lvIndev, encoder_read_cb);

    ledStartupFlash();
    createMainMenu();
    updateTopbarWifiOverlay(true);

    Serial.printf("[Rogue-Radar] Device: %s\n", DEVICE_TYPE);
    Serial.printf("[Rogue-Radar] Firmware: %s\n", FIRMWARE_VERSION);
    Serial.printf("[Rogue-Radar] Persistent Settings: %s\n", PERSISTENT_SETTINGS_ENABLED ? "ON" : "OFF");
    Serial.printf("[Rogue-Radar] Chip: %s rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf("[Rogue-Radar] CPU: %u MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("[Rogue-Radar] Free heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("[Rogue-Radar] Flash size: %u bytes\n", ESP.getFlashChipSize());
    Serial.printf("[Rogue-Radar] Sketch size: %u bytes\n", ESP.getSketchSize());
    Serial.printf("[Rogue-Radar] Free sketch space: %u bytes\n", ESP.getFreeSketchSpace());
    Serial.printf("[Rogue-Radar] WiFi STA MAC %s\n", WiFi.macAddress().c_str());
    Serial.printf("[Rogue-Radar] WiFi AP MAC: %s\n", WiFi.softAPmacAddress().c_str());

#if BATTERY_METER_ENABLED
    int bootBattRaw = 0;
    float bootBattVolts = 0.0f;
    int bootBattPercent = 0;

    readBatterySnapshot(&bootBattRaw, &bootBattVolts, &bootBattPercent);

    Serial.printf("[Rogue-Radar] Battery: %.2f V / %d%%  raw:%d\n",
                  bootBattVolts, bootBattPercent, bootBattRaw);
#endif

    Serial.println("[Rogue-Radar] Boot complete.");
    Serial.println("====================================");
    Serial.println();
}

// ════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════
void loop() {
    // Feed all available GPS bytes into TinyGPS++ — non-blocking
    while (gpsSerial.available())
        gps.encode(gpsSerial.read());

    lv_timer_handler();

    // Keyboard OK/Back safety: finish after LVGL event handling and after
    // the encoder button has physically released.
    processKeyboardDeferredFinish();

    // Keyboard key sound safety: play delayed click only after GPIO0 is released.
    processKeyboardClickFeedback();

    // Connect to AP watchdog: plays the disconnect tone when an AP drops unexpectedly.
    processConnectApConnectionWatchdog();

    // Persistent top-bar WiFi icon: keep it visible on every page while connected.
    updateTopbarWifiOverlay();

    // Safe inactivity behavior: dim backlight + APA102 brightness only, no ESP32 sleep yet.
    updateInactivityDimmer();

    // Optional UI auto-return: after inactivity, jump back to the main home menu.
    updateAutoReturnHome();

    // Channel hopping: deauth detector and pwnagotchi watch use the normal
    // hop delay. Flock modes can optionally use adaptive dwell, giving
    // channels 1/6/11 a longer listen window and other channels a quicker pass.
    static unsigned long lastHop = 0;
    if (deauthActive || pwnActive || flockActive || hybridWifiActive) {
        uint16_t hopMs = (flockActive || hybridWifiActive)
                       ? flockAdaptiveDwellMs(deauthChannel, deauthHopMs)
                       : deauthHopMs;

        if (millis() - lastHop >= (unsigned long)hopMs) {
            lastHop       = millis();
            deauthChannel = (deauthChannel % 13) + 1;
            esp_wifi_set_channel(deauthChannel, WIFI_SECOND_CHAN_NONE);
        }
    }

    // Software power-off now lives at Misc Tools > Power Off.
    // GPIO0 is no longer watched in the background for long-hold shutdown.


    delay(5);
}

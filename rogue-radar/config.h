// ============================================================
//  config.h — Rogue Radar T-Embed v1.0.4
//  Edit this file to customise pins, limits, themes, and behaviour.
//  Do not edit rogue-radar.ino unless you know what you're doing.
// ============================================================
#pragma once

// ─── Device Name / Firmware Version ─────────────────────────────
#define DEVICE_NAME       "Rogue Radar"
#define FIRMWARE_VERSION  "RR v1.0.4"

// ─── Device Type ─────────────────────────────
#define DEVICE_TYPE       "T-Embed Non CC1101"

// ─── Splash Screen ──────────────────────────────────────────────
#define SPLASH_DURATION_MS  2600

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

// ─── Persistent Settings / NVS ─────────────────────────────────
// Saved in ESP32 NVS so settings survive reboot without an SD card.
#define PERSISTENT_SETTINGS_ENABLED  1
#define PREFS_NAMESPACE             "rogueradar"

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

// ─── Boot APA102 Ring Animation ────────────────────────────────
// Plays a red, green, then blue circling animation during boot.
// This is separate from the runtime Misc > LEDs ON/OFF toggle.
// Set BOOT_LIGHTS_ENABLED to 0 to disable the boot animation.
#define BOOT_LIGHTS_ENABLED              1
#define BOOT_LIGHTS_COLOR_MS           960   // How long each color spins
#define BOOT_LIGHTS_STEP_MS             74   // Lower = faster circle
#define BOOT_LIGHTS_MIN_REVOLUTIONS      1   // Minimum full circles per color
#define BOOT_LIGHTS_COLOR_GAP_MS       110   // Short dark gap between colors
#define BOOT_LIGHTS_END_HOLD_MS        215   // Hold final restored/dark state before splash
#define BOOT_LIGHTS_BRIGHTNESS          10   // 0-31 APA102 brightness
#define BOOT_LIGHTS_TAIL_PERCENT        20   // trailing LED brightness percent
#define BOOT_LIGHTS_RESTORE_MENU_COLOR   1   // 1 = end on WiFi/menu color, 0 = end dark
#define BOOT_LIGHTS_RESPECT_LED_TOGGLE   0   // 1 = skip if runtime LEDs are OFF


// ─── I2S Speaker / Alert Chirps ────────────────────────────────
// Uses the T-Embed speaker connector / onboard I2S amp pins.
// 1 = alert chirps enabled | 0 = alert chirps disabled.
// I2S is lazy-initialized on first chirp, then shut down after each chirp.
#define SOUND_ENABLED_DEFAULT      1
#define SOUND_I2S_BCLK            7
#define SOUND_I2S_WCLK            5
#define SOUND_I2S_DOUT            6
#define SOUND_SAMPLE_RATE         16000
#define SOUND_VOLUME_PERCENT      35   // 0-100 default for detection alert chirps
#define SOUND_VOLUME_MIN_PERCENT   0
#define SOUND_VOLUME_MAX_PERCENT   70   // runtime cap for the small T-Embed speaker
#define SOUND_VOLUME_STEP_PERCENT  5
#define SOUND_VOLUME_AUTO_SET_MS   1500 // after last +/- press, focus returns to Back
#define SOUND_ALERT_COOLDOWN_MS   1200 // prevents repeated chirp spam

// ─── Menu Feedback Sounds ──────────────────────────────────────
// Separate from the Misc > Sound ON/OFF alert toggle above.
// Set MENU_FEEDBACK_ENABLED_DEFAULT to 0 to boot with encoder/menu sounds
// disabled while keeping detection alert chirps available. Runtime toggle:
// Misc > Menu Sounds ON/OFF.
#define MENU_FEEDBACK_ENABLED_DEFAULT  1
#define MENU_FEEDBACK_VOLUME_PERCENT  8   // 0-100, kept low on purpose
#define MENU_FEEDBACK_VOLUME_MIN_PERCENT  0
#define MENU_FEEDBACK_VOLUME_MAX_PERCENT  30
#define MENU_FEEDBACK_VOLUME_STEP_PERCENT 2
#define MENU_FEEDBACK_VOLUME_AUTO_SET_MS   1500  // after last +/- press, focus returns to Back
#define MENU_FEEDBACK_TICK_COOLDOWN_MS 85  // prevents noisy encoder tick spam

// ─── Auto-return Home ──────────────────────────────────────────
// This does NOT sleep or restart the device. It only returns the UI
// back to the main home menu after no encoder movement/button activity.
// Set AUTO_RETURN_HOME_TIMEOUT_MS to 0 to disable auto-return.
#define AUTO_RETURN_HOME_TIMEOUT_MS  120000  // 2 minutes

// ─── Battery Meter ──────────────────────────────────────────────
// T-Embed LiPo battery monitor. GPIO4 is the expected ADC battery pin.
// If your board revision uses a different ADC pin, change BATTERY_ADC_PIN here.
// Note: GPIO1 is used by the encoder in this sketch, so test carefully before using 1.
#define BATTERY_METER_ENABLED       1
#define BATTERY_ADC_PIN             4
#define BATTERY_ADC_RESOLUTION   4095.0f
#define BATTERY_ADC_REF_VOLTAGE     3.30f
#define BATTERY_DIVIDER_RATIO       2.12f
#define BATTERY_UPDATE_MS        15000
#define BATTERY_DISPLAY_UPDATE_MS 30000  // top-bar percent sticks across screens
#define BATTERY_WARN_PERCENT       20
#define BATTERY_CRITICAL_PERCENT   10
#define BATTERY_AVG_SAMPLES         8

// ─── GPS ────────────────────────────────────────────────────────
#define GPS_RX_PIN  44
#define GPS_TX_PIN  43
#define GPS_BAUD  9600

// ─── Audio Tools / Sound Recorder ──────────────────────────────
// First-pass record/playback tool for the T-Embed ES7210 microphone.
// Mic pins are from the T-Embed pinout image: ES_BCLK=IO47, ES_LRCK=IO21,
// ES_DIN=IO14, ES_MCLK=IO48. Speaker playback reuses SOUND_I2S_* pins.
#define AUDIO_TOOLS_ENABLED                 1
#define AUDIO_RECORDER_MIC_BCLK           47
#define AUDIO_RECORDER_MIC_LRCK           21
#define AUDIO_RECORDER_MIC_DIN            14
#define AUDIO_RECORDER_MIC_MCLK           48

// ES7210 control bus. LilyGO's official T-Embed mic example uses SDA=IO18 and SCL=IO8.
// The ES7210 must be configured over I2C before useful mic audio appears on I2S.
#define AUDIO_RECORDER_USE_ES7210_I2C       1
#define AUDIO_RECORDER_I2C_SDA             18
#define AUDIO_RECORDER_I2C_SCL              8
#define AUDIO_RECORDER_ES7210_ADDR       0x40

// ES7210 mic gain values use the same scale as LilyGO's mic example.
// 0 = 0dB, 8 = 24dB, 10 = 30dB, 14 = 37.5dB.
#define AUDIO_RECORDER_ES7210_GAIN_MIC12   14
#define AUDIO_RECORDER_ES7210_GAIN_MIC34   14

// Set to 1 to require an ES7210 I2C control/probe hit before recording.
// Set to 0 to continue anyway and try raw I2S RX using the mic pins.
// Useful while testing because the pinout lists the I2S mic pins but may not
// expose the ES7210 control bus on the same pins we first tested.
#define AUDIO_RECORDER_REQUIRE_ES7210_I2C   1

// RAM recorder settings. Keep this small while testing the mic path.
#define AUDIO_RECORD_SAMPLE_RATE        16000
#define AUDIO_RECORD_SECONDS                5

// If full clip allocation fails, the recorder can fall back to a shorter
// RAM buffer instead of showing "No audio buffer". This keeps LVGL stable.
#define AUDIO_RECORD_ALLOW_SHORT_BUFFER     1
#define AUDIO_RECORD_BUFFER_SAFETY_BYTES 24576

#define AUDIO_RECORD_PLAYBACK_VOLUME_PERCENT 35

// Playback speed percent for Sound Recorder.
// 100 = normal 16 kHz playback. If recordings sound slow-motion,
// try 200 first. Lower/higher values are useful while tuning the ES7210/I2S path.
#define AUDIO_RECORD_PLAYBACK_SPEED_PERCENT 200

// Manual Stop button tuning for Sound Recorder.
// Poll = how often the blocking recorder loop checks the encoder button.
// Guard = how long to ignore the leftover Record/Stop click after a manual stop,
// so the button does not immediately start a new recording from bounce/release.
#define AUDIO_RECORD_STOP_POLL_MS            20
#define AUDIO_RECORD_STOP_RESTART_GUARD_MS  900

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
#define LED_COLOR_AUDIO   { 100,   0, 180 }

// ─── Power Off Menu ─────────────────────────────────────────────
// Software power-off is now triggered from Misc Tools > Power Off.
// GPIO0/encoder long-hold shutdown was removed because GPIO0 is also
// the ESP32-S3 boot/download strap pin used during flashing.
#define POWER_OFF_DELAY_MS  1200

// ─── WiFi Scanner ───────────────────────────────────────────────
// WiFi scan time is session-adjustable from Misc > Scan Defaults.
#define WIFI_SCAN_SECS    10
#define MAX_WIFI_RESULTS  30

// ─── Connect to AP Tool ────────────────────────────────────────
// Runtime tool: WiFi Tools > Connect to AP.
// Scans nearby APs, lets you select one, opens the LVGL keyboard for
// the password, then keeps the station connection alive for future
// safe LAN tools such as gateway checks, simple port checks, or SSH banners.
#define CONNECT_AP_TIMEOUT_MS     15000
#define CONNECT_AP_KEEP_RESULTS       1

// Connected Status page internet reachability check.
// Uses a lightweight TCP connection test after WiFi connects.
// Set to 0 if you want faster status-page loading with no internet check.
#define CONNECT_AP_INTERNET_CHECK_ENABLED     1
#define CONNECT_AP_INTERNET_CHECK_HOST        "1.1.1.1"
#define CONNECT_AP_INTERNET_CHECK_PORT        80
#define CONNECT_AP_INTERNET_CHECK_TIMEOUT_MS  1200

// ─── Gateway Info / Router Check Tool ──────────────────────────
// Connected-only WiFi tool. It appears dimmed in WiFi Tools until
// Rogue Radar is connected to an AP, then opens a router-focused page
// with a Refresh button. Uses lightweight TCP checks only.
#define GATEWAY_INFO_ENABLED                 1
#define GATEWAY_INFO_TCP_TIMEOUT_MS        450
// Horizontal scrolling speed for the one-line Gateway Info status banner.
// Higher = faster marquee scroll. Lower = slower/easier to read.
// Horizontal status marquee speed. Higher = faster, lower = slower.
#define GATEWAY_INFO_STATUS_SCROLL_SPEED   28
#define GATEWAY_INFO_PORT_COUNT              3
static const uint16_t GATEWAY_INFO_PORTS[GATEWAY_INFO_PORT_COUNT] = {
    80,   // HTTP/router admin
    443,  // HTTPS/router admin
    53    // DNS/router service
};

// Top-bar WiFi connection icon.
// Shows only while the device is connected to an AP.
// Leave TOPBAR_WIFI_ICON_CUSTOM_TEXT blank to use LVGL's built-in WiFi symbol.
// Put custom text here if your font/theme ever needs a different marker.
#define TOPBAR_WIFI_ICON_ENABLED          1
#define TOPBAR_WIFI_ICON_CUSTOM_TEXT      ""

// ─── LAN Host Discovery Tool ───────────────────────────────────
// Connected-only WiFi tool. It appears dimmed in WiFi Tools until
// Rogue Radar is connected to an AP.
// Uses lightweight TCP connection probes on the current LAN subnet.
// The scan range is built from WiFi.localIP() + WiFi.subnetMask(),
// then capped by LAN_DISCOVERY_START_HOST and LAN_DISCOVERY_MAX_HOSTS.
// This is safe host discovery only; it does not run service scripts,
// brute force, exploit checks, or deep port scans.
#define LAN_DISCOVERY_ENABLED              1
#define LAN_DISCOVERY_START_HOST           1
#define LAN_DISCOVERY_MAX_HOSTS          254
#define LAN_DISCOVERY_MAX_RESULTS         40
#define LAN_DISCOVERY_TCP_TIMEOUT_MS      85
#define LAN_DISCOVERY_SHOW_CLOSED_SUMMARY  1

// Rainbow APA102 spinner while LAN Host Discovery is actively probing.
// This gives visible feedback because TCP probes can make the UI feel paused.
#define LAN_DISCOVERY_RAINBOW_LED_ENABLED   1
#define LAN_DISCOVERY_RAINBOW_LED_DELAY_MS 70

// Dedicated LAN Discovery completion sound.
// Different from connect/disconnect and detector chirps.
// Follows Misc > Menu Sounds ON/OFF, and can also be disabled here.
#define LAN_DISCOVERY_DONE_SOUND_ENABLED   1
#define LAN_DISCOVERY_DONE_SOUND_VOLUME_PERCENT  16

#define LAN_DISCOVERY_PORT_COUNT           4
static const uint16_t LAN_DISCOVERY_PORTS[LAN_DISCOVERY_PORT_COUNT] = {
    80,   // HTTP / router/admin pages
    443,  // HTTPS
    22,   // SSH
    53    // DNS/router
};


// Dedicated Connect to AP event sounds.
// These use the same small I2S speaker path as menu feedback, but with
// a different tone pattern so connection/disconnect events stand out.
// They follow Misc > Menu Sounds ON/OFF because they are UI feedback sounds.
#define CONNECT_AP_EVENT_SOUNDS_ENABLED     1
#define CONNECT_AP_EVENT_VOLUME_PERCENT    14

// Save successful AP passwords in ESP32 NVS so selecting the same
// secured AP later can reconnect without opening the keyboard again.
#define CONNECT_AP_SAVE_PASSWORDS     1
#define CONNECT_AP_SAVED_SLOT_COUNT   5

// Optional hard-coded fallback credentials.
// Leave SSID/password entries blank if you do not want to use this.
// These are checked only if no saved NVS password is found.
#define CONNECT_AP_USE_CONFIG_CREDENTIALS  1
#define CONNECT_AP_CONFIG_CRED_COUNT       3
static const char* CONNECT_AP_CONFIG_SSIDS[CONNECT_AP_CONFIG_CRED_COUNT] = {
    "",
    "",
    ""
};
static const char* CONNECT_AP_CONFIG_PASSWORDS[CONNECT_AP_CONFIG_CRED_COUNT] = {
    "",
    "",
    ""
};


// ─── Packet Monitor ─────────────────────────────────────────────
// Display-only packet monitor inspired by https://github.com/spacehuhn/PacketMonitor32.
#define PACKET_MONITOR_DEFAULT_CH        6
#define PACKET_MONITOR_UPDATE_MS       500
#define PACKET_MONITOR_GRAPH_BARS       48
#define PACKET_MONITOR_GRAPH_MAX_RATE  200  // baseline packets/sec for graph scaling; set 0 for autoscale only

// Channel hopping for WiFi Tools > Packet Monitor.
// Runtime controls: Misc Tools > Scan Defaults > Packet Hop / Packet Hop ms.
#define PACKET_MONITOR_HOP_ENABLED_DEFAULT  0
#define PACKET_MONITOR_HOP_MS             750
#define PACKET_MONITOR_HOP_PRESET_0_MS    250
#define PACKET_MONITOR_HOP_PRESET_1_MS    500
#define PACKET_MONITOR_HOP_PRESET_2_MS    750
#define PACKET_MONITOR_HOP_PRESET_3_MS   1000
#define PACKET_MONITOR_HOP_PRESET_4_MS   1500


// ─── Station Scanner ───────────────────────────────────────────
// Passive client/station scanner inspired by GhostESP station scan logic.
// Runtime tool: WiFi Tools > Station Scanner.
#define MAX_STATION_RESULTS        30
#define MAX_STATION_APS            20
#define STATION_SCAN_HOP_MS       250
#define STATION_SCAN_MAX_CHANNEL   13

// ─── WiFi Mapper ───────────────────────────────────────────────
// Visual WiFi packet map inspired by Raymond-exe/wifi-mapper.
// Runtime tool: WiFi Tools > WiFi Mapper.
#define WIFI_MAPPER_MAX_POINTS       80
#define WIFI_MAPPER_RSSI_MIN        -90
#define WIFI_MAPPER_RSSI_MAX        -10
#define WIFI_MAPPER_HOP_SLOW_MS     500
#define WIFI_MAPPER_HOP_NORMAL_MS   250
#define WIFI_MAPPER_HOP_FAST_MS     120
#define WIFI_MAPPER_DEFAULT_SPEED     1   // 0=Slow, 1=Normal, 2=Fast

// ─── Deauth Detector ────────────────────────────────────────────
#define MAX_DEAUTH        12
// Channel hop delay is adjustable from Misc > Scan Defaults.
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
#define FLOCK_KEYWORD_5  "FS_"
#define FLOCK_KEYWORD_6  "FlockOS"
#define FLOCK_KEYWORD_7  "flocksafety"
#define FLOCK_KEYWORD_8  "FlockCam"
#define FLOCK_KEYWORD_9  "FS-"

// Stronger SSID/name pattern check for IDs like Flock-1A2B.
// This is more specific than the generic "flock" keyword.
#define FLOCK_STRICT_ID_PATTERN_ENABLED  1
#define FLOCK_STRICT_ID_MIN_HEX_CHARS    4

// Adaptive WiFi dwell for Flock sniffing.
// Channels 1, 6, and 11 get a longer dwell because many 2.4GHz networks
// sit there. Other channels use a shorter dwell for quicker coverage.
#define FLOCK_ADAPTIVE_DWELL_ENABLED     1
#define FLOCK_DWELL_MAIN_MS            500
#define FLOCK_DWELL_OTHER_MS           180

// 1 = dedupe Flock hits mainly by source MAC, 0 = dedupe by SSID only.
#define FLOCK_DEDUPE_BY_MAC  1

// 1 = show source MAC under each Flock hit row, 0 = compact one-line rows.
#define FLOCK_SHOW_SOURCE_MAC  1

// ─── Flock Upgrade Pass 1 ─────────────────────────────────────
// Adds expanded MAC/OUI matching, BLE manufacturer ID matching,
// confidence labels, method labels, and richer hit details.
#define FLOCK_MAC_PREFIX_MATCH_ENABLED      1
#define FLOCK_BLE_MFR_ID_MATCH_ENABLED      1
#define FLOCK_BLE_MFR_ID_XUNTONG       0x09C8
#define FLOCK_HYBRID_SHOW_HEARD_COUNT       1

// ─── Flock Hybrid Scanner ──────────────────────────────────────
// Combined BLE + WiFi scanner. It runs BLE first, then WiFi sniffing,
// and merges both hit types into one list.
#define MAX_FLOCK_HYBRID_HITS      30
#define FLOCK_HYBRID_BLE_SECS       8
#define FLOCK_HYBRID_WIFI_SECS     10
#define FLOCK_HYBRID_WIFI_HOP_MS  200
#define FLOCK_HYBRID_PRESET_DEFAULT  1   // 0=Quick, 1=Balanced, 2=Wide, 3=Deep, 4=Patient

// Presets used by Misc > Scan Defaults > Flock Hybrid.
// Each click cycles to the next set. 
#define FLOCK_HYBRID_PRESET_0_NAME  "Quick"
#define FLOCK_HYBRID_PRESET_0_BLE    5
#define FLOCK_HYBRID_PRESET_0_WIFI   6
#define FLOCK_HYBRID_PRESET_0_HOP  150

#define FLOCK_HYBRID_PRESET_1_NAME  "Balanced"
#define FLOCK_HYBRID_PRESET_1_BLE    FLOCK_HYBRID_BLE_SECS
#define FLOCK_HYBRID_PRESET_1_WIFI   FLOCK_HYBRID_WIFI_SECS
#define FLOCK_HYBRID_PRESET_1_HOP    FLOCK_HYBRID_WIFI_HOP_MS

#define FLOCK_HYBRID_PRESET_2_NAME  "Wide"
#define FLOCK_HYBRID_PRESET_2_BLE    8
#define FLOCK_HYBRID_PRESET_2_WIFI  15
#define FLOCK_HYBRID_PRESET_2_HOP  250

#define FLOCK_HYBRID_PRESET_3_NAME  "Deep"
#define FLOCK_HYBRID_PRESET_3_BLE   12
#define FLOCK_HYBRID_PRESET_3_WIFI  20
#define FLOCK_HYBRID_PRESET_3_HOP  300

#define FLOCK_HYBRID_PRESET_4_NAME  "Patient"
#define FLOCK_HYBRID_PRESET_4_BLE   15
#define FLOCK_HYBRID_PRESET_4_WIFI  25
#define FLOCK_HYBRID_PRESET_4_HOP  500
#define FLOCK_HYBRID_SHOW_MAC       1
// Teal slow APA102 spinner shown while the hybrid scan cycle is running.
#define FLOCK_HYBRID_LED_R          0
#define FLOCK_HYBRID_LED_G        180
#define FLOCK_HYBRID_LED_B        170
#define FLOCK_HYBRID_LED_SPIN_MS  160

// ─── BLE Scanner ────────────────────────────────────────────────
#define MAX_BLE_RESULTS   30
// BLE scan time is session-adjustable from Misc > Scan Defaults.
#define BLE_SCAN_SECS      8


// ─── Tesla Detector ─────────────────────────────────────────────
// Passive BLE name-pattern detector inspired by Esp32vsEvil/TeslaScanner.
// It only checks TESLA_NAME_END_INDEX when the BLE name length is long enough.
#define MAX_TESLA_RESULTS        20
#define TESLA_SCAN_SECS           8
#define TESLA_NAME_START_CHAR   'S'
#define TESLA_NAME_END_INDEX     17
#define TESLA_NAME_END_CHAR     'C'
#define TESLA_SHOW_FULL_MAC       1

// ─── AirTag Detector ────────────────────────────────────────────
// Adds GhostESP-style passive BLE payload pattern detection for
// Apple Find My / AirTag advertisements. This stays passive and
// does not connect to BLE devices.
#define AIRTAG_PAYLOAD_DETECT_ENABLED   1
#define AIRTAG_FINDMY_TYPE             0x12
#define AIRTAG_FINDMY_SUBTYPE          0x19
#define AIRTAG_NEARBY_TYPE             0x07
#define AIRTAG_APPLE_COMPANY_LE_0      0x4C
#define AIRTAG_APPLE_COMPANY_LE_1      0x00

// ─── Skimmer Detector ───────────────────────────────────────────
// Expanded passive BLE name matching based on GhostESP device detect logic.
// 1 = include the extended suspicious serial/BLE module names below.
#define SKIMMER_EXTENDED_NAMES_ENABLED  1
#define SKIMMER_NAME_MATCH_COUNT       12

static const char* SKIMMER_NAME_MATCHES[SKIMMER_NAME_MATCH_COUNT] = {
    "HC-03",
    "HC-05",
    "HC-06",
    "HC-08",
    "BT-HC05",
    "JDY-31",
    "AT-09",
    "HM-10",
    "CC41-A",
    "MLT-BT05",
    "SPP-CA",
    "FFD0"
};


// ─── nyanBOX Detector ─────────────────────────────────────────── (Credit to https://github.com/jbohack/nyanBOX)
// BLE-only detector for nyanBOX / Nyan Devices badges.
#define MAX_NYANBOX_RESULTS       30
#define NYANBOX_SCAN_SECS          8
#define NYANBOX_LOCATE_SCAN_SECS   2
#define NYANBOX_SERVICE_UUID      "6e79616e-424f-582d-7365-727669636521"

// ─── Axon Detector ────────────────────────────────────────────── (Credit to https://github.com/jbohack/nyanBOX)
// BLE-only detector for Axon-style BLE devices using the configured MAC/OUI prefix.
#define MAX_AXON_RESULTS          30
#define AXON_SCAN_SECS             8
#define AXON_LOCATE_SCAN_SECS      2
#define AXON_MAC_PREFIX           "00:25:df"
#define AXON_SHOW_FULL_MAC         1

// ─── Raven Detector ────────────────────────────────────────────
// BLE-only detector for Raven / SoundThinking-style gunshot sensors.
// Uses service UUID patterns from 0xXyc/flock-you-wifi-recon.
// Runtime tool: BLE Tools > Raven Detector.
#define MAX_RAVEN_RESULTS         30
#define RAVEN_SCAN_SECS            8
#define RAVEN_SHOW_FULL_MAC        1
#define RAVEN_DEVICE_INFO_SERVICE "0000180a-0000-1000-8000-00805f9b34fb"
#define RAVEN_GPS_SERVICE         "00003100-0000-1000-8000-00805f9b34fb"
#define RAVEN_POWER_SERVICE       "00003200-0000-1000-8000-00805f9b34fb"
#define RAVEN_NETWORK_SERVICE     "00003300-0000-1000-8000-00805f9b34fb"
#define RAVEN_UPLOAD_SERVICE      "00003400-0000-1000-8000-00805f9b34fb"
#define RAVEN_ERROR_SERVICE       "00003500-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_HEALTH_SERVICE  "00001809-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_LOCATION_SERVICE "00001819-0000-1000-8000-00805f9b34fb"

// ─── Flipper Detector ───────────────────────────────────────────
// Name matching stays enabled, and UUID detection adds the GhostESP-style
// passive BLE advertisement check for known Flipper BLE UUIDs.
#define FLIPPER_UUID_DETECT_ENABLED  1
#define FLIPPER_UUID_BLACK        0x3081
#define FLIPPER_UUID_WHITE        0x3082
#define FLIPPER_UUID_TRANSPARENT  0x3083

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
// 0 = Dark  |  1 = Flipper  |  2 = Matrix | 3 = POSEIDON and so on.
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

#define THEME_POSEIDON \
    "Poseidon", \
    0x000000, 0x211429, 0x080808, 0x212421, 0x101010, \
    0xffffff, 0x7b7d7b, 0x00ffff, \
    0x00ff00, 0xffff00, 0xff0000, \
    0x101010, 0x310039, 0xff00ff, \
    0x211429, 0x310039, 0xff00ff, \
    0x00ff00, 0xff0000

#define THEME_PHANTOM \
    "Phantom", \
    0x000000, 0x390042, 0x100021, 0x4a0084, 0x180021, \
    0xdedbde, 0x636163, 0xc600ff, \
    0x84ff84, 0xff7d00, 0xff0000, \
    0x180021, 0x290042, 0xc600ff, \
    0x390042, 0x290042, 0xc600ff, \
    0x84ff84, 0xff0000

#define THEME_AMBER \
    "Amber", \
    0x000000, 0x422000, 0x100800, 0x422000, 0x211000, \
    0xff9600, 0x633000, 0xff9600, \
    0xff9600, 0xff9600, 0xa56100, \
    0x211000, 0x211000, 0xff9600, \
    0x422000, 0x211000, 0xff9600, \
    0xff9600, 0xa56100

#define THEME_TRON \
    "Tron", \
    0x000000, 0x00205a, 0x000029, 0x009aff, 0x00204a, \
    0xbdffff, 0x292c39, 0x00ffff, \
    0x00ffff, 0xffff00, 0xff0000, \
    0x00204a, 0x00209c, 0x00ffff, \
    0x00205a, 0x00209c, 0x00ffff, \
    0x00ffff, 0xff0000

#define THEME_TYPER \
    "TypeR", \
    0x0b0b0b, 0x1a1a1a, 0x111111, 0x5a0000, 0x210000, \
    0xffffff, 0xb8b8b8, 0xff1a1a, \
    0xffffff, 0xffcc00, 0xff0000, \
    0x2a0000, 0x7a0000, 0xff1a1a, \
    0x3a0000, 0x7a0000, 0xff1a1a, \
    0xffffff, 0xff0000

#define THEME_JOKER \
    "Joker", \
    0x1a001a, 0x2e002e, 0x220022, 0x00ff66, 0x006600, \
    0xe0e0ff, 0x660066, 0x00cc33, \
    0x9900ff, 0xff00ff, 0xffcc00, \
    0x330033, 0x660066, 0x00ff66, \
    0x220022, 0x440044, 0x00cc33

    
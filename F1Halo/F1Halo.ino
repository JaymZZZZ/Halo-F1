const int DRIVERS_NUMBER = 22;

// ESP32 Boards used v3.3.4

// fix the results api for when there are changes that result in a lesser number of drivers
// add other rss feed in other languages
// add language switcher to wifi setup screen
// settings: add UTC offset modifier (for people using VPN or anyways if WiFi detection goes crazy)
// settings: make user decide if they want to see drivers standings, constructors or both one after the other in the main page (select tool)
// settings: add bool switch to control if tabs should be switched to news when a new article is fetched

// Board selector (single switch):
// - HALO_BOARD_JC8048W550 (default)
// - HALO_BOARD_JC4827W543
#define HALO_BOARD_JC4827W543 1
#define HALO_BOARD_JC8048W550 2
#define HALO_BOARD_MODEL HALO_BOARD_JC8048W550

// Optional UI scaling mode:
// When enabled (recommended for JC8048W550), the UI keeps the JC4827W543
// layout proportions and is scaled by the exact pixel delta to fit the panel.
#define HALO_ENABLE_PIXEL_DELTA_UI_SCALE 0

// HTTP debug logs (URL + response code + payload preview where applicable)
// Set to 0 to reduce Serial output noise.
#define HALO_HTTP_DEBUG 1

// Loop task stack debugging and headroom.
// Scaling-heavy LVGL render paths on ESP32-S3 can exceed the default loop stack.
#define HALO_STACK_DEBUG 0
#define HALO_LOOP_STACK_SIZE_BYTES (24 * 1024)

// Personal defaults
#define HALO_ENABLE_POPUP_NOTIFICATIONS 0
#define HALO_FORCE_DEFAULT_TIMEZONE 1
#define HALO_DEFAULT_TIMEZONE_NAME "America/Chicago"
#define HALO_DEFAULT_UTC_OFFSET_SECONDS (-6 * 3600)

#if HALO_BOARD_MODEL == HALO_BOARD_JC4827W543

#define HALO_BOARD_NAME "JC4827W543"
#define DISPLAY_TYPE DISPLAY_CYD_543
#define TOUCH_CAPACITIVE
#define TOUCH_SDA 8
#define TOUCH_SCL 4
#define TOUCH_INT 3
#define TOUCH_RST -1
#define TOUCH_MOSI 11
#define TOUCH_MISO 13
#define TOUCH_CLK 12
#define TOUCH_CS 38
#define TOUCH_MIN_X 1
#define TOUCH_MAX_X 480
#define TOUCH_MIN_Y 1
#define TOUCH_MAX_Y 272
#define SCREEN_WIDTH 272
#define SCREEN_HEIGHT 480

#elif HALO_BOARD_MODEL == HALO_BOARD_JC8048W550

#define HALO_BOARD_NAME "JC8048W550"
#define DISPLAY_TYPE DISPLAY_CYD_8048
#define TOUCH_CAPACITIVE
#define TOUCH_SDA 19
#define TOUCH_SCL 20
#define TOUCH_INT 18
#define TOUCH_RST 38
#define TOUCH_MOSI 11
#define TOUCH_MISO 13
#define TOUCH_CLK 12
#define TOUCH_CS 10
#define TOUCH_MIN_X 1
#define TOUCH_MAX_X 800
#define TOUCH_MIN_Y 1
#define TOUCH_MAX_Y 480
#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 800

#else
#error "Unsupported HALO_BOARD_MODEL value."
#endif

#define HALO_UI_BASE_WIDTH 272
#define HALO_UI_BASE_HEIGHT 480

#if HALO_BOARD_MODEL == HALO_BOARD_JC8048W550
// DISPLAY_CYD_8048 uses a virtual/RGB panel path in bb_spi_lcd where setRotation() is ignored.
// Use LVGL display rotation to force portrait logical coordinates (480x800).
#define HALO_LVGL_DISPLAY_ROTATION LV_DISPLAY_ROTATION_270
#define HALO_UI_ENABLE_CONTINUOUS_TEXT_SCROLL 0
#define HALO_UI_ENABLE_FADE_ANIMATIONS 0
#else
#define HALO_LVGL_DISPLAY_ROTATION LV_DISPLAY_ROTATION_0
#define HALO_UI_ENABLE_CONTINUOUS_TEXT_SCROLL 1
#define HALO_UI_ENABLE_FADE_ANIMATIONS 1
#endif

#if HALO_BOARD_MODEL == HALO_BOARD_JC8048W550 && HALO_ENABLE_PIXEL_DELTA_UI_SCALE
#define HALO_UI_SCALE_ACTIVE 1
#else
#define HALO_UI_SCALE_ACTIVE 0
#endif

#if HALO_UI_SCALE_ACTIVE
#define HALO_UI_LAYOUT_WIDTH HALO_UI_BASE_WIDTH
#define HALO_UI_LAYOUT_HEIGHT HALO_UI_BASE_HEIGHT
#define HALO_UI_SCALE_X ((SCREEN_WIDTH * 256 + (HALO_UI_BASE_WIDTH / 2)) / HALO_UI_BASE_WIDTH)
#define HALO_UI_SCALE_Y ((SCREEN_HEIGHT * 256 + (HALO_UI_BASE_HEIGHT / 2)) / HALO_UI_BASE_HEIGHT)
#else
#define HALO_UI_LAYOUT_WIDTH SCREEN_WIDTH
#define HALO_UI_LAYOUT_HEIGHT SCREEN_HEIGHT
#define HALO_UI_SCALE_X 256
#define HALO_UI_SCALE_Y 256
#endif

#if HALO_BOARD_MODEL == HALO_BOARD_JC8048W550
static const uint32_t HALO_RECOMMENDED_FLASH_BYTES = 16UL * 1024UL * 1024UL;
static const uint32_t HALO_RECOMMENDED_PSRAM_BYTES = 8UL * 1024UL * 1024UL;
#else
static const uint32_t HALO_RECOMMENDED_FLASH_BYTES = 4UL * 1024UL * 1024UL;
static const uint32_t HALO_RECOMMENDED_PSRAM_BYTES = 8UL * 1024UL * 1024UL;
#endif

#ifdef TOUCH_CAPACITIVE
const String fw_version = "1.2.0";
#else
const String fw_version = "1.2.0-R";
#endif


#include <ArduinoJson.h>
#include <time.h>
#include "esp_heap_caps.h"

// Increase Arduino loopTask stack to avoid canary trips in scaled LVGL draw paths.
SET_LOOP_TASK_STACK_SIZE(HALO_LOOP_STACK_SIZE_BYTES);

// WIFI MANAGEMENT
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
WiFiManager wm;

// LVGL
#include <lvgl.h> // v9.3.0
#include "lv_conf.h"
LV_FONT_DECLARE(f1_symbols_28);
LV_FONT_DECLARE(weather_icons_12);
LV_FONT_DECLARE(montserrat_12);
LV_FONT_DECLARE(montserrat_14);
LV_FONT_DECLARE(montserrat_18);
LV_FONT_DECLARE(montserrat_20);
LV_FONT_DECLARE(montserrat_24);
LV_FONT_DECLARE(montserrat_38);

#if HALO_BOARD_MODEL == HALO_BOARD_JC8048W550
#define HALO_FONT_XS &montserrat_14
#define HALO_FONT_SM &montserrat_18
#define HALO_FONT_MD &montserrat_20
#define HALO_FONT_LG &montserrat_24
#define HALO_FONT_XL &montserrat_38
#else
#define HALO_FONT_XS &montserrat_12
#define HALO_FONT_SM &montserrat_14
#define HALO_FONT_MD &montserrat_18
#define HALO_FONT_LG &montserrat_24
#define HALO_FONT_XL &montserrat_38
#endif


#define F1_SYMBOL_RANKING "\xEE\x95\xA1"
#define F1_SYMBOL_CHEQUERED_FLAG "\xEF\x84\x9E"
#define F1_SYMBOL_BARS "\xEF\x83\x89" 
#define F1_SYMBOL_GEAR "\xEF\x80\x93"
#define F1_SYMBOL_GEARS "\xEF\x82\x85"
#define F1_SYMBOL_SLIDERS "\xEF\x87\x9E"
#define F1_SYMBOL_WRENCH "\xEF\x82\xAD"
#define F1_SYMBOL_HAMMER "\xEF\x9B\xA3"

// Weather icon symbols (Font Awesome 7 Free Solid, font: weather_icons_16)
#define WX_SYMBOL_SUN           "\xEF\x86\x85"  // U+F185 fa-sun
#define WX_SYMBOL_CLOUD_SUN     "\xEF\x9B\x84"  // U+F6C4 fa-cloud-sun
#define WX_SYMBOL_CLOUD         "\xEF\x83\x82"  // U+F0C2 fa-cloud
#define WX_SYMBOL_SMOG          "\xEF\x9D\x9F"  // U+F75F fa-smog
#define WX_SYMBOL_DRIZZLE       "\xEF\x9C\xBD"  // U+F73D fa-cloud-rain
#define WX_SYMBOL_RAIN          "\xEF\x9D\x80"  // U+F740 fa-cloud-showers-heavy
#define WX_SYMBOL_SNOW          "\xEF\x8B\x9C"  // U+F2DC fa-snowflake
#define WX_SYMBOL_STORM         "\xEF\x83\xA7"  // U+F0E7 fa-bolt

#define HALO_COLOR_RED 0xFF1511

long UTCoffset;
int32_t UTCoffsetHours, UTCoffsetMinutes;

struct TimeRoller {
  lv_obj_t * hours;
  lv_obj_t * minutes;
};

TimeRoller nightModeStartRoller, nightModeStopRoller;

struct NightModeTimes {
  uint8_t start_hours;
  uint8_t start_minutes;
  uint8_t stop_hours;
  uint8_t stop_minutes;
};

NightModeTimes nightModeTimes = {23, 0, 8, 0};

bool nightModeActive = true;

struct DriverStanding {
  String position;
  String points;
  String number;
  String name;
  String surname;
  String constructor;
  String constructorId;
};

struct TeamStandings {
  String position;
  String points;
  String name;
  String id;
};

struct SeasonStanding {
  String season;
  String round;
  DriverStanding driver_standings[30];
  TeamStandings team_standings[12];
  int driver_count;
  int team_count;
};

SeasonStanding current_season;

int driverStandingsCount = 0;

struct RaceSession {
    String name;   // e.g., "FP1", "Sprint", "Race"
    String date;   // Local date after offset applied
    String time;   // Local time after offset applied
};

struct NextRaceInfo {
    String raceName;
    String circuitName;
    String country;
    float lat;   // circuit latitude  (from Jolpi API)
    float lon;   // circuit longitude (from Jolpi API)
    bool isSprintWeekend;
    int sessionCount;
    RaceSession sessions[10]; // Usually no more than 6
};

NextRaceInfo next_race;

struct SessionResults {
  String driver_number;
  String position;
  float duration;          // for race (seconds)
  float quali[3];          // for qualifying (Q1, Q2, Q3)
  float gap_to_leader;
  float gap_to_leader_quali[3];
  bool isQualifying;
  bool dnf;
  bool dns;
};

SessionResults results[DRIVERS_NUMBER];
String current_results, last_results;
bool results_checked_once = false, results_loaded_once = false, standings_loaded_once = false, got_new_results = false;

unsigned long long last_checked_session_results = 0;
unsigned int check_delay = 0;

lv_display_t * disp;
lv_timer_t * clock_timer, * f1_api_timer, * standings_ui_timer, *news_timer, *statistics_timer, *notifications_timer;

lv_obj_t * sessions_container, * standings_container;

// Settings stuff
lv_obj_t * language_selector; // localized_text defined in localized_strings.h
lv_obj_t * no_spoiler_switch; bool noSpoilerModeActive = true;
lv_obj_t * brightness_slider, *night_brightness_slider; uint8_t brightness = 255, night_brightness = 30;

// No-Spoiler lift state (not a setting — temporary per-session override)
bool   noSpoilerLifted            = false; // true after user presses "Show"
String noSpoilerLiftedForSession  = "";    // session name that was active when lifted
String noSpoilerLastKnownSession  = "";    // updated each render cycle; read by the button callback
bool   noSpoilerWasStandings      = false; // true = was hiding standings; false = was hiding results

static int standings_offset = 0;
const int STANDINGS_PAGE_SIZE = 5;
const int TOTAL_DRIVERS = 22; // adjust if needed

struct ScreenStruct {
  lv_obj_t * wifi;       // screen object
  lv_obj_t * home;       // screen object
  lv_obj_t * wifi_root;  // scaled UI root (or same as screen when scaling is disabled)
  lv_obj_t * home_root;  // scaled UI root (or same as screen when scaling is disabled)
  //lv_obj_t * settings;
};

ScreenStruct screen;

struct RaceTabLabelsStruct {
  lv_obj_t * clock;
  lv_obj_t * date;
  lv_obj_t * race_name;
};

RaceTabLabelsStruct racetab_labels;

lv_obj_t * home_tabs;

struct TabsStruct {
  lv_obj_t * race;
  lv_obj_t * news;
  lv_obj_t * settings;
};

TabsStruct tabs;

// LCD SCREEN
#include <bb_spi_lcd.h> // v2.7.1
#include "lv_bb_spi_lcd.h"
#include "touchscreen.h"
// FILES
#include "audio.h"
#include "localized_strings.h"
#include "utils.h"
#include "notifications.h"
#include "weather.h"      // ← weather forecast (Open-Meteo, no API key)
#include "ui.h"
#include "wifi_handler.h" // WiFiManager by Tzapu v2.0.17

void printMemoryConfigReport() {
  uint32_t flashBytes = ESP.getFlashChipSize();
  uint32_t psramBytes = ESP.getPsramSize();
  bool hasPsram = psramFound();

  Serial.printf("[HW] Board profile: %s\n", HALO_BOARD_NAME);
  Serial.printf("[HW] Flash detected: %u MB\n", (unsigned)(flashBytes / (1024UL * 1024UL)));
  if (hasPsram) {
    Serial.printf("[HW] PSRAM detected: %u MB\n", (unsigned)(psramBytes / (1024UL * 1024UL)));
  } else {
    Serial.println("[HW] PSRAM detected: none");
  }

  if (flashBytes < HALO_RECOMMENDED_FLASH_BYTES) {
    Serial.printf("[HW][WARN] Flash is smaller than recommended for %s. Recommended >= %u MB.\n",
                  HALO_BOARD_NAME, (unsigned)(HALO_RECOMMENDED_FLASH_BYTES / (1024UL * 1024UL)));
  }

  if (!hasPsram || psramBytes < HALO_RECOMMENDED_PSRAM_BYTES) {
    Serial.printf("[HW][WARN] PSRAM is smaller than recommended for %s. Recommended >= %u MB.\n",
                  HALO_BOARD_NAME, (unsigned)(HALO_RECOMMENDED_PSRAM_BYTES / (1024UL * 1024UL)));
  }
}


void setup() {
  Serial.begin(115200);
  delay(100);
  printMemoryConfigReport();
  if (psramFound()) {
    // Let malloc spill to PSRAM aggressively so TLS handshakes keep enough
    // internal heap available on ESP32-S3.
    heap_caps_malloc_extmem_enable(0);
    Serial.println("[HW] Enabled malloc() external-memory fallback");
  }
  //debug = &Serial;

  localized_text = &language_strings_en;

  // Initialise LVGL
  lv_init();
  lv_tick_set_cb([](){ 
    return (uint32_t) (esp_timer_get_time() / 1000ULL);
  });
  disp = lv_bb_spi_lcd_create(DISPLAY_TYPE);
  lv_display_set_rotation(disp, HALO_LVGL_DISPLAY_ROTATION);
  Serial.printf("[HW] LVGL display rotation: %d\n", (int)HALO_LVGL_DISPLAY_ROTATION);

#ifdef TOUCH_CAPACITIVE
  // Initialize touch screen
  bbct.init(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);
  //bbct.setOrientation(270, SCREEN_WIDTH, SCREEN_HEIGHT);
#else
  lv_bb_spi_lcd_t* dsc = (lv_bb_spi_lcd_t *)lv_display_get_driver_data(disp);
  lcd = dsc->lcd;
  lcd->rtInit(TOUCH_MOSI, TOUCH_MISO, TOUCH_CLK, TOUCH_CS);
#endif

  // Register touch
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);  
  lv_indev_set_display(indev, disp);
  lv_indev_set_read_cb(indev, touch_read);

  playNotificationSound();

  create_ui_skeleton();

  setupWiFiManager(false);

  post_wifi_ui_creation();

  lv_screen_load(screen.home);

  String uuid = getDeviceUUID();
  Serial.println("Device UUID: " + uuid);
  Serial.println("Device FW: " + fw_version);

  sendStatisticData(nullptr);

  Serial.println("Setup done");
}

void loop() {   
  lv_timer_periodic_handler();
#if HALO_STACK_DEBUG
  static uint32_t lastStackLogMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastStackLogMs >= 10000UL) {
    lastStackLogMs = nowMs;
    UBaseType_t words = uxTaskGetStackHighWaterMark(NULL);
    Serial.printf("[Stack] loopTask watermark: free=%u bytes\n", (unsigned)(words * sizeof(StackType_t)));
  }
#endif
  delay(5);
}

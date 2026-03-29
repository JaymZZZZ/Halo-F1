const int DRIVERS_NUMBER = 22;

// ESP32 Boards used v3.3.4

// fix the results api for when there are changes that result in a lesser number of drivers
// add other rss feed in other languages
// add language switcher to wifi setup screen
// settings: add UTC offset modifier (for people using VPN or anyways if WiFi detection goes crazy)
// settings: make user decide if they want to see drivers standings, constructors or both one after the other in the main page (select tool)
// settings: add bool switch to control if tabs should be switched to news when a new article is fetched

#include "board_config.h"

const String fw_version = "1.2.0";


#include <ArduinoJson.h>
#include <time.h>

// WIFI MANAGEMENT
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <esp_bt.h>
WiFiManager wm;

// LVGL
#include <lvgl.h> // v9.3.0
#include "lv_conf.h"
LV_FONT_DECLARE(f1_symbols_28);
LV_FONT_DECLARE(weather_icons_12);
LV_FONT_DECLARE(weather_icons_14);
LV_FONT_DECLARE(weather_icons_16);
LV_FONT_DECLARE(weather_icons_18);
LV_FONT_DECLARE(montserrat_12);
LV_FONT_DECLARE(montserrat_14);
LV_FONT_DECLARE(montserrat_18);
LV_FONT_DECLARE(montserrat_20);
LV_FONT_DECLARE(montserrat_24);
LV_FONT_DECLARE(montserrat_38);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_40);


#define F1_SYMBOL_RANKING "\xEE\x95\xA1"
#define F1_SYMBOL_CHEQUERED_FLAG "\xEF\x84\x9E"
#define F1_SYMBOL_BARS "\xEF\x83\x89" 
#define F1_SYMBOL_GEAR "\xEF\x80\x93"
#define F1_SYMBOL_GEARS "\xEF\x82\x85"
#define F1_SYMBOL_SLIDERS "\xEF\x87\x9E"
#define F1_SYMBOL_WRENCH "\xEF\x82\xAD"
#define F1_SYMBOL_HAMMER "\xEF\x9B\xA3"

// Weather icon symbols (Font Awesome Free Solid, font: weather_icons_18)
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

bool nightModeActive = false;

struct DriverStanding {
  String position;
  String points;
  String number;
  String name;
  String surname;
  String nationality;
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
lv_timer_t * clock_timer, * f1_api_timer, * standings_ui_timer, *news_timer, *statistics_timer, *notifications_timer, *memory_maintenance_timer;
bool race_ui_refresh_pending = true;
unsigned long last_f1_api_attempt_ms = 0;
unsigned long last_f1_api_success_ms = 0;

lv_obj_t * sessions_container, * standings_container;

// Settings stuff
lv_obj_t * language_selector; // localized_text defined in localized_strings.h
lv_obj_t * no_spoiler_switch; bool noSpoilerModeActive = false;
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
  lv_obj_t * wifi;
  lv_obj_t * home;
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

// LCD + TOUCH (ESP32_Display_Panel)
#include "lv_esp_panel.h"
// FILES
#include "audio.h"
#include "localized_strings.h"
#include "utils.h"
#include "notifications.h"
#include "weather.h"      // ← weather forecast (Open-Meteo, no API key)
#include "ui.h"
#include "wifi_handler.h" // WiFiManager by Tzapu v2.0.17


void setup() {
  Serial.begin(115200);
  //debug = &Serial;

  // Bluetooth is unused in Halo-F1; release BT stack memory to improve TLS headroom.
  esp_err_t btRelease = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
  (void)btRelease;

  localized_text = &language_strings_en_us;

  // Initialise LVGL
  lv_init();
  lv_tick_set_cb([](){ 
    return (uint32_t) (esp_timer_get_time() / 1000ULL);
  });
  disp = halo_panel_display_create();
  if (!disp) {
    Serial.println("[HW] Failed to create LCD driver");
    delay(3000);
    ESP.restart();
  }

  // Register touch
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);  
  lv_indev_set_read_cb(indev, halo_panel_touch_read);
  playNotificationSound();

  create_ui_skeleton();

  setupWiFiManager(false);

  post_wifi_ui_creation();

  lv_screen_load(screen.home);
  lv_obj_invalidate(screen.home);
  lv_timer_periodic_handler();

  String uuid = getDeviceUUID();
  (void)uuid;

  sendStatisticData(nullptr);
}

void loop() {   
  lv_timer_periodic_handler();
  halo_panel_diag_tick();
  delay(5);
}

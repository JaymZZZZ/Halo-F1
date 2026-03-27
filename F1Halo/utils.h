// forward declaration
String current_f1_champion = "4"; // Norris
void create_or_reload_race_sessions(bool force_reload = false);
void adjustBrightness(uint8_t brightness);
void create_or_reload_settings_ui();
void sendStatisticData(lv_timer_t *timer);

String getDeviceUUID() {
  uint64_t chipid = ESP.getEfuseMac();  // unique 48-bit ID from eFuse

  char uuid[37]; // 36 chars + null terminator

  // Format into UUID v4 style string
  snprintf(uuid, sizeof(uuid),
           "%08lx-%04lx-%04lx-%04lx-%012llx",
           (unsigned long)((chipid >> 32) & 0xFFFFFFFF),
           (unsigned long)((chipid >> 16) & 0xFFFF),
           (unsigned long)(0x4000 | ((chipid >> 0) & 0x0FFF)), // version 4 marker
           (unsigned long)(0x8000 | ((chipid >> 48) & 0x3FFF)), // variant marker
           (unsigned long long)(chipid & 0xFFFFFFFFFFFFULL));

  return String(uuid);
}

// Formats Lap Time from seconds to M:SS.sss
String formatLapTime(float seconds) {
  int mins = (int)(seconds / 60);          // whole minutes
  float secs = fmod(seconds, 60.0);        // remainder seconds
  char buffer[16];

  // Format: M:SS.sss (zero-pad seconds to 2 digits, milliseconds to 3 digits)
  sprintf(buffer, "%d:%06.3f", mins, secs);

  return String(buffer);
}

static void setUtcOffsetComponents(int64_t offsetSeconds) {
  int64_t absSeconds = llabs(offsetSeconds);
  int32_t hours = (int32_t)(absSeconds / 3600);
  int32_t minutes = (int32_t)((absSeconds % 3600) / 60);

  UTCoffsetHours = (offsetSeconds < 0) ? -hours : hours;
  UTCoffsetMinutes = (offsetSeconds < 0) ? -minutes : minutes;
}

static bool copyCurrentTZ(char *tzOut, size_t tzOutLen) {
  if (!tzOut || tzOutLen == 0) return false;
  tzOut[0] = '\0';
  const char *currentTZ = getenv("TZ");
  if (!currentTZ) return false;
  strncpy(tzOut, currentTZ, tzOutLen - 1);
  tzOut[tzOutLen - 1] = '\0';
  return true;
}

static void restoreTZ(const char *tzBackup, bool hadTZ) {
  if (hadTZ) {
    setenv("TZ", tzBackup, 1);
  } else {
    unsetenv("TZ");
  }
  tzset();
}

time_t timegm(struct tm* tm);

// API call to get current timezone offset
int64_t getUtcOffsetInSeconds() {
#if HALO_FORCE_CHICAGO_TZ
  // America/Chicago with automatic DST transitions.
  static const char *HALO_CHICAGO_TZ = "CST6CDT,M3.2.0/2,M11.1.0/2";

  char previousTZ[96];
  bool hadTZ = copyCurrentTZ(previousTZ, sizeof(previousTZ));

  setenv("TZ", HALO_CHICAGO_TZ, 1);
  tzset();

  time_t nowUTC = time(nullptr);
  if (nowUTC <= 1000) {
    restoreTZ(previousTZ, hadTZ);
    setUtcOffsetComponents(HALO_DEFAULT_UTC_OFFSET_SECONDS);
    return HALO_DEFAULT_UTC_OFFSET_SECONDS;
  }

  struct tm chicagoLocal = {};
  localtime_r(&nowUTC, &chicagoLocal);

  // Treat local wall clock as UTC to infer offset seconds from UTC epoch.
  time_t chicagoAsUTC = timegm(&chicagoLocal);
  int64_t offsetSeconds = (int64_t)(chicagoAsUTC - nowUTC);

  restoreTZ(previousTZ, hadTZ);
  setUtcOffsetComponents(offsetSeconds);

  return offsetSeconds;
#else
  HTTPClient client;

  //debug->println("Getting local time zone");
  std::string url = "https://ipapi.co/utc_offset/";
  client.begin(url.c_str());
  int statusCode = client.GET();
  //debug->println("Status code: %i", statusCode);
  if (statusCode != 200 && statusCode != 429 && statusCode != -11) {
    //debug.println("Error getting local time zone, status code: %i\n", statusCode);
    return 0;
  }

  std::string offset;

  if (statusCode == 429 || statusCode == -11) {
    offset = "+0000"; // defaults to UTC
  } else {
    offset = std::string(client.getString().c_str());
  }

  client.end();

  //debug->println("Offset: %s", offset.c_str());
  char sign = offset[0];
  uint32_t hours = std::stoi(offset.substr(1, 2));
  uint32_t minutes = std::stoi(offset.substr(3, 2));
  int64_t result = (hours * 3600) + minutes * 60.0;

  if (sign == '-') {
    result = -result;
  }

  setUtcOffsetComponents(result);
  //debug->print("Offset in seconds: "); debug->println(result);
  return result;
#endif
}

// Arduino didn't have this basic one so we had to make do...
time_t timegm(struct tm* tm) {
  // Save the current TZ
  char oldTZ[96];
  bool hadTZ = copyCurrentTZ(oldTZ, sizeof(oldTZ));
  setenv("TZ", "", 1); // Empty string means UTC
  tzset();

  time_t result = mktime(tm);

  // Restore the previous TZ
  restoreTZ(oldTZ, hadTZ);

  return result;
}

static bool parse_utc_session_tm(const String &utcSessionDate,
                                 const String &utcSessionTime,
                                 struct tm *outTm) {
  if (outTm == nullptr) return false;

  int year = 0, month = 0, day = 0;
  if (sscanf(utcSessionDate.c_str(), "%d-%d-%d", &year, &month, &day) != 3) {
    return false;
  }

  char timeBuf[16];
  const char *timeStr = utcSessionTime.c_str();
  size_t timeLen = strnlen(timeStr, sizeof(timeBuf) - 1);
  memcpy(timeBuf, timeStr, timeLen);
  timeBuf[timeLen] = '\0';

  // Drop trailing UTC marker.
  if (timeLen > 0 && timeBuf[timeLen - 1] == 'Z') {
    timeBuf[timeLen - 1] = '\0';
    timeLen--;
  }

  // Drop fractional seconds (e.g. "14:00:00.000").
  for (size_t i = 0; i < timeLen; i++) {
    if (timeBuf[i] == '.') {
      timeBuf[i] = '\0';
      timeLen = i;
      break;
    }
  }

  // Normalize HH:MM -> HH:MM:SS
  if (timeLen == 5) {
    if ((timeLen + 3) >= sizeof(timeBuf)) return false;
    timeBuf[5] = ':';
    timeBuf[6] = '0';
    timeBuf[7] = '0';
    timeBuf[8] = '\0';
  } else if (timeLen > 8) {
    timeBuf[8] = '\0';
  }

  int hour = 0, minute = 0, second = 0;
  if (sscanf(timeBuf, "%d:%d:%d", &hour, &minute, &second) != 3) {
    return false;
  }

  memset(outTm, 0, sizeof(*outTm));
  outTm->tm_year = year - 1900;
  outTm->tm_mon = month - 1;
  outTm->tm_mday = day;
  outTm->tm_hour = hour;
  outTm->tm_min = minute;
  outTm->tm_sec = second;
  outTm->tm_isdst = 0;
  return true;
}

// Tells if a session has already started given the strings for date and time retrieved from API
bool hasSessionStarted(const String &utcSessionDate, const String &utcSessionTime) {
    struct tm tmUTC = {};
    if (!parse_utc_session_tm(utcSessionDate, utcSessionTime, &tmUTC)) return false;
    
    time_t sessionEpoch = timegm(&tmUTC); // UTC epoch
    time_t nowUTC = time(nullptr); // Already UTC because gmtOffset_sec = 0

    return sessionEpoch <= nowUTC ? true : false;
}

// Tells if FP finished, used for avoiding unnecessary API calls, it waits a few minutes longer than an hour
bool hasFreePracticeFinished(const String &utcSessionDate, const String &utcSessionTime) {
    struct tm tmUTC = {};
    if (!parse_utc_session_tm(utcSessionDate, utcSessionTime, &tmUTC)) return false;
    
    time_t sessionEpoch = timegm(&tmUTC); // UTC epoch
    time_t nowUTC = time(nullptr); // Already UTC because gmtOffset_sec = 0

    return (nowUTC - sessionEpoch) >= 3900;
}

// Retrieves the next (unstarted) session from the race weekend sessions
RaceSession getNextSession(NextRaceInfo &race) {
  RaceSession session;

  for (int i=0; i<race.sessionCount; i++) {
    session = race.sessions[i];
    bool started = hasSessionStarted(session.date, session.time);

    if (!started) {
      return session;
    }
  }

  return session;
}

// Localizes session names (handy shortcut)
const char* getLocalizedSessionName(RaceSession &session) {
  if (session.name == "FP1") return localized_text->FP1;
  if (session.name == "FP2") return localized_text->FP2;
  if (session.name == "FP3") return localized_text->FP3;
  if (session.name == "Qualifying") return localized_text->qualifying;
  if (session.name == "Race") return localized_text->race;
  if (session.name == "Sprint Qualifying") return localized_text->sprint_q;
  if (session.name == "Sprint Race") return localized_text->sprint_race;

  return session.name.c_str();
}

static bool isUsEnglishLanguageSelected() {
  return localized_text == &language_strings_en_us;
}

static const char* getEnglishOrdinalSuffix(int day) {
  const int mod100 = day % 100;
  if (mod100 >= 11 && mod100 <= 13) return "th";
  switch (day % 10) {
    case 1: return "st";
    case 2: return "nd";
    case 3: return "rd";
    default: return "th";
  }
}

static void formatLocalizedDate(const struct tm &tmValue, char *out, size_t outLen) {
  if (!out || outLen == 0) return;

  if (isUsEnglishLanguageSelected()) {
    snprintf(out, outLen, "%s, %s %d%s",
             localized_text->short_days[tmValue.tm_wday],
             localized_text->months[tmValue.tm_mon],
             tmValue.tm_mday,
             getEnglishOrdinalSuffix(tmValue.tm_mday));
    return;
  }

  snprintf(out, outLen, "%s %d, %s",
           localized_text->short_days[tmValue.tm_wday],
           tmValue.tm_mday,
           localized_text->months[tmValue.tm_mon]);
}

// Results API doesn't give driver infos, this retrieves them from the saved standings to avoid another API call
DriverStanding* getDriverInfoByNumber(const String& driverNumber) {
  for (int i = 0; i < current_season.driver_count; i++) {
    //Serial.printf("Driver number: %s\n", current_season.driver_standings[i].number);
    if (current_season.driver_standings[i].number == driverNumber || current_season.driver_standings[i].number == current_f1_champion && driverNumber == "1") {
      return &current_season.driver_standings[i];  // return pointer to the matching struct
    }
  }
  return nullptr; // not found
}

// Formats session datetime 
// @param -> iWant = "all", "date", "time"
char* getSessionDateTimeFormatted(const String &utcSessionDate,
                                  const String &utcSessionTime,
                                  const char *iWant = "all") {
    static char formatted[50]; // static so it persists after return

    struct tm sessionTime = {};
    if (!parse_utc_session_tm(utcSessionDate, utcSessionTime, &sessionTime)) {
        formatted[0] = '\0';
        return formatted;
    }

    // Convert to time_t in UTC
    time_t sessionEpoch = timegm(&sessionTime);

    // Apply offset in seconds
    sessionEpoch += UTCoffset;

    // Convert back to local tm with rollover handled
    struct tm adjustedTime;
    gmtime_r(&sessionEpoch, &adjustedTime);

    if (iWant != nullptr && strcmp(iWant, "date") == 0) {
        if (isUsEnglishLanguageSelected()) {
            snprintf(formatted, sizeof(formatted), "%s, %s %d%s",
                    localized_text->short_days[adjustedTime.tm_wday],
                    localized_text->months[adjustedTime.tm_mon],
                    adjustedTime.tm_mday,
                    getEnglishOrdinalSuffix(adjustedTime.tm_mday));
        } else {
            snprintf(formatted, sizeof(formatted), "%s %d %s",
                    localized_text->short_days[adjustedTime.tm_wday],
                    adjustedTime.tm_mday,
                    localized_text->months[adjustedTime.tm_mon]);
        }
    } else if (iWant != nullptr && strcmp(iWant, "time") == 0) {
        snprintf(formatted, sizeof(formatted), "%02d:%02d",
                 adjustedTime.tm_hour,
                 adjustedTime.tm_min);
    } else {
        if (isUsEnglishLanguageSelected()) {
            snprintf(formatted, sizeof(formatted), "%s, %s %d%s, %02d:%02d",
                    localized_text->short_days[adjustedTime.tm_wday],
                    localized_text->months[adjustedTime.tm_mon],
                    adjustedTime.tm_mday,
                    getEnglishOrdinalSuffix(adjustedTime.tm_mday),
                    adjustedTime.tm_hour,
                    adjustedTime.tm_min);
        } else {
            snprintf(formatted, sizeof(formatted), "%s %d %s, %02d:%02d",
                    localized_text->short_days[adjustedTime.tm_wday],
                    adjustedTime.tm_mday,
                    localized_text->months[adjustedTime.tm_mon],
                    adjustedTime.tm_hour,
                    adjustedTime.tm_min);
        }
    }

    return formatted;
}

// Calls NTP Server to update internal clock
void update_internal_clock() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  getLocalTime(&timeinfo, 3000);
  UTCoffset = (long)getUtcOffsetInSeconds();
}

// Runs with lvgl timer, updates internal clock with offset, updates UI display of clock, date, race name and sessions
void update_ui(lv_timer_t *timer) {
  LV_UNUSED(timer);
  static int last_race_refresh_bucket = -1;
  struct tm timeinfo;
  
  if (!getLocalTime(&timeinfo)) return;

#if HALO_FORCE_CHICAGO_TZ
  UTCoffset = (long)getUtcOffsetInSeconds();
#endif

  time_t timeEpoch = timegm(&timeinfo);

  if (timeEpoch <= 1000) return;

  // Apply offset in seconds
  timeEpoch += UTCoffset;
  //timeEpoch += 60; //time correction (clock is dragging a tiny bit)


  // Convert back to local tm with rollover handled
  struct tm adjustedTime;
  gmtime_r(&timeEpoch, &adjustedTime);

  //if (timeinfo.tm_min % 60 == 0) update_internal_clock();
  if (racetab_labels.clock) lv_label_set_text_fmt(racetab_labels.clock, "%02d:%02d", adjustedTime.tm_hour, adjustedTime.tm_min);
  if (racetab_labels.date) {
    char dateFormatted[40];
    formatLocalizedDate(adjustedTime, dateFormatted, sizeof(dateFormatted));
    lv_label_set_text(racetab_labels.date, dateFormatted);
  }
  if (racetab_labels.race_name) lv_label_set_text_fmt(racetab_labels.race_name, "%s", next_race.raceName.c_str());

  // Rebuild race/session widgets at most every 5 minutes unless a data refresh requested it.
  const int refresh_bucket = adjustedTime.tm_min / 5;
  if (race_ui_refresh_pending || (refresh_bucket != last_race_refresh_bucket)) {
    create_or_reload_race_sessions();
    race_ui_refresh_pending = false;
    last_race_refresh_bucket = refresh_bucket;
  }

  // NIGHT MODE
  if (nightModeActive) {
    if (adjustedTime.tm_hour == nightModeTimes.start_hours && adjustedTime.tm_min == nightModeTimes.start_minutes) {
      adjustBrightness(night_brightness);
    }
    if (adjustedTime.tm_hour == nightModeTimes.stop_hours && adjustedTime.tm_min == nightModeTimes.stop_minutes) {
      adjustBrightness(brightness);
    }
  }
}

void force_update_ui() {
  struct tm timeinfo;
  
  if (!getLocalTime(&timeinfo)) return;

#if HALO_FORCE_CHICAGO_TZ
  UTCoffset = (long)getUtcOffsetInSeconds();
#endif

  time_t timeEpoch = timegm(&timeinfo);

  if (timeEpoch <= 1000) return;

  // Apply offset in seconds
  timeEpoch += UTCoffset;
  //timeEpoch += 60; //time correction (clock is dragging a tiny bit)


  // Convert back to local tm with rollover handled
  struct tm adjustedTime;
  gmtime_r(&timeEpoch, &adjustedTime);

  //if (timeinfo.tm_min % 60 == 0) update_internal_clock();
  if (racetab_labels.clock) lv_label_set_text_fmt(racetab_labels.clock, "%02d:%02d", adjustedTime.tm_hour, adjustedTime.tm_min);
  if (racetab_labels.date) {
    char dateFormatted[40];
    formatLocalizedDate(adjustedTime, dateFormatted, sizeof(dateFormatted));
    lv_label_set_text(racetab_labels.date, dateFormatted);
  }
  if (racetab_labels.race_name) lv_label_set_text_fmt(racetab_labels.race_name, "%s", next_race.raceName.c_str());

  create_or_reload_race_sessions( true );
  race_ui_refresh_pending = false;

  // NIGHT MODE
  if (nightModeActive) {
    if (adjustedTime.tm_hour == nightModeTimes.start_hours && adjustedTime.tm_min == nightModeTimes.start_minutes) {
      adjustBrightness(night_brightness);
    }
    if (adjustedTime.tm_hour == nightModeTimes.stop_hours && adjustedTime.tm_min == nightModeTimes.stop_minutes) {
      adjustBrightness(brightness);
    }
  }
}

// Tells if the first session of the race weekend has started
bool hasRaceWeekendStarted() {
  if (next_race.sessionCount <= 0) return false;
  return hasSessionStarted(next_race.sessions[0].date, next_race.sessions[0].time);
}

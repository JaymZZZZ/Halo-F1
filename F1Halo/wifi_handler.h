#include <esp_heap_caps.h>

// Memory maintenance (ESP32 has no GC; this reduces long-run heap fragmentation).
#ifndef HALO_MEMORY_MAINTENANCE_PERIOD_MS
#define HALO_MEMORY_MAINTENANCE_PERIOD_MS (2UL * 60UL * 1000UL)
#endif

#ifndef HALO_MEMORY_GUARD_HEAP_THRESHOLD
#define HALO_MEMORY_GUARD_HEAP_THRESHOLD (30000U)
#endif

#ifndef HALO_MEMORY_GUARD_INTERNAL_THRESHOLD
#define HALO_MEMORY_GUARD_INTERNAL_THRESHOLD (14000U)
#endif

#ifndef HALO_MEMORY_GUARD_CONSECUTIVE_HITS
#define HALO_MEMORY_GUARD_CONSECUTIVE_HITS (4U)
#endif

static void memory_maintenance_task(lv_timer_t *timer) {
  LV_UNUSED(timer);
  static uint8_t low_mem_hits = 0;

  // Release excess capacity retained by notification queue allocations.
  std::vector<NotificationItem>(notificationQueue).swap(notificationQueue);

  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t minHeap = ESP.getMinFreeHeap();
  const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t minInternal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t freePsram = ESP.getFreePsram();
  const uint32_t minPsram = ESP.getMinFreePsram();

  bool low_mem = (freeHeap < HALO_MEMORY_GUARD_HEAP_THRESHOLD) ||
                 (freeInternal < HALO_MEMORY_GUARD_INTERNAL_THRESHOLD);
  low_mem_hits = low_mem ? (uint8_t)(low_mem_hits + 1U) : 0U;

  Serial.printf(
    "[Mem] heap=%u min_heap=%u int=%u min_int=%u psram=%u min_psram=%u notif=%u low=%u hits=%u\n",
    freeHeap,
    minHeap,
    freeInternal,
    minInternal,
    freePsram,
    minPsram,
    (unsigned int)notificationQueue.size(),
    low_mem ? 1U : 0U,
    (unsigned int)low_mem_hits
  );

  if (low_mem_hits >= HALO_MEMORY_GUARD_CONSECUTIVE_HITS) {
    Serial.printf(
      "[MemGuard] Persistent low memory (heap=%u int=%u). Restarting...\n",
      freeHeap,
      freeInternal
    );
    delay(100);
    ESP.restart();
  }
}

static inline void news_strip_markup(String &value) {
  value.replace("<![CDATA[", "");
  value.replace("]]>", "");
  value.replace("&amp;", "&");
  value.replace("&quot;", "\"");
  value.replace("&#39;", "'");
  value.trim();
}

static bool news_extract_tag_body(const String &xml, const char *tag, String &out) {
  String openPrefix = "<" + String(tag);
  int open = xml.indexOf(openPrefix);
  if (open < 0) return false;

  int openEnd = xml.indexOf('>', open);
  if (openEnd < 0) return false;

  String closeTag = "</" + String(tag) + ">";
  int close = xml.indexOf(closeTag, openEnd + 1);
  if (close <= openEnd) return false;

  out = xml.substring(openEnd + 1, close);
  news_strip_markup(out);
  return out.length() > 0;
}

static bool news_extract_link_href(const String &xml, String &out) {
  int pos = xml.indexOf("<link");
  while (pos >= 0) {
    int end = xml.indexOf('>', pos);
    if (end < 0) break;

    int hrefPos = xml.indexOf("href=\"", pos);
    if (hrefPos > 0 && hrefPos < end) {
      hrefPos += 6;
      int hrefEnd = xml.indexOf('"', hrefPos);
      if (hrefEnd > hrefPos && hrefEnd <= end) {
        out = xml.substring(hrefPos, hrefEnd);
        news_strip_markup(out);
        return out.length() > 0;
      }
    }

    pos = xml.indexOf("<link", end + 1);
  }
  return false;
}

static bool news_decode_chunked_body(const String &chunked, String &decoded) {
  decoded = "";
  int pos = 0;
  while (pos < (int)chunked.length()) {
    int lineEnd = chunked.indexOf("\r\n", pos);
    if (lineEnd < 0) return false;

    String lenHex = chunked.substring(pos, lineEnd);
    int semi = lenHex.indexOf(';');
    if (semi >= 0) lenHex = lenHex.substring(0, semi);
    lenHex.trim();

    int chunkLen = (int)strtol(lenHex.c_str(), nullptr, 16);
    pos = lineEnd + 2;

    if (chunkLen <= 0) return true;
    if (pos + chunkLen > (int)chunked.length()) return false;

    decoded += chunked.substring(pos, pos + chunkLen);
    pos += chunkLen;

    if (pos + 1 >= (int)chunked.length()) return false;
    if (chunked[pos] == '\r' && chunked[pos + 1] == '\n') {
      pos += 2;
    } else {
      return false;
    }
  }

  return decoded.length() > 0;
}

static bool news_fetch_payload_raw_tls(const char *url, String &payload) {
  String urlStr(url);
  if (!urlStr.startsWith("https://")) return false;

  int hostStart = 8;
  int pathStart = urlStr.indexOf('/', hostStart);
  String host = (pathStart >= 0) ? urlStr.substring(hostStart, pathStart) : urlStr.substring(hostStart);
  String path = (pathStart >= 0) ? urlStr.substring(pathStart) : "/";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(12000);
  if (!client.connect(host.c_str(), 443)) return false;

  client.printf(
      "GET %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "User-Agent: Halo-F1/1.2.0 (ESP32-S3)\r\n"
      "Accept: application/rss+xml, application/xml;q=0.9, text/xml;q=0.8, */*;q=0.1\r\n"
      "Accept-Encoding: identity\r\n"
      "Connection: close\r\n\r\n",
      path.c_str(), host.c_str());

  String response = "";
  unsigned long t0 = millis();
  while (millis() - t0 < 12000) {
    while (client.available()) {
      response += (char)client.read();
      t0 = millis();
    }
    if (!client.connected()) break;
    delay(2);
  }
  client.stop();

  int hdrEnd = response.indexOf("\r\n\r\n");
  if (hdrEnd < 0) return false;

  String headers = response.substring(0, hdrEnd);
  String body = response.substring(hdrEnd + 4);

  if (headers.indexOf("Transfer-Encoding: chunked") >= 0 || headers.indexOf("transfer-encoding: chunked") >= 0) {
    String decoded;
    if (news_decode_chunked_body(body, decoded)) {
      body = decoded;
    }
  }

  payload = body;
  return payload.length() > 0;
}

static bool fetchLatestNewsFromUrl(const char *url, String &title, String &link, String &desc) {
  HTTPClient http;
  // Keep HTTP/1.1 for this feed; some CDNs return empty body on forced HTTP/1.0.
  http.useHTTP10(false);
  http.setTimeout(12000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "Halo-F1/1.2.0 (ESP32-S3)");
  http.addHeader("Accept", "application/rss+xml, application/xml;q=0.9, text/xml;q=0.8, */*;q=0.1");
  http.addHeader("Connection", "close");

  int httpCode = -1;
  if (String(url).startsWith("https://")) {
    WiFiClientSecure client;
    client.setInsecure();
    http.begin(client, url);
    httpCode = http.GET();
  } else {
    WiFiClient client;
    http.begin(client, url);
    httpCode = http.GET();
  }

  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();
  if (payload.length() == 0) {
    if (!news_fetch_payload_raw_tls(url, payload)) {
      return false;
    }
  }

  title = "";
  link = "";
  desc = "";

  int entryStart = payload.indexOf("<item>");
  int entryEnd = -1;
  if (entryStart >= 0) {
    entryEnd = payload.indexOf("</item>", entryStart);
    if (entryEnd < 0) entryEnd = payload.length();
  } else {
    int atomEntryStart = payload.indexOf("<entry");
    if (atomEntryStart >= 0) {
      entryStart = payload.indexOf('>', atomEntryStart);
      if (entryStart >= 0) entryStart += 1;
      entryEnd = payload.indexOf("</entry>", entryStart);
      if (entryEnd < 0) entryEnd = payload.length();
    }
  }

  if (entryStart < 0 || entryEnd <= entryStart) return false;

  String entry = payload.substring(entryStart, entryEnd);

  bool gotTitle = news_extract_tag_body(entry, "title", title);
  bool gotLink = news_extract_tag_body(entry, "link", link);
  if (!gotLink) {
    gotLink = news_extract_link_href(entry, link);
  }
  bool gotDesc = news_extract_tag_body(entry, "description", desc);
  if (!gotDesc) gotDesc = news_extract_tag_body(entry, "summary", desc);
  if (!gotDesc) gotDesc = news_extract_tag_body(entry, "content", desc);

  if (gotDesc && desc.length() > 300) {
    desc = desc.substring(0, 300) + "...";
  }

  return gotLink && (gotTitle || gotDesc);
}

// Tries once to fetch the latest news from the original feed URL
bool fetchLatestNews(String &title, String &link, String &desc) {
  title = "";
  link = "";
  desc = "";
  if (!fetchLatestNewsFromUrl("https://www.the-race.com/category/formula-1/rss/", title, link, desc)) {
    return false;
  }

  return true;
}

// Single attempt to avoid aggressive retry storms when TLS fails on constrained heap.
bool getLatestNews(String &title, String &link, String &desc) {
  return fetchLatestNews(title, link, desc);
}

// Fetch latest session result, returns false if failed or not yet available (session ongoing or not enough time passed since the end)
bool getLastSessionResults(SessionResults results[DRIVERS_NUMBER]) {
  got_new_results = false;

  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure secureClient;
  secureClient.setInsecure();          // same approach as fetchLatestNews

  HTTPClient http;

  String url = "https://api.openf1.org/v1/session_result?session_key=latest&position%3C=" + (String)DRIVERS_NUMBER;
  //http.begin("https://api.openf1.org/v1/session_result?session_key=7782&position%3C=20"); // debug

  http.useHTTP10(true);
  http.begin(secureClient, url);       // explicit TLS client passed
  http.setTimeout(10000); 

  int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error) return false;
  if (doc["detail"].is<JsonVariant>()) return false;

  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull() || arr.size() == 0) return false;
  int i = 0;

  for (JsonObject obj : arr) {
    if (i >= TOTAL_DRIVERS) break;

    if(!obj["position"]) return false;

    results[i].position = obj["position"].as<String>();
    results[i].driver_number = obj["driver_number"].as<String>();

    // Handle "duration" (could be number or array)
    if (obj["duration"].is<JsonArray>()) {
      results[i].isQualifying = true;
      int j = 0;
      for (JsonVariant v : obj["duration"].as<JsonArray>()) {
        if (j < 3) results[i].quali[j] = v.as<float>();
        j++;
      }

      j = 0;
      for (JsonVariant v : obj["gap_to_leader"].as<JsonArray>()) {
        if (j < 3) results[i].gap_to_leader_quali[j] = v.as<float>();
        j++;
      }

    } else {
      results[i].isQualifying = false;
      results[i].duration = obj["duration"].as<float>();
      results[i].gap_to_leader = obj["gap_to_leader"].as<float>();
      results[i].dns = obj["dns"].as<bool>();
      results[i].dnf = obj["dnf"].as<bool>();
    }

    i++;
  }

  results_loaded_once = true;
  got_new_results = true;
  
  return true;
}

bool fetch_f1_driver_standings() {
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient client;
  client.useHTTP10(true);
  client.setTimeout(12000);
  JsonDocument doc;
  DeserializationError error;
  int statusCode;
  bool preSeasonFallback = false;

  // ── Driver Standings ────────────────────────────────────────────────────────
  std::string url = "https://api.jolpi.ca/ergast/f1/current/driverstandings/";
  client.begin(secureClient, url.c_str());
  statusCode = client.GET();
  if (statusCode != 200) { client.end(); return false; }
  error = deserializeJson(doc, client.getStream());
  client.end();
  if (error) return false;

  JsonArray driverStandingsLists = doc["MRData"]["StandingsTable"]["StandingsLists"].as<JsonArray>();

  if (!driverStandingsLists.isNull() && driverStandingsLists.size() > 0) {
    // ── Normal path ──────────────────────────────────────────────────────────
    JsonObject standingsList = driverStandingsLists[0];
    current_season.season = standingsList["season"].as<String>();
    current_season.round  = standingsList["round"].as<String>();
    JsonArray standings = standingsList["DriverStandings"].as<JsonArray>();
    current_season.driver_count = standings.size();
    for (size_t i = 0; i < current_season.driver_count && i < 30; i++) {
      JsonObject item   = standings[i];
      JsonObject driver = item["Driver"];
      JsonArray  constructors = item["Constructors"].as<JsonArray>();
      JsonObject constructor;
      if (!constructors.isNull() && constructors.size() > 0)
        constructor = constructors[constructors.size() - 1];
      current_season.driver_standings[i].position      = item["positionText"].as<String>();
      current_season.driver_standings[i].points        = item["points"].as<String>();
      current_season.driver_standings[i].number        = driver["permanentNumber"].as<String>();
      current_season.driver_standings[i].name          = driver["givenName"].as<String>();
      current_season.driver_standings[i].surname       = driver["familyName"].as<String>();
      current_season.driver_standings[i].nationality   = driver["nationality"].as<String>();
      current_season.driver_standings[i].constructor   = constructor["name"].as<String>();
      current_season.driver_standings[i].constructorId = constructor["constructorId"].as<String>();
      if (current_season.driver_standings[i].name == "Andrea Kimi")
        current_season.driver_standings[i].name = "A. Kimi";
    }
  } else {
    // ── Pre-season fallback ──────────────────────────────────────────────────
    preSeasonFallback = true;

    // Simple lookup table: driverId → {constructorId, constructorName}
    String mapDriverId[30], mapCtorId[30], mapCtorName[30];
    int mapSize = 0;

    // Step 1: fetch constructor list.
    // For each constructor, fetch its driver roster to build the lookup map.
    // Also populate team_standings here so we don't need to fetch it again later.
    doc.clear();
    url = "https://api.jolpi.ca/ergast/f1/current/constructors/";
    client.begin(secureClient, url.c_str());
    statusCode = client.GET();
    if (statusCode != 200) { client.end(); return false; }
    error = deserializeJson(doc, client.getStream());
    client.end();
    if (error) return false;

    JsonArray constructors = doc["MRData"]["ConstructorTable"]["Constructors"].as<JsonArray>();
    current_season.team_count = min((int)constructors.size(), 12);

    for (size_t i = 0; i < (size_t)current_season.team_count; i++) {
      JsonObject ctor     = constructors[i];
      String     ctorId   = ctor["constructorId"].as<String>();
      String     ctorName = ctor["name"].as<String>();

      // Populate team standings with zero points while we're here
      current_season.team_standings[i].position = String(i + 1);
      current_season.team_standings[i].points   = "0";
      current_season.team_standings[i].name     = ctorName;
      current_season.team_standings[i].id       = ctorId;

      // Fetch the driver roster for this constructor
      JsonDocument ctorDriverDoc;
      std::string  ctorDriverUrl = "https://api.jolpi.ca/ergast/f1/current/constructors/"
                                   + std::string(ctorId.c_str()) + "/drivers/";
      client.begin(secureClient, ctorDriverUrl.c_str());
      if (client.GET() == 200) {
        deserializeJson(ctorDriverDoc, client.getStream());
        JsonArray ctorDrivers = ctorDriverDoc["MRData"]["DriverTable"]["Drivers"].as<JsonArray>();
        for (JsonObject d : ctorDrivers) {
          if (mapSize < 30) {
            mapDriverId[mapSize] = d["driverId"].as<String>();
            mapCtorId[mapSize]   = ctorId;
            mapCtorName[mapSize] = ctorName;
            mapSize++;
          }
        }
      }
      client.end();
    }

    // Step 2: fetch the full driver list and resolve constructor via the map
    doc.clear();
    url = "https://api.jolpi.ca/ergast/f1/current/drivers/";
    client.begin(secureClient, url.c_str());
    statusCode = client.GET();
    if (statusCode != 200) { client.end(); return false; }
    error = deserializeJson(doc, client.getStream());
    client.end();
    if (error) return false;

    current_season.season = doc["MRData"]["DriverTable"]["season"].as<String>();
    current_season.round  = "0";

    JsonArray drivers = doc["MRData"]["DriverTable"]["Drivers"].as<JsonArray>();
    current_season.driver_count = drivers.size();
    for (size_t i = 0; i < current_season.driver_count && i < 30; i++) {
      JsonObject driver   = drivers[i];
      String     driverId = driver["driverId"].as<String>();

      // Resolve constructor from lookup map
      String ctorId = "", ctorName = "";
      for (int m = 0; m < mapSize; m++) {
        if (mapDriverId[m] == driverId) { ctorId = mapCtorId[m]; ctorName = mapCtorName[m]; break; }
      }

      current_season.driver_standings[i].position      = String(i + 1);
      current_season.driver_standings[i].points        = "0";
      current_season.driver_standings[i].name          = driver["givenName"].as<String>();
      current_season.driver_standings[i].surname       = driver["familyName"].as<String>();
      current_season.driver_standings[i].nationality   = driver["nationality"].as<String>();
      current_season.driver_standings[i].number        = driver["familyName"].as<String>() == "Lindblad" ? "41" : driver["permanentNumber"].as<String>();
      current_season.driver_standings[i].constructor   = ctorName;
      current_season.driver_standings[i].constructorId = ctorId;
      if (current_season.driver_standings[i].name == "Andrea Kimi")
        current_season.driver_standings[i].name = "A. Kimi";
    }
  }

  // ── Constructor Standings ───────────────────────────────────────────────────
  if (preSeasonFallback) {
    // team_standings already populated during the driver fallback above — skip
  } else {
    doc.clear();
    url = "https://api.jolpi.ca/ergast/f1/current/constructorstandings/";
    client.begin(secureClient, url.c_str());
    statusCode = client.GET();
    if (statusCode != 200) { client.end(); return false; }
    error = deserializeJson(doc, client.getStream());
    client.end();
    if (error) return false;

    JsonArray constructorStandingsLists = doc["MRData"]["StandingsTable"]["StandingsLists"].as<JsonArray>();

    if (!constructorStandingsLists.isNull() && constructorStandingsLists.size() > 0) {
      // ── Normal path ────────────────────────────────────────────────────────
      JsonObject standingsList = constructorStandingsLists[0];
      JsonArray  standings     = standingsList["ConstructorStandings"].as<JsonArray>();
      current_season.team_count = standings.size();
      for (size_t i = 0; i < current_season.team_count && i < 12; i++) {
        JsonObject item = standings[i];
        JsonObject team = item["Constructor"];
        current_season.team_standings[i].position = item["position"].as<String>();
        current_season.team_standings[i].points   = item["points"].as<String>();
        current_season.team_standings[i].name     = team["name"].as<String>();
        current_season.team_standings[i].id       = team["constructorId"].as<String>();
      }
    } else {
      // ── Fallback (edge case: driver standings exist but constructor don't) ─
      doc.clear();
      url = "https://api.jolpi.ca/ergast/f1/current/constructors/";
      client.begin(secureClient, url.c_str());
      statusCode = client.GET();
      if (statusCode != 200) { client.end(); return false; }
      error = deserializeJson(doc, client.getStream());
      client.end();
      if (error) return false;
      JsonArray constructors = doc["MRData"]["ConstructorTable"]["Constructors"].as<JsonArray>();
      current_season.team_count = constructors.size();
      for (size_t i = 0; i < current_season.team_count && i < 12; i++) {
        JsonObject team = constructors[i];
        current_season.team_standings[i].position = String(i + 1);
        current_season.team_standings[i].points   = "0";
        current_season.team_standings[i].name     = team["name"].as<String>();
        current_season.team_standings[i].id       = team["constructorId"].as<String>();
      }
    }
  }

  standings_loaded_once = true;
  return true;
}


// Fetch next race infos and loads them into the given "NextRaceInfo" type struct or returns false on fail
bool getNextRaceInfo(NextRaceInfo &info) {
    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(12000);
    //http.begin("https://api.jolpi.ca/ergast/f1/2026/2/races/"); //sprint weekend for testing purposes
    http.begin(secureClient, "https://api.jolpi.ca/ergast/f1/current/next/races/");
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_FOUND) {
      String newUrl = http.getLocation(); // this gives the "Location" header from the redirect
      http.end(); // close the previous connection
      http.begin(secureClient, newUrl);
      httpCode = http.GET();
    }

    if (httpCode != 200) {
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getStream());
    http.end();
    if (error) return false;

    JsonObject race = doc["MRData"]["RaceTable"]["Races"][0];
    info.raceName    = race["raceName"].as<String>();
    info.circuitName = race["Circuit"]["circuitName"].as<String>();
    info.country     = race["Circuit"]["Location"]["country"].as<String>();
    info.lat         = race["Circuit"]["Location"]["lat"].as<float>();
    info.lon         = race["Circuit"]["Location"]["long"].as<float>();

    info.sessionCount = 0;
    info.isSprintWeekend = race["Sprint"].is<JsonVariant>(); //checking if key exists, maybe better to do as race["Sprint"] ? true : false; ??

    // List of possible sessions in API
    const char* sessionKeys[] = { "FirstPractice", "SecondPractice", "ThirdPractice", "SprintQualifying", "Sprint", "Qualifying"};
    const char* sessionNames[] = { "FP1", "FP2", "FP3", "Sprint Qualifying", "Sprint Race", "Qualifying" };
    int sessionTotal = sizeof(sessionKeys) / sizeof(sessionKeys[0]);

    for (int i = 0; i < sessionTotal; i++) {
      if (race[sessionKeys[i]]) {
        String dateUTC = race[sessionKeys[i]]["date"].as<String>();
        String timeUTC = race[sessionKeys[i]]["time"].as<String>();

        info.sessions[info.sessionCount].name = sessionNames[i];
        info.sessions[info.sessionCount].date = dateUTC;
        info.sessions[info.sessionCount].time = timeUTC;
        info.sessionCount++;
      }
    }

    String dateUTC = race["date"].as<String>();
    String timeUTC = race["time"].as<String>();

    info.sessions[info.sessionCount].name = "Race";
    info.sessions[info.sessionCount].date = dateUTC;
    info.sessions[info.sessionCount].time = timeUTC;
    info.sessionCount++;

    return true;
}

// Runs with a lvgl timer, fetches driver standings and next race infos (baseline F1 APIs)
void update_f1_api(lv_timer_t *timer) {
  if (!fetch_f1_driver_standings()) {
    next_race.sessionCount = 0;
    return;
  }

  if (getNextRaceInfo(next_race)) {
      // ── Weather forecast for each session (Open-Meteo, no API key) ─────────
      // fetchWeatherForRace() is self-throttled (WEATHER_REFRESH_MS = 1 h) so
      // calling it here every time update_f1_api fires (hourly) is safe.
      fetchWeatherForRace(next_race);
  } else {
      next_race.sessionCount = 0;
  }

  //update_driver_standings_ui();
}

void sendStatisticData(lv_timer_t *timer) {
  String UUID = getDeviceUUID();
  String current_language = localized_text->language_name_eng;
  String offset = (String)UTCoffset;

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  String url = "https://www.we-race.it/wp-json/f1-halo/v2/sendstats?uuid=" + UUID + "&language=" + current_language + "&offset=" + offset + "&version=" + fw_version + "&flush=" + random(0, millis());
  http.useHTTP10(true);
  http.setTimeout(12000);
  http.begin(secureClient, url.c_str());

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();

  if (!error) {
    // check for updates
    updateAvailable = doc["update_available"];
    latestVersionString = doc["latest_version"].as<String>();
    update_link = doc["update_link"].as<String>();

    // populate notifications
    notificationQueue.clear();
    notificationQueue.shrink_to_fit();
    JsonArray notifications = doc["notifications"];
    
    for (JsonObject notification : notifications) {
      NotificationItem newItem;
      newItem.title = notification["title"].as<String>();
      newItem.text = notification["text"].as<String>();
      newItem.qrLink = notification["qr"].as<String>();
      notificationQueue.push_back(newItem);
    }
  }

  return;
}

//flag for saving data, needed for WiFiManager
bool shouldSaveConfig = false;

// WiFiManager callback notifying us of the need to save config
void saveConfigCallback () {
  shouldSaveConfig = true;
}

// Called when WiFi Access Point is activated for connection setup (first setup or connection failed)
void configModeCallback (WiFiManager *myWiFiManager) {
  lv_screen_load(screen.wifi);
  lv_obj_t * label3 = lv_label_create(screen.wifi);
  lv_label_set_text(label3, localized_text->wifi_connection_failed);
  lv_obj_align(label3, LV_ALIGN_CENTER, 0, 50);
  lv_label_set_long_mode(label3, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_width(label3, 250, 0);
  lv_timer_periodic_handler();
}

// WiFi Manager and WiFi Handler, runs in setup once. If connection success sets up a bunch of lvgl timers for API update (clock, F1 baseline, News)
void setupWiFiManager(bool forceConfig) {
  //set config save notify callback
  wm.setSaveConfigCallback(saveConfigCallback);
  //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wm.setAPCallback(configModeCallback);
  wm.setClass("invert"); // dark theme

  if (forceConfig) {
    if (!wm.startConfigPortal("Halo-F1")) {
      if (clock_timer) lv_timer_del(clock_timer);
      clock_timer = NULL;
      if (f1_api_timer) lv_timer_del(f1_api_timer);
      f1_api_timer = NULL;
      if (news_timer) lv_timer_del(news_timer);
      news_timer = NULL;
      if (statistics_timer) lv_timer_del(statistics_timer);
      statistics_timer = NULL;
      if (notifications_timer) lv_timer_del(notifications_timer);
      notifications_timer = NULL;
      if (memory_maintenance_timer) lv_timer_del(memory_maintenance_timer);
      memory_maintenance_timer = NULL;
      Serial.println("failed to connect and hit timeout");
      delay(3000);
      //reset and try again, or maybe put it to deep sleep
      ESP.restart();
      delay(5000);
    } else {
      update_internal_clock();
      update_f1_api(nullptr);
      update_ui(nullptr);
      create_or_reload_news_ui(nullptr);
      if (!clock_timer) clock_timer = lv_timer_create(update_ui, 60000, NULL);
      if (!f1_api_timer) f1_api_timer = lv_timer_create(update_f1_api, 3600000, NULL);
      if (!news_timer) news_timer = lv_timer_create(create_or_reload_news_ui, 5*60000, NULL);
      if (!statistics_timer) statistics_timer = lv_timer_create(sendStatisticData, 59*6000, NULL);
      if (!notifications_timer) notifications_timer = lv_timer_create(notification_scheduler_task, NOTIFICATION_INTERVAL_MS, NULL);
      if (!memory_maintenance_timer) memory_maintenance_timer = lv_timer_create(memory_maintenance_task, HALO_MEMORY_MAINTENANCE_PERIOD_MS, NULL);
      lv_screen_load(screen.home);
    }
  } else {
    if (!wm.autoConnect("Halo-F1")) {
      if (clock_timer) lv_timer_del(clock_timer);
      clock_timer = NULL;
      if (f1_api_timer) lv_timer_del(f1_api_timer);
      f1_api_timer = NULL;
      if (news_timer) lv_timer_del(news_timer);
      news_timer = NULL;
      if (statistics_timer) lv_timer_del(statistics_timer);
      statistics_timer = NULL;
      if (notifications_timer) lv_timer_del(notifications_timer);
      notifications_timer = NULL;
      if (memory_maintenance_timer) lv_timer_del(memory_maintenance_timer);
      memory_maintenance_timer = NULL;
      Serial.println("failed to connect and hit timeout");
      delay(3000);
      // if we still have not connected restart and try all over again
      ESP.restart();
      delay(5000);
    } else {
      update_internal_clock();
      update_f1_api(nullptr);
      update_ui(nullptr);
      create_or_reload_news_ui(nullptr);
      if (!clock_timer) clock_timer = lv_timer_create(update_ui, 60000, NULL);
      if (!f1_api_timer) f1_api_timer = lv_timer_create(update_f1_api, 3600000, NULL);
      if (!news_timer) news_timer = lv_timer_create(create_or_reload_news_ui, 5*60000, NULL);
      if (!statistics_timer) statistics_timer = lv_timer_create(sendStatisticData, 59*60000, NULL);
      if (!notifications_timer) notifications_timer = lv_timer_create(notification_scheduler_task, NOTIFICATION_INTERVAL_MS, NULL);
      if (!memory_maintenance_timer) memory_maintenance_timer = lv_timer_create(memory_maintenance_task, HALO_MEMORY_MAINTENANCE_PERIOD_MS, NULL);
      lv_screen_load(screen.home);
    }
  }
  lv_timer_periodic_handler();


  //save the custom parameters to FS (not used for now)
  if (shouldSaveConfig)
  {
    ESP.restart();
    delay(5000);
  }

}

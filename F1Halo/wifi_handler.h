static void logHttpResult(const char *tag, const String &url, int httpCode, WiFiClientSecure *secureClient = nullptr) {
#if HALO_HTTP_DEBUG
  (void)url;
  Serial.printf("[%s] HTTP: %d (%s)\n", tag, httpCode, HTTPClient::errorToString(httpCode).c_str());
  if (httpCode < 0 && secureClient != nullptr) {
    char tlsErr[128] = {0};
    int tlsCode = secureClient->lastError(tlsErr, sizeof(tlsErr));
    Serial.printf("[%s] TLS: %d (%s)\n", tag, tlsCode, tlsErr);
  }
#endif
}

static void logHttpPayloadPreview(const char *tag, const String &payload) {
#if HALO_HTTP_DEBUG
  int n = min((int)payload.length(), 240);
  String preview = payload.substring(0, n);
  preview.replace("\r", " ");
  preview.replace("\n", " ");
  Serial.printf("[%s] Payload len: %d\n", tag, payload.length());
  Serial.printf("[%s] Payload preview: %s\n", tag, preview.c_str());
#endif
}

static String extractHostFromUrl(const String &url) {
  int schemePos = url.indexOf("://");
  int hostStart = (schemePos >= 0) ? (schemePos + 3) : 0;
  int hostEnd = url.indexOf('/', hostStart);
  if (hostEnd < 0) hostEnd = url.length();
  String hostPort = url.substring(hostStart, hostEnd);
  int colonPos = hostPort.indexOf(':');
  if (colonPos >= 0) return hostPort.substring(0, colonPos);
  return hostPort;
}

static void logDnsForUrl(const char *tag, const String &url) {
#if HALO_HTTP_DEBUG
  String host = extractHostFromUrl(url);
  if (host.length() == 0) return;
  IPAddress resolvedIp;
  bool ok = WiFi.hostByName(host.c_str(), resolvedIp);
  if (ok) {
    Serial.printf("[%s] DNS: %s -> %u.%u.%u.%u\n",
                  tag, host.c_str(),
                  resolvedIp[0], resolvedIp[1], resolvedIp[2], resolvedIp[3]);
  } else {
    Serial.printf("[%s] DNS failed: %s\n", tag, host.c_str());
  }
#else
  (void)tag;
  (void)url;
#endif
}

static void ensureStationDnsConfigured() {
  IPAddress dns0 = WiFi.dnsIP(0);
  IPAddress dns1 = WiFi.dnsIP(1);
  Serial.printf("[WiFi] DNS before: %u.%u.%u.%u / %u.%u.%u.%u\n",
                dns0[0], dns0[1], dns0[2], dns0[3],
                dns1[0], dns1[1], dns1[2], dns1[3]);

  // Force known-good public resolvers to avoid flaky DHCP DNS on embedded clients.
  WiFi.setDNS(IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8));

  dns0 = WiFi.dnsIP(0);
  dns1 = WiFi.dnsIP(1);
  Serial.printf("[WiFi] DNS after:  %u.%u.%u.%u / %u.%u.%u.%u\n",
                dns0[0], dns0[1], dns0[2], dns0[3],
                dns1[0], dns1[1], dns1[2], dns1[3]);
}

static bool beginSecureHttpGet(HTTPClient &http, WiFiClientSecure &client, const String &url, const char *tag = "HTTP") {
  client.setInsecure();
  client.setTimeout(12000);
  client.setHandshakeTimeout(20);
  http.setConnectTimeout(12000);
  http.setTimeout(12000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
#if HALO_HTTP_DEBUG
  Serial.printf("[%s] URL: %s\n", tag, url.c_str());
  logDnsForUrl(tag, url);
  Serial.printf("[%s] Heap: free=%u max=%u psram=%u\n",
                tag,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap(),
                (unsigned)ESP.getFreePsram());
#endif
  if (!http.begin(client, url)) {
#if HALO_HTTP_DEBUG
    Serial.printf("[%s] begin() failed\n", tag);
#endif
    return false;
  }
  http.setUserAgent("Halo-F1/1.2.0");
  return true;
}

static String extractXmlTagContent(const String &xml, const char *tag) {
  String openTag = "<" + String(tag) + ">";
  String closeTag = "</" + String(tag) + ">";
  int start = xml.indexOf(openTag);
  if (start < 0) return "";
  start += openTag.length();
  int end = xml.indexOf(closeTag, start);
  if (end < 0 || end <= start) return "";
  String out = xml.substring(start, end);
  out.trim();

  if (out.startsWith("<![CDATA[")) {
    out.remove(0, 9);
    int cdataEnd = out.indexOf("]]>");
    if (cdataEnd >= 0) out.remove(cdataEnd);
  }

  out.replace("&amp;", "&");
  out.replace("&quot;", "\"");
  out.replace("&#39;", "'");
  return out;
}

// Tries once to fetch the latest news.
// It attempts multiple feeds and falls back to a local informational card
// to avoid blocking loops/spam when an RSS host is unreachable from ESP32 TLS.
bool fetchLatestNews(String &title, String &link, String &desc) {
  static const char * NEWS_FEEDS[] = {
    "https://www.the-race.com/category/formula-1/rss/",
    "https://feeds.bbci.co.uk/sport/formula1/rss.xml",
    "http://feeds.bbci.co.uk/sport/formula1/rss.xml"
  };

  title = "";
  link = "";
  desc = "";

  for (size_t i = 0; i < (sizeof(NEWS_FEEDS) / sizeof(NEWS_FEEDS[0])); i++) {
    String requestUrl = NEWS_FEEDS[i];

    HTTPClient http;
    WiFiClientSecure secureClient;
    int httpCode = -1;

    if (requestUrl.startsWith("https://")) {
      if (!beginSecureHttpGet(http, secureClient, requestUrl, "News")) {
        continue;
      }
      httpCode = http.GET();
    } else {
      http.setConnectTimeout(12000);
      http.setTimeout(12000);
      http.setReuse(false);
      http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
#if HALO_HTTP_DEBUG
      Serial.printf("[News] URL: %s\n", requestUrl.c_str());
#endif
      if (!http.begin(requestUrl)) {
#if HALO_HTTP_DEBUG
        Serial.println("[News] begin() failed");
#endif
        continue;
      }
      http.setUserAgent("Halo-F1/1.2.0");
      httpCode = http.GET();
    }

    logHttpResult("News", requestUrl, httpCode, &secureClient);

    // Some feeds return 30x. Retry once with the Location header URL.
    if (httpCode == HTTP_CODE_MOVED_PERMANENTLY ||
        httpCode == HTTP_CODE_FOUND ||
        httpCode == HTTP_CODE_TEMPORARY_REDIRECT ||
        httpCode == HTTP_CODE_PERMANENT_REDIRECT) {
      String redirectUrl = http.getLocation();
      http.end();
      if (redirectUrl.length() == 0) {
        continue;
      }
#if HALO_HTTP_DEBUG
      Serial.printf("[News] Redirect: %s\n", redirectUrl.c_str());
#endif
      requestUrl = redirectUrl;

      if (requestUrl.startsWith("https://")) {
        if (!beginSecureHttpGet(http, secureClient, requestUrl, "News")) {
          continue;
        }
      } else {
        http.setConnectTimeout(12000);
        http.setTimeout(12000);
        http.setReuse(false);
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
#if HALO_HTTP_DEBUG
        Serial.printf("[News] URL: %s\n", requestUrl.c_str());
#endif
        if (!http.begin(requestUrl)) {
#if HALO_HTTP_DEBUG
          Serial.println("[News] begin() failed");
#endif
          continue;
        }
        http.setUserAgent("Halo-F1/1.2.0");
      }

      httpCode = http.GET();
      logHttpResult("News", requestUrl, httpCode, &secureClient);
    }

    if (httpCode != HTTP_CODE_OK) {
      http.end();
      continue;
    }

    String payload = http.getString();
    http.end();
    logHttpPayloadPreview("News", payload);
    if (payload.length() == 0) continue;

    int itemStart = payload.indexOf("<item");
    if (itemStart < 0) continue;
    int itemTagEnd = payload.indexOf('>', itemStart);
    int itemEnd = payload.indexOf("</item>", itemTagEnd);
    if (itemTagEnd < 0 || itemEnd < 0 || itemEnd <= itemTagEnd) continue;

    String itemXml = payload.substring(itemTagEnd + 1, itemEnd);
    title = extractXmlTagContent(itemXml, "title");
    link = extractXmlTagContent(itemXml, "link");
    desc = extractXmlTagContent(itemXml, "description");

    if (desc.length() > 300) desc = desc.substring(0, 300) + "...";

    if (link != "" && (title != "" || desc != "")) {
      return true;
    }
  }

  // Fallback content so the UI remains usable when RSS hosts are blocked.
  title = "F1 News Feed Unavailable";
  link = "https://jolpi.ca/";
  desc = "Live RSS endpoints could not be reached from this network. Core race/session APIs continue to run.";
  return true;
}

// Non-blocking wrapper used by UI refresh.
bool getLatestNews(String &title, String &link, String &desc) {
  Serial.println("Fetching News");
  return fetchLatestNews(title, link, desc);
}

// Fetch latest session result, returns false if failed or not yet available (session ongoing or not enough time passed since the end)
bool getLastSessionResults(SessionResults results[DRIVERS_NUMBER]) {
  got_new_results = false;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return false;
  }

  WiFiClientSecure secureClient;
  HTTPClient http;

  String url = "https://api.openf1.org/v1/session_result?session_key=latest&position%3C=" + (String)DRIVERS_NUMBER;
  //http.begin("https://api.openf1.org/v1/session_result?session_key=7782&position%3C=20"); // debug

  if (!beginSecureHttpGet(http, secureClient, url, "Results")) {
    Serial.println("HTTP begin() failed for Last Session Results");
    return false;
  }

  int httpCode = http.GET();
  logHttpResult("Results", url, httpCode, &secureClient);
  if (httpCode != 200) {
    Serial.printf("HTTP request failed for Last Session Results, code: %d\n", httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();
  logHttpPayloadPreview("Results", payload);

  if (payload.substring(2,8) == "detail") return false;

  JsonDocument doc;

  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("JSON parsing failed for Last Session Results: ");
    Serial.println(error.c_str());
    return false;
  }

  Serial.print("Payload: ");
  Serial.println(payload);

  if (payload == "[]") return false;

  JsonArray arr = doc.as<JsonArray>();
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
  HTTPClient client;
  WiFiClientSecure secureClient;
  JsonDocument doc;
  DeserializationError error;
  int statusCode;
  bool preSeasonFallback = false;

  // ── Driver Standings ────────────────────────────────────────────────────────
  std::string url = "https://api.jolpi.ca/ergast/f1/current/driverstandings/";
  if (!beginSecureHttpGet(client, secureClient, url.c_str(), "Standings")) return false;
  statusCode = client.GET();
  logHttpResult("Standings", url.c_str(), statusCode, &secureClient);
  if (statusCode != 200) { client.end(); return false; }
  error = deserializeJson(doc, client.getStream());
  client.end();
  if (error) { Serial.printf("JSON error: %s\n", error.c_str()); return false; }

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
    if (!beginSecureHttpGet(client, secureClient, url.c_str(), "Standings")) return false;
    statusCode = client.GET();
    logHttpResult("Standings", url.c_str(), statusCode, &secureClient);
    if (statusCode != 200) { client.end(); return false; }
    error = deserializeJson(doc, client.getStream());
    client.end();
    if (error) { Serial.printf("JSON error: %s\n", error.c_str()); return false; }

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
      if (!beginSecureHttpGet(client, secureClient, ctorDriverUrl.c_str(), "Standings")) return false;
      statusCode = client.GET();
      logHttpResult("Standings", ctorDriverUrl.c_str(), statusCode, &secureClient);
      if (statusCode == 200) {
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
    if (!beginSecureHttpGet(client, secureClient, url.c_str(), "Standings")) return false;
    statusCode = client.GET();
    logHttpResult("Standings", url.c_str(), statusCode, &secureClient);
    if (statusCode != 200) { client.end(); return false; }
    error = deserializeJson(doc, client.getStream());
    client.end();
    if (error) { Serial.printf("JSON error: %s\n", error.c_str()); return false; }

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
    if (!beginSecureHttpGet(client, secureClient, url.c_str(), "Standings")) return false;
    statusCode = client.GET();
    logHttpResult("Standings", url.c_str(), statusCode, &secureClient);
    if (statusCode != 200) { client.end(); return false; }
    error = deserializeJson(doc, client.getStream());
    client.end();
    if (error) { Serial.printf("JSON error: %s\n", error.c_str()); return false; }

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
      if (!beginSecureHttpGet(client, secureClient, url.c_str(), "Standings")) return false;
      statusCode = client.GET();
      logHttpResult("Standings", url.c_str(), statusCode, &secureClient);
      if (statusCode != 200) { client.end(); return false; }
      error = deserializeJson(doc, client.getStream());
      client.end();
      if (error) { Serial.printf("JSON error: %s\n", error.c_str()); return false; }
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
    HTTPClient http;
    //http.begin("https://api.jolpi.ca/ergast/f1/2026/2/races/"); //sprint weekend for testing purposes
    String url = "https://api.jolpi.ca/ergast/f1/current/next/races/";
    if (!beginSecureHttpGet(http, secureClient, url, "NextRace")) {
      Serial.println("HTTP begin() failed for next race call");
      return false;
    }
    int httpCode = http.GET();
    logHttpResult("NextRace", url, httpCode, &secureClient);

    if (httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_FOUND) {
      String newUrl = http.getLocation(); // this gives the "Location" header from the redirect
      Serial.println("Redirect to: " + newUrl);
      http.end(); // close the previous connection
      if (!beginSecureHttpGet(http, secureClient, newUrl, "NextRace")) {
        Serial.println("HTTP begin() failed for redirected next race call");
        return false;
      }
      httpCode = http.GET();
      logHttpResult("NextRace", newUrl, httpCode, &secureClient);
    }

    if (httpCode != 200) {
        Serial.println("HTTP Error: " + String(httpCode));
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();
    logHttpPayloadPreview("NextRace", payload);

    JsonDocument doc;

    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.println("JSON parse failed");
        return false;
    }

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
    Serial.println("[F1 API] Failed fetching standings.");
    return;
  }

  if (getNextRaceInfo(next_race)) {    
      Serial.println("Race: " + next_race.raceName);
      Serial.println("Circuit: " + next_race.circuitName);
      Serial.println("Country: " + next_race.country);
      Serial.println(next_race.isSprintWeekend ? "Sprint Weekend" : "Normal Weekend");

      for (int i = 0; i < next_race.sessionCount; i++) {
        String has_started = "No";
        if (hasSessionStarted(next_race.sessions[i].date, next_race.sessions[i].time)) has_started = "Yes";
          Serial.printf("%s - %s %s - Has already started: %s\n",
                        next_race.sessions[i].name.c_str(),
                        next_race.sessions[i].date.c_str(),
                        next_race.sessions[i].time.c_str(),
                        has_started.c_str());
      }

      // ── Weather forecast for each session (Open-Meteo, no API key) ─────────
      // fetchWeatherForRace() is self-throttled (WEATHER_REFRESH_MS = 1 h) so
      // calling it here every time update_f1_api fires (hourly) is safe.
      fetchWeatherForRace(next_race);
  } else {
      Serial.println("[F1 API] Failed fetching next race.");
  }

  //update_driver_standings_ui();
}

void sendStatisticData(lv_timer_t *timer) {
  String UUID = getDeviceUUID();
  String current_language = localized_text->language_name_eng;
  String offset = (String)UTCoffset;

  WiFiClientSecure secureClient;
  HTTPClient http;
  String url = "https://www.we-race.it/wp-json/f1-halo/v2/sendstats?uuid=" + UUID + "&language=" + current_language + "&offset=" + offset + "&version=" + fw_version + "&flush=" + random(0, millis());
  if (!beginSecureHttpGet(http, secureClient, url, "Stats")) {
    Serial.println("HTTP begin() failed for sendStatisticData");
    return;
  }

  int httpCode = http.GET();
  logHttpResult("Stats", url, httpCode, &secureClient);
  if (httpCode < 0) {
    // Some networks/routers behave differently for www/non-www routes.
    String altUrl = url;
    altUrl.replace("https://www.we-race.it/", "https://we-race.it/");
    if (altUrl != url) {
      http.end();
      if (beginSecureHttpGet(http, secureClient, altUrl, "Stats")) {
        httpCode = http.GET();
        logHttpResult("Stats", altUrl, httpCode, &secureClient);
        if (httpCode == HTTP_CODE_OK) {
          url = altUrl;
        }
      }
    }
  }
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[Stats] HTTP error: %d\n", httpCode);
    http.end();
    return;
  }

  String payload = http.getString();
  logHttpPayloadPreview("Stats", payload);

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (!error) {
    // check for updates
    updateAvailable = doc["update_available"];
    latestVersionString = doc["latest_version"].as<String>();
    update_link = doc["update_link"].as<String>();

    // populate notifications
    notificationQueue.clear();
    JsonArray notifications = doc["notifications"];
    
    for (JsonObject notification : notifications) {
      NotificationItem newItem;
      newItem.title = notification["title"].as<String>();
      newItem.text = notification["text"].as<String>();
      newItem.qrLink = notification["qr"].as<String>();
      notificationQueue.push_back(newItem);
    }
    Serial.printf("Synced: %d notifications available.\n", notificationQueue.size());
  }

  http.end();

  //Serial.printf("Stats response: %s\n", payload.c_str()); // debug
  return;
}

//flag for saving data, needed for WiFiManager
bool shouldSaveConfig = false;

// WiFiManager callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

// Called when WiFi Access Point is activated for connection setup (first setup or connection failed)
void configModeCallback (WiFiManager *myWiFiManager) {
  lv_screen_load(screen.wifi);
  lv_obj_t * label3 = lv_label_create(screen.wifi_root);
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
      Serial.println("failed to connect and hit timeout");
      delay(3000);
      //reset and try again, or maybe put it to deep sleep
      ESP.restart();
      delay(5000);
    } else {
      ensureStationDnsConfigured();
      update_internal_clock();
      update_f1_api(nullptr);
      update_ui(nullptr);
      create_or_reload_news_ui(nullptr);
      if (!clock_timer) clock_timer = lv_timer_create(update_ui, 60000, NULL);
      if (!f1_api_timer) f1_api_timer = lv_timer_create(update_f1_api, 3600000, NULL);
      if (!news_timer) news_timer = lv_timer_create(create_or_reload_news_ui, 5*60000, NULL);
      if (!statistics_timer) statistics_timer = lv_timer_create(sendStatisticData, 59*6000, NULL);
      if (!notifications_timer) notifications_timer = lv_timer_create(notification_scheduler_task, NOTIFICATION_INTERVAL_MS, NULL);
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
      Serial.println("failed to connect and hit timeout");
      delay(3000);
      // if we still have not connected restart and try all over again
      ESP.restart();
      delay(5000);
    } else {
      ensureStationDnsConfigured();
      update_internal_clock();
      update_f1_api(nullptr);
      update_ui(nullptr);
      create_or_reload_news_ui(nullptr);
      if (!clock_timer) clock_timer = lv_timer_create(update_ui, 60000, NULL);
      if (!f1_api_timer) f1_api_timer = lv_timer_create(update_f1_api, 3600000, NULL);
      if (!news_timer) news_timer = lv_timer_create(create_or_reload_news_ui, 5*60000, NULL);
      if (!statistics_timer) statistics_timer = lv_timer_create(sendStatisticData, 59*60000, NULL);
      if (!notifications_timer) notifications_timer = lv_timer_create(notification_scheduler_task, NOTIFICATION_INTERVAL_MS, NULL);
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

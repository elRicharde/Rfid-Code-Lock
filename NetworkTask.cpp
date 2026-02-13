#include "NetworkTask.h"
#include "DebugService.h"
#include "Secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>

// =============================================================
// Shared State Definitions
// =============================================================
SemaphoreHandle_t mutexDongleList = nullptr;
JsonDocument ramDonglesDoc;
JsonArray ramDonglesArr;
QueueHandle_t logQueue = nullptr;
QueueHandle_t buzzerSignalQueue = nullptr;

// =============================================================
// Internal State (file-scoped)
// =============================================================
static TaskHandle_t networkTaskHandle = nullptr;
static volatile int droppedLogCount = 0;  // Acceptable as volatile: single-writer monitoring counter
static unsigned long lastDongleRefreshTime = 0;
static unsigned long lastWifiReconnectCheck = 0;
static unsigned long lastLogRetryTime = 0;

// =============================================================
// Forward Declarations (internal)
// =============================================================
static void networkTaskLoop(void* param);
static void fetchAndStoreDongleIds();
static bool sendLogEntryViaHttp(const LogEntryStruct& entry);
static bool sendStoredLogEntries();
static void saveFailedLogEntry(const LogEntryStruct& entry);
static String urlEncode(const String& str);
static void sendBuzzerSignal(BuzzerSignal signal);

// =============================================================
// Public API
// =============================================================

void startNetworkTask() {
  logQueue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(LogEntryStruct));
  buzzerSignalQueue = xQueueCreate(1, sizeof(BuzzerSignal));

  xTaskCreatePinnedToCore(
    networkTaskLoop,
    "NetworkTask",
    NETWORK_TASK_STACK_SIZE,
    nullptr,
    NETWORK_TASK_PRIORITY,
    &networkTaskHandle,
    NETWORK_TASK_CORE
  );
}

void loadDonglesFromPersistentMemory() {
  // Called from setup() on the main core before network task starts.
  // No mutex needed yet because the network task hasn't been created.
  Preferences prefs;
  prefs.begin("dongleStore", true);  // ReadOnly = true
  String json = prefs.getString(PERS_MEM_DONGLE_IDS, "[]");
  prefs.end();

  DeserializationError error = deserializeJson(ramDonglesDoc, json);
  if (error) {
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Failed to deserialize persisted dongles: ", error.f_str());
    ramDonglesDoc.clear();
    deserializeJson(ramDonglesDoc, "[]");
  }
  ramDonglesArr = ramDonglesDoc.as<JsonArray>();
  DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Loaded ", ramDonglesArr.size(), " dongles from NVS");
}

bool enqueueLogEntry(const LogEntryStruct& entry) {
  if (logQueue == nullptr) {
    return false;
  }
  if (xQueueSend(logQueue, &entry, 0) != pdTRUE) {
    droppedLogCount++;
    DBG(DebugFlags::NETWORK_TASK, "Log queue full — entry dropped (total dropped: ", droppedLogCount, ")");
    return false;
  }
  return true;
}

void requestDongleRefresh() {
  if (networkTaskHandle != nullptr) {
    xTaskNotify(networkTaskHandle, 0, eNoAction);
  }
}

bool arrayContains(const JsonArray& arr, const JsonVariant& value) {
  for (const auto& v : arr) {
    if (v == value) {
      return true;
    }
  }
  return false;
}

// =============================================================
// Network Task Loop (runs on Core 0)
// =============================================================

static void networkTaskLoop(void* param) {
  (void)param;

  // Initial dongle fetch from Google Sheets
  fetchAndStoreDongleIds();
  lastDongleRefreshTime = millis();

  for (;;) {
    // --- WiFi reconnect ---
    if (millis() - lastWifiReconnectCheck > WIFI_RECONNECT_INTERVAL_MS) {
      lastWifiReconnectCheck = millis();
      if (WiFi.status() != WL_CONNECTED) {
        DBG(DebugFlags::WIFI_LOGGING, "WiFi disconnected, attempting reconnect...");
        WiFi.reconnect();
      }
    }

    // --- Dongle refresh: periodic timer ---
    if (millis() - lastDongleRefreshTime > DONGLE_REFRESH_INTERVAL_MS) {
      lastDongleRefreshTime = millis();
      fetchAndStoreDongleIds();
    }

    // --- Dongle refresh: on-demand via xTaskNotify (MasterCard scan) ---
    uint32_t notifyValue;
    if (xTaskNotifyWait(0, ULONG_MAX, &notifyValue, 0) == pdTRUE) {
      // Debounce: ignore requests within DONGLE_REFRESH_DEBOUNCE_MS of last refresh
      if (millis() - lastDongleRefreshTime > DONGLE_REFRESH_DEBOUNCE_MS) {
        DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "MasterCard triggered dongle refresh");
        fetchAndStoreDongleIds();
        lastDongleRefreshTime = millis();
      } else {
        DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "MasterCard refresh debounced (within 30s cooldown)");
      }
    }

    // --- Retry stored (failed) log entries with backoff ---
    if (millis() - lastLogRetryTime > LOG_RETRY_BACKOFF_MS) {
      lastLogRetryTime = millis();
      sendStoredLogEntries();
    }

    // --- Process queued log entries ---
    LogEntryStruct entry;
    while (xQueueReceive(logQueue, &entry, 0) == pdTRUE) {
      if (!sendLogEntryViaHttp(entry)) {
        saveFailedLogEntry(entry);
        sendBuzzerSignal(BUZZER_SOS);
        break;  // Stop processing queue — likely no connectivity
      }
    }

    // --- Dropped log warning ---
    if (droppedLogCount > 0) {
      DBG(DebugFlags::NETWORK_TASK, "WARNING: ", droppedLogCount, " log entries dropped since startup");
    }

    // --- Stack monitoring (debug only) ---
    #ifdef DEBUG_MODE
    {
      static unsigned long lastStackCheck = 0;
      if (millis() - lastStackCheck > 60000) {
        lastStackCheck = millis();
        DBG(DebugFlags::NETWORK_TASK, "NetworkTask free stack: ", uxTaskGetStackHighWaterMark(nullptr), " words");
      }
    }
    #endif

    vTaskDelay(pdMS_TO_TICKS(NETWORK_TASK_LOOP_DELAY_MS));
  }
}

// =============================================================
// HTTP Operations (internal, run on Core 0 only)
// =============================================================

static void fetchAndStoreDongleIds() {
  DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Begin fetchAndStoreDongleIds()");

  // --- Step 1: Read persisted dongles from NVS ---
  Preferences prefs;
  prefs.begin("dongleStore", true);  // ReadOnly = true for reading
  String persJson = prefs.getString(PERS_MEM_DONGLE_IDS, "[]");
  prefs.end();

  JsonDocument persDoc;
  DeserializationError error = deserializeJson(persDoc, persJson);
  if (error) {
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Failed to deserialize persisted dongles: ", error.f_str());
    persDoc.clear();
    deserializeJson(persDoc, "[]");
  }
  JsonArray persArr = persDoc.as<JsonArray>();
  DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Persistent dongles: ", persArr.size());

  // --- Step 2: Fetch from Google Sheets (HTTP — outside mutex!) ---
  HTTPClient http;
  http.setTimeout(20000);
  http.begin(WEB_APP_URL_READ);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int httpCode = http.GET();

  if (httpCode != 200) {
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "HTTP error: ", httpCode, " - ", http.errorToString(httpCode));
    http.end();
    sendBuzzerSignal(BUZZER_SOS);
    return;
  }

  String payload = http.getString();
  http.end();

  // --- Step 3: Parse response (still outside mutex) ---
  JsonDocument onlineDoc;
  error = deserializeJson(onlineDoc, payload);
  if (error) {
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Failed to deserialize online dongles: ", error.f_str());
    sendBuzzerSignal(BUZZER_SOS);
    return;
  }
  JsonArray onlineArr = onlineDoc.as<JsonArray>();

  #ifdef DEBUG_MODE
  if (DebugFlags::FETCH_AND_STORE_DONGLE_IDS_DETAIL) {
    for (JsonVariant v : onlineArr) {
      DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS_DETAIL, "  online: ", v.as<String>());
    }
  }
  #endif

  // --- Step 4: Compare online vs. persisted ---
  bool isDifferent = false;
  if (persArr.size() != onlineArr.size()) {
    isDifferent = true;
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Size diff: ", persArr.size(), " vs ", onlineArr.size());
  } else {
    for (JsonVariant v : onlineArr) {
      vTaskDelay(1);  // Yield to prevent watchdog reset on large lists
      if (!arrayContains(persArr, v)) {
        DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Diff: ", v.as<String>(), " not in persisted");
        isDifferent = true;
        break;
      }
    }
  }

  // --- Step 5: Update NVS if different (outside mutex — NVS is not thread-safe, but
  //     confined to this task only, so no concurrent NVS access is possible) ---
  if (isDifferent) {
    Preferences prefsWrite;
    prefsWrite.begin("dongleStore", false);
    prefsWrite.clear();
    prefsWrite.putString(PERS_MEM_DONGLE_IDS, payload);
    prefsWrite.end();
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "NVS updated with new dongle list");
  }

  // --- Step 6: Update RAM array (mutex ONLY during the brief swap) ---
  if (xSemaphoreTake(mutexDongleList, pdMS_TO_TICKS(100)) == pdTRUE) {
    ramDonglesDoc.clear();
    if (isDifferent) {
      // Use the fresh online data
      deserializeJson(ramDonglesDoc, payload);
    } else {
      // No change — ensure RAM is populated (covers cold-start with matching NVS)
      deserializeJson(ramDonglesDoc, persJson);
    }
    ramDonglesArr = ramDonglesDoc.as<JsonArray>();
    xSemaphoreGive(mutexDongleList);
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "RAM updated: ", ramDonglesArr.size(), " dongles");
  } else {
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Mutex timeout during RAM update!");
  }

  if (isDifferent) {
    sendBuzzerSignal(BUZZER_OK);
  }

  DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "End fetchAndStoreDongleIds()");
}

static bool sendLogEntryViaHttp(const LogEntryStruct& entry) {
  HTTPClient http;
  String url = String(WEB_APP_URL) + "?action=write_log_pa";
  url += "&date=" + urlEncode(String(entry.date));
  url += "&time=" + urlEncode(String(entry.time));
  url += "&access=" + urlEncode(String(entry.access));
  url += "&dongle_id=" + urlEncode(String(entry.dongle_id));

  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int httpCode = http.GET();
  http.end();

  return httpCode == 200;
}

static bool sendStoredLogEntries() {
  DBG(DebugFlags::SEND_STORED_LOG_ENTRIES, "Start sendStoredLogEntries()");

  Preferences prefsLog;
  prefsLog.begin(PERS_MEM_FAILED_LOGS, false);

  JsonDocument doc;
  String keyArrayStr = prefsLog.getString("keyArray", "[]");
  DeserializationError error = deserializeJson(doc, keyArrayStr);
  if (error) {
    DBG(DebugFlags::SEND_STORED_LOG_ENTRIES, "Failed to deserialize keyArray: ", error.f_str());
    prefsLog.clear();
    prefsLog.end();
    return true;  // Corrupted data cleared — treat as empty
  }
  JsonArray keyArray = doc.as<JsonArray>();

  if (keyArray.size() == 0) {
    prefsLog.end();
    return true;  // Nothing stored
  }

  int originalKeyCount = keyArray.size();
  int i = 0;
  while (i < (int)keyArray.size()) {
    String key = keyArray[i];
    String csv = prefsLog.getString(key.c_str(), "");

    if (csv != "") {
      int c1 = csv.indexOf(',');
      int c2 = csv.indexOf(',', c1 + 1);
      int c3 = csv.indexOf(',', c2 + 1);

      LogEntryStruct entry;
      safeCopyStringToChar(csv.substring(0, c1), entry.date, CharArrayDateSize);
      safeCopyStringToChar(csv.substring(c1 + 1, c2), entry.time, CharArrayTimeSize);
      safeCopyStringToChar(csv.substring(c2 + 1, c3), entry.access, CharArrayAccessSize);
      safeCopyStringToChar(csv.substring(c3 + 1), entry.dongle_id, CharArrayDongleIdSize);

      if (sendLogEntryViaHttp(entry)) {
        prefsLog.remove(key.c_str());
        keyArray.remove(i);
        // No i++ needed — array shifted left
      } else {
        break;  // No connectivity — stop trying
      }
    } else {
      // Empty entry — skip to prevent infinite loop
      i++;
    }

    vTaskDelay(1);  // Yield between HTTP calls
  }

  if (keyArray.size() == 0) {
    prefsLog.remove("keyArray");
    prefsLog.end();
    return true;
  } else if ((int)keyArray.size() != originalKeyCount) {
    // Partial success — update persisted key array
    String updatedKeys;
    serializeJson(doc, updatedKeys);
    prefsLog.putString("keyArray", updatedKeys.c_str());
    prefsLog.end();
    return false;
  } else {
    prefsLog.end();
    return false;
  }
}

static void saveFailedLogEntry(const LogEntryStruct& entry) {
  Preferences prefsLog;
  prefsLog.begin(PERS_MEM_FAILED_LOGS, false);

  JsonDocument doc;
  String keyArrayStr = prefsLog.getString("keyArray", "[]");
  DeserializationError error = deserializeJson(doc, keyArrayStr);
  if (error) {
    doc.clear();
    deserializeJson(doc, "[]");
  }
  JsonArray keyArray = doc.as<JsonArray>();

  // Generate a unique key
  String newKey;
  int keyIndex = keyArray.size();
  bool keyExists = true;
  while (keyExists) {
    keyExists = false;
    keyIndex++;
    newKey = "log" + String(keyIndex);
    for (int i = 0; i < (int)keyArray.size(); i++) {
      if (keyArray[i].as<String>() == newKey) {
        keyExists = true;
        break;
      }
    }
  }

  String csv = String(entry.date) + "," + String(entry.time) + "," + String(entry.access) + "," + String(entry.dongle_id);
  prefsLog.putString(newKey.c_str(), csv);

  keyArray.add(newKey);

  String updatedKeys;
  serializeJson(doc, updatedKeys);
  prefsLog.putString("keyArray", updatedKeys.c_str());
  prefsLog.end();
}

static String urlEncode(const String& str) {
  // URL-encode for safe HTTP query parameters (RFC 3986).
  // Unreserved characters pass through; all others are percent-encoded.
  String encoded;
  encoded.reserve(str.length() + 8);
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

static void sendBuzzerSignal(BuzzerSignal signal) {
  // Send buzzer signal to main loop via queue (depth 1, overwrite semantics).
  // Network task cannot call buzzer directly — not thread-safe.
  if (buzzerSignalQueue != nullptr) {
    xQueueOverwrite(buzzerSignalQueue, &signal);
  }
}

#include "NetworkTask.h"
#include "DebugService.h"
#include "Secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <atomic>

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
static std::atomic<int> droppedLogCount{0};  // Atomic: written on Core 1, read on Core 0
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
  configASSERT(logQueue != nullptr);
  configASSERT(buzzerSignalQueue != nullptr);

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
  // Must be called BEFORE startNetworkTask() — no mutex taken because
  // the network task does not exist yet.
  configASSERT(networkTaskHandle == nullptr);

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
    DBG(DebugFlags::NETWORK_TASK, "Log queue full — entry dropped (total: ", droppedLogCount.load(), ")");
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
        DBG(DebugFlags::WIFI_LOGGING, "WiFi disconnected, reconnecting...");
        WiFi.disconnect();
        WiFi.begin(SSID, WIFI_PASSWORD);
      }
    }

    // --- Dongle refresh: periodic timer ---
    if (millis() - lastDongleRefreshTime > DONGLE_REFRESH_INTERVAL_MS) {
      lastDongleRefreshTime = millis();
      fetchAndStoreDongleIds();
    }

    // --- Dongle refresh: on-demand via xTaskNotify (MasterCard scan) ---
    uint32_t notifyValue;
    if (xTaskNotifyWait(0, UINT32_MAX, &notifyValue, 0) == pdTRUE) {
      // Debounce: ignore requests within DONGLE_REFRESH_DEBOUNCE_MS of last refresh
      if (millis() - lastDongleRefreshTime > DONGLE_REFRESH_DEBOUNCE_MS) {
        DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "MasterCard triggered dongle refresh");
        fetchAndStoreDongleIds();
        lastDongleRefreshTime = millis();
      } else {
        DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Refresh debounced (30s cooldown)");
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
    int dropped = droppedLogCount.load();
    if (dropped > 0) {
      DBG(DebugFlags::NETWORK_TASK, "WARNING: ", dropped, " log entries dropped since startup");
    }

    // --- Debug monitoring (stack + heap) ---
    #ifdef DEBUG_MODE
    {
      static unsigned long lastMonitorCheck = 0;
      if (millis() - lastMonitorCheck > 60000) {
        lastMonitorCheck = millis();
        DBG(DebugFlags::NETWORK_TASK, "NetworkTask free stack: ", uxTaskGetStackHighWaterMark(nullptr), " words");
        DBG(DebugFlags::NETWORK_TASK, "Free heap: ", ESP.getFreeHeap(), " min: ", ESP.getMinFreeHeap());
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
  prefs.begin("dongleStore", true);  // ReadOnly = true
  String persJson = prefs.getString(PERS_MEM_DONGLE_IDS, "[]");
  prefs.end();

  // --- Step 2: Fetch from Google Sheets (outside mutex!) ---
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

  // --- Step 3: Validate response by parsing (still outside mutex) ---
  JsonDocument validationDoc;
  DeserializationError error = deserializeJson(validationDoc, payload);
  if (error) {
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Failed to deserialize online dongles: ", error.f_str());
    sendBuzzerSignal(BUZZER_SOS);
    return;
  }

  #ifdef DEBUG_MODE
  if (DebugFlags::FETCH_AND_STORE_DONGLE_IDS_DETAIL) {
    JsonArray debugArr = validationDoc.as<JsonArray>();
    for (JsonVariant v : debugArr) {
      DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS_DETAIL, "  online: ", v.as<String>());
    }
  }
  #endif

  // --- Step 4: Compare online vs. persisted ---
  // Simple string comparison of raw JSON payloads. The Google Script returns
  // deterministic JSON, and persJson is the raw payload from the previous fetch.
  bool isDifferent = (payload != persJson);
  if (isDifferent) {
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Online data differs from persisted");
  }

  // --- Step 5: Update NVS if different (NVS confined to this task — no concurrent access) ---
  if (isDifferent) {
    Preferences prefsWrite;
    prefsWrite.begin("dongleStore", false);
    prefsWrite.putString(PERS_MEM_DONGLE_IDS, payload);  // Overwrite directly, no clear() needed
    prefsWrite.end();
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "NVS updated with new dongle list");
  }

  // --- Step 6: Pre-parse JSON outside mutex, then swap inside (brief critical section) ---
  JsonDocument newDoc;
  deserializeJson(newDoc, isDifferent ? payload : persJson);

  if (xSemaphoreTake(mutexDongleList, pdMS_TO_TICKS(100)) == pdTRUE) {
    ramDonglesDoc = std::move(newDoc);
    ramDonglesArr = ramDonglesDoc.as<JsonArray>();
    int newSize = ramDonglesArr.size();  // Capture inside mutex before releasing
    xSemaphoreGive(mutexDongleList);
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "RAM updated: ", newSize, " dongles");
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
  String url;
  url.reserve(256);  // Pre-allocate to reduce heap fragmentation
  url = WEB_APP_URL;
  url += "?action=write_log_pa";
  url += "&date="; url += urlEncode(String(entry.date));
  url += "&time="; url += urlEncode(String(entry.time));
  url += "&access="; url += urlEncode(String(entry.access));
  url += "&dongle_id="; url += urlEncode(String(entry.dongle_id));

  http.begin(url);
  http.setTimeout(20000);
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

      // Validate CSV structure — malformed entries are removed
      if (c1 < 0 || c2 < 0 || c3 < 0) {
        DBG(DebugFlags::SEND_STORED_LOG_ENTRIES, "Malformed CSV entry removed: ", key);
        prefsLog.remove(key.c_str());
        keyArray.remove(i);
        continue;
      }

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

  // Enforce maximum stored log count to prevent NVS partition exhaustion.
  // Oldest entries are discarded first (FIFO).
  while ((int)keyArray.size() >= MAX_FAILED_LOGS) {
    String oldestKey = keyArray[0].as<String>();
    prefsLog.remove(oldestKey.c_str());
    keyArray.remove(0);
    DBG(DebugFlags::SEND_STORED_LOG_ENTRIES, "NVS full — discarded oldest log: ", oldestKey);
  }

  // Generate unique key using monotonic counter (avoids O(n^2) search)
  int nextIndex = prefsLog.getInt("nextIdx", 1);
  String newKey = "log" + String(nextIndex);
  prefsLog.putInt("nextIdx", nextIndex + 1);

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

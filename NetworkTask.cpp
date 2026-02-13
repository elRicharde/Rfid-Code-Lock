#include "NetworkTask.h"
#include "DebugService.h"
#include "Secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <atomic>

// =============================================================
// Encapsulated State (file-scoped — no external access possible)
// =============================================================
static SemaphoreHandle_t mutexDongleList = nullptr;
static JsonDocument ramDonglesDoc;
static JsonArray ramDonglesArr;
static QueueHandle_t logQueue = nullptr;
static QueueHandle_t buzzerSignalQueue = nullptr;
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
static int urlEncodeToBuffer(const char* src, char* dest, int destSize);
static void sendBuzzerSignal(BuzzerSignal signal);
static bool arrayContains(const JsonArray& arr, const JsonVariant& value);

// =============================================================
// Public API
// =============================================================

void startNetworkTask() {
  // Create mutex (must exist before task starts using the dongle list)
  mutexDongleList = xSemaphoreCreateMutex();
  configASSERT(mutexDongleList != nullptr);

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
  char json[2048];
  json[0] = '\0';
  size_t len = prefs.getString(PERS_MEM_DONGLE_IDS, json, sizeof(json));
  prefs.end();
  if (len == 0 || json[0] == '\0') {
    strcpy(json, "[]");
  }

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

bool isDongleIdAuthorized(const String& dongleIdStr) {
  // MasterCard check: triggers async DB refresh without granting access.
  // No mutex needed — doesn't read the dongle list.
  if (dongleIdStr.equals(DONGLE_MASTER_CARD_UPDATE_DB)) {
    DBG(DebugFlags::DONGLE_AUTH, "MasterCard scanned — requesting dongle refresh");
    requestDongleRefresh();
    return false;
  }

  if (xSemaphoreTake(mutexDongleList, pdMS_TO_TICKS(100)) == pdTRUE) {
    bool authorized = false;
    for (JsonVariant v : ramDonglesArr) {
      DBG(DebugFlags::DONGLE_AUTH, "Compare: ", dongleIdStr, " vs ", v.as<String>());

      // Special value: if the list contains OPEN_FOR_ALL_DONGLES, grant access to everyone
      if (v.as<String>() == OPEN_FOR_ALL_DONGLES) {
        authorized = true;
        break;
      }
      if (dongleIdStr.equals(v.as<String>())) {
        authorized = true;
        break;
      }
    }
    xSemaphoreGive(mutexDongleList);
    return authorized;
  }

  DBG(DebugFlags::DONGLE_AUTH, "Mutex timeout — returning unauthorized");
  return false;
}

bool receiveBuzzerSignal(BuzzerSignal* outSignal) {
  if (buzzerSignalQueue == nullptr || outSignal == nullptr) {
    return false;
  }
  return xQueueReceive(buzzerSignalQueue, outSignal, 0) == pdTRUE;
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

  // --- Step 1: Read persisted dongles from NVS into stack buffer ---
  Preferences prefs;
  prefs.begin("dongleStore", true);  // ReadOnly = true
  char persJson[2048];
  persJson[0] = '\0';
  size_t persLen = prefs.getString(PERS_MEM_DONGLE_IDS, persJson, sizeof(persJson));
  prefs.end();
  if (persLen == 0 || persJson[0] == '\0') {
    strcpy(persJson, "[]");
  }

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

  // --- Step 3: Read response into stack buffer via stream (zero heap allocation) ---
  char payload[2048];
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "No stream available");
    http.end();
    sendBuzzerSignal(BUZZER_SOS);
    return;
  }
  size_t bytesRead = stream->readBytes(payload, sizeof(payload) - 1);
  payload[bytesRead] = '\0';
  http.end();

  if (bytesRead == 0) {
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Empty response");
    sendBuzzerSignal(BUZZER_SOS);
    return;
  }

  // --- Step 4: Validate response by parsing (still outside mutex) ---
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

  // --- Step 5: Compare online vs. persisted ---
  // Simple string comparison of raw JSON payloads. The Google Script returns
  // deterministic JSON, and persJson is the raw payload from the previous fetch.
  bool isDifferent = (strcmp(payload, persJson) != 0);
  if (isDifferent) {
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Online data differs from persisted");
  }

  // --- Step 6: Update NVS if different (NVS confined to this task — no concurrent access) ---
  if (isDifferent) {
    Preferences prefsWrite;
    prefsWrite.begin("dongleStore", false);
    prefsWrite.putString(PERS_MEM_DONGLE_IDS, payload);  // Overwrite directly, no clear() needed
    prefsWrite.end();
    DBG(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "NVS updated with new dongle list");
  }

  // --- Step 7: Pre-parse JSON outside mutex, then swap inside (brief critical section) ---
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
  // Build URL in stack buffer — zero heap allocation
  char url[384];
  int pos = snprintf(url, sizeof(url), "%s?action=write_log_pa&date=", WEB_APP_URL);
  pos += urlEncodeToBuffer(entry.date, url + pos, sizeof(url) - pos);
  pos += snprintf(url + pos, sizeof(url) - pos, "&time=");
  pos += urlEncodeToBuffer(entry.time, url + pos, sizeof(url) - pos);
  pos += snprintf(url + pos, sizeof(url) - pos, "&access=");
  pos += urlEncodeToBuffer(entry.access, url + pos, sizeof(url) - pos);
  pos += snprintf(url + pos, sizeof(url) - pos, "&dongle_id=");
  pos += urlEncodeToBuffer(entry.dongle_id, url + pos, sizeof(url) - pos);

  HTTPClient http;
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

  // Read keyArray from NVS into stack buffer
  JsonDocument doc;
  char keyArrayBuf[1024];
  keyArrayBuf[0] = '\0';
  size_t keyBufLen = prefsLog.getString("keyArray", keyArrayBuf, sizeof(keyArrayBuf));
  if (keyBufLen == 0 || keyArrayBuf[0] == '\0') {
    strcpy(keyArrayBuf, "[]");
  }
  DeserializationError error = deserializeJson(doc, keyArrayBuf);
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
    // Read CSV log entry into stack buffer
    const char* key = keyArray[i].as<const char*>();
    char csv[128];  // Max: date(10)+time(8)+access(14)+dongle(26)+3commas+null = 62
    csv[0] = '\0';
    size_t csvLen = prefsLog.getString(key, csv, sizeof(csv));

    if (csvLen > 0 && csv[0] != '\0') {
      int c1 = -1, c2 = -1, c3 = -1;
      for (int j = 0; csv[j] != '\0'; j++) {
        if (csv[j] == ',') {
          if (c1 < 0) c1 = j;
          else if (c2 < 0) c2 = j;
          else if (c3 < 0) { c3 = j; break; }
        }
      }

      // Validate CSV structure — malformed entries are removed
      if (c1 < 0 || c2 < 0 || c3 < 0) {
        DBG(DebugFlags::SEND_STORED_LOG_ENTRIES, "Malformed CSV entry removed: ", key);
        prefsLog.remove(key);
        keyArray.remove(i);
        continue;
      }

      // Parse CSV fields directly from the stack buffer
      LogEntryStruct entry;
      csv[c1] = '\0';  // Terminate date
      csv[c2] = '\0';  // Terminate time
      csv[c3] = '\0';  // Terminate access
      safeCopyStringToChar(csv, entry.date, CharArrayDateSize);
      safeCopyStringToChar(csv + c1 + 1, entry.time, CharArrayTimeSize);
      safeCopyStringToChar(csv + c2 + 1, entry.access, CharArrayAccessSize);
      safeCopyStringToChar(csv + c3 + 1, entry.dongle_id, CharArrayDongleIdSize);

      if (sendLogEntryViaHttp(entry)) {
        prefsLog.remove(key);
        keyArray.remove(i);
        // No i++ needed — array shifted left
      } else {
        break;  // No connectivity — stop trying
      }
    } else {
      // Empty entry — remove orphaned key to prevent accumulation
      prefsLog.remove(key);
      keyArray.remove(i);
    }

    vTaskDelay(1);  // Yield between HTTP calls
  }

  if (keyArray.size() == 0) {
    prefsLog.remove("keyArray");
    prefsLog.end();
    return true;
  } else if ((int)keyArray.size() != originalKeyCount) {
    // Partial success — serialize to stack buffer and update NVS
    char updatedBuf[1024];
    serializeJson(doc, updatedBuf, sizeof(updatedBuf));
    prefsLog.putString("keyArray", updatedBuf);
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

  // Read keyArray from NVS into stack buffer
  JsonDocument doc;
  char keyArrayBuf[1024];
  keyArrayBuf[0] = '\0';
  size_t keyBufLen = prefsLog.getString("keyArray", keyArrayBuf, sizeof(keyArrayBuf));
  if (keyBufLen == 0 || keyArrayBuf[0] == '\0') {
    strcpy(keyArrayBuf, "[]");
  }
  DeserializationError error = deserializeJson(doc, keyArrayBuf);
  if (error) {
    doc.clear();
    deserializeJson(doc, "[]");
  }
  JsonArray keyArray = doc.as<JsonArray>();

  // Enforce maximum stored log count to prevent NVS partition exhaustion.
  // Oldest entries are discarded first (FIFO).
  while ((int)keyArray.size() >= MAX_FAILED_LOGS) {
    const char* oldestKey = keyArray[0].as<const char*>();
    DBG(DebugFlags::SEND_STORED_LOG_ENTRIES, "NVS full — discarded oldest log: ", oldestKey);
    prefsLog.remove(oldestKey);
    keyArray.remove(0);
  }

  // Generate unique key using monotonic counter (avoids O(n^2) search)
  int nextIndex = prefsLog.getInt("nextIdx", 1);
  char newKey[16];
  snprintf(newKey, sizeof(newKey), "log%d", nextIndex);
  prefsLog.putInt("nextIdx", nextIndex + 1);

  // Build CSV in stack buffer
  char csv[128];
  snprintf(csv, sizeof(csv), "%s,%s,%s,%s", entry.date, entry.time, entry.access, entry.dongle_id);
  prefsLog.putString(newKey, csv);

  keyArray.add(newKey);

  // Serialize updated keyArray to stack buffer
  char updatedBuf[1024];
  serializeJson(doc, updatedBuf, sizeof(updatedBuf));
  prefsLog.putString("keyArray", updatedBuf);
  prefsLog.end();
}

// =============================================================
// Utility Functions (internal)
// =============================================================

static int urlEncodeToBuffer(const char* src, char* dest, int destSize) {
  // URL-encode src directly into dest buffer (RFC 3986, zero heap allocation).
  // Returns number of bytes written (excluding null terminator).
  int pos = 0;
  for (int i = 0; src[i] != '\0' && pos < destSize - 4; i++) {
    char c = src[i];
    if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      dest[pos++] = c;
    } else {
      pos += snprintf(dest + pos, destSize - pos, "%%%02X", (unsigned char)c);
    }
  }
  if (pos < destSize) {
    dest[pos] = '\0';
  }
  return pos;
}

static void sendBuzzerSignal(BuzzerSignal signal) {
  // Send buzzer signal to main loop via queue (depth 1, overwrite semantics).
  // Network task cannot call buzzer directly — not thread-safe.
  if (buzzerSignalQueue != nullptr) {
    xQueueOverwrite(buzzerSignalQueue, &signal);
  }
}

static bool arrayContains(const JsonArray& arr, const JsonVariant& value) {
  for (const auto& v : arr) {
    if (v == value) {
      return true;
    }
  }
  return false;
}

/*
RFID Dongle Zugangskontrolle für null7b Technikecke PA
======================================================

am Schloss sind: 
  schwarz = GND
  rot = 5 V open
  gelb/blau = NO Kontakt (ist das Schloss offen, gibt es kein Durchgang)

durch die Steckerverbinder ändern sich die Kabelfarben am Board wie folgt:
  gelb = GND
  blau = 5 V open
  rot/schwarz = NO Kontakt (ist das Schloss offen, gibt es kein Durchgang)

*/


#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "Secrets.h"
#include "DebugService.h"
#include <ArduinoJson.h>
#include "time.h"
#include <Preferences.h>
#include "freertos/semphr.h"
#include "ArduinoBuzzerSoundsRG.h"

constexpr long GMT_OFFSET_SEC = 3600;
constexpr int DAYLIGHT_OFFSET_SEC = 3600;
constexpr const char TIME_SERVER_1[16] = "de.pool.ntp.org";
constexpr const char TIME_SERVER_2[13] = "pool.ntp.org";
constexpr const char TIME_SERVER_3[14] = "time.nist.gov";
                                      
constexpr const char PERS_MEM_DONGLE_IDS[10] = "DongleIds";
constexpr const char PERS_MEM_FAILED_LOGS[12] = "Failed_Logs";

constexpr int BUZZERPIN = 4;
constexpr int UNLOCKPIN = 2;
constexpr int DOOR_STATE_PIN = 12;
constexpr int INTERRUPT_IO_PIN_1 = 10;
constexpr int INTERRUPT_IO_PIN_2 = 8;
constexpr int SWITCHDURATION_ms = 250;
constexpr int DOOR_IS_CLOSED = 0;
constexpr int DOOR_IS_OPEN = 1;
constexpr int WIEGAND_TIMEOUT_MS = 200;               // Timeout in ms to reset partial Wiegand reads (see loop() for design rationale)
constexpr float DONGLE_REFRESH_INTERVAL_HOURS = 4.0;  // Periodic dongle DB refresh (range: 0.01 for testing, 0.5 - 72.0 for production)
constexpr unsigned long DONGLE_REFRESH_INTERVAL_MS = (unsigned long)(DONGLE_REFRESH_INTERVAL_HOURS * 3600.0f * 1000.0f);
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 30000;  // Check and reconnect WiFi every 30 seconds


enum CharArraySizes {
  CharArrayDateSize = 11,
  CharArrayTimeSize = 9,
  CharArrayAccessSize = 15,
  CharArrayDongleIdSize = 27,
};
typedef struct {
  char date[CharArrayDateSize];
  char time[CharArrayTimeSize];
  char access[CharArrayAccessSize];
  char dongle_id[CharArrayDongleIdSize];
} LogEntryStruct;
LogEntryStruct logEntry;

volatile int bitCount = 0;               // Counter for the number of bits received
volatile unsigned long dongleValue = 0;  // Variable to store the dongle value
int DoorStateMemory = 2;                  // DoorStateMemory Initial = 2, in use can be only 1 or 0
volatile unsigned long lastBitTime = 0;   // Timestamp of last received Wiegand bit (for timeout detection in loop)
unsigned long lastDongleRefreshTime = 0;  // Tracks when dongle IDs were last refreshed from Google Sheets
unsigned long lastWifiReconnectCheck = 0; // Tracks last WiFi status check for reconnect logic


SemaphoreHandle_t mutexDongleList; // Mutex protecting ramDonglesArr (the in-RAM dongle list)
Preferences preferencesDongles;  // for access to persistent memory of the ESP32 - mem area for dongles
Preferences preferencesLog;  // for access to persistent memory of the ESP32 - mem area for dongles
JsonDocument ramDonglesDoc;  // JSON Doc for handling DongleIds in string like array as global var in ram
JsonArray ramDonglesArr; // Properly initialized in setup() after deserializing the JSON document

BuzzerSoundsRgNonRtos* buzzerSounds;

// =============================================================================================================================
// Debugging Control ===========================================================================================================
DebugService* debugService;                                 
struct DebugFlags {                                         
  // static constexpr bool DEBUG_MODE = true;    // Muss immer true sein, wenn mindestens ein anderes Flag true ist        
  // static constexpr bool WIFI_LOGGING = true;                
  // static constexpr bool FETCH_AND_STORE_DONGLE_IDS = true;
  // static constexpr bool FETCH_AND_STORE_DONGLE_IDS_DETAIL = true;
  // static constexpr bool DOOR_STATE = true;
  // static constexpr bool DONGLE_SCAN = true;
  // static constexpr bool DONGLE_AUTH = true;
  // static constexpr bool SEND_STORED_DONLGE_LOG_ENTRIES = true;

  static constexpr bool DEBUG_MODE = false;    // Muss immer true sein, wenn mindestens ein anderes Flag true ist        
  static constexpr bool WIFI_LOGGING = false;                
  static constexpr bool FETCH_AND_STORE_DONGLE_IDS = false;
  static constexpr bool FETCH_AND_STORE_DONGLE_IDS_DETAIL = false;
  static constexpr bool DOOR_STATE = false;
  static constexpr bool DONGLE_SCAN = false;
  static constexpr bool DONGLE_AUTH = false;
  static constexpr bool SEND_STORED_DONLGE_LOG_ENTRIES = false;
};                                                          
// extern defefinition needed for linker, obsolete from c++17 with inline in struct
constexpr bool DebugFlags::DEBUG_MODE;    
constexpr bool DebugFlags::WIFI_LOGGING;  
constexpr bool DebugFlags::FETCH_AND_STORE_DONGLE_IDS;
constexpr bool DebugFlags::FETCH_AND_STORE_DONGLE_IDS_DETAIL;
constexpr bool DebugFlags::DOOR_STATE;
constexpr bool DebugFlags::DONGLE_SCAN;
constexpr bool DebugFlags::DONGLE_AUTH;
constexpr bool DebugFlags::SEND_STORED_DONLGE_LOG_ENTRIES;
// Debugging Control ===========================================================================================================
// =============================================================================================================================





// Function Defenitions ========================================================================================================
// =============================================================================================================================

// This function formats the current date and time
void getCurrentDateTime(char formattedDate[11], char formattedTime[9]);

bool safeCopyStringToChar(const String& source, char* dest, size_t destSize);

// URL-encode a string for safe HTTP query parameters
String urlEncode(const String& str);

// check if Value is included in JsonArray
bool arrayContains(const JsonArray& arr, const JsonVariant& value);

// ISR (interrupt Service Routine) for Data0 (represents bit '0') in the Wiegand protocol
void IRAM_ATTR ISRreceiveData0();

// ISR (Interrupt Service Routine) for Data1 (represents bit '1') in the Wiegand protocol.
void IRAM_ATTR ISRreceiveData1();

void fetchAndStoreDongleIds();

// log doorstate when changed
void trackDoorStateChange();
  
// main Posting Method  
void PostLog(LogEntryStruct &logEntry);

// sub Posting methods
bool sendStoredLogEntries();
bool sendLogEntryViaHttp(LogEntryStruct &logEntry);
void saveFailedLogEntry(LogEntryStruct &logEntry);

void handleRFIDScanResult();

bool isDongleIdAuthorized(String dongleIdStr);

// unlock with a short signal
void unlock();


// SetUp ======================================================================================================================
//=============================================================================================================================
void setup() {
  // Always initialize Serial and DebugService to prevent nullptr crashes.
  // When DEBUG_MODE is false, debug calls are optimized away by the compiler (constexpr),
  // but the instance must exist for safe method calls. Cost: ~80 bytes RAM for the mutex.
  Serial.begin(115200);
  debugService = DebugService::getInstance();

  if (DebugFlags::DEBUG_MODE) {
    debugService->SerialPrintln_ifDebug(DebugFlags::DEBUG_MODE, "5 seconds......");
    delay(1000);
    debugService->SerialPrintln_ifDebug(DebugFlags::DEBUG_MODE, "4 seconds.....");
    delay(1000);
    debugService->SerialPrintln_ifDebug(DebugFlags::DEBUG_MODE, "3 seconds....");
    delay(1000);
    debugService->SerialPrintln_ifDebug(DebugFlags::DEBUG_MODE, "2 seconds...");
    delay(1000);
    debugService->SerialPrintln_ifDebug(DebugFlags::DEBUG_MODE, "1 seconds..");
    delay(1000);
    debugService->SerialPrintln_ifDebug(DebugFlags::DEBUG_MODE, "Start!");
  }

  // connect to WIFI
  WiFi.begin(SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {  // Wait until the connection is established, max. 10 seconds, then continue without Internet
    debugService->SerialPrintln_ifDebug(DebugFlags::WIFI_LOGGING, "Connect to WiFi ", SSID, " max 10 seconds: ", attempts, " seconds");
    attempts = attempts + 1;
    delay(1000);
  }

  if (WiFi.status() == WL_CONNECTED) {
    debugService->SerialPrintln_ifDebug(DebugFlags::WIFI_LOGGING, "Verbunden mit dem WiFi-Netzwerk");
    debugService->SerialPrintln_ifDebug(DebugFlags::WIFI_LOGGING, "IP-Adresse: ", WiFi.localIP());  // output of the ESP32 IP address
  } else {
    debugService->SerialPrintln_ifDebug(DebugFlags::WIFI_LOGGING, "nicht mit dem Internet Verbunden");  // connect and reconnect take place automatically depending on WiFi availability
  }

  buzzerSounds = new BuzzerSoundsRgNonRtos(BUZZERPIN); // create instance of BuzzerSoundsRgRtos
  // Init and get the time
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, TIME_SERVER_1, TIME_SERVER_2, TIME_SERVER_3);

  pinMode(UNLOCKPIN, OUTPUT);           // Pin for Unlock
  pinMode(DOOR_STATE_PIN, INPUT_PULLUP);  // Pin for Unlock (Open = 1 / closed = 0)

  // Set up the interrupt pins, attach the interrupt routines
  pinMode(INTERRUPT_IO_PIN_1, INPUT_PULLUP);  // Set up first Pin used for the Lock in Wiegand-Mode as input with internal pull-up
  pinMode(INTERRUPT_IO_PIN_2, INPUT_PULLUP);  // Set up second Pin used for the Lock in Wiegand-Mode as input with internal pull-up
  attachInterrupt(digitalPinToInterrupt(INTERRUPT_IO_PIN_1), ISRreceiveData0, FALLING);
  attachInterrupt(digitalPinToInterrupt(INTERRUPT_IO_PIN_2), ISRreceiveData1, FALLING);

  String ramDonglesJSON = "[]";
  deserializeJson(ramDonglesDoc, ramDonglesJSON); // deserialize (parse persOfflineDonglesJson to fill persOfflineDonglesDoc with its content)
  ramDonglesArr = ramDonglesDoc.as<JsonArray>(); // convert to "linked" array
  if (ramDonglesArr.isNull()) {
      debugService->SerialPrintln_ifDebug(DebugFlags::DEBUG_MODE, "Das Array ramDonglesArr ist N I C H T initialisiert oder N I C H T gültig");
  } else {
      debugService->SerialPrintln_ifDebug(DebugFlags::DEBUG_MODE, "Das Array ramDonglesArr ist initialisiert und gültig");
  }

  mutexDongleList = xSemaphoreCreateMutex();
  fetchAndStoreDongleIds();
  lastDongleRefreshTime = millis();  // Start periodic refresh timer after initial fetch
} //setup()


// Loop =======================================================================================================================
//=============================================================================================================================
void loop() {
  trackDoorStateChange();
  handleRFIDScanResult();

  // Reset partial RFID reads that timed out.
  // Design: The Wiegand protocol transmits all 26 bits within ~52ms (2ms per bit).
  // If interference or a partial read leaves bitCount between 1-25, no further scans
  // can succeed because bitCount never reaches 26. A 200ms timeout is chosen to be
  // well above the maximum valid transmission time while still recovering quickly.
  if (bitCount > 0 && bitCount < 26 && millis() - lastBitTime > WIEGAND_TIMEOUT_MS) {
    noInterrupts();
    bitCount = 0;
    dongleValue = 0;
    interrupts();
  }

  // Periodic WiFi reconnect check
  if (millis() - lastWifiReconnectCheck > WIFI_RECONNECT_INTERVAL_MS) {
    lastWifiReconnectCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      debugService->SerialPrintln_ifDebug(DebugFlags::WIFI_LOGGING, "WiFi disconnected, attempting reconnect...");
      WiFi.reconnect();
    }
  }

  // Periodic dongle ID refresh from Google Sheets.
  // Note: This briefly blocks the main loop during the HTTP call (typically 1-3s, max 20s).
  // RFID scanning is paused during this time. For non-blocking refresh,
  // see BACKLOG.md item 1: "Refactor HTTP calls to separate FreeRTOS task".
  if (millis() - lastDongleRefreshTime > DONGLE_REFRESH_INTERVAL_MS) {
    lastDongleRefreshTime = millis();
    fetchAndStoreDongleIds();
  }

  // Yield to RTOS scheduler and reduce CPU load.
  // RFID bits are captured by hardware interrupts (ISR) and are never missed by this delay.
  // Door state changes occur in the seconds range; 10ms = 100 checks/s is more than sufficient.
  delay(10);
} // loop()


// Function Implementations ===================================================================================================
//=============================================================================================================================

void getCurrentDateTime(char formattedDate[CharArraySizes::CharArrayDateSize], char formattedTime[CharArraySizes::CharArrayTimeSize]) {
// This function formats the current date and time
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    // Error, time determination failed
    safeCopyStringToChar("Date Error", formattedDate, CharArraySizes::CharArrayDateSize);
    safeCopyStringToChar("Date Err", formattedTime, CharArraySizes::CharArrayTimeSize);
    return;
  }

  // Create the date and time strings in the desired format
  // DD.MM.YYYY = 10 Char + 1 for '\0' (null terminator)
  strftime(formattedDate, CharArraySizes::CharArrayDateSize, "%d.%m.%Y", &timeinfo);
  // HH:MM:SS = 8 Char + 1 for '\0' (null terminator)
  strftime(formattedTime, CharArraySizes::CharArrayTimeSize, "%H:%M:%S", &timeinfo);
} //getCurrentDateTime



bool safeCopyStringToChar(const String& source, char* dest, size_t destSize) {
  /*
  The function safeCopyStringToChar was developed to safely and robustly copy a 
  C++ string (this is a dynamic string from the Arduino environment) into a 
  C array of characters (char[]). The aim of this function is to prevent possible 
  memory overflows and unsafe memory accesses that could occur if a string that is 
  too large is copied into a character array that is too small. 
  */
  if (dest == nullptr || destSize <= 0) {
    return false;  // Error if destination is invalid or zero size
  }

  // Check if the source string plus null terminator fits in the buffer
  if (source.length() + 1 > destSize) {
    return false;  // The source string is too long for the buffer
  }

  memset(dest, 0, destSize);  // clear whole destination first

  // Copy the string into the buffer
  source.toCharArray(dest, source.length() + 1);  // Include null terminator in the copy

  return true;
} // safeCopyStringToChar



bool arrayContains(const JsonArray& arr, const JsonVariant& value) {
  /*
  The contains function checks whether a specific value is contained in a JSON array (arr). 
  It runs through the array and compares each element with the value searched for. 
  If the value is found, the function returns true. If the value is not found, false is returned.
  */
  for (const auto& v : arr) {
    if (v == value) {
      return true;
    }
  }
  return false;
} // arrayContains


// IRAM_ATTR: On ESP32, ISR code must reside in Internal RAM (IRAM), not in Flash.
// During Flash operations (e.g., Preferences writes), Flash is temporarily unavailable.
// Without IRAM_ATTR, an interrupt during a Flash operation would try to execute code
// from unavailable Flash memory, causing a "Guru Meditation Error" (crash).
void IRAM_ATTR ISRreceiveData0() {
  /*
  ISR for Data0 (bit '0') in the Wiegand protocol.
  Called on FALLING edge of the Data0 pin. Collects up to 26 bits in dongleValue.
  */
  if (bitCount < 26) {
    dongleValue <<= 1;  // Shift the earlier bits left (new bit is implicitly 0)
    bitCount++;
    lastBitTime = millis();  // millis() is ISR-safe on ESP32 (reads hardware timer)
  }
} //ISRreceiveData0



void IRAM_ATTR ISRreceiveData1() {
  /*
  ISR for Data1 (bit '1') in the Wiegand protocol.
  Called on FALLING edge of the Data1 pin. Sets the LSB to represent the received '1' bit.
  */
  if (bitCount < 26) {
    dongleValue <<= 1;  // Shift the earlier bits
    dongleValue |= 1;   // Set LSB to '1'
    bitCount++;
    lastBitTime = millis();  // millis() is ISR-safe on ESP32 (reads hardware timer)
  }
} //ISRreceiveData1




void fetchAndStoreDongleIds() {
  /*
  This function reads all authorized dongles from a Google Sheet via a web application and compares 
  them with the dongle IDs previously stored in the persistent storage.
  If the retrieval of data from Google Sheets is successful and differences are detected, 
  the dongle IDs in the persistent storage are deleted and updated. 
  This ensures that the lock continues to work with the last known dongle IDs even in the event of an internet failure.
  */
  debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Beginn Methode fetchAndStoreDongleIds()");

  preferencesDongles.begin("dongleStore", false);  // ReadOnly=false for read and write access to the persistent memory for dongle ids

  // Step 1 - read the save Dongle Ids from persistent memory as JSON-String-Array
  String persOfflineDonglesJson = preferencesDongles.getString(PERS_MEM_DONGLE_IDS, "[]");  // read stored dongleIds in string-like array ([key], [default])
  JsonDocument persOfflineDonglesDoc;                                               // JSON Doc for handling DongleIds in string like array
  DeserializationError error = deserializeJson(persOfflineDonglesDoc, persOfflineDonglesJson);     // deserialize (parse persOfflineDonglesJson to fill persOfflineDonglesDoc with its content)
  if (error) {
    debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Failed to deserialize persisted dongles: ", error.f_str());
    // Handle error: you might choose to clear the document or return
    persOfflineDonglesDoc.clear();
  }  
  JsonArray persOfflineDonglesArr = persOfflineDonglesDoc.as<JsonArray>();          // convert to "linked" array

  debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Dongles Read from PersMem to JSON");
  // Step 2 - get authorised DongleIds from WebApp as JSON-String-Array
  HTTPClient http;
  http.setTimeout(20000);                 // Set timeout to 20 seconds
  //http.setReuse(true);                  // activate reuse of the connection
  http.begin(WEB_APP_URL_READ);           // starting connection to webApp
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int httpCode = http.GET();

  // Check Status, if not 200 (OK) then use dongleIds from persistent memory and close all connections
  if (httpCode != 200) {  
    //String myDebugMsg = "http is " + String(httpCode);
    debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "http is ", httpCode);
    debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "HTTP Code: " , httpCode , " - " , http.errorToString(httpCode));
    if (ramDonglesArr.size() == 0) {                                                // restart without internetconnection -> use dongles from persistent memory to fill ram
      if (xSemaphoreTake(mutexDongleList, pdMS_TO_TICKS(5000)) == pdTRUE) {  // Mutex anfordern
        for (JsonVariant v : persOfflineDonglesArr) {
          ramDonglesArr.add(v);
        }
        xSemaphoreGive(mutexDongleList);
      }
    }
    preferencesDongles.end();   // will set the persistent Memory for Dongles to Read_Only again
    http.end();
    buzzerSounds->playSound(BuzzerSoundsRgBase::SoundType::SOS); // http error
    return;
  }

  debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Start Deserialize Donges from gSheets");
  String payload = http.getString();                              // get Answer as String
  JsonDocument onlineDonglesDoc;                                  // JSON Doc for handling DongleIds in string like array
  error = deserializeJson(onlineDonglesDoc, payload);             // deserialize (parse payload to fill onlineDonglesDoc with its content)
  if (error) {
    debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Failed to deserialize online dongles: ", error.f_str());
    // Handle error
    onlineDonglesDoc.clear();   // ToDo - this seems incomplete
  }  
  JsonArray onlineDonglesArr = onlineDonglesDoc.as<JsonArray>();  // convert to "linked" array

  if (DebugFlags::FETCH_AND_STORE_DONGLE_IDS_DETAIL) {
      // print all dongle ids read from gsheets
    for (JsonVariant v : onlineDonglesArr) {
      debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS_DETAIL, v.as<String>());
    }
  }
  debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "done");
  
  // check for differences in between online & offline stored dongles
  bool isDifferent = false;
  if (persOfflineDonglesArr.size() != onlineDonglesArr.size()) {
    isDifferent = true; // can´t be equal if size is different
    debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Diff in Size ", persOfflineDonglesArr.size(), " vs ",  onlineDonglesArr.size() );
  } else {
    for (JsonVariant v : onlineDonglesArr) {
       vTaskDelay(1);  // Give control back to the system to prevent watchdog resets 
      if (!arrayContains(persOfflineDonglesArr, v)) {
        debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Diff: ", v.as<String>(), " not in OfflineDongles PersMem");
        isDifferent = true;
        break;
      }
    }
  }

  // if there were differences we need to update the persistent memory and the ram
  if (isDifferent) {
    debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "clear Mem");
    preferencesDongles.clear();  // clear persistent memory for stored dongles
    debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Mem Cleared, putString");
    preferencesDongles.putString(PERS_MEM_DONGLE_IDS, payload);  // just safe the payload which are the online saved dongleIDs in JSON-string-format
    debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Payload as JSON Saved - ask Mutex");
    if (xSemaphoreTake(mutexDongleList, portMAX_DELAY) == pdTRUE) {  // Mutex anfordern
      debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "got Mutex PersDongleStore");
      ramDonglesArr.clear();
      for (JsonVariant v : onlineDonglesArr) {
        ramDonglesArr.add(v);
      }
      xSemaphoreGive(mutexDongleList);
      debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Mutex PersDongleStore free");
      buzzerSounds->playSound(BuzzerSoundsRgBase::SoundType::OK); // Update Dongles Done, we are ready
    }
  } else {
    debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "read dongleIds from gsheet are the same as in pers. Memory, write PersMem to Ram");
    if (ramDonglesArr.size() == 0) {            
      if (xSemaphoreTake(mutexDongleList, pdMS_TO_TICKS(5000)) == pdTRUE) {   // restart with internetconnection but also latest dongleIds already in persistent memory, just fill the ram
        debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Got Mutex");        
        for (JsonVariant v : persOfflineDonglesArr) {
          ramDonglesArr.add(v);
        }
        xSemaphoreGive(mutexDongleList);
        debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Gave Mutex Back");      
      } else {
        debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "didn´t wait forever");
      }
    }
  }

  debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "http End");
  http.end();                // Close Connection
  preferencesDongles.end();  // will set the persistent Memory for Dongles to Read_Only again
  debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "End Method fetchAndStoreDongleIds()");
} //fetchAndStoreDongleIds




void trackDoorStateChange(){
  // Log door state changes (open/closed) to track physical access
  LogEntryStruct logEntry;

  // Read pin once to avoid inconsistency between comparison and assignment
  int currentDoorState = digitalRead(DOOR_STATE_PIN);

  if (DoorStateMemory != currentDoorState){
    DoorStateMemory = currentDoorState;
    // debouncing probably not necessary, if it is, insert here
    if (DoorStateMemory == DOOR_IS_CLOSED){
      debugService->SerialPrintln_ifDebug(DebugFlags::DOOR_STATE, "PostLogToQueue(door_is_closed, doorstate)");
      getCurrentDateTime(logEntry.date, logEntry.time);

      safeCopyStringToChar("door_is_closed", logEntry.access, CharArraySizes::CharArrayAccessSize);
      safeCopyStringToChar("doorstate", logEntry.dongle_id, CharArraySizes::CharArrayDongleIdSize);

      PostLog(logEntry);

    } else if (DoorStateMemory == DOOR_IS_OPEN) {
      debugService->SerialPrintln_ifDebug(DebugFlags::DOOR_STATE, "PostLogToQueue(door_is_open, doorstate)");
      getCurrentDateTime(logEntry.date, logEntry.time);

      safeCopyStringToChar("door_is_open", logEntry.access, CharArraySizes::CharArrayAccessSize);
      safeCopyStringToChar("doorstate", logEntry.dongle_id, CharArraySizes::CharArrayDongleIdSize);

      PostLog(logEntry);
    } else {
      // nothing here, shouldn't happen
    }
  }
} // trackDoorStateChange



void PostLog(LogEntryStruct &logEntry) {
  // first, send old stored log entries if there are some, then send the new log entry, 
  // if somethng fails store the logentry to the persistant memory

  // check if there is unsent log entries in memory and send them, use JSON for handling keys
  if (sendStoredLogEntries()){
    // all old saved logs are sent, so send the new one too
    if (sendLogEntryViaHttp(logEntry)){
      // return; // Sending Successfull, nothing to do here
    } else {
      saveFailedLogEntry(logEntry);
    }
  } else {
    saveFailedLogEntry(logEntry);
  }

}  // PostLog



String urlEncode(const String& str) {
  // URL-encode a string for safe use in HTTP query parameters (RFC 3986).
  // Unreserved characters (A-Z, a-z, 0-9, '-', '_', '.', '~') pass through;
  // all others are percent-encoded (e.g., ':' becomes '%3A').
  String encoded;
  encoded.reserve(str.length() + 8);  // Pre-allocate to reduce heap fragmentation
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
} // urlEncode



bool sendLogEntryViaHttp(LogEntryStruct &logEntry) {
  // Send a log entry via HTTP GET to the Google Apps Script web app
  HTTPClient http;
  String webappurl_write = String(WEB_APP_URL) + "?action=write_log_pa";
  webappurl_write += "&date=" + urlEncode(String(logEntry.date));
  webappurl_write += "&time=" + urlEncode(String(logEntry.time));
  webappurl_write += "&access=" + urlEncode(String(logEntry.access));
  webappurl_write += "&dongle_id=" + urlEncode(String(logEntry.dongle_id));

  http.begin(webappurl_write);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int httpCode = http.GET();
  http.end();

  return httpCode == 200;
} //sendLogEntryViaHttp





bool sendStoredLogEntries() {
  debugService->SerialPrintln_ifDebug(DebugFlags::SEND_STORED_DONLGE_LOG_ENTRIES, "Start Method sendStoredLogEntries()");
  // check if there is unsent log entries in memory and send them, use JSON for handling keys
  LogEntryStruct logEntry;
  // preferences object for log
  preferencesLog.begin(PERS_MEM_FAILED_LOGS, false); // ReadOnly=false for read and write access to the persistent memory for failed_logs
  
  JsonDocument doc;   // JSON Doc for handling keys in string like array
  String keyArrayStr = preferencesLog.getString("keyArray", "[]");  // read stored logentries-keys in string-like array ([key], [default])
  DeserializationError error = deserializeJson(doc, keyArrayStr);  // deserialize (parse keyArrayStr to fill doc with its content)
  if (error) {
    debugService->SerialPrintln_ifDebug(DebugFlags::SEND_STORED_DONLGE_LOG_ENTRIES, "Failed to deserialize keyArray: ", error.f_str());
    // Handle error, possibly clear the stored logs
    preferencesLog.clear();
    preferencesLog.end();
    return true; // Return true to prevent further attempts on corrupted data
  }  
  JsonArray keyArray = doc.as<JsonArray>();  // convert to "linked" array

  if (keyArray.size() == 0) {
    preferencesLog.end();
    return true; // leave function with true for success, because there is nothing left in Memory to log
  }

  int originalKeyCount = keyArray.size();  // Store count before sending, to detect partial success

  // loop over all keys for access to stored logs, processing is sequential and not based on key name
  int i = 0;
  while (i < keyArray.size()) {
    String key = keyArray[i];
    String logEntryStringCommaSeparated = preferencesLog.getString(key.c_str(), "");

  	// split comma separated fields 
    if (logEntryStringCommaSeparated != "") {
      int firstComma = logEntryStringCommaSeparated.indexOf(',');
      int secondComma = logEntryStringCommaSeparated.indexOf(',', firstComma + 1);
      int thirdComma = logEntryStringCommaSeparated.indexOf(',', secondComma + 1);

      String date = logEntryStringCommaSeparated.substring(0, firstComma);
      String time = logEntryStringCommaSeparated.substring(firstComma + 1, secondComma);
      String access = logEntryStringCommaSeparated.substring(secondComma + 1, thirdComma);
      String dongleId = logEntryStringCommaSeparated.substring(thirdComma + 1);

      safeCopyStringToChar(date, logEntry.date, CharArraySizes::CharArrayDateSize);
      safeCopyStringToChar(time, logEntry.time, CharArraySizes::CharArrayTimeSize);
      safeCopyStringToChar(access, logEntry.access, CharArraySizes::CharArrayAccessSize);
      safeCopyStringToChar(dongleId, logEntry.dongle_id, CharArraySizes::CharArrayDongleIdSize);

      // try sending the logentry
      if (sendLogEntryViaHttp(logEntry)) { 
          preferencesLog.remove(key.c_str()); // delete the successfull sent logentry from persistent memory
          keyArray.remove(i); // delete the key for just sent and deleted entry
          // no need to change i because key array was deleted in first position(0) and got shift left
      } else {
          break;  // leave while
      }
    } else {
      // Empty log entry found in persistent memory - skip to next key to prevent infinite loop.
      // This can happen if a key exists in keyArray but its value was already removed
      // or was never written correctly.
      i++;
    }
  }

  if (keyArray.size() == 0){
    preferencesLog.remove("keyArray"); // if the array is empty, delete all keys in persistent memory
    preferencesLog.end();
    return true; // leave function with true for success
  } else if (keyArray.size() != originalKeyCount) {
    // Some entries were sent successfully but not all. Update the persisted keyArray.
    // serialize (convert content of doc into a JSON string and store it in result) - doc is linked to keyArray and therefore always uptodate
    serializeJson(doc, keyArrayStr);  
    preferencesLog.putString("keyArray", keyArrayStr.c_str());  // save changed keys as string in persistent memory 
    preferencesLog.end();
    return false; // leave function with false --> means that there are still old logs in memory
  } else {  // the KeyArray is the same as before
    preferencesLog.end();
    return false; // leave function with false --> means that there are still old logs in memory
  }

} // sendStoredLogEntries




void saveFailedLogEntry(LogEntryStruct &logEntry) {
  // save log entries that could not be sent successfully via http for later attempts

  preferencesLog.begin(PERS_MEM_FAILED_LOGS, false); // ReadOnly=false for read and write access to the persistent memory for logs

  JsonDocument doc;   // JSON Doc for handling keys in string like array
  String keyArrayStr = preferencesLog.getString("keyArray", "[]");  // read stored logentries-keys in string-like array ([key], [default])
  DeserializationError error = deserializeJson(doc, keyArrayStr);
  if (error) {
    // Corrupted data in persistent memory - start fresh with empty key array.
    // Existing log entries under their individual keys remain in storage
    // but won't be retried until a new keyArray references them.
    doc.clear();
    deserializeJson(doc, "[]");
  }
  JsonArray keyArray = doc.as<JsonArray>();  // convert to array

  // generate a new unique key for the log entry
  String newKey;
  int keyIndex = keyArray.size(); // start-value for key
  bool noUniqueKeyFound = true;
  while (noUniqueKeyFound){

    noUniqueKeyFound = false; // init bool
    keyIndex++; 
    newKey = "log" + String(keyIndex);

    int i = 0;
    while (i < keyArray.size()) {
      String key = keyArray[i];
      if (key == newKey){
        noUniqueKeyFound = true;
        break; // leave inner while, because there is a dupRec
      }
      i++; 
    }
  }

  String logEntryStringCommaSeparated = String(logEntry.date) + "," + String(logEntry.time) + "," + String(logEntry.access) + "," + String(logEntry.dongle_id);
  preferencesLog.putString(newKey.c_str(), logEntryStringCommaSeparated); // put that log with the new unique key to persistent memory
  
  keyArray.add(newKey);  // add key to keyArray
  
  // update the KeyArray
  // serialize (convert content of doc into a JSON string and store it in result) - doc is linked to keyArray and therefore always uptodate
  serializeJson(doc, keyArrayStr);  
  preferencesLog.putString("keyArray", keyArrayStr.c_str());  // save changed keys as string in persistent memory 
  preferencesLog.end();
} // saveFailedLogEntry




void handleRFIDScanResult(){
  LogEntryStruct logEntry;

  // critical section: disabling interrupts to save current content
  noInterrupts();
  unsigned long readDongleValue = dongleValue;
  int countedBits = bitCount;
  interrupts(); // reactivate interrupts

  // process the data when all bits have been received
  if (countedBits == 26) {
    String dongleIdStr = ""; // initialize an empty string for the dongle ID
    
    for (int i = 25; i >= 0; i--) {
      bool bit = (readDongleValue >> i) & 1; // extract every bit
      dongleIdStr += String(bit); // add the bit to the dongleIdStr
    }

    debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_SCAN, "scanned Dongle: " , dongleIdStr);
    if (isDongleIdAuthorized(dongleIdStr)) { 
      debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_SCAN, "PlaySound AuthOk");
      buzzerSounds->playSound(BuzzerSoundsRgBase::SoundType::AuthOk);
      unlock();
      debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_SCAN, "PostLogToQueue(authorised," , dongleIdStr , ")");
      getCurrentDateTime(logEntry.date, logEntry.time);

      safeCopyStringToChar("authorised", logEntry.access, CharArraySizes::CharArrayAccessSize);
      safeCopyStringToChar(dongleIdStr, logEntry.dongle_id, CharArraySizes::CharArrayDongleIdSize);

      PostLog(logEntry);

    } else {
      debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_SCAN, "PlaySound NoAuth)");
      buzzerSounds->playSound(BuzzerSoundsRgBase::SoundType::NoAuth);
      debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_SCAN, "PostLogToQueue(denied," , dongleIdStr , ")");
      getCurrentDateTime(logEntry.date, logEntry.time);

      safeCopyStringToChar("denied", logEntry.access, CharArraySizes::CharArrayAccessSize);
      safeCopyStringToChar(dongleIdStr, logEntry.dongle_id, CharArraySizes::CharArrayDongleIdSize);

      PostLog(logEntry);
    }
    
    // critical section: disabling interrupts to init their content
    noInterrupts();
    bitCount = 0;
    dongleValue = 0;
    interrupts(); // reactivate interrupts
  }
} // handleRFIDScanResult()


bool isDongleIdAuthorized(String dongleIdStr) {
  /*
  Checks whether the scanned DongleId is authorized.
  Authorization is checked against the dongle list in RAM, which is synced
  from Google Sheets at startup, periodically, and on MasterCard scan.
  Special cases:
  - MasterCard: triggers a DB refresh without opening the door
  - OPEN_FOR_ALL_DONGLES: if this value is in the list, all dongles are authorized
  */
  debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_AUTH, "Start Function isDongleAuthorized");

  // MasterCard check first: triggers DB refresh without granting access.
  // No mutex needed since this doesn't read the dongle list.
  if (dongleIdStr.equals(DONGLE_MASTER_CARD_UPDATE_DB)) {
    debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_AUTH, "MasterCard scanned - refreshing dongle DB");
    fetchAndStoreDongleIds();
    return false;
  }

  if (xSemaphoreTake(mutexDongleList, pdMS_TO_TICKS(5000)) == pdTRUE) {
    for (JsonVariant v : ramDonglesArr) {
        debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_AUTH, "CompareScan ", dongleIdStr);
        debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_AUTH, "CompareMem  ", v.as<String>());

        // Special value: if the list contains OPEN_FOR_ALL_DONGLES, grant access to everyone
        if (v.as<String>() == OPEN_FOR_ALL_DONGLES) {
            xSemaphoreGive(mutexDongleList);
            return true;
        }
        // Match scanned dongle against authorized list
        if (dongleIdStr.equals(v.as<String>())) {
            xSemaphoreGive(mutexDongleList);
            return true;
        }
    }
    xSemaphoreGive(mutexDongleList);
    return false;
  }
  debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_AUTH, "Failed to acquire mutex - returning unauthorized");
  return false;
} // isDongleIdAuthorized


void unlock() {
  // unlock with a short signal
  digitalWrite(UNLOCKPIN, HIGH);
  delay(SWITCHDURATION_ms);
  digitalWrite(UNLOCKPIN, LOW); 
} // unlock()

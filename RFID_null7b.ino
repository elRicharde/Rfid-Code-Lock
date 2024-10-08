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

constexpr long GMT_OFFSET_SEC = 3600;
constexpr int DAYLIGHT_OFFSET_SEC = 3600;
constexpr const char TIME_SERVER_1[16] = "de.pool.ntp.org";
constexpr const char TIME_SERVER_2[13] = "pool.ntp.org";
constexpr const char TIME_SERVER_3[14] = "time.nist.gov";
                                      
constexpr const char PERS_MEM_DONGLE_IDS[10] = "DonlgeIds";
constexpr const char PERS_MEM_FAILED_LOGS[12] = "Failed_Logs";

constexpr int BUZZERPIN = 4;
constexpr int UNLOCKPIN = 2;
constexpr int DOOR_STATE_PIN = 12;
constexpr int INTERRUPT_IO_PIN_1 = 10;
constexpr int INTERRUPT_IO_PIN_2 = 8;
constexpr int SWITCHDURATION_ms = 250;
constexpr int DOOR_IS_CLOSED = 0;
constexpr int DOOR_IS_OPEN = 1;



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
int DoorStateMemory = 2;                 // DoorStateMemory Initial = 2, in use can be only 1 or 0


SemaphoreHandle_t mutexPersistentDongleStorage; // Mutex für den Zugriff auf den persistenten Speicher
Preferences preferencesDongles;  // for access to persistent memory of the ESP32 - mem area for dongles
Preferences preferencesLog;  // for access to persistent memory of the ESP32 - mem area for dongles
JsonDocument ramDonglesDoc;  // JSON Doc for handling DongleIds in string like array as global var in ram
JsonArray ramDonglesArr = ramDonglesDoc.as<JsonArray>(); // convert to "linked" array



// =============================================================================================================================
// Debugging Control ===========================================================================================================
DebugService* debugService;                                 
struct DebugFlags {                                         
  static constexpr bool DEBUG_MODE = true;    // Muss immer true sein, wenn mindestens ein anderes Flag true ist        
  static constexpr bool WIFI_LOGGING = true;                
  static constexpr bool FETCH_AND_STORE_DONGLE_IDS = true;
  static constexpr bool FETCH_AND_STORE_DONGLE_IDS_DETAIL = true;
  static constexpr bool DOOR_STATE = true;
  static constexpr bool DONGLE_SCAN = true;
  static constexpr bool DONGLE_AUTH = true;
};                                                          
// extern defefinition needed for linker, obsolete from c++17 with inline in struct
constexpr bool DebugFlags::DEBUG_MODE;    
constexpr bool DebugFlags::WIFI_LOGGING;  
constexpr bool DebugFlags::FETCH_AND_STORE_DONGLE_IDS;
constexpr bool DebugFlags::FETCH_AND_STORE_DONGLE_IDS_DETAIL;
constexpr bool DebugFlags::DOOR_STATE;
constexpr bool DebugFlags::DONGLE_SCAN;
constexpr bool DebugFlags::DONGLE_AUTH;
// Debugging Control ===========================================================================================================
// =============================================================================================================================





// Function Defenitions ========================================================================================================
// =============================================================================================================================

// This function formats the current date and time
void getCurrentDateTime(char formattedDate[11], char formattedTime[9]);

bool safeCopyStringToChar(const String& source, char* dest, size_t destSize);

// check if Value is included in JsonArray
bool arrayContains(const JsonArray& arr, const JsonVariant& value);

// ISR (interrupt Service Routine) for Data0 (represents bit '0') in the Wiegand protocol
void ISRreceiveData0();

// ISR (Interrupt Service Routine) for Data1 (represents bit '1') in the Wiegand protocol.
void ISRreceiveData1();

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
  if (DebugFlags::DEBUG_MODE) {
    Serial.begin(115200);
    debugService = DebugService::getInstance();
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


  // Init and get the time
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, TIME_SERVER_1, TIME_SERVER_2, TIME_SERVER_3);

  // pinMode(Buzzer, OUTPUT);    // Pin for Buzzer
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

  mutexPersistentDongleStorage = xSemaphoreCreateMutex(); // Initialisiing Mutex here
  fetchAndStoreDongleIds();
} //setup() 


// Loop =======================================================================================================================
//=============================================================================================================================
void loop() {
  // put your main code here, to run repeatedly:
  trackDoorStateChange();
  handleRFIDScanResult();

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


void ISRreceiveData0() {
  /*
  ISR (Interrupt Service Routine) for Data0 (represents bit '0') in the Wiegand protocol.
  This routine is called when a FALLING interrupt occurs on the Data0 pin,
  which corresponds to a '0' bit in the Wiegand protocol. The ISR collects the 
  received binary bits (0 and 1) in the global variable dongleValue. A maximum of 26 bits are 
  collected, corresponding to the usual format of Wiegand data.
  */
  if (bitCount < 26) {
    dongleValue <<= 1;  // Shift the earlier bits left
    bitCount++;
  }
} //ISRreceiveData0



void ISRreceiveData1() {
  /*
  ISR (Interrupt Service Routine) for Data1 (represents bit '1') in the Wiegand protocol.
  Similar to ISRreceiveData0 but for the '1' bit. This routine is called
  when a FALLING interrupt occurs on the Data1 pin, which corresponds to a '1' bit in 
  the Wiegand protocol. In addition to shifting the bits in dongleValue, the LSB (Least Significant Bit) 
  of donngleValue is set to '1' to represent the received '1' bit.
  */
  if (bitCount < 26) {
    dongleValue <<= 1;  // Shift the earlier bits
    dongleValue |= 1;   // Add '1' to the LSB of dongleValue
    bitCount++;
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
  deserializeJson(persOfflineDonglesDoc, persOfflineDonglesJson);                   // deserialize (parse persOfflineDonglesJson to fill persOfflineDonglesDoc with its content)
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
    debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "http is " + httpCode);
    debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "HTTP Code: " , httpCode , " - " , http.errorToString(httpCode));
    if (ramDonglesArr.size() == 0) {                                                // restart without internetconnection -> use dongles from persistent memory to fill ram
      if (xSemaphoreTake(mutexPersistentDongleStorage, pdMS_TO_TICKS(5000)) == pdTRUE) {  // Mutex anfordern
        for (JsonVariant v : persOfflineDonglesArr) {
          ramDonglesArr.add(v);
        }
        xSemaphoreGive(mutexPersistentDongleStorage);
      }
    }
    preferencesDongles.end();   // will set the persistent Memory for Dongles to Read_Only again
    http.end();
    return;
  }

  debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Start Deserialize Donges from gSheets");
  String payload = http.getString();                              // get Answer as String
  JsonDocument onlineDonglesDoc;                                  // JSON Doc for handling DongleIds in string like array
  deserializeJson(onlineDonglesDoc, payload);                     // deserialize (parse payload to fill onlineDonglesDoc with its content)
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
    if (xSemaphoreTake(mutexPersistentDongleStorage, portMAX_DELAY) == pdTRUE) {  // Mutex anfordern
      debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "got Mutex PersDongleStore");
      ramDonglesArr.clear();
      for (JsonVariant v : onlineDonglesArr) {
        ramDonglesArr.add(v);
      }
      xSemaphoreGive(mutexPersistentDongleStorage);
      debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Mutex PersDongleStore free");
    }
  } else {
    debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "read dongleIds from gsheet are the same as in pers. Memory, write PersMem to Ram");
    if (ramDonglesArr.size() == 0) {            
      if (xSemaphoreTake(mutexPersistentDongleStorage, pdMS_TO_TICKS(5000)) == pdTRUE) {   // restart with internetconnection but also latest dongleIds already in persistent memory, just fill the ram
        debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "Got Mutex");        
        for (JsonVariant v : persOfflineDonglesArr) {
          ramDonglesArr.add(v);
        }
        xSemaphoreGive(mutexPersistentDongleStorage);
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
  // log doorstate when changed
  LogEntryStruct logEntry;
  String dateString, timeString;


  if (DoorStateMemory != digitalRead(DOOR_STATE_PIN)){
    DoorStateMemory = digitalRead(DOOR_STATE_PIN);  
    // debouncing probably not necessary, if it is, insert here
    if (DoorStateMemory == DOOR_IS_CLOSED){
      debugService->SerialPrintln_ifDebug(DebugFlags::DOOR_STATE, "PostLogToQueue(door_is_closed, doorstate)");
      getCurrentDateTime(logEntry.date, logEntry.time);

      safeCopyStringToChar("door_is_closed", logEntry.access, 15);
      safeCopyStringToChar("doorstate", logEntry.dongle_id, 10);

      //PostLogToQueue("door_is_closed", "doorstate"); //ERSETZEN!!!! TODO
    } else if (DoorStateMemory == DOOR_IS_OPEN) {
      debugService->SerialPrintln_ifDebug(DebugFlags::DOOR_STATE, "PostLogToQueue(door_is_open, doorstate)");
      getCurrentDateTime(logEntry.date, logEntry.time);

      safeCopyStringToChar("door_is_open", logEntry.access, 13);
      safeCopyStringToChar("doorstate", logEntry.dongle_id, 10);

      //PostLogToQueue("door_is_open", "doorstate");     // ERSETZEN!!!! TODO
    } else {
      // nothing here, shouldn´t happen
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
      return; // Sending Successfull, nothing to do here
    } else {
      saveFailedLogEntry(logEntry);
    }
  } else {
    saveFailedLogEntry(logEntry);
  }
}  // PostLog



bool sendLogEntryViaHttp(LogEntryStruct &logEntry) {
  // send a log entry thru http via webApp into googleSheet
  HTTPClient http;
  String webappurl_write = String(WEB_APP_URL) + "?action=write_log_pa";
  webappurl_write += "&date=" + String(logEntry.date) + "&time=" + String(logEntry.time) + "&access=" + String(logEntry.access) + "&dongle_id=" + String(logEntry.dongle_id);

  http.begin(webappurl_write);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int httpCode = http.GET();
  http.end();

  return httpCode == 200; // wenn der httpCode 200 ist, gibt die Funktion True zurück
} //sendLogEntryViaHttp





bool sendStoredLogEntries() {
  // check if there is unsent log entries in memory and send them, use JSON for handling keys
  LogEntryStruct logEntry;
  // preferences object for log
  preferencesLog.begin(PERS_MEM_FAILED_LOGS, false); // ReadOnly=false for read and write access to the persistent memory for failed_logs
  
  JsonDocument doc;   // JSON Doc for handling keys in string like array
  String keyArrayStr = preferencesLog.getString("keyArray", "[]");  // read stored logentries-keys in string-like array ([key], [default])
  deserializeJson(doc, keyArrayStr);  // deserialize (parse keyArrayStr to fill doc with its content)
  JsonArray keyArray = doc.as<JsonArray>();  // convert to "linked" array

  if (keyArray.size() == 0) {
    preferencesLog.end();
    return true; // leave function with true for success, because there is nothing left in Memory to log
  }

  JsonArray keyArrayCopy = keyArray;  // save keyArray for checking if it has changed

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
    }
  }

  if (keyArray.size() == 0){
    preferencesLog.remove("keyArray"); // if the array is empty, delete all keys in persistent memory
    preferencesLog.end();
    return true; // leave function with true for success
  } else if (keyArray.size() != keyArrayCopy.size()){
    // update the KeyArray if it still contains keys but is different to the initial read KeyArray, compare of JSON-Array not directly possible, therefore compare size
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
  deserializeJson(doc, keyArrayStr);  // deserialize (parse keyArrayStr to fill doc with its content)
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
      // addSoundToQueue(BuzzerSoundsRgBase::SoundType::AuthOk); TODO
      unlock();
      debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_SCAN, "PostLogToQueue(authorised," , dongleIdStr , ")");
      getCurrentDateTime(logEntry.date, logEntry.time);

      safeCopyStringToChar("authorised", logEntry.access, 11);
      safeCopyStringToChar(dongleIdStr, logEntry.dongle_id, CharArraySizes::CharArrayDongleIdSize);

      PostLog(logEntry);
    } else {
      debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_SCAN, "PlaySound NoAuth)");
      // addSoundToQueue(BuzzerSoundsRgBase::SoundType::NoAuth);  TODO
      debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_SCAN, "PostLogToQueue(denied," , dongleIdStr , ")");
      getCurrentDateTime(logEntry.date, logEntry.time);

      safeCopyStringToChar("denied", logEntry.access, 7);
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
  Function checks whether the read DongleId is authorized.
  The check is performed on the dongles in the volatile memory,
  this is filled with every restart and from the dongles in the Google Sheet
  or the persistent memory and updated when changes are made
  */
  debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_AUTH, "Start Function isDongleAuthorized");
  if (xSemaphoreTake(mutexPersistentDongleStorage, pdMS_TO_TICKS(5000)) == pdTRUE) {  // Mutex anfordern
    for (JsonVariant v : ramDonglesArr) {
        debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_AUTH, "CompareScan ", dongleIdStr);
        debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_AUTH, "CompareMem  ", v.as<String>());
        if (dongleIdStr.equals(v.as<String>())) {
            xSemaphoreGive(mutexPersistentDongleStorage);
            return true;
        }
    }
    xSemaphoreGive(mutexPersistentDongleStorage);
    return false;
  }
  debugService->SerialPrintln_ifDebug(DebugFlags::DONGLE_AUTH, "didn´t get Mutex - return False");
  return false; // will only be reached if there is no mutex in portMAX_DELAY
} // isDongleIdAuthorized


void unlock() {
  // unlock with a short signal
  digitalWrite(UNLOCKPIN, HIGH);
  delay(SWITCHDURATION_ms);
  digitalWrite(UNLOCKPIN, LOW); 
} // unlock()

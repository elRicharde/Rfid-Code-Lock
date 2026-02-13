#ifndef CONFIG_H
#define CONFIG_H

// =============================================================
// Debug Mode: Uncomment the following line to enable debug output.
// When commented out, ALL debug code is eliminated from the binary (zero overhead).
// =============================================================
// #define DEBUG_MODE

#include <Arduino.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// =============================================================
// Pin Definitions
// =============================================================
constexpr int BUZZERPIN = 4;
constexpr int UNLOCKPIN = 2;
constexpr int DOOR_STATE_PIN = 12;
constexpr int INTERRUPT_IO_PIN_1 = 10;
constexpr int INTERRUPT_IO_PIN_2 = 8;

// =============================================================
// Timing Constants
// =============================================================
constexpr int SWITCHDURATION_MS = 250;                 // Relay pulse duration for unlock
constexpr int WIEGAND_TIMEOUT_MS = 200;                // Reset partial RFID reads (see loop() for rationale)
constexpr float DONGLE_REFRESH_INTERVAL_HOURS = 4.0;   // Periodic dongle DB refresh (0.01 for testing, 0.5-72.0 production)
constexpr unsigned long DONGLE_REFRESH_INTERVAL_MS = (unsigned long)(DONGLE_REFRESH_INTERVAL_HOURS * 3600.0f * 1000.0f);
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 30000;   // WiFi reconnect check every 30s
constexpr unsigned long DONGLE_REFRESH_DEBOUNCE_MS = 30000;   // MasterCard refresh cooldown (30s)
constexpr unsigned long LOG_RETRY_BACKOFF_MS = 60000;          // Wait 60s between retry attempts for failed logs

// =============================================================
// NTP Configuration
// =============================================================
constexpr long GMT_OFFSET_SEC = 3600;
constexpr int DAYLIGHT_OFFSET_SEC = 3600;
constexpr const char TIME_SERVER_1[] = "de.pool.ntp.org";
constexpr const char TIME_SERVER_2[] = "pool.ntp.org";
constexpr const char TIME_SERVER_3[] = "time.nist.gov";

// =============================================================
// Persistent Memory Keys
// =============================================================
constexpr const char PERS_MEM_DONGLE_IDS[] = "DongleIds";
constexpr const char PERS_MEM_FAILED_LOGS[] = "Failed_Logs";

// =============================================================
// Door State Constants
// =============================================================
constexpr int DOOR_IS_CLOSED = 0;
constexpr int DOOR_IS_OPEN = 1;

// =============================================================
// Network Task Configuration
// =============================================================
constexpr int LOG_QUEUE_SIZE = 30;              // Max queued log entries (30 x 62 bytes = ~1.9 KB)
constexpr int NETWORK_TASK_STACK_SIZE = 16384;  // 16 KB — HTTPS with TLS + JSON parsing needs generous stack
constexpr int NETWORK_TASK_PRIORITY = 1;        // Same as default loop task (both pinned to separate cores)
constexpr int MAX_FAILED_LOGS = 50;             // Max stored failed log entries in NVS (prevents partition exhaustion)
constexpr int NETWORK_TASK_CORE = 0;            // Core 0 (Core 1 = main loop + ISRs)
constexpr int NETWORK_TASK_LOOP_DELAY_MS = 100; // Task loop interval

// =============================================================
// Types
// =============================================================

enum CharArraySizes {
  CharArrayDateSize = 11,      // DD.MM.YYYY + null
  CharArrayTimeSize = 9,       // HH:MM:SS + null
  CharArrayAccessSize = 15,    // "door_is_closed" + null
  CharArrayDongleIdSize = 27,  // 26 Wiegand bits + null
};

typedef struct {
  char date[CharArrayDateSize];
  char time[CharArrayTimeSize];
  char access[CharArrayAccessSize];
  char dongle_id[CharArrayDongleIdSize];
} LogEntryStruct;

// Buzzer signals passed from network task to main loop via FreeRTOS queue.
// Network task cannot call buzzer directly (not thread-safe).
enum BuzzerSignal : int8_t {
  BUZZER_NONE = -1,
  BUZZER_SOS = 0,
  BUZZER_OK = 1,
};

// =============================================================
// Shared Inline Utilities
// =============================================================

// Safely copy an Arduino String into a fixed-size char array.
// Returns false if the string is too long or the destination is invalid.
inline bool safeCopyStringToChar(const String& source, char* dest, size_t destSize) {
  if (dest == nullptr || destSize == 0) {
    return false;
  }
  if (source.length() + 1 > destSize) {
    return false;
  }
  memset(dest, 0, destSize);
  source.toCharArray(dest, source.length() + 1);
  return true;
}

// Format the current date and time from NTP into char arrays.
inline void getCurrentDateTime(char formattedDate[CharArrayDateSize], char formattedTime[CharArrayTimeSize]) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    static_assert(sizeof("Date Error") <= CharArrayDateSize, "Error string too long for date buffer");
    static_assert(sizeof("Date Err") <= CharArrayTimeSize, "Error string too long for time buffer");
    safeCopyStringToChar("Date Error", formattedDate, CharArrayDateSize);
    safeCopyStringToChar("Date Err", formattedTime, CharArrayTimeSize);
    return;
  }
  strftime(formattedDate, CharArrayDateSize, "%d.%m.%Y", &timeinfo);
  strftime(formattedTime, CharArrayTimeSize, "%H:%M:%S", &timeinfo);
}

#endif // CONFIG_H

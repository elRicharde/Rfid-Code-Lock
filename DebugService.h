#ifndef DEBUG_SERVICE_H
#define DEBUG_SERVICE_H

#include "Config.h"

#ifdef DEBUG_MODE

// =============================================================
// Per-subsystem debug flags (only exist in debug builds).
// Toggle individual flags to narrow debug output.
// =============================================================
struct DebugFlags {
  static constexpr bool SETUP = true;
  static constexpr bool WIFI_LOGGING = true;
  static constexpr bool FETCH_AND_STORE_DONGLE_IDS = true;
  static constexpr bool FETCH_AND_STORE_DONGLE_IDS_DETAIL = true;
  static constexpr bool DOOR_STATE = true;
  static constexpr bool DONGLE_SCAN = true;
  static constexpr bool DONGLE_AUTH = true;
  static constexpr bool SEND_STORED_LOG_ENTRIES = true;
  static constexpr bool NETWORK_TASK = true;
};

class DebugService {
  private:
    SemaphoreHandle_t _serialPrintMutex = nullptr;

    DebugService();

    // Variadic print helpers (single + recursive)
    template<typename T>
    void printHelper(T arg) {
        Serial.print(arg);
    }
    template<typename T, typename... Args>
    void printHelper(T arg, Args... args) {
        Serial.print(arg);
        printHelper(args...);
    }

  public:
    DebugService(const DebugService&) = delete;
    DebugService& operator=(const DebugService&) = delete;

    static DebugService* getInstance();

    // Thread-safe println with mutex protection.
    // Flag check is done by the DBG() macro before calling this.
    template<typename... Args>
    void println(Args... args) {
      if (xSemaphoreTake(_serialPrintMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          printHelper(args...);
          Serial.println();
          xSemaphoreGive(_serialPrintMutex);
      }
    }
};

// DBG macro: checks the per-subsystem flag, then calls println.
// do-while(0) ensures safe use in if/else without braces.
#define DBG(flag, ...) do { if (flag) DebugService::getInstance()->println(__VA_ARGS__); } while(0)

#else // DEBUG_MODE not defined — production build

// All debug code compiles to nothing. Zero overhead.
#define DBG(flag, ...) ((void)0)

#endif // DEBUG_MODE
#endif // DEBUG_SERVICE_H

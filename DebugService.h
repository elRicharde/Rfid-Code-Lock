#ifndef DEBUG_SERVICE_H
#define DEBUG_SERVICE_H

#include <Arduino.h>
#include "freertos/semphr.h"

class DebugService {
  private:
    SemaphoreHandle_t _serialPrintMutex = nullptr;

    static DebugService* _instance; // static instance for singleton

    // private constructor for singleton
    explicit DebugService();

    // Helper function to recursively print each argument
    // single argument
    template<typename T>
    void SerialPrintHelper(T arg) {
        Serial.print(arg);
    }
    // multi argument
    template<typename T, typename... Args>
    void SerialPrintHelper(T arg, Args... args) {
        Serial.print(arg);
        SerialPrintHelper(args...); // Recursion
    }

  public:
    // prevent copying and referencing in Singleton
    DebugService(const DebugService&) = delete;
    DebugService& operator=(const DebugService&) = delete;

    // static method for getting a singleton instance 
    static DebugService* getInstance();

    // ## Methods ## //
    // Output in debug, as template (therefore in .h file) with 1 to n arguments
    template<typename... Args>
    void SerialPrintln_ifDebug(const bool &Debug, Args... args) {
    if (Debug) {
      if(xSemaphoreTake(_serialPrintMutex, portMAX_DELAY) == pdTRUE) {
          SerialPrintHelper(args...); // Print all arguments
          Serial.println();           // Newline at the end
          xSemaphoreGive(_serialPrintMutex);
      }
    }
  }
};

#endif // DEBUG_SERVICE_H




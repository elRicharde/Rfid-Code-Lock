#include "DebugService.h"

#ifdef DEBUG_MODE

DebugService::DebugService() {
  _serialPrintMutex = xSemaphoreCreateMutex();
}

// Meyers singleton: C++11 guarantees thread-safe initialization of static locals.
// Safe even when first DBG() call happens concurrently on Core 0 and Core 1.
DebugService* DebugService::getInstance() {
  static DebugService instance;
  return &instance;
}

#endif // DEBUG_MODE

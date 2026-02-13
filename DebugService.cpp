#include "DebugService.h"

#ifdef DEBUG_MODE

DebugService* DebugService::_instance = nullptr;

DebugService::DebugService() {
  _serialPrintMutex = xSemaphoreCreateMutex();
}

DebugService* DebugService::getInstance() {
  if (_instance == nullptr) {
    _instance = new DebugService();
  }
  return _instance;
}

#endif // DEBUG_MODE

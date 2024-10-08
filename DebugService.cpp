#include "DebugService.h"

// Class Implementation DebugService
// private
DebugService* DebugService::_instance = nullptr; // initialize the static variable

// implementation of constructor
DebugService::DebugService() {
  _serialPrintMutex = xSemaphoreCreateMutex(); // Initialisiing Mutex here
}

// public
DebugService* DebugService::getInstance() {
  if (_instance == nullptr) {
    _instance = new DebugService();
  }
  return DebugService::_instance;
}

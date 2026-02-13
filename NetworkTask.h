#ifndef NETWORK_TASK_H
#define NETWORK_TASK_H

#include "Config.h"

// =============================================================
// Shared State (defined in NetworkTask.cpp)
// =============================================================

// Mutex protecting ramDonglesArr. Must only be held during the brief
// array swap operation, NEVER during HTTP calls or NVS access.
extern SemaphoreHandle_t mutexDongleList;

extern JsonDocument ramDonglesDoc;
extern JsonArray ramDonglesArr;

// FreeRTOS queues for cross-task communication
extern QueueHandle_t logQueue;          // LogEntryStruct items from main loop -> network task
extern QueueHandle_t buzzerSignalQueue; // BuzzerSignal items from network task -> main loop

// =============================================================
// Public API (called from main loop / setup)
// =============================================================

// Start the network task on Core 0. Call once from setup() after WiFi.begin()
// and loadDonglesFromPersistentMemory().
void startNetworkTask();

// Load dongle IDs from NVS into RAM (fast, no HTTP).
// Called from setup() before startNetworkTask() for immediate RFID availability.
void loadDonglesFromPersistentMemory();

// Queue a log entry for async sending by the network task.
// Non-blocking: returns false if queue is full (entry is dropped, counter incremented).
bool enqueueLogEntry(const LogEntryStruct& entry);

// Signal the network task to refresh dongle IDs from Google Sheets.
// Uses xTaskNotify — safe from any core/context. Debounced (30s cooldown).
void requestDongleRefresh();

// Check if a value exists in a JsonArray.
bool arrayContains(const JsonArray& arr, const JsonVariant& value);

#endif // NETWORK_TASK_H

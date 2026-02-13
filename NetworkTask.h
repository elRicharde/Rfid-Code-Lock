#ifndef NETWORK_TASK_H
#define NETWORK_TASK_H

#include "Config.h"

// =============================================================
// Public API
// All internal state (mutex, dongle list, queues) is encapsulated
// in NetworkTask.cpp — no extern globals exposed.
// =============================================================

// Start the network task on Core 0. Creates mutex, queues, and task internally.
// Call once from setup() after WiFi.begin() and loadDonglesFromPersistentMemory().
void startNetworkTask();

// Load dongle IDs from NVS into RAM (fast, no HTTP).
// Must be called from setup() BEFORE startNetworkTask().
void loadDonglesFromPersistentMemory();

// Queue a log entry for async sending by the network task.
// Non-blocking: returns false if queue is full (entry dropped, counter incremented).
bool enqueueLogEntry(const LogEntryStruct& entry);

// Signal the network task to refresh dongle IDs from Google Sheets.
// Uses xTaskNotify — safe from any core/context. Debounced (30s cooldown).
void requestDongleRefresh();

// Check if a dongle ID is authorized against the RAM dongle list.
// Handles MasterCard (triggers async refresh, returns false) and
// OPEN_FOR_ALL_DONGLES (grants access to all). Thread-safe (mutex protected).
bool isDongleIdAuthorized(const String& dongleId);

// Check if the network task sent a buzzer signal. Non-blocking.
// Returns true if a signal was received, with the signal stored in *outSignal.
bool receiveBuzzerSignal(BuzzerSignal* outSignal);

#endif // NETWORK_TASK_H

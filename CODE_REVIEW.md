# Code Review: RFID-Zugangskontrolle (null7b Technikecke PA)

**Date:** 2026-02-12
**Project:** ESP32-S3 RFID Access Control with Google Sheets Integration
**Review scope:** Full codebase after FreeRTOS refactoring (Backlog items 1, 2, 5)
**Files:** 8 source files, ~700 lines firmware code

---

## Architecture Overview

```
Core 1 (Main Loop)              Core 0 (Network Task)
===================              =====================
ISR: Wiegand bits (IRAM)         WiFi reconnect (30s)
loop(): door monitoring          Periodic dongle refresh (4h)
        RFID scan processing     On-demand refresh (MasterCard)
        buzzer signal check      Log queue processing
        Wiegand timeout          Failed log retry (60s backoff)
        10ms yield               NVS persistence
                                 100ms yield
         ──── Communication ────
         logQueue (30 entries)     Main → Network
         buzzerSignalQueue (1)     Network → Main
         mutexDongleList           Shared dongle array
         xTaskNotify               MasterCard → refresh
```

### File Structure

| File | Purpose | Lines |
|------|---------|-------|
| `Config.h` | Central configuration, types, constants, shared utilities | ~125 |
| `DebugService.h` | Debug singleton, DBG macro, DebugFlags (ifdef guarded) | ~70 |
| `DebugService.cpp` | Meyers singleton implementation (ifdef guarded) | ~18 |
| `NetworkTask.h` | Network task public API, extern declarations | ~45 |
| `NetworkTask.cpp` | HTTP operations, dongle sync, log management, FreeRTOS task | ~310 |
| `RFID_null7b.ino` | Main sketch: setup, loop, ISR, door, RFID, buzzer, unlock | ~210 |
| `Secrets.h` | WiFi credentials, Google Script URLs | ~20 |
| `googleScript` | Google Apps Script web app (read dongles, write logs) | ~80 |
| `Convert_DEC_to_BIN` | Google Sheets custom function (decimal to Wiegand 26-bit) | ~35 |

---

## 1. Thread Safety & Concurrency

### 1.1 Mutex Usage — GOOD

The `mutexDongleList` protects `ramDonglesDoc` / `ramDonglesArr` with a 100ms timeout. The critical section is minimal: only the `std::move` swap and array reassignment occur inside the mutex. JSON parsing and HTTP calls are correctly performed outside the mutex. The `isDongleIdAuthorized()` function uses a single `authorized` flag to avoid early-return mutex leaks.

### 1.2 Queue-Based Communication — GOOD

`logQueue` (depth 30) passes `LogEntryStruct` by value from Core 1 to Core 0. `buzzerSignalQueue` (depth 1) uses `xQueueOverwrite` for latest-wins semantics. Both are created with `configASSERT` guards.

### 1.3 ISR Safety — GOOD

ISR handlers use `IRAM_ATTR`, only access `volatile` variables, and call only `millis()` (ISR-safe on ESP32). The `noInterrupts()` sections correctly snapshot all ISR variables atomically before processing.

### 1.4 Cross-Core Atomic Access — GOOD

`droppedLogCount` uses `std::atomic<int>` for safe cross-core read/write. `xTaskNotify` provides memory barriers for the dongle refresh signal.

### 1.5 Remaining Concern — LOW RISK

`ramDonglesArr` must never be accessed without holding `mutexDongleList`. This invariant is correctly maintained in all current code paths but relies on developer discipline. A future refactor could accidentally break this. Consider documenting this constraint prominently in `NetworkTask.h`.

---

## 2. Resource Management

### 2.1 Stack & Heap — GOOD

Network task stack is 16 KB (generous for HTTPS/TLS). Debug builds include stack high-water mark and heap monitoring (logged every 60s). `String::reserve()` is used for URL construction to reduce heap fragmentation.

### 2.2 NVS Wear — GOOD

`Preferences::clear()` was removed from the dongle update path (direct `putString` overwrite). Failed log entries are capped at `MAX_FAILED_LOGS = 50` with FIFO discard. The `nextIdx` monotonic counter adds one small NVS write per failed log save.

### 2.3 Remaining Concern — LOW RISK

Heavy `String` usage in HTTP paths (URL construction, `http.getString()`, JSON serialization) causes heap allocations on every network operation. On ESP32-S3 with 512 KB SRAM this is unlikely to cause issues, but for multi-year uptime, heap fragmentation monitoring (already in debug builds) should be watched.

---

## 3. Debug Architecture

### 3.1 Zero-Overhead Production — GOOD

`#ifdef DEBUG_MODE` with `DBG()` macro compiles to `((void)0)` when disabled. The preprocessor discards all arguments including `DebugFlags::` references, so the `DebugFlags` struct and `DebugService` class don't even need to exist in production. Serial is never initialized. True zero overhead.

### 3.2 Per-Subsystem Flags — GOOD

`DebugFlags` struct provides granular control: `SETUP`, `WIFI_LOGGING`, `FETCH_AND_STORE_DONGLE_IDS`, `FETCH_AND_STORE_DONGLE_IDS_DETAIL`, `DOOR_STATE`, `DONGLE_SCAN`, `DONGLE_AUTH`, `SEND_STORED_LOG_ENTRIES`, `NETWORK_TASK`. All default to `true` in debug builds; developers can disable individual subsystems to reduce output.

### 3.3 Thread-Safe Singleton — GOOD

Meyers singleton (`static DebugService instance` in `getInstance()`) guarantees C++11 thread-safe initialization even if first called concurrently from both cores. Serial print mutex (100ms timeout) prevents interleaved output.

---

## 4. Network Operations

### 4.1 HTTP Timeout & Redirect — GOOD

Both `fetchAndStoreDongleIds()` and `sendLogEntryViaHttp()` set `setTimeout(20000)` and `setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS)` consistently. Google Apps Script requires redirect following.

### 4.2 WiFi Reconnect — GOOD

Moved from main loop to network task. Uses `WiFi.disconnect()` + `WiFi.begin()` (clean reconnection) instead of `WiFi.reconnect()` (fragile). 30-second check interval.

### 4.3 Failed Log Management — GOOD

Failed logs are stored in NVS as comma-separated strings with a JSON key array. Retry with 60-second backoff prevents rapid NVS wear. CSV validation catches malformed entries. Empty/orphaned entries are cleaned up. FIFO cap at 50 entries.

### 4.4 Dongle Sync — GOOD

Three sync paths: (1) NVS load at boot (immediate availability), (2) HTTP fetch at startup and every 4 hours, (3) on-demand via MasterCard with 30-second debounce. Simple string comparison of raw JSON payloads for change detection (deterministic Google Script output).

### 4.5 URL Encoding — GOOD

RFC 3986 compliant `urlEncode()` for all HTTP query parameters. Prevents injection of control characters in log data.

### 4.6 Remaining Concern — LOW RISK

Google Apps Script authentication is absent (Backlog item 4). Anyone with the URL can read dongle IDs or write arbitrary log entries.

---

## 5. RFID / Wiegand Processing

### 5.1 Bit Collection — GOOD

ISR-based bit collection with 26-bit limit. `lastBitTime` tracked for timeout detection. Both ISR handlers are minimal and IRAM-resident.

### 5.2 Timeout Reset — GOOD

Partial reads (1-25 bits) are reset after 200ms. All ISR variables (`bitCount`, `lastBitTime`) are snapshotted atomically with `noInterrupts()`. Design rationale is documented in comments.

### 5.3 Scan Processing Race — DOCUMENTED, ACCEPTABLE

Between snapshot and ISR state reset, a new scan could theoretically begin and be discarded. This is documented as acceptable (human badge scans are seconds apart).

### 5.4 Remaining Concern — BACKLOG

Wiegand parity bits (bit 1 = even, bit 26 = odd) are not validated. Corrupted reads could produce false positives or negatives. See Backlog item 3.

---

## 6. Google Apps Script

### 6.1 Read Action — GOOD

Returns flat JSON array of dongle IDs from column C. Correctly filters empty rows.

### 6.2 Write Action — GOOD

Looks up dongle name from column A by matching dongle ID in column C. Appends log row with timestamp, date, time, access type, dongle ID, and name. Range `A2:C` is correct.

### 6.3 Error Handling — GOOD

Try-catch wraps all operations. Invalid/missing action parameter returns structured JSON error. All variables properly scoped with `const`/`var`.

### 6.4 Remaining Concern — BACKLOG

No authentication (Backlog item 4). `Convert_DEC_to_BIN` has swapped parity variable names (Backlog item 3). German comments (Backlog item 6).

---

## Summary

| Category | Status |
|----------|--------|
| Thread Safety | All shared state properly synchronized |
| Resource Management | Stack, heap, NVS all within safe bounds with monitoring |
| Debug Architecture | Zero production overhead, per-subsystem flags, thread-safe |
| Network Operations | Non-blocking, resilient, properly configured |
| RFID Processing | ISR-safe, timeout-protected, race documented |
| Code Structure | Clean modular architecture with clear responsibilities |

**Open items:** See `BACKLOG.md` items 3, 4, 6.

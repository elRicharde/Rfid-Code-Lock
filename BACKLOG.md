# Backlog: RFID-Zugangskontrolle (null7b Technikecke PA)

Items are ordered by priority. Each item should be implemented as a separate commit and tested individually.

---

## 1. ~~[ARCH] Refactor HTTP calls to separate FreeRTOS task~~ DONE

**Status:** Completed
**Implemented in:** `NetworkTask.h` / `NetworkTask.cpp`

All HTTP operations (log sending, dongle refresh) moved to dedicated FreeRTOS task on Core 0. Log entries are queued via `enqueueLogEntry()` (non-blocking). Dongle refresh triggered via `xTaskNotify` (MasterCard) or periodic timer. Buzzer signals from network task to main loop via FreeRTOS queue. RFID scanning on Core 1 is never blocked by network operations. Failed logs stored in NVS with retry backoff (60s) and cap (50 entries).

---

## 2. ~~[ARCH] Debug architecture redesign~~ DONE

**Status:** Completed
**Implemented in:** `Config.h`, `DebugService.h` / `DebugService.cpp`

Replaced runtime `constexpr bool` flags with `#ifdef DEBUG_MODE` preprocessor approach. `DBG()` macro compiles to `((void)0)` in production — zero binary overhead. Per-subsystem flags (`DebugFlags::WIFI_LOGGING`, `DONGLE_SCAN`, etc.) available inside debug builds. Meyers singleton for thread-safe initialization. Serial, DebugService instance, and all debug code completely eliminated from production binary.

---

## 3. [FEAT] Wiegand parity bit validation

**Priority:** Medium
**Relates to:** CODE_REVIEW 2.1, 5.2

Currently all 26 Wiegand bits are used as the dongle ID without validating parity:
- Bit 1: Even parity for bits 2-13 (first 12 data bits)
- Bit 26: Odd parity for bits 14-25 (last 12 data bits)

Without validation, corrupted reads could grant or deny access incorrectly.

**Also fix:** Swapped variable names `evenParityBit`/`oddParityBit` in `Convert_DEC_to_BIN` (DEC_TO_BIN26 function). The computed values are correct but the names are misleading.

**Note:** Requires testing with physical hardware. The cheap 125kHz RFID reader has known quirks. Current system works without parity validation — this is a robustness improvement, not a bugfix.

---

## 4. [SEC] Google Script authentication

**Priority:** Medium
**Relates to:** CODE_REVIEW 4.2

The Google Apps Script web app currently accepts unauthenticated requests. Anyone with the URL can read all authorized dongle IDs or write arbitrary log entries.

**Options:**
- Add API key parameter validated in the script
- Use Google Apps Script built-in authentication (requires Google account)
- IP-based restriction (not practical for ESP32 with dynamic IP)

**Depends on:** Item 3 (Wiegand parity) — implement after parity validation to avoid multiple changes to the same data flow.

---

## 5. ~~[ARCH] Class architecture review~~ DONE

**Status:** Completed
**Implemented in:** Full project restructuring

Modular architecture with clear separation of concerns:
- `Config.h` — Central configuration, types, constants, shared utilities
- `DebugService.h/.cpp` — Debug-only singleton with per-subsystem flags
- `NetworkTask.h/.cpp` — All network operations, NVS access, dongle sync, log management
- `RFID_null7b.ino` — Slim main sketch: setup, loop, ISR, door monitoring, RFID processing, unlock
- `Secrets.h` — Credentials and URLs (unchanged)
- Thread safety: mutex for dongle list, queues for cross-task communication, xTaskNotify for signaling

---

## 6. [QUALITY] Language consistency

**Priority:** Low
**Relates to:** CODE_REVIEW 5.4

Standardize all comments, variable names, and debug messages to English for consistency in an open-source project. Currently a mix of German and English.

**Scope:** All source files including Google Apps Script.
**Note:** This is a cosmetic change. Implement last to avoid merge conflicts with other items.

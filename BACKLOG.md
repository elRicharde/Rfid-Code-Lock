# Backlog: RFID-Zugangskontrolle (null7b Technikecke PA)

Items are ordered by priority. Each item should be implemented as a separate commit and tested individually.

---

## 1. [ARCH] Refactor HTTP calls to separate FreeRTOS task

**Priority:** High
**Relates to:** CODE_REVIEW 3.1, Performance

Currently all HTTP calls (PostLog, fetchAndStoreDongleIds) block the main loop for up to 20 seconds (HTTP timeout). During this time, RFID scanning and door monitoring are paused.

**Goal:** Move HTTP operations to a dedicated FreeRTOS task on Core 0. Use a queue to pass log entries from the main loop to the HTTP task. This enables non-blocking logging and dongle refresh.

**Acceptance Criteria:**
- Log entries are queued and sent asynchronously
- RFID scanning is never blocked by network operations
- Dongle refresh runs in background without affecting scan responsiveness
- Error handling for queue full scenarios

---

## 2. [ARCH] Debug architecture redesign

**Priority:** Medium
**Relates to:** CODE_REVIEW 1.1 (extended), 3.1

Consider replacing the current runtime debug flags with a preprocessor-based approach (`#ifdef DEBUG_MODE`) to completely eliminate debug code from production binaries (zero flash/RAM overhead). The current `constexpr bool` approach already enables compiler dead-code elimination, but the DebugService instance and Serial initialization remain.

**Options to evaluate:**
- `#ifdef DEBUG_MODE` preprocessor guards (zero overhead, less flexible)
- Current constexpr approach with always-initialized service (current fix, ~80 bytes RAM)
- Interface-based with empty production class (adds vtable overhead, prevents inlining)

**Decision:** Evaluate after FreeRTOS refactor (item 1), as the debug architecture may need to account for multi-task logging.

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

## 5. [ARCH] Class architecture review

**Priority:** Low
**Relates to:** User request

The architecture has grown organically. Review overall class design, dependencies, and separation of concerns.

**Areas to evaluate:**
- WiFi management (connect, reconnect, status) as separate module
- RFID/Wiegand handling as separate module
- Logging service (local + remote) as separate module
- Configuration management (Secrets, constants)
- DebugService role and scope

**Depends on:** Items 1 and 2 — the FreeRTOS refactor and debug redesign will significantly change the architecture. Do this review after those are complete.

---

## 6. [QUALITY] Language consistency

**Priority:** Low
**Relates to:** CODE_REVIEW 5.4

Standardize all comments, variable names, and debug messages to English for consistency in an open-source project. Currently a mix of German and English.

**Scope:** All source files including Google Apps Script.
**Note:** This is a cosmetic change. Implement last to avoid merge conflicts with other items.

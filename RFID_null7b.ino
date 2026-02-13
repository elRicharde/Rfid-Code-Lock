/*
RFID Dongle Zugangskontrolle fuer null7b Technikecke PA
========================================================

am Schloss sind:
  schwarz = GND
  rot = 5 V open
  gelb/blau = NO Kontakt (ist das Schloss offen, gibt es kein Durchgang)

durch die Steckerverbinder aendern sich die Kabelfarben am Board wie folgt:
  gelb = GND
  blau = 5 V open
  rot/schwarz = NO Kontakt (ist das Schloss offen, gibt es kein Durchgang)

Architecture:
  Core 1 (this file): RFID scanning via ISR, door monitoring, buzzer, unlock relay
  Core 0 (NetworkTask): All HTTP operations, WiFi reconnect, dongle sync, log sending
  Communication: FreeRTOS queues (logs, buzzer signals), mutex (dongle list), xTaskNotify (refresh)
*/

#include "Config.h"
#include "DebugService.h"
#include "NetworkTask.h"
#include "Secrets.h"
#include <WiFi.h>
#include "ArduinoBuzzerSoundsRG.h"

// =============================================================
// ISR-shared variables (volatile, modified in ISR context)
// =============================================================
volatile int bitCount = 0;
volatile unsigned long dongleValue = 0;
volatile unsigned long lastBitTime = 0;

// =============================================================
// Main loop state
// =============================================================
int doorStateMemory = 2;  // Initial = 2 (neither open nor closed), in use: 0 or 1

BuzzerSoundsRgNonRtos* buzzerSounds;

// =============================================================
// Forward Declarations
// =============================================================
void IRAM_ATTR ISRreceiveData0();
void IRAM_ATTR ISRreceiveData1();
void trackDoorStateChange();
void handleRFIDScanResult();
void checkPendingBuzzerSignals();
void unlock();

// =============================================================
// Setup
// =============================================================
void setup() {
  #ifdef DEBUG_MODE
  Serial.begin(115200);
  // Countdown for serial monitor connection
  for (int i = 5; i > 0; i--) {
    DBG(DebugFlags::SETUP, i, " seconds", (i == 1 ? "." : "..."));
    delay(1000);
  }
  DBG(DebugFlags::SETUP, "Start!");
  #endif

  // Connect to WiFi (blocking, max 10 seconds — then continue without internet)
  WiFi.begin(SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    DBG(DebugFlags::WIFI_LOGGING, "Connecting to WiFi ", SSID, " ... ", attempts, "s");
    attempts++;
    delay(1000);
  }

  if (WiFi.status() == WL_CONNECTED) {
    DBG(DebugFlags::WIFI_LOGGING, "WiFi connected, IP: ", WiFi.localIP());
  } else {
    DBG(DebugFlags::WIFI_LOGGING, "WiFi not connected — will retry in background");
  }

  // NTP time sync
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, TIME_SERVER_1, TIME_SERVER_2, TIME_SERVER_3);

  // Pin setup
  pinMode(UNLOCKPIN, OUTPUT);
  pinMode(DOOR_STATE_PIN, INPUT_PULLUP);

  // Wiegand ISR setup
  pinMode(INTERRUPT_IO_PIN_1, INPUT_PULLUP);
  pinMode(INTERRUPT_IO_PIN_2, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(INTERRUPT_IO_PIN_1), ISRreceiveData0, FALLING);
  attachInterrupt(digitalPinToInterrupt(INTERRUPT_IO_PIN_2), ISRreceiveData1, FALLING);

  // Load dongles from NVS for immediate RFID availability (no HTTP needed)
  loadDonglesFromPersistentMemory();

  // Start network task on Core 0 (creates mutex + queues internally, then starts task)
  startNetworkTask();

  // Buzzer
  buzzerSounds = new BuzzerSoundsRgNonRtos(BUZZERPIN);
}

// =============================================================
// Main Loop (Core 1)
// =============================================================
void loop() {
  trackDoorStateChange();
  handleRFIDScanResult();
  checkPendingBuzzerSignals();

  // Reset partial RFID reads that timed out.
  // Design: The Wiegand protocol transmits all 26 bits within ~52ms (2ms per bit).
  // If interference or a partial read leaves bitCount between 1-25, no further scans
  // can succeed because bitCount never reaches 26. A 200ms timeout is chosen to be
  // well above the maximum valid transmission time while still recovering quickly.
  noInterrupts();
  int snapBitCount = bitCount;
  unsigned long snapLastBitTime = lastBitTime;
  interrupts();
  if (snapBitCount > 0 && snapBitCount < 26 && millis() - snapLastBitTime > WIEGAND_TIMEOUT_MS) {
    noInterrupts();
    bitCount = 0;
    dongleValue = 0;
    interrupts();
  }

  // Yield to RTOS scheduler and reduce CPU load.
  // RFID bits are captured by hardware interrupts (ISR) and are never missed by this delay.
  // Door state changes occur in the seconds range; 10ms = 100 checks/s is more than sufficient.
  delay(10);
}

// =============================================================
// ISR Handlers
// =============================================================

// IRAM_ATTR: On ESP32, ISR code must reside in Internal RAM (IRAM), not in Flash.
// During Flash operations (e.g., Preferences writes), Flash is temporarily unavailable.
// Without IRAM_ATTR, an interrupt during a Flash operation would try to execute code
// from unavailable Flash memory, causing a "Guru Meditation Error" (crash).

void IRAM_ATTR ISRreceiveData0() {
  // ISR for Data0 (bit '0') in the Wiegand protocol.
  if (bitCount < 26) {
    dongleValue <<= 1;
    bitCount++;
    lastBitTime = millis();  // millis() is ISR-safe on ESP32 (reads hardware timer)
  }
}

void IRAM_ATTR ISRreceiveData1() {
  // ISR for Data1 (bit '1') in the Wiegand protocol.
  if (bitCount < 26) {
    dongleValue <<= 1;
    dongleValue |= 1;
    bitCount++;
    lastBitTime = millis();
  }
}

// =============================================================
// Door State Monitoring
// =============================================================

void trackDoorStateChange() {
  // Read pin once to avoid inconsistency between comparison and assignment
  int currentDoorState = digitalRead(DOOR_STATE_PIN);

  if (doorStateMemory != currentDoorState) {
    doorStateMemory = currentDoorState;

    LogEntryStruct logEntry;
    getCurrentDateTime(logEntry.date, logEntry.time);

    if (doorStateMemory == DOOR_IS_CLOSED) {
      DBG(DebugFlags::DOOR_STATE, "Door closed — logging");
      safeCopyStringToChar("door_is_closed", logEntry.access, CharArrayAccessSize);
      safeCopyStringToChar("doorstate", logEntry.dongle_id, CharArrayDongleIdSize);
      enqueueLogEntry(logEntry);
    } else if (doorStateMemory == DOOR_IS_OPEN) {
      DBG(DebugFlags::DOOR_STATE, "Door opened — logging");
      safeCopyStringToChar("door_is_open", logEntry.access, CharArrayAccessSize);
      safeCopyStringToChar("doorstate", logEntry.dongle_id, CharArrayDongleIdSize);
      enqueueLogEntry(logEntry);
    } else {
      // Shouldn't happen — pin reads only 0 or 1
    }
  }
}

// =============================================================
// RFID Scan Processing
// =============================================================

void handleRFIDScanResult() {
  // Snapshot ISR variables with interrupts disabled
  noInterrupts();
  unsigned long readDongleValue = dongleValue;
  int countedBits = bitCount;
  interrupts();

  if (countedBits != 26) {
    return;
  }

  // Convert 26-bit value to binary string
  String dongleIdStr;
  dongleIdStr.reserve(27);
  for (int i = 25; i >= 0; i--) {
    dongleIdStr += ((readDongleValue >> i) & 1) ? '1' : '0';
  }

  DBG(DebugFlags::DONGLE_SCAN, "Scanned dongle: ", dongleIdStr);

  LogEntryStruct logEntry;
  getCurrentDateTime(logEntry.date, logEntry.time);

  if (isDongleIdAuthorized(dongleIdStr)) {
    DBG(DebugFlags::DONGLE_SCAN, "Access granted");
    buzzerSounds->playSound(BuzzerSoundsRgBase::SoundType::AuthOk);
    unlock();
    safeCopyStringToChar("authorised", logEntry.access, CharArrayAccessSize);
    safeCopyStringToChar(dongleIdStr, logEntry.dongle_id, CharArrayDongleIdSize);
    enqueueLogEntry(logEntry);
  } else {
    DBG(DebugFlags::DONGLE_SCAN, "Access denied");
    buzzerSounds->playSound(BuzzerSoundsRgBase::SoundType::NoAuth);
    safeCopyStringToChar("denied", logEntry.access, CharArrayAccessSize);
    safeCopyStringToChar(dongleIdStr, logEntry.dongle_id, CharArrayDongleIdSize);
    enqueueLogEntry(logEntry);
  }

  // Clear ISR state for next scan.
  // Note: if a new scan begins between our snapshot (above) and this reset,
  // the new scan's bits are discarded. This is acceptable because human
  // badge scans are seconds apart — the narrow race window (~ms) is benign.
  noInterrupts();
  bitCount = 0;
  dongleValue = 0;
  interrupts();
}

// =============================================================
// Buzzer Signal Processing
// =============================================================

void checkPendingBuzzerSignals() {
  // Check if the network task sent a buzzer signal (non-blocking)
  BuzzerSignal signal;
  if (receiveBuzzerSignal(&signal)) {
    switch (signal) {
      case BUZZER_OK:
        buzzerSounds->playSound(BuzzerSoundsRgBase::SoundType::OK);
        break;
      case BUZZER_SOS:
        buzzerSounds->playSound(BuzzerSoundsRgBase::SoundType::SOS);
        break;
      default:
        break;
    }
  }
}

// =============================================================
// Unlock
// =============================================================

void unlock() {
  digitalWrite(UNLOCKPIN, HIGH);
  delay(SWITCHDURATION_MS);
  digitalWrite(UNLOCKPIN, LOW);
}

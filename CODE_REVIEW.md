# Code Review: RFID-Zugangskontrolle (null7b Technikecke PA)

**Datum:** 2026-02-12
**Projekt:** ESP32-S3 RFID Access Control mit Google Sheets Integration
**Dateien:** 6 Quelldateien, ~900 Zeilen Code
**Reviewer:** Claude Opus 4.6

---

## Inhaltsverzeichnis

- [1. Kritische Bugs](#1-kritische-bugs)
- [2. Mittlere Bugs / Probleme](#2-mittlere-bugs--probleme)
- [3. Architektur & Design](#3-architektur--design)
- [4. Sicherheit](#4-sicherheit)
- [5. Code-Qualitat](#5-code-qualität)
- [6. Zusammenfassung](#6-zusammenfassung)

---

## 1. Kritische Bugs

### 1.1 Crash bei `DEBUG_MODE = false`

**Datei:** `RFID_null7b.ino:175`
**Schwere:** Kritisch

`debugService` wird nur initialisiert wenn `DEBUG_MODE == true` (Zeile 154-168), aber ausserhalb des if-Blocks bedingungslos verwendet:

```cpp
// Zeile 175 - wird IMMER erreicht, auch wenn debugService == nullptr
debugService->SerialPrintln_ifDebug(DebugFlags::WIFI_LOGGING, "Connect to WiFi...");
```

Da `WIFI_LOGGING` auch `false` ist, wird `if (Debug)` in der Methode zwar `false` ergeben, aber der Methodenaufruf auf einem nullptr ist **Undefined Behavior** in C++. Das funktioniert zufaellig auf manchen Compilern, ist aber nicht garantiert und crasht moeglicherweise bei Compiler-Updates oder Optimierungs-Level-Aenderungen.

**Betroffen:** Alle `debugService->` Aufrufe ausserhalb des `if (DebugFlags::DEBUG_MODE)` Blocks in `setup()` sowie alle Aufrufe in den weiteren Funktionen.

**Fix:** `debugService` immer initialisieren (auch bei `DEBUG_MODE = false`), oder alle Aufrufe mit einem Null-Check bzw. `if (DebugFlags::DEBUG_MODE)` schuetzen.

---

### 1.2 Infinite Loop in `sendStoredLogEntries()`

**Datei:** `RFID_null7b.ino:549-578`
**Schwere:** Kritisch

```cpp
while (i < keyArray.size()) {
    String key = keyArray[i];
    String logEntryStringCommaSeparated = preferencesLog.getString(key.c_str(), "");

    if (logEntryStringCommaSeparated != "") {
        // ... send or break
    }
    // KEIN i++ und KEIN break wenn der String leer ist!
}
```

Wenn ein gespeicherter Log-Eintrag leer ist (`""`), wird `i` nie inkrementiert und die Schleife nie verlassen. Das System haengt in einer **Endlosschleife**, der ESP reagiert nicht mehr.

**Fix:** Nach dem `if`-Block ein `else { i++; }` einfuegen, oder den leeren Eintrag entfernen und den Index nicht erhoehen.

---

### 1.3 Shallow Copy von JsonArray

**Datei:** `RFID_null7b.ino:546`
**Schwere:** Kritisch

```cpp
JsonArray keyArrayCopy = keyArray;  // Das ist KEINE tiefe Kopie!
```

`JsonArray` ist eine Referenz/View auf dasselbe `JsonDocument`. Wenn Elemente aus `keyArray` mit `keyArray.remove(i)` entfernt werden, aendert sich `keyArrayCopy` identisch mit. Die Vergleichslogik in Zeile 585:

```cpp
} else if (keyArray.size() != keyArrayCopy.size()) {  // IMMER gleich!
```

...ist **immer `false`**.

**Konsequenz:** Wenn nur ein Teil der gespeicherten Logs erfolgreich gesendet wird, wird der `keyArray` im persistenten Speicher nie aktualisiert. Diese Logs werden bei jedem Neustart erneut gesendet (Duplikate) oder blockieren neue Eintraege.

**Fix:** Die Groesse vor der Schleife in einer `int`-Variable speichern: `int originalSize = keyArray.size();`

---

### 1.4 Pointer-Arithmetik statt String-Konkatenation

**Datei:** `RFID_null7b.ino:357`
**Schwere:** Kritisch

```cpp
debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "http is " + httpCode);
```

`"http is "` ist ein `const char*`. Der `+`-Operator mit `int` fuehrt **Pointer-Arithmetik** aus (nicht String-Verkettung!). Das liest aus zufaelligem Speicher und fuehrt zu Absturz oder Garbage-Output.

**Fix:** Die variadic Template-Methode nutzen (Komma statt Plus):
```cpp
debugService->SerialPrintln_ifDebug(DebugFlags::FETCH_AND_STORE_DONGLE_IDS, "http is ", httpCode);
```

---

### 1.5 Doppeltes `digitalRead` bei Tuerstatus

**Datei:** `RFID_null7b.ino:456-457`
**Schwere:** Hoch

```cpp
if (DoorStateMemory != digitalRead(DOOR_STATE_PIN)){      // Erster Read
    DoorStateMemory = digitalRead(DOOR_STATE_PIN);          // Zweiter Read - Pin kann sich geaendert haben!
```

Zwischen den zwei Reads kann sich der Pin-Zustand aendern (Prellen, Timing). Das kann zu inkonsistentem Zustand fuehren, z.B. wird ein Zustandswechsel erkannt, aber der gespeicherte Wert stimmt nicht mit dem urspruenglichen ueberein.

**Fix:** Einmal lesen, Wert zwischenspeichern:
```cpp
int currentState = digitalRead(DOOR_STATE_PIN);
if (DoorStateMemory != currentState) {
    DoorStateMemory = currentState;
```

---

### 1.6 Fehlender RFID-Timeout

**Datei:** `RFID_null7b.ino:296, 658`
**Schwere:** Hoch

Wenn nur ein Teil der 26 Wiegand-Bits empfangen wird (z.B. durch Stoerung oder partielles Lesen), bleibt `bitCount` groesser als 0 aber kleiner als 26 - fuer immer. Es gibt keinen Reset-Mechanismus. Das System kann dann **nie wieder einen Dongle lesen** bis zum Neustart.

**Fix:** Einen Timestamp beim ersten empfangenen Bit setzen und in `loop()` pruefen ob seit dem letzten Bit zu viel Zeit vergangen ist (typisch ~50ms). Dann `bitCount` und `dongleValue` zuruecksetzen:

```cpp
volatile unsigned long lastBitTime = 0;  // global

// In ISRs:
lastBitTime = millis();

// In loop():
if (bitCount > 0 && bitCount < 26 && millis() - lastBitTime > 50) {
    noInterrupts();
    bitCount = 0;
    dongleValue = 0;
    interrupts();
}
```

---

### 1.7 Fehlende `IRAM_ATTR` bei ISR-Funktionen

**Datei:** `RFID_null7b.ino:288, 304`
**Schwere:** Hoch

```cpp
void ISRreceiveData0() {   // Fehlt: IRAM_ATTR
void ISRreceiveData1() {   // Fehlt: IRAM_ATTR
```

Auf ESP32 muessen Interrupt Service Routines im IRAM (Internal RAM) liegen. Ohne `IRAM_ATTR` koennen die Funktionen im Flash liegen. Wenn waehrend einer Flash-Operation (z.B. Preferences-Schreibzugriff) ein Interrupt auftritt, fuehrt das zu einem **Guru Meditation Error (Crash)**.

**Fix:**
```cpp
void IRAM_ATTR ISRreceiveData0() { ... }
void IRAM_ATTR ISRreceiveData1() { ... }
```

---

## 2. Mittlere Bugs / Probleme

### 2.1 Wiegand-Paritaetsbits werden nicht validiert

**Datei:** `RFID_null7b.ino:658-696`

`handleRFIDScanResult()` nutzt alle 26 Bits als ID, ohne Bit 0 (even parity) und Bit 25 (odd parity) zu pruefen. Im Wiegand-26-Format sind Bit 1 und Bit 26 Paritaetsbits, die nur 24 Datenbits schuetzen.

**Konsequenz:** Fehlerhaft empfangene IDs koennten als gueltig durchgehen oder gueltige Dongles koennen abgelehnt werden.

**Empfehlung:** Paritaet pruefen und bei Fehler den Scan verwerfen.

---

### 2.2 Hardcoded Buffer-Groessen

**Datei:** `RFID_null7b.ino:463-464, 472-474`

```cpp
safeCopyStringToChar("door_is_closed", logEntry.access, 15);  // Magic number statt CharArrayAccessSize
safeCopyStringToChar("doorstate", logEntry.dongle_id, 10);     // 10 statt CharArrayDongleIdSize
```

Bei Aenderung der Enum-Werte oder Strings bricht das still und unbemerkt.

**Fix:** Ueberall `CharArraySizes::CharArrayAccessSize` bzw. `CharArraySizes::CharArrayDongleIdSize` verwenden.

---

### 2.3 `MasterCard`-Check innerhalb der Schleife

**Datei:** `RFID_null7b.ino:715`

```cpp
for (JsonVariant v : ramDonglesArr) {
    if (dongleIdStr.equals(DONGLE_MASTER_CARD_UPDATE_DB)) {  // bei JEDER Iteration geprueft
```

Die MasterCard-Pruefung hat nichts mit dem aktuellen Array-Element `v` zu tun. Sie sollte **vor** der Schleife stehen, um unnoetige Iterationen zu vermeiden.

---

### 2.4 Deserialisierung ohne Fehlerpruefung in `saveFailedLogEntry`

**Datei:** `RFID_null7b.ino:609`

```cpp
deserializeJson(doc, keyArrayStr);  // Rueckgabewert wird ignoriert
```

Wenn der persistente Speicher korrupte Daten enthaelt, wird das nicht erkannt. Neue Log-Eintraege koennten verloren gehen.

---

### 2.5 Main Loop ohne Delay

**Datei:** `RFID_null7b.ino:216-221`

```cpp
void loop() {
    trackDoorStateChange();
    handleRFIDScanResult();
    // Kein delay/yield -> maximale CPU-Last, unnoetig hoher Stromverbrauch
}
```

**Fix:** Ein kurzes `delay(1)` oder `vTaskDelay(1)` einfuegen.

---

## 3. Architektur & Design

### 3.1 Blocking HTTP-Calls im Main Loop

`PostLog()` -> `sendStoredLogEntries()` -> `sendLogEntryViaHttp()` blockiert den Main-Thread fuer bis zu 20+ Sekunden (HTTP-Timeout). Waehrend dieser Zeit:

- Werden **keine RFID-Scans** verarbeitet
- Wird **kein Tuerstatus** ueberwacht
- Ist das System effektiv nicht funktionsfaehig

**Empfehlung:** HTTP-Calls in einen separaten FreeRTOS-Task auslagern und Log-Eintraege per Queue uebergeben. Der ESP32 hat zwei Kerne - ideal fuer diese Aufgabe.

---

### 3.2 Kein WiFi-Reconnect-Handling

Nach Verbindungsverlust gibt es keine explizite Reconnect-Logik. `WiFi.begin()` wird nur einmal in `setup()` aufgerufen. Der ESP32 hat Auto-Reconnect, aber das ist nicht immer zuverlaessig.

**Empfehlung:** Periodisch `WiFi.status()` pruefen und bei Bedarf `WiFi.reconnect()` aufrufen.

---

### 3.3 Dongle-IDs werden nur beim Start geladen

`fetchAndStoreDongleIds()` wird nur in `setup()` und bei MasterCard-Scan aufgerufen. Neue Dongles erfordern entweder einen Neustart oder die MasterCard.

**Empfehlung:** Periodisches Refresh (z.B. alle 30 Minuten) in einem separaten Task.

---

### 3.4 `ramDonglesArr` Initialisierung fehlerhaft

**Datei:** `RFID_null7b.ino:71`

```cpp
JsonArray ramDonglesArr = ramDonglesDoc.as<JsonArray>(); // Null-Array auf leerem Doc!
```

Wird dann in `setup()` Zeile 202 nochmal korrekt initialisiert. Die globale Initialisierung ist nutzlos und irrefuehrend.

---

## 4. Sicherheit

### 4.1 Keine URL-Kodierung bei HTTP-Parametern

**Datei:** `RFID_null7b.ino:508`

```cpp
webappurl_write += "&date=" + String(logEntry.date) + "&time=" + ...
```

Wenn ein Feld Sonderzeichen enthaelt (`&`, `=`, Leerzeichen), wird die URL fehlerhaft. Das Risiko ist gering da die Daten intern generiert werden, aber es ist schlechte Praxis und koennte bei unerwarteten Eingaben zu Problemen fuehren.

---

### 4.2 Google Script ohne Authentifizierung

**Datei:** `googleScript:2`

Die Web-App akzeptiert jede Anfrage ohne Authentifizierung. Jeder mit der URL kann:
- Alle autorisierten Dongle-IDs auslesen (`action=read_pa`)
- Beliebige Log-Eintraege schreiben (`action=write_log_pa`)

**Empfehlung:** Mindestens einen API-Key als Parameter hinzufuegen und im Script validieren.

---

### 4.3 Google Script: `access` als implizite globale Variable

**Datei:** `googleScript:45`

```javascript
access = String(e.parameter.access);  // 'var'/'let'/'const' fehlt!
```

`access` wird ohne Deklaration zugewiesen und wird damit zur impliziten globalen Variable. Bei parallelen Anfragen an die Web-App koennte das zu Race Conditions fuehren.

**Fix:** `var access = ...` oder besser `const access = ...`

---

### 4.4 Google Script: Ungewoehnliche Range-Definition

**Datei:** `googleScript:50`

```javascript
var idRange = idSheet.getRange('C2:A' + idSheet.getLastRow());
```

Die Range geht von Spalte C zu Spalte A (rueckwaerts). Google Sheets normalisiert das intern zu `A2:C`, aber es ist verwirrend und fehleranfaellig. Korrekt waere `'A2:C' + idSheet.getLastRow()`.

---

## 5. Code-Qualitaet

### 5.1 Tippfehler im Konstantennamen

**Datei:** `RFID_null7b.ino:35`

```cpp
constexpr const char PERS_MEM_DONGLE_IDS[10] = "DonlgeIds";  // "Donlge" statt "Dongle"
```

Funktioniert intern konsistent, ist aber verwirrend beim Lesen und Debugging.

---

### 5.2 Vertauschte Variablennamen in `DEC_TO_BIN26`

**Datei:** `Convert_DEC_to_BIN:26-27`

```javascript
var evenParityBit = ...  // Berechnet tatsaechlich ODD Parity (Bit 26)
var oddParityBit = ...   // Berechnet tatsaechlich EVEN Parity (Bit 1)
```

Die berechneten **Werte sind korrekt** fuer Wiegand-26, aber die Variablennamen sind vertauscht. Das fuehrt zu Verwirrung bei Wartung und Debugging.

---

### 5.3 Mutex-Benennung ist irrefuehrend

`mutexPersistentDongleStorage` schuetzt `ramDonglesArr` (RAM), nicht den persistenten Speicher. Der Name suggeriert etwas anderes als der tatsaechliche Schutzbereich.

---

### 5.4 Gemischte Sprachen

Kommentare und Debug-Meldungen sind ein Mix aus Deutsch und Englisch. Fuer ein Open-Source-Projekt empfiehlt sich einheitlich Englisch.

---

### 5.5 `constexpr const char` Arrays mit expliziten Groessen

**Datei:** `Secrets.h`

```cpp
constexpr char SSID[10] = "YOUR SSID";
constexpr char WEB_APP_URL[124] = "https://...";
```

Hardcoded Array-Groessen sind fehleranfaellig. Wenn der tatsaechliche String laenger ist als die angegebene Groesse, wird er still abgeschnitten. Besser den Compiler die Groesse bestimmen lassen:

```cpp
constexpr char SSID[] = "YOUR SSID";
```

---

## 6. Zusammenfassung

| Kategorie | Anzahl | Schwere |
|-----------|--------|---------|
| Kritische Bugs | 7 | System-Crash, Endlosschleifen, Datenverlust |
| Mittlere Bugs | 5 | Fehlerhafte Logik, unnoetige CPU-Last |
| Architektur-Probleme | 4 | Blockierendes I/O, fehlende Reconnect-Logik |
| Sicherheitsprobleme | 4 | Fehlende Authentifizierung, implizite Globals |
| Code-Qualitaet | 5 | Tippfehler, irrefuehrende Namen, Inkonsistenzen |
| **Gesamt** | **25** | |

### Priorisierte Empfehlung

| Prioritaet | Issue | Grund |
|------------|-------|-------|
| 1 | Crash bei `DEBUG_MODE = false` (#1.1) | Betrifft den Produktivbetrieb direkt |
| 2 | Infinite Loop in `sendStoredLogEntries` (#1.2) | Kann das System komplett lahmlegen |
| 3 | `IRAM_ATTR` bei ISRs (#1.7) | Sporadische Crashes bei Flash-Operationen |
| 4 | RFID-Timeout (#1.6) | System kann dauerhaft blockiert werden |
| 5 | Shallow Copy JsonArray (#1.3) | Doppelte Log-Eintraege, Speicher-Inkonsistenz |
| 6 | Blocking HTTP im Main Loop (#3.1) | System fuer 20+ Sekunden nicht funktionsfaehig |
| 7 | Pointer-Arithmetik (#1.4) | Speicherzugriffsfehler bei Debug-Ausgabe |

---

*Code Review erstellt mit Claude Opus 4.6*

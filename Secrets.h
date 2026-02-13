#ifndef SECRETS_H
#define SECRETS_H

#include <Arduino.h>  // Include the Arduino core library for String type

// MasterKeyCard and MagicWord
// Array sizes are determined by the compiler to prevent silent truncation when values change.
constexpr const char OPEN_FOR_ALL_DONGLES[] = "YOUR_MAGIC_WORDS";
constexpr const char DONGLE_MASTER_CARD_UPDATE_DB[] = "MASTER_KEY_CARD_ID";

// WiFi credentials
constexpr char SSID[] = "YOUR SSID";
constexpr char WIFI_PASSWORD[] = "YOUR WIFI PW";

// Google Apps Script Web-App URL
constexpr char WEB_APP_URL[] =      "https://script.google.com/macros/s/67890123456789012345678901234567890123456789012345678901234567890123456789/exec";
constexpr char WEB_APP_URL_READ[] = "https://script.google.com/macros/s/67890123456789012345678901234567890123456789012345678901234567890123456789/exec?action=read_pa";
                                                                      
#endif // SECRETS_H

#ifndef SECRETS_H
#define SECRETS_H

#include <Arduino.h>  // Include the Arduino core library for String type

// MasterKeyCard und MagicWord 
constexpr const char OPEN_FOR_ALL_DONGLES[21] = "YOUR_MAGIC_WORDS";
constexpr const char DONGLE_MASTER_CARD_UPDATE_DB[27] = "MASTER_KEY_CARD_ID";

// Wifi Daten as Constants
constexpr char SSID[10] = "YOUR SSID";
constexpr char WIFI_PASSWORD[75] = "YOUR WIFI PW";

// URL Ihrer Web-App
constexpr char WEB_APP_URL[124] =      "https://script.google.com/macros/s/67890123456789012345678901234567890123456789012345678901234567890123456789/exec";  //put your web-app-url here
constexpr char WEB_APP_URL_READ[139] = "https://script.google.com/macros/s/67890123456789012345678901234567890123456789012345678901234567890123456789/exec?action=read_pa";  //put your web-app-url here
// weitere Konstanten                   12345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890
                                                                      
#endif // SECRETS_H

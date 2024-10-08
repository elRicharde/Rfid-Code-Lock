#ifndef SECRETS_H
#define SECRETS_H

#include <Arduino.h>  // Include the Arduino core library for String type

// Wifi Daten as Constants
constexpr char SSID[10] = "YOUR SSID";
constexpr char WIFI_PASSWORD[75] = "YOUR WIFI PW";

// URL Ihrer Web-App
constexpr char WEB_APP_URL[124] =      "https://script.google.com/macros/s/67890123456789012345678901234567890123456789012345678901234567890123456789/exec";  //put your web-app-url here
constexpr char WEB_APP_URL_READ[139] = "https://script.google.com/macros/s/67890123456789012345678901234567890123456789012345678901234567890123456789/exec?action=read_pa";  //put your web-app-url here
// weitere Konstanten                   12345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890
                                                                      
#endif // SECRETS_H

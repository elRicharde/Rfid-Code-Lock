# Rfid-Code-Lock
125 kHz RfID Code Lock with Arduino and GoogleSheets &amp; WebApp

# purpose


# Sheet for Dongle Ids
Name | Id Dec | Id Bin W26 ValueOnly | Datum hinzugefügt | Id BinW26 Formula
|-| - | - | - | -
John Doe |	0001217496 |	00001001010010011110110001 |	01.01.2024 | 	=DEC_TO_BIN26(B2)

# Sheet for Log
Datum Db Write | Datum Rfid Scan | Uhrzeit Rfid Scan | access | Dongle Id | Name
|-|-|-|-|-|-
06.03.2024 20:34:27 |	06.03.2024 | 20:34:24	| authorised | 00001001010010011110110001	| John Doe
06.03.2024 20:41:17	| 06.03.2024 |	20:41:14 |	denied	| 11100110110100100111011001	

# Hardware used
- Arduino Nano ESP32 (ESP32-S3)
- Seeedgrove 125 kHz RfId Modul with Antenna (https://wiki.seeedstudio.com/Grove-125KHz_RFID_Reader/)
- Buzzer AZDelivery KY-012 (https://www.az-delivery.de/products/buzzer-modul-aktiv)
- 5 V Relay complete with protection diode (https://www.az-delivery.de/products/relais-modul OR https://www.az-delivery.de/products/relais-modul)
- 5 V Power Supply AZ-Delivery MB102 (https://www.az-delivery.de/products/mb102-breadboard)
- 9 V Power Adapter vor Power Supply
- Breadboard or Circuit Board 

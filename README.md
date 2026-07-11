# ESP32 lamp controller

Firmware for an ESP32 Dev Module that serves a local web dashboard for lamp control, local time, DHT22 readings, soil moisture, and Open-Meteo weather for Magdeburg.

## Hardware

- Lamp relay: `GPIO13`, active `LOW` (`LOW` turns it on, `HIGH` turns it off)
- Soil moisture analog output: `GPIO34`
- Soil moisture sensor power: `GPIO32`
- DHT22 data: `GPIO15`

`GPIO15` is an ESP32 boot-strapping pin. If the board starts slowly or fails to boot after connecting the DHT22 pull-up, move the DHT22 signal to a non-strapping GPIO such as `GPIO27`, `GPIO33`, or `GPIO25`.

`GPIO34` is an input-only ADC1 pin, so it can read the analog soil sensor while Wi-Fi is active.

## Setup

1. Open the project in PlatformIO.
2. In `src/main.cpp`, set:
   - `WIFI_SSID`
   - `WIFI_PASSWORD`
   - soil calibration values if needed: `SOIL_DRY_VALUE`, `SOIL_WET_VALUE`
3. Build and upload:

```sh
pio run --target upload
```

4. Open the serial monitor at `115200` baud to see the ESP32 IP address.
5. Visit that IP address in a browser on the same Wi-Fi network.

The firmware uses NTP with the POSIX timezone string for Germany:

```cpp
const char* TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";
```

Open-Meteo is queried without an API key for Magdeburg coordinates.
Sensor readings are saved once per minute in ESP32 flash via `Preferences`, so the web page can show the latest history after reboot.

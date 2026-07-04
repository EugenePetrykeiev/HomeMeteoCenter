# ESP32 lamp controller

Firmware for an ESP32 Dev Module that serves a local web dashboard for lamp control, local time, DHT22 readings, soil moisture, and Open-Meteo weather for Magdeburg.

## Hardware

- Lamp relay: `GPIO21`, active `HIGH`
- Soil moisture analog output: `GPIO34`
- Soil moisture sensor power: `GPIO32`
- DHT22 data: `GPIO33`

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

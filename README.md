# DIMO

DIMO is a desktop robot companion firmware for the Waveshare ESP32-C6 Touch AMOLED 1.8.

## Current Version

`0.18.0`

## Hardware

- Waveshare ESP32-C6 Touch AMOLED 1.8
- 368 x 448 AMOLED display
- FT3168 touch controller
- ESP32-C6 WiFi/BLE

## Features

- Animated DIMO face
- Touch navigation
- Clock page
- Weather page
- Music / Bluetooth HID remote page
- Settings page
- WiFi setup portal with saved credentials

## Private Config

Copy `secrets.example.h` to `secrets.h` and fill in your private WiFi and OpenWeather values.

`secrets.h` is intentionally ignored by git.

## Build

Compile/upload using Arduino CLI or Arduino IDE with ESP32 core:

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32c6:FlashSize=8M,PartitionScheme=default_8MB,CDCOnBoot=cdc,UploadSpeed=115200 \
  .
```

```bash
arduino-cli upload -p /dev/cu.usbmodemXXXX \
  --fqbn esp32:esp32:esp32c6:FlashSize=8M,PartitionScheme=default_8MB,CDCOnBoot=cdc,UploadSpeed=115200 \
  .
```

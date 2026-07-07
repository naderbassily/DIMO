# Changelog

## 0.19.0-alpha.1 - 2026-07-07

- Starts the Taby-inspired visual rebuild on `v0.19-next`.
- Replaces the old default face with a black-and-white digital sketch animation loop.
- Adds idle scenes inspired by the new references: drawing/pencil, maker-table, flower eyes, and checklist.
- Keeps the firmware vector-only so the animation stays lightweight on the ESP32-C6 AMOLED.

## 0.18.0 - 2026-07-07

- Baseline DIMO firmware for the Waveshare ESP32-C6 Touch AMOLED 1.8.
- Adds custom font headers for cleaner AMOLED UI typography.
- Improves boot, clock, weather, WiFi portal, settings, and music UI text layout.
- Keeps WiFi credentials and OpenWeather key out of git via `secrets.h`.

## 0.17.0

- Added WiFi setup portal, AP setup page, and NVS credential storage.

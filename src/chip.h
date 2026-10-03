#pragma once
// Chip-abhängige Festwerte. Die Firmware läuft auf dem ESP32-C6 und dem ESP32-C3; der Zielchip
// kommt aus der Build-Umgebung (CONFIG_IDF_TARGET_*). Alles andere ist für beide Chips gleich.
#include <cstddef>
#include <cstdint>
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32C6
// ESP32-C6: GPIO 12/13 = USB (Konsole), 24-30 = interner Flash
#define CHIP_NAME "ESP32-C6"
#define CHIP_TARGET_ID "esp32c6"
inline constexpr int8_t CHIP_ALLOWED_GPIOS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 15, 16, 17, 18, 19, 20, 21, 22, 23};
inline constexpr int CHIP_SCAN_GPIOS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 15, 16, 17, 18, 19, 20, 21, 22, 23};
// Standard-Pins: RX, TX, DE
inline constexpr int8_t CHIP_DEFAULT_PINS_RTU1[3] = {16, 17, 2};
inline constexpr int8_t CHIP_DEFAULT_PINS_RTU2[3] = {20, 21, 22};

#elif CONFIG_IDF_TARGET_ESP32C3
// ESP32-C3: GPIO 18/19 = USB (Konsole), 11-17 = SPI-Flash, 2/8/9 = Strapping-Pins
#define CHIP_NAME "ESP32-C3"
#define CHIP_TARGET_ID "esp32c3"
inline constexpr int8_t CHIP_ALLOWED_GPIOS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21};
inline constexpr int CHIP_SCAN_GPIOS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 20, 21};
// Standard-Pins: RX, TX, DE (-1 = RS485-Modul mit automatischer Richtungsumschaltung);
// entspricht der Beschriftung des C3-Boards mit zwei Auto-Modulen
inline constexpr int8_t CHIP_DEFAULT_PINS_RTU1[3] = {21, 20, -1};
inline constexpr int8_t CHIP_DEFAULT_PINS_RTU2[3] = {10, 3, -1};

#else
#error "Nicht unterstützter Chip: nur ESP32-C6 und ESP32-C3"
#endif

// Standardwert der maximalen WLAN-Sendeleistung in 0,25 dBm (esp_wifi_set_max_tx_power); 0 = Maximum.
// In der Oberfläche unter System › WLAN einstellbar (wifiTxPower).
// Viele kleine C3-Boards (z. B. "ESP32-C3 SuperMini") haben eine schlecht abgestimmte Antenne:
// ab ca. 15 dBm bricht das Funksignal ein (gemessen: 100 % Paketverlust). Gemessen am SuperMini:
// 8,5 dBm zu schwach (bis 40 % Verlust), 11 dBm 0 % Verlust bei kurzer Laufzeit -> 11 dBm (44).
#if CONFIG_IDF_TARGET_ESP32C3
inline constexpr int8_t CHIP_WIFI_MAX_TX_POWER = 44;
#else
inline constexpr int8_t CHIP_WIFI_MAX_TX_POWER = 0;
#endif

// Größe des Log-Ringpuffers für die Web-Konsole (statischer RAM). Der C3 hat deutlich weniger RAM.
#if CONFIG_IDF_TARGET_ESP32C3
inline constexpr size_t CHIP_LOG_BUFFER_SIZE = 8 * 1024;
#else
inline constexpr size_t CHIP_LOG_BUFFER_SIZE = 24 * 1024;
#endif

// Boot-Taster (Werksreset beim Start 5 s halten) liegt bei beiden Chips an GPIO 9
inline constexpr int CHIP_FACTORY_RESET_GPIO = 9;

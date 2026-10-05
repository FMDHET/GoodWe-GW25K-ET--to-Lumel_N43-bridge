cmake_minimum_required(VERSION 3.16)

# Firmware-Version (landet in esp_app_desc_t und wird beim OTA-Update geprüft)
set(PROJECT_VER "2.10.10")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# WireGuard (esphome/wireguard): Anzahl erlaubter Netze (AllowedIPs) wird beim Übersetzen festgelegt.
# Tunnel-Subnetz + bis zu 8 Netze aus der Konfiguration.
idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_WIREGUARD_MAX_SRC_IPS=9" APPEND)
project(modbus-bridge)

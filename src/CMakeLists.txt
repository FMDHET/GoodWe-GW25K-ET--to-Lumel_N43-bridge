idf_component_register(
    SRCS
        "main.cpp"
        "shared.cpp"
        "config.cpp"
        "modbus_rtu.cpp"
        "modbus_tcp.cpp" "modbus_tcp_server.cpp"
        "goodwe.cpp"
        "goodwe_sensors.cpp"
        "lumel.cpp"
        "net.cpp"
        "dns_server.cpp"
        "web.cpp"
        "mqtt.cpp"
        "ota.cpp"
        "console.cpp"
        "log_buffer.cpp"
        "web_console.cpp"
        "wireguard_vpn.cpp"
        "time_sync.cpp"
        "diagnostics.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer
        app_update esp_app_format bootloader_support esp_partition
        esp_driver_uart esp_driver_gpio mqtt esp-tls mbedtls json console
        esp_system lwip spi_flash espcoredump
)

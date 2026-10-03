// ESP32-C6/-C3 Modbus-Translator: GoodWe ET per Modbus RTU auslesen, Daten als Lumel N43 ausgeben.
// Reines ESP-IDF-Projekt. Diese Datei initialisiert alle Module und läuft danach als Supervisor-Task.
#include "chip.h"
#include "config.h"
#include "console.h"
#include "driver/gpio.h"
#include "diagnostics.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "goodwe.h"
#include "log_buffer.h"
#include "lumel.h"
#include "modbus_rtu.h"
#include "modbus_tcp.h"
#include "modbus_tcp_server.h"
#include "time_sync.h"
#include "wireguard_vpn.h"
#include "mqtt.h"
#include "net.h"
#include "nvs_flash.h"
#include "ota.h"
#include "shared.h"
#include "util.h"
#include "web.h"

static const char* TAG = "main";

static const gpio_num_t FACTORY_RESET_BUTTON = (gpio_num_t)CHIP_FACTORY_RESET_GPIO;  // BOOT-Taste
static const uint32_t FACTORY_RESET_HOLD_MS = 5000;

// RTU1 nutzt UART1, RTU2 nutzt UART0 (die Konsole läuft über USB-Serial-JTAG)
static RtuPort rtuPort1;
static RtuPort rtuPort2;
static RtuPort* rtuPorts[2] = {&rtuPort1, &rtuPort2};
static const uart_port_t RTU_UART_NUMBERS[2] = {UART_NUM_1, UART_NUM_0};

// Initialisiert die NVS-Partition. Ist sie unlesbar (voll oder neues Format), wird sie gelöscht
// und neu angelegt - die Konfiguration geht dann verloren, das Gerät startet aber sicher.
static void initNvs() {
  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS-Partition unlesbar - wird neu angelegt");
    ESP_ERROR_CHECK(nvs_flash_erase());
    result = nvs_flash_init();
  }
  ESP_ERROR_CHECK(result);
}

// Prüft beim Start die BOOT-Taste: 5 s gehalten -> Werkseinstellungen und Neustart.
// Wird sie vorher losgelassen, läuft der Start normal weiter.
static void checkFactoryResetButton() {
  gpio_config_t buttonConfig = {};
  buttonConfig.pin_bit_mask = 1ULL << FACTORY_RESET_BUTTON;
  buttonConfig.mode = GPIO_MODE_INPUT;
  buttonConfig.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&buttonConfig);
  if (gpio_get_level(FACTORY_RESET_BUTTON) != 0) return;  // nicht gedrückt (aktiv Low)

  ESP_LOGW(TAG, "BOOT-Taste gedrückt - %lu s halten für Werksreset", FACTORY_RESET_HOLD_MS / 1000);
  uint32_t pressedSinceMs = millisSinceBoot();
  while (gpio_get_level(FACTORY_RESET_BUTTON) == 0) {
    if (millisSinceBoot() - pressedSinceMs > FACTORY_RESET_HOLD_MS) {
      configFactoryReset();
      ESP_LOGW(TAG, "Werkseinstellungen geladen - Neustart");
      // Erst nach dem Loslassen neu starten, sonst würde der Werksreset beim nächsten Start erneut ausgelöst
      while (gpio_get_level(FACTORY_RESET_BUTTON) == 0) vTaskDelay(pdMS_TO_TICKS(10));
      esp_restart();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  gpio_reset_pin(FACTORY_RESET_BUTTON);
}

// Startet beide RS485-Ports gemäß Konfiguration und je nach Rolle den GoodWe-Master-
// bzw. Lumel-Slave-Task. Deaktivierte oder fehlerhaft initialisierte Ports werden übersprungen.
static void startModbusPorts() {
  for (int portIndex = 0; portIndex < 2; portIndex++) {
    const PortConfig& portConfig = g_config.port[portIndex];
    if (portConfig.role == ROLE_OFF) {
      ESP_LOGI(TAG, "RTU%d deaktiviert", portIndex + 1);
      continue;
    }
    RtuPort* port = rtuPorts[portIndex];
    port->setLogName(portIndex == 0 ? "RTU1" : "RTU2");
    bool started = port->begin(RTU_UART_NUMBERS[portIndex], portConfig);
    ESP_LOGI(TAG, "RTU%d: %s, %lu %c%u, Adresse %u, RX=%d TX=%d DE=%d, t1,5=%lu us t3,5=%lu us -> %s",
             portIndex + 1, portConfig.role == ROLE_GOODWE_MASTER ? "GoodWe-Master" : "Lumel-Slave",
             (unsigned long)portConfig.baudRate, portConfig.parity, portConfig.stopBits, portConfig.slaveAddress,
             portConfig.rxPin, portConfig.txPin, portConfig.driverEnablePin, (unsigned long)port->charTimeout15Us(),
             (unsigned long)port->frameTimeout35Us(), started ? "OK" : "FEHLER");
    if (!started) continue;
    if (portConfig.role == ROLE_GOODWE_MASTER) goodweStart(portIndex, port, portConfig.slaveAddress);
    else lumelStart(portIndex, port);
  }
}

// Startet das Auslesen des GoodWe über Modbus TCP (z. B. über ein RTU-zu-TCP-Gateway),
// wenn diese Anbindung eingestellt ist.
static void startGoodweOverTcp() {
  if (g_config.goodweTransport != GOODWE_VIA_TCP) return;
  static ModbusTcpClient goodweTcpClient;
  goodweTcpClient.configure(g_config.goodweTcpHost, g_config.goodweTcpPort);
  ESP_LOGI(TAG, "GoodWe über Modbus TCP: %s:%u, Unit-ID %u", g_config.goodweTcpHost, g_config.goodweTcpPort,
           g_config.goodweTcpUnitId);
  goodweStart(GOODWE_TCP_STATS_INDEX, &goodweTcpClient, g_config.goodweTcpUnitId);
}

// Einstiegspunkt von ESP-IDF. Reihenfolge ist wichtig: NVS vor OTA/Konfiguration,
// Bus-Diagnose vor dem Start der Modbus-Ports (UARTs müssen frei sein).
// Begrenzt die sehr gesprächigen Meldungen einzelner ESP-IDF-Komponenten auf Fehler,
// damit die Konsole (USB und Web) die eigentlichen Aktivitäten der Bridge zeigt.
static void configureLogLevels() {
  esp_log_level_set("wifi", ESP_LOG_ERROR);        // WLAN-Treiber: Verbindungsdetails
  esp_log_level_set("uart", ESP_LOG_WARN);         // "queue free spaces"
  esp_log_level_set("httpd_uri", ESP_LOG_ERROR);   // 404 bei Captive-Portal-Anfragen
  esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
  esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
}

extern "C" void app_main() {
  logBufferInit();  // zuerst, damit auch alle Startmeldungen in der Web-Konsole erscheinen
  configureLogLevels();
  diagnosticsBegin();  // Neustartgrund und ggf. Absturzbericht des letzten Laufs
  ESP_LOGI(TAG, "=== Modbus-Bridge GoodWe ET -> Lumel N43 (" CHIP_NAME ") v%s ===", otaRunningVersion());

  initNvs();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  sharedInit();
  otaBegin();
  configLoad();
  checkFactoryResetButton();
  consoleEarlyBoot();  // ggf. angeforderte Bus-Diagnose, solange die UARTs frei sind

  startModbusPorts();
  netBegin();
  timeSyncStart();       // NTP-Abgleich, sobald das WLAN eine IP-Adresse hat
  wireguardStart();      // VPN-Tunnel (falls aktiviert) - baut sich im Hintergrund auf
  startGoodweOverTcp();  // erst nach dem WLAN, da es das Netzwerk braucht
  modbusTcpServerStart();  // Modbus-TCP-Bridge zum GoodWe (falls aktiviert)
  webBegin(rtuPorts);
  mqttStart();
  consoleStart();
  ESP_LOGI(TAG, "Weboberfläche: http://%s.local/  (Login %s)", g_config.hostname,
           g_config.webLoginEnabled ? "aktiv" : "deaktiviert");

  // Supervisor: OTA-Bestätigung und WLAN-Fallback; vom Task-Watchdog überwacht
  esp_task_wdt_add(nullptr);
  for (;;) {
    esp_task_wdt_reset();
    otaLoop();
    netLoop();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

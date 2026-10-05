// WLAN-Verwaltung (esp_wifi/esp_netif): Station mit den gespeicherten Zugangsdaten,
// eigener Access-Point mit Captive Portal als Fallback, mDNS und WLAN-Scan für die Weboberfläche.
#include "net.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "chip.h"
#include "config.h"
#include "dns_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "util.h"

static const char* TAG = "wlan";

// Setzt die maximale WLAN-Sendeleistung aus der Konfiguration (0 = Maximum des Chips, 20 dBm).
// Muss nach esp_wifi_start() und nach jedem Moduswechsel aufgerufen werden.
static void applyWifiTxPowerLimit() {
  int8_t txPower = g_config.wifiMaxTxPower ? g_config.wifiMaxTxPower : 80;
  esp_err_t result = esp_wifi_set_max_tx_power(txPower);
  if (result != ESP_OK) ESP_LOGW(TAG, "Sendeleistung nicht gesetzt: %s", esp_err_to_name(result));
}

void netApplyWifiTxPower() {
  applyWifiTxPowerLimit();
  int8_t appliedPower = 0;
  esp_wifi_get_max_tx_power(&appliedPower);
  ESP_LOGI(TAG, "WLAN-Sendeleistung max. %.2f dBm", appliedPower / 4.0);
}

static const int STATION_CONNECTED_BIT = BIT0;
static const uint32_t STATION_CONNECT_WAIT_MS = 15000;     // beim Start auf die Verbindung warten
static const uint32_t STATION_LOST_FALLBACK_MS = 60000;    // AP einschalten nach 60 s ohne Verbindung
static const uint32_t ACCESS_POINT_IDLE_OFF_MS = 300000;   // AP abschalten 5 min nach erneuter Verbindung
static const uint32_t STATION_RETRY_WITH_AP_MS = 60000;    // bei aktivem AP höchstens jede Minute neu verbinden
static const size_t ACCESS_POINT_MIN_PASSWORD_LENGTH = 8;    // WPA2 verlangt mindestens 8 Zeichen
static const uint8_t ACCESS_POINT_MAX_CLIENTS = 4;
static const uint8_t ACCESS_POINT_CHANNEL = 1;
static const uint16_t SCAN_MAX_RESULTS = 20;

static esp_netif_t* stationInterface = nullptr;
static esp_netif_t* accessPointInterface = nullptr;
static EventGroupHandle_t wifiEventGroup;

static bool accessPointActive = false;
static uint32_t accessPointStartedAtMs = 0;
static uint32_t stationLostSinceMs = 0;   // 0 = Verbindung nicht verloren
static char accessPointSsid[32];
static char stationIp[16] = "";
static volatile bool scanInProgress = false;
static volatile bool stationReconnectPending = false;  // Verbindungsversuch wartet auf netLoop()
static uint32_t lastStationRetryAtMs = 0;

// Ereignis-Handler für WLAN- und IP-Ereignisse (läuft im Event-Loop-Task).
// Verbindet die Station, verfolgt den Verbindungszustand und merkt sich die erhaltene IP.
static void onWifiEvent(void*, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
  if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_START) {
    if (g_config.wifiSsid[0]) esp_wifi_connect();
  } else if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(wifiEventGroup, STATION_CONNECTED_BIT);
    stationIp[0] = 0;
    // erneut verbinden (nicht während eines Scans, der sonst abbricht);
    // bei aktivem Access-Point gedrosselt über netLoop(), damit der AP stabil bleibt
    if (!g_config.wifiSsid[0] || scanInProgress) return;
    if (accessPointActive) stationReconnectPending = true;
    else esp_wifi_connect();
  } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
    auto* gotIpEvent = (ip_event_got_ip_t*)eventData;
    snprintf(stationIp, sizeof(stationIp), IPSTR, IP2STR(&gotIpEvent->ip_info.ip));
    xEventGroupSetBits(wifiEventGroup, STATION_CONNECTED_BIT);
    ESP_LOGI(TAG, "Verbunden mit '%s', IP %s", g_config.wifiSsid, stationIp);
  } else if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_AP_STACONNECTED) {
    ESP_LOGI(TAG, "Client am Access-Point angemeldet");
  }
}

// Schaltet zusätzlich den eigenen Access-Point (Modus AP+STA) samt Captive-Portal-DNS ein.
// Ohne AP-Passwort (bzw. kürzer als 8 Zeichen) ist der AP offen.
static void startAccessPoint() {
  if (accessPointActive) return;
  wifi_config_t accessPointConfig = {};
  copyString((char*)accessPointConfig.ap.ssid, accessPointSsid, sizeof(accessPointConfig.ap.ssid));
  accessPointConfig.ap.ssid_len = strlen(accessPointSsid);
  copyString((char*)accessPointConfig.ap.password, g_config.accessPointPassword,
             sizeof(accessPointConfig.ap.password));
  accessPointConfig.ap.authmode = strlen(g_config.accessPointPassword) >= ACCESS_POINT_MIN_PASSWORD_LENGTH
                                      ? WIFI_AUTH_WPA2_PSK
                                      : WIFI_AUTH_OPEN;
  accessPointConfig.ap.max_connection = ACCESS_POINT_MAX_CLIENTS;
  accessPointConfig.ap.channel = ACCESS_POINT_CHANNEL;
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  esp_wifi_set_config(WIFI_IF_AP, &accessPointConfig);
  applyWifiTxPowerLimit();

  esp_netif_ip_info_t accessPointIpInfo;
  esp_netif_get_ip_info(accessPointInterface, &accessPointIpInfo);
  dnsServerStart(accessPointIpInfo.ip.addr);
  accessPointActive = true;
  accessPointStartedAtMs = millisSinceBoot();
  ESP_LOGI(TAG, "Access-Point '%s' aktiv, IP " IPSTR, accessPointSsid, IP2STR(&accessPointIpInfo.ip));
}

// Beendet Access-Point und DNS-Server; die Station bleibt aktiv
static void stopAccessPoint() {
  if (!accessPointActive) return;
  dnsServerStop();
  esp_wifi_set_mode(WIFI_MODE_STA);
  accessPointActive = false;
  ESP_LOGI(TAG, "Access-Point beendet");
}

// Initialisiert WLAN und mDNS. Wartet bis zu 15 s auf die Station-Verbindung;
// gelingt sie nicht (oder ist kein WLAN konfiguriert), startet der Access-Point.
void netBegin() {
  // Netzwerk-Interfaces anlegen
  wifiEventGroup = xEventGroupCreate();
  stationInterface = esp_netif_create_default_wifi_sta();
  accessPointInterface = esp_netif_create_default_wifi_ap();
  esp_netif_set_hostname(stationInterface, g_config.hostname);

  // AP-Name aus den letzten beiden MAC-Bytes, damit mehrere Geräte unterscheidbar sind
  uint8_t macAddress[6];
  esp_read_mac(macAddress, ESP_MAC_WIFI_STA);
  snprintf(accessPointSsid, sizeof(accessPointSsid), "Modbus-Bridge-%02X%02X", macAddress[4], macAddress[5]);

  // WLAN-Treiber starten
  wifi_init_config_t wifiInitConfig = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifiInitConfig));
  esp_wifi_set_storage(WIFI_STORAGE_RAM);  // Zugangsdaten verwaltet die eigene Konfiguration
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifiEvent, nullptr);

  // Station konfigurieren
  wifi_config_t stationConfig = {};
  copyString((char*)stationConfig.sta.ssid, g_config.wifiSsid, sizeof(stationConfig.sta.ssid));
  copyString((char*)stationConfig.sta.password, g_config.wifiPassword, sizeof(stationConfig.sta.password));
  stationConfig.sta.threshold.authmode = g_config.wifiPassword[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
  stationConfig.sta.pmf_cfg.capable = true;
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_config(WIFI_IF_STA, &stationConfig);
  ESP_ERROR_CHECK(esp_wifi_start());
  applyWifiTxPowerLimit();

  // Auf Verbindung warten, sonst Access-Point
  if (g_config.wifiSsid[0]) {
    ESP_LOGI(TAG, "Verbinde mit '%s' ...", g_config.wifiSsid);
    xEventGroupWaitBits(wifiEventGroup, STATION_CONNECTED_BIT, pdFALSE, pdTRUE,
                        pdMS_TO_TICKS(STATION_CONNECT_WAIT_MS));
  }
  if (!netStaConnected()) startAccessPoint();

  // mDNS: Gerät unter <hostname>.local erreichbar machen
  if (mdns_init() == ESP_OK) {
    mdns_hostname_set(g_config.hostname);
    mdns_instance_name_set("Modbus-Bridge");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
  }
}

// Anzahl der am eigenen Access-Point angemeldeten Clients (0, wenn der AP aus ist)
static int accessPointClientCount() {
  wifi_sta_list_t connectedClients;
  if (!accessPointActive || esp_wifi_ap_get_sta_list(&connectedClients) != ESP_OK) return 0;
  return connectedClients.num;
}

// Verbindet die Station bei aktivem Access-Point nur gedrosselt neu: Jeder Versuch sucht alle
// Kanäle nach dem WLAN ab, und da AP und Station sich ein Funkteil teilen, verlässt dabei auch
// der AP seinen Kanal - angemeldete Clients verlieren die Verbindung. Deshalb höchstens jede
// Minute und gar nicht, solange ein Client am AP angemeldet ist.
static void retryStationWhileAccessPointActive(uint32_t nowMs) {
  if (!accessPointActive || !stationReconnectPending || scanInProgress) return;
  if (nowMs - lastStationRetryAtMs < STATION_RETRY_WITH_AP_MS) return;
  if (accessPointClientCount() > 0) return;
  stationReconnectPending = false;
  lastStationRetryAtMs = nowMs;
  esp_wifi_connect();
}

// Zyklische Überwachung: Access-Point nach Verbindungsverlust einschalten
// bzw. nach stabiler Verbindung wieder abschalten, wenn kein Client angemeldet ist.
void netLoop() {
  uint32_t nowMs = millisSinceBoot();
  if (netStaConnected()) {
    stationLostSinceMs = 0;
    // AP 5 Minuten nach erfolgreicher Verbindung abschalten, wenn niemand angemeldet ist
    stationReconnectPending = false;
    if (accessPointActive && nowMs - accessPointStartedAtMs > ACCESS_POINT_IDLE_OFF_MS &&
        accessPointClientCount() == 0)
      stopAccessPoint();
  } else if (g_config.wifiSsid[0]) {
    if (!stationLostSinceMs) stationLostSinceMs = nowMs;
    // Fallback nach 60 s Ausfall
    if (!accessPointActive && nowMs - stationLostSinceMs > STATION_LOST_FALLBACK_MS) startAccessPoint();
    retryStationWhileAccessPointActive(nowMs);
  }
}

// true, solange der eigene Access-Point läuft
bool netApActive() { return accessPointActive; }

// true, wenn die Station verbunden ist und eine IP hat
bool netStaConnected() {
  return wifiEventGroup && (xEventGroupGetBits(wifiEventGroup) & STATION_CONNECTED_BIT);
}

// IP-Adresse der Station als Text ("" ohne Verbindung)
const char* netStaIp() { return stationIp; }

// Trägt den WLAN-Zustand (Station, Access-Point, Hostname, MAC) in statusObject ein
void netStatusToJson(cJSON* statusObject) {
  bool stationConnected = netStaConnected();
  wifi_ap_record_t connectedApInfo;
  int rssi = stationConnected && esp_wifi_sta_get_ap_info(&connectedApInfo) == ESP_OK ? connectedApInfo.rssi : 0;
  cJSON_AddBoolToObject(statusObject, "sta", stationConnected);
  cJSON_AddStringToObject(statusObject, "ssid", g_config.wifiSsid);
  cJSON_AddStringToObject(statusObject, "ip", stationConnected ? stationIp : "");
  cJSON_AddNumberToObject(statusObject, "rssi", rssi);
  int8_t txPower = 0;
  esp_wifi_get_max_tx_power(&txPower);
  cJSON_AddNumberToObject(statusObject, "txPower", txPower / 4.0);
  cJSON_AddBoolToObject(statusObject, "ap", accessPointActive);
  cJSON_AddStringToObject(statusObject, "apSsid", accessPointSsid);

  char accessPointIp[16] = "";
  if (accessPointActive) {
    esp_netif_ip_info_t accessPointIpInfo;
    esp_netif_get_ip_info(accessPointInterface, &accessPointIpInfo);
    snprintf(accessPointIp, sizeof(accessPointIp), IPSTR, IP2STR(&accessPointIpInfo.ip));
  }
  cJSON_AddStringToObject(statusObject, "apIp", accessPointIp);
  cJSON_AddStringToObject(statusObject, "hostname", g_config.hostname);

  uint8_t macAddress[6];
  esp_read_mac(macAddress, ESP_MAC_WIFI_STA);
  char macText[18];
  snprintf(macText, sizeof(macText), "%02X:%02X:%02X:%02X:%02X:%02X", macAddress[0], macAddress[1],
           macAddress[2], macAddress[3], macAddress[4], macAddress[5]);
  cJSON_AddStringToObject(statusObject, "mac", macText);
}

// Synchroner WLAN-Scan (blockiert einige Sekunden). Rückgabe: neues cJSON-Array
// [{ssid, rssi, secure}] ohne versteckte Netze; der Aufrufer gibt es frei.
cJSON* netScan() {
  cJSON* networkList = cJSON_CreateArray();
  scanInProgress = true;
  wifi_scan_config_t scanConfig = {};
  if (esp_wifi_scan_start(&scanConfig, true) == ESP_OK) {
    uint16_t recordCount = SCAN_MAX_RESULTS;
    wifi_ap_record_t* scanRecords = (wifi_ap_record_t*)calloc(recordCount, sizeof(wifi_ap_record_t));
    if (scanRecords && esp_wifi_scan_get_ap_records(&recordCount, scanRecords) == ESP_OK) {
      for (int recordIndex = 0; recordIndex < recordCount; recordIndex++) {
        const wifi_ap_record_t& record = scanRecords[recordIndex];
        if (!record.ssid[0]) continue;  // verstecktes Netz
        cJSON* networkEntry = cJSON_CreateObject();
        cJSON_AddStringToObject(networkEntry, "ssid", (const char*)record.ssid);
        cJSON_AddNumberToObject(networkEntry, "rssi", record.rssi);
        cJSON_AddBoolToObject(networkEntry, "secure", record.authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(networkList, networkEntry);
      }
    }
    free(scanRecords);
  }
  scanInProgress = false;
  // Ein während des Scans unterdrückter Wiederverbindungsversuch wird hier nachgeholt
  // (bei aktivem Access-Point gedrosselt über netLoop())
  if (g_config.wifiSsid[0] && !netStaConnected()) {
    if (accessPointActive) stationReconnectPending = true;
    else esp_wifi_connect();
  }
  return networkList;
}

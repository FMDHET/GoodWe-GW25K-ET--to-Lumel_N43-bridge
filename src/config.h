#pragma once
// Einstellungen des Geräts. Gespeichert als JSON im NVS, siehe config.cpp.
#include <cstddef>
#include <cstdint>
#include <string>
#include "cJSON.h"

enum PortRole : uint8_t { ROLE_OFF = 0, ROLE_GOODWE_MASTER = 1, ROLE_LUMEL_SLAVE = 2 };
enum GoodweDataSource : uint8_t { GOODWE_SOURCE_METER = 0, GOODWE_SOURCE_INVERTER = 1 };
enum GoodweTransport : uint8_t { GOODWE_VIA_RTU = 0, GOODWE_VIA_TCP = 1 };
static const size_t MAX_GOODWE_SENSORS = 256;  // Größe des Bitfelds goodweFastSensorBits

enum MqttTlsMode : uint8_t { TLS_VERIFY_WITH_CA = 0, TLS_VERIFY_WITH_BUNDLE = 1, TLS_VERIFY_NONE = 2 };

// Einstellungen eines RS485-Ports
struct PortConfig {
  uint8_t role;           // PortRole
  uint32_t baudRate;
  char parity;            // 'N', 'E' oder 'O'
  uint8_t stopBits;       // 1 oder 2
  uint8_t slaveAddress;   // Master: Adresse des GoodWe / Slave: eigene Adresse
  int8_t rxPin;           // GPIO an RO des Transceivers
  int8_t txPin;           // GPIO an DI des Transceivers
  int8_t driverEnablePin; // GPIO an DE/!RE, -1 = Modul mit automatischer Richtungsumschaltung
};

struct AppConfig {
  char hostname[32];
  char wifiSsid[33];
  uint8_t wifiMaxTxPower;        // max. WLAN-Sendeleistung in 0,25 dBm (esp_wifi_set_max_tx_power), 0 = Maximum
  char wifiPassword[65];
  char accessPointPassword[65];
  bool webLoginEnabled;
  char webUser[33];
  char webPassword[65];
  PortConfig port[2];  // [0] = RTU1, [1] = RTU2
  // GoodWe
  uint8_t goodweDataSource;      // GoodweDataSource
  uint16_t goodwePollIntervalMs;      // Intervall 1 (schnell): Werte zur Regelung, z. B. Leistungen
  uint32_t goodweSlowPollIntervalMs;  // Intervall 2 (langsam): alle übrigen Register, vollständige Blöcke
  // Zuordnung der Sensoren zu Intervall 1: Bit n = Sensor n aus GOODWE_SENSORS (goodwe_sensors.h).
  // Gespeichert wird die Liste der Sensor-IDs ("gwFast"), damit sie Änderungen der Tabelle übersteht.
  uint8_t goodweFastSensorBits[32];
  uint8_t goodweDisabledDevices;      // Bit n = GoodweDevice n abgeschaltet (Smart-Meter, Batterie 1/2)
  uint16_t goodweTimeoutMs;
  bool goodweInvertSign;         // GoodWe: + = Einspeisung -> Zähler: + = Bezug
  uint8_t goodweTransport;       // GoodweTransport: RS485-Port mit Rolle GoodWe oder Modbus TCP
  char goodweTcpHost[64];        // IP/Hostname des Modbus-TCP-Gateways
  uint16_t goodweTcpPort;
  uint8_t goodweTcpUnitId;       // Unit-ID = RTU-Adresse des GoodWe hinter dem Gateway (247)
  // Modbus-TCP-Bridge: reicht Anfragen aus dem lokalen Netz unverändert an den GoodWe durch
  bool bridgeEnabled;
  uint16_t bridgePort;
  // Lumel
  bool lumelSwapFloatWords;
  bool lumelSilentWhenStale;     // ohne aktuelle GoodWe-Daten nicht antworten
  bool lumelReplaceUndefined;    // statt 1e20 (nicht definiert) PF = 1 und tg φ = 0 ausgeben
  uint16_t staleAfterSec;
  bool testModeEnabled;
  float testPowerWatt;
  float testVoltage;
  // MQTT
  bool mqttEnabled;
  char mqttHost[64];
  uint16_t mqttPort;
  char mqttUser[65];
  char mqttPassword[65];
  char mqttBaseTopic[48];
  uint16_t mqttIntervalSec;
  bool mqttSingleTopics;         // zusätzlich jeden Wert als eigenes Topic
  bool mqttDiscoveryEnabled;     // Home Assistant Auto-Discovery
  char mqttDiscoveryPrefix[32];
  bool mqttTlsEnabled;
  uint8_t mqttTlsMode;           // MqttTlsMode
  // WireGuard-VPN (Werte wie in einer wg-quick-Konfiguration)
  bool wgEnabled;
  char wgPrivateKey[48];         // [Interface] PrivateKey (Base64)
  char wgAddress[24];            // [Interface] Address, z. B. 192.168.178.201/24
  char wgPeerPublicKey[48];      // [Peer] PublicKey
  char wgPresharedKey[48];       // [Peer] PresharedKey (optional)
  char wgEndpoint[64];           // [Peer] Endpoint-Host, z. B. xyz.myfritz.net
  uint16_t wgEndpointPort;       // [Peer] Endpoint-Port
  char wgAllowedIps[128];        // [Peer] AllowedIPs, z. B. 192.168.178.0/24
  uint16_t wgKeepaliveSec;       // [Peer] PersistentKeepalive
  // Uhrzeit
  char ntpServer1[64];
  char ntpServer2[64];
  char timeZone[48];             // POSIX-TZ, z. B. CET-1CEST,M3.5.0,M10.5.0/3
};

// Die aktuell gültige Konfiguration (wird beim Start aus dem NVS geladen)
extern AppConfig g_config;

// Setzt alle Einstellungen in `config` auf die Werkseinstellungen.
void configDefaults(AppConfig& config);
// true, wenn der Sensor mit Index sensorIndex (GOODWE_SENSORS) Intervall 1 (schnell) zugeordnet ist
bool isGoodweSensorFast(const AppConfig& config, size_t sensorIndex);

// Lädt die Konfiguration aus dem NVS nach g_config. Fehlt sie oder ist sie beschädigt,
// gelten die Werkseinstellungen.
void configLoad();

// Speichert g_config als JSON im NVS. Liefert false, wenn das Schreiben fehlschlägt.
bool configSave();

// Löscht Konfiguration und Zertifikate im NVS und setzt g_config auf Werkseinstellungen.
void configFactoryReset();

// Wandelt `config` in ein JSON-Objekt um (Aufrufer muss es mit cJSON_Delete freigeben).
// withSecrets = true: Passwörter im Klartext mit ausgeben (für NVS, Backup und Weboberfläche).
cJSON* configToJson(const AppConfig& config, bool withSecrets);

// Übernimmt die im JSON enthaltenen Felder in `config` und prüft sie auf Gültigkeit.
// Nicht enthaltene Felder bleiben unverändert, leere Passwortfelder ebenfalls.
// Bei einem Fehler bleibt `config` unverändert, errorMessage enthält den Grund.
bool configApplyJson(AppConfig& config, const cJSON* json, std::string& errorMessage);

// Prüft, ob ein GPIO für RS485 verwendet werden darf (nicht USB, nicht Flash).
bool isGpioAllowed(int gpioNumber);
extern const int8_t* const ALLOWED_GPIOS;
extern const size_t ALLOWED_GPIO_COUNT;

// Lädt ein MQTT-TLS-Zertifikat (PEM) aus dem NVS-Namespace "certs".
// name: "ca" (Zertifizierungsstelle), "crt" (Client-Zertifikat) oder "key" (privater Schlüssel).
// Liefert einen leeren Text, wenn keines gespeichert ist.
std::string certificateLoad(const char* name);

// Speichert ein Zertifikat (PEM) im NVS. Ein leerer Text löscht es.
bool certificateStore(const char* name, const std::string& pem);

// Liest einen Text aus dem NVS (leer, wenn nicht vorhanden).
std::string nvsReadString(const char* nvsNamespace, const char* key);

// Schreibt einen Text dauerhaft ins NVS.
bool nvsWriteString(const char* nvsNamespace, const char* key, const char* value);

// Geräte-Konfiguration: Standardwerte, Umwandlung von/nach JSON mit Validierung,
// Laden/Speichern im NVS sowie Ablage der MQTT-TLS-Zertifikate.
//
// Konfiguration als JSON im NVS (Namespace "cfg", Schlüssel "json").
// Die NVS-Partition liegt getrennt von den App-Partitionen und bleibt bei OTA-Updates erhalten.
// Neue Felder einer neueren Firmware erhalten ihren Standardwert, bestehende bleiben erhalten.
//
// ACHTUNG: Die JSON-Schlüssel ("gwPollMs", "mqttBase", ...) sind das Speicherformat im NVS und
// die Web-API. Sie dürfen nicht umbenannt werden, sonst gehen gespeicherte Einstellungen verloren.
#include "config.h"
#include "chip.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "esp_log.h"
#include "goodwe_sensors.h"
#include "nvs.h"
#include "util.h"

static const char* TAG = "cfg";

static const char* CONFIG_NAMESPACE = "cfg";
static const char* CONFIG_KEY = "json";
static const char* CERTIFICATE_NAMESPACE = "certs";

AppConfig g_config;

// Für RS485 wählbare GPIOs des Zielchips (siehe chip.h)
const int8_t* const ALLOWED_GPIOS = CHIP_ALLOWED_GPIOS;
const size_t ALLOWED_GPIO_COUNT = sizeof(CHIP_ALLOWED_GPIOS) / sizeof(CHIP_ALLOWED_GPIOS[0]);

// Prüft, ob gpioNumber für RS485-Pins verwendet werden darf (true = in ALLOWED_GPIOS enthalten).
// Ausgenommen sind u. a. die USB-Pins der Konsole und die Flash-Pins (je nach Chip, siehe chip.h).
bool isGpioAllowed(int gpioNumber) {
  for (int8_t allowedGpio : CHIP_ALLOWED_GPIOS)
    if (allowedGpio == gpioNumber) return true;
  return false;
}

// Setzt config vollständig auf die Werkseinstellungen zurück.
bool isGoodweSensorFast(const AppConfig& config, size_t sensorIndex) {
  return sensorIndex < MAX_GOODWE_SENSORS && (config.goodweFastSensorBits[sensorIndex / 8] & (1 << (sensorIndex % 8)));
}

// Ordnet den Sensor mit der ID sensorId Intervall 1 (fast = true) oder Intervall 2 zu.
// Unbekannte IDs (z. B. aus einer älteren Firmware) werden ignoriert.
static void setGoodweSensorFast(AppConfig& config, const char* sensorId, bool fast) {
  int sensorIndex = goodweSensorIndexById(sensorId);
  if (sensorIndex < 0 || sensorIndex >= (int)MAX_GOODWE_SENSORS) return;
  uint8_t bitMask = 1 << (sensorIndex % 8);
  if (fast) config.goodweFastSensorBits[sensorIndex / 8] |= bitMask;
  else config.goodweFastSensorBits[sensorIndex / 8] &= ~bitMask;
}

void configDefaults(AppConfig& config) {
  memset(&config, 0, sizeof(config));
  copyString(config.hostname, "modbus-bridge", sizeof(config.hostname));
  copyString(config.accessPointPassword, "modbus1234", sizeof(config.accessPointPassword));
  config.webLoginEnabled = false;
  copyString(config.webUser, "admin", sizeof(config.webUser));
  copyString(config.webPassword, "admin", sizeof(config.webPassword));

  // Reihenfolge: role, baudRate, parity, stopBits, slaveAddress, rxPin, txPin, driverEnablePin
  // RTU1: GoodWe ET auslesen (Default 9600 8N1, Adresse 247); Pins je nach Chip (chip.h)
  config.port[0] = {ROLE_GOODWE_MASTER, 9600, 'N', 1, 247, CHIP_DEFAULT_PINS_RTU1[0], CHIP_DEFAULT_PINS_RTU1[1],
                    CHIP_DEFAULT_PINS_RTU1[2]};
  // RTU2: Lumel N43 simulieren (Default 9600 8N2, Adresse 1)
  config.port[1] = {ROLE_LUMEL_SLAVE, 9600, 'N', 2, 1, CHIP_DEFAULT_PINS_RTU2[0], CHIP_DEFAULT_PINS_RTU2[1],
                    CHIP_DEFAULT_PINS_RTU2[2]};

  config.goodweDataSource = GOODWE_SOURCE_METER;
  config.wifiMaxTxPower = CHIP_WIFI_MAX_TX_POWER;  // je nach Chip, siehe chip.h
  config.goodwePollIntervalMs = 1000;
  config.goodweSlowPollIntervalMs = 10000;
  // Intervall 1 ab Werk: Leistungen und Ströme, die sich laufend ändern bzw. zur Regelung dienen
  static const char* const DEFAULT_FAST_SENSOR_IDS[] = {
      "ppv1", "ppv2", "ppv3", "ppv4", "pgrid1", "pgrid2", "pgrid3", "total_inverter_power", "active_power",
      "load_p1", "load_p2", "load_p3", "load_ptotal", "ibattery1", "pbattery1", "ibattery2", "pbattery2",
      "meter_p1", "meter_p2", "meter_p3", "meter_p", "meter_q1", "meter_q2", "meter_q3", "meter_q",
      "meter_s1", "meter_s2", "meter_s3", "meter_s", "meter_i1", "meter_i2", "meter_i3"};
  for (const char* sensorId : DEFAULT_FAST_SENSOR_IDS) setGoodweSensorFast(config, sensorId, true);
  config.goodweTimeoutMs = 500;
  config.goodweInvertSign = true;
  config.goodweTransport = GOODWE_VIA_RTU;
  config.goodweTcpPort = 502;
  config.goodweTcpUnitId = 247;
  config.bridgeEnabled = false;
  config.bridgePort = 502;
  config.wgEnabled = false;
  config.wgEndpointPort = 51820;
  config.wgKeepaliveSec = 25;
  copyString(config.ntpServer1, "pool.ntp.org", sizeof(config.ntpServer1));
  copyString(config.ntpServer2, "time.cloudflare.com", sizeof(config.ntpServer2));
  copyString(config.timeZone, "CET-1CEST,M3.5.0,M10.5.0/3", sizeof(config.timeZone));
  config.lumelSwapFloatWords = false;
  config.lumelSilentWhenStale = true;
  config.staleAfterSec = 10;
  config.testModeEnabled = false;
  config.testPowerWatt = 1500;
  config.testVoltage = 230;

  config.mqttEnabled = false;
  config.mqttPort = 1883;
  copyString(config.mqttBaseTopic, "modbus-bridge", sizeof(config.mqttBaseTopic));
  config.mqttIntervalSec = 10;
  config.mqttSingleTopics = false;
  config.mqttDiscoveryEnabled = true;
  copyString(config.mqttDiscoveryPrefix, "homeassistant", sizeof(config.mqttDiscoveryPrefix));
  config.mqttTlsEnabled = false;
  config.mqttTlsMode = TLS_VERIFY_WITH_CA;
}

// ---------------------------------------------------------------- JSON schreiben

// Wandelt eine PortRole in ihren JSON-Namen ("goodwe", "lumel", "off") um.
static const char* roleToName(uint8_t role) {
  switch (role) {
    case ROLE_GOODWE_MASTER: return "goodwe";
    case ROLE_LUMEL_SLAVE: return "lumel";
    default: return "off";
  }
}

// Liefert die PortRole oder -1 bei unbekanntem Namen
static int roleFromName(const char* name) {
  if (!name) return -1;
  if (!strcmp(name, "goodwe")) return ROLE_GOODWE_MASTER;
  if (!strcmp(name, "lumel")) return ROLE_LUMEL_SLAVE;
  if (!strcmp(name, "off")) return ROLE_OFF;
  return -1;
}

// Wandelt einen MqttTlsMode in seinen JSON-Namen ("ca", "bundle", "none") um.
static const char* tlsModeToName(uint8_t tlsMode) {
  if (tlsMode == TLS_VERIFY_WITH_BUNDLE) return "bundle";
  if (tlsMode == TLS_VERIFY_NONE) return "none";
  return "ca";
}

// Erzeugt das JSON-Objekt eines RS485-Ports. Der Aufrufer übernimmt den Speicher.
static cJSON* portConfigToJson(const PortConfig& portConfig) {
  cJSON* portObject = cJSON_CreateObject();
  char parityText[2] = {portConfig.parity, 0};
  cJSON_AddStringToObject(portObject, "role", roleToName(portConfig.role));
  cJSON_AddNumberToObject(portObject, "baud", portConfig.baudRate);
  cJSON_AddStringToObject(portObject, "parity", parityText);
  cJSON_AddNumberToObject(portObject, "stopBits", portConfig.stopBits);
  cJSON_AddNumberToObject(portObject, "addr", portConfig.slaveAddress);
  cJSON_AddNumberToObject(portObject, "rx", portConfig.rxPin);
  cJSON_AddNumberToObject(portObject, "tx", portConfig.txPin);
  cJSON_AddNumberToObject(portObject, "de", portConfig.driverEnablePin);
  return portObject;
}

// Serialisiert die komplette Konfiguration. Passwörter nur bei withSecrets = true
// (zum Speichern im NVS); sonst nur die Information, ob sie gesetzt sind.
// Rückgabe: neues cJSON-Objekt, der Aufrufer gibt es frei.
cJSON* configToJson(const AppConfig& config, bool withSecrets) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "hostname", config.hostname);
  cJSON_AddStringToObject(root, "wifiSsid", config.wifiSsid);
  cJSON_AddBoolToObject(root, "wifiPassSet", config.wifiPassword[0] != 0);
  cJSON_AddBoolToObject(root, "webAuth", config.webLoginEnabled);
  cJSON_AddStringToObject(root, "webUser", config.webUser);
  if (withSecrets) {
    cJSON_AddStringToObject(root, "wifiPass", config.wifiPassword);
    cJSON_AddStringToObject(root, "apPass", config.accessPointPassword);
    cJSON_AddStringToObject(root, "webPass", config.webPassword);
    cJSON_AddStringToObject(root, "mqttPass", config.mqttPassword);
  }
  cJSON* portArray = cJSON_AddArrayToObject(root, "ports");
  for (int portIndex = 0; portIndex < 2; portIndex++)
    cJSON_AddItemToArray(portArray, portConfigToJson(config.port[portIndex]));

  cJSON_AddStringToObject(root, "gwSource", config.goodweDataSource == GOODWE_SOURCE_METER ? "meter" : "inverter");
  // Sendeleistung in dBm (0 = Maximum des Chips)
  cJSON_AddNumberToObject(root, "wifiTxPower", config.wifiMaxTxPower / 4.0);
  cJSON_AddNumberToObject(root, "gwPollMs", config.goodwePollIntervalMs);
  cJSON_AddNumberToObject(root, "gwSlowPollMs", config.goodweSlowPollIntervalMs);
  cJSON* disabledDeviceArray = cJSON_AddArrayToObject(root, "gwDisabled");
  for (uint8_t device = 0; device < GOODWE_DEVICE_COUNT; device++)
    if (config.goodweDisabledDevices & (1 << device))
      cJSON_AddItemToArray(disabledDeviceArray, cJSON_CreateString(goodweDeviceKey(device)));
  cJSON* fastSensorArray = cJSON_AddArrayToObject(root, "gwFast");
  for (size_t sensorIndex = 0; sensorIndex < GOODWE_SENSOR_COUNT; sensorIndex++)
    if (isGoodweSensorFast(config, sensorIndex))
      cJSON_AddItemToArray(fastSensorArray, cJSON_CreateString(GOODWE_SENSORS[sensorIndex].id));
  cJSON_AddNumberToObject(root, "gwTimeoutMs", config.goodweTimeoutMs);
  cJSON_AddBoolToObject(root, "gwInvertSign", config.goodweInvertSign);
  cJSON_AddStringToObject(root, "gwTransport", config.goodweTransport == GOODWE_VIA_TCP ? "tcp" : "rtu");
  cJSON_AddStringToObject(root, "gwTcpHost", config.goodweTcpHost);
  cJSON_AddNumberToObject(root, "gwTcpPort", config.goodweTcpPort);
  cJSON_AddNumberToObject(root, "gwTcpUnit", config.goodweTcpUnitId);
  cJSON_AddBoolToObject(root, "bridgeEnabled", config.bridgeEnabled);
  cJSON_AddNumberToObject(root, "bridgePort", config.bridgePort);
  cJSON_AddStringToObject(root, "ntpServer1", config.ntpServer1);
  cJSON_AddStringToObject(root, "ntpServer2", config.ntpServer2);
  cJSON_AddStringToObject(root, "timeZone", config.timeZone);
  cJSON_AddBoolToObject(root, "wgEnabled", config.wgEnabled);
  cJSON_AddStringToObject(root, "wgAddress", config.wgAddress);
  cJSON_AddStringToObject(root, "wgPeerPublicKey", config.wgPeerPublicKey);
  cJSON_AddStringToObject(root, "wgEndpoint", config.wgEndpoint);
  cJSON_AddNumberToObject(root, "wgPort", config.wgEndpointPort);
  cJSON_AddStringToObject(root, "wgAllowedIps", config.wgAllowedIps);
  cJSON_AddNumberToObject(root, "wgKeepalive", config.wgKeepaliveSec);
  if (withSecrets) {
    cJSON_AddStringToObject(root, "wgPrivateKey", config.wgPrivateKey);
    cJSON_AddStringToObject(root, "wgPresharedKey", config.wgPresharedKey);
  }
  cJSON_AddBoolToObject(root, "lumelWordSwap", config.lumelSwapFloatWords);
  cJSON_AddBoolToObject(root, "lumelSilentOnStale", config.lumelSilentWhenStale);
  cJSON_AddBoolToObject(root, "lumelNoUndefined", config.lumelReplaceUndefined);
  cJSON_AddNumberToObject(root, "staleSec", config.staleAfterSec);
  cJSON_AddBoolToObject(root, "testMode", config.testModeEnabled);
  cJSON_AddNumberToObject(root, "testPowerW", config.testPowerWatt);
  cJSON_AddNumberToObject(root, "testVoltage", config.testVoltage);
  cJSON_AddBoolToObject(root, "mqttEnabled", config.mqttEnabled);
  cJSON_AddStringToObject(root, "mqttHost", config.mqttHost);
  cJSON_AddNumberToObject(root, "mqttPort", config.mqttPort);
  cJSON_AddStringToObject(root, "mqttUser", config.mqttUser);
  cJSON_AddBoolToObject(root, "mqttPassSet", config.mqttPassword[0] != 0);
  cJSON_AddStringToObject(root, "mqttBase", config.mqttBaseTopic);
  cJSON_AddNumberToObject(root, "mqttIntervalSec", config.mqttIntervalSec);
  cJSON_AddBoolToObject(root, "mqttSingleTopics", config.mqttSingleTopics);
  cJSON_AddBoolToObject(root, "mqttDiscovery", config.mqttDiscoveryEnabled);
  cJSON_AddStringToObject(root, "mqttDiscPrefix", config.mqttDiscoveryPrefix);
  cJSON_AddBoolToObject(root, "mqttTls", config.mqttTlsEnabled);
  cJSON_AddStringToObject(root, "mqttTlsMode", tlsModeToName(config.mqttTlsMode));
  return root;
}

// ---------------------------------------------------------------- JSON lesen (mit Validierung)

// Text übernehmen. Fehlender Schlüssel oder null = unverändert.
// displayName erscheint in der Fehlermeldung.
static bool readString(const cJSON* json, const char* key, char* destination, size_t destinationSize,
                       size_t minLength, const char* displayName, std::string& errorMessage) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(json, key);
  if (!item || cJSON_IsNull(item)) return true;
  if (!cJSON_IsString(item)) {
    errorMessage = std::string(displayName) + ": Text erwartet";
    return false;
  }
  size_t length = strlen(item->valuestring);
  if (length >= destinationSize) {
    errorMessage = std::string(displayName) + ": zu lang";
    return false;
  }
  if (length < minLength) {
    errorMessage = std::string(displayName) + ": mindestens " + std::to_string(minLength) + " Zeichen";
    return false;
  }
  copyString(destination, item->valuestring, destinationSize);
  return true;
}

// Passwortfelder: fehlend oder leer = unverändert (die Weboberfläche schickt Passwörter nie zurück)
static bool readSecret(const cJSON* json, const char* key, char* destination, size_t destinationSize,
                       size_t minLength, const char* displayName, std::string& errorMessage) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(json, key);
  if (!cJSON_IsString(item) || !item->valuestring[0]) return true;
  return readString(json, key, destination, destinationSize, minLength, displayName, errorMessage);
}

// Zahl übernehmen und auf [minimum, maximum] begrenzen; fehlt sie, bleibt der Wert unverändert
template <typename NumberType>
static void readClampedNumber(const cJSON* json, const char* key, NumberType& destination, double minimum,
                              double maximum) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(json, key);
  if (!cJSON_IsNumber(item)) return;
  double value = item->valuedouble;
  if (value < minimum) value = minimum;
  if (value > maximum) value = maximum;
  destination = (NumberType)value;
}

// Bool übernehmen; fehlt der Schlüssel oder ist er kein Bool, bleibt destination unverändert.
static void readBool(const cJSON* json, const char* key, bool& destination) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(json, key);
  if (cJSON_IsBool(item)) destination = cJSON_IsTrue(item);
}

// true nur, wenn der Schlüssel vorhanden und true ist (z. B. "wifiPassClear")
static bool isFlagSet(const cJSON* json, const char* key) {
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, key));
}

// true, wenn baudRate eine der unterstützten Standard-Baudraten ist.
static bool isSupportedBaudRate(uint32_t baudRate) {
  static const uint32_t SUPPORTED_BAUD_RATES[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
  for (uint32_t supportedBaudRate : SUPPORTED_BAUD_RATES)
    if (supportedBaudRate == baudRate) return true;
  return false;
}

// Übernimmt die Einstellungen eines Ports aus portJson. portIndex nur für Fehlermeldungen.
// Rückgabe false mit errorMessage bei ungültigem Wert; portConfig ist dann evtl. teilweise geändert
// (unkritisch, da configApplyJson auf einer Kopie arbeitet).
static bool applyPortJson(PortConfig& portConfig, const cJSON* portJson, int portIndex, std::string& errorMessage) {
  const std::string errorPrefix = "RTU" + std::to_string(portIndex + 1) + ": ";
  const cJSON* item;

  if ((item = cJSON_GetObjectItemCaseSensitive(portJson, "role")) && cJSON_IsString(item)) {
    int role = roleFromName(item->valuestring);
    if (role < 0) {
      errorMessage = errorPrefix + "unbekannte Rolle";
      return false;
    }
    portConfig.role = role;
  }
  if (cJSON_IsNumber(item = cJSON_GetObjectItemCaseSensitive(portJson, "baud"))) {
    uint32_t baudRate = (uint32_t)item->valuedouble;
    if (!isSupportedBaudRate(baudRate)) {
      errorMessage = errorPrefix + "ungültige Baudrate";
      return false;
    }
    portConfig.baudRate = baudRate;
  }
  if (cJSON_IsString(item = cJSON_GetObjectItemCaseSensitive(portJson, "parity"))) {
    char parity = item->valuestring[0];
    if (parity != 'N' && parity != 'E' && parity != 'O') {
      errorMessage = errorPrefix + "Parität N/E/O";
      return false;
    }
    portConfig.parity = parity;
  }
  if (cJSON_IsNumber(item = cJSON_GetObjectItemCaseSensitive(portJson, "stopBits"))) {
    int stopBits = item->valueint;
    if (stopBits != 1 && stopBits != 2) {
      errorMessage = errorPrefix + "Stopbits 1 oder 2";
      return false;
    }
    portConfig.stopBits = stopBits;
  }
  if (cJSON_IsNumber(item = cJSON_GetObjectItemCaseSensitive(portJson, "addr"))) {
    int slaveAddress = item->valueint;
    if (slaveAddress < 1 || slaveAddress > 247) {
      errorMessage = errorPrefix + "Adresse 1..247";
      return false;
    }
    portConfig.slaveAddress = slaveAddress;
  }

  // Pins: nur erlaubte GPIOs; der DE-Pin darf zusätzlich -1 sein (automatische Richtungsumschaltung)
  struct PinField {
    const char* key;
    int8_t* destination;
    bool mayBeDisabled;
  };
  const PinField pinFields[3] = {{"rx", &portConfig.rxPin, false},
                                 {"tx", &portConfig.txPin, false},
                                 {"de", &portConfig.driverEnablePin, true}};
  for (const PinField& pinField : pinFields) {
    if (!cJSON_IsNumber(item = cJSON_GetObjectItemCaseSensitive(portJson, pinField.key))) continue;
    int gpioNumber = item->valueint;
    if (!(isGpioAllowed(gpioNumber) || (pinField.mayBeDisabled && gpioNumber == -1))) {
      errorMessage = errorPrefix + "GPIO " + std::to_string(gpioNumber) + " für " + pinField.key + " nicht erlaubt";
      return false;
    }
    *pinField.destination = gpioNumber;
  }
  return true;
}

// Kein GPIO darf von zwei aktiven Ports (oder doppelt im selben Port) benutzt werden
static bool checkGpioConflicts(const AppConfig& config, std::string& errorMessage) {
  int usedGpios[6];
  int usedGpioCount = 0;
  for (int portIndex = 0; portIndex < 2; portIndex++) {
    const PortConfig& portConfig = config.port[portIndex];
    if (portConfig.role == ROLE_OFF) continue;
    for (int gpioNumber : {(int)portConfig.rxPin, (int)portConfig.txPin, (int)portConfig.driverEnablePin}) {
      if (gpioNumber < 0) continue;
      for (int usedIndex = 0; usedIndex < usedGpioCount; usedIndex++) {
        if (usedGpios[usedIndex] == gpioNumber) {
          errorMessage = "GPIO " + std::to_string(gpioNumber) + " ist doppelt belegt";
          return false;
        }
      }
      usedGpios[usedGpioCount++] = gpioNumber;
    }
  }
  return true;
}

// Es gibt nur einen GoodWe-Master- und einen Lumel-Slave-Task
static bool checkEachRoleUsedOnce(const AppConfig& config, std::string& errorMessage) {
  int goodwePortCount = 0;
  int lumelPortCount = 0;
  for (int portIndex = 0; portIndex < 2; portIndex++) {
    goodwePortCount += config.port[portIndex].role == ROLE_GOODWE_MASTER;
    lumelPortCount += config.port[portIndex].role == ROLE_LUMEL_SLAVE;
  }
  if (goodwePortCount > 1 || lumelPortCount > 1) {
    errorMessage = "Jede Rolle darf nur einem Port zugeordnet sein";
    return false;
  }
  return true;
}

// Übernimmt den TLS-Prüfmodus aus "mqttTlsMode" bzw. dem Altschlüssel "mqttTlsInsecure".
static void applyTlsModeJson(AppConfig& config, const cJSON* json) {
  const cJSON* tlsModeItem = cJSON_GetObjectItemCaseSensitive(json, "mqttTlsMode");
  if (cJSON_IsString(tlsModeItem)) {
    if (!strcmp(tlsModeItem->valuestring, "bundle")) config.mqttTlsMode = TLS_VERIFY_WITH_BUNDLE;
    else if (!strcmp(tlsModeItem->valuestring, "none")) config.mqttTlsMode = TLS_VERIFY_NONE;
    else config.mqttTlsMode = TLS_VERIFY_WITH_CA;
  }
  // Kompatibilität mit Firmware 1.x: dort gab es nur den Schalter "mqttTlsInsecure"
  const cJSON* tlsInsecureItem = cJSON_GetObjectItemCaseSensitive(json, "mqttTlsInsecure");
  if (cJSON_IsBool(tlsInsecureItem) && !cJSON_IsString(tlsModeItem))
    config.mqttTlsMode = cJSON_IsTrue(tlsInsecureItem) ? TLS_VERIFY_NONE : TLS_VERIFY_WITH_CA;
}

// Abschließende '/' entfernen und MQTT-Wildcards/Leerzeichen ablehnen
static bool normalizeMqttTopics(AppConfig& config, std::string& errorMessage) {
  for (char* topic : {config.mqttBaseTopic, config.mqttDiscoveryPrefix}) {
    size_t length = strlen(topic);
    while (length && topic[length - 1] == '/') topic[--length] = 0;
    if (strpbrk(topic, "#+ ")) {
      errorMessage = "MQTT-Topics dürfen kein #, + oder Leerzeichen enthalten";
      return false;
    }
  }
  return true;
}

// Übernimmt eine (Teil-)Konfiguration aus json in config. Nur vorhandene Schlüssel werden geändert.
// Rückgabe false mit errorMessage, wenn ein Wert ungültig ist; config bleibt dann unverändert.
bool configApplyJson(AppConfig& config, const cJSON* json, std::string& errorMessage) {
  if (!cJSON_IsObject(json)) {
    errorMessage = "JSON-Objekt erwartet";
    return false;
  }
  // Auf einer Kopie arbeiten: bei einem Fehler bleibt die übergebene Konfiguration unverändert
  AppConfig updated = config;

  // Allgemein / WLAN / Web
  if (!readString(json, "hostname", updated.hostname, sizeof(updated.hostname), 1, "Hostname", errorMessage))
    return false;
  if (!readString(json, "wifiSsid", updated.wifiSsid, sizeof(updated.wifiSsid), 0, "SSID", errorMessage))
    return false;
  if (!readSecret(json, "wifiPass", updated.wifiPassword, sizeof(updated.wifiPassword), 8, "WLAN-Passwort",
                  errorMessage))
    return false;
  if (isFlagSet(json, "wifiPassClear")) updated.wifiPassword[0] = 0;
  if (!readSecret(json, "apPass", updated.accessPointPassword, sizeof(updated.accessPointPassword), 8,
                  "AP-Passwort", errorMessage))
    return false;
  readBool(json, "webAuth", updated.webLoginEnabled);
  if (!readString(json, "webUser", updated.webUser, sizeof(updated.webUser), 1, "Benutzer", errorMessage))
    return false;
  if (!readSecret(json, "webPass", updated.webPassword, sizeof(updated.webPassword), 4, "Web-Passwort",
                  errorMessage))
    return false;

  // RS485-Ports
  const cJSON* portArray = cJSON_GetObjectItemCaseSensitive(json, "ports");
  for (int portIndex = 0; portIndex < 2 && cJSON_IsArray(portArray) && portIndex < cJSON_GetArraySize(portArray);
       portIndex++) {
    if (!applyPortJson(updated.port[portIndex], cJSON_GetArrayItem(portArray, portIndex), portIndex, errorMessage))
      return false;
  }
  if (!checkGpioConflicts(updated, errorMessage)) return false;
  if (!checkEachRoleUsedOnce(updated, errorMessage)) return false;

  // GoodWe / Lumel
  const cJSON* sourceItem = cJSON_GetObjectItemCaseSensitive(json, "gwSource");
  if (cJSON_IsString(sourceItem))
    updated.goodweDataSource =
        strcmp(sourceItem->valuestring, "inverter") ? GOODWE_SOURCE_METER : GOODWE_SOURCE_INVERTER;
  const cJSON* txPowerItem = cJSON_GetObjectItemCaseSensitive(json, "wifiTxPower");
  if (cJSON_IsNumber(txPowerItem)) {
    double txPowerDbm = txPowerItem->valuedouble;
    // 0 = Maximum; sonst 2 ... 20 dBm in 0,25-dBm-Schritten
    updated.wifiMaxTxPower = txPowerDbm <= 0 ? 0 : (uint8_t)(fmin(fmax(txPowerDbm, 2.0), 20.0) * 4 + 0.5);
  }
  readClampedNumber(json, "gwPollMs", updated.goodwePollIntervalMs, 100, 60000);
  readClampedNumber(json, "gwSlowPollMs", updated.goodweSlowPollIntervalMs, 1000, 3600000);
  // Abgeschaltete Geräte: vollständige Liste der Schlüssel ersetzt die bisherige Auswahl.
  // Die Geräteinfo lässt sich nicht abschalten, das Gerät der Datenquelle (s. u.) ebenso nicht.
  const cJSON* disabledDeviceArray = cJSON_GetObjectItemCaseSensitive(json, "gwDisabled");
  if (cJSON_IsArray(disabledDeviceArray)) {
    updated.goodweDisabledDevices = 0;
    const cJSON* deviceKeyItem;
    cJSON_ArrayForEach(deviceKeyItem, disabledDeviceArray) {
      for (uint8_t device = DEVICE_INVERTER; device < GOODWE_DEVICE_COUNT && cJSON_IsString(deviceKeyItem); device++)
        if (!strcmp(deviceKeyItem->valuestring, goodweDeviceKey(device))) updated.goodweDisabledDevices |= 1 << device;
    }
  }
  if (updated.goodweDataSource == GOODWE_SOURCE_METER && (updated.goodweDisabledDevices & (1 << DEVICE_METER))) {
    errorMessage = "Smart-Meter ist die Datenquelle und kann nicht abgeschaltet werden";
    return false;
  }
  if (updated.goodweDataSource == GOODWE_SOURCE_INVERTER && (updated.goodweDisabledDevices & (1 << DEVICE_INVERTER))) {
    errorMessage = "Wechselrichter ist die Datenquelle und kann nicht abgeschaltet werden";
    return false;
  }
  // Zuordnung zu Intervall 1: vollständige Liste der Sensor-IDs ersetzt die bisherige Auswahl
  const cJSON* fastSensorArray = cJSON_GetObjectItemCaseSensitive(json, "gwFast");
  if (cJSON_IsArray(fastSensorArray)) {
    memset(updated.goodweFastSensorBits, 0, sizeof(updated.goodweFastSensorBits));
    const cJSON* sensorIdItem;
    cJSON_ArrayForEach(sensorIdItem, fastSensorArray)
      if (cJSON_IsString(sensorIdItem)) setGoodweSensorFast(updated, sensorIdItem->valuestring, true);
  }
  readClampedNumber(json, "gwTimeoutMs", updated.goodweTimeoutMs, 50, 5000);
  readBool(json, "gwInvertSign", updated.goodweInvertSign);

  // --- Uhrzeit ---
  if (!readString(json, "ntpServer1", updated.ntpServer1, sizeof(updated.ntpServer1), 1, "NTP-Server 1", errorMessage) ||
      !readString(json, "ntpServer2", updated.ntpServer2, sizeof(updated.ntpServer2), 0, "NTP-Server 2", errorMessage) ||
      !readString(json, "timeZone", updated.timeZone, sizeof(updated.timeZone), 3, "Zeitzone", errorMessage))
    return false;

  // --- WireGuard-VPN ---
  readBool(json, "wgEnabled", updated.wgEnabled);
  if (!readSecret(json, "wgPrivateKey", updated.wgPrivateKey, sizeof(updated.wgPrivateKey), 44,
                  "WireGuard Private Key", errorMessage))
    return false;
  if (isFlagSet(json, "wgPresharedKeyClear")) updated.wgPresharedKey[0] = 0;
  if (!readSecret(json, "wgPresharedKey", updated.wgPresharedKey, sizeof(updated.wgPresharedKey), 44,
                  "WireGuard Preshared Key", errorMessage))
    return false;
  if (!readString(json, "wgAddress", updated.wgAddress, sizeof(updated.wgAddress), 0, "Tunnel-Adresse",
                  errorMessage) ||
      !readString(json, "wgPeerPublicKey", updated.wgPeerPublicKey, sizeof(updated.wgPeerPublicKey), 0,
                  "Public Key der Gegenstelle", errorMessage) ||
      !readString(json, "wgEndpoint", updated.wgEndpoint, sizeof(updated.wgEndpoint), 0, "Endpunkt", errorMessage) ||
      !readString(json, "wgAllowedIps", updated.wgAllowedIps, sizeof(updated.wgAllowedIps), 0, "AllowedIPs",
                  errorMessage))
    return false;
  readClampedNumber(json, "wgPort", updated.wgEndpointPort, 1, 65535);
  readClampedNumber(json, "wgKeepalive", updated.wgKeepaliveSec, 0, 65535);
  if (updated.wgEnabled) {
    if (strlen(updated.wgPrivateKey) != 44 || strlen(updated.wgPeerPublicKey) != 44) {
      errorMessage = "WireGuard: Private Key und Public Key müssen 44 Zeichen (Base64) lang sein";
      return false;
    }
    if (!updated.wgAddress[0] || !strchr(updated.wgAddress, '.')) {
      errorMessage = "WireGuard: Tunnel-Adresse fehlt (z. B. 192.168.178.201/24)";
      return false;
    }
    if (!updated.wgEndpoint[0]) {
      errorMessage = "WireGuard: Endpunkt (Adresse der Gegenstelle) fehlt";
      return false;
    }
  }

  // --- Anbindung des GoodWe: RS485 oder Modbus TCP (z. B. RTU-TCP-Gateway) ---
  const cJSON* transportItem = cJSON_GetObjectItemCaseSensitive(json, "gwTransport");
  if (cJSON_IsString(transportItem))
    updated.goodweTransport = strcmp(transportItem->valuestring, "tcp") ? GOODWE_VIA_RTU : GOODWE_VIA_TCP;
  if (!readString(json, "gwTcpHost", updated.goodweTcpHost, sizeof(updated.goodweTcpHost), 0,
                  "GoodWe-TCP-Adresse", errorMessage))
    return false;
  readClampedNumber(json, "gwTcpPort", updated.goodweTcpPort, 1, 65535);
  readClampedNumber(json, "gwTcpUnit", updated.goodweTcpUnitId, 0, 255);
  readBool(json, "bridgeEnabled", updated.bridgeEnabled);
  readClampedNumber(json, "bridgePort", updated.bridgePort, 1, 65535);
  if (updated.bridgeEnabled && updated.bridgePort == 80) {
    errorMessage = "Modbus-TCP-Bridge: Port 80 ist von der Web-Oberfläche belegt";
    return false;
  }
  if (updated.goodweTransport == GOODWE_VIA_TCP) {
    if (!updated.goodweTcpHost[0]) {
      errorMessage = "GoodWe über TCP: Adresse des Gateways fehlt";
      return false;
    }
    for (int portIndex = 0; portIndex < 2; portIndex++) {
      if (updated.port[portIndex].role == ROLE_GOODWE_MASTER) {
        errorMessage = "GoodWe über TCP: RTU" + std::to_string(portIndex + 1) +
                       " hat noch die Rolle GoodWe - bitte auf Aus oder Lumel stellen";
        return false;
      }
    }
  }
  readBool(json, "lumelWordSwap", updated.lumelSwapFloatWords);
  readBool(json, "lumelSilentOnStale", updated.lumelSilentWhenStale);
  readBool(json, "lumelNoUndefined", updated.lumelReplaceUndefined);
  readClampedNumber(json, "staleSec", updated.staleAfterSec, 2, 3600);
  readBool(json, "testMode", updated.testModeEnabled);
  readClampedNumber(json, "testPowerW", updated.testPowerWatt, -1e6, 1e6);
  readClampedNumber(json, "testVoltage", updated.testVoltage, 0, 1000);

  // MQTT
  readBool(json, "mqttEnabled", updated.mqttEnabled);
  if (!readString(json, "mqttHost", updated.mqttHost, sizeof(updated.mqttHost), 0, "MQTT-Broker", errorMessage))
    return false;
  readClampedNumber(json, "mqttPort", updated.mqttPort, 1, 65535);
  if (!readString(json, "mqttUser", updated.mqttUser, sizeof(updated.mqttUser), 0, "MQTT-Benutzer", errorMessage))
    return false;
  if (!readSecret(json, "mqttPass", updated.mqttPassword, sizeof(updated.mqttPassword), 0, "MQTT-Passwort",
                  errorMessage))
    return false;
  if (isFlagSet(json, "mqttPassClear")) updated.mqttPassword[0] = 0;
  if (!readString(json, "mqttBase", updated.mqttBaseTopic, sizeof(updated.mqttBaseTopic), 1, "MQTT-Basis-Topic",
                  errorMessage))
    return false;
  readClampedNumber(json, "mqttIntervalSec", updated.mqttIntervalSec, 1, 3600);
  readBool(json, "mqttSingleTopics", updated.mqttSingleTopics);
  readBool(json, "mqttDiscovery", updated.mqttDiscoveryEnabled);
  if (!readString(json, "mqttDiscPrefix", updated.mqttDiscoveryPrefix, sizeof(updated.mqttDiscoveryPrefix), 1,
                  "Discovery-Prefix", errorMessage))
    return false;
  readBool(json, "mqttTls", updated.mqttTlsEnabled);
  applyTlsModeJson(updated, json);

  if (updated.mqttEnabled && !updated.mqttHost[0]) {
    errorMessage = "MQTT: Broker-Adresse fehlt";
    return false;
  }
  if (!normalizeMqttTopics(updated, errorMessage)) return false;

  config = updated;
  return true;
}

// ---------------------------------------------------------------- NVS

// Liest einen String aus dem NVS. Rückgabe: Wert oder leerer String, falls nicht vorhanden.
std::string nvsReadString(const char* nvsNamespace, const char* key) {
  nvs_handle_t nvsHandle;
  std::string value;
  if (nvs_open(nvsNamespace, NVS_READONLY, &nvsHandle) != ESP_OK) return value;
  size_t length = 0;  // inkl. abschließender Null
  if (nvs_get_str(nvsHandle, key, nullptr, &length) == ESP_OK && length > 0) {
    value.resize(length);
    if (nvs_get_str(nvsHandle, key, &value[0], &length) == ESP_OK) value.resize(length - 1);
    else value.clear();
  }
  nvs_close(nvsHandle);
  return value;
}

// Schreibt einen String ins NVS und committet. Rückgabe: true bei Erfolg.
bool nvsWriteString(const char* nvsNamespace, const char* key, const char* value) {
  nvs_handle_t nvsHandle;
  if (nvs_open(nvsNamespace, NVS_READWRITE, &nvsHandle) != ESP_OK) return false;
  bool success = nvs_set_str(nvsHandle, key, value) == ESP_OK && nvs_commit(nvsHandle) == ESP_OK;
  nvs_close(nvsHandle);
  return success;
}

// Lädt die Konfiguration aus dem NVS nach g_config. Fehlt sie oder ist sie fehlerhaft,
// gelten die Standardwerte.
void configLoad() {
  configDefaults(g_config);
  std::string storedJson = nvsReadString(CONFIG_NAMESPACE, CONFIG_KEY);
  if (storedJson.empty()) {
    ESP_LOGI(TAG, "Keine gespeicherte Konfiguration - Standardwerte");
    return;
  }
  JsonDocument document(cJSON_Parse(storedJson.c_str()));
  std::string errorMessage;
  if (!document.get() || !configApplyJson(g_config, document.get(), errorMessage)) {
    ESP_LOGW(TAG, "Gespeicherte Konfiguration fehlerhaft (%s) - Standardwerte", errorMessage.c_str());
    configDefaults(g_config);
  } else {
    ESP_LOGI(TAG, "Konfiguration geladen (%u Byte)", (unsigned)storedJson.size());
  }
}

// Speichert g_config (inkl. Passwörter) als JSON im NVS. Rückgabe: true bei Erfolg.
bool configSave() {
  JsonDocument document(configToJson(g_config, true));
  std::string jsonText = jsonToString(document.get());
  bool success = nvsWriteString(CONFIG_NAMESPACE, CONFIG_KEY, jsonText.c_str());
  if (!success) ESP_LOGE(TAG, "Speichern fehlgeschlagen");
  return success;
}

// Löscht Konfiguration und Zertifikate im NVS und setzt g_config auf Standardwerte.
// Der Neustart ist Sache des Aufrufers.
void configFactoryReset() {
  for (const char* nvsNamespace : {CONFIG_NAMESPACE, CERTIFICATE_NAMESPACE}) {
    nvs_handle_t nvsHandle;
    if (nvs_open(nvsNamespace, NVS_READWRITE, &nvsHandle) == ESP_OK) {
      nvs_erase_all(nvsHandle);
      nvs_commit(nvsHandle);
      nvs_close(nvsHandle);
    }
  }
  configDefaults(g_config);
}

// Zertifikate als Blob (PEM-Ketten können größer als die 4000-Byte-Grenze für NVS-Strings sein)
// Rückgabe: PEM-Text des Zertifikats name ("ca", "crt", "key") oder leerer String.
std::string certificateLoad(const char* name) {
  nvs_handle_t nvsHandle;
  std::string pem;
  if (nvs_open(CERTIFICATE_NAMESPACE, NVS_READONLY, &nvsHandle) != ESP_OK) return pem;
  size_t length = 0;
  if (nvs_get_blob(nvsHandle, name, nullptr, &length) == ESP_OK && length > 0) {
    pem.resize(length);
    if (nvs_get_blob(nvsHandle, name, &pem[0], &length) != ESP_OK) pem.clear();
  }
  nvs_close(nvsHandle);
  return pem;
}

// Speichert das Zertifikat name; ein leeres PEM löscht es. Rückgabe: true bei Erfolg.
bool certificateStore(const char* name, const std::string& pem) {
  nvs_handle_t nvsHandle;
  if (nvs_open(CERTIFICATE_NAMESPACE, NVS_READWRITE, &nvsHandle) != ESP_OK) return false;
  esp_err_t result =
      pem.empty() ? nvs_erase_key(nvsHandle, name) : nvs_set_blob(nvsHandle, name, pem.data(), pem.size());
  bool success = (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(nvsHandle) == ESP_OK;
  nvs_close(nvsHandle);
  return success;
}

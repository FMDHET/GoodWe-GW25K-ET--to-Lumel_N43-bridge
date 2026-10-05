// Weboberfläche und REST-API auf esp_http_server (ESP-IDF):
// Status, GoodWe-Werte, Konfiguration (inkl. Sicherung), WLAN-Scan, Neustart/Werksreset,
// MQTT-Discovery, OTA-Update/-Rollback sowie Captive-Portal-Umleitung im AP-Modus.
#include "web.h"
#include <cstdarg>
#include <cstdio>
#include <new>
#include <cstdlib>
#include <cstring>
#include "chip.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "goodwe.h"
#include "lumel.h"
#include "goodwe_sensors.h"
#include "mbedtls/base64.h"
#include "modbus_rtu.h"
#include "modbus_tcp_server.h"
#include "mqtt.h"
#include "net.h"
#include "ota.h"
#include "shared.h"
#include "util.h"
#include "web_console.h"
#include "diagnostics.h"
#include "time_sync.h"
#include "wireguard_vpn.h"
#include "web_ui.h"

static const char* TAG = "web";

// HTTP-Statuszeilen
static const char* const HTTP_STATUS_OK = "200 OK";
static const char* const HTTP_STATUS_BAD_REQUEST = "400 Bad Request";

static const size_t MAX_CONFIG_BODY_SIZE = 32768;
static const size_t MAX_CERTIFICATE_SIZE = 8000;
static const size_t OTA_CHUNK_SIZE = 4096;
static const int OTA_MAX_CONSECUTIVE_TIMEOUTS = 5;
static const uint32_t REBOOT_DELAY_AFTER_CONFIG_MS = 1500;
static const uint32_t REBOOT_DELAY_AFTER_FIRMWARE_MS = 1000;

static httpd_handle_t httpServer = nullptr;
static RtuPort* rtuPorts[2] = {nullptr, nullptr};
static esp_timer_handle_t rebootTimer = nullptr;

// Startet das Gerät nach delayMs neu. Verzögert, damit die HTTP-Antwort noch beim Browser ankommt;
// ein erneuter Aufruf startet die Wartezeit neu.
void scheduleReboot(uint32_t delayMs) {
  if (!rebootTimer) {
    esp_timer_create_args_t timerArgs = {};
    // Timer-Callback: neue Firmware ggf. bestätigen (gewollter Neustart ist kein Fehlstart), dann neu starten
    timerArgs.callback = [](void*) {
      otaConfirmBeforeIntentionalReboot();
      esp_restart();
    };
    timerArgs.name = "reboot";
    esp_timer_create(&timerArgs, &rebootTimer);
  }
  esp_timer_stop(rebootTimer);
  esp_timer_start_once(rebootTimer, (uint64_t)delayMs * 1000);
}

// Liefert den Rollennamen eines Ports für die API ("goodwe", "lumel" oder "off")
static const char* portRoleName(uint8_t role) {
  switch (role) {
    case ROLE_GOODWE_MASTER: return "goodwe";
    case ROLE_LUMEL_SLAVE: return "lumel";
    default: return "off";
  }
}

// ---------------------------------------------------------------- Konfiguration

// Zuordnung der JSON-Schlüssel der Web-API zu den Zertifikatsnamen im NVS
struct CertificateKey {
  const char* jsonKey;
  const char* nvsName;
};
static const CertificateKey CERTIFICATE_KEYS[] = {
    {"mqttCa", "ca"},     // CA-Zertifikat des Brokers
    {"mqttCert", "crt"},  // Client-Zertifikat
    {"mqttKey", "key"},   // privater Schlüssel des Clients
};
static const int CERTIFICATE_COUNT = 3;
static const int CERT_INDEX_CA = 0;
static const int CERT_INDEX_CLIENT_CERT = 1;
static const int CERT_INDEX_CLIENT_KEY = 2;

// Komplette Konfiguration als neues cJSON-Objekt (vom Aufrufer freizugeben):
// inkl. Passwörtern, MQTT-Zertifikaten und der Liste erlaubter GPIOs
cJSON* webConfigJson() {
  // inkl. Passwörtern, damit die Weboberfläche sie per "Auge" im Klartext anzeigen kann
  cJSON* configJson = configToJson(g_config, true);
  for (const CertificateKey& certificate : CERTIFICATE_KEYS)
    cJSON_AddStringToObject(configJson, certificate.jsonKey, certificateLoad(certificate.nvsName).c_str());
  cJSON* allowedGpioList = cJSON_AddArrayToObject(configJson, "validGpios");
  for (size_t gpioIndex = 0; gpioIndex < ALLOWED_GPIO_COUNT; gpioIndex++)
    cJSON_AddItemToArray(allowedGpioList, cJSON_CreateNumber(ALLOWED_GPIOS[gpioIndex]));
  return configJson;
}

// true, wenn pem leer ist (= Zertifikat löschen) oder BEGIN- und END-Markierung enthält
static bool isPemOrEmpty(const std::string& pem) {
  return pem.empty() ||
         (pem.find("-----BEGIN ") != std::string::npos && pem.find("-----END ") != std::string::npos);
}

// Entfernt Leerzeichen, Tabs und Zeilenumbrüche am Anfang und Ende
static std::string trimWhitespace(const char* text) {
  std::string result = text;
  size_t firstPosition = result.find_first_not_of(" \t\r\n");
  size_t lastPosition = result.find_last_not_of(" \t\r\n");
  return firstPosition == std::string::npos ? "" : result.substr(firstPosition, lastPosition - firstPosition + 1);
}

// Ermittelt die neuen Zertifikate: gespeicherte Werte, überschrieben von den im JSON
// vorhandenen ("" = löschen). Prüft Format und Größe.
// Rückgabe false mit errorMessage bei ungültigem Zertifikat; certificatesChanged meldet Änderungen.
static bool collectCertificates(const cJSON* json, std::string pems[CERTIFICATE_COUNT],
                                bool& certificatesChanged, std::string& errorMessage) {
  certificatesChanged = false;
  for (int certIndex = 0; certIndex < CERTIFICATE_COUNT; certIndex++) {
    const CertificateKey& certificate = CERTIFICATE_KEYS[certIndex];
    pems[certIndex] = certificateLoad(certificate.nvsName);
    const cJSON* jsonValue = cJSON_GetObjectItemCaseSensitive(json, certificate.jsonKey);
    if (!cJSON_IsString(jsonValue)) continue;  // nicht angegeben = unverändert
    std::string newPem = trimWhitespace(jsonValue->valuestring);
    if (!isPemOrEmpty(newPem)) {
      errorMessage = std::string(certificate.jsonKey) + ": kein gültiges PEM (-----BEGIN ... -----END ...)";
      return false;
    }
    if (newPem.size() > MAX_CERTIFICATE_SIZE) {
      errorMessage = std::string(certificate.jsonKey) + ": zu groß";
      return false;
    }
    if (newPem != pems[certIndex]) certificatesChanged = true;
    pems[certIndex] = newPem;
  }
  return true;
}

// true, wenn sich Einstellungen geändert haben, die erst nach einem Neustart wirken
// (RS485-Ports, WLAN, Hostname, AP-Passwort, GoodWe-Datenquelle)
static bool rebootNeededForChange(const AppConfig& newConfig, const AppConfig& oldConfig) {
  return memcmp(newConfig.port, oldConfig.port, sizeof(newConfig.port)) != 0 ||
         strcmp(newConfig.wifiSsid, oldConfig.wifiSsid) || strcmp(newConfig.wifiPassword, oldConfig.wifiPassword) ||
         strcmp(newConfig.hostname, oldConfig.hostname) ||
         strcmp(newConfig.accessPointPassword, oldConfig.accessPointPassword) ||
         newConfig.goodweDataSource != oldConfig.goodweDataSource ||
         newConfig.goodweTransport != oldConfig.goodweTransport ||
         strcmp(newConfig.goodweTcpHost, oldConfig.goodweTcpHost) || newConfig.goodweTcpPort != oldConfig.goodweTcpPort ||
         newConfig.goodweTcpUnitId != oldConfig.goodweTcpUnitId ||
         newConfig.bridgeEnabled != oldConfig.bridgeEnabled || newConfig.bridgePort != oldConfig.bridgePort ||
         // WireGuard: Tunnel wird nur beim Start aufgebaut
         newConfig.wgEnabled != oldConfig.wgEnabled || strcmp(newConfig.wgPrivateKey, oldConfig.wgPrivateKey) ||
         strcmp(newConfig.wgAddress, oldConfig.wgAddress) || strcmp(newConfig.wgPeerPublicKey, oldConfig.wgPeerPublicKey) ||
         strcmp(newConfig.wgPresharedKey, oldConfig.wgPresharedKey) || strcmp(newConfig.wgEndpoint, oldConfig.wgEndpoint) ||
         newConfig.wgEndpointPort != oldConfig.wgEndpointPort || strcmp(newConfig.wgAllowedIps, oldConfig.wgAllowedIps) ||
         newConfig.wgKeepaliveSec != oldConfig.wgKeepaliveSec ||
         strcmp(newConfig.ntpServer1, oldConfig.ntpServer1) || strcmp(newConfig.ntpServer2, oldConfig.ntpServer2) ||
         strcmp(newConfig.timeZone, oldConfig.timeZone);
}

// true, wenn sich MQTT-Verbindungsparameter geändert haben und der Client neu starten muss
static bool mqttSettingsChanged(const AppConfig& newConfig, const AppConfig& oldConfig) {
  return newConfig.mqttEnabled != oldConfig.mqttEnabled || strcmp(newConfig.mqttHost, oldConfig.mqttHost) ||
         newConfig.mqttPort != oldConfig.mqttPort || strcmp(newConfig.mqttUser, oldConfig.mqttUser) ||
         strcmp(newConfig.mqttPassword, oldConfig.mqttPassword) ||
         strcmp(newConfig.mqttBaseTopic, oldConfig.mqttBaseTopic) ||
         newConfig.mqttDiscoveryEnabled != oldConfig.mqttDiscoveryEnabled ||
         strcmp(newConfig.mqttDiscoveryPrefix, oldConfig.mqttDiscoveryPrefix) ||
         newConfig.mqttIntervalSec != oldConfig.mqttIntervalSec ||
         newConfig.mqttTlsEnabled != oldConfig.mqttTlsEnabled || newConfig.mqttTlsMode != oldConfig.mqttTlsMode;
}

// Prüft und übernimmt eine (Teil-)Konfiguration aus json, speichert Zertifikate und Konfiguration
// im NVS und startet MQTT bei Bedarf neu. rebootRequired wird gesetzt, wenn ein Neustart nötig ist.
// Rückgabe false mit errorMessage, wenn etwas ungültig ist oder nicht gespeichert werden konnte.
bool webApplyConfig(const cJSON* json, std::string& errorMessage, bool& rebootRequired) {
  AppConfig newConfig = g_config;
  if (!configApplyJson(newConfig, json, errorMessage)) return false;

  // Zertifikate: nur übernehmen, wenn im JSON vorhanden ("" = löschen)
  std::string pems[CERTIFICATE_COUNT];
  bool certificatesChanged = false;
  if (!collectCertificates(json, pems, certificatesChanged, errorMessage)) return false;
  if (newConfig.mqttEnabled && newConfig.mqttTlsEnabled && newConfig.mqttTlsMode == TLS_VERIFY_WITH_CA &&
      pems[CERT_INDEX_CA].empty()) {
    errorMessage = "MQTTS: CA-Zertifikat fehlt (oder andere Prüfart wählen)";
    return false;
  }
  if (pems[CERT_INDEX_CLIENT_CERT].empty() != pems[CERT_INDEX_CLIENT_KEY].empty()) {
    errorMessage = "MQTTS: Client-Zertifikat und Schlüssel nur gemeinsam angeben";
    return false;
  }
  if (certificatesChanged)
    for (int certIndex = 0; certIndex < CERTIFICATE_COUNT; certIndex++)
      if (!certificateStore(CERTIFICATE_KEYS[certIndex].nvsName, pems[certIndex])) {
        errorMessage = "Zertifikat konnte nicht gespeichert werden (NVS voll?)";
        return false;
      }

  // Auswirkungen bestimmen, dann übernehmen und speichern
  rebootRequired = rebootNeededForChange(newConfig, g_config);
  bool mqttChanged = mqttSettingsChanged(newConfig, g_config) || certificatesChanged;
  bool txPowerChanged = newConfig.wifiMaxTxPower != g_config.wifiMaxTxPower;
  g_config = newConfig;
  if (!configSave()) {
    errorMessage = "Speichern im NVS fehlgeschlagen";
    return false;
  }
  if (mqttChanged) mqttReconfigure();
  if (txPowerChanged) netApplyWifiTxPower();
  return true;
}

// ---------------------------------------------------------------- Status

// Fügt ein Array mit den drei Phasenwerten (2 Nachkommastellen) unter key in jsonObject ein
static void addPhaseArray(cJSON* jsonObject, const char* key, const float* phaseValues) {
  cJSON* phaseArray = cJSON_AddArrayToObject(jsonObject, key);
  for (int phaseIndex = 0; phaseIndex < 3; phaseIndex++)
    cJSON_AddItemToArray(phaseArray, jsonNumberWithDecimals(phaseValues[phaseIndex], 2));
}

// Abschnitt "meter": Werte, die als Lumel N43 ausgegeben werden
static void addMeterStatus(cJSON* statusJson, const MeterData& meterData, uint32_t nowMs) {
  cJSON* meterJson = cJSON_AddObjectToObject(statusJson, "meter");
  cJSON_AddBoolToObject(meterJson, "valid", meterData.valid);
  cJSON_AddItemToObject(meterJson, "age",
                        jsonNumberWithDecimals(meterData.updatedAtMs ? (nowMs - meterData.updatedAtMs) / 1000.0 : -1, 1));
  cJSON_AddBoolToObject(meterJson, "test", g_config.testModeEnabled);
  addPhaseArray(meterJson, "u", meterData.voltage);
  addPhaseArray(meterJson, "i", meterData.current);
  addPhaseArray(meterJson, "p", meterData.activePower);
  addPhaseArray(meterJson, "q", meterData.reactivePower);
  addPhaseArray(meterJson, "s", meterData.apparentPower);
  addPhaseArray(meterJson, "pf", meterData.powerFactor);
  addPhaseArray(meterJson, "uLL", meterData.lineToLineVoltage);
  cJSON_AddItemToObject(meterJson, "f", jsonNumberWithDecimals(meterData.frequency, 2));
  cJSON_AddItemToObject(meterJson, "pTot", jsonNumberWithDecimals(meterData.totalActivePower, 1));
  cJSON_AddItemToObject(meterJson, "qTot", jsonNumberWithDecimals(meterData.totalReactivePower, 1));
  cJSON_AddItemToObject(meterJson, "sTot", jsonNumberWithDecimals(meterData.totalApparentPower, 1));
  cJSON_AddItemToObject(meterJson, "pfTot", jsonNumberWithDecimals(meterData.totalPowerFactor, 3));
  cJSON_AddItemToObject(meterJson, "eImp", jsonNumberWithDecimals(meterData.importedEnergyKwh, 2));
  cJSON_AddItemToObject(meterJson, "eExp", jsonNumberWithDecimals(meterData.exportedEnergyKwh, 2));
}

// Abschnitt "goodwe": Zusammenfassung des Wechselrichters für die Übersichtsseite
static void addGoodweStatus(cJSON* statusJson, const GoodweInfo& goodweInfo) {
  cJSON* goodweJson = cJSON_AddObjectToObject(statusJson, "goodwe");
  cJSON_AddBoolToObject(goodweJson, "valid", goodweInfo.valid);
  cJSON_AddStringToObject(goodweJson, "model", goodweInfo.model);
  cJSON_AddStringToObject(goodweJson, "serial", goodweInfo.serialNumber);
  cJSON_AddStringToObject(goodweJson, "firmware", goodweInfo.firmwareVersion);
  cJSON_AddBoolToObject(goodweJson, "inverter", goodweInfo.inverterPresent);
  cJSON_AddBoolToObject(goodweJson, "batt1", goodweInfo.battery1Present);
  cJSON_AddItemToObject(goodweJson, "battSoc", jsonNumberWithDecimals(goodweInfo.batterySoc, 0));
  cJSON_AddBoolToObject(goodweJson, "batt2", goodweInfo.battery2Present);
  cJSON_AddItemToObject(goodweJson, "batt2V", jsonNumberWithDecimals(goodweInfo.battery2Voltage, 1));
  cJSON_AddItemToObject(goodweJson, "batt2I", jsonNumberWithDecimals(goodweInfo.battery2Current, 1));
  cJSON_AddItemToObject(goodweJson, "batt2P", jsonNumberWithDecimals(goodweInfo.battery2Power, 0));
  cJSON_AddItemToObject(goodweJson, "batt2Soc", jsonNumberWithDecimals(goodweInfo.battery2Soc, 0));
  cJSON_AddNumberToObject(goodweJson, "cycleMs", goodweInfo.cycleDurationMs);
  cJSON_AddNumberToObject(goodweJson, "slowCycleMs", goodweInfo.slowCycleDurationMs);
  int32_t nextFastPollInMs, nextSlowPollInMs;
  if (goodweNextPollTimes(nextFastPollInMs, nextSlowPollInMs)) {
    cJSON_AddNumberToObject(goodweJson, "nextFastMs", nextFastPollInMs);
    cJSON_AddNumberToObject(goodweJson, "nextSlowMs", nextSlowPollInMs);
  }
  cJSON* pvPowerArray = cJSON_AddArrayToObject(goodweJson, "pv");
  for (float stringPower : goodweInfo.pvPower) cJSON_AddItemToArray(pvPowerArray, jsonNumberWithDecimals(stringPower, 0));
  cJSON_AddItemToObject(goodweJson, "pvTotal", jsonNumberWithDecimals(goodweInfo.pvPowerTotal, 0));
  cJSON_AddItemToObject(goodweJson, "battV", jsonNumberWithDecimals(goodweInfo.batteryVoltage, 1));
  cJSON_AddItemToObject(goodweJson, "battI", jsonNumberWithDecimals(goodweInfo.batteryCurrent, 1));
  cJSON_AddItemToObject(goodweJson, "battP", jsonNumberWithDecimals(goodweInfo.batteryPower, 0));
  cJSON_AddItemToObject(goodweJson, "temp", jsonNumberWithDecimals(goodweInfo.temperature, 1));
  cJSON_AddItemToObject(goodweJson, "eTotal", jsonNumberWithDecimals(goodweInfo.energyTotalKwh, 1));
  cJSON_AddItemToObject(goodweJson, "eDay", jsonNumberWithDecimals(goodweInfo.energyTodayKwh, 1));
  cJSON_AddNumberToObject(goodweJson, "gridMode", goodweInfo.gridMode);
  cJSON_AddNumberToObject(goodweJson, "meterComm", goodweInfo.meterCommStatus);
  cJSON_AddNumberToObject(goodweJson, "meterRegs", goodweInfo.meterRegisterCount);
}

// Abschnitt "ports": Rolle, Statistik und Modbus-RTU-Diagnose je RS485-Port
static void addPortStatus(cJSON* statusJson, const PortStats portStats[PORT_STATS_COUNT], uint32_t nowMs) {
  cJSON* portArray = cJSON_AddArrayToObject(statusJson, "ports");
  // Einträge 0/1 = RTU-Ports; Eintrag 2 nur, wenn der GoodWe über Modbus TCP angebunden ist
  int entryCount = g_config.goodweTransport == GOODWE_VIA_TCP ? 3 : 2;
  for (int portIndex = 0; portIndex < entryCount; portIndex++) {
    const PortStats& stats = portStats[portIndex];
    cJSON* portJson = cJSON_CreateObject();
    cJSON_AddStringToObject(portJson, "role",
                            portIndex == GOODWE_TCP_STATS_INDEX ? "goodwe-tcp" : portRoleName(g_config.port[portIndex].role));
    cJSON_AddNumberToObject(portJson, "requests", stats.requests);
    cJSON_AddNumberToObject(portJson, "responses", stats.responses);
    cJSON_AddNumberToObject(portJson, "timeouts", stats.timeouts);
    cJSON_AddNumberToObject(portJson, "exceptions", stats.exceptions);
    // Sekunden seit der letzten erfolgreichen Übertragung, -1 = noch nie
    cJSON_AddNumberToObject(portJson, "lastOk",
                            stats.lastSuccessAtMs ? (int)((nowMs - stats.lastSuccessAtMs) / 1000) : -1);
    cJSON_AddStringToObject(portJson, "lastError", stats.lastError);
    cJSON_AddStringToObject(portJson, "lastRequest", stats.lastRequest);
    const RtuPort* rtuPort = portIndex < 2 ? rtuPorts[portIndex] : nullptr;
    if (portIndex == GOODWE_TCP_STATS_INDEX) {
      std::string target = std::string(g_config.goodweTcpHost) + ":" + std::to_string(g_config.goodweTcpPort);
      cJSON_AddStringToObject(portJson, "target", target.c_str());
    }
    if (rtuPort) {
      const RtuCounters& counters = rtuPort->counters;
      cJSON_AddNumberToObject(portJson, "frames", counters.validFrames);
      cJSON_AddNumberToObject(portJson, "crc", counters.crcErrors);
      cJSON_AddNumberToObject(portJson, "charErr", counters.characterErrors);
      cJSON_AddNumberToObject(portJson, "gapErr", counters.gapErrors);
      cJSON_AddNumberToObject(portJson, "t15", rtuPort->charTimeout15Us());
      cJSON_AddNumberToObject(portJson, "t35", rtuPort->frameTimeout35Us());
      cJSON_AddStringToObject(portJson, "raw", rtuPort->lastDiscardedFrameHex);
    }
    cJSON_AddItemToArray(portArray, portJson);
  }
}

// Gesamtstatus als neues cJSON-Objekt (vom Aufrufer freizugeben): System, WLAN, MQTT, OTA,
// Zählerwerte, GoodWe-Zusammenfassung und Port-Statistik
cJSON* webStatusJson() {
  // Momentaufnahme der geteilten Daten, damit die Sperre nur kurz gehalten wird
  MeterData meterData;
  GoodweInfo goodweInfo;
  PortStats portStats[PORT_STATS_COUNT];
  sharedLock();
  meterData = g_meterData;
  goodweInfo = g_goodweInfo;
  for (int statsIndex = 0; statsIndex < PORT_STATS_COUNT; statsIndex++) portStats[statsIndex] = g_portStats[statsIndex];
  sharedUnlock();

  uint32_t nowMs = millisSinceBoot();
  cJSON* statusJson = cJSON_CreateObject();
  cJSON_AddNumberToObject(statusJson, "uptime", nowMs / 1000);
  cJSON_AddStringToObject(statusJson, "chip", CHIP_NAME);
  cJSON_AddNumberToObject(statusJson, "heap", esp_get_free_heap_size());
  cJSON_AddNumberToObject(statusJson, "heapMin", esp_get_minimum_free_heap_size());
  cJSON_AddNumberToObject(statusJson, "heapTotal", heap_caps_get_total_size(MALLOC_CAP_DEFAULT));
  cJSON_AddStringToObject(statusJson, "fw", otaRunningVersion());
  netStatusToJson(cJSON_AddObjectToObject(statusJson, "wifi"));
  mqttStatusToJson(cJSON_AddObjectToObject(statusJson, "mqtt"));
  wireguardStatusToJson(cJSON_AddObjectToObject(statusJson, "vpn"));
  timeStatusToJson(cJSON_AddObjectToObject(statusJson, "time"));
  modbusTcpServerStatusToJson(cJSON_AddObjectToObject(statusJson, "bridge"));
  diagnosticsStatusToJson(cJSON_AddObjectToObject(statusJson, "diag"));
  otaStatusToJson(cJSON_AddObjectToObject(statusJson, "ota"));

  addMeterStatus(statusJson, meterData, nowMs);
  addGoodweStatus(statusJson, goodweInfo);
  addPortStatus(statusJson, portStats, nowMs);
  return statusJson;
}

// ---------------------------------------------------------------- HTTP-Helfer

// Prüft die HTTP-Basic-Anmeldung (nur wenn in der Konfiguration aktiviert).
// Bei fehlender/falscher Anmeldung wird bereits "401" gesendet und false geliefert;
// der Handler muss dann nur noch ESP_OK zurückgeben.
static bool isAuthorized(httpd_req_t* request) {
  if (!g_config.webLoginEnabled) return true;
  char authorizationHeader[160];
  if (httpd_req_get_hdr_value_str(request, "Authorization", authorizationHeader, sizeof(authorizationHeader)) ==
          ESP_OK &&
      !strncmp(authorizationHeader, "Basic ", 6)) {
    const char* encodedCredentials = authorizationHeader + 6;
    unsigned char decodedCredentials[128];
    size_t decodedLength = 0;
    if (mbedtls_base64_decode(decodedCredentials, sizeof(decodedCredentials) - 1, &decodedLength,
                              (const unsigned char*)encodedCredentials, strlen(encodedCredentials)) == 0) {
      decodedCredentials[decodedLength] = 0;
      std::string expectedCredentials = std::string(g_config.webUser) + ":" + g_config.webPassword;
      if (expectedCredentials == (const char*)decodedCredentials) return true;
    }
  }
  httpd_resp_set_status(request, "401 Unauthorized");
  httpd_resp_set_hdr(request, "WWW-Authenticate", "Basic realm=\"Modbus-Bridge\"");
  httpd_resp_sendstr(request, "Anmeldung erforderlich");
  return false;
}

// Sendet jsonDocument als Antwort mit dem angegebenen Status und gibt das Dokument frei
static esp_err_t sendJson(httpd_req_t* request, cJSON* jsonDocument, const char* status = HTTP_STATUS_OK) {
  std::string responseText = jsonToString(jsonDocument);
  cJSON_Delete(jsonDocument);
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "application/json; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, responseText.data(), responseText.size());
}

// Sendet {"ok": true}
static esp_err_t sendOk(httpd_req_t* request) {
  cJSON* responseJson = cJSON_CreateObject();
  cJSON_AddBoolToObject(responseJson, "ok", true);
  return sendJson(request, responseJson);
}

// Sendet {"ok": false, "error": errorMessage} mit dem angegebenen HTTP-Status
static esp_err_t sendError(httpd_req_t* request, const char* status, const std::string& errorMessage) {
  cJSON* responseJson = cJSON_CreateObject();
  cJSON_AddBoolToObject(responseJson, "ok", false);
  cJSON_AddStringToObject(responseJson, "error", errorMessage.c_str());
  return sendJson(request, responseJson, status);
}

// Liest den kompletten Request-Body (max. maxLength Byte) nach body.
// false, wenn er zu groß ist oder die Verbindung abbricht.
static bool readRequestBody(httpd_req_t* request, std::string& body, size_t maxLength) {
  if (request->content_len > maxLength) return false;
  body.resize(request->content_len);
  size_t receivedTotal = 0;
  while (receivedTotal < request->content_len) {
    int receivedNow = httpd_req_recv(request, &body[receivedTotal], request->content_len - receivedTotal);
    if (receivedNow == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (receivedNow <= 0) return false;
    receivedTotal += receivedNow;
  }
  return true;
}

// ---------------------------------------------------------------- Handler

// GET / : liefert die Weboberfläche (eine HTML-Seite inkl. Skript)
static esp_err_t handleGetIndexPage(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// GET /api/status : Gesamtstatus, siehe webStatusJson()
static esp_err_t handleGetStatus(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  return sendJson(request, webStatusJson());
}

// Schreibt eine JSON-Antwort in Stücken (Chunked Transfer) statt sie komplett im RAM aufzubauen.
// Für große Antworten wie die Wertetabelle: ein cJSON-Baum mit über 100 Zeilen bräuchte
// 60-80 kB Heap, was auf dem ESP32-C3 nicht übrig ist. Gepuffert wird nur ein kleiner Block.
class JsonStreamWriter {
 public:
  explicit JsonStreamWriter(httpd_req_t* request) : request(request) {
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  }

  // Hängt Text unverändert an (JSON-Syntax wie Klammern, Kommas, Zahlen).
  void raw(const char* text) {
    for (; *text; text++) {
      if (bufferedLength == sizeof(buffer)) flush();
      buffer[bufferedLength++] = *text;
    }
  }

  // Hängt formatierten Text an (für Zahlen).
  void format(const char* formatText, ...) __attribute__((format(printf, 2, 3))) {
    char text[48];
    va_list arguments;
    va_start(arguments, formatText);
    vsnprintf(text, sizeof(text), formatText, arguments);
    va_end(arguments);
    raw(text);
  }

  // Hängt einen JSON-String mit Anführungszeichen an; ", \ und Steuerzeichen werden maskiert.
  void string(const char* text) {
    raw("\"");
    char escaped[8];
    for (; text && *text; text++) {
      unsigned char character = (unsigned char)*text;
      if (character == '"' || character == '\\') {
        escaped[0] = '\\';
        escaped[1] = (char)character;
        escaped[2] = 0;
      } else if (character < 0x20) {
        snprintf(escaped, sizeof(escaped), "\\u%04x", character);
      } else {
        escaped[0] = (char)character;
        escaped[1] = 0;
      }
      raw(escaped);
    }
    raw("\"");
  }

  // Sendet den Rest und beendet die Antwort.
  esp_err_t finish() {
    flush();
    if (sendFailed) return ESP_FAIL;
    return httpd_resp_send_chunk(request, nullptr, 0);
  }

 private:
  // Schickt den gepufferten Block als Chunk.
  void flush() {
    if (bufferedLength && !sendFailed)
      sendFailed = httpd_resp_send_chunk(request, buffer, bufferedLength) != ESP_OK;
    bufferedLength = 0;
  }

  httpd_req_t* request;
  char buffer[512];
  size_t bufferedLength = 0;
  bool sendFailed = false;
};

// GET /api/goodwe : alle GoodWe-Werte mit Name/Einheit für die Tabelle im Web.
// Jede Zeile in "sensors" ist ein Array [id, name, block, register, wert als Text, einheit,
// abfragegruppe (1 = Intervall 1, 2 = Intervall 2), für Lumel fest in Intervall 1 (bool), gerät (GoodweDevice)].
// Wird gestreamt (JsonStreamWriter), damit die Tabelle auch auf dem ESP32-C3 kaum Heap braucht.
static esp_err_t handleGetGoodweValues(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  // Kopie der Rohregister (ca. 0,6 kB) statt den Lock während des Sendens zu halten
  GoodweRegisters* goodweRegisters = new (std::nothrow) GoodweRegisters;
  if (!goodweRegisters) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "kein Speicher");
  sharedLock();
  *goodweRegisters = g_goodweRegisters;
  sharedUnlock();

  JsonStreamWriter json(request);
  // Kopfdaten: Gerät, Alter der Daten, gültige Registerblöcke
  char model[20], serialNumber[18];
  goodweReadDeviceStrings(*goodweRegisters, model, sizeof(model), serialNumber, sizeof(serialNumber));
  json.raw("{\"model\":");
  json.string(model);
  json.raw(",\"serial\":");
  json.string(serialNumber);
  json.format(",\"age\":%d", goodweRegisters->updatedAtMs
                                  ? (int)((millisSinceBoot() - goodweRegisters->updatedAtMs) / 1000)
                                  : -1);
  json.format(",\"blocks\":{\"info\":%s,\"inverter\":%s,", goodweRegisters->deviceInfoValid ? "true" : "false",
              goodweRegisters->inverterValid ? "true" : "false");
  json.format("\"meter\":%s,\"bms\":%s}", goodweRegisters->meterValid ? "true" : "false",
              goodweRegisters->bmsValid ? "true" : "false");

  // Registerbereiche, die Intervall 1 liest: [[start, anzahl], ...]
  uint16_t fastStarts[24], fastCounts[24];
  size_t fastRangeCount = goodweCopyFastRanges(fastStarts, fastCounts, 24);
  json.raw(",\"fastRanges\":[");
  for (size_t rangeIndex = 0; rangeIndex < fastRangeCount; rangeIndex++)
    json.format("%s[%u,%u]", rangeIndex ? "," : "", fastStarts[rangeIndex], fastCounts[rangeIndex]);
  json.raw("]");

  // Sensortabelle: nur Werte, die in den Rohdaten vorhanden sind
  json.raw(",\"sensors\":[");
  bool firstRow = true;
  for (size_t sensorIndex = 0; sensorIndex < GOODWE_SENSOR_COUNT; sensorIndex++) {
    const GoodweSensor& sensor = GOODWE_SENSORS[sensorIndex];
    float value;
    const char* stateText;
    if (!goodweSensorRead(sensor, *goodweRegisters, value, stateText)) continue;
    char valueText[24];
    if (!stateText) snprintf(valueText, sizeof(valueText), "%.*f", goodweSensorDecimals(sensor), value);
    json.raw(firstRow ? "[" : ",[");
    firstRow = false;
    json.string(sensor.id);
    json.raw(",");
    json.string(sensor.name);
    json.format(",%u,%u,", sensor.block, sensor.registerAddress);
    json.string(stateText ? stateText : valueText);
    json.raw(",");
    json.string(sensor.unit ? sensor.unit : "");
    json.format(",%u,%s,%u]", goodweSensorPollGroup(sensorIndex),
                goodweSensorRequiredForLumel(sensor) ? "true" : "false", goodweSensorDevice(sensor));
  }
  json.raw("]}");
  delete goodweRegisters;
  return json.finish();
}

// GET /api/lumel : Register und Werte, die die Lumel-Simulation gerade ausgibt (Reiter Lumel)
static esp_err_t handleGetLumelValues(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  cJSON* responseJson = cJSON_CreateObject();
  lumelValuesToJson(responseJson);
  return sendJson(request, responseJson);
}

// GET /api/config : aktuelle Konfiguration, siehe webConfigJson()
static esp_err_t handleGetConfig(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  return sendJson(request, webConfigJson());
}

// POST /api/config : (Teil-)Konfiguration als JSON übernehmen.
// Antwort {"ok": true, "reboot": bool}; bei nötigem Neustart erfolgt dieser nach 1,5 s.
static esp_err_t handlePostConfig(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  std::string body;
  if (!readRequestBody(request, body, MAX_CONFIG_BODY_SIZE))
    return sendError(request, HTTP_STATUS_BAD_REQUEST, "Anfrage zu groß oder unvollständig");
  JsonDocument requestJson(cJSON_Parse(body.c_str()));
  if (!requestJson.get()) return sendError(request, HTTP_STATUS_BAD_REQUEST, "Ungültiges JSON");
  std::string errorMessage;
  bool rebootRequired = false;
  if (!webApplyConfig(requestJson.get(), errorMessage, rebootRequired))
    return sendError(request, HTTP_STATUS_BAD_REQUEST, errorMessage);
  cJSON* responseJson = cJSON_CreateObject();
  cJSON_AddBoolToObject(responseJson, "ok", true);
  cJSON_AddBoolToObject(responseJson, "reboot", rebootRequired);
  esp_err_t sendResult = sendJson(request, responseJson);
  if (rebootRequired) scheduleReboot(REBOOT_DELAY_AFTER_CONFIG_MS);
  return sendResult;
}

// GET /api/config/backup : Sicherung inkl. Passwörtern und Zertifikaten als Datei-Download
// (Wiederherstellung über POST /api/config). Reine Anzeige-Felder werden entfernt.
static esp_err_t handleGetConfigBackup(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  cJSON* backupJson = webConfigJson();
  cJSON_DeleteItemFromObject(backupJson, "validGpios");
  cJSON_DeleteItemFromObject(backupJson, "wifiPassSet");
  cJSON_DeleteItemFromObject(backupJson, "mqttPassSet");
  std::string backupText = jsonToString(backupJson, true);
  cJSON_Delete(backupJson);
  httpd_resp_set_type(request, "application/json; charset=utf-8");
  httpd_resp_set_hdr(request, "Content-Disposition", "attachment; filename=\"modbus-bridge-config.json\"");
  return httpd_resp_send(request, backupText.data(), backupText.size());
}

// GET /api/scan : WLAN-Scan, Antwort {"networks": [...]} (siehe netScan())
static esp_err_t handleGetWifiScan(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  cJSON* responseJson = cJSON_CreateObject();
  cJSON_AddItemToObject(responseJson, "networks", netScan());
  return sendJson(request, responseJson);
}

// POST /api/reboot : Neustart nach dem Senden der Antwort
// POST /api/identify?port=1|2 : lässt die TXD-LED des RS485-Moduls 10 s blinken (welches Modul ist welcher Port?)
static esp_err_t handlePostIdentify(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  static const uint32_t IDENTIFY_DURATION_MS = 10000;
  char query[16] = "", portText[4] = "";
  httpd_req_get_url_query_str(request, query, sizeof(query));
  httpd_query_key_value(query, "port", portText, sizeof(portText));
  int portIndex = atoi(portText) - 1;
  if (portIndex < 0 || portIndex > 1 || !rtuPorts[portIndex])
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "port=1 oder port=2");
  cJSON* responseJson = cJSON_CreateObject();
  bool started = rtuPorts[portIndex]->identify(g_config.port[portIndex], IDENTIFY_DURATION_MS);
  cJSON_AddBoolToObject(responseJson, "ok", started);
  if (!started) cJSON_AddStringToObject(responseJson, "error", "Identify läuft bereits");
  cJSON_AddNumberToObject(responseJson, "seconds", IDENTIFY_DURATION_MS / 1000);
  cJSON_AddBoolToObject(responseJson, "viaUart", rtuPorts[portIndex]->isRunning());
  return sendJson(request, responseJson);
}

static esp_err_t handlePostReboot(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  esp_err_t sendResult = sendOk(request);
  scheduleReboot();
  return sendResult;
}

// POST /api/factory : Werkseinstellungen herstellen und neu starten
static esp_err_t handlePostFactoryReset(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  configFactoryReset();
  esp_err_t sendResult = sendOk(request);
  scheduleReboot();
  return sendResult;
}

// POST /api/mqtt/discovery : Home-Assistant-Discovery erneut veröffentlichen
static esp_err_t handlePostMqttDiscovery(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  mqttRepublishDiscovery();
  return sendOk(request);
}

// POST /api/ota/rollback : auf die Firmware der anderen Partition umschalten und neu starten
static esp_err_t handlePostOtaRollback(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  std::string errorMessage;
  if (!otaSwitchToPreviousFirmware(errorMessage)) return sendError(request, HTTP_STATUS_BAD_REQUEST, errorMessage);
  esp_err_t sendResult = sendOk(request);
  scheduleReboot(REBOOT_DELAY_AFTER_FIRMWARE_MS);
  return sendResult;
}

// Liest den nächsten Block des Request-Bodys (höchstens remainingBytes) nach chunkBuffer.
// Rückgabe wie httpd_req_recv(): Anzahl Bytes oder <= 0 bei Fehler/Timeout.
static int receiveChunk(httpd_req_t* request, uint8_t* chunkBuffer, size_t remainingBytes) {
  size_t chunkLength = remainingBytes < OTA_CHUNK_SIZE ? remainingBytes : OTA_CHUNK_SIZE;
  return httpd_req_recv(request, (char*)chunkBuffer, chunkLength);
}

// POST /api/update : OTA-Update; die Firmware kommt als Binär-Stream
// (Content-Type application/octet-stream) und wird blockweise geschrieben.
static esp_err_t handlePostFirmwareUpdate(httpd_req_t* request) {
  if (!isAuthorized(request)) return ESP_OK;
  std::string errorMessage;
  if (request->content_len == 0) return sendError(request, HTTP_STATUS_BAD_REQUEST, "Leere Datei");
  if (!otaUploadStart(errorMessage)) return sendError(request, HTTP_STATUS_BAD_REQUEST, errorMessage);

  // statisch, damit 4 KB nicht auf dem Stack des HTTP-Tasks liegen
  static uint8_t chunkBuffer[OTA_CHUNK_SIZE];
  size_t remainingBytes = request->content_len;
  int consecutiveTimeouts = 0;
  while (remainingBytes > 0) {
    int receivedLength = receiveChunk(request, chunkBuffer, remainingBytes);
    if (receivedLength == HTTPD_SOCK_ERR_TIMEOUT && ++consecutiveTimeouts < OTA_MAX_CONSECUTIVE_TIMEOUTS) continue;
    if (receivedLength <= 0) {
      otaUploadAbort();
      return sendError(request, HTTP_STATUS_BAD_REQUEST, "Upload abgebrochen");
    }
    consecutiveTimeouts = 0;
    if (!otaUploadWrite(chunkBuffer, receivedLength, errorMessage)) {
      // restliche Daten verwerfen, damit der Browser die Fehlermeldung erhält
      while (remainingBytes > (size_t)receivedLength) {
        remainingBytes -= receivedLength;
        receivedLength = receiveChunk(request, chunkBuffer, remainingBytes);
        if (receivedLength <= 0) break;
      }
      return sendError(request, HTTP_STATUS_BAD_REQUEST, errorMessage);
    }
    remainingBytes -= receivedLength;
  }

  // Image prüfen, als Boot-Partition setzen und neu starten
  if (!otaUploadFinish(errorMessage)) return sendError(request, HTTP_STATUS_BAD_REQUEST, errorMessage);
  esp_err_t sendResult = sendOk(request);
  scheduleReboot(REBOOT_DELAY_AFTER_FIRMWARE_MS);
  return sendResult;
}

// 404-Handler. Captive Portal: im AP-Modus alle unbekannten Pfade auf die Startseite umleiten,
// damit Betriebssysteme die Anmeldeseite des WLANs anzeigen.
static esp_err_t handleNotFound(httpd_req_t* request, httpd_err_code_t) {
  if (netApActive()) {
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
    return httpd_resp_send(request, nullptr, 0);
  }
  httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
  return ESP_FAIL;
}

// Startet den HTTP-Server auf Port 80 und registriert alle Routen.
// ports: die beiden RS485-Ports für die Diagnose im Status (Einträge dürfen nullptr sein).
void webBegin(RtuPort* ports[2]) {
  rtuPorts[0] = ports[0];
  rtuPorts[1] = ports[1];

  httpd_config_t serverConfig = HTTPD_DEFAULT_CONFIG();
  serverConfig.max_uri_handlers = 24;
  serverConfig.stack_size = 8192;
  serverConfig.lru_purge_enable = true;  // bei vielen Verbindungen die älteste schließen
  // TCP-Keepalive: Verbindungen von Clients, die ohne Abmelden verschwinden (z. B. Handy verlässt
  // den Access-Point, Web-Konsole bleibt offen), nach etwa 20 s schließen. Sonst belegen sie
  // dauerhaft Sockets, bis keine neue Verbindung mehr angenommen wird.
  serverConfig.keep_alive_enable = true;
  serverConfig.keep_alive_idle = 5;
  serverConfig.keep_alive_interval = 5;
  serverConfig.keep_alive_count = 3;
  serverConfig.recv_wait_timeout = 10;
  serverConfig.send_wait_timeout = 10;
  if (httpd_start(&httpServer, &serverConfig) != ESP_OK) {
    ESP_LOGE(TAG, "HTTP-Server konnte nicht gestartet werden");
    return;
  }

  struct Route {
    const char* uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t*);
  };
  static const Route ROUTES[] = {
      {"/", HTTP_GET, handleGetIndexPage},
      {"/api/status", HTTP_GET, handleGetStatus},
      {"/api/goodwe", HTTP_GET, handleGetGoodweValues},
      {"/api/lumel", HTTP_GET, handleGetLumelValues},
      {"/api/config", HTTP_GET, handleGetConfig},
      {"/api/config", HTTP_POST, handlePostConfig},
      {"/api/config/backup", HTTP_GET, handleGetConfigBackup},
      {"/api/scan", HTTP_GET, handleGetWifiScan},
      {"/api/reboot", HTTP_POST, handlePostReboot},
      {"/api/identify", HTTP_POST, handlePostIdentify},
      {"/api/factory", HTTP_POST, handlePostFactoryReset},
      {"/api/mqtt/discovery", HTTP_POST, handlePostMqttDiscovery},
      {"/api/ota/rollback", HTTP_POST, handlePostOtaRollback},
      {"/api/update", HTTP_POST, handlePostFirmwareUpdate},
  };
  for (const Route& route : ROUTES) {
    httpd_uri_t uriHandler = {};
    uriHandler.uri = route.uri;
    uriHandler.method = route.method;
    uriHandler.handler = route.handler;
    httpd_register_uri_handler(httpServer, &uriHandler);
  }
  webConsoleBegin(httpServer, isAuthorized);  // Live-Konsole per WebSocket
  httpd_register_err_handler(httpServer, HTTPD_404_NOT_FOUND, handleNotFound);
  ESP_LOGI(TAG, "HTTP-Server läuft auf Port 80");
}

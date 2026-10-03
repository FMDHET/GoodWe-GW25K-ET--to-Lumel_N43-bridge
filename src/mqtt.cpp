// MQTT-Anbindung der Bridge: Verbindungsaufbau mit esp-mqtt (optional TLS), zyklisches Senden der
// GoodWe-, Zähler- und Diagnosewerte sowie Home-Assistant-Auto-Discovery. Läuft in einem eigenen Task.
#include <initializer_list>
#include "chip.h"
#include "mqtt.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "goodwe_sensors.h"
#include "mqtt_client.h"
#include "net.h"
#include "ota.h"
#include "shared.h"
#include "util.h"

static const char* TAG = "mqtt";

// Home-Assistant-Discovery verwendet die offiziellen Abkürzungen der Konfigurationsschlüssel:
//   name          Anzeigename                 uniq_id       unique_id
//   obj_id        object_id (Entity-ID)       stat_t        state_topic
//   val_tpl       value_template              unit_of_meas  unit_of_measurement
//   dev_cla       device_class                stat_cla      state_class
//   ent_cat       entity_category             sug_dsp_prc   suggested_display_precision
//   exp_aft       expire_after                avty_t        availability_topic
//   dev           device
// Im Objekt "dev":
//   ids           identifiers                 mf            manufacturer
//   mdl           model                       sn            serial_number
//   sw            sw_version                  cu            configuration_url
//   via_device    übergeordnetes Gerät (hier die Bridge)

static const size_t MAX_ANNOUNCED_SENSORS = 256;

static esp_mqtt_client_handle_t mqttClient = nullptr;
static std::atomic<bool> reconfigureRequested{true};
static std::atomic<bool> rediscoveryRequested{false};
static std::atomic<bool> isConnected{false};
static std::atomic<bool> connectionJustEstablished{false};

static char nodeId[24];  // MQTT-Client-ID und Basis der Home-Assistant-IDs, z.B. "mbbridge_a1b2c3"
static char lastErrorMessage[128] = "";
static uint32_t publishedMessageCount = 0;
static uint32_t connectionCount = 0;

// esp-mqtt speichert nur Zeiger auf diese Daten -> müssen leben, solange der Client existiert
static std::string brokerUriText, willTopic, homeAssistantStatusTopic, caCertificatePem, clientCertificatePem,
    clientKeyPem;

static uint8_t announcedSensorBits[MAX_ANNOUNCED_SENSORS / 8];  // Bitmaske angekündigter Sensoren
static char announcedModel[20] = "";

// ---------------------------------------------------------------- Hilfsfunktionen

// Liefert das vollständige Topic "<basis>/<subTopic>".
static std::string topic(const char* subTopic) { return std::string(g_config.mqttBaseTopic) + "/" + subTopic; }

// Baut die Broker-Adresse "mqtt(s)://<host>:<port>" aus der aktuellen Konfiguration.
static std::string brokerUri() {
  return std::string(g_config.mqttTlsEnabled ? "mqtts://" : "mqtt://") + g_config.mqttHost + ":" +
         std::to_string(g_config.mqttPort);
}

// Sendet eine Nachricht mit QoS 0. Rückgabe false, wenn nicht verbunden oder der Client sie ablehnt
// (dann wird lastErrorMessage gesetzt).
static bool publish(const std::string& topicName, const std::string& payload, bool retain = false) {
  if (!mqttClient || !isConnected) return false;
  int messageId = esp_mqtt_client_publish(mqttClient, topicName.c_str(), payload.data(), payload.size(), 0, retain);
  if (messageId < 0) {
    snprintf(lastErrorMessage, sizeof(lastErrorMessage), "Senden fehlgeschlagen: %s", topicName.c_str());
    return false;
  }
  publishedMessageCount++;
  return true;
}

// Sendet das JSON-Dokument als Text und gibt es anschließend frei (übernimmt also den Besitz).
static void publishJson(const std::string& topicName, cJSON* document, bool retain = false) {
  publish(topicName, jsonToString(document), retain);
  cJSON_Delete(document);
}

// true, wenn der Sensor mit diesem Index seit dem letzten Reset der Bitmaske angekündigt wurde.
static bool isSensorAnnounced(size_t sensorIndex) {
  return announcedSensorBits[sensorIndex / 8] & (1 << (sensorIndex % 8));
}

// Merkt den Sensor als angekündigt, damit er nicht bei jedem Zyklus erneut gesendet wird.
static void markSensorAnnounced(size_t sensorIndex) {
  announcedSensorBits[sensorIndex / 8] |= 1 << (sensorIndex % 8);
}

// ---------------------------------------------------------------- Discovery

// Erzeugt den HA-Geräteeintrag ("dev") des GoodWe-Wechselrichters; hängt in HA über via_device unter
// der Bridge. Leere model/serialNumber werden weggelassen. Rückgabe: neues cJSON-Objekt.
static cJSON* createGoodweDevice(const char* model, const char* serialNumber, const char* firmwareVersion) {
  cJSON* device = cJSON_CreateObject();
  cJSON* identifiers = cJSON_AddArrayToObject(device, "ids");
  cJSON_AddItemToArray(identifiers, cJSON_CreateString((std::string(nodeId) + "_goodwe").c_str()));
  cJSON_AddStringToObject(device, "name", (std::string("GoodWe ") + (model[0] ? model : "Wechselrichter")).c_str());
  cJSON_AddStringToObject(device, "mf", "GoodWe");
  if (model[0]) cJSON_AddStringToObject(device, "mdl", model);
  if (serialNumber[0]) cJSON_AddStringToObject(device, "sn", serialNumber);
  if (firmwareVersion[0]) cJSON_AddStringToObject(device, "sw", firmwareVersion);
  cJSON_AddStringToObject(device, "via_device", nodeId);
  return device;
}

// Erzeugt den HA-Geräteeintrag ("dev") der Bridge selbst, inkl. Firmware-Version und Link zur
// Weboberfläche (nur bei bestehender WLAN-Verbindung). Rückgabe: neues cJSON-Objekt.
static cJSON* createBridgeDevice() {
  cJSON* device = cJSON_CreateObject();
  cJSON* identifiers = cJSON_AddArrayToObject(device, "ids");
  cJSON_AddItemToArray(identifiers, cJSON_CreateString(nodeId));
  cJSON_AddStringToObject(device, "name", "Modbus-Bridge");
  cJSON_AddStringToObject(device, "mf", "DIY");
  cJSON_AddStringToObject(device, "mdl", CHIP_NAME " GoodWe -> Lumel N43");
  cJSON_AddStringToObject(device, "sw", otaRunningVersion());
  if (netStaConnected())
    cJSON_AddStringToObject(device, "cu", (std::string("http://") + netStaIp() + "/").c_str());
  return device;
}

// Sendet eine Discovery-Konfiguration retained an <prefix>/<component>/<node>/<objectId>/config,
// damit HA sie auch nach eigenem Neustart vorfindet. Gibt document frei.
static void publishDiscoveryConfig(const char* component, const char* objectId, cJSON* document) {
  std::string configTopic =
      std::string(g_config.mqttDiscoveryPrefix) + "/" + component + "/" + nodeId + "/" + objectId + "/config";
  publishJson(configTopic, document, true);
}

// Wert für "expire_after" in Sekunden: nach dieser Zeit ohne neue Werte zeigt HA den Sensor als
// "nicht verfügbar". 3 Sendeintervalle, begrenzt auf 60..3600 s.
static int expireAfterSec() {
  int expireSec = g_config.mqttIntervalSec * 3;
  return expireSec < 60 ? 60 : expireSec > 3600 ? 3600 : expireSec;
}

// Kündigt die Diagnose-Entitäten der Bridge an (WLAN-Signal, Laufzeit, RTU1-Fehler und den
// Binärsensor "GoodWe Verbindung"). Alle lesen aus <basis>/bridge/state.
static void publishBridgeDiscovery() {
  struct DiagnosticSensor {
    const char* id;
    const char* name;
    const char* unit;
    const char* deviceClass;
    const char* valueTemplate;
  };
  static const DiagnosticSensor DIAGNOSTIC_SENSORS[] = {
      {"wifi_rssi", "WLAN Signal", "dBm", "signal_strength", "{{ value_json.rssi }}"},
      {"uptime", "Laufzeit", "s", "duration", "{{ value_json.uptime }}"},
      {"rtu1_errors", "RTU1 Fehler", nullptr, nullptr, "{{ value_json.rtu1_errors }}"},
  };
  for (const DiagnosticSensor& sensor : DIAGNOSTIC_SENSORS) {
    cJSON* config = cJSON_CreateObject();
    cJSON_AddStringToObject(config, "name", sensor.name);
    cJSON_AddStringToObject(config, "uniq_id", (std::string(nodeId) + "_" + sensor.id).c_str());
    cJSON_AddStringToObject(config, "stat_t", topic("bridge/state").c_str());
    cJSON_AddStringToObject(config, "val_tpl", sensor.valueTemplate);
    if (sensor.unit) cJSON_AddStringToObject(config, "unit_of_meas", sensor.unit);
    if (sensor.deviceClass) cJSON_AddStringToObject(config, "dev_cla", sensor.deviceClass);
    cJSON_AddStringToObject(config, "stat_cla", "measurement");
    cJSON_AddStringToObject(config, "ent_cat", "diagnostic");
    cJSON_AddStringToObject(config, "avty_t", topic("status").c_str());
    cJSON_AddItemToObject(config, "dev", createBridgeDevice());
    publishDiscoveryConfig("sensor", sensor.id, config);
  }

  // Binärsensor: GoodWe-Daten aktuell ja/nein
  cJSON* config = cJSON_CreateObject();
  cJSON_AddStringToObject(config, "name", "GoodWe Verbindung");
  cJSON_AddStringToObject(config, "uniq_id", (std::string(nodeId) + "_goodwe_online").c_str());
  cJSON_AddStringToObject(config, "stat_t", topic("bridge/state").c_str());
  cJSON_AddStringToObject(config, "val_tpl", "{{ 'ON' if value_json.goodwe_online else 'OFF' }}");
  cJSON_AddStringToObject(config, "dev_cla", "connectivity");
  cJSON_AddStringToObject(config, "ent_cat", "diagnostic");
  cJSON_AddStringToObject(config, "avty_t", topic("status").c_str());
  cJSON_AddItemToObject(config, "dev", createBridgeDevice());
  publishDiscoveryConfig("binary_sensor", "goodwe_online", config);
}

// Erzeugt die Discovery-Konfiguration eines GoodWe-Sensors; der Wert wird per Template aus
// <basis>/goodwe/state gelesen. model/serialNumber gehen in den Geräteeintrag. Rückgabe: neues cJSON-Objekt.
static cJSON* createGoodweSensorConfig(const GoodweSensor& sensor, const char* model, const char* serialNumber,
                                       const char* firmwareVersion) {
  cJSON* config = cJSON_CreateObject();
  cJSON_AddStringToObject(config, "name", sensor.name);
  cJSON_AddStringToObject(config, "obj_id", (std::string("goodwe_") + sensor.id).c_str());
  cJSON_AddStringToObject(config, "uniq_id", (std::string(nodeId) + "_" + sensor.id).c_str());
  cJSON_AddStringToObject(config, "stat_t", topic("goodwe/state").c_str());
  cJSON_AddStringToObject(config, "val_tpl", (std::string("{{ value_json.get('") + sensor.id + "') }}").c_str());
  if (sensor.unit) cJSON_AddStringToObject(config, "unit_of_meas", sensor.unit);
  // Statustexte ohne device_class "enum", da unbekannte Codes sonst Fehler in HA erzeugen
  if (sensor.homeAssistantDeviceClass && !sensor.stateTexts)
    cJSON_AddStringToObject(config, "dev_cla", sensor.homeAssistantDeviceClass);
  if (sensor.stateClass == STATE_CLASS_MEASUREMENT)
    cJSON_AddStringToObject(config, "stat_cla", "measurement");
  else if (sensor.stateClass == STATE_CLASS_TOTAL_INCREASING)
    cJSON_AddStringToObject(config, "stat_cla", "total_increasing");
  if (!sensor.stateTexts && !sensor.isDiagnostic)
    cJSON_AddNumberToObject(config, "sug_dsp_prc", goodweSensorDecimals(sensor));
  if (sensor.isDiagnostic) cJSON_AddStringToObject(config, "ent_cat", "diagnostic");
  cJSON_AddNumberToObject(config, "exp_aft", expireAfterSec());
  cJSON_AddStringToObject(config, "avty_t", topic("status").c_str());
  cJSON_AddItemToObject(config, "dev", createGoodweDevice(model, serialNumber, firmwareVersion));
  return config;
}

// Berechnete Summen für Home-Assistant-Karten (z. B. Energiefluss), die je Quelle nur einen Sensor
// anzeigen können. Sie stehen nicht in der Registertabelle, sondern werden beim Senden gebildet.
static const GoodweSensor COMPUTED_SENSORS[] = {
    {"ppv_total", "PV Leistung gesamt", BLOCK_INVERTER, 0, TYPE_S32, 1, "W", "power", STATE_CLASS_MEASUREMENT,
     nullptr, 0, false},
    {"pbattery_total", "Batterie Leistung gesamt", BLOCK_INVERTER, 0, TYPE_S32, 1, "W", "power",
     STATE_CLASS_MEASUREMENT, nullptr, 0, false},
};
static bool computedSensorsAnnounced = false;

// Kündigt alle aktuell lesbaren GoodWe-Sensoren an (z.B. BMS-Werte erst, wenn eine Batterie antwortet).
// Bereits angekündigte Sensoren werden nur bei geändertem Modell erneut gesendet.
static void publishGoodweDiscovery(const GoodweRegisters& registers) {
  char model[20], serialNumber[18], firmwareVersion[28];
  goodweReadDeviceStrings(registers, model, sizeof(model), serialNumber, sizeof(serialNumber));
  goodweReadFirmwareVersion(registers, firmwareVersion, sizeof(firmwareVersion));
  bool modelChanged = strcmp(model, announcedModel) != 0;

  for (size_t sensorIndex = 0; sensorIndex < GOODWE_SENSOR_COUNT; sensorIndex++) {
    const GoodweSensor& sensor = GOODWE_SENSORS[sensorIndex];
    float value;
    const char* stateText;
    bool isAvailable = goodweSensorRead(sensor, registers, value, stateText);
    bool wasAnnounced = isSensorAnnounced(sensorIndex);
    if (!isAvailable || (wasAnnounced && !modelChanged)) continue;

    publishDiscoveryConfig("sensor", sensor.id, createGoodweSensorConfig(sensor, model, serialNumber, firmwareVersion));
    markSensorAnnounced(sensorIndex);
    vTaskDelay(pdMS_TO_TICKS(5));  // Sendepuffer nicht fluten
  }
  // Berechnete Summen, sobald der Wechselrichterblock gelesen ist
  if (registers.inverterValid && (!computedSensorsAnnounced || modelChanged)) {
    for (const GoodweSensor& sensor : COMPUTED_SENSORS)
      publishDiscoveryConfig("sensor", sensor.id, createGoodweSensorConfig(sensor, model, serialNumber, firmwareVersion));
    computedSensorsAnnounced = true;
  }
  copyString(announcedModel, model, sizeof(announcedModel));
}

// ---------------------------------------------------------------- Zustände

// Sendet alle lesbaren GoodWe-Werte samt Modell/Seriennummer als JSON an <basis>/goodwe/state und,
// falls konfiguriert, zusätzlich jeden Wert einzeln an <basis>/goodwe/<id>.
// Bildet die Summe der vorhandenen Werte keys (z. B. PV1..PV4) aus state und trägt sie unter sumKey ein.
// Fehlen alle Werte (Gerät abgeschaltet oder nicht vorhanden), wird nichts eingetragen.
static void addSumOfValues(cJSON* state, const char* sumKey, std::initializer_list<const char*> keys) {
  double sum = 0;
  bool anyValue = false;
  for (const char* key : keys) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(state, key);
    // Messwerte stehen als vorformatierte Zahl (Raw, siehe jsonNumberWithDecimals), nicht als Number
    if (cJSON_IsRaw(item)) sum += atof(item->valuestring);
    else if (cJSON_IsNumber(item)) sum += item->valuedouble;
    else continue;
    anyValue = true;
  }
  if (anyValue) cJSON_AddItemToObject(state, sumKey, jsonNumberWithDecimals(sum, 0));
}

static void publishGoodweState(const GoodweRegisters& registers) {
  cJSON* state = cJSON_CreateObject();
  goodweSensorsToJson(state, registers);
  // Summen für Home-Assistant-Karten (siehe COMPUTED_SENSORS); GoodWe: Batterie + = Entladen
  addSumOfValues(state, "ppv_total", {"ppv1", "ppv2", "ppv3", "ppv4"});
  addSumOfValues(state, "pbattery_total", {"pbattery1", "pbattery2"});
  char model[20], serialNumber[18];
  goodweReadDeviceStrings(registers, model, sizeof(model), serialNumber, sizeof(serialNumber));
  if (model[0]) cJSON_AddStringToObject(state, "model", model);
  if (serialNumber[0]) cJSON_AddStringToObject(state, "serial", serialNumber);
  if (g_config.mqttSingleTopics) {
    cJSON* item;
    cJSON_ArrayForEach(item, state) {
      std::string valueText = cJSON_IsString(item) ? item->valuestring : jsonToString(item);
      publish(topic("goodwe/") + item->string, valueText);
    }
  }
  publishJson(topic("goodwe/state"), state);
}

// Sendet die Zählerwerte, wie sie als Lumel N43 ausgegeben werden, an <basis>/meter/state:
// je Phase ein Objekt l1..l3, dazu Summen, Frequenz und Energiezähler.
static void publishMeterState(const MeterData& meter) {
  cJSON* state = cJSON_CreateObject();
  const char* PHASE_KEYS[] = {"l1", "l2", "l3"};
  for (int phaseIndex = 0; phaseIndex < 3; phaseIndex++) {
    cJSON* phase = cJSON_AddObjectToObject(state, PHASE_KEYS[phaseIndex]);
    cJSON_AddItemToObject(phase, "u", jsonNumberWithDecimals(meter.voltage[phaseIndex], 1));
    cJSON_AddItemToObject(phase, "i", jsonNumberWithDecimals(meter.current[phaseIndex], 2));
    cJSON_AddItemToObject(phase, "p", jsonNumberWithDecimals(meter.activePower[phaseIndex], 0));
    cJSON_AddItemToObject(phase, "q", jsonNumberWithDecimals(meter.reactivePower[phaseIndex], 0));
    cJSON_AddItemToObject(phase, "s", jsonNumberWithDecimals(meter.apparentPower[phaseIndex], 0));
    cJSON_AddItemToObject(phase, "pf", jsonNumberWithDecimals(meter.powerFactor[phaseIndex], 3));
  }
  cJSON_AddItemToObject(state, "p", jsonNumberWithDecimals(meter.totalActivePower, 0));
  cJSON_AddItemToObject(state, "q", jsonNumberWithDecimals(meter.totalReactivePower, 0));
  cJSON_AddItemToObject(state, "s", jsonNumberWithDecimals(meter.totalApparentPower, 0));
  cJSON_AddItemToObject(state, "pf", jsonNumberWithDecimals(meter.totalPowerFactor, 3));
  cJSON_AddItemToObject(state, "f", jsonNumberWithDecimals(meter.frequency, 2));
  cJSON_AddItemToObject(state, "e_imp", jsonNumberWithDecimals(meter.importedEnergyKwh, 2));
  cJSON_AddItemToObject(state, "e_exp", jsonNumberWithDecimals(meter.exportedEnergyKwh, 2));
  publishJson(topic("meter/state"), state);
}

// Sendet die Diagnose der Bridge an <basis>/bridge/state. goodweOnline = GoodWe-Daten aktuell,
// rtu1Stats = Statistik von Port RTU1 (Anfragen/Fehler).
static void publishBridgeState(bool goodweOnline, const PortStats& rtu1Stats) {
  wifi_ap_record_t accessPointInfo;
  cJSON* state = cJSON_CreateObject();
  cJSON_AddBoolToObject(state, "goodwe_online", goodweOnline);
  cJSON_AddNumberToObject(state, "rssi",
                          esp_wifi_sta_get_ap_info(&accessPointInfo) == ESP_OK ? accessPointInfo.rssi : 0);
  cJSON_AddNumberToObject(state, "uptime", millisSinceBoot() / 1000);
  cJSON_AddNumberToObject(state, "heap", esp_get_free_heap_size());
  cJSON_AddStringToObject(state, "ip", netStaIp());
  cJSON_AddNumberToObject(state, "rtu1_requests", rtu1Stats.requests);
  cJSON_AddNumberToObject(state, "rtu1_errors", rtu1Stats.requests - rtu1Stats.responses);
  cJSON_AddStringToObject(state, "version", otaRunningVersion());
  publishJson(topic("bridge/state"), state);
}

// Ein Sendezyklus: kopiert die geteilten Daten und sendet GoodWe- (nur wenn aktuell), Zähler- (nur wenn
// gültig) und Bridge-Zustand. Neue GoodWe-Sensoren werden dabei bei Bedarf angekündigt.
static void publishStates() {
  // Kopie der geteilten Daten, damit die Sperre nicht während des Sendens gehalten wird
  GoodweRegisters registers;
  MeterData meter;
  PortStats rtu1Stats;
  sharedLock();
  registers = g_goodweRegisters;
  meter = g_meterData;
  rtu1Stats = g_portStats[0];
  sharedUnlock();

  bool goodweDataFresh =
      registers.inverterValid && millisSinceBoot() - registers.updatedAtMs < g_config.staleAfterSec * 1000UL;
  if (goodweDataFresh) {
    if (g_config.mqttDiscoveryEnabled) publishGoodweDiscovery(registers);
    publishGoodweState(registers);
  }

  if (meter.valid) publishMeterState(meter);

  publishBridgeState(goodweDataFresh, rtu1Stats);
}

// ---------------------------------------------------------------- esp-mqtt

// Übersetzt einen esp-mqtt-Fehler (Transport/TLS oder vom Broker abgelehnt) in einen lesbaren Text
// in lastErrorMessage. Andere Fehlertypen lassen den Text unverändert.
static void setLastErrorFromMqttError(const esp_mqtt_error_codes_t* error) {
  if (error->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
    if (error->esp_tls_last_esp_err)
      snprintf(lastErrorMessage, sizeof(lastErrorMessage), "Verbindung/TLS: %s (TLS-Stack 0x%x, Zertifikat 0x%x)",
               esp_err_to_name(error->esp_tls_last_esp_err), error->esp_tls_stack_err,
               error->esp_tls_cert_verify_flags);
    else
      snprintf(lastErrorMessage, sizeof(lastErrorMessage), "Broker nicht erreichbar (errno %d)",
               error->esp_transport_sock_errno);
  } else if (error->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
    const char* reason = error->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                                 error->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
                             ? "Benutzer/Passwort falsch bzw. nicht autorisiert"
                             : "vom Broker abgelehnt";
    snprintf(lastErrorMessage, sizeof(lastErrorMessage), "Verbindung %s (Code %d)", reason,
             error->connect_return_code);
  }
}

// Ereignis-Handler von esp-mqtt (läuft im esp-mqtt-Task): pflegt den Verbindungszustand, meldet
// "online", abonniert den HA-Status und wertet Fehler aus.
static void onMqttEvent(void*, esp_event_base_t, int32_t eventId, void* eventData) {
  auto* event = (esp_mqtt_event_handle_t)eventData;
  switch ((esp_mqtt_event_id_t)eventId) {
    case MQTT_EVENT_CONNECTED:
      connectionCount++;
      isConnected = true;
      connectionJustEstablished = true;
      lastErrorMessage[0] = 0;
      esp_mqtt_client_publish(mqttClient, willTopic.c_str(), "online", 6, 1, 1);
      esp_mqtt_client_subscribe(mqttClient, homeAssistantStatusTopic.c_str(), 0);
      ESP_LOGI(TAG, "Verbunden mit %s", brokerUriText.c_str());
      break;
    case MQTT_EVENT_DISCONNECTED:
      isConnected = false;
      if (!lastErrorMessage[0]) copyString(lastErrorMessage, "Verbindung getrennt", sizeof(lastErrorMessage));
      break;
    case MQTT_EVENT_DATA:
      // Home Assistant meldet nach einem Neustart "online" -> Discovery erneut senden
      if (event->topic_len == (int)homeAssistantStatusTopic.size() &&
          !memcmp(event->topic, homeAssistantStatusTopic.data(), event->topic_len) && event->data_len == 6 &&
          !memcmp(event->data, "online", 6))
        rediscoveryRequested = true;
      break;
    case MQTT_EVENT_ERROR:
      setLastErrorFromMqttError(event->error_handle);
      ESP_LOGW(TAG, "%s", lastErrorMessage);
      break;
    default:
      break;
  }
}

// Meldet sich (falls verbunden) mit "offline" ab und beendet/zerstört den Client.
static void stopClient() {
  if (!mqttClient) return;
  if (isConnected) esp_mqtt_client_publish(mqttClient, willTopic.c_str(), "offline", 7, 1, 1);
  esp_mqtt_client_stop(mqttClient);
  esp_mqtt_client_destroy(mqttClient);
  mqttClient = nullptr;
  isConnected = false;
}

// Richtet für mqtts:// die Server-Prüfung (eigene CA, Zertifikats-Bundle oder keine) und optional ein
// Client-Zertifikat ein. Die PEM-Texte liegen in Dateivariablen, weil esp-mqtt nur Zeiger speichert.
static void configureTls(esp_mqtt_client_config_t& clientConfig) {
  caCertificatePem = certificateLoad("ca");
  clientCertificatePem = certificateLoad("crt");
  clientKeyPem = certificateLoad("key");
  if (g_config.mqttTlsMode == TLS_VERIFY_WITH_CA && !caCertificatePem.empty()) {
    clientConfig.broker.verification.certificate = caCertificatePem.c_str();
    clientConfig.broker.verification.certificate_len = caCertificatePem.size() + 1;
  } else if (g_config.mqttTlsMode == TLS_VERIFY_WITH_BUNDLE) {
    clientConfig.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  } else {
    // keine Prüfung (CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) - nur verschlüsselt
    clientConfig.broker.verification.skip_cert_common_name_check = true;
  }
  if (!clientCertificatePem.empty() && !clientKeyPem.empty()) {
    clientConfig.credentials.authentication.certificate = clientCertificatePem.c_str();
    clientConfig.credentials.authentication.certificate_len = clientCertificatePem.size() + 1;
    clientConfig.credentials.authentication.key = clientKeyPem.c_str();
    clientConfig.credentials.authentication.key_len = clientKeyPem.size() + 1;
  }
}

// Erzeugt und startet den esp-mqtt-Client nach der aktuellen Konfiguration. Bei Fehler bleibt
// mqttClient nullptr und lastErrorMessage wird gesetzt; der Task versucht es im nächsten Durchlauf erneut.
static void startClient() {
  brokerUriText = brokerUri();
  willTopic = topic("status");
  homeAssistantStatusTopic = std::string(g_config.mqttDiscoveryPrefix) + "/status";

  esp_mqtt_client_config_t clientConfig = {};
  clientConfig.broker.address.uri = brokerUriText.c_str();
  if (g_config.mqttTlsEnabled) configureTls(clientConfig);
  clientConfig.credentials.client_id = nodeId;
  if (g_config.mqttUser[0]) {
    clientConfig.credentials.username = g_config.mqttUser;
    clientConfig.credentials.authentication.password = g_config.mqttPassword;
  }
  // Last Will: Broker meldet "offline", wenn die Verbindung abreißt
  clientConfig.session.last_will.topic = willTopic.c_str();
  clientConfig.session.last_will.msg = "offline";
  clientConfig.session.last_will.msg_len = 7;
  clientConfig.session.last_will.qos = 1;
  clientConfig.session.last_will.retain = 1;
  clientConfig.session.keepalive = 30;
  clientConfig.network.reconnect_timeout_ms = 10000;
  clientConfig.network.timeout_ms = 10000;
  clientConfig.buffer.size = 4096;
  clientConfig.buffer.out_size = 4096;
  clientConfig.task.stack_size = 6144;

  mqttClient = esp_mqtt_client_init(&clientConfig);
  if (!mqttClient) {
    copyString(lastErrorMessage, "Client konnte nicht erzeugt werden", sizeof(lastErrorMessage));
    return;
  }
  esp_mqtt_client_register_event(mqttClient, MQTT_EVENT_ANY, onMqttEvent, nullptr);
  esp_mqtt_client_start(mqttClient);
  ESP_LOGI(TAG, "Starte Verbindung zu %s", brokerUriText.c_str());
}

// MQTT-Task: startet/stoppt den Client je nach Konfiguration und WLAN, sendet nach (Neu-)Verbindung
// die Discovery und danach zyklisch alle mqttIntervalSec Sekunden die Zustände.
static void mqttTask(void*) {
  uint32_t lastPublishMs = 0;  // 0 = sofort senden
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));

    // Client bei Konfigurationsänderung oder fehlender Voraussetzung beenden
    bool shouldRun = g_config.mqttEnabled && netStaConnected();
    if (reconfigureRequested.exchange(false) || (!shouldRun && mqttClient)) {
      stopClient();
      if (!g_config.mqttEnabled) copyString(lastErrorMessage, "deaktiviert", sizeof(lastErrorMessage));
    }
    if (!shouldRun) {
      if (g_config.mqttEnabled) copyString(lastErrorMessage, "kein WLAN", sizeof(lastErrorMessage));
      continue;
    }
    if (!mqttClient) startClient();
    if (!isConnected) continue;

    // Nach (Neu-)Verbindung oder Anforderung durch HA alles neu ankündigen

    if (connectionJustEstablished.exchange(false) || rediscoveryRequested.exchange(false)) {
      memset(announcedSensorBits, 0, sizeof(announcedSensorBits));
      announcedModel[0] = 0;
      computedSensorsAnnounced = false;
      if (g_config.mqttDiscoveryEnabled) publishBridgeDiscovery();
      lastPublishMs = 0;
    }

    // Zyklisch senden
    if (!lastPublishMs || millisSinceBoot() - lastPublishMs >= g_config.mqttIntervalSec * 1000UL) {
      lastPublishMs = millisSinceBoot();
      publishStates();
    }
  }
}

// ---------------------------------------------------------------- öffentliche Schnittstelle

// Bildet die Node-ID aus den letzten drei Bytes der WLAN-MAC und startet den MQTT-Task.
void mqttStart() {
  uint8_t macAddress[6];
  esp_read_mac(macAddress, ESP_MAC_WIFI_STA);
  snprintf(nodeId, sizeof(nodeId), "mbbridge_%02x%02x%02x", macAddress[3], macAddress[4], macAddress[5]);
  xTaskCreate(mqttTask, "mqtt_pub", 6144, nullptr, 3, nullptr);
}

// Fordert einen Neuaufbau der Verbindung an (nach geänderter Konfiguration); wirkt im MQTT-Task.
void mqttReconfigure() { reconfigureRequested = true; }
// Fordert an, dass alle Discovery-Konfigurationen erneut gesendet werden.
void mqttRepublishDiscovery() { rediscoveryRequested = true; }

// Trägt den MQTT-Status für die Weboberfläche in statusObject ein.
void mqttStatusToJson(cJSON* statusObject) {
  cJSON_AddBoolToObject(statusObject, "enabled", g_config.mqttEnabled);
  cJSON_AddBoolToObject(statusObject, "connected", isConnected);
  cJSON_AddStringToObject(statusObject, "broker", brokerUri().c_str());
  cJSON_AddBoolToObject(statusObject, "tls", g_config.mqttTlsEnabled);
  cJSON_AddStringToObject(statusObject, "node", nodeId);
  cJSON_AddNumberToObject(statusObject, "published", publishedMessageCount);
  cJSON_AddNumberToObject(statusObject, "connects", connectionCount);
  cJSON_AddStringToObject(statusObject, "lastError", lastErrorMessage);
  cJSON_AddStringToObject(statusObject, "base", g_config.mqttBaseTopic);
}

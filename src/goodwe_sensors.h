#pragma once
// Vollständige Sensorliste des GoodWe ET (Grundlage für Web-Tabelle, MQTT und Home-Assistant-Discovery)
#include <cstddef>
#include <cstdint>
#include "cJSON.h"
#include "shared.h"

// Registerblock, aus dem ein Wert stammt
enum GoodweRegisterBlock : uint8_t {
  BLOCK_DEVICE_INFO,  // ab 35000
  BLOCK_INVERTER,     // ab 35100
  BLOCK_METER,        // ab 36000
  BLOCK_BMS,          // ab 37000
  BLOCK_BATTERY2,     // ab 35262
  BLOCK_BMS2,         // ab 39000
};

// Gerät, zu dem ein Wert gehört: je eine Karte auf dem Reiter GoodWe; Smart-Meter und Batterien
// lassen sich abschalten (werden dann nicht abgefragt und überall ausgeblendet).
enum GoodweDevice : uint8_t {
  DEVICE_INFO,
  DEVICE_INVERTER,
  DEVICE_METER,
  DEVICE_BATTERY1,
  DEVICE_BATTERY2,
  GOODWE_DEVICE_COUNT
};

enum RegisterDataType : uint8_t { TYPE_U16, TYPE_S16, TYPE_U32, TYPE_S32, TYPE_FLOAT32 };

// Home Assistant state_class
enum SensorStateClass : uint8_t { STATE_CLASS_NONE, STATE_CLASS_MEASUREMENT, STATE_CLASS_TOTAL_INCREASING };

struct GoodweSensor {
  const char* id;                    // MQTT-/JSON-Schlüssel, z.B. "ppv1"
  const char* name;                  // Anzeigename
  uint8_t block;                     // GoodweRegisterBlock
  uint16_t registerAddress;
  uint8_t dataType;                  // RegisterDataType
  uint16_t divisor;                  // physikalischer Wert = Rohwert / divisor
  const char* unit;                  // nullptr = ohne Einheit
  const char* homeAssistantDeviceClass;
  uint8_t stateClass;                // SensorStateClass
  const char* const* stateTexts;     // Klartext für Statuscodes (Index = Rohwert), sonst nullptr
  uint8_t stateTextCount;
  bool isDiagnostic;                 // Home Assistant entity_category "diagnostic"
};

extern const GoodweSensor GOODWE_SENSORS[];
extern const size_t GOODWE_SENSOR_COUNT;

// Gerät (GoodweDevice), zu dem der Wert gehört
uint8_t goodweSensorDevice(const GoodweSensor& sensor);
// Schlüssel eines Geräts für Konfiguration und Web ("meter", "battery1", ...); nullptr bei ungültigem Wert
const char* goodweDeviceKey(uint8_t device);
// true, wenn das Gerät nicht in den Einstellungen abgeschaltet ist
bool goodweDeviceEnabled(uint8_t device);

// Index des Sensors mit der ID sensorId in GOODWE_SENSORS, -1 wenn unbekannt
int goodweSensorIndexById(const char* sensorId);
// Anzahl der 16-Bit-Register, die der Wert belegt (1 oder 2)
uint8_t goodweSensorRegisterCount(const GoodweSensor& sensor);
// true, wenn die Lumel-Emulation den Wert braucht (abhängig von der Datenquelle). Solche Werte
// werden immer in Intervall 1 gelesen, sofern ein RS485-Port die Rolle Lumel hat.
bool goodweSensorRequiredForLumel(const GoodweSensor& sensor);
// Abfragegruppe des Sensors: 1 = Intervall 1 (schnell), 2 = Intervall 2 (langsam)
uint8_t goodweSensorPollGroup(size_t sensorIndex);

// true, wenn der Wert in den Rohdaten vorhanden ist. Bei Statuscodes ist stateText gesetzt.
bool goodweSensorRead(const GoodweSensor& sensor, const GoodweRegisters& registers, float& value,
                      const char*& stateText);
// Anzahl Nachkommastellen für die Ausgabe
uint8_t goodweSensorDecimals(const GoodweSensor& sensor);
// Alle verfügbaren Werte als {id: wert} in jsonObject eintragen
void goodweSensorsToJson(cJSON* jsonObject, const GoodweRegisters& registers);
// Firmware-Kennung des Wechselrichters aus dem Geräteinfo-Block (Register 35021..35032)
void goodweReadFirmwareVersion(const GoodweRegisters& registers, char* firmwareVersion, size_t firmwareVersionSize);
// Modell und Seriennummer aus dem Geräteinfo-Block
void goodweReadDeviceStrings(const GoodweRegisters& registers, char* model, size_t modelSize,
                             char* serialNumber, size_t serialNumberSize);

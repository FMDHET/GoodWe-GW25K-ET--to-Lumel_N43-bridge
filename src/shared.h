#pragma once
// Daten, die zwischen den Tasks geteilt werden (Zugriff nur zwischen sharedLock()/sharedUnlock()).
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Messwerte im neutralen Format, so wie sie als Lumel N43 ausgegeben werden.
// Einheiten: V, A, W, var, VA, Hz, kWh. Index 0..2 = Phase L1..L3.
struct MeterData {
  float voltage[3] = {0, 0, 0};            // Phasenspannung L-N
  float current[3] = {0, 0, 0};
  float activePower[3] = {0, 0, 0};
  float reactivePower[3] = {0, 0, 0};
  float apparentPower[3] = {0, 0, 0};
  float powerFactor[3] = {0, 0, 0};
  float lineToLineVoltage[3] = {0, 0, 0};  // U12, U23, U31
  float frequency = 0;
  float totalActivePower = 0;
  float totalReactivePower = 0;
  float totalApparentPower = 0;
  float totalPowerFactor = 0;
  float importedEnergyKwh = 0;  // Bezug
  float exportedEnergyKwh = 0;  // Lieferung
  bool valid = false;
  uint32_t updatedAtMs = 0;
};

// Zusammenfassung des GoodWe für die Übersichtsseite
struct GoodweInfo {
  char model[20] = "";
  char firmwareVersion[28] = "";  // z. B. "04062-13-S0002071-17-449"
  char serialNumber[18] = "";
  float pvPower[4] = {0, 0, 0, 0};  // PV-String 1..4
  float pvPowerTotal = 0;
  float batteryVoltage = 0;
  float batteryCurrent = 0;
  float batteryPower = 0;
  float batterySoc = -1;      // Ladezustand Batterie 1 in %, -1 = unbekannt
  bool inverterPresent = true; // false, wenn der Wechselrichterblock abgeschaltet ist
  bool battery1Present = true; // false, wenn Batterie 1 in den Einstellungen abgeschaltet ist
  bool battery2Present = false;
  float battery2Voltage = 0;
  float battery2Current = 0;
  float battery2Power = 0;
  float battery2Soc = -1;     // Ladezustand Batterie 2 in %, -1 = unbekannt
  uint32_t cycleDurationMs = 0;      // tatsächlicher Abstand der schnellen Abfragen (Intervall 1)
  uint32_t slowCycleDurationMs = 0;  // tatsächlicher Abstand der vollständigen Abfragen (Intervall 2)
  float temperature = 0;
  float energyTotalKwh = 0;
  float energyTodayKwh = 0;
  uint16_t gridMode = 0;
  uint16_t meterCommStatus = 0;
  uint16_t meterRegisterCount = 0;  // 58, 45 oder 0
  bool valid = false;
};

// Registerblöcke des GoodWe (Startadresse und Anzahl der gelesenen Register)
static const uint16_t DEVICE_INFO_BLOCK_START = 35000;   // Geräteinfo (Modell, Seriennummer)
static const uint16_t DEVICE_INFO_REGISTER_COUNT = 68;  // 35000..35067 inkl. Modellname ab 35060
static const uint16_t INVERTER_BLOCK_START = 35100;      // Laufzeitdaten des Wechselrichters
static const uint16_t INVERTER_REGISTER_COUNT = 125;
static const uint16_t METER_BLOCK_START = 36000;         // Smart-Meter am Netzanschlusspunkt
static const uint16_t METER_REGISTER_COUNT_EXTENDED = 58; // neuere Firmware mit U/I je Phase
static const uint16_t METER_REGISTER_COUNT_SHORT = 45;    // ältere ARM-Firmware
static const uint16_t BMS_BLOCK_START = 37000;           // Batterie-Management Batterie 1
static const uint16_t BMS_REGISTER_COUNT = 24;
static const uint16_t BATTERY2_BLOCK_START = 35262;      // Batterie 2 am Wechselrichter (U, I, P, Modus)
static const uint16_t BATTERY2_REGISTER_COUNT = 5;
static const uint16_t BMS2_BLOCK_START = 39000;          // Batterie-Management Batterie 2
static const uint16_t BMS2_REGISTER_COUNT = 22;

// Rohregister aller GoodWe-Blöcke (Grundlage für Sensorliste, Web-Tabelle und MQTT)
struct GoodweRegisters {
  uint16_t deviceInfoRegisters[DEVICE_INFO_REGISTER_COUNT];
  uint16_t inverterRegisters[INVERTER_REGISTER_COUNT];
  uint16_t meterRegisters[METER_REGISTER_COUNT_EXTENDED];
  uint16_t bmsRegisters[BMS_REGISTER_COUNT];
  uint16_t battery2Registers[BATTERY2_REGISTER_COUNT];
  uint16_t bms2Registers[BMS2_REGISTER_COUNT];
  uint16_t meterRegisterCount = 0;
  bool deviceInfoValid = false;
  bool inverterValid = false;
  bool meterValid = false;
  bool bmsValid = false;
  bool battery2Valid = false;
  bool bms2Valid = false;
  uint32_t updatedAtMs = 0;
};

// Statistik je RS485-Port
struct PortStats {
  uint32_t requests = 0;   // Master: gesendete Anfragen / Slave: empfangene Anfragen
  uint32_t responses = 0;  // Master: gültige Antworten / Slave: gesendete Antworten
  uint32_t timeouts = 0;
  uint32_t crcErrors = 0;
  uint32_t exceptions = 0;
  uint32_t lastSuccessAtMs = 0;
  char lastError[64] = "";
  char lastRequest[64] = "";
};

extern MeterData g_meterData;
extern GoodweRegisters g_goodweRegisters;
extern GoodweInfo g_goodweInfo;
// Statistik-Einträge: [0] = RTU1, [1] = RTU2, [2] = GoodWe über Modbus TCP
static const int PORT_STATS_COUNT = 3;
static const int GOODWE_TCP_STATS_INDEX = 2;
extern PortStats g_portStats[PORT_STATS_COUNT];

// Legt den Mutex für die gemeinsamen Daten an (einmal beim Start).
void sharedInit();
// Sperrt die gemeinsamen Daten für den aktuellen Task (immer mit sharedUnlock() freigeben).
void sharedLock();
// Gibt die gemeinsamen Daten wieder frei.
void sharedUnlock();

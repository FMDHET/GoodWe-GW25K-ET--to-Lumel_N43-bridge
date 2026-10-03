// Sensortabelle des GoodWe ET/EH/BT/BH und das Auslesen einzelner Werte aus den Rohregistern.
// Die Tabelle ist die gemeinsame Grundlage für Web-Tabelle, MQTT und Home-Assistant-Discovery.
//
// Registerbelegung nach ARM-Protokoll, vgl. Python-Bibliothek "goodwe" (et.py).
// Block 35000: Geräteinfo, 35100: Laufzeitdaten, 36000: Smart-Meter, 37000: BMS
#include "goodwe_sensors.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "config.h"
#include "util.h"

// Klartexte für Statuscodes (Index = Rohwert)
static const char* const GRID_MODE_TEXTS[] = {"Nicht am Netz", "Am Netz", "Fehler"};
static const char* const BATTERY_MODE_TEXTS[] = {"Keine Batterie", "Standby", "Entladen", "Laden",
                                                 "Wartet auf Laden", "Wartet auf Entladen"};
static const char* const WORK_MODE_TEXTS[] = {"Warten", "Normal (Netz)", "Normal (Insel)", "Fehler",
                                              "Firmware-Update", "Prüfung"};
static const char* const LOAD_MODE_TEXTS[] = {"Aus", "An"};

// ---- Makros für die Sensortabelle
// Anzahl Einträge eines Text-Arrays
#define TEXT_COUNT(texts) (uint8_t)(sizeof(texts) / sizeof(texts[0]))

// Allgemeiner Sensor: id, Name, Block, Register, Datentyp, Teiler, Einheit, HA device_class, HA state_class
#define SENSOR(id, name, block, address, dataType, divisor, unit, deviceClass, stateClass) \
  {id, name, block, address, dataType, divisor, unit, deviceClass, stateClass, nullptr, 0, false}
// Diagnosewert ohne Einheit (HA entity_category "diagnostic")
#define DIAGNOSTIC_SENSOR(id, name, block, address, dataType) \
  {id, name, block, address, dataType, 1, nullptr, nullptr, STATE_CLASS_NONE, nullptr, 0, true}
// Statuscode mit Klartext (HA device_class "enum")
#define STATE_SENSOR(id, name, block, address, texts) \
  {id, name, block, address, TYPE_U16, 1, nullptr, "enum", STATE_CLASS_NONE, texts, TEXT_COUNT(texts), false}

// Häufige Messgrößen mit festem Typ, Teiler und Einheit
#define VOLTAGE_SENSOR(id, name, block, address) \
  SENSOR(id, name, block, address, TYPE_U16, 10, "V", "voltage", STATE_CLASS_MEASUREMENT)
#define CURRENT_SENSOR(id, name, block, address) \
  SENSOR(id, name, block, address, TYPE_U16, 10, "A", "current", STATE_CLASS_MEASUREMENT)
#define SIGNED_CURRENT_SENSOR(id, name, block, address) \
  SENSOR(id, name, block, address, TYPE_S16, 10, "A", "current", STATE_CLASS_MEASUREMENT)
#define FREQUENCY_SENSOR(id, name, block, address) \
  SENSOR(id, name, block, address, TYPE_U16, 100, "Hz", "frequency", STATE_CLASS_MEASUREMENT)
#define POWER_SENSOR(id, name, block, address) \
  SENSOR(id, name, block, address, TYPE_S32, 1, "W", "power", STATE_CLASS_MEASUREMENT)
#define UNSIGNED_POWER_SENSOR(id, name, block, address) \
  SENSOR(id, name, block, address, TYPE_U32, 1, "W", "power", STATE_CLASS_MEASUREMENT)
#define TEMPERATURE_SENSOR(id, name, block, address) \
  SENSOR(id, name, block, address, TYPE_S16, 10, "°C", "temperature", STATE_CLASS_MEASUREMENT)
#define ENERGY_SENSOR_32BIT(id, name, block, address) \
  SENSOR(id, name, block, address, TYPE_U32, 10, "kWh", "energy", STATE_CLASS_TOTAL_INCREASING)
#define ENERGY_SENSOR_16BIT(id, name, block, address) \
  SENSOR(id, name, block, address, TYPE_U16, 10, "kWh", "energy", STATE_CLASS_TOTAL_INCREASING)

const GoodweSensor GOODWE_SENSORS[] = {
    // ---- Geräteinfo
    SENSOR("rated_power", "Nennleistung", BLOCK_DEVICE_INFO, 35001, TYPE_U16, 1, "W", "power", STATE_CLASS_NONE),

    // ---- PV
    VOLTAGE_SENSOR("vpv1", "PV1 Spannung", BLOCK_INVERTER, 35103),
    CURRENT_SENSOR("ipv1", "PV1 Strom", BLOCK_INVERTER, 35104),
    UNSIGNED_POWER_SENSOR("ppv1", "PV1 Leistung", BLOCK_INVERTER, 35105),
    VOLTAGE_SENSOR("vpv2", "PV2 Spannung", BLOCK_INVERTER, 35107),
    CURRENT_SENSOR("ipv2", "PV2 Strom", BLOCK_INVERTER, 35108),
    UNSIGNED_POWER_SENSOR("ppv2", "PV2 Leistung", BLOCK_INVERTER, 35109),
    VOLTAGE_SENSOR("vpv3", "PV3 Spannung", BLOCK_INVERTER, 35111),
    CURRENT_SENSOR("ipv3", "PV3 Strom", BLOCK_INVERTER, 35112),
    UNSIGNED_POWER_SENSOR("ppv3", "PV3 Leistung", BLOCK_INVERTER, 35113),
    VOLTAGE_SENSOR("vpv4", "PV4 Spannung", BLOCK_INVERTER, 35115),
    CURRENT_SENSOR("ipv4", "PV4 Strom", BLOCK_INVERTER, 35116),
    UNSIGNED_POWER_SENSOR("ppv4", "PV4 Leistung", BLOCK_INVERTER, 35117),
    DIAGNOSTIC_SENSOR("pv_mode", "PV Modus", BLOCK_INVERTER, 35119, TYPE_U16),

    // ---- Netz (Wechselrichter-Ausgang)
    VOLTAGE_SENSOR("vgrid1", "Netz L1 Spannung", BLOCK_INVERTER, 35121),
    CURRENT_SENSOR("igrid1", "Netz L1 Strom", BLOCK_INVERTER, 35122),
    FREQUENCY_SENSOR("fgrid1", "Netz L1 Frequenz", BLOCK_INVERTER, 35123),
    POWER_SENSOR("pgrid1", "Netz L1 Leistung", BLOCK_INVERTER, 35124),
    VOLTAGE_SENSOR("vgrid2", "Netz L2 Spannung", BLOCK_INVERTER, 35126),
    CURRENT_SENSOR("igrid2", "Netz L2 Strom", BLOCK_INVERTER, 35127),
    FREQUENCY_SENSOR("fgrid2", "Netz L2 Frequenz", BLOCK_INVERTER, 35128),
    POWER_SENSOR("pgrid2", "Netz L2 Leistung", BLOCK_INVERTER, 35129),
    VOLTAGE_SENSOR("vgrid3", "Netz L3 Spannung", BLOCK_INVERTER, 35131),
    CURRENT_SENSOR("igrid3", "Netz L3 Strom", BLOCK_INVERTER, 35132),
    FREQUENCY_SENSOR("fgrid3", "Netz L3 Frequenz", BLOCK_INVERTER, 35133),
    POWER_SENSOR("pgrid3", "Netz L3 Leistung", BLOCK_INVERTER, 35134),
    STATE_SENSOR("grid_mode", "Netzstatus", BLOCK_INVERTER, 35136, GRID_MODE_TEXTS),
    POWER_SENSOR("total_inverter_power", "Wechselrichter Leistung", BLOCK_INVERTER, 35137),
    POWER_SENSOR("active_power", "Netzleistung", BLOCK_INVERTER, 35139),
    // am GW25K-ET geprüft: Blindleistung ab 35141, Scheinleistung ab 35143 (je 32 Bit)
    SENSOR("reactive_power", "Blindleistung", BLOCK_INVERTER, 35141, TYPE_S32, 1, "var", "reactive_power", STATE_CLASS_MEASUREMENT),
    SENSOR("apparent_power", "Scheinleistung", BLOCK_INVERTER, 35143, TYPE_S32, 1, "VA", "apparent_power", STATE_CLASS_MEASUREMENT),

    // ---- Backup / Notstrom
    VOLTAGE_SENSOR("backup_v1", "Backup L1 Spannung", BLOCK_INVERTER, 35145),
    CURRENT_SENSOR("backup_i1", "Backup L1 Strom", BLOCK_INVERTER, 35146),
    FREQUENCY_SENSOR("backup_f1", "Backup L1 Frequenz", BLOCK_INVERTER, 35147),
    STATE_SENSOR("load_mode1", "Backup L1 Modus", BLOCK_INVERTER, 35148, LOAD_MODE_TEXTS),
    POWER_SENSOR("backup_p1", "Backup L1 Leistung", BLOCK_INVERTER, 35149),
    VOLTAGE_SENSOR("backup_v2", "Backup L2 Spannung", BLOCK_INVERTER, 35151),
    CURRENT_SENSOR("backup_i2", "Backup L2 Strom", BLOCK_INVERTER, 35152),
    FREQUENCY_SENSOR("backup_f2", "Backup L2 Frequenz", BLOCK_INVERTER, 35153),
    STATE_SENSOR("load_mode2", "Backup L2 Modus", BLOCK_INVERTER, 35154, LOAD_MODE_TEXTS),
    POWER_SENSOR("backup_p2", "Backup L2 Leistung", BLOCK_INVERTER, 35155),
    VOLTAGE_SENSOR("backup_v3", "Backup L3 Spannung", BLOCK_INVERTER, 35157),
    CURRENT_SENSOR("backup_i3", "Backup L3 Strom", BLOCK_INVERTER, 35158),
    FREQUENCY_SENSOR("backup_f3", "Backup L3 Frequenz", BLOCK_INVERTER, 35159),
    STATE_SENSOR("load_mode3", "Backup L3 Modus", BLOCK_INVERTER, 35160, LOAD_MODE_TEXTS),
    POWER_SENSOR("backup_p3", "Backup L3 Leistung", BLOCK_INVERTER, 35161),
    POWER_SENSOR("load_p1", "Last L1", BLOCK_INVERTER, 35163),
    POWER_SENSOR("load_p2", "Last L2", BLOCK_INVERTER, 35165),
    POWER_SENSOR("load_p3", "Last L3", BLOCK_INVERTER, 35167),
    POWER_SENSOR("backup_ptotal", "Backup Leistung gesamt", BLOCK_INVERTER, 35169),
    POWER_SENSOR("load_ptotal", "Last gesamt", BLOCK_INVERTER, 35171),
    SENSOR("ups_load", "Backup Auslastung", BLOCK_INVERTER, 35173, TYPE_U16, 1, "%", nullptr, STATE_CLASS_MEASUREMENT),

    // ---- Temperaturen / Bus
    TEMPERATURE_SENSOR("temperature_air", "Temperatur Luft", BLOCK_INVERTER, 35174),
    TEMPERATURE_SENSOR("temperature_module", "Temperatur Modul", BLOCK_INVERTER, 35175),
    TEMPERATURE_SENSOR("temperature", "Temperatur Kühlkörper", BLOCK_INVERTER, 35176),
    DIAGNOSTIC_SENSOR("function_bit", "Funktionsbits", BLOCK_INVERTER, 35177, TYPE_U16),
    VOLTAGE_SENSOR("bus_voltage", "Busspannung", BLOCK_INVERTER, 35178),
    VOLTAGE_SENSOR("nbus_voltage", "NBus-Spannung", BLOCK_INVERTER, 35179),

    // ---- Batterie
    VOLTAGE_SENSOR("vbattery1", "Batterie 1 Spannung", BLOCK_INVERTER, 35180),
    SIGNED_CURRENT_SENSOR("ibattery1", "Batterie 1 Strom", BLOCK_INVERTER, 35181),
    POWER_SENSOR("pbattery1", "Batterie 1 Leistung", BLOCK_INVERTER, 35182),
    STATE_SENSOR("battery_mode", "Batterie 1 Modus", BLOCK_INVERTER, 35184, BATTERY_MODE_TEXTS),

    // ---- Status
    DIAGNOSTIC_SENSOR("warning_code", "Warncode", BLOCK_INVERTER, 35185, TYPE_U16),
    DIAGNOSTIC_SENSOR("safety_country", "Ländernorm", BLOCK_INVERTER, 35186, TYPE_U16),
    STATE_SENSOR("work_mode", "Betriebszustand", BLOCK_INVERTER, 35187, WORK_MODE_TEXTS),
    DIAGNOSTIC_SENSOR("operation_mode", "Betriebsmodus", BLOCK_INVERTER, 35188, TYPE_U16),
    DIAGNOSTIC_SENSOR("error_codes", "Fehlercodes", BLOCK_INVERTER, 35189, TYPE_U32),

    // ---- Energie
    ENERGY_SENSOR_32BIT("e_total", "PV Erzeugung gesamt", BLOCK_INVERTER, 35191),
    ENERGY_SENSOR_32BIT("e_day", "PV Erzeugung heute", BLOCK_INVERTER, 35193),
    ENERGY_SENSOR_32BIT("e_total_exp", "Einspeisung gesamt", BLOCK_INVERTER, 35195),
    SENSOR("h_total", "Betriebsstunden", BLOCK_INVERTER, 35197, TYPE_U32, 1, "h", "duration", STATE_CLASS_TOTAL_INCREASING),
    ENERGY_SENSOR_16BIT("e_day_exp", "Einspeisung heute", BLOCK_INVERTER, 35199),
    ENERGY_SENSOR_32BIT("e_total_imp", "Bezug gesamt", BLOCK_INVERTER, 35200),
    ENERGY_SENSOR_16BIT("e_day_imp", "Bezug heute", BLOCK_INVERTER, 35202),
    ENERGY_SENSOR_32BIT("e_load_total", "Verbrauch gesamt", BLOCK_INVERTER, 35203),
    ENERGY_SENSOR_16BIT("e_load_day", "Verbrauch heute", BLOCK_INVERTER, 35205),
    ENERGY_SENSOR_32BIT("e_bat_charge_total", "Batterie Ladung gesamt", BLOCK_INVERTER, 35206),
    ENERGY_SENSOR_16BIT("e_bat_charge_day", "Batterie Ladung heute", BLOCK_INVERTER, 35208),
    ENERGY_SENSOR_32BIT("e_bat_discharge_total", "Batterie Entladung gesamt", BLOCK_INVERTER, 35209),
    ENERGY_SENSOR_16BIT("e_bat_discharge_day", "Batterie Entladung heute", BLOCK_INVERTER, 35211),
    DIAGNOSTIC_SENSOR("diagnose_result", "Diagnose", BLOCK_INVERTER, 35220, TYPE_U32),

    // ---- Smart-Meter am Netzanschlusspunkt
    DIAGNOSTIC_SENSOR("meter_comm_status", "Meter Kommunikation", BLOCK_METER, 36004, TYPE_U16),
    SENSOR("meter_pf1", "Meter L1 Leistungsfaktor", BLOCK_METER, 36010, TYPE_S16, 1000, nullptr, "power_factor", STATE_CLASS_MEASUREMENT),
    SENSOR("meter_pf2", "Meter L2 Leistungsfaktor", BLOCK_METER, 36011, TYPE_S16, 1000, nullptr, "power_factor", STATE_CLASS_MEASUREMENT),
    SENSOR("meter_pf3", "Meter L3 Leistungsfaktor", BLOCK_METER, 36012, TYPE_S16, 1000, nullptr, "power_factor", STATE_CLASS_MEASUREMENT),
    SENSOR("meter_pf", "Meter Leistungsfaktor", BLOCK_METER, 36013, TYPE_S16, 1000, nullptr, "power_factor", STATE_CLASS_MEASUREMENT),
    FREQUENCY_SENSOR("meter_freq", "Meter Frequenz", BLOCK_METER, 36014),
    SENSOR("meter_e_total_exp", "Meter Einspeisung gesamt", BLOCK_METER, 36015, TYPE_FLOAT32, 1000, "kWh", "energy", STATE_CLASS_TOTAL_INCREASING),
    SENSOR("meter_e_total_imp", "Meter Bezug gesamt", BLOCK_METER, 36017, TYPE_FLOAT32, 1000, "kWh", "energy", STATE_CLASS_TOTAL_INCREASING),
    POWER_SENSOR("meter_p1", "Meter L1 Leistung", BLOCK_METER, 36019),
    POWER_SENSOR("meter_p2", "Meter L2 Leistung", BLOCK_METER, 36021),
    POWER_SENSOR("meter_p3", "Meter L3 Leistung", BLOCK_METER, 36023),
    POWER_SENSOR("meter_p", "Meter Leistung gesamt", BLOCK_METER, 36025),
    SENSOR("meter_q1", "Meter L1 Blindleistung", BLOCK_METER, 36027, TYPE_S32, 1, "var", "reactive_power", STATE_CLASS_MEASUREMENT),
    SENSOR("meter_q2", "Meter L2 Blindleistung", BLOCK_METER, 36029, TYPE_S32, 1, "var", "reactive_power", STATE_CLASS_MEASUREMENT),
    SENSOR("meter_q3", "Meter L3 Blindleistung", BLOCK_METER, 36031, TYPE_S32, 1, "var", "reactive_power", STATE_CLASS_MEASUREMENT),
    SENSOR("meter_q", "Meter Blindleistung gesamt", BLOCK_METER, 36033, TYPE_S32, 1, "var", "reactive_power", STATE_CLASS_MEASUREMENT),
    SENSOR("meter_s1", "Meter L1 Scheinleistung", BLOCK_METER, 36035, TYPE_S32, 1, "VA", "apparent_power", STATE_CLASS_MEASUREMENT),
    SENSOR("meter_s2", "Meter L2 Scheinleistung", BLOCK_METER, 36037, TYPE_S32, 1, "VA", "apparent_power", STATE_CLASS_MEASUREMENT),
    SENSOR("meter_s3", "Meter L3 Scheinleistung", BLOCK_METER, 36039, TYPE_S32, 1, "VA", "apparent_power", STATE_CLASS_MEASUREMENT),
    SENSOR("meter_s", "Meter Scheinleistung gesamt", BLOCK_METER, 36041, TYPE_S32, 1, "VA", "apparent_power", STATE_CLASS_MEASUREMENT),
    DIAGNOSTIC_SENSOR("meter_type", "Meter Typ", BLOCK_METER, 36043, TYPE_U16),
    DIAGNOSTIC_SENSOR("meter_sw_version", "Meter SW-Version", BLOCK_METER, 36044, TYPE_U16),
    VOLTAGE_SENSOR("meter_v1", "Meter L1 Spannung", BLOCK_METER, 36052),
    VOLTAGE_SENSOR("meter_v2", "Meter L2 Spannung", BLOCK_METER, 36053),
    VOLTAGE_SENSOR("meter_v3", "Meter L3 Spannung", BLOCK_METER, 36054),
    CURRENT_SENSOR("meter_i1", "Meter L1 Strom", BLOCK_METER, 36055),
    CURRENT_SENSOR("meter_i2", "Meter L2 Strom", BLOCK_METER, 36056),
    CURRENT_SENSOR("meter_i3", "Meter L3 Strom", BLOCK_METER, 36057),

    // ---- BMS
    DIAGNOSTIC_SENSOR("battery_bms", "BMS 1 Typ", BLOCK_BMS, 37000, TYPE_U16),
    DIAGNOSTIC_SENSOR("battery_status", "BMS 1 Status", BLOCK_BMS, 37002, TYPE_U16),
    TEMPERATURE_SENSOR("battery_temperature", "Batterie 1 Temperatur", BLOCK_BMS, 37003),
    SENSOR("battery_charge_limit", "Batterie 1 Ladestromgrenze", BLOCK_BMS, 37004, TYPE_U16, 1, "A", "current", STATE_CLASS_MEASUREMENT),
    SENSOR("battery_discharge_limit", "Batterie 1 Entladestromgrenze", BLOCK_BMS, 37005, TYPE_U16, 1, "A", "current", STATE_CLASS_MEASUREMENT),
    DIAGNOSTIC_SENSOR("battery_error", "BMS 1 Fehler", BLOCK_BMS, 37006, TYPE_U16),
    SENSOR("battery_soc", "Batterie 1 Ladezustand", BLOCK_BMS, 37007, TYPE_U16, 1, "%", "battery", STATE_CLASS_MEASUREMENT),
    SENSOR("battery_soh", "Batterie 1 Gesundheit", BLOCK_BMS, 37008, TYPE_U16, 1, "%", nullptr, STATE_CLASS_MEASUREMENT),
    DIAGNOSTIC_SENSOR("battery_modules", "Batterie 1 Module", BLOCK_BMS, 37009, TYPE_U16),
    DIAGNOSTIC_SENSOR("battery_warning", "BMS 1 Warnung", BLOCK_BMS, 37010, TYPE_U16),
    DIAGNOSTIC_SENSOR("battery_protocol", "BMS 1 Protokoll", BLOCK_BMS, 37011, TYPE_U16),

    // ---- Batterie 2 (am GW25K-ET geprüft; vgl. Python-Bibliothek goodwe, et.py)
    VOLTAGE_SENSOR("vbattery2", "Batterie 2 Spannung", BLOCK_BATTERY2, 35262),
    SIGNED_CURRENT_SENSOR("ibattery2", "Batterie 2 Strom", BLOCK_BATTERY2, 35263),
    POWER_SENSOR("pbattery2", "Batterie 2 Leistung", BLOCK_BATTERY2, 35264),
    STATE_SENSOR("battery2_mode", "Batterie 2 Modus", BLOCK_BATTERY2, 35266, BATTERY_MODE_TEXTS),
    DIAGNOSTIC_SENSOR("battery2_status", "BMS 2 Status", BLOCK_BMS2, 39000, TYPE_U16),
    TEMPERATURE_SENSOR("battery2_temperature", "Batterie 2 Temperatur", BLOCK_BMS2, 39001),
    SENSOR("battery2_charge_limit", "Batterie 2 Ladestromgrenze", BLOCK_BMS2, 39002, TYPE_U16, 1, "A", "current", STATE_CLASS_MEASUREMENT),
    SENSOR("battery2_discharge_limit", "Batterie 2 Entladestromgrenze", BLOCK_BMS2, 39003, TYPE_U16, 1, "A", "current", STATE_CLASS_MEASUREMENT),
    DIAGNOSTIC_SENSOR("battery2_error_l", "BMS 2 Fehler", BLOCK_BMS2, 39004, TYPE_U16),
    SENSOR("battery2_soc", "Batterie 2 Ladezustand", BLOCK_BMS2, 39005, TYPE_U16, 1, "%", "battery", STATE_CLASS_MEASUREMENT),
    SENSOR("battery2_soh", "Batterie 2 Gesundheit", BLOCK_BMS2, 39006, TYPE_U16, 1, "%", nullptr, STATE_CLASS_MEASUREMENT),
    DIAGNOSTIC_SENSOR("battery2_modules", "Batterie 2 Module", BLOCK_BMS2, 39007, TYPE_U16),
    DIAGNOSTIC_SENSOR("battery2_warning_l", "BMS 2 Warnung", BLOCK_BMS2, 39008, TYPE_U16),
    DIAGNOSTIC_SENSOR("battery2_protocol", "BMS 2 Protokoll", BLOCK_BMS2, 39009, TYPE_U16),
    DIAGNOSTIC_SENSOR("battery2_sw_version", "BMS 2 Softwareversion", BLOCK_BMS2, 39012, TYPE_U16),
};
const size_t GOODWE_SENSOR_COUNT = sizeof(GOODWE_SENSORS) / sizeof(GOODWE_SENSORS[0]);
static_assert(sizeof(GOODWE_SENSORS) / sizeof(GOODWE_SENSORS[0]) <= 256,
              "MQTT-Bitmaske der angekündigten Sensoren (mqtt.cpp) vergrößern");

// Liefert zu einem Registerblock den Puffer (registers), dessen Startadresse und die Anzahl
// gelesener Register. Rückgabe: true, wenn der Block aktuell gültige Daten enthält.
static bool getBlockData(const GoodweRegisters& allRegisters, uint8_t block, const uint16_t*& registers,
                         uint16_t& blockStart, uint16_t& registerCount) {
  switch (block) {
    case BLOCK_DEVICE_INFO:
      registers = allRegisters.deviceInfoRegisters;
      blockStart = DEVICE_INFO_BLOCK_START;
      registerCount = DEVICE_INFO_REGISTER_COUNT;
      return allRegisters.deviceInfoValid;
    case BLOCK_INVERTER:
      registers = allRegisters.inverterRegisters;
      blockStart = INVERTER_BLOCK_START;
      registerCount = INVERTER_REGISTER_COUNT;
      return allRegisters.inverterValid;
    case BLOCK_METER:
      registers = allRegisters.meterRegisters;
      blockStart = METER_BLOCK_START;
      registerCount = allRegisters.meterRegisterCount;  // 58 oder 45, je nach Firmware
      return allRegisters.meterValid;
    case BLOCK_BMS:
      registers = allRegisters.bmsRegisters;
      blockStart = BMS_BLOCK_START;
      registerCount = BMS_REGISTER_COUNT;
      return allRegisters.bmsValid;
    case BLOCK_BATTERY2:
      registers = allRegisters.battery2Registers;
      blockStart = BATTERY2_BLOCK_START;
      registerCount = BATTERY2_REGISTER_COUNT;
      return allRegisters.battery2Valid;
    case BLOCK_BMS2:
      registers = allRegisters.bms2Registers;
      blockStart = BMS2_BLOCK_START;
      registerCount = BMS2_REGISTER_COUNT;
      return allRegisters.bms2Valid;
  }
  return false;
}

static_assert(sizeof(GOODWE_SENSORS) / sizeof(GOODWE_SENSORS[0]) <= MAX_GOODWE_SENSORS,
              "Bitfeld goodweFastSensorBits in config.h vergrößern");

// Werte von Batterie 1, die im Wechselrichterblock liegen
static const char* const BATTERY1_SENSORS_IN_INVERTER_BLOCK[] = {"vbattery1", "ibattery1", "pbattery1", "battery_mode"};

uint8_t goodweSensorDevice(const GoodweSensor& sensor) {
  switch (sensor.block) {
    case BLOCK_DEVICE_INFO: return DEVICE_INFO;
    case BLOCK_METER: return DEVICE_METER;
    case BLOCK_BMS: return DEVICE_BATTERY1;
    case BLOCK_BATTERY2:
    case BLOCK_BMS2: return DEVICE_BATTERY2;
  }
  for (const char* sensorId : BATTERY1_SENSORS_IN_INVERTER_BLOCK)
    if (!strcmp(sensor.id, sensorId)) return DEVICE_BATTERY1;
  return DEVICE_INVERTER;
}

const char* goodweDeviceKey(uint8_t device) {
  static const char* const DEVICE_KEYS[GOODWE_DEVICE_COUNT] = {"info", "inverter", "meter", "battery1", "battery2"};
  return device < GOODWE_DEVICE_COUNT ? DEVICE_KEYS[device] : nullptr;
}

bool goodweDeviceEnabled(uint8_t device) { return !(g_config.goodweDisabledDevices & (1 << device)); }

int goodweSensorIndexById(const char* sensorId) {
  for (size_t sensorIndex = 0; sensorIndex < GOODWE_SENSOR_COUNT; sensorIndex++)
    if (!strcmp(GOODWE_SENSORS[sensorIndex].id, sensorId)) return (int)sensorIndex;
  return -1;
}

uint8_t goodweSensorRegisterCount(const GoodweSensor& sensor) {
  return (sensor.dataType == TYPE_U16 || sensor.dataType == TYPE_S16) ? 1 : 2;
}

// Werte, aus denen die Lumel-Emulation ihre Messwerte bildet (vgl. decodeFromMeter/decodeFromInverter
// in goodwe.cpp).
static const char* const LUMEL_SENSORS_FROM_METER[] = {
    "meter_freq", "meter_e_total_exp", "meter_e_total_imp", "meter_p1", "meter_p2", "meter_p3", "meter_p",
    "meter_q1", "meter_q2", "meter_q3", "meter_q", "meter_s1", "meter_s2", "meter_s3", "meter_s",
    "meter_v1", "meter_v2", "meter_v3", "meter_i1", "meter_i2", "meter_i3"};
// nur nötig, wenn der Smart-Meter-Block keine Spannungen enthält (kurzer Block älterer Firmware)
static const char* const LUMEL_VOLTAGES_FROM_INVERTER[] = {"vgrid1", "vgrid2", "vgrid3"};
static const char* const LUMEL_SENSORS_FROM_INVERTER[] = {
    "vgrid1", "igrid1", "fgrid1", "pgrid1", "vgrid2", "igrid2", "pgrid2", "vgrid3", "igrid3", "pgrid3",
    "total_inverter_power", "reactive_power", "apparent_power", "e_total"};

bool goodweSensorRequiredForLumel(const GoodweSensor& sensor) {
  bool lumelActive = g_config.port[0].role == ROLE_LUMEL_SLAVE || g_config.port[1].role == ROLE_LUMEL_SLAVE;
  if (!lumelActive) return false;
  if (g_config.goodweDataSource == GOODWE_SOURCE_METER) {
    for (const char* sensorId : LUMEL_SENSORS_FROM_METER)
      if (!strcmp(sensor.id, sensorId)) return true;
    if (g_goodweRegisters.meterRegisterCount < METER_REGISTER_COUNT_EXTENDED)
      for (const char* sensorId : LUMEL_VOLTAGES_FROM_INVERTER)
        if (!strcmp(sensor.id, sensorId)) return true;
  } else {
    for (const char* sensorId : LUMEL_SENSORS_FROM_INVERTER)
      if (!strcmp(sensor.id, sensorId)) return true;
  }
  return false;
}

uint8_t goodweSensorPollGroup(size_t sensorIndex) {
  const GoodweSensor& sensor = GOODWE_SENSORS[sensorIndex];
  return isGoodweSensorFast(g_config, sensorIndex) || goodweSensorRequiredForLumel(sensor) ? 1 : 2;
}

// Liest den Wert eines Sensors aus den Rohregistern und rechnet ihn mit dem Teiler um.
// value: physikalischer Wert; stateText: Klartext bei Statuscodes, sonst nullptr.
// Rückgabe: false, wenn der Block ungültig ist oder das Register außerhalb des gelesenen Bereichs liegt.
bool goodweSensorRead(const GoodweSensor& sensor, const GoodweRegisters& registers, float& value,
                      const char*& stateText) {
  const uint16_t* blockRegisters;
  uint16_t blockStart, registerCount;
  stateText = nullptr;
  if (!goodweDeviceEnabled(goodweSensorDevice(sensor))) return false;  // abgeschaltetes Gerät ausblenden
  if (!getBlockData(registers, sensor.block, blockRegisters, blockStart, registerCount)) return false;

  // Liegt das Register (1 oder 2 Wörter) im gelesenen Bereich?
  uint16_t wordCount = (sensor.dataType == TYPE_U16 || sensor.dataType == TYPE_S16) ? 1 : 2;
  if (sensor.registerAddress < blockStart || sensor.registerAddress - blockStart + wordCount > registerCount)
    return false;

  // Rohwert dekodieren (32 Bit: höherwertiges Wort zuerst)
  const uint16_t* sensorRegisters = blockRegisters + (sensor.registerAddress - blockStart);
  uint32_t raw32 = wordCount == 2 ? ((uint32_t)sensorRegisters[0] << 16) | sensorRegisters[1] : sensorRegisters[0];
  switch (sensor.dataType) {
    case TYPE_U16: value = sensorRegisters[0]; break;
    case TYPE_S16: value = (int16_t)sensorRegisters[0]; break;
    case TYPE_U32: value = raw32; break;
    case TYPE_S32: value = (int32_t)raw32; break;
    case TYPE_FLOAT32:
      memcpy(&value, &raw32, 4);
      if (!std::isfinite(value)) value = 0;
      break;
  }
  value /= sensor.divisor;

  // Statuscode in Klartext übersetzen; unbekannte Codes als "Code <n>"
  if (sensor.stateTexts) {
    uint16_t stateCode = sensorRegisters[0];
    stateText = stateCode < sensor.stateTextCount ? sensor.stateTexts[stateCode] : nullptr;
    if (!stateText) {
      // Ringpuffer, damit mehrere unbekannte Codes gleichzeitig gültig bleiben (Rückgabe als Zeiger)
      static char unknownCodeTexts[8][16];
      static uint8_t nextSlot = 0;
      char* unknownCodeText = unknownCodeTexts[nextSlot++ & 7];
      snprintf(unknownCodeText, 16, "Code %u", stateCode);
      stateText = unknownCodeText;
    }
  }
  return true;
}

// Liefert die Anzahl Nachkommastellen für die Ausgabe, abgeleitet aus dem Teiler
// (Float-Werte immer mit 3 Stellen).
uint8_t goodweSensorDecimals(const GoodweSensor& sensor) {
  if (sensor.dataType == TYPE_FLOAT32) return 3;
  if (sensor.divisor >= 1000) return 3;
  if (sensor.divisor >= 100) return 2;
  if (sensor.divisor >= 10) return 1;
  return 0;
}

// Trägt alle aktuell lesbaren Sensorwerte als {id: wert} in jsonObject ein.
// Statuscodes als Text, alle anderen als Zahl mit passender Nachkommastellenzahl.
void goodweSensorsToJson(cJSON* jsonObject, const GoodweRegisters& registers) {
  for (size_t sensorIndex = 0; sensorIndex < GOODWE_SENSOR_COUNT; sensorIndex++) {
    const GoodweSensor& sensor = GOODWE_SENSORS[sensorIndex];
    float value;
    const char* stateText;
    if (!goodweSensorRead(sensor, registers, value, stateText)) continue;
    if (stateText) cJSON_AddStringToObject(jsonObject, sensor.id, stateText);
    else cJSON_AddItemToObject(jsonObject, sensor.id, jsonNumberWithDecimals(value, goodweSensorDecimals(sensor)));
  }
}

// Dekodiert wordCount Register ab firstRegister des Geräteinfo-Blocks als ASCII-Text
// (2 Zeichen je Register). Nicht druckbare Zeichen (0x00, 0xFF als Füllung) werden übersprungen,
// Leerzeichen am Ende entfernt. Ist der Block ungültig, bleibt der Text leer.
static void decodeDeviceInfoText(const GoodweRegisters& registers, uint16_t firstRegister, int wordCount,
                                 char* text, size_t textSize) {
  size_t textLength = 0;
  int firstWordIndex = firstRegister - DEVICE_INFO_BLOCK_START;
  for (int wordIndex = 0; wordIndex < wordCount && registers.deviceInfoValid; wordIndex++) {
    uint16_t word = registers.deviceInfoRegisters[firstWordIndex + wordIndex];
    char characters[2] = {(char)(word >> 8), (char)(word & 0xFF)};
    for (char character : characters)
      if (character >= 0x20 && character < 0x7F && textLength + 1 < textSize) text[textLength++] = character;
  }
  while (textLength > 0 && text[textLength - 1] == ' ') textLength--;
  text[textLength] = 0;
}

// Leitet den Modellnamen aus der Seriennummer ab (nur als Notlösung, wenn die Firmware keinen
// Namen liefert): GoodWe-Seriennummern enthalten ab Stelle 3 Leistung und Baureihe,
// z. B. "9025KETT..." -> "GW25K-ET". Sonst wird die Nennleistung aus Register 35001 angezeigt.
static void deriveModelFromSerialNumber(const GoodweRegisters& registers, const char* serialNumber, char* model,
                                        size_t modelSize) {
  const char* cursor = serialNumber + 2;
  char powerDigits[4] = "";
  size_t digitCount = 0;
  while (digitCount < 3 && cursor[digitCount] >= '0' && cursor[digitCount] <= '9') {
    powerDigits[digitCount] = cursor[digitCount];
    digitCount++;
  }
  const char* afterDigits = cursor + digitCount;
  bool looksLikeModel = digitCount > 0 && afterDigits[0] == 'K' && afterDigits[1] >= 'A' && afterDigits[1] <= 'Z' &&
                        afterDigits[2] >= 'A' && afterDigits[2] <= 'Z';
  if (looksLikeModel) {
    snprintf(model, modelSize, "GW%sK-%c%c", powerDigits, afterDigits[1], afterDigits[2]);
  } else {
    unsigned ratedPowerKw = registers.deviceInfoRegisters[35001 - DEVICE_INFO_BLOCK_START] / 1000;
    if (ratedPowerKw) snprintf(model, modelSize, "%u kW", ratedPowerKw);
  }
}

// Liest Modellname und Seriennummer aus dem Geräteinfo-Block.
// Modellname (am GW25K-ET geprüft): Register 35060..35067; ältere Firmware: 35011..35015;
// fehlt beides, wird er aus der Seriennummer abgeleitet.
void goodweReadDeviceStrings(const GoodweRegisters& registers, char* model, size_t modelSize,
                             char* serialNumber, size_t serialNumberSize) {
  decodeDeviceInfoText(registers, 35003, 8, serialNumber, serialNumberSize);  // 35003..35010
  decodeDeviceInfoText(registers, 35060, 8, model, modelSize);                // 35060..35067
  if (!model[0]) decodeDeviceInfoText(registers, 35011, 5, model, modelSize);  // 35011..35015
  if (!model[0] && registers.deviceInfoValid && serialNumber[0])
    deriveModelFromSerialNumber(registers, serialNumber, model, modelSize);
}

// Liest die Firmware-Kennung des Wechselrichters (Register 35021..35032, 24 Zeichen).
void goodweReadFirmwareVersion(const GoodweRegisters& registers, char* firmwareVersion, size_t firmwareVersionSize) {
  decodeDeviceInfoText(registers, 35021, 12, firmwareVersion, firmwareVersionSize);
}

// Modbus-Master für den GoodWe-Wechselrichter: fragt zyklisch die Registerblöcke ab, stellt die
// Rohregister bereit und rechnet daraus die Zählerwerte für die Lumel-Emulation und die Übersicht.
//
// GoodWe ET-Serie (z.B. GW25K-ET) per Modbus RTU auslesen.
// Registeradressen nach GoodWe-ARM-Protokoll (vgl. Python-Lib "goodwe", et.py):
//   35000.. (16 Register): Geräteinfo (Modell, Seriennummer)
//   35100.. (125 Register): Wechselrichter-Laufzeitdaten
//   36000.. (58 bzw. 45 Register): Daten des Smart-Meters (GM3000) am Netzanschlusspunkt
//   37000.. (24 Register): Batterie/BMS (optional)
#include "goodwe.h"
#include "config.h"
#include "modbus_rtu.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "esp_task_wdt.h"
#include "freertos/semphr.h"
#include "goodwe_sensors.h"
#include "ota.h"
#include "shared.h"
#include "util.h"

// Registerblöcke
// Blockadressen und -längen: siehe shared.h

static const uint8_t FUNCTION_READ_HOLDING_REGISTERS = 3;

// Zeiten
static const uint32_t PAUSE_BETWEEN_REQUESTS_MS = 30;
static const uint32_t DEVICE_INFO_RETRY_MS = 5000;
static const uint32_t UNSUPPORTED_BLOCK_RETRY_MS = 60000;  // Block hat mit Exception geantwortet

// Fehlerschwellen (aufeinanderfolgende Zyklen ohne verwertbare Daten)
static const uint8_t FAILURES_UNTIL_INVALID = 3;
static const uint8_t FAILURES_UNTIL_DEVICE_INFO_RELOAD = 10;

static ModbusMaster* modbusMaster = nullptr;  // RS485-Port oder Modbus-TCP-Client
static uint8_t portIndex = 0;                 // Index in g_portStats (0/1 = RTU-Port, 2 = TCP)
static uint32_t fastCyclePeriodMs = 0;        // tatsächlicher Abstand der Zyklen (Intervall 1)
static uint32_t slowCyclePeriodMs = 0;
static uint32_t nextFastPollAtMs = 0;         // geplanter Start des nächsten Zyklus (0 = kein GoodWe)
static uint32_t nextSlowPollAtMs = 0;         // geplanter Start der nächsten vollständigen Abfrage        // tatsächlicher Abstand der vollständigen Abfragen (Intervall 2)
static uint8_t goodweSlaveAddress = 247;      // Modbus-Adresse bzw. Unit-ID des GoodWe
static SemaphoreHandle_t masterLock = nullptr; // schützt modbusMaster (GoodWe-Task und Modbus-TCP-Server)

// Empfangspuffer (statisch statt auf dem Task-Stack)
static uint16_t deviceInfoRegisters[DEVICE_INFO_REGISTER_COUNT];
static uint16_t inverterRegisters[INVERTER_REGISTER_COUNT];
static uint16_t meterRegisters[METER_REGISTER_COUNT_EXTENDED];
static uint16_t bmsRegisters[BMS_REGISTER_COUNT];
static uint16_t battery2Registers[BATTERY2_REGISTER_COUNT];
static uint16_t bms2Registers[BMS2_REGISTER_COUNT];


// ---- Zugriff auf Register innerhalb eines gelesenen Blocks
// Alle Helfer bekommen den Blockpuffer, dessen Startadresse und die absolute Registeradresse.

// Liefert ein Register als vorzeichenlosen 16-Bit-Wert.
static inline uint16_t registerU16(const uint16_t* registers, uint16_t blockStart, uint16_t address) {
  return registers[address - blockStart];
}
// Liefert ein Register als vorzeichenbehafteten 16-Bit-Wert.
static inline int16_t registerS16(const uint16_t* registers, uint16_t blockStart, uint16_t address) {
  return (int16_t)registers[address - blockStart];
}
// Liefert zwei Register als vorzeichenlosen 32-Bit-Wert (höherwertiges Wort zuerst).
static inline uint32_t registerU32(const uint16_t* registers, uint16_t blockStart, uint16_t address) {
  return ((uint32_t)registers[address - blockStart] << 16) | registers[address - blockStart + 1];
}
// Liefert zwei Register als vorzeichenbehafteten 32-Bit-Wert.
static inline int32_t registerS32(const uint16_t* registers, uint16_t blockStart, uint16_t address) {
  return (int32_t)registerU32(registers, blockStart, address);
}
// Liefert zwei Register als IEEE-754-Float; NaN/Unendlich werden zu 0, damit keine
// ungültigen Werte in JSON oder Modbus-Antworten landen.
static inline float registerFloat32(const uint16_t* registers, uint16_t blockStart, uint16_t address) {
  uint32_t rawBits = registerU32(registers, blockStart, address);
  float value;
  memcpy(&value, &rawBits, 4);
  return std::isfinite(value) ? value : 0.0f;
}

// Berechnet die Leiter-Leiter-Spannung aus zwei Phasenspannungen (120° Versatz angenommen),
// weil der GoodWe nur Phasenspannungen liefert.
static float lineToLineVoltage(float phaseVoltageA, float phaseVoltageB) {
  return sqrtf(phaseVoltageA * phaseVoltageA + phaseVoltageB * phaseVoltageB + phaseVoltageA * phaseVoltageB);
}

// Prüft, ob ein Block wieder abgefragt werden darf.
// Rückgabe: true, wenn kein Wiederholzeitpunkt gesetzt ist (0) oder er erreicht wurde.
// Der Vergleich über int32_t bleibt auch beim Überlauf des Millisekundenzählers korrekt.
static bool isRetryDue(uint32_t retryAtMs) {
  return retryAtMs == 0 || (int32_t)(millisSinceBoot() - retryAtMs) >= 0;
}

// Liest registerCount Holding-Register (FC03) ab startAddress vom GoodWe in values.
// Timeout kommt aus der Konfiguration. Rückgabe: ModbusResult bzw. Exception-Code.
// Die Verbindung wird mit dem Modbus-TCP-Server geteilt; die Sperre verhindert gleichzeitige Anfragen.
static int readHoldingRegisters(uint16_t startAddress, uint16_t registerCount, uint16_t* values) {
  xSemaphoreTake(masterLock, portMAX_DELAY);
  int result = modbusMaster->readRegisters(goodweSlaveAddress, FUNCTION_READ_HOLDING_REGISTERS, startAddress,
                                           registerCount, values, g_config.goodweTimeoutMs);
  xSemaphoreGive(masterLock);
  return result;
}

int goodweForwardPdu(uint8_t unitId, const uint8_t* requestPdu, size_t requestLength, uint8_t* responsePdu,
                     size_t& responseLength) {
  if (!modbusMaster) return GOODWE_NOT_CONFIGURED;
  if (unitId == 0 || unitId == 255) unitId = goodweSlaveAddress;
  // nicht ewig warten, falls die GoodWe-Abfrage gerade einen langen Block liest
  if (xSemaphoreTake(masterLock, pdMS_TO_TICKS(3000)) != pdTRUE) return MODBUS_ERROR_TIMEOUT;
  int result = modbusMaster->transact(unitId, requestPdu, requestLength, responsePdu, responseLength,
                                      g_config.goodweTimeoutMs);
  xSemaphoreGive(masterLock);
  return result;
}

// Kurze Pause zwischen zwei Anfragen, damit der GoodWe zwischen den Blöcken Luft hat.
static void pauseBetweenRequests() { vTaskDelay(pdMS_TO_TICKS(PAUSE_BETWEEN_REQUESTS_MS)); }

// Trägt das Ergebnis einer Anfrage in die Port-Statistik ein (Zähler, letzter Erfolg, letzter Fehler).
// requestName erscheint im Fehlertext, z.B. "36000: Timeout".
static void recordResult(int result, const char* requestName) {
  PortStats& stats = g_portStats[portIndex];
  sharedLock();
  stats.requests++;
  if (result == MODBUS_OK) {
    stats.responses++;
    stats.lastSuccessAtMs = millisSinceBoot();
  } else {
    if (result == MODBUS_ERROR_TIMEOUT) stats.timeouts++;
    else if (result == MODBUS_ERROR_CRC) stats.crcErrors++;
    else if (result > 0) stats.exceptions++;
    snprintf(stats.lastError, sizeof(stats.lastError), "%s: %s", requestName, modbusResultText(result));
  }
  sharedUnlock();
}

// ---- Dekodierung

// Füllt die Übersichtsdaten (PV-Leistung, Batterie, Temperatur, Energie, Netzstatus)
// aus dem Wechselrichterblock 35100.
static void decodeInverterInfo(const uint16_t* inverter, GoodweInfo& info) {
  info.pvPowerTotal = 0;
  for (int stringIndex = 0; stringIndex < 4; stringIndex++) {
    info.pvPower[stringIndex] = (float)registerU32(inverter, INVERTER_BLOCK_START, 35105 + 4 * stringIndex);
    info.pvPowerTotal += info.pvPower[stringIndex];
  }
  info.gridMode = registerU16(inverter, INVERTER_BLOCK_START, 35136);
  info.temperature = registerS16(inverter, INVERTER_BLOCK_START, 35176) / 10.0f;
  info.batteryVoltage = registerU16(inverter, INVERTER_BLOCK_START, 35180) / 10.0f;
  info.batteryCurrent = registerS16(inverter, INVERTER_BLOCK_START, 35181) / 10.0f;
  info.batteryPower = (float)registerS32(inverter, INVERTER_BLOCK_START, 35182);
  info.energyTotalKwh = registerU32(inverter, INVERTER_BLOCK_START, 35191) / 10.0f;
  info.energyTodayKwh = registerU32(inverter, INVERTER_BLOCK_START, 35193) / 10.0f;
  info.valid = true;
}

// Füllt meter mit den Netzwerten des Wechselrichter-Ausgangs (Datenquelle "Wechselrichter").
// Fehlende Größen (Scheinleistung und Blindleistung je Phase) werden berechnet.
static void decodeFromInverter(const uint16_t* inverter, MeterData& meter) {
  for (int phaseIndex = 0; phaseIndex < 3; phaseIndex++) {
    uint16_t phaseStart = 35121 + 5 * phaseIndex;  // 35121 / 35126 / 35131
    meter.voltage[phaseIndex] = registerU16(inverter, INVERTER_BLOCK_START, phaseStart) / 10.0f;
    meter.current[phaseIndex] = registerU16(inverter, INVERTER_BLOCK_START, phaseStart + 1) / 10.0f;
    meter.activePower[phaseIndex] = (float)registerS32(inverter, INVERTER_BLOCK_START, phaseStart + 3);
    meter.apparentPower[phaseIndex] = meter.voltage[phaseIndex] * meter.current[phaseIndex];
  }
  meter.frequency = registerU16(inverter, INVERTER_BLOCK_START, 35123) / 100.0f;
  meter.totalActivePower = (float)registerS32(inverter, INVERTER_BLOCK_START, 35137);
  // am GW25K-ET geprüft: Blindleistung ab 35141, Scheinleistung ab 35143 (je 32 Bit)
  meter.totalReactivePower = (float)registerS32(inverter, INVERTER_BLOCK_START, 35141);
  meter.totalApparentPower = fabsf((float)registerS32(inverter, INVERTER_BLOCK_START, 35143));
  // Blindleistung je Phase liefert der WR nicht: aus S und P berechnen, Vorzeichen der Summe übernehmen
  for (int phaseIndex = 0; phaseIndex < 3; phaseIndex++) {
    float reactivePowerSquared = meter.apparentPower[phaseIndex] * meter.apparentPower[phaseIndex] -
                                 meter.activePower[phaseIndex] * meter.activePower[phaseIndex];
    meter.reactivePower[phaseIndex] =
        reactivePowerSquared > 0 ? copysignf(sqrtf(reactivePowerSquared), meter.totalReactivePower) : 0;
  }
  meter.importedEnergyKwh = registerU32(inverter, INVERTER_BLOCK_START, 35191) / 10.0f;  // E-Total des Wechselrichters
  meter.exportedEnergyKwh = 0;
}

// Füllt meter mit den Werten des Smart-Meters am Netzanschlusspunkt (Datenquelle "Zähler").
// meterRegisterCount ist 58 oder 45; beim kurzen Block fehlen Spannung/Strom je Phase,
// dann wird auf den Wechselrichterblock zurückgegriffen.
static void decodeFromMeter(const uint16_t* inverter, const uint16_t* meterBlock, uint16_t meterRegisterCount,
                            MeterData& meter) {
  meter.frequency = registerU16(meterBlock, METER_BLOCK_START, 36014) / 100.0f;
  meter.exportedEnergyKwh = registerFloat32(meterBlock, METER_BLOCK_START, 36015) / 1000.0f;
  meter.importedEnergyKwh = registerFloat32(meterBlock, METER_BLOCK_START, 36017) / 1000.0f;
  // Manche Firmware (z. B. GW25K-ET 04062-13) liefert hier immer 0: dann die Zählerstände des
  // Wechselrichters verwenden (Einspeisung 35195, Bezug 35200, je 0,1 kWh; Intervall 2 genügt)
  if (meter.exportedEnergyKwh == 0 && meter.importedEnergyKwh == 0 && goodweDeviceEnabled(DEVICE_INVERTER)) {
    meter.exportedEnergyKwh = registerU32(inverter, INVERTER_BLOCK_START, 35195) / 10.0f;
    meter.importedEnergyKwh = registerU32(inverter, INVERTER_BLOCK_START, 35200) / 10.0f;
  }
  for (int phaseIndex = 0; phaseIndex < 3; phaseIndex++) {
    meter.activePower[phaseIndex] = (float)registerS32(meterBlock, METER_BLOCK_START, 36019 + 2 * phaseIndex);
    meter.reactivePower[phaseIndex] = (float)registerS32(meterBlock, METER_BLOCK_START, 36027 + 2 * phaseIndex);
    meter.apparentPower[phaseIndex] = fabsf((float)registerS32(meterBlock, METER_BLOCK_START, 36035 + 2 * phaseIndex));
  }
  meter.totalActivePower = (float)registerS32(meterBlock, METER_BLOCK_START, 36025);
  meter.totalReactivePower = (float)registerS32(meterBlock, METER_BLOCK_START, 36033);
  meter.totalApparentPower = fabsf((float)registerS32(meterBlock, METER_BLOCK_START, 36041));

  bool hasVoltageAndCurrent = meterRegisterCount >= METER_REGISTER_COUNT_EXTENDED &&
                              registerU16(meterBlock, METER_BLOCK_START, 36052) > 0;
  for (int phaseIndex = 0; phaseIndex < 3; phaseIndex++) {
    if (hasVoltageAndCurrent) {
      meter.voltage[phaseIndex] = registerU16(meterBlock, METER_BLOCK_START, 36052 + phaseIndex) / 10.0f;
      meter.current[phaseIndex] = registerU16(meterBlock, METER_BLOCK_START, 36055 + phaseIndex) / 10.0f;
    } else {
      // ältere ARM-Firmware: Spannung vom WR-Netzanschluss, Strom berechnet
      meter.voltage[phaseIndex] = registerU16(inverter, INVERTER_BLOCK_START, 35121 + 5 * phaseIndex) / 10.0f;
      meter.current[phaseIndex] =
          meter.voltage[phaseIndex] > 1 ? meter.apparentPower[phaseIndex] / meter.voltage[phaseIndex] : 0;
    }
  }
}

// Passt bei Bedarf das Vorzeichen an (GoodWe: + = Einspeisung, Zähler: + = Bezug), berechnet
// abgeleitete Größen (Leistungsfaktor, Leiter-Leiter-Spannung) und markiert die Daten als gültig.
static void finishMeterData(MeterData& meter) {
  if (g_config.goodweInvertSign) {
    for (int phaseIndex = 0; phaseIndex < 3; phaseIndex++) {
      meter.activePower[phaseIndex] = -meter.activePower[phaseIndex];
      meter.reactivePower[phaseIndex] = -meter.reactivePower[phaseIndex];
    }
    meter.totalActivePower = -meter.totalActivePower;
    meter.totalReactivePower = -meter.totalReactivePower;
  }
  for (int phaseIndex = 0; phaseIndex < 3; phaseIndex++) {
    meter.powerFactor[phaseIndex] = meter.apparentPower[phaseIndex] > 1
                                        ? meter.activePower[phaseIndex] / meter.apparentPower[phaseIndex]
                                        : 1.0f;
    meter.lineToLineVoltage[phaseIndex] =
        lineToLineVoltage(meter.voltage[phaseIndex], meter.voltage[(phaseIndex + 1) % 3]);
  }
  meter.totalPowerFactor = meter.totalApparentPower > 1 ? meter.totalActivePower / meter.totalApparentPower : 1.0f;
  meter.valid = true;
  meter.updatedAtMs = millisSinceBoot();
}

// ---- Polling
//
// Zwei Abfrageintervalle:
//   Intervall 1 (schnell, goodwePollIntervalMs): nur die Register der Sensoren, die der Benutzer
//     Intervall 1 zugeordnet hat, plus die Werte für die Lumel-Emulation. Benachbarte Register
//     werden zu möglichst wenigen Anfragen zusammengefasst.
//   Intervall 2 (langsam, goodweSlowPollIntervalMs): alle Blöcke vollständig.
// Die Register landen in denselben Blockpuffern; ein Teilbereich aktualisiert also nur seine Register.

// Ein abgefragter Registerblock des GoodWe mit seinem Empfangspuffer
struct PolledBlock {
  uint8_t blockId;          // GoodweRegisterBlock
  uint8_t device;           // GoodweDevice: abgeschaltete Geräte werden nicht abgefragt
  uint16_t startAddress;
  uint16_t registerCount;   // aktuell gelesene Länge (Smart-Meter: 58 oder 45, je nach Firmware)
  uint16_t* buffer;
  const char* requestName;  // für Statistik und Fehlertext, z. B. "36000"
  uint32_t retryAtMs = 0;   // != 0: Gerät hat den Block mit Exception abgelehnt, erneuter Versuch ab hier
  bool everRead = false;    // mindestens einmal vollständig gelesen

  // true, wenn das Gerät den Block zuletzt abgelehnt hat (z. B. keine zweite Batterie)
  bool unsupported() const { return retryAtMs != 0; }
  // true, wenn das Gerät des Blocks nicht abgeschaltet ist
  bool enabled() const { return goodweDeviceEnabled(device); }
  // true, wenn Teilbereiche des Blocks in Intervall 1 gelesen werden dürfen
  bool usableForFastReads() const { return enabled() && everRead && !unsupported(); }
};

static PolledBlock polledBlocks[] = {
    {BLOCK_INVERTER, DEVICE_INVERTER, INVERTER_BLOCK_START, INVERTER_REGISTER_COUNT, inverterRegisters, "35100"},
    {BLOCK_METER, DEVICE_METER, METER_BLOCK_START, METER_REGISTER_COUNT_EXTENDED, meterRegisters, "36000"},
    {BLOCK_BMS, DEVICE_BATTERY1, BMS_BLOCK_START, BMS_REGISTER_COUNT, bmsRegisters, "37000"},
    {BLOCK_BATTERY2, DEVICE_BATTERY2, BATTERY2_BLOCK_START, BATTERY2_REGISTER_COUNT, battery2Registers, "35262"},
    {BLOCK_BMS2, DEVICE_BATTERY2, BMS2_BLOCK_START, BMS2_REGISTER_COUNT, bms2Registers, "39000"},
};
static PolledBlock& inverterBlock = polledBlocks[0];
static PolledBlock& meterBlock = polledBlocks[1];

// Lücken bis zu dieser Größe werden mitgelesen, statt eine weitere Anfrage zu stellen: ein Register mehr
// kostet bei 9600 Baud ca. 2 ms, eine zusätzliche Anfrage (Pause, Antwortzeit, ggf. Gateway und VPN)
// 100-300 ms.
static const uint16_t MAX_GAP_TO_MERGE = 40;
static const size_t MAX_FAST_RANGES = 24;
static const uint16_t MAX_REGISTERS_PER_REQUEST = 125;

// Zusammenhängender Registerbereich, der in Intervall 1 gelesen wird
struct RegisterRange {
  PolledBlock* block;
  uint16_t startAddress;
  uint16_t registerCount;
};

// Register, die in Intervall 1 übernommen werden (je Block, Index = Register - Blockstart).
// Mitgelesene Lücken zwischen ihnen werden verworfen, damit Werte aus Intervall 2 wirklich nur
// im Takt von Intervall 2 aktualisiert werden.
static bool fastRegisterWanted[sizeof(polledBlocks) / sizeof(polledBlocks[0])][MAX_REGISTERS_PER_REQUEST];

// Zuletzt verwendete Bereiche von Intervall 1 (für die Anzeige auf dem Reiter GoodWe)
static RegisterRange lastFastRanges[MAX_FAST_RANGES];
static size_t lastFastRangeCount = 0;

// Zustand des Polling-Tasks zwischen den Zyklen
struct PollState {
  bool deviceInfoValid = false;
  uint32_t lastDeviceInfoAttemptMs = 0;
  uint32_t lastSlowCycleStartMs = 0;
  uint32_t lastFastCycleStartMs = 0;
  uint8_t consecutiveFailures = 0;
};

// Ergebnis eines Zyklus
struct CycleResult {
  bool slowCycle = false;          // alle Blöcke vollständig gelesen (Intervall 2)
  uint8_t requestCount = 0;
  uint8_t successCount = 0;
  bool sourceBlockRead = false;    // Block der Datenquelle (Smart-Meter bzw. Wechselrichter) gelesen
  bool sourceBlockFailed = false;  // ... und dabei ist mindestens eine Anfrage fehlgeschlagen
};

// Block, aus dem die Netzwerte für Lumel und Übersicht stammen
static PolledBlock& sourceBlock() {
  return g_config.goodweDataSource == GOODWE_SOURCE_METER ? meterBlock : inverterBlock;
}

// Liest die Geräteinfo 35000..35067 (Modell, Seriennummer, Firmware) einmalig bzw. nach Ausfall alle 5 s.
// Ergebnis steht in state.deviceInfoValid.
static void readDeviceInfoIfNeeded(PollState& state, uint32_t cycleStartMs) {
  if (state.deviceInfoValid) return;
  if (state.lastDeviceInfoAttemptMs != 0 && cycleStartMs - state.lastDeviceInfoAttemptMs <= DEVICE_INFO_RETRY_MS) return;
  state.lastDeviceInfoAttemptMs = cycleStartMs;
  int result = readHoldingRegisters(DEVICE_INFO_BLOCK_START, DEVICE_INFO_REGISTER_COUNT, deviceInfoRegisters);
  recordResult(result, "35000");
  state.deviceInfoValid = result == MODBUS_OK;
  pauseBetweenRequests();
}

// Liest registerCount Register ab startAddress in den Puffer des Blocks und trägt das Ergebnis
// in cycle ein. Rückgabe: ModbusResult bzw. Exception-Code.
// Ist destination gesetzt, landen die Werte dort statt im Blockpuffer (Intervall 1, siehe readFastRanges).
static int readBlockRange(PolledBlock& block, uint16_t startAddress, uint16_t registerCount, CycleResult& cycle,
                          uint16_t* destination = nullptr) {
  if (cycle.requestCount++) pauseBetweenRequests();
  if (!destination) destination = block.buffer + (startAddress - block.startAddress);
  int result = readHoldingRegisters(startAddress, registerCount, destination);
  recordResult(result, block.requestName);
  bool isSourceBlock = &block == &sourceBlock();
  if (result == MODBUS_OK) {
    cycle.successCount++;
    if (isSourceBlock) cycle.sourceBlockRead = true;
  } else if (isSourceBlock) {
    cycle.sourceBlockFailed = true;
  }
  return result;
}

// Intervall 2: liest alle Blöcke vollständig. Lehnt das Gerät einen Block ab (Exception), wird er erst
// nach 60 s erneut versucht. Kennt die Firmware die erweiterten Smart-Meter-Register nicht,
// wird dauerhaft auf den kurzen Block umgestellt.
static void readAllBlocks(CycleResult& cycle) {
  cycle.slowCycle = true;
  bool firstBlock = true;
  for (PolledBlock& block : polledBlocks) {
    if (!block.enabled()) {
      block.everRead = false;  // nach dem Wiedereinschalten erst wieder vollständig lesen
      continue;
    }
    if (!isRetryDue(block.retryAtMs)) continue;
    int result = readBlockRange(block, block.startAddress, block.registerCount, cycle);
    if (result > 0 && &block == &meterBlock && block.registerCount == METER_REGISTER_COUNT_EXTENDED) {
      block.registerCount = METER_REGISTER_COUNT_SHORT;
      result = readBlockRange(block, block.startAddress, block.registerCount, cycle);
    }
    block.retryAtMs = result > 0 ? millisSinceBoot() + UNSUPPORTED_BLOCK_RETRY_MS : 0;
    if (result == MODBUS_OK) block.everRead = true;
    // Keine Antwort auf den ersten Block: Gerät nicht erreichbar, Rest überspringen
    if (firstBlock && result < 0) break;
    firstBlock = false;
  }
}

// Ermittelt die Registerbereiche für Intervall 1: markiert je Block die Register aller Sensoren der
// Gruppe 1 und fasst sie (inkl. kleiner Lücken) zu Bereichen zusammen. Rückgabe: Anzahl Bereiche.
static size_t buildFastRanges(RegisterRange* ranges) {
  size_t rangeCount = 0;
  for (size_t blockIndex = 0; blockIndex < sizeof(polledBlocks) / sizeof(polledBlocks[0]); blockIndex++) {
    PolledBlock& block = polledBlocks[blockIndex];
    bool* registerWanted = fastRegisterWanted[blockIndex];
    memset(registerWanted, 0, MAX_REGISTERS_PER_REQUEST * sizeof(bool));
    if (!block.usableForFastReads()) continue;
    for (size_t sensorIndex = 0; sensorIndex < GOODWE_SENSOR_COUNT; sensorIndex++) {
      const GoodweSensor& sensor = GOODWE_SENSORS[sensorIndex];
      if (sensor.block != block.blockId || goodweSensorPollGroup(sensorIndex) != 1) continue;
      for (uint8_t wordIndex = 0; wordIndex < goodweSensorRegisterCount(sensor); wordIndex++) {
        int offset = sensor.registerAddress - block.startAddress + wordIndex;
        if (offset >= 0 && offset < block.registerCount) registerWanted[offset] = true;
      }
    }
    // Bereiche bilden: eine Lücke bis MAX_GAP_TO_MERGE wird mitgelesen
    int rangeStart = -1, lastWanted = -1;
    for (int offset = 0; offset <= block.registerCount; offset++) {
      bool wanted = offset < block.registerCount && registerWanted[offset];
      if (wanted && rangeStart < 0) rangeStart = offset;
      bool gapTooLarge = rangeStart >= 0 && offset - lastWanted > MAX_GAP_TO_MERGE;
      if (rangeStart >= 0 && (offset == block.registerCount || (!wanted && gapTooLarge))) {
        if (rangeCount < MAX_FAST_RANGES)
          ranges[rangeCount++] = {&block, (uint16_t)(block.startAddress + rangeStart),
                                  (uint16_t)(lastWanted - rangeStart + 1)};
        rangeStart = -1;
      }
      if (wanted) {
        if (rangeStart < 0) rangeStart = offset;
        lastWanted = offset;
      }
    }
  }
  return rangeCount;
}

// Intervall 1: liest die Registerbereiche der schnellen Werte (inkl. kleiner Lücken, das spart Anfragen)
// und übernimmt davon nur die Register der Werte aus Intervall 1 in den Blockpuffer.
static void readFastRanges(CycleResult& cycle) {
  RegisterRange ranges[MAX_FAST_RANGES];
  size_t rangeCount = buildFastRanges(ranges);
  uint16_t rangeValues[MAX_REGISTERS_PER_REQUEST];
  for (size_t rangeIndex = 0; rangeIndex < rangeCount; rangeIndex++) {
    const RegisterRange& range = ranges[rangeIndex];
    PolledBlock& block = *range.block;
    if (readBlockRange(block, range.startAddress, range.registerCount, cycle, rangeValues) != MODBUS_OK) continue;
    const bool* registerWanted = fastRegisterWanted[&block - polledBlocks];
    uint16_t firstOffset = range.startAddress - block.startAddress;
    for (uint16_t index = 0; index < range.registerCount; index++)
      if (registerWanted[firstOffset + index]) block.buffer[firstOffset + index] = rangeValues[index];
  }
  sharedLock();
  memcpy(lastFastRanges, ranges, rangeCount * sizeof(RegisterRange));
  lastFastRangeCount = rangeCount;
  sharedUnlock();
}

size_t goodweCopyFastRanges(uint16_t* startAddresses, uint16_t* registerCounts, size_t maxRanges) {
  sharedLock();
  size_t rangeCount = lastFastRangeCount < maxRanges ? lastFastRangeCount : maxRanges;
  for (size_t rangeIndex = 0; rangeIndex < rangeCount; rangeIndex++) {
    startAddresses[rangeIndex] = lastFastRanges[rangeIndex].startAddress;
    registerCounts[rangeIndex] = lastFastRanges[rangeIndex].registerCount;
  }
  sharedUnlock();
  return rangeCount;
}

// Kopiert die Rohregister in die geteilten Daten (für Sensorliste, Web-Tabelle und MQTT).
// Ein Block gilt als gültig, sobald er einmal vollständig gelesen wurde, solange das Gerät ihn
// nicht ablehnt und nicht mehrere Zyklen in Folge fehlgeschlagen sind.
static void publishRawRegisters(const PollState& state) {
  bool deviceReachable = state.consecutiveFailures < FAILURES_UNTIL_INVALID;
  auto isBlockValid = [&](const PolledBlock& block) {
    return deviceReachable && block.enabled() && block.everRead && !block.unsupported();
  };
  sharedLock();
  GoodweRegisters& shared = g_goodweRegisters;
  memcpy(shared.inverterRegisters, inverterRegisters, sizeof(inverterRegisters));
  memcpy(shared.meterRegisters, meterRegisters, sizeof(meterRegisters));
  memcpy(shared.bmsRegisters, bmsRegisters, sizeof(bmsRegisters));
  memcpy(shared.battery2Registers, battery2Registers, sizeof(battery2Registers));
  memcpy(shared.bms2Registers, bms2Registers, sizeof(bms2Registers));
  shared.meterRegisterCount = meterBlock.registerCount;
  shared.inverterValid = isBlockValid(inverterBlock);
  shared.meterValid = isBlockValid(meterBlock);
  shared.bmsValid = isBlockValid(polledBlocks[2]);
  shared.battery2Valid = isBlockValid(polledBlocks[3]);
  shared.bms2Valid = isBlockValid(polledBlocks[4]);
  if (state.deviceInfoValid) memcpy(shared.deviceInfoRegisters, deviceInfoRegisters, sizeof(deviceInfoRegisters));
  shared.deviceInfoValid = state.deviceInfoValid;
  shared.updatedAtMs = millisSinceBoot();
  sharedUnlock();
}

// Dekodiert Übersicht und – wenn der Block der Datenquelle in diesem Zyklus gelesen wurde –
// die Zählerwerte für die Lumel-Emulation und stellt sie in den geteilten Daten bereit.
static void publishDecodedData(const PollState& state, const CycleResult& cycle) {
  GoodweInfo info;
  // abgeschalteter Wechselrichter: PV, Temperatur, Energie und Batterie-1-Werte bleiben leer
  info.inverterPresent = inverterBlock.usableForFastReads();
  if (info.inverterPresent) decodeInverterInfo(inverterRegisters, info);
  info.valid = true;
  // Modell/Seriennummer: Hilfsstruktur, weil goodweReadDeviceStrings mit GoodweRegisters arbeitet
  if (state.deviceInfoValid) {
    GoodweRegisters deviceInfoOnly;
    memcpy(deviceInfoOnly.deviceInfoRegisters, deviceInfoRegisters, sizeof(deviceInfoRegisters));
    deviceInfoOnly.deviceInfoValid = true;
    goodweReadDeviceStrings(deviceInfoOnly, info.model, sizeof(info.model), info.serialNumber,
                            sizeof(info.serialNumber));
    goodweReadFirmwareVersion(deviceInfoOnly, info.firmwareVersion, sizeof(info.firmwareVersion));
  }
  if (meterBlock.usableForFastReads()) {
    info.meterCommStatus = registerU16(meterRegisters, METER_BLOCK_START, 36004);
    info.meterRegisterCount = meterBlock.registerCount;
  }
  // Batterien (abgeschaltete Batterie 1: Werte aus dem Wechselrichterblock verwerfen)
  info.battery1Present = goodweDeviceEnabled(DEVICE_BATTERY1);
  if (!info.battery1Present) info.batteryVoltage = info.batteryCurrent = info.batteryPower = 0;
  if (polledBlocks[2].usableForFastReads()) info.batterySoc = registerU16(bmsRegisters, BMS_BLOCK_START, 37007);
  if (polledBlocks[3].usableForFastReads()) {
    info.battery2Voltage = registerU16(battery2Registers, BATTERY2_BLOCK_START, 35262) / 10.0f;
    info.battery2Current = registerS16(battery2Registers, BATTERY2_BLOCK_START, 35263) / 10.0f;
    info.battery2Power = (float)registerS32(battery2Registers, BATTERY2_BLOCK_START, 35264);
    info.battery2Present = info.battery2Voltage > 10.0f;
  }
  if (polledBlocks[4].usableForFastReads()) info.battery2Soc = registerU16(bms2Registers, BMS2_BLOCK_START, 39005);
  info.cycleDurationMs = fastCyclePeriodMs;
  info.slowCycleDurationMs = slowCyclePeriodMs;

  // Netzwerte je nach eingestellter Datenquelle – nur bei frischen Daten, damit das Alter
  // (updatedAtMs, Lumel-Option "nicht antworten bei veralteten Daten") stimmt
  bool meterUpdated = cycle.sourceBlockRead && !cycle.sourceBlockFailed;
  MeterData meter;
  if (meterUpdated) {
    if (g_config.goodweDataSource == GOODWE_SOURCE_METER)
      decodeFromMeter(inverterRegisters, meterRegisters, meterBlock.registerCount, meter);
    else
      decodeFromInverter(inverterRegisters, meter);
    finishMeterData(meter);
  }
  sharedLock();
  if (meterUpdated) g_meterData = meter;
  g_goodweInfo = info;
  sharedUnlock();
}

// Zählt einen Zyklus ohne verwertbare Daten. Ab 3 Fehlzyklen in Folge werden die Daten als ungültig
// markiert (die Lumel-Emulation verstummt dann ggf.), ab 10 wird die Geräteinfo neu gelesen.
static void handleFailedCycle(PollState& state) {
  if (state.consecutiveFailures < 255) state.consecutiveFailures++;
  if (state.consecutiveFailures < FAILURES_UNTIL_INVALID) return;
  sharedLock();
  g_meterData.valid = false;
  g_goodweInfo.valid = false;
  sharedUnlock();
  // nach längerem Ausfall Geräteinfo neu lesen
  if (state.consecutiveFailures >= FAILURES_UNTIL_DEVICE_INFO_RELOAD) state.deviceInfoValid = false;
}

// Polling-Task: liest in Intervall 2 alle Blöcke, dazwischen in Intervall 1 nur die schnellen Werte,
// und wartet dann den Rest des schnellen Intervalls ab. Meldet sich beim Task-Watchdog
// und beim OTA-Modul als lebendig.
static void goodweTask(void*) {
  PollState state;

  esp_task_wdt_add(nullptr);
  for (;;) {
    uint32_t cycleStartMs = millisSinceBoot();
    otaReportTaskAlive(portIndex);
    esp_task_wdt_reset();

    readDeviceInfoIfNeeded(state, cycleStartMs);
    CycleResult cycle;
    bool slowCycleDue = !sourceBlock().everRead || state.lastSlowCycleStartMs == 0 ||
                        cycleStartMs - state.lastSlowCycleStartMs >= g_config.goodweSlowPollIntervalMs;
    if (slowCycleDue) {
      if (state.lastSlowCycleStartMs) slowCyclePeriodMs = cycleStartMs - state.lastSlowCycleStartMs;
      state.lastSlowCycleStartMs = cycleStartMs;
      readAllBlocks(cycle);
    } else {
      readFastRanges(cycle);
    }
    if (state.lastFastCycleStartMs) fastCyclePeriodMs = cycleStartMs - state.lastFastCycleStartMs;
    state.lastFastCycleStartMs = cycleStartMs;

    // Zyklus verwertbar: Gerät hat geantwortet und der Block der Datenquelle ist vorhanden und fehlerfrei
    bool deviceAnswered = cycle.requestCount == 0 || cycle.successCount > 0;
    bool cycleUsable = sourceBlock().everRead && deviceAnswered && !cycle.sourceBlockFailed;
    if (cycleUsable) state.consecutiveFailures = 0;
    else handleFailedCycle(state);
    if (sourceBlock().everRead) publishRawRegisters(state);
    if (cycleUsable) publishDecodedData(state, cycle);

    // Restzeit des schnellen Intervalls warten (mindestens 10 ms)
    uint32_t elapsedMs = millisSinceBoot() - cycleStartMs;
    uint32_t waitMs = elapsedMs < g_config.goodwePollIntervalMs ? g_config.goodwePollIntervalMs - elapsedMs : 10;
    nextFastPollAtMs = millisSinceBoot() + waitMs;
    nextSlowPollAtMs = state.lastSlowCycleStartMs + g_config.goodweSlowPollIntervalMs;
    vTaskDelay(pdMS_TO_TICKS(waitMs));
  }
}

bool goodweNextPollTimes(int32_t& fastInMs, int32_t& slowInMs) {
  if (!modbusMaster || !nextFastPollAtMs) return false;
  uint32_t nowMs = millisSinceBoot();
  fastInMs = (int32_t)(nextFastPollAtMs - nowMs);
  slowInMs = (int32_t)(nextSlowPollAtMs - nowMs);
  if (fastInMs < 0) fastInMs = 0;  // Zyklus läuft gerade
  // die vollständige Abfrage beginnt erst mit dem nächsten Zyklus nach Ablauf von Intervall 2
  if (slowInMs < fastInMs) slowInMs = fastInMs;
  return true;
}

// Merkt sich Verbindung, Statistik-Index und Adresse des GoodWe und startet den Polling-Task.
void goodweStart(uint8_t statsIndex, ModbusMaster* master, uint8_t slaveAddress) {
  portIndex = statsIndex;
  modbusMaster = master;
  goodweSlaveAddress = slaveAddress;
  masterLock = xSemaphoreCreateMutex();
  xTaskCreate(goodweTask, "goodwe", 6144, nullptr, 3, nullptr);
}

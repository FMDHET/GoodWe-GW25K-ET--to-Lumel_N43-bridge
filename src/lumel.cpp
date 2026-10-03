// Simulation eines Lumel N43 (3-Phasen-Netzmessgerät) als Modbus-RTU-Slave.
// Beantwortet Leseanfragen mit den aktuellen Messwerten aus g_meterData (oder Testwerten).
//
// Registerbelegung nach N43-Anleitung (Rev. A, Tabellen 6-10), gegengeprüft mit einem echten N43;
// vollständige Tabelle: docs/lumel_n43_register.md
//   7500..7574  Messwerte als float32, EIN 32-Bit-Register je Wert (4 Byte pro Register)
//   7000..7149  dieselben Werte als 2 x 16 Bit, High-Word zuerst
//   6000..6149  dieselben Werte als 2 x 16 Bit, Low-Word zuerst
//   4000..4066  Konfiguration und 16-Bit-Energiezähler (Schreiben wird quittiert, nicht gespeichert)
#include "lumel.h"
#include "shared.h"
#include "ota.h"
#include <cmath>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include "esp_task_wdt.h"
#include "time_sync.h"
#include "util.h"

// Wert, den der N43 für nicht definierte oder außerhalb des Messbereichs liegende Größen sendet
static const float LUMEL_UNDEFINED = 1e20f;

// Anzahl der dokumentierten Messwerte (7500..7564) und der Register, die das echte Gerät beantwortet (7500..7574)
static const uint16_t DOCUMENTED_VALUE_COUNT = 65;
static const uint16_t ANSWERED_VALUE_COUNT = 75;

// Registerbereiche, Ende jeweils exklusiv
static const uint16_t FLOAT32_REGISTERS_START = 7500;
static const uint16_t FLOAT32_REGISTERS_END = FLOAT32_REGISTERS_START + ANSWERED_VALUE_COUNT;
static const uint16_t HIGH_WORD_FIRST_PAIRS_START = 7000;  // Wortreihenfolge 3-2-1-0
static const uint16_t HIGH_WORD_FIRST_PAIRS_END = HIGH_WORD_FIRST_PAIRS_START + 2 * ANSWERED_VALUE_COUNT;
static const uint16_t LOW_WORD_FIRST_PAIRS_START = 6000;   // Wortreihenfolge 1-0-3-2
static const uint16_t LOW_WORD_FIRST_PAIRS_END = LOW_WORD_FIRST_PAIRS_START + 2 * ANSWERED_VALUE_COUNT;
static const uint16_t CONFIG_REGISTERS_START = 4000, CONFIG_REGISTERS_END = 4067;

// Funktionscodes
static const uint8_t FC_READ_HOLDING_REGISTERS = 0x03;
static const uint8_t FC_READ_INPUT_REGISTERS = 0x04;
static const uint8_t FC_WRITE_SINGLE_REGISTER = 0x06;
static const uint8_t FC_DIAGNOSTICS = 0x08;
static const uint8_t FC_WRITE_MULTIPLE_REGISTERS = 0x10;
static const uint8_t EXCEPTION_FLAG = 0x80;

static const uint8_t BROADCAST_ADDRESS = 0;
static const uint16_t MAX_FLOAT32_REGISTERS_PER_READ = 62;  // 62 * 4 Byte passen in eine Antwort
static const uint16_t MAX_REGISTERS_PER_READ = 125;
static const uint16_t MAX_REGISTERS_PER_WRITE = 123;

static const double ENERGY_COUNTER_LIMIT_KWH = 100000.0;  // Zähler läuft bis 99999,9 kWh, dann +1 Überlauf

static RtuPort* lumelPort = nullptr;
static uint8_t portIndex = 0;

// Leistungsfaktor P/S; ohne nennenswerte Scheinleistung nicht definiert: wie beim echten Gerät 1e20,
// mit der Option lumelReplaceUndefined stattdessen 1 (für Auswertegeräte, die 1e20 nicht verstehen).
static float powerFactor(float activePower, float apparentPower) {
  if (fabsf(apparentPower) >= 1.0f) return activePower / apparentPower;
  return g_config.lumelReplaceUndefined ? 1.0f : LUMEL_UNDEFINED;
}

// tg φ = Q / P; ohne nennenswerte Wirkleistung nicht definiert: 1e20 bzw. mit der Option 0.
static float tanPhi(float activePower, float reactivePower) {
  if (fabsf(activePower) >= 1.0f) return reactivePower / activePower;
  return g_config.lumelReplaceUndefined ? 0.0f : LUMEL_UNDEFINED;
}

// Mittelwert der drei Phasenwerte
static float average3(const float values[3]) { return (values[0] + values[1] + values[2]) / 3; }

// Neutralleiterstrom aus den drei Phasenströmen, unter Annahme von 120° Phasenverschiebung
// (Betrag der Vektorsumme). Bei symmetrischer Last 0.
static float neutralCurrent(const float current[3]) {
  const float cos120 = -0.5f, sin120 = 0.8660254f;
  float real = current[0] + current[1] * cos120 + current[2] * cos120;
  float imaginary = -current[1] * sin120 + current[2] * sin120;
  return sqrtf(real * real + imaginary * imaginary);
}

// Zerlegt eine Energie in die Lumel-Darstellung: Anzahl der Überläufe (je 100 MWh) und Zählerstand (kWh).
static float energyOverflows(float energyKwh) { return floorf(energyKwh / ENERGY_COUNTER_LIMIT_KWH); }
static float energyCounter(float energyKwh) {
  return energyKwh - energyOverflows(energyKwh) * ENERGY_COUNTER_LIMIT_KWH;
}

// Liefert die Ortszeit, wenn die Uhr per NTP gestellt ist; sonst false.
static bool localTime(struct tm& timeParts) {
  if (!timeIsValid()) return false;
  time_t now = time(nullptr);
  localtime_r(&now, &timeParts);
  return true;
}

// Liefert den Wert eines Phasenregisters: valueIndex 0..8 = U, I, P, Q, S, PF, tgφ, THD-U, THD-I.
// THD misst der GoodWe nicht; es wird 0 ausgegeben.
static float phaseValue(const MeterData& meter, int phaseIndex, int valueIndex) {
  switch (valueIndex) {
    case 0: return meter.voltage[phaseIndex];
    case 1: return meter.current[phaseIndex];
    case 2: return meter.activePower[phaseIndex];
    case 3: return meter.reactivePower[phaseIndex];
    case 4: return meter.apparentPower[phaseIndex];
    case 5: return powerFactor(meter.activePower[phaseIndex], meter.apparentPower[phaseIndex]);
    case 6: return tanPhi(meter.activePower[phaseIndex], meter.reactivePower[phaseIndex]);
    default: return 0;  // THD U / THD I
  }
}

// Liefert den Messwert mit Index valueIndex (Register 7500 + valueIndex) aus meter.
// Belegung siehe docs/lumel_n43_register.md. Nicht dokumentierte Register liefern 0.
static float measuredValue(uint16_t valueIndex, const MeterData& meter) {
  // 7500..7526: je Phase 9 Werte
  if (valueIndex < 27) return phaseValue(meter, valueIndex / 9, valueIndex % 9);

  struct tm timeParts;
  switch (valueIndex) {
    case 27: return average3(meter.voltage);                       // 7527 mittlere Spannung
    case 28: return average3(meter.current);                       // 7528 mittlerer Strom
    case 29: return meter.totalActivePower;                        // 7529 P gesamt
    case 30: return meter.totalReactivePower;                      // 7530 Q gesamt
    case 31: return meter.totalApparentPower;                      // 7531 S gesamt
    case 32: return powerFactor(meter.totalActivePower, meter.totalApparentPower);  // 7532 PF
    case 33: return tanPhi(meter.totalActivePower, meter.totalReactivePower);       // 7533 tgφ
    case 34: return meter.frequency;                               // 7534 Frequenz
    case 35: return meter.lineToLineVoltage[0];                    // 7535 U12
    case 36: return meter.lineToLineVoltage[1];                    // 7536 U23
    case 37: return meter.lineToLineVoltage[2];                    // 7537 U31
    case 38: return average3(meter.lineToLineVoltage);             // 7538 mittlere Leiterspannung
    // Mittelwerte (Demand) und deren Min/Max: der GoodWe liefert Momentanwerte, diese werden ausgegeben
    case 39: return meter.totalActivePower;                        // 7539 P Demand
    case 40: return meter.totalApparentPower;                      // 7540 S Demand
    case 41: return average3(meter.current);                       // 7541 I Demand
    case 42: return 0;                                             // 7542 THD U Mittelwert
    case 43: return 0;                                             // 7543 THD I Mittelwert
    case 44: return neutralCurrent(meter.current);                 // 7544 Neutralleiterstrom
    case 45: return energyOverflows(meter.importedEnergyKwh);      // 7545 Bezug, Überläufe
    case 46: return energyCounter(meter.importedEnergyKwh);        // 7546 Bezug, kWh
    case 47: return energyOverflows(meter.exportedEnergyKwh);      // 7547 Lieferung, Überläufe
    case 48: return energyCounter(meter.exportedEnergyKwh);        // 7548 Lieferung, kWh
    // 7549..7554 Blind-/Scheinenergie: liefert der GoodWe nicht -> 0
    case 55: return localTime(timeParts) ? timeParts.tm_sec : 0;   // 7555 Sekunden
    case 56:                                                       // 7556 Stunden,Minuten (z. B. 6.24)
      return localTime(timeParts) ? timeParts.tm_hour + timeParts.tm_min / 100.0f : 0;
    case 57:                                                       // 7557 Monat,Tag (wie beim echten Gerät)
      return localTime(timeParts) ? (timeParts.tm_mon + 1) + timeParts.tm_mday / 100.0f : 0;
    case 58: return localTime(timeParts) ? timeParts.tm_year + 1900 : 0;  // 7558 Jahr
    case 59: return average3(meter.current);                       // 7559 mittlerer Strom max
    case 60:                                                       // 7560 max. Phasenspannung
      return fmaxf(meter.voltage[0], fmaxf(meter.voltage[1], meter.voltage[2]));
    case 61: return meter.totalActivePower;                        // 7561 P Demand min
    case 62: return meter.totalActivePower;                        // 7562 P Demand max
    case 63: return meter.totalApparentPower;                      // 7563 S Demand max
    case 64: return average3(meter.current);                       // 7564 I Demand max
    default: return 0;
  }
}

// Index des RS485-Ports mit der Rolle Lumel (laufende Simulation bzw. aus der Konfiguration);
// -1, wenn kein Port die Rolle hat.
static int configuredLumelPortIndex() {
  if (lumelPort) return portIndex;
  for (int index = 0; index < 2; index++)
    if (g_config.port[index].role == ROLE_LUMEL_SLAVE) return index;
  return -1;
}

// Wie configuredLumelPortIndex, aber immer ein gültiger Index (ohne Lumel-Port: RTU1)
static int lumelPortIndex() {
  int index = configuredLumelPortIndex();
  return index < 0 ? 0 : index;
}

// Format-Code des Lumel (Register 4040): 0 = 8N2, 1 = 8E1, 2 = 8O1, 3 = 8N1
static uint16_t serialFormatCode(const PortConfig& portConfig) {
  if (portConfig.parity == 'E') return 1;
  if (portConfig.parity == 'O') return 2;
  return portConfig.stopBits == 2 ? 0 : 3;
}

// Baudraten-Code des Lumel (Register 4041): 0 = 4800, 1 = 9600, 2 = 19200, 3 = 38400
static uint16_t baudRateCode(uint32_t baudRate) {
  switch (baudRate) {
    case 4800: return 0;
    case 19200: return 2;
    case 38400: return 3;
    default: return 1;
  }
}

// Liefert ein Konfigurationsregister (4000..4066) mit plausiblen Werten des simulierten Geräts.
static uint16_t configRegisterValue(uint16_t registerAddress, const MeterData& meter) {
  const PortConfig& portConfig = g_config.port[lumelPortIndex()];
  // Energien in 100-Wh-Einheiten als High/Low-Wort
  uint32_t importedTenthKwh = (uint32_t)(meter.importedEnergyKwh * 10.0f);
  uint32_t exportedTenthKwh = (uint32_t)(meter.exportedEnergyKwh * 10.0f);
  struct tm timeParts;
  switch (registerAddress) {
    case 4003: return 0;     // Anschluss 3Ph/4W
    case 4004: return 1;     // Stromeingang 5 A
    case 4005: return 1;     // Stromwandler-Übersetzung
    case 4006: return 10;    // Spannungswandler-Übersetzung x 10
    case 4008: return 1;     // Synchronisation mit Uhr
    case 4038: return 1000;  // Impulse je kWh
    case 4039: return portConfig.slaveAddress;
    case 4040: return serialFormatCode(portConfig);
    case 4041: return baudRateCode(portConfig.baudRate);
    case 4045: return localTime(timeParts) ? timeParts.tm_hour * 100 + timeParts.tm_min : 0;
    case 4048: return importedTenthKwh >> 16;
    case 4049: return importedTenthKwh & 0xFFFF;
    case 4050: return exportedTenthKwh >> 16;
    case 4051: return exportedTenthKwh & 0xFFFF;
    case 4061: return 0x0043;  // Seriennummer (fest, kennzeichnet die Simulation)
    case 4062: return 0x0006;
    case 4063: return 100;     // Softwareversion x 100
    default: return 0;
  }
}

// Liefert die IEEE-754-Bits eines float für die Ausgabe (höherwertiges Wort zuerst).
// Mit lumelSwapFloatWords werden die beiden 16-Bit-Wörter getauscht (für Master mit anderer Wortfolge).
static uint32_t floatToRegisterBits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, 4);
  if (g_config.lumelSwapFloatWords) bits = (bits << 16) | (bits >> 16);
  return bits;
}

// Füllt meter mit symmetrischen Testwerten aus der Konfiguration (Testmodus ohne GoodWe).
static void fillTestData(MeterData& meter) {
  float voltage = g_config.testVoltage;
  float powerPerPhase = g_config.testPowerWatt / 3.0f;
  for (int phaseIndex = 0; phaseIndex < 3; phaseIndex++) {
    meter.voltage[phaseIndex] = voltage;
    meter.activePower[phaseIndex] = powerPerPhase;
    meter.apparentPower[phaseIndex] = fabsf(powerPerPhase);
    meter.reactivePower[phaseIndex] = 0;
    meter.powerFactor[phaseIndex] = 1;
    meter.current[phaseIndex] = voltage > 1 ? meter.apparentPower[phaseIndex] / voltage : 0;
    meter.lineToLineVoltage[phaseIndex] = voltage * 1.7320508f;  // U_LL = √3 · U_LN
  }
  meter.totalActivePower = g_config.testPowerWatt;
  meter.totalApparentPower = fabsf(meter.totalActivePower);
  meter.totalReactivePower = 0;
  meter.totalPowerFactor = 1;
  meter.frequency = 50.0f;
  meter.valid = true;
  meter.updatedAtMs = millisSinceBoot();
}

// Schreibt eine Exception-Antwort (FC | 0x80, Code) nach response. Rückgabe: Antwortlänge ohne CRC.
static size_t buildExceptionResponse(uint8_t* response, uint8_t functionCode, uint8_t exceptionCode) {
  response[1] = functionCode | EXCEPTION_FLAG;
  response[2] = exceptionCode;
  return 3;
}

// Liest einen 16-Bit-Wert in Modbus-Byte-Reihenfolge (Big Endian).
static inline uint16_t readBigEndian16(const uint8_t* bytes) { return (bytes[0] << 8) | bytes[1]; }

// Merkt die letzte Anfrage als Text (printf-Format) für die Web-Statistik.
static void setLastRequest(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void setLastRequest(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(g_portStats[portIndex].lastRequest, sizeof(g_portStats[portIndex].lastRequest), format, arguments);
  va_end(arguments);
}

// FC03/FC04: beantwortet Lesezugriffe auf die drei Registerbereiche.
// request/requestLength = Anfrage ohne CRC, response = Antwortpuffer (Adresse und FC schon gesetzt).
// Rückgabe: Länge der Antwort ohne CRC, 0 = keine Antwort.
static size_t handleReadRegisters(const uint8_t* request, size_t requestLength, uint8_t* response,
                                  const MeterData& meter, bool isBroadcast) {
  uint8_t functionCode = request[1];
  if (isBroadcast) return 0;  // Lesen per Broadcast ist nicht erlaubt
  if (requestLength != 6) return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_DATA_VALUE);

  uint16_t startRegister = readBigEndian16(request + 2);
  uint16_t registerCount = readBigEndian16(request + 4);
  setLastRequest("FC%02u Start %u Anzahl %u", functionCode, startRegister, registerCount);
  uint32_t endRegister = (uint32_t)startRegister + registerCount;  // exklusiv

  // 7500..7574: 32-Bit-Register, 4 Byte pro Register, max. 62 Register pro Antwort
  if (startRegister >= FLOAT32_REGISTERS_START && startRegister < FLOAT32_REGISTERS_END) {
    if (registerCount < 1 || registerCount > MAX_FLOAT32_REGISTERS_PER_READ)
      return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_DATA_VALUE);
    if (endRegister > FLOAT32_REGISTERS_END)
      return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_DATA_ADDRESS);
    response[2] = registerCount * 4;
    for (uint16_t registerIndex = 0; registerIndex < registerCount; registerIndex++) {
      uint32_t bits = floatToRegisterBits(measuredValue(startRegister - FLOAT32_REGISTERS_START + registerIndex, meter));
      uint8_t* target = response + 3 + 4 * registerIndex;
      target[0] = bits >> 24;
      target[1] = bits >> 16;
      target[2] = bits >> 8;
      target[3] = bits;
    }
    return 3 + registerCount * 4;
  }

  if (registerCount < 1 || registerCount > MAX_REGISTERS_PER_READ)
    return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_DATA_VALUE);

  // 7000..7149 (High-Word zuerst) und 6000..6149 (Low-Word zuerst): je Messwert zwei 16-Bit-Register
  bool highWordFirstRange = startRegister >= HIGH_WORD_FIRST_PAIRS_START && endRegister <= HIGH_WORD_FIRST_PAIRS_END;
  bool lowWordFirstRange = startRegister >= LOW_WORD_FIRST_PAIRS_START && endRegister <= LOW_WORD_FIRST_PAIRS_END;
  if (highWordFirstRange || lowWordFirstRange) {
    uint16_t rangeStart = highWordFirstRange ? HIGH_WORD_FIRST_PAIRS_START : LOW_WORD_FIRST_PAIRS_START;
    response[2] = registerCount * 2;
    for (uint16_t registerIndex = 0; registerIndex < registerCount; registerIndex++) {
      uint16_t registerOffset = startRegister - rangeStart + registerIndex;
      uint32_t bits = floatToRegisterBits(measuredValue(registerOffset / 2, meter));
      bool isFirstWordOfPair = (registerOffset & 1) == 0;
      // erstes Register des Paars: High-Word (7000er) bzw. Low-Word (6000er)
      bool sendHighWord = highWordFirstRange ? isFirstWordOfPair : !isFirstWordOfPair;
      uint16_t word = sendHighWord ? (bits >> 16) : (bits & 0xFFFF);
      response[3 + 2 * registerIndex] = word >> 8;
      response[4 + 2 * registerIndex] = word;
    }
    return 3 + registerCount * 2;
  }

  // 4000..4066: Konfiguration und 16-Bit-Energiezähler
  if (startRegister >= CONFIG_REGISTERS_START && endRegister <= CONFIG_REGISTERS_END) {
    response[2] = registerCount * 2;
    for (uint16_t registerIndex = 0; registerIndex < registerCount; registerIndex++) {
      uint16_t value = configRegisterValue(startRegister + registerIndex, meter);
      response[3 + 2 * registerIndex] = value >> 8;
      response[4 + 2 * registerIndex] = value;
    }
    return 3 + registerCount * 2;
  }
  return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_DATA_ADDRESS);
}

// FC06 Write Single Register: wird im Konfigurationsbereich quittiert, aber nicht gespeichert.
// Rückgabe: Länge der Antwort ohne CRC, 0 = keine Antwort.
static size_t handleWriteSingleRegister(const uint8_t* request, size_t requestLength, uint8_t* response,
                                        bool isBroadcast) {
  uint8_t functionCode = request[1];
  if (requestLength != 6) return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_DATA_VALUE);
  uint16_t registerAddress = readBigEndian16(request + 2);
  if (registerAddress < CONFIG_REGISTERS_START || registerAddress >= CONFIG_REGISTERS_END)
    return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_DATA_ADDRESS);
  if (isBroadcast) return 0;
  memcpy(response, request, 6);  // Echo der Anfrage
  return 6;
}

// FC16 Write Multiple Registers: wird im Konfigurationsbereich quittiert, aber nicht gespeichert.
// Rückgabe: Länge der Antwort ohne CRC, 0 = keine Antwort.
static size_t handleWriteMultipleRegisters(const uint8_t* request, size_t requestLength, uint8_t* response,
                                           bool isBroadcast) {
  uint8_t functionCode = request[1];
  if (requestLength < 7) return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_DATA_VALUE);
  uint16_t startRegister = readBigEndian16(request + 2);
  uint16_t registerCount = readBigEndian16(request + 4);
  uint8_t byteCount = request[6];
  if (registerCount < 1 || registerCount > MAX_REGISTERS_PER_WRITE || byteCount != registerCount * 2 ||
      requestLength != 7u + byteCount)
    return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_DATA_VALUE);
  if (startRegister < CONFIG_REGISTERS_START || (uint32_t)startRegister + registerCount > CONFIG_REGISTERS_END)
    return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_DATA_ADDRESS);
  if (isBroadcast) return 0;
  memcpy(response, request, 6);  // Adresse, FC, Start, Anzahl
  return 6;
}

// FC08 Diagnostics: nur Sub-Function 0x0000 "Return Query Data" (Echo der Anfrage).
// Rückgabe: Länge der Antwort ohne CRC, 0 = keine Antwort.
static size_t handleDiagnostics(const uint8_t* request, size_t requestLength, uint8_t* response,
                                bool isBroadcast) {
  uint8_t functionCode = request[1];
  if (requestLength < 4) return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_DATA_VALUE);
  if (readBigEndian16(request + 2) != 0x0000)
    return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_FUNCTION);
  if (isBroadcast) return 0;
  memcpy(response, request, requestLength);
  return requestLength;
}

// Verarbeitet eine Anfrage-PDU (request/requestLength ohne CRC) und baut die Antwort in response.
// Rückgabe: Länge der Antwort (ohne CRC), 0 = keine Antwort (z.B. bei Broadcast).
static size_t handleRequest(const uint8_t* request, size_t requestLength, uint8_t* response,
                            const MeterData& meter, bool isBroadcast) {
  uint8_t functionCode = request[1];
  response[0] = request[0];
  response[1] = functionCode;
  setLastRequest("FC%02u%s", functionCode, isBroadcast ? " (Broadcast)" : "");

  switch (functionCode) {
    case FC_READ_HOLDING_REGISTERS:
    case FC_READ_INPUT_REGISTERS:
      return handleReadRegisters(request, requestLength, response, meter, isBroadcast);
    case FC_WRITE_SINGLE_REGISTER:
      return handleWriteSingleRegister(request, requestLength, response, isBroadcast);
    case FC_WRITE_MULTIPLE_REGISTERS:
      return handleWriteMultipleRegisters(request, requestLength, response, isBroadcast);
    case FC_DIAGNOSTICS:
      return handleDiagnostics(request, requestLength, response, isBroadcast);
    default:
      return buildExceptionResponse(response, functionCode, EXCEPTION_ILLEGAL_FUNCTION);
  }
}

// Holt die auszugebenden Messwerte (Testwerte oder Kopie von g_meterData) nach meter.
// Rückgabe: true, wenn die Daten veraltet oder ungültig sind.
static bool loadMeterData(MeterData& meter) {
  if (g_config.testModeEnabled) {
    fillTestData(meter);
    return false;
  }
  sharedLock();
  meter = g_meterData;
  sharedUnlock();
  return !meter.valid || millisSinceBoot() - meter.updatedAtMs > g_config.staleAfterSec * 1000UL;
}

// Slave-Task: wartet auf Anfragen an die eigene Adresse (oder Broadcast), beantwortet sie
// und führt die Port-Statistik. Meldet sich zyklisch beim Task-Watchdog und bei der OTA-Überwachung.
static void lumelTask(void*) {
  static uint8_t requestFrame[MODBUS_MAX_FRAME_SIZE];
  static uint8_t responseFrame[MODBUS_MAX_FRAME_SIZE + 2];  // + 2 Byte für die CRC
  const PortConfig& portConfig = g_config.port[portIndex];
  esp_task_wdt_add(nullptr);

  for (;;) {
    otaReportTaskAlive(portIndex);
    esp_task_wdt_reset();

    // --- Anfrage empfangen und Adresse prüfen ---
    size_t requestLength;
    if (!lumelPort->waitForFrame(requestFrame, requestLength, pdMS_TO_TICKS(1000))) continue;
    uint8_t targetAddress = requestFrame[0];
    bool isBroadcast = targetAddress == BROADCAST_ADDRESS;
    if (requestLength < 2 || (!isBroadcast && targetAddress != portConfig.slaveAddress))
      continue;  // Frame für anderes Gerät

    // --- Messwerte holen ---
    MeterData meter;
    bool isStale = loadMeterData(meter);
    sharedLock();
    g_portStats[portIndex].requests++;
    sharedUnlock();

    // Ohne aktuelle Quelldaten optional schweigen, damit der Master den Ausfall erkennt
    if (isStale && g_config.lumelSilentWhenStale) {
      sharedLock();
      copyString(g_portStats[portIndex].lastError, "Keine aktuellen GoodWe-Daten - keine Antwort",
                 sizeof(g_portStats[portIndex].lastError));
      sharedUnlock();
      continue;
    }

    // --- Antwort bauen und senden ---
    size_t responseLength = handleRequest(requestFrame, requestLength, responseFrame, meter, isBroadcast);
    if (!responseLength) continue;
    lumelPort->sendFrame(responseFrame, responseLength);

    // --- Statistik ---
    sharedLock();
    g_portStats[portIndex].responses++;
    g_portStats[portIndex].lastSuccessAtMs = millisSinceBoot();
    if (responseFrame[1] & EXCEPTION_FLAG) {
      g_portStats[portIndex].exceptions++;
      snprintf(g_portStats[portIndex].lastError, sizeof(g_portStats[portIndex].lastError), "Gesendet: %s",
               modbusResultText(responseFrame[2]));
    }
    sharedUnlock();
  }
}

// Startet den Lumel-Slave-Task für Port portIndexToUse (0/1) auf dem bereits initialisierten port.
void lumelStart(uint8_t portIndexToUse, RtuPort* port) {
  portIndex = portIndexToUse;
  lumelPort = port;
  xTaskCreate(lumelTask, "lumel", 4096, nullptr, 5, nullptr);
}

void lumelValuesToJson(cJSON* jsonObject) {
  MeterData meter;
  bool isStale = loadMeterData(meter);
  int lumelIndex = configuredLumelPortIndex();
  cJSON_AddBoolToObject(jsonObject, "active", lumelPort != nullptr);
  cJSON_AddNumberToObject(jsonObject, "port", lumelIndex + 1);  // 0 = kein Port mit Rolle Lumel
  if (lumelIndex >= 0) {
    const PortConfig& portConfig = g_config.port[lumelIndex];
    cJSON_AddNumberToObject(jsonObject, "address", portConfig.slaveAddress);
    cJSON_AddNumberToObject(jsonObject, "baud", portConfig.baudRate);
    char formatText[8];
    snprintf(formatText, sizeof(formatText), "8%c%u", portConfig.parity, portConfig.stopBits);
    cJSON_AddStringToObject(jsonObject, "format", formatText);
  }
  cJSON_AddBoolToObject(jsonObject, "test", g_config.testModeEnabled);
  cJSON_AddBoolToObject(jsonObject, "stale", isStale);
  cJSON_AddBoolToObject(jsonObject, "silent", isStale && g_config.lumelSilentWhenStale);
  cJSON_AddBoolToObject(jsonObject, "wordSwap", g_config.lumelSwapFloatWords);

  cJSON* valueArray = cJSON_AddArrayToObject(jsonObject, "values");
  for (uint16_t valueIndex = 0; valueIndex < ANSWERED_VALUE_COUNT; valueIndex++) {
    float value = measuredValue(valueIndex, meter);
    char bitsText[12];
    snprintf(bitsText, sizeof(bitsText), "%08lX", (unsigned long)floatToRegisterBits(value));
    cJSON* entry = cJSON_CreateArray();
    cJSON_AddItemToArray(entry, cJSON_CreateNumber(value));
    cJSON_AddItemToArray(entry, cJSON_CreateString(bitsText));
    cJSON_AddItemToArray(valueArray, entry);
  }

  cJSON* configArray = cJSON_AddArrayToObject(jsonObject, "config");
  for (uint16_t registerAddress = CONFIG_REGISTERS_START; registerAddress < CONFIG_REGISTERS_END; registerAddress++) {
    uint16_t value = configRegisterValue(registerAddress, meter);
    if (!value) continue;  // nur belegte Register
    cJSON* entry = cJSON_CreateArray();
    cJSON_AddItemToArray(entry, cJSON_CreateNumber(registerAddress));
    cJSON_AddItemToArray(entry, cJSON_CreateNumber(value));
    cJSON_AddItemToArray(configArray, entry);
  }
}

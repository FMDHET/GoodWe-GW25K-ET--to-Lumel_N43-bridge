#pragma once
#include "cJSON.h"
#include "modbus_rtu.h"

// Startet den Task, der am angegebenen Port (0 = RTU1, 1 = RTU2) einen Lumel N43 simuliert:
// er beantwortet Modbus-Anfragen mit den Werten aus g_meterData.
void lumelStart(uint8_t portIndex, RtuPort* port);

// Trägt die Register ein, die die Simulation gerade ausgeben würde (für den Reiter Lumel):
// "values": Messwerte 7500..7574 als [wert, ausgegebene Bits als Hex], "config": belegte Register
// 4000..4066 als [register, wert], dazu Port, Adresse, Format und ob die Daten veraltet sind.
void lumelValuesToJson(cJSON* jsonObject);

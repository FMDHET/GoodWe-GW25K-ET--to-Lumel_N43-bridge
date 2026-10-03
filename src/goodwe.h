#pragma once
#include <cstddef>
#include <cstdint>
#include "modbus_master.h"

// Startet den Task, der den GoodWe ET zyklisch über den angegebenen Port (0 = RTU1, 1 = RTU2)
// ausliest und die Werte in g_goodweRegisters, g_goodweInfo und g_meterData ablegt.
// statsIndex: Eintrag in g_portStats (0 = RTU1, 1 = RTU2, 2 = Modbus TCP)
// master: RS485-Port oder Modbus-TCP-Client; slaveAddress: Modbus-Adresse bzw. Unit-ID des GoodWe
void goodweStart(uint8_t statsIndex, ModbusMaster* master, uint8_t slaveAddress);

// Rückgabe von goodweForwardPdu, wenn kein GoodWe eingerichtet ist
static const int GOODWE_NOT_CONFIGURED = -100;

// Kopiert die Registerbereiche, die Intervall 1 zuletzt gelesen hat (Startadresse, Anzahl).
// Davon übernommen werden nur die Register der Werte aus Intervall 1. Rückgabe: Anzahl Bereiche.
size_t goodweCopyFastRanges(uint16_t* startAddresses, uint16_t* registerCounts, size_t maxRanges);

// Zeit bis zum nächsten schnellen Zyklus (Intervall 1) und zur nächsten vollständigen Abfrage
// (Intervall 2) in ms. Rückgabe: false, solange kein GoodWe abgefragt wird.
bool goodweNextPollTimes(int32_t& fastInMs, int32_t& slowInMs);

// Reicht eine Modbus-Anfrage (PDU) unverändert an den GoodWe durch und liefert dessen Antwort-PDU.
// unitId 0 oder 255 wird durch die eingestellte Adresse des GoodWe ersetzt.
// Rückgabe: 0 = Antwort erhalten, < 0 = Übertragungsfehler bzw. GOODWE_NOT_CONFIGURED.
int goodweForwardPdu(uint8_t unitId, const uint8_t* requestPdu, size_t requestLength, uint8_t* responsePdu,
                     size_t& responseLength);

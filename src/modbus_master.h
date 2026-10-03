#pragma once
// Gemeinsame Schnittstelle für alle Wege, Register von einem Modbus-Gerät zu lesen:
// RS485 (RtuPort) oder Netzwerk (ModbusTcpClient). Der GoodWe-Task kennt nur diese Schnittstelle.
#include <cstddef>
#include <cstdint>

class ModbusMaster {
 public:
  virtual ~ModbusMaster() = default;

  // Liest registerCount Register ab startRegister (FC03 oder FC04) in values.
  // Rückgabe: 0 = OK, < 0 = Übertragungsfehler (ModbusResult), > 0 = Modbus-Exception-Code.
  virtual int readRegisters(uint8_t slaveAddress, uint8_t functionCode, uint16_t startRegister,
                            uint16_t registerCount, uint16_t* values, uint32_t timeoutMs) = 0;

  // Reicht eine beliebige Modbus-Anfrage (PDU = Funktionscode + Daten) unverändert an das Gerät
  // und liefert dessen Antwort-PDU (auch Exception-Antworten) zurück. Für die Modbus-TCP-Bridge.
  // Rückgabe: 0 = Antwort erhalten, < 0 = Übertragungsfehler (ModbusResult).
  virtual int transact(uint8_t unitId, const uint8_t* requestPdu, size_t requestLength, uint8_t* responsePdu,
                       size_t& responseLength, uint32_t timeoutMs) = 0;
};

#pragma once
// Modbus-TCP-Client (Modbus Messaging on TCP/IP Implementation Guide V1.0b) auf lwIP-Sockets.
// Wird z. B. für ein RTU-zu-TCP-Gateway vor dem GoodWe verwendet.
//
// Jede Anfrage bekommt einen MBAP-Header (Transaktions-ID, Protokoll-ID 0, Länge, Unit-ID).
// Die Verbindung bleibt zwischen den Anfragen offen und wird bei Fehlern neu aufgebaut.
#include <cstddef>
#include <cstdint>
#include "modbus_master.h"

class ModbusTcpClient : public ModbusMaster {
 public:
  // Legt Zieladresse (IP oder Hostname) und TCP-Port fest; eine bestehende Verbindung wird getrennt.
  void configure(const char* host, uint16_t port);

  // Liest Register über Modbus TCP. slaveAddress wird als Unit-ID gesendet
  // (bei Gateways die RTU-Adresse des Geräts dahinter, beim GoodWe 247).
  int readRegisters(uint8_t slaveAddress, uint8_t functionCode, uint16_t startRegister,
                    uint16_t registerCount, uint16_t* values, uint32_t timeoutMs) override;

  // Reicht eine beliebige PDU an das Gerät durch (Modbus-TCP-Bridge).
  int transact(uint8_t unitId, const uint8_t* requestPdu, size_t requestLength, uint8_t* responsePdu,
               size_t& responseLength, uint32_t timeoutMs) override;

  // true, solange eine TCP-Verbindung besteht
  bool isConnected() const { return socketHandle >= 0; }

 private:
  // Baut die TCP-Verbindung auf (mit Zeitlimit). Rückgabe: true bei Erfolg.
  bool connectToServer(uint32_t timeoutMs);
  // Trennt die Verbindung (z. B. nach einem Fehler, damit die nächste Anfrage neu verbindet).
  void disconnect();
  // Liest bis zu `length` Bytes, bricht nach timeoutMs ab. Rückgabe: Anzahl gelesener Bytes.
  size_t receiveExactly(uint8_t* buffer, size_t length, uint32_t timeoutMs);
  // Wartet auf die Antwort mit der passenden Transaktions-ID (MBAP-Header + PDU) und überspringt
  // verspätete Antworten früherer Anfragen. Rückgabe: MODBUS_OK oder Übertragungsfehler.
  int receiveResponse(uint16_t transactionId, uint8_t* header, uint8_t* pdu, size_t& pduLength, uint32_t timeoutMs);
  // Protokolliert eine Nachricht als Hex-Zeile, wenn der Busmonitor (trace on) aktiv ist.
  void logMessage(const char* direction, const uint8_t* data, size_t length);

  char host[64] = "";
  uint16_t port = 502;
  int socketHandle = -1;
  uint16_t nextTransactionId = 1;
  uint32_t lastConnectAttemptMs = 0;
};

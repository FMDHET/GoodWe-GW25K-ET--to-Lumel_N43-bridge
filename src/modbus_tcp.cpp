// Modbus-TCP-Client auf lwIP-Sockets (ESP-IDF)
#include "modbus_tcp.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "log_buffer.h"
#include "modbus_rtu.h"
#include "util.h"
#include "wireguard_vpn.h"

static const char* TAG = "TCP";

static const size_t MBAP_HEADER_SIZE = 7;          // Transaktion(2) Protokoll(2) Länge(2) Unit(1)
static const size_t MAX_RESPONSE_PDU_SIZE = 253;   // Funktionscode + Daten
static const uint32_t RECONNECT_PAUSE_MS = 2000;   // nach einem fehlgeschlagenen Verbindungsversuch
static const uint8_t EXCEPTION_FLAG = 0x80;

void ModbusTcpClient::configure(const char* newHost, uint16_t newPort) {
  disconnect();
  copyString(host, newHost, sizeof(host));
  port = newPort;
}

void ModbusTcpClient::disconnect() {
  if (socketHandle >= 0) close(socketHandle);
  socketHandle = -1;
}

bool ModbusTcpClient::connectToServer(uint32_t timeoutMs) {
  // Nicht bei jeder Anfrage erneut versuchen, wenn das Gerät gerade nicht erreichbar ist
  if (lastConnectAttemptMs && millisSinceBoot() - lastConnectAttemptMs < RECONNECT_PAUSE_MS) return false;
  lastConnectAttemptMs = millisSinceBoot();

  // --- Adresse auflösen (IP-Adresse oder Hostname) ---
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* resolved = nullptr;
  char portText[8];
  snprintf(portText, sizeof(portText), "%u", port);
  if (getaddrinfo(host, portText, &hints, &resolved) != 0 || !resolved) {
    ESP_LOGW(TAG, "Adresse '%s' nicht auflösbar", host);
    return false;
  }

  // --- nicht blockierend verbinden, damit ein nicht erreichbares Gerät den Task nicht lange aufhält ---
  int newSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (newSocket < 0) {
    freeaddrinfo(resolved);
    return false;
  }
  // Liegt das Ziel hinter dem WireGuard-Tunnel, an die Tunnel-Adresse binden: der ESP-IDF-Routing-Hook
  // (ip4_route_src_hook) schickt Pakete mit dieser Absenderadresse über das WireGuard-Interface.
  uint32_t destinationIp = ((sockaddr_in*)resolved->ai_addr)->sin_addr.s_addr;
  uint32_t tunnelSourceIp = 0;
  if (wireguardSourceAddressFor(destinationIp, tunnelSourceIp)) {
    sockaddr_in sourceAddress = {};
    sourceAddress.sin_family = AF_INET;
    sourceAddress.sin_addr.s_addr = tunnelSourceIp;
    bind(newSocket, (sockaddr*)&sourceAddress, sizeof(sourceAddress));
  }
  fcntl(newSocket, F_SETFL, fcntl(newSocket, F_GETFL, 0) | O_NONBLOCK);
  int result = connect(newSocket, resolved->ai_addr, resolved->ai_addrlen);
  freeaddrinfo(resolved);
  if (result != 0 && errno != EINPROGRESS) {
    close(newSocket);
    return false;
  }
  fd_set writeSet;
  FD_ZERO(&writeSet);
  FD_SET(newSocket, &writeSet);
  timeval timeout = {(time_t)(timeoutMs / 1000), (suseconds_t)((timeoutMs % 1000) * 1000)};
  int connectError = 0;
  socklen_t errorLength = sizeof(connectError);
  if (select(newSocket + 1, nullptr, &writeSet, nullptr, &timeout) <= 0 ||
      getsockopt(newSocket, SOL_SOCKET, SO_ERROR, &connectError, &errorLength) != 0 || connectError != 0) {
    ESP_LOGW(TAG, "Verbindung zu %s:%u fehlgeschlagen (%s)", host, port,
             connectError ? strerror(connectError) : "Zeitüberschreitung");
    close(newSocket);
    return false;
  }
  fcntl(newSocket, F_SETFL, fcntl(newSocket, F_GETFL, 0) & ~O_NONBLOCK);
  int noDelay = 1;  // Anfragen sofort senden, nicht sammeln
  setsockopt(newSocket, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
  socketHandle = newSocket;
  ESP_LOGI(TAG, "Verbunden mit %s:%u", host, port);
  return true;
}

size_t ModbusTcpClient::receiveExactly(uint8_t* buffer, size_t length, uint32_t timeoutMs) {
  uint32_t startMs = millisSinceBoot();
  size_t received = 0;
  while (received < length) {
    uint32_t elapsedMs = millisSinceBoot() - startMs;
    if (elapsedMs >= timeoutMs) break;
    uint32_t remainingMs = timeoutMs - elapsedMs;
    timeval timeout = {(time_t)(remainingMs / 1000), (suseconds_t)((remainingMs % 1000) * 1000)};
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int count = recv(socketHandle, buffer + received, length - received, 0);
    if (count == 0) {  // Gegenstelle hat die Verbindung geschlossen
      disconnect();
      break;
    }
    if (count < 0) break;  // Zeitüberschreitung
    received += count;
  }
  return received;
}

int ModbusTcpClient::receiveResponse(uint16_t transactionId, uint8_t* header, uint8_t* pdu, size_t& pduLength,
                                     uint32_t timeoutMs) {
  for (;;) {
    size_t headerBytes = receiveExactly(header, MBAP_HEADER_SIZE, timeoutMs);
    if (headerBytes == 0) {
      // Noch nichts angekommen: Verbindung bleibt bestehen. Kommt die Antwort später doch,
      // wird sie bei der nächsten Anfrage anhand der Transaktions-ID verworfen.
      return socketHandle < 0 ? MODBUS_ERROR_CONNECTION : MODBUS_ERROR_TIMEOUT;
    }
    if (headerBytes < MBAP_HEADER_SIZE) {
      disconnect();  // halber Header: Datenstrom nicht mehr synchron -> neu aufbauen
      return MODBUS_ERROR_TIMEOUT;
    }
    uint16_t responseTransactionId = (header[0] << 8) | header[1];
    uint16_t protocolId = (header[2] << 8) | header[3];
    uint16_t followingLength = (header[4] << 8) | header[5];  // Unit-ID + PDU
    if (protocolId != 0 || followingLength < 2 || followingLength - 1 > MAX_RESPONSE_PDU_SIZE) {
      disconnect();
      return MODBUS_ERROR_FRAME;
    }
    pduLength = followingLength - 1;
    if (receiveExactly(pdu, pduLength, timeoutMs) != pduLength) {
      disconnect();
      return MODBUS_ERROR_TIMEOUT;
    }
    if (responseTransactionId == transactionId) return MODBUS_OK;
    // verspätete Antwort auf eine frühere Anfrage -> überspringen
  }
}

void ModbusTcpClient::logMessage(const char* direction, const uint8_t* data, size_t length) {
  if (!g_logModbusTraffic) return;
  char hexText[64 * 3 + 1];
  size_t textLength = 0;
  size_t bytesShown = length < 64 ? length : 64;
  for (size_t byteIndex = 0; byteIndex < bytesShown; byteIndex++)
    textLength += snprintf(hexText + textLength, sizeof(hexText) - textLength, "%02X ", data[byteIndex]);
  ESP_LOGI(TAG, "%-9s %3u Byte: %s%s", direction, (unsigned)length, hexText, length > bytesShown ? "..." : "");
}

int ModbusTcpClient::readRegisters(uint8_t slaveAddress, uint8_t functionCode, uint16_t startRegister,
                                   uint16_t registerCount, uint16_t* values, uint32_t timeoutMs) {
  if (registerCount < 1 || registerCount > 125) return MODBUS_ERROR_FRAME;
  if (socketHandle < 0 && !connectToServer(timeoutMs)) return MODBUS_ERROR_CONNECTION;

  // --- Anfrage: MBAP-Header + PDU (Funktionscode, Startregister, Anzahl) ---
  uint16_t transactionId = nextTransactionId++;
  uint8_t request[MBAP_HEADER_SIZE + 5] = {
      (uint8_t)(transactionId >> 8), (uint8_t)transactionId,  // Transaktions-ID
      0, 0,                                                   // Protokoll-ID: 0 = Modbus
      0, 6,                                                   // Länge: Unit-ID + 5 Byte PDU
      slaveAddress,                                           // Unit-ID
      functionCode,
      (uint8_t)(startRegister >> 8), (uint8_t)startRegister,
      (uint8_t)(registerCount >> 8), (uint8_t)registerCount,
  };
  logMessage("TX", request, sizeof(request));
  if (send(socketHandle, request, sizeof(request), 0) != (int)sizeof(request)) {
    disconnect();
    return MODBUS_ERROR_CONNECTION;
  }

  // --- Antwort-Header lesen; Antworten mit fremder Transaktions-ID (verspätete) überspringen ---
  uint8_t header[MBAP_HEADER_SIZE];
  uint8_t pdu[MAX_RESPONSE_PDU_SIZE];
  size_t pduLength = 0;
  int receiveResult = receiveResponse(transactionId, header, pdu, pduLength, timeoutMs);
  if (receiveResult != MODBUS_OK) return receiveResult;
  uint8_t loggedResponse[MBAP_HEADER_SIZE + MAX_RESPONSE_PDU_SIZE];
  memcpy(loggedResponse, header, sizeof(header));
  memcpy(loggedResponse + sizeof(header), pdu, pduLength);
  logMessage("RX", loggedResponse, sizeof(header) + pduLength);

  // --- Antwort prüfen ---
  if (header[6] != slaveAddress) return MODBUS_ERROR_WRONG_ADDRESS;
  if (pdu[0] == (functionCode | EXCEPTION_FLAG)) return pduLength >= 2 ? pdu[1] : MODBUS_ERROR_FRAME;
  if (pdu[0] != functionCode || pduLength < 2 || pdu[1] != 2 * registerCount || pduLength != 2u + 2 * registerCount)
    return MODBUS_ERROR_FRAME;
  for (uint16_t registerIndex = 0; registerIndex < registerCount; registerIndex++)
    values[registerIndex] = (pdu[2 + 2 * registerIndex] << 8) | pdu[3 + 2 * registerIndex];
  return MODBUS_OK;
}

int ModbusTcpClient::transact(uint8_t unitId, const uint8_t* requestPdu, size_t requestLength, uint8_t* responsePdu,
                              size_t& responseLength, uint32_t timeoutMs) {
  if (requestLength < 1 || requestLength > MAX_RESPONSE_PDU_SIZE) return MODBUS_ERROR_FRAME;
  if (socketHandle < 0 && !connectToServer(timeoutMs)) return MODBUS_ERROR_CONNECTION;

  // --- Anfrage: MBAP-Header + PDU unverändert ---
  uint16_t transactionId = nextTransactionId++;
  uint8_t request[MBAP_HEADER_SIZE + MAX_RESPONSE_PDU_SIZE];
  uint16_t followingLength = requestLength + 1;  // Unit-ID + PDU
  request[0] = transactionId >> 8;
  request[1] = transactionId;
  request[2] = 0;
  request[3] = 0;
  request[4] = followingLength >> 8;
  request[5] = followingLength;
  request[6] = unitId;
  memcpy(request + MBAP_HEADER_SIZE, requestPdu, requestLength);
  size_t requestSize = MBAP_HEADER_SIZE + requestLength;
  logMessage("TX", request, requestSize);
  if (send(socketHandle, request, requestSize, 0) != (int)requestSize) {
    disconnect();
    return MODBUS_ERROR_CONNECTION;
  }

  // --- Antwort mit passender Transaktions-ID lesen ---
  uint8_t header[MBAP_HEADER_SIZE];
  int receiveResult = receiveResponse(transactionId, header, responsePdu, responseLength, timeoutMs);
  if (receiveResult != MODBUS_OK) return receiveResult;
  uint8_t loggedResponse[MBAP_HEADER_SIZE + MAX_RESPONSE_PDU_SIZE];
  memcpy(loggedResponse, header, sizeof(header));
  memcpy(loggedResponse + sizeof(header), responsePdu, responseLength);
  logMessage("RX", loggedResponse, sizeof(header) + responseLength);
  return MODBUS_OK;
}

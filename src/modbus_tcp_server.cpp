// Modbus-TCP-Server (Bridge) auf lwIP-Sockets (ESP-IDF).
// Ein Task bedient per select() bis zu MAX_CLIENTS Verbindungen gleichzeitig. Jede Anfrage wird
// mit goodweForwardPdu() an den GoodWe weitergegeben; der MBAP-Header (Transaktions-ID, Unit-ID)
// des Clients bleibt in der Antwort erhalten.
#include "modbus_tcp_server.h"
#include <atomic>
#include <cerrno>
#include <cstring>
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "goodwe.h"
#include "lwip/sockets.h"
#include "modbus_rtu.h"
#include "util.h"

static const char* TAG = "BRIDGE";

static const int MAX_CLIENTS = 4;
static const size_t MBAP_HEADER_SIZE = 7;
static const size_t MAX_PDU_SIZE = 253;
static const uint32_t CLIENT_IDLE_TIMEOUT_MS = 120000;  // ungenutzte Verbindungen schließen
static const uint32_t REQUEST_BODY_TIMEOUT_MS = 2000;   // Rest einer begonnenen Anfrage
static const uint8_t EXCEPTION_FLAG = 0x80;
static const uint8_t EXCEPTION_GATEWAY_TARGET_FAILED = 0x0B;  // Gateway: Zielgerät antwortet nicht
static const uint8_t EXCEPTION_GATEWAY_PATH_UNAVAILABLE = 0x0A;

struct BridgeClient {
  int socketHandle = -1;
  uint32_t lastActivityMs = 0;
  char address[24] = "";
};

static BridgeClient clients[MAX_CLIENTS];
static bool serverRunning = false;
static std::atomic<uint32_t> requestCount{0};
static std::atomic<uint32_t> forwardErrorCount{0};
static std::atomic<uint32_t> connectionCount{0};

// Liest genau length Bytes vom Socket (mit Zeitlimit). Rückgabe: true, wenn vollständig.
static bool receiveExactly(int socketHandle, uint8_t* buffer, size_t length, uint32_t timeoutMs) {
  size_t receivedBytes = 0;
  while (receivedBytes < length) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socketHandle, &readSet);
    timeval timeout = {(long)(timeoutMs / 1000), (long)((timeoutMs % 1000) * 1000)};
    if (select(socketHandle + 1, &readSet, nullptr, nullptr, &timeout) <= 0) return false;
    int chunk = recv(socketHandle, buffer + receivedBytes, length - receivedBytes, 0);
    if (chunk <= 0) return false;
    receivedBytes += chunk;
  }
  return true;
}

// Schließt die Verbindung eines Clients und gibt den Platz frei.
static void closeClient(BridgeClient& client, const char* reason) {
  ESP_LOGI(TAG, "Client %s getrennt (%s)", client.address, reason);
  close(client.socketHandle);
  client.socketHandle = -1;
}

// Nimmt eine neue Verbindung an; ist kein Platz frei, wird sie sofort wieder geschlossen.
static void acceptClient(int listenSocket) {
  sockaddr_in clientAddress = {};
  socklen_t addressLength = sizeof(clientAddress);
  int socketHandle = accept(listenSocket, (sockaddr*)&clientAddress, &addressLength);
  if (socketHandle < 0) return;
  char addressText[24];
  inet_ntoa_r(clientAddress.sin_addr, addressText, sizeof(addressText));
  for (BridgeClient& client : clients) {
    if (client.socketHandle >= 0) continue;
    int noDelay = 1;
    setsockopt(socketHandle, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
    client.socketHandle = socketHandle;
    client.lastActivityMs = millisSinceBoot();
    copyString(client.address, addressText, sizeof(client.address));
    connectionCount++;
    ESP_LOGI(TAG, "Client %s verbunden", client.address);
    return;
  }
  ESP_LOGW(TAG, "Client %s abgewiesen: bereits %d Verbindungen", addressText, MAX_CLIENTS);
  close(socketHandle);
}

// Liest eine Anfrage des Clients, reicht sie an den GoodWe durch und sendet die Antwort zurück.
// Rückgabe: false, wenn die Verbindung geschlossen werden soll.
static bool handleClientRequest(BridgeClient& client) {
  uint8_t header[MBAP_HEADER_SIZE];
  int firstChunk = recv(client.socketHandle, header, 1, 0);  // select() meldete Daten oder Verbindungsende
  if (firstChunk <= 0) return false;
  if (!receiveExactly(client.socketHandle, header + 1, MBAP_HEADER_SIZE - 1, REQUEST_BODY_TIMEOUT_MS)) return false;
  uint16_t protocolId = (header[2] << 8) | header[3];
  uint16_t followingLength = (header[4] << 8) | header[5];
  if (protocolId != 0 || followingLength < 2 || followingLength - 1 > MAX_PDU_SIZE) return false;

  uint8_t requestPdu[MAX_PDU_SIZE];
  size_t requestLength = followingLength - 1;
  if (!receiveExactly(client.socketHandle, requestPdu, requestLength, REQUEST_BODY_TIMEOUT_MS)) return false;
  client.lastActivityMs = millisSinceBoot();
  requestCount++;

  // --- an den GoodWe durchreichen ---
  uint8_t unitId = header[6];
  uint8_t responsePdu[MAX_PDU_SIZE];
  size_t responseLength = 0;
  int result = goodweForwardPdu(unitId, requestPdu, requestLength, responsePdu, responseLength);
  if (result != MODBUS_OK) {
    // Übertragungsfehler als Gateway-Exception melden (Modbus Application Protocol, Kap. 7)
    forwardErrorCount++;
    responsePdu[0] = requestPdu[0] | EXCEPTION_FLAG;
    responsePdu[1] = result == GOODWE_NOT_CONFIGURED ? EXCEPTION_GATEWAY_PATH_UNAVAILABLE
                                                     : EXCEPTION_GATEWAY_TARGET_FAILED;
    responseLength = 2;
    ESP_LOGD(TAG, "FC%02X von %s: Weiterleitung fehlgeschlagen (%d)", requestPdu[0], client.address, result);
  }
  if (unitId == 0 && result == MODBUS_OK) return true;  // Broadcast: keine Antwort

  // --- Antwort mit dem MBAP-Header des Clients zurücksenden ---
  uint8_t response[MBAP_HEADER_SIZE + MAX_PDU_SIZE];
  memcpy(response, header, 4);  // Transaktions- und Protokoll-ID unverändert
  response[4] = (responseLength + 1) >> 8;
  response[5] = (responseLength + 1);
  response[6] = unitId;
  memcpy(response + MBAP_HEADER_SIZE, responsePdu, responseLength);
  size_t responseSize = MBAP_HEADER_SIZE + responseLength;
  return send(client.socketHandle, response, responseSize, 0) == (int)responseSize;
}

// Server-Task: wartet per select() auf neue Verbindungen und Anfragen aller Clients.
static void serverTask(void*) {
  int listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int reuseAddress = 1;
  setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));
  sockaddr_in bindAddress = {};
  bindAddress.sin_family = AF_INET;
  bindAddress.sin_port = htons(g_config.bridgePort);
  bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
  if (listenSocket < 0 || bind(listenSocket, (sockaddr*)&bindAddress, sizeof(bindAddress)) != 0 ||
      listen(listenSocket, 2) != 0) {
    ESP_LOGE(TAG, "Port %u kann nicht geöffnet werden (errno %d)", g_config.bridgePort, errno);
    if (listenSocket >= 0) close(listenSocket);
    serverRunning = false;
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(TAG, "Modbus-TCP-Bridge lauscht auf Port %u", g_config.bridgePort);

  for (;;) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenSocket, &readSet);
    int highestSocket = listenSocket;
    for (const BridgeClient& client : clients) {
      if (client.socketHandle < 0) continue;
      FD_SET(client.socketHandle, &readSet);
      if (client.socketHandle > highestSocket) highestSocket = client.socketHandle;
    }
    timeval timeout = {1, 0};
    int readyCount = select(highestSocket + 1, &readSet, nullptr, nullptr, &timeout);
    if (readyCount < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (FD_ISSET(listenSocket, &readSet)) acceptClient(listenSocket);

    uint32_t nowMs = millisSinceBoot();
    for (BridgeClient& client : clients) {
      if (client.socketHandle < 0) continue;
      if (FD_ISSET(client.socketHandle, &readSet)) {
        if (!handleClientRequest(client)) closeClient(client, "Verbindungsende");
      } else if (nowMs - client.lastActivityMs > CLIENT_IDLE_TIMEOUT_MS) {
        closeClient(client, "inaktiv");
      }
    }
  }
}

void modbusTcpServerStart() {
  if (!g_config.bridgeEnabled) return;
  serverRunning = true;
  xTaskCreate(serverTask, "mb_bridge", 4096, nullptr, 4, nullptr);
}

void modbusTcpServerStatusToJson(cJSON* jsonObject) {
  cJSON_AddBoolToObject(jsonObject, "enabled", g_config.bridgeEnabled);
  cJSON_AddBoolToObject(jsonObject, "running", serverRunning);
  cJSON_AddNumberToObject(jsonObject, "port", g_config.bridgePort);
  cJSON* clientArray = cJSON_AddArrayToObject(jsonObject, "clients");
  for (const BridgeClient& client : clients)
    if (client.socketHandle >= 0) cJSON_AddItemToArray(clientArray, cJSON_CreateString(client.address));
  cJSON_AddNumberToObject(jsonObject, "requests", requestCount.load());
  cJSON_AddNumberToObject(jsonObject, "errors", forwardErrorCount.load());
  cJSON_AddNumberToObject(jsonObject, "connections", connectionCount.load());
}

// Live-Konsole der Weboberfläche: überträgt den Log-Ringpuffer per WebSocket an den Browser
// und führt dort eingegebene Konsolenbefehle aus.
//
// Ablauf:
//  1. Browser öffnet ws://<gerät>/ws/console  -> bekommt den bisherigen Puffer-Inhalt.
//  2. Ein Task prüft alle 200 ms, ob neue Log-Daten vorliegen, und schickt sie an alle Clients.
//  3. Textnachrichten vom Browser sind Befehlszeilen (wie an der USB-Konsole); Befehl und Ausgabe
//     landen im Log-Puffer und damit bei allen verbundenen Browsern.
#include "web_console.h"
#include <cstring>
#include <string>
#include "console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_buffer.h"

static const char* TAG = "konsole";

static const int MAX_CONSOLE_CLIENTS = 4;
static const uint32_t BROADCAST_INTERVAL_MS = 200;
static const size_t MAX_COMMAND_LENGTH = 512;
static const size_t MAX_CHUNK_PER_SEND = 4096;  // größere Datenmengen werden aufgeteilt

static httpd_handle_t server = nullptr;
static bool (*checkAuthorization)(httpd_req_t*) = nullptr;
static int clientSockets[MAX_CONSOLE_CLIENTS] = {-1, -1, -1, -1};
static portMUX_TYPE clientListLock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t broadcastPosition = 0;  // bis zu dieser Log-Position wurde an alle verteilt

// Nimmt einen Client in die Verteilerliste auf. Ist die Liste voll, wird der älteste ersetzt.
static void addClient(int socket) {
  taskENTER_CRITICAL(&clientListLock);
  int freeSlot = 0;
  for (int slot = 0; slot < MAX_CONSOLE_CLIENTS; slot++) {
    if (clientSockets[slot] == socket) { freeSlot = -1; break; }
    if (clientSockets[slot] < 0) { freeSlot = slot; break; }
  }
  if (freeSlot >= 0) clientSockets[freeSlot] = socket;
  taskEXIT_CRITICAL(&clientListLock);
}

// Entfernt Clients, deren WebSocket-Verbindung nicht mehr besteht.
static void removeClosedClients() {
  for (int slot = 0; slot < MAX_CONSOLE_CLIENTS; slot++) {
    int socket = clientSockets[slot];
    if (socket >= 0 && httpd_ws_get_fd_info(server, socket) != HTTPD_WS_CLIENT_WEBSOCKET)
      clientSockets[slot] = -1;
  }
}

// Sendet Text als WebSocket-Textframe an einen Socket (außerhalb eines Request-Kontexts).
static void sendTextToSocket(int socket, const char* text, size_t length) {
  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = (uint8_t*)text;
  frame.len = length;
  httpd_ws_send_frame_async(server, socket, &frame);
}

// Daten für einen Verteil-Auftrag, der im HTTP-Server-Task ausgeführt wird
struct BroadcastJob {
  std::string text;
};

// Läuft im HTTP-Server-Task (httpd_queue_work), damit nicht parallel zu Requests
// auf dieselben Sockets geschrieben wird.
static void broadcastJobRun(void* argument) {
  BroadcastJob* job = static_cast<BroadcastJob*>(argument);
  removeClosedClients();
  for (int slot = 0; slot < MAX_CONSOLE_CLIENTS; slot++) {
    int socket = clientSockets[slot];
    if (socket < 0) continue;
    for (size_t offset = 0; offset < job->text.size(); offset += MAX_CHUNK_PER_SEND) {
      size_t chunkLength = job->text.size() - offset;
      if (chunkLength > MAX_CHUNK_PER_SEND) chunkLength = MAX_CHUNK_PER_SEND;
      sendTextToSocket(socket, job->text.data() + offset, chunkLength);
    }
  }
  delete job;
}

// Prüft zyklisch, ob neue Log-Daten vorliegen, und übergibt sie an den HTTP-Server zum Verteilen.
static void broadcastTask(void*) {
  broadcastPosition = logBufferWritePosition();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(BROADCAST_INTERVAL_MS));
    if (logBufferWritePosition() == broadcastPosition) continue;
    bool anyClient = false;
    for (int socket : clientSockets) anyClient |= socket >= 0;
    if (!anyClient) {
      broadcastPosition = logBufferWritePosition();  // niemand verbunden: nichts nachholen
      continue;
    }
    BroadcastJob* job = new BroadcastJob();
    broadcastPosition = logBufferReadFrom(broadcastPosition, job->text);
    if (httpd_queue_work(server, broadcastJobRun, job) != ESP_OK) delete job;
  }
}

// Führt eine vom Browser gesendete Befehlszeile aus und schreibt Befehl und Ausgabe
// in den Log-Puffer, damit alle verbundenen Browser sie sehen.
static void runCommandFromBrowser(const char* commandLine) {
  std::string echo = std::string("bridge> ") + commandLine + "\n";
  logBufferAppend(echo.data(), echo.size());
  std::string output;
  consoleRunCommand(commandLine, output);
  if (!output.empty() && output.back() != '\n') output += '\n';
  logBufferAppend(output.data(), output.size());
}

// WebSocket-Handler für /ws/console: Handshake (GET) und eingehende Textframes (Befehle).
static esp_err_t handleConsoleWebSocket(httpd_req_t* request) {
  if (request->method == HTTP_GET) {
    // Handshake: Zugriff prüfen, Verlauf schicken, Client für die Live-Ausgabe eintragen
    if (checkAuthorization && !checkAuthorization(request)) return ESP_FAIL;
    int socket = httpd_req_to_sockfd(request);
    std::string history;
    uint32_t historyEnd = logBufferReadFrom(0, history);
    // Nur bis zur aktuellen Verteilposition schicken; den Rest liefert der Verteil-Task
    if (historyEnd > broadcastPosition) history.resize(history.size() - (historyEnd - broadcastPosition));
    for (size_t offset = 0; offset < history.size(); offset += MAX_CHUNK_PER_SEND) {
      size_t chunkLength = history.size() - offset;
      if (chunkLength > MAX_CHUNK_PER_SEND) chunkLength = MAX_CHUNK_PER_SEND;
      sendTextToSocket(socket, history.data() + offset, chunkLength);
    }
    addClient(socket);
    ESP_LOGI(TAG, "Web-Konsole verbunden");
    return ESP_OK;
  }

  // Länge des eingehenden Frames ermitteln, dann den Inhalt lesen
  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t result = httpd_ws_recv_frame(request, &frame, 0);
  if (result != ESP_OK) return result;
  if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0 || frame.len > MAX_COMMAND_LENGTH) return ESP_OK;
  char commandLine[MAX_COMMAND_LENGTH + 1];
  frame.payload = (uint8_t*)commandLine;
  result = httpd_ws_recv_frame(request, &frame, frame.len);
  if (result != ESP_OK) return result;
  commandLine[frame.len] = 0;
  runCommandFromBrowser(commandLine);
  return ESP_OK;
}

void webConsoleBegin(httpd_handle_t httpServer, bool (*isAuthorized)(httpd_req_t*)) {
  server = httpServer;
  checkAuthorization = isAuthorized;
  httpd_uri_t consoleEndpoint = {};
  consoleEndpoint.uri = "/ws/console";
  consoleEndpoint.method = HTTP_GET;
  consoleEndpoint.handler = handleConsoleWebSocket;
  consoleEndpoint.is_websocket = true;
  httpd_register_uri_handler(server, &consoleEndpoint);
  xTaskCreate(broadcastTask, "web_console", 4096, nullptr, 2, nullptr);
}

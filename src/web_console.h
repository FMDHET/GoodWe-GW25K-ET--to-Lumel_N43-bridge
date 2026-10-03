#pragma once
// Live-Konsole der Weboberfläche über WebSocket (/ws/console)
#include "esp_http_server.h"

// Registriert den WebSocket-Endpunkt am laufenden HTTP-Server und startet den Task,
// der neue Log-Zeilen an alle verbundenen Browser verteilt.
// isAuthorized: Prüffunktion für den Web-Login (beim Verbindungsaufbau aufgerufen).
void webConsoleBegin(httpd_handle_t httpServer, bool (*isAuthorized)(httpd_req_t* request));

#pragma once
#include <cstdint>
#include <string>
#include "cJSON.h"

class RtuPort;

// Startet den HTTP-Server (Port 80) mit Weboberfläche und REST-API.
// ports: die beiden RS485-Ports, deren Zähler im Status angezeigt werden.
void webBegin(RtuPort* ports[2]);

// ---- gemeinsam für Web-API und Konsole

// Konfiguration inkl. Passwörtern und Zertifikaten sowie der Liste erlaubter GPIOs.
// Der Aufrufer muss das Ergebnis mit cJSON_Delete freigeben.
cJSON* webConfigJson();

// Kompletter Live-Status (WLAN, MQTT, OTA, Messwerte, GoodWe, Port-Statistik).
// Der Aufrufer muss das Ergebnis mit cJSON_Delete freigeben.
cJSON* webStatusJson();

// Übernimmt geänderte Einstellungen (auch Zertifikate), prüft und speichert sie.
// rebootRequired = true, wenn die Änderung erst nach einem Neustart wirkt (Ports, WLAN).
bool webApplyConfig(const cJSON* json, std::string& errorMessage, bool& rebootRequired);

// Startet das Gerät nach delayMs neu (damit die HTTP-Antwort vorher noch gesendet wird).
void scheduleReboot(uint32_t delayMs = 800);

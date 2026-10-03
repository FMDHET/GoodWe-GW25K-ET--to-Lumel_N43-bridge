#pragma once
// Modbus-TCP-Server (Bridge): reicht jede Anfrage aus dem lokalen Netz unverändert an den GoodWe
// durch – egal ob dieser per RS485 oder per Modbus TCP (z. B. über den VPN-Tunnel) angebunden ist.
// Kein Zwischenspeicher: Antworten und Exceptions kommen direkt vom Gerät.
#include "cJSON.h"

// Startet den Server-Task, wenn die Bridge in den Einstellungen aktiviert ist.
void modbusTcpServerStart();

// Status (aktiv, Port, verbundene Clients, Zähler) für die Web-Oberfläche in jsonObject eintragen.
void modbusTcpServerStatusToJson(cJSON* jsonObject);

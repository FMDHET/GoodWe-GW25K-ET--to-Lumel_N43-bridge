#pragma once
// Zeitsynchronisation per NTP (ESP-IDF esp_netif_sntp) und Zeitzone (POSIX-TZ).
#include <cstdint>
#include "cJSON.h"

// Setzt die Zeitzone und startet den NTP-Abgleich (beim Start, danach stündlich).
// Kann vor der WLAN-Verbindung aufgerufen werden; der Abgleich erfolgt, sobald das Netz steht.
void timeSyncStart();

// true, sobald die Uhr mindestens einmal per NTP gestellt wurde.
bool timeIsValid();

// Wartet bis zu timeoutMs auf eine gültige Uhrzeit. Rückgabe: true, wenn die Uhr gestellt ist.
bool timeWaitForSync(uint32_t timeoutMs);

// Trägt Ortszeit, Zeitzone, NTP-Server und Zeitpunkt des letzten Abgleichs in statusObject ein.
void timeStatusToJson(cJSON* statusObject);

#pragma once
#include "cJSON.h"

// WLAN (esp_wifi/esp_netif): Station mit gespeicherten Zugangsdaten,
// sonst bzw. bei Ausfall > 60 s zusätzlich eigener Access-Point mit Captive Portal.

// Startet WLAN und mDNS. Wartet bis zu 15 s auf die Verbindung mit dem gespeicherten WLAN;
// klappt das nicht, wird zusätzlich der Konfigurations-Access-Point gestartet.
void netBegin();

// Zyklisch aufrufen: schaltet den Access-Point bei längerem WLAN-Ausfall ein
// und 5 Minuten nach erfolgreicher Verbindung wieder aus.
void netLoop();

// true, solange der eigene Access-Point (192.168.4.1) aktiv ist.
bool netApActive();

// true, wenn eine Verbindung zum gespeicherten WLAN besteht und eine IP-Adresse vorliegt.
bool netStaConnected();

// Trägt den WLAN-Status (SSID, IP, Signal, AP, MAC) in statusObject ein.
void netStatusToJson(cJSON* statusObject);

// Sucht WLANs in der Umgebung (blockiert ca. 2-3 s).
// Liefert ein JSON-Array [{ssid, rssi, secure}], das der Aufrufer freigeben muss.
cJSON* netScan();

// Übernimmt die maximale WLAN-Sendeleistung aus der Konfiguration sofort (ohne Neustart).
void netApplyWifiTxPower();

// IP-Adresse im WLAN als Text, leer wenn nicht verbunden.
const char* netStaIp();

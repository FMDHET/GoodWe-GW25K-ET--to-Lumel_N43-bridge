#pragma once
#include "cJSON.h"

// MQTT über esp-mqtt (ESP-IDF). Topics (Basis = g_config.mqttBaseTopic):
//   <basis>/status           online / offline (retained, Last Will)
//   <basis>/goodwe/state     alle GoodWe-Werte als JSON
//   <basis>/goodwe/<id>      Einzelwerte (optional)
//   <basis>/meter/state      Werte, die als Lumel N43 ausgegeben werden
//   <basis>/bridge/state     Diagnose der Bridge
// Home Assistant Discovery unter <prefix>/sensor|binary_sensor/<node>/<id>/config

// Startet den MQTT-Task. Er verbindet sich, sobald MQTT aktiviert ist und WLAN besteht,
// und sendet die Werte im eingestellten Intervall.
void mqttStart();

// Nach geänderten MQTT-Einstellungen aufrufen: trennt die Verbindung und baut sie neu auf.
void mqttReconfigure();

// Sendet die Home-Assistant-Discovery beim nächsten Durchlauf erneut.
void mqttRepublishDiscovery();

// Trägt den MQTT-Status (verbunden, Broker, Zähler, letzter Fehler) in statusObject ein.
void mqttStatusToJson(cJSON* statusObject);

#pragma once
#include <cstdint>

// Minimaler DNS-Server für das Captive Portal: beantwortet jede A-Anfrage mit der AP-Adresse.

// Startet den DNS-Task auf UDP-Port 53. accessPointIpNetworkOrder: IPv4 des Access-Points.
void dnsServerStart(uint32_t accessPointIpNetworkOrder);

// Beendet den DNS-Task (spätestens nach 1 s).
void dnsServerStop();

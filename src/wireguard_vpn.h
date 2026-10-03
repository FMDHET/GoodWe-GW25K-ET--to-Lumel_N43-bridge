#pragma once
// WireGuard-VPN-Client (Komponente trombik/esp_wireguard) für den Zugriff auf entfernte Netze,
// z. B. ein RTU-zu-TCP-Gateway am GoodWe hinter einer FRITZ!Box.
//
// Routing: Nur Ziele aus "AllowedIPs" gehen durch den Tunnel, alles andere (MQTT, NTP, Web)
// bleibt im normalen WLAN. Liegt das Ziel im Subnetz der Tunnel-Adresse (typisch bei FRITZ!Box,
// z. B. 192.168.178.201/24), greift die normale Netz-Route. Für weitere AllowedIPs bindet der
// Modbus-TCP-Client seinen Socket an die Tunnel-Adresse; der ESP-IDF-Routing-Hook
// (ip4_route_src_hook) schickt die Pakete dann über das WireGuard-Interface.
#include <cstdint>
#include "cJSON.h"

// Startet den VPN-Task, wenn WireGuard in der Konfiguration aktiviert ist.
void wireguardStart();

// Trägt den Tunnel-Status (Zustand, Adresse, Gegenstelle, letzter Fehler) in statusObject ein.
void wireguardStatusToJson(cJSON* statusObject);

// Liefert die Tunnel-Adresse als Absender, wenn `destinationIp` über den Tunnel erreicht werden
// soll (Ziel liegt in AllowedIPs, aber nicht im eigenen Tunnel-Subnetz) und der Tunnel läuft.
// Beide Adressen in Netzwerk-Byte-Reihenfolge. Rückgabe: true = Socket an sourceIp binden.
bool wireguardSourceAddressFor(uint32_t destinationIp, uint32_t& sourceIp);

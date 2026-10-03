#pragma once
// Absturzanalyse: Neustartgrund (esp_reset_reason) und Zusammenfassung des letzten Core-Dumps
// (esp_core_dump, Partition "coredump"). Zusätzlich ein Schutz vor Absturzschleifen beim VPN-Start.
#include "cJSON.h"

// Früh in app_main() aufrufen: liest Neustartgrund und ggf. den gespeicherten Absturzbericht.
void diagnosticsBegin();

// Trägt Neustartgrund und letzten Absturz (Task, Programmadresse, Backtrace) in statusObject ein.
void diagnosticsStatusToJson(cJSON* statusObject);

// ---- Schutz vor Absturzschleifen beim Aufbau des VPN-Tunnels

// true, wenn der VPN-Start in den letzten Starts wiederholt zu einem Absturz geführt hat.
bool diagnosticsVpnStartBlocked();
// Vom VPN-Task vor dem Tunnelaufbau aufrufen.
void diagnosticsVpnStartBegin();
// Vom VPN-Task aufrufen, sobald der Tunnel stabil läuft (setzt den Absturzzähler zurück).
void diagnosticsVpnStartSucceeded();

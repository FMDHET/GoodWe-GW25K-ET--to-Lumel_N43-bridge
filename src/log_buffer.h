#pragma once
// Ringpuffer für alle Log-Ausgaben (ESP_LOGx) und Konsolen-Antworten.
// Die Weboberfläche liest daraus die Live-Konsole; die Ausgabe auf USB bleibt unverändert.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// Hängt sich per esp_log_set_vprintf() in die ESP-IDF-Logausgabe ein.
// Muss als Erstes in app_main() aufgerufen werden, damit auch die Startmeldungen erfasst werden.
void logBufferInit();

// Fügt Text direkt in den Puffer ein (z. B. Ausgaben von Konsolenbefehlen aus der Weboberfläche).
void logBufferAppend(const char* text, size_t length);

// Gesamtzahl der jemals geschriebenen Bytes (fortlaufende Position, läuft nicht zurück).
uint32_t logBufferWritePosition();

// Kopiert alle Bytes ab `fromPosition` bis zum aktuellen Ende nach `text`.
// Ist `fromPosition` bereits überschrieben, beginnt die Kopie beim ältesten noch vorhandenen Byte.
// Rückgabe: Position direkt hinter dem letzten kopierten Byte.
uint32_t logBufferReadFrom(uint32_t fromPosition, std::string& text);

// Schalter für die Protokollierung jedes Modbus-Frames (Hex-Dump), zur Laufzeit umschaltbar.
extern std::atomic<bool> g_logModbusTraffic;

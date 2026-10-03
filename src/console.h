#pragma once
#include <string>
// Serielle Konsole (esp_console REPL über USB-Serial-JTAG)

// Startet die Eingabeaufforderung "bridge>" mit allen Befehlen (help zeigt die Liste).
void consoleStart();

// Vor dem Start der Modbus-Ports aufrufen: führt eine per "detect"/"diag" angeforderte
// Bus-Diagnose aus, solange die UARTs noch frei sind.
void consoleEarlyBoot();

// Führt eine Befehlszeile (z. B. "status") aus und liefert die Textausgabe des Befehls.
// Wird von der Konsole der Weboberfläche benutzt. Rückgabe: true, wenn der Befehl erfolgreich war.
bool consoleRunCommand(const char* commandLine, std::string& output);

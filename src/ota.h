#pragma once
// OTA-Update nach ESP-IDF (app_update / esp_ota_ops) mit A/B-Partitionen und App-Rollback.
//
// Ablauf (angelehnt an das IDF-Beispiel system/ota/native_ota_example):
//  1. Upload-Stream: Image-Header (Magic, Chip-ID des Zielchips) und esp_app_desc_t prüfen
//     (Projektname "modbus-bridge", Version != zuletzt verworfener Version).
//  2. esp_ota_begin() auf die inaktive Partition, esp_ota_write() für jeden Block.
//  3. esp_ota_end() prüft das komplette Image (Segmente, Prüfsumme, SHA-256).
//  4. esp_ota_set_boot_partition(), Neustart.
//  5. Die neue App startet im Zustand ESP_OTA_IMG_PENDING_VERIFY. Nach 60 s fehlerfreiem Lauf
//     esp_ota_mark_app_valid_cancel_rollback(), sonst esp_ota_mark_app_invalid_rollback_and_reboot().
//  6. Reset/Absturz/Task-Watchdog vor der Bestätigung -> der Bootloader
//     (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) startet automatisch die vorherige App.
#include <cstddef>
#include <cstdint>
#include <string>
#include "cJSON.h"

// Früh in app_main() aufrufen: ermittelt, ob die laufende Firmware noch auf Bewährung läuft
// und ob der Bootloader zuletzt eine Firmware verworfen hat.
void otaBegin();

// Zyklisch aufrufen: bestätigt eine neue Firmware nach 60 s fehlerfreiem Lauf oder
// löst nach 5 Minuten ohne Bestätigung den Rollback aus.
void otaLoop();

// Trägt Version, Partitionen und Update-Status in statusObject ein.
void otaStatusToJson(cJSON* statusObject);

// Wird von den Modbus-Tasks regelmäßig aufgerufen; dient als Lebenszeichen für die Gesundheitsprüfung.
void otaReportTaskAlive(uint8_t portIndex);

// true, solange eine neue Firmware noch auf Bewährung läuft (vor der Bestätigung nach 60 s).
bool otaIsAwaitingConfirmation();

// Version der laufenden Firmware (PROJECT_VER aus CMakeLists.txt).
const char* otaRunningVersion();

// ---- Firmware-Upload als Datenstrom: Start -> beliebig oft Write -> Finish (oder Abort)

// Bereitet das Schreiben in die inaktive App-Partition vor.
bool otaUploadStart(std::string& errorMessage);

// Schreibt den nächsten Datenblock. Prüft beim ersten Block Kopf und App-Beschreibung des Images.
bool otaUploadWrite(const uint8_t* data, size_t length, std::string& errorMessage);

// Schließt den Upload ab, prüft das komplette Image (SHA-256) und stellt die Bootpartition um.
bool otaUploadFinish(std::string& errorMessage);

// Bricht einen laufenden Upload ab; die bisherige Firmware bleibt aktiv.
void otaUploadAbort();

// Vor einem gewollten Neustart aufrufen: bestätigt eine gesund laufende Firmware auf Bewährung,
// damit der Neustart keinen Rollback auslöst.
void otaConfirmBeforeIntentionalReboot();

// Stellt die Bootpartition auf die Firmware in der anderen Partition um (wirksam nach Neustart).
bool otaSwitchToPreviousFirmware(std::string& errorMessage);

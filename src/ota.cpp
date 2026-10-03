// Firmware-Update (OTA) und App-Rollback: nimmt ein Image als Datenstrom entgegen, prüft und schreibt es
// in die inaktive Partition, bestätigt eine neu gestartete Firmware nach einer Bewährungszeit oder
// verwirft sie. Ablauf im Detail siehe ota.h.
#include "ota.h"
#include <cstdio>
#include "config.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "net.h"
#include "chip.h"
#include "util.h"

static const char* TAG = "ota";

// Kennung für den Übergang von Firmware 1.x (Arduino): deren Upload-Prüfung sucht diesen Text
__attribute__((used)) static const char LEGACY_IMAGE_MARKER[] = "MBBRIDGE-FW-IMAGE:" CHIP_TARGET_ID ":v1";

static const uint32_t CONFIRM_AFTER_MS = 60000;   // Bewährungszeit
static const uint32_t GIVE_UP_AFTER_MS = 300000;  // ohne Bestätigung -> Rollback
static const uint32_t MIN_FREE_HEAP_BYTES = 30000;
static const uint32_t TASK_ALIVE_TIMEOUT_MS = 10000;
static const int MODBUS_PORT_COUNT = 3;  // RTU1, RTU2, GoodWe über TCP

static bool awaitingConfirmation = false;          // laufende App im Zustand PENDING_VERIFY
static char lastInvalidVersion[32] = "";           // vom Bootloader verworfene App
static char lastResultMessage[112] = "";
static volatile uint32_t lastTaskAliveMs[MODBUS_PORT_COUNT] = {0, 0, 0};

// Upload-Zustand
static esp_ota_handle_t otaHandle = 0;
static const esp_partition_t* targetPartition = nullptr;
static bool uploadActive = false;
static bool otaWriteStarted = false;  // esp_ota_begin() erfolgreich, Handle gültig
static size_t receivedBytes = 0;
// Anfang des Images: Image-Header + erster Segment-Header + App-Beschreibung, wird vor esp_ota_begin() geprüft
static uint8_t imageHeaderBuffer[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                                 sizeof(esp_app_desc_t)];
static size_t imageHeaderLength = 0;

// Speichert das Ergebnis eines OTA-Vorgangs im RAM und im NVS (überlebt den Neustart) und loggt es.
static void saveResult(const char* message) {
  copyString(lastResultMessage, message, sizeof(lastResultMessage));
  nvsWriteString("ota", "result", message);
  ESP_LOGI(TAG, "%s", message);
}

// Liest die App-Beschreibung einer Partition. Rückgabe true, wenn dort eine App liegt, die der
// Bootloader nicht als ungültig oder abgebrochen markiert hat.
static bool readValidAppDescription(const esp_partition_t* partition, esp_app_desc_t& description) {
  esp_ota_img_states_t imageState;
  return partition && esp_ota_get_partition_description(partition, &description) == ESP_OK &&
         !(esp_ota_get_state_partition(partition, &imageState) == ESP_OK &&
           (imageState == ESP_OTA_IMG_INVALID || imageState == ESP_OTA_IMG_ABORTED));
}

// Liefert die Version der laufenden App (PROJECT_VER).
const char* otaRunningVersion() { return esp_app_get_description()->version; }

// Lebenszeichen eines Modbus-Tasks (portIndex 0/1); wird für die Bestätigung neuer Firmware geprüft.
void otaReportTaskAlive(uint8_t portIndex) {
  if (portIndex < MODBUS_PORT_COUNT) lastTaskAliveMs[portIndex] = millisSinceBoot();
}

// Ermittelt beim Start, ob die laufende App noch bestätigt werden muss und ob der Bootloader zuvor
// eine App verworfen hat; lädt das letzte OTA-Ergebnis aus dem NVS.
void otaBegin() {
  const esp_partition_t* runningPartition = esp_ota_get_running_partition();
  esp_ota_img_states_t imageState;
  awaitingConfirmation = esp_ota_get_state_partition(runningPartition, &imageState) == ESP_OK &&
                         imageState == ESP_OTA_IMG_PENDING_VERIFY;

  const esp_partition_t* invalidPartition = esp_ota_get_last_invalid_partition();
  esp_app_desc_t invalidDescription;
  if (invalidPartition && esp_ota_get_partition_description(invalidPartition, &invalidDescription) == ESP_OK)
    copyString(lastInvalidVersion, invalidDescription.version, sizeof(lastInvalidVersion));

  copyString(lastResultMessage, nvsReadString("ota", "result").c_str(), sizeof(lastResultMessage));
  ESP_LOGI(TAG, "App %s auf %s, %s%s%s", otaRunningVersion(), runningPartition->label,
           awaitingConfirmation ? "auf Bewährung (Bestätigung nach 60 s)" : "bestätigt",
           lastInvalidVersion[0] ? ", verworfen wurde: " : "", lastInvalidVersion);
}

// Prüft, ob die neue Firmware ordentlich läuft: genug Heap, alle aktiven Modbus-Tasks melden sich,
// WLAN oder Access-Point aktiv. Bei false steht der Grund in reason.
static bool isSystemHealthy(std::string& reason) {
  if (esp_get_free_heap_size() < MIN_FREE_HEAP_BYTES) {
    reason = "zu wenig freier Speicher";
    return false;
  }
  for (int portIndex = 0; portIndex < 2; portIndex++) {
    if (g_config.port[portIndex].role == ROLE_OFF) continue;
    if (millisSinceBoot() - lastTaskAliveMs[portIndex] > TASK_ALIVE_TIMEOUT_MS) {
      reason = "Modbus-Task RTU" + std::to_string(portIndex + 1) + " läuft nicht";
      return false;
    }
  }
  // Index 2: GoodWe über Modbus TCP
  if (g_config.goodweTransport == GOODWE_VIA_TCP && millisSinceBoot() - lastTaskAliveMs[2] > TASK_ALIVE_TIMEOUT_MS) {
    reason = "GoodWe-TCP-Task läuft nicht";
    return false;
  }
  if (!netStaConnected() && !netApActive()) {
    reason = "weder WLAN noch Access-Point aktiv";
    return false;
  }
  return true;
}

// Zyklisch aufgerufen: bestätigt eine neue Firmware nach der Bewährungszeit, sofern das System gesund
// ist. Bleibt sie bis GIVE_UP_AFTER_MS ungesund, wird sie verworfen und die vorherige App gestartet.
void otaLoop() {
  if (!awaitingConfirmation) return;
  uint32_t uptimeMs = millisSinceBoot();
  if (uptimeMs < CONFIRM_AFTER_MS) return;
  std::string reason;
  if (isSystemHealthy(reason)) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      awaitingConfirmation = false;
      char message[96];
      snprintf(message, sizeof(message), "Firmware %s bestätigt", otaRunningVersion());
      saveResult(message);
    }
  } else if (uptimeMs > GIVE_UP_AFTER_MS) {
    char message[112];
    snprintf(message, sizeof(message), "Firmware %s verworfen: %s", otaRunningVersion(), reason.c_str());
    saveResult(message);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

bool otaIsAwaitingConfirmation() { return awaitingConfirmation; }

// Vor einem gewollten Neustart (Einstellungen gespeichert, Reboot-Knopf): Eine Firmware auf Bewährung,
// die bis hierhin gesund läuft, wird bestätigt. Sonst würde der Bootloader den Neustart als Fehlstart
// werten und auf die alte Firmware zurückschalten. Abstürze lösen den Rollback weiterhin aus.
void otaConfirmBeforeIntentionalReboot() {
  if (!awaitingConfirmation) return;
  std::string reason;
  if (!isSystemHealthy(reason)) return;
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    awaitingConfirmation = false;
    char message[112];
    snprintf(message, sizeof(message), "Firmware %s vor gewolltem Neustart bestätigt", otaRunningVersion());
    saveResult(message);
  }
}

// Trägt den OTA-Status für die Weboberfläche in statusObject ein (Version, Partitionen, Bestätigung,
// Rollback-Möglichkeit, Upload-Fortschritt).
void otaStatusToJson(cJSON* statusObject) {
  const esp_partition_t* runningPartition = esp_ota_get_running_partition();
  const esp_partition_t* otherPartition = esp_ota_get_next_update_partition(nullptr);
  const esp_app_desc_t* runningApp = esp_app_get_description();

  // Laufende App
  cJSON_AddStringToObject(statusObject, "version", runningApp->version);
  char buildInfo[40];
  snprintf(buildInfo, sizeof(buildInfo), "%s %s, IDF %s", runningApp->date, runningApp->time, runningApp->idf_ver);
  cJSON_AddStringToObject(statusObject, "build", buildInfo);
  cJSON_AddStringToObject(statusObject, "partition", runningPartition ? runningPartition->label : "?");
  cJSON_AddStringToObject(statusObject, "nextPartition", otherPartition ? otherPartition->label : "?");
  cJSON_AddNumberToObject(statusObject, "partitionSize", otherPartition ? otherPartition->size : 0);

  // Bestätigung / Rollback durch den Bootloader
  cJSON_AddBoolToObject(statusObject, "pending", awaitingConfirmation);
  cJSON_AddNumberToObject(statusObject, "confirmIn",
                          awaitingConfirmation && millisSinceBoot() < CONFIRM_AFTER_MS
                              ? (CONFIRM_AFTER_MS - millisSinceBoot()) / 1000
                              : 0);
  cJSON_AddBoolToObject(statusObject, "rolledBack", lastInvalidVersion[0] != 0);
  cJSON_AddStringToObject(statusObject, "invalidVersion", lastInvalidVersion);

  // Version in der anderen Partition (direkt aus deren esp_app_desc_t)
  esp_app_desc_t otherDescription;
  bool otherValid = readValidAppDescription(otherPartition, otherDescription);
  cJSON_AddStringToObject(statusObject, "previous", otherValid ? otherDescription.version : "");
  cJSON_AddBoolToObject(statusObject, "canRollback", otherValid && !awaitingConfirmation);

  // Upload
  cJSON_AddStringToObject(statusObject, "lastResult", lastResultMessage);
  cJSON_AddBoolToObject(statusObject, "uploading", uploadActive);
  cJSON_AddNumberToObject(statusObject, "received", receivedBytes);
  // Referenz verhindert, dass der Linker die Übergangskennung entfernt
  cJSON_AddStringToObject(statusObject, "imageMarker", LEGACY_IMAGE_MARKER);
}

// ---------------------------------------------------------------- Upload

// Bereitet einen neuen Upload vor (ein laufender wird abgebrochen). Abgelehnt, solange die laufende
// Firmware noch nicht bestätigt ist oder keine Zielpartition existiert; Grund dann in errorMessage.
bool otaUploadStart(std::string& errorMessage) {
  otaUploadAbort();
  if (awaitingConfirmation) {
    errorMessage = "Laufende Firmware ist noch nicht bestätigt - bitte 60 s warten";
    return false;
  }
  targetPartition = esp_ota_get_next_update_partition(nullptr);
  if (!targetPartition) {
    errorMessage = "Keine OTA-Partition gefunden";
    return false;
  }
  receivedBytes = 0;
  imageHeaderLength = 0;
  otaWriteStarted = false;
  uploadActive = true;
  ESP_LOGI(TAG, "Upload gestartet -> %s", targetPartition->label);
  return true;
}

// Prüft Image-Header und App-Beschreibung des neuen Images in imageHeaderBuffer (wie
// native_ota_example): ESP-Image, passender Chip (ESP32-C6/-C3), gleiches Projekt, nicht die zuletzt verworfene Version.
// Bei false steht der Grund in errorMessage.
static bool validateImageHeader(std::string& errorMessage) {
  const esp_image_header_t* imageHeader = (const esp_image_header_t*)imageHeaderBuffer;
  if (imageHeader->magic != ESP_IMAGE_HEADER_MAGIC) {
    errorMessage = "Keine ESP-Firmware (.bin)";
    return false;
  }
  // Chip-ID des laufenden Zielchips (ESP32-C6 bzw. -C3): verhindert z. B. C3-Firmware auf einem C6
  if (imageHeader->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
    errorMessage = "Firmware ist nicht für den " CHIP_NAME;
    return false;
  }
  // Die App-Beschreibung folgt direkt auf Image-Header und ersten Segment-Header
  esp_app_desc_t newApp;
  memcpy(&newApp, imageHeaderBuffer + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
         sizeof(newApp));
  const esp_app_desc_t* runningApp = esp_app_get_description();

  if (newApp.magic_word != ESP_APP_DESC_MAGIC_WORD) {
    errorMessage = "Image ohne App-Beschreibung";
    return false;
  }
  // Firmware 1.x (Arduino) hatte den Projektnamen der Arduino-Bibliothek -> nur eigenes Projekt zulassen
  if (strncmp(newApp.project_name, runningApp->project_name, sizeof(newApp.project_name)) != 0) {
    errorMessage = std::string("Image gehört nicht zu diesem Projekt (") + newApp.project_name + ")";
    return false;
  }
  if (lastInvalidVersion[0] && !strncmp(newApp.version, lastInvalidVersion, sizeof(newApp.version))) {
    errorMessage = std::string("Version ") + newApp.version + " wurde bereits vom Bootloader verworfen (Absturz)";
    return false;
  }
  ESP_LOGI(TAG, "Neues Image: %s %s (laufend %s)", newApp.project_name, newApp.version, runningApp->version);
  return true;
}

// Bricht den Upload wegen reason ab, speichert das Ergebnis und gibt reason in errorMessage zurück.
// Rückgabe immer false, damit Aufrufer direkt "return failUpload(...)" schreiben können.
static bool failUpload(std::string& errorMessage, const std::string& reason) {
  errorMessage = reason;
  otaUploadAbort();
  saveResult(("Upload abgelehnt: " + reason).c_str());
  return false;
}

// Nimmt den nächsten Block des Upload-Streams an. Zuerst wird der Image-Anfang gepuffert und geprüft,
// erst danach esp_ota_begin() aufgerufen, damit ungeeignete Images die Partition nicht anrühren.
// Rückgabe false bei Fehler (Grund in errorMessage) oder wenn kein Upload aktiv ist.
bool otaUploadWrite(const uint8_t* data, size_t length, std::string& errorMessage) {
  if (!uploadActive) return false;
  if (!otaWriteStarted) {
    // Erst genug Bytes für Header + App-Beschreibung sammeln
    size_t bytesToCopy = sizeof(imageHeaderBuffer) - imageHeaderLength;
    if (bytesToCopy > length) bytesToCopy = length;
    memcpy(imageHeaderBuffer + imageHeaderLength, data, bytesToCopy);
    imageHeaderLength += bytesToCopy;
    receivedBytes += bytesToCopy;
    data += bytesToCopy;
    length -= bytesToCopy;
    if (imageHeaderLength < sizeof(imageHeaderBuffer)) return true;

    // Header vollständig: prüfen, OTA beginnen und den gepufferten Anfang schreiben
    std::string reason;
    if (!validateImageHeader(reason)) return failUpload(errorMessage, reason);
    esp_err_t result = esp_ota_begin(targetPartition, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle);
    if (result != ESP_OK) return failUpload(errorMessage, std::string("esp_ota_begin: ") + esp_err_to_name(result));
    otaWriteStarted = true;
    result = esp_ota_write(otaHandle, imageHeaderBuffer, imageHeaderLength);
    if (result != ESP_OK) return failUpload(errorMessage, std::string("esp_ota_write: ") + esp_err_to_name(result));
  }
  // Restliche Daten dieses Blocks direkt schreiben
  if (length) {
    esp_err_t result = esp_ota_write(otaHandle, data, length);
    if (result != ESP_OK) return failUpload(errorMessage, std::string("esp_ota_write: ") + esp_err_to_name(result));
    receivedBytes += length;
  }
  return true;
}

// Schließt den Upload ab: esp_ota_end() prüft das komplette Image, danach wird die neue Partition als
// Boot-Partition gesetzt. Den Neustart löst der Aufrufer aus. Rückgabe false mit Grund in errorMessage.
bool otaUploadFinish(std::string& errorMessage) {
  if (!uploadActive) {
    errorMessage = "Kein Upload aktiv";
    return false;
  }
  if (!otaWriteStarted) return failUpload(errorMessage, "Datei zu klein für ein Firmware-Image");
  esp_err_t result = esp_ota_end(otaHandle);  // prüft das komplette Image inkl. SHA-256
  otaWriteStarted = false;
  otaHandle = 0;
  if (result != ESP_OK) {
    uploadActive = false;
    return failUpload(errorMessage, result == ESP_ERR_OTA_VALIDATE_FAILED
                                        ? "Image-Prüfung fehlgeschlagen (beschädigt?)"
                                        : std::string("esp_ota_end: ") + esp_err_to_name(result));
  }
  result = esp_ota_set_boot_partition(targetPartition);
  uploadActive = false;
  if (result != ESP_OK)
    return failUpload(errorMessage, std::string("esp_ota_set_boot_partition: ") + esp_err_to_name(result));
  char message[112];
  snprintf(message, sizeof(message), "Neue Firmware (%u Byte) in %s installiert - Neustart", (unsigned)receivedBytes,
           targetPartition->label);
  saveResult(message);
  return true;
}

// Bricht einen laufenden Upload ab und gibt das OTA-Handle frei (falls esp_ota_begin() schon lief).
void otaUploadAbort() {
  if (otaWriteStarted) esp_ota_abort(otaHandle);
  otaWriteStarted = false;
  otaHandle = 0;
  uploadActive = false;
}

// Setzt die App der anderen Partition als Boot-Partition (manueller Rollback), sofern dort eine gültige
// App liegt. Den Neustart löst der Aufrufer aus. Rückgabe false mit Grund in errorMessage.
bool otaSwitchToPreviousFirmware(std::string& errorMessage) {
  const esp_partition_t* otherPartition = esp_ota_get_next_update_partition(nullptr);
  esp_app_desc_t otherDescription;
  if (!readValidAppDescription(otherPartition, otherDescription)) {
    errorMessage = "Keine gültige Firmware in der anderen Partition";
    return false;
  }
  esp_err_t result = esp_ota_set_boot_partition(otherPartition);
  if (result != ESP_OK) {
    errorMessage = std::string("Umschalten fehlgeschlagen: ") + esp_err_to_name(result);
    return false;
  }
  char message[96];
  snprintf(message, sizeof(message), "Manuell auf Firmware %s (%s) umgeschaltet", otherDescription.version,
           otherPartition->label);
  saveResult(message);
  return true;
}

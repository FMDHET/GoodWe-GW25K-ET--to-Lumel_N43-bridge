// Absturzanalyse mit esp_reset_reason() und esp_core_dump (ESP-IDF)
#include "diagnostics.h"
#include <cstdio>
#include <string>
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_system.h"

static const char* TAG = "diag";

static const uint32_t VPN_START_MARKER = 0x56504E31;  // "VPN1": Tunnelaufbau lief beim letzten Neustart
static const uint32_t MAX_VPN_START_CRASHES = 2;

// Überlebt einen Software-Neustart (nicht aber das Abschalten der Versorgung)
static RTC_NOINIT_ATTR uint32_t vpnStartMarker;
static RTC_NOINIT_ATTR uint32_t vpnStartCrashCount;

static esp_reset_reason_t resetReason = ESP_RST_UNKNOWN;
static bool crashReportAvailable = false;
static char crashTask[20] = "";
static uint32_t crashProgramCounter = 0;
static std::string crashBacktrace;
static bool vpnStartBlocked = false;

// Klartext zum Neustartgrund
static const char* resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "Einschalten";
    case ESP_RST_EXT: return "externer Reset";
    case ESP_RST_SW: return "Software-Neustart";
    case ESP_RST_PANIC: return "Absturz (Panic)";
    case ESP_RST_INT_WDT: return "Interrupt-Watchdog";
    case ESP_RST_TASK_WDT: return "Task-Watchdog";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep-Sleep";
    case ESP_RST_BROWNOUT: return "Unterspannung (Brownout)";
    case ESP_RST_USB: return "USB-Reset";
    default: return "unbekannt";
  }
}

// true für Neustarts, die auf einen Fehler in der Firmware hindeuten
static bool isCrash(esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}

// Liest die Zusammenfassung des gespeicherten Core-Dumps (falls vorhanden).
static void readCrashReport() {
  if (esp_core_dump_image_check() != ESP_OK) return;
  esp_core_dump_summary_t* summary = (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
  if (!summary) return;
  if (esp_core_dump_get_summary(summary) == ESP_OK) {
    crashReportAvailable = true;
    snprintf(crashTask, sizeof(crashTask), "%s", summary->exc_task);
    crashProgramCounter = summary->exc_pc;
    // RISC-V (ESP32-C6/-C3): kein Backtrace auf dem Gerät; Rücksprungadresse und Fehlerursache genügen,
    // um mit addr2line und der firmware.elf die betroffenen Funktionen zu bestimmen.
    char details[96];
    snprintf(details, sizeof(details), "RA 0x%08lx, mcause 0x%lx, mtval 0x%08lx", (unsigned long)summary->ex_info.ra,
             (unsigned long)summary->ex_info.mcause, (unsigned long)summary->ex_info.mtval);
    crashBacktrace = details;
  }
  free(summary);
}

void diagnosticsBegin() {
  resetReason = esp_reset_reason();
  bool crashed = isCrash(resetReason);
  if (crashed) readCrashReport();

  // --- Absturzschleife beim VPN-Start erkennen ---
  if (resetReason == ESP_RST_POWERON || resetReason == ESP_RST_BROWNOUT) {
    vpnStartMarker = 0;  // RTC-Speicher enthält nach dem Einschalten zufällige Werte
    vpnStartCrashCount = 0;
  }
  if (crashed && vpnStartMarker == VPN_START_MARKER) {
    vpnStartCrashCount++;
  } else if (!crashed) {
    vpnStartCrashCount = 0;  // normaler Neustart (z. B. nach Speichern): neuer Versuch erlaubt
  }
  vpnStartMarker = 0;
  vpnStartBlocked = vpnStartCrashCount >= MAX_VPN_START_CRASHES;

  ESP_LOGI(TAG, "Neustartgrund: %s", resetReasonText(resetReason));
  if (crashReportAvailable)
    ESP_LOGW(TAG, "Letzter Absturz in Task '%s' bei PC 0x%08lx, %s", crashTask,
             (unsigned long)crashProgramCounter, crashBacktrace.c_str());
  if (vpnStartBlocked)
    ESP_LOGE(TAG, "VPN-Start hat %lu-mal zum Absturz geführt - VPN wird bis zum nächsten Speichern nicht gestartet",
             (unsigned long)vpnStartCrashCount);
}

void diagnosticsStatusToJson(cJSON* statusObject) {
  cJSON_AddStringToObject(statusObject, "resetReason", resetReasonText(resetReason));
  cJSON_AddBoolToObject(statusObject, "crashReport", crashReportAvailable);
  if (crashReportAvailable) {
    cJSON_AddStringToObject(statusObject, "crashTask", crashTask);
    char programCounter[12];
    snprintf(programCounter, sizeof(programCounter), "0x%08lx", (unsigned long)crashProgramCounter);
    cJSON_AddStringToObject(statusObject, "crashPc", programCounter);
    cJSON_AddStringToObject(statusObject, "crashBacktrace", crashBacktrace.c_str());
  }
  cJSON_AddBoolToObject(statusObject, "vpnStartBlocked", vpnStartBlocked);
}

bool diagnosticsVpnStartBlocked() { return vpnStartBlocked; }
void diagnosticsVpnStartBegin() { vpnStartMarker = VPN_START_MARKER; }
void diagnosticsVpnStartSucceeded() {
  vpnStartMarker = 0;
  vpnStartCrashCount = 0;
}

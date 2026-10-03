// Zeitsynchronisation per NTP über esp_netif_sntp (ESP-IDF)
#include "time_sync.h"
#include <cstdlib>
#include <ctime>
#include <sys/time.h>
#include "config.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "util.h"

static const char* TAG = "ntp";

static const uint32_t SYNC_INTERVAL_MS = 60 * 60 * 1000;  // stündlicher Abgleich
static const time_t EARLIEST_VALID_TIME = 1700000000;     // Nov. 2023: davor ist die Uhr nicht gestellt

static EventGroupHandle_t timeEvents = nullptr;
static const int TIME_SYNCED_BIT = BIT0;
static time_t lastSyncTime = 0;
static uint32_t syncCount = 0;

// Wird von SNTP nach jedem erfolgreichen Abgleich aufgerufen.
static void onTimeSynchronized(struct timeval* syncedTime) {
  lastSyncTime = syncedTime->tv_sec;
  syncCount++;
  xEventGroupSetBits(timeEvents, TIME_SYNCED_BIT);
  char text[32];
  struct tm localTime;
  localtime_r(&lastSyncTime, &localTime);
  strftime(text, sizeof(text), "%d.%m.%Y %H:%M:%S", &localTime);
  if (syncCount == 1) ESP_LOGI(TAG, "Uhrzeit per NTP gestellt: %s", text);
  else ESP_LOGD(TAG, "NTP-Abgleich: %s", text);
}

void timeSyncStart() {
  timeEvents = xEventGroupCreate();

  // Zeitzone als POSIX-TZ, z. B. "CET-1CEST,M3.5.0,M10.5.0/3" (Mitteleuropa mit Sommerzeit)
  setenv("TZ", g_config.timeZone, 1);
  tzset();

  // Zwei Server: fällt einer aus, wird der andere gefragt. Leerer zweiter Eintrag = nur einer.
  esp_sntp_config_t sntpConfig = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
      2, ESP_SNTP_SERVER_LIST(g_config.ntpServer1, g_config.ntpServer2[0] ? g_config.ntpServer2 : g_config.ntpServer1));
  sntpConfig.sync_cb = onTimeSynchronized;
  sntpConfig.start = true;  // startet automatisch, sobald eine IP-Adresse vorliegt
  esp_netif_sntp_init(&sntpConfig);
  esp_sntp_set_sync_interval(SYNC_INTERVAL_MS);
  ESP_LOGI(TAG, "NTP-Server: %s %s, Zeitzone: %s", g_config.ntpServer1, g_config.ntpServer2, g_config.timeZone);
}

bool timeIsValid() { return time(nullptr) > EARLIEST_VALID_TIME; }

bool timeWaitForSync(uint32_t timeoutMs) {
  if (timeIsValid()) return true;
  if (!timeEvents) return false;
  xEventGroupWaitBits(timeEvents, TIME_SYNCED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeoutMs));
  return timeIsValid();
}

void timeStatusToJson(cJSON* statusObject) {
  time_t now = time(nullptr);
  bool valid = timeIsValid();
  cJSON_AddBoolToObject(statusObject, "valid", valid);
  char text[32] = "";
  if (valid) {
    struct tm localTime;
    localtime_r(&now, &localTime);
    strftime(text, sizeof(text), "%d.%m.%Y %H:%M:%S %Z", &localTime);
  }
  cJSON_AddStringToObject(statusObject, "local", text);
  cJSON_AddNumberToObject(statusObject, "epoch", valid ? (double)now : 0);
  cJSON_AddNumberToObject(statusObject, "lastSyncAgo", lastSyncTime ? (double)(now - lastSyncTime) : -1);
  cJSON_AddNumberToObject(statusObject, "syncCount", syncCount);
  cJSON_AddStringToObject(statusObject, "server1", g_config.ntpServer1);
  cJSON_AddStringToObject(statusObject, "server2", g_config.ntpServer2);
  cJSON_AddStringToObject(statusObject, "timeZone", g_config.timeZone);
}

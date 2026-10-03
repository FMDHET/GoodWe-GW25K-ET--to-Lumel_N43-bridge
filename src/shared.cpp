#include "shared.h"

MeterData g_meterData;
GoodweRegisters g_goodweRegisters;
GoodweInfo g_goodweInfo;
PortStats g_portStats[PORT_STATS_COUNT];

static SemaphoreHandle_t sharedDataMutex = nullptr;

void sharedInit() { sharedDataMutex = xSemaphoreCreateMutex(); }
void sharedLock() { xSemaphoreTake(sharedDataMutex, portMAX_DELAY); }
void sharedUnlock() { xSemaphoreGive(sharedDataMutex); }

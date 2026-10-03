#pragma once
// Kleine Hilfsfunktionen, die in mehreren Modulen gebraucht werden
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "cJSON.h"
#include "esp_timer.h"

// Zeit seit dem Start
inline uint32_t millisSinceBoot() { return (uint32_t)(esp_timer_get_time() / 1000); }
inline uint32_t microsSinceBoot() { return (uint32_t)esp_timer_get_time(); }

// Kopiert einen Text und kürzt ihn bei Bedarf; das Ziel ist immer nullterminiert
inline void copyString(char* destination, const char* source, size_t destinationSize) {
  if (!destinationSize) return;
  size_t copied = 0;
  while (source && source[copied] && copied + 1 < destinationSize) {
    destination[copied] = source[copied];
    copied++;
  }
  destination[copied] = 0;
}

// cJSON-Zahl mit fester Anzahl Nachkommastellen (ohne Float-Artefakte wie 229.99999)
inline cJSON* jsonNumberWithDecimals(double value, int decimals) {
  char text[32];
  snprintf(text, sizeof(text), "%.*f", decimals, value);
  return cJSON_CreateRaw(text);
}

// cJSON-Baum als Text
inline std::string jsonToString(const cJSON* json, bool pretty = false) {
  char* text = pretty ? cJSON_Print(json) : cJSON_PrintUnformatted(json);
  std::string result = text ? text : "";
  cJSON_free(text);
  return result;
}

// Gibt einen cJSON-Baum beim Verlassen des Gültigkeitsbereichs automatisch frei
class JsonDocument {
 public:
  explicit JsonDocument(cJSON* root) : root(root) {}
  ~JsonDocument() { cJSON_Delete(root); }
  JsonDocument(const JsonDocument&) = delete;
  JsonDocument& operator=(const JsonDocument&) = delete;
  cJSON* get() const { return root; }

 private:
  cJSON* root;
};

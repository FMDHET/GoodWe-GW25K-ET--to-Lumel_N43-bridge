// Ringpuffer für die Live-Konsole der Weboberfläche.
// Alle ESP_LOGx-Ausgaben werden zusätzlich zur USB-Konsole hier abgelegt.
#include "chip.h"
#include "log_buffer.h"
#include <cstdarg>
#include <cstdio>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const size_t LOG_BUFFER_SIZE = CHIP_LOG_BUFFER_SIZE;  // je nach Chip, siehe chip.h
static const size_t MAX_LOG_LINE_LENGTH = 384;

static char ringBuffer[LOG_BUFFER_SIZE];
static uint32_t totalBytesWritten = 0;  // fortlaufende Schreibposition
static portMUX_TYPE ringBufferLock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t originalLogOutput = nullptr;

std::atomic<bool> g_logModbusTraffic{false};

// Kopiert `length` Bytes in den Ringpuffer. Der kritische Abschnitt umfasst nur das Kopieren,
// damit Logausgaben aus beliebigen Tasks sich nicht gegenseitig zerstückeln.
void logBufferAppend(const char* text, size_t length) {
  if (length > LOG_BUFFER_SIZE) {
    text += length - LOG_BUFFER_SIZE;
    length = LOG_BUFFER_SIZE;
  }
  taskENTER_CRITICAL(&ringBufferLock);
  for (size_t byteIndex = 0; byteIndex < length; byteIndex++)
    ringBuffer[(totalBytesWritten + byteIndex) % LOG_BUFFER_SIZE] = text[byteIndex];
  totalBytesWritten += length;
  taskEXIT_CRITICAL(&ringBufferLock);
}

// Ersatz für die IDF-Logausgabe: formatiert die Meldung einmal, schreibt sie in den Ringpuffer
// und gibt sie anschließend wie gewohnt auf der USB-Konsole aus.
static int logOutputToBufferAndConsole(const char* format, va_list arguments) {
  char line[MAX_LOG_LINE_LENGTH];
  va_list argumentsCopy;
  va_copy(argumentsCopy, arguments);
  int formattedLength = vsnprintf(line, sizeof(line), format, argumentsCopy);
  va_end(argumentsCopy);
  if (formattedLength > 0) {
    size_t storedLength = (size_t)formattedLength < sizeof(line) ? formattedLength : sizeof(line) - 1;
    logBufferAppend(line, storedLength);
  }
  return originalLogOutput ? originalLogOutput(format, arguments) : formattedLength;
}

void logBufferInit() { originalLogOutput = esp_log_set_vprintf(logOutputToBufferAndConsole); }

uint32_t logBufferWritePosition() {
  taskENTER_CRITICAL(&ringBufferLock);
  uint32_t position = totalBytesWritten;
  taskEXIT_CRITICAL(&ringBufferLock);
  return position;
}

uint32_t logBufferReadFrom(uint32_t fromPosition, std::string& text) {
  text.clear();
  uint32_t endPosition = logBufferWritePosition();
  uint32_t oldestAvailable = endPosition > LOG_BUFFER_SIZE ? endPosition - LOG_BUFFER_SIZE : 0;
  if (fromPosition < oldestAvailable || fromPosition > endPosition) fromPosition = oldestAvailable;
  text.reserve(endPosition - fromPosition);
  // ohne Sperre lesen: der Bereich [fromPosition, endPosition) wird erst nach weiteren
  // LOG_BUFFER_SIZE Bytes überschrieben; schlimmstenfalls ist der älteste Teil unscharf
  for (uint32_t position = fromPosition; position < endPosition; position++)
    text.push_back(ringBuffer[position % LOG_BUFFER_SIZE]);
  return endPosition;
}

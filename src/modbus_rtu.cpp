// Modbus-RTU-Schicht: UART-Initialisierung, Frame-Erkennung über t1,5/t3,5 im Empfangs-Task,
// CRC-Prüfung, Senden von Frames sowie die Master-Funktion "Register lesen" (FC03/FC04).
#include "modbus_rtu.h"
#include <cstdio>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "log_buffer.h"
#include "util.h"

static const char* TAG = "rtu";

// Hardware-Einstellungen des UART-Treibers
static const int UART_RX_BUFFER_SIZE = 1024;
static const int UART_EVENT_QUEUE_LENGTH = 32;
static const int UART_RX_FULL_THRESHOLD = 120;
static const UBaseType_t RECEIVED_FRAME_QUEUE_LENGTH = 2;
static const uint32_t RECEIVE_TASK_STACK_SIZE = 4096;
static const UBaseType_t RECEIVE_TASK_PRIORITY = 12;

// Bus-Ruhe vor dem Senden: maximale Wartezeit, danach wird trotzdem gesendet
static const uint32_t BUS_IDLE_MAX_WAIT_MS = 200;
static const uint32_t TX_DONE_TIMEOUT_MS = 500;

// Funktionscode einer Exception-Antwort = Funktionscode der Anfrage mit gesetztem Bit 7
static const uint8_t EXCEPTION_FLAG = 0x80;

// Berechnet die Modbus-CRC16 (Polynom 0xA001, Startwert 0xFFFF) über data[0..length).
// Rückgabe: CRC, beim Senden mit dem Low-Byte zuerst anzuhängen.
uint16_t modbusCrc16(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t byteIndex = 0; byteIndex < length; byteIndex++) {
    crc ^= data[byteIndex];
    for (int bitIndex = 0; bitIndex < 8; bitIndex++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

// Liefert einen lesbaren Text zu einem ModbusResult bzw. Exception-Code (für Web/Log).
const char* modbusResultText(int result) {
  switch (result) {
    case MODBUS_OK: return "OK";
    case MODBUS_ERROR_TIMEOUT: return "Timeout (keine Antwort)";
    case MODBUS_ERROR_CRC: return "CRC-Fehler";
    case MODBUS_ERROR_FRAME: return "Ungültiger Frame";
    case MODBUS_ERROR_WRONG_ADDRESS: return "Antwort von falscher Adresse";
    case MODBUS_ERROR_CONNECTION: return "Keine TCP-Verbindung zum Gerät";
    case EXCEPTION_ILLEGAL_FUNCTION: return "Exception 01: Illegal Function";
    case EXCEPTION_ILLEGAL_DATA_ADDRESS: return "Exception 02: Illegal Data Address";
    case EXCEPTION_ILLEGAL_DATA_VALUE: return "Exception 03: Illegal Data Value";
    case EXCEPTION_SLAVE_DEVICE_FAILURE: return "Exception 04: Slave Device Failure";
    default: return "Exception";
  }
}

// Prüft die CRC am Frame-Ende (Low-Byte zuerst). Kürzer als 4 Byte ist nie ein gültiger Frame.
static bool hasValidCrc(const uint8_t* frame, size_t frameLength) {
  if (frameLength < 4) return false;
  uint16_t receivedCrc = (uint16_t)(frame[frameLength - 2] | (frame[frameLength - 1] << 8));
  return modbusCrc16(frame, frameLength - 2) == receivedCrc;
}

// Übersetzt die Parität aus der Konfiguration ('N', 'E', 'O') in den Wert des UART-Treibers.
static uart_parity_t toUartParity(char parity) {
  if (parity == 'E') return UART_PARITY_EVEN;
  if (parity == 'O') return UART_PARITY_ODD;
  return UART_PARITY_DISABLE;
}

// Richtet den UART für Modbus RTU ein, berechnet die Zeichen- und Pausenzeiten und startet den
// Empfangs-Task. uartPort = UART-Nummer, portConfig = Baudrate/Parität/Pins des Ports.
// Rückgabe: false, wenn der UART-Treiber nicht initialisiert werden konnte.
bool RtuPort::begin(uart_port_t uartPort, const PortConfig& portConfig) {
  uartNumber = uartPort;

  // --- Zeitwerte nach Spec 2.5.1.1 ---
  // Zeichenzeit: 1 Startbit + 8 Datenbits + Parität + Stoppbits, aufgerundet auf ganze µs
  uint32_t bitsPerCharacter = 1 + 8 + (portConfig.parity == 'N' ? 0 : 1) + portConfig.stopBits;
  characterTimeUs = (1000000UL * bitsPerCharacter + portConfig.baudRate - 1) / portConfig.baudRate;
  if (portConfig.baudRate > 19200) {
    // Spec 2.5.1.1: oberhalb 19200 Baud feste Werte
    t15Us = 750;
    t35Us = 1750;
  } else {
    t15Us = characterTimeUs * 15 / 10;
    t35Us = characterTimeUs * 35 / 10;
  }

  // --- Hardware-RX-Timeout in Zeichenzeiten, aufgerundet auf >= t1,5 ---
  // (Der UART kann nur ganze Zeichenzeiten; das Idle-Event kommt deshalb ggf. etwas nach t1,5.)
  uint32_t rxTimeoutCharacters = (t15Us + characterTimeUs - 1) / characterTimeUs;
  if (rxTimeoutCharacters < 2) rxTimeoutCharacters = 2;
  hardwareIdleTimeoutUs = rxTimeoutCharacters * characterTimeUs;

  // --- UART-Treiber konfigurieren ---
  uart_config_t uartConfig = {};
  uartConfig.baud_rate = (int)portConfig.baudRate;
  uartConfig.data_bits = UART_DATA_8_BITS;
  uartConfig.parity = toUartParity(portConfig.parity);
  uartConfig.stop_bits = portConfig.stopBits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
  uartConfig.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uartConfig.source_clk = UART_SCLK_DEFAULT;

  // Ohne DE-Pin: Modul mit automatischer Richtungsumschaltung, normaler UART-Modus
  bool hasDriverEnablePin = portConfig.driverEnablePin >= 0;
  int rtsPin = hasDriverEnablePin ? portConfig.driverEnablePin : UART_PIN_NO_CHANGE;
  uart_mode_t uartMode = hasDriverEnablePin ? UART_MODE_RS485_HALF_DUPLEX : UART_MODE_UART;

  esp_err_t error;
  if ((error = uart_driver_install(uartNumber, UART_RX_BUFFER_SIZE, 0, UART_EVENT_QUEUE_LENGTH,
                                   &uartEventQueue, 0)) != ESP_OK ||
      (error = uart_param_config(uartNumber, &uartConfig)) != ESP_OK ||
      (error = uart_set_pin(uartNumber, portConfig.txPin, portConfig.rxPin, rtsPin, UART_PIN_NO_CHANGE)) !=
          ESP_OK ||
      (error = uart_set_mode(uartNumber, uartMode)) != ESP_OK ||
      (error = uart_set_rx_timeout(uartNumber, rxTimeoutCharacters)) != ESP_OK ||
      (error = uart_set_rx_full_threshold(uartNumber, UART_RX_FULL_THRESHOLD)) != ESP_OK) {
    ESP_LOGE(TAG, "UART%d: Initialisierung fehlgeschlagen: %s", uartNumber, esp_err_to_name(error));
    return false;
  }
  // --- Frame-Queue und Empfangs-Task ---
  uart_flush_input(uartNumber);
  receivedFrameQueue = xQueueCreate(RECEIVED_FRAME_QUEUE_LENGTH, sizeof(ReceivedFrame));

  char taskName[16];
  snprintf(taskName, sizeof(taskName), "rtu%d_rx", uartNumber);
  xTaskCreate(receiveTaskEntry, taskName, RECEIVE_TASK_STACK_SIZE, this, RECEIVE_TASK_PRIORITY, nullptr);
  return true;
}

// FreeRTOS-Einstieg des Empfangs-Tasks; port = das zugehörige RtuPort-Objekt.
void RtuPort::receiveTaskEntry(void* port) { static_cast<RtuPort*>(port)->receiveTask(); }

// Wird nach t3,5 Busruhe aufgerufen: prüft den gesammelten Frame (Fehlermarkierung, CRC) und
// stellt ihn in die Frame-Queue oder verwirft ihn (mit Hex-Dump und Grund für die Diagnose).
// Danach ist der Empfangszustand für den nächsten Frame zurückgesetzt.
void RtuPort::completeFrame() {
  lastBusActivityUs = microsSinceBoot();
  if (frameLength == 0) {
    frameIsCorrupt = false;
    idleDetected = false;
    return;
  }

  // --- Gültigkeit prüfen ---
  int discardReason = 0;
  if (frameIsCorrupt) {
    discardReason = corruptReason;
  } else if (!hasValidCrc(frameBuffer, frameLength)) {
    counters.crcErrors++;
    discardReason = MODBUS_ERROR_CRC;
  }

  // --- Verwerfen oder weitergeben ---
  logFrame(discardReason ? "verworfen" : "RX", frameBuffer, frameLength,
           discardReason ? modbusResultText(discardReason) : nullptr);
  if (discardReason) {
    // Rohdaten für die Diagnose festhalten
    size_t hexLength = 0;
    for (size_t byteIndex = 0; byteIndex < frameLength && hexLength + 4 < sizeof(lastDiscardedFrameHex);
         byteIndex++)
      hexLength += snprintf(lastDiscardedFrameHex + hexLength, sizeof(lastDiscardedFrameHex) - hexLength,
                            "%02X ", frameBuffer[byteIndex]);
    if (frameLength >= 4) {  // einzelne Störbytes (z.B. beim Umschalten von DE) zählen nicht als Antwort
      lastDiscardReason = discardReason;
      discardedFrameCount++;
    }
  } else {
    counters.validFrames++;
    ReceivedFrame receivedFrame;
    receivedFrame.length = (uint16_t)frameLength;
    memcpy(receivedFrame.data, frameBuffer, frameLength);
    if (xQueueSend(receivedFrameQueue, &receivedFrame, 0) != pdTRUE) counters.overruns++;
  }

  frameLength = 0;
  frameIsCorrupt = false;
  idleDetected = false;
}

// Empfangs-Task: wertet die UART-Events aus, sammelt Zeichen in frameBuffer und erkennt
// Frame-Enden (t3,5 Ruhe) bzw. Fehler (Pause zwischen t1,5 und t3,5, Parität, Framing, Überlauf).
// Läuft endlos.
void RtuPort::receiveTask() {
  uart_event_t uartEvent;
  for (;;) {
    // Nach einem Idle-Event (>= t1,5) nur noch bis t3,5 auf weitere Events warten
    TickType_t waitTicks = portMAX_DELAY;
    if (frameLength > 0 && idleDetected) {
      // Restzeit bis t3,5 seit der letzten empfangenen Zeichenzeit
      uint32_t silenceUs = microsSinceBoot() - idleDetectedAtUs + hardwareIdleTimeoutUs;
      uint32_t remainingUs = silenceUs >= t35Us ? 0 : t35Us - silenceUs;
      waitTicks = remainingUs ? pdMS_TO_TICKS((remainingUs + 999) / 1000) : 0;
      if (waitTicks == 0 && remainingUs) waitTicks = 1;
    }
    if (xQueueReceive(uartEventQueue, &uartEvent, waitTicks) != pdTRUE) {
      completeFrame();  // t3,5 Ruhe erreicht
      continue;
    }

    switch (uartEvent.type) {
      case UART_DATA: {
        if (frameLength > 0 && idleDetected) {
          // neue Zeichen nach Pause zwischen t1,5 und t3,5 -> Frame unvollständig
          if (!frameIsCorrupt) counters.gapErrors++;
          frameIsCorrupt = true;
          corruptReason = MODBUS_ERROR_FRAME;
          idleDetected = false;
        }

        size_t freeSpace = sizeof(frameBuffer) - frameLength;
        size_t bytesToRead = uartEvent.size < freeSpace ? uartEvent.size : freeSpace;
        int bytesRead = uart_read_bytes(uartNumber, frameBuffer + frameLength, bytesToRead, 0);
        if (bytesRead > 0) frameLength += bytesRead;

        if (uartEvent.size > freeSpace) {
          // Puffer voll: Rest aus dem Treiber lesen und wegwerfen, Frame ist unbrauchbar
          uint8_t discardBuffer[64];
          size_t bytesLeft = uartEvent.size - freeSpace;
          while (bytesLeft) {
            size_t chunkSize = bytesLeft < sizeof(discardBuffer) ? bytesLeft : sizeof(discardBuffer);
            int bytesDiscarded = uart_read_bytes(uartNumber, discardBuffer, chunkSize, 0);
            if (bytesDiscarded <= 0) break;
            bytesLeft -= bytesDiscarded;
          }
          if (!frameIsCorrupt) counters.overruns++;
          frameIsCorrupt = true;
          corruptReason = MODBUS_ERROR_FRAME;
        }

        if (frameLength > MODBUS_MAX_FRAME_SIZE && !frameIsCorrupt) {
          counters.overruns++;
          frameIsCorrupt = true;
          corruptReason = MODBUS_ERROR_FRAME;
        }

        // timeout_flag = der UART hat eine Ruhepause >= t1,5 erkannt
        if (uartEvent.timeout_flag) {
          idleDetected = true;
          idleDetectedAtUs = microsSinceBoot();
        }
        lastBusActivityUs = microsSinceBoot();
        break;
      }

      case UART_PARITY_ERR:
      case UART_FRAME_ERR:
      case UART_BREAK:
        if (!frameIsCorrupt) counters.characterErrors++;
        frameIsCorrupt = true;
        corruptReason = MODBUS_ERROR_FRAME;
        break;

      case UART_FIFO_OVF:
      case UART_BUFFER_FULL:
        // Daten verloren: alles verwerfen und neu synchronisieren
        uart_flush_input(uartNumber);
        xQueueReset(uartEventQueue);
        counters.overruns++;
        frameLength = 0;
        frameIsCorrupt = false;
        idleDetected = false;
        break;

      default:
        break;
    }
  }
}

// Wartet, bis der Bus mindestens t3,5 ruhig ist und kein Frame im Empfang steckt,
// höchstens BUS_IDLE_MAX_WAIT_MS lang (danach wird trotzdem gesendet).
void RtuPort::waitForBusIdle() {
  // mindestens t3,5 Ruhe seit der letzten Bus-Aktivität
  uint32_t waitStartMs = millisSinceBoot();
  while (millisSinceBoot() - waitStartMs < BUS_IDLE_MAX_WAIT_MS) {
    uint32_t silenceUs = microsSinceBoot() - lastBusActivityUs.load();
    if (silenceUs >= t35Us && frameLength == 0) return;
    vTaskDelay(1);
  }
}

// Hängt die CRC an frame[0..payloadLength) an (frame braucht 2 Byte Reserve), sendet den Frame
// und wartet, bis er komplett auf dem Bus ist. Merkt den Zeitpunkt als letzte Bus-Aktivität.
void RtuPort::sendFrame(uint8_t* frame, size_t payloadLength) {
  uint16_t crc = modbusCrc16(frame, payloadLength);
  frame[payloadLength] = crc & 0xFF;  // CRC: Low-Byte zuerst
  frame[payloadLength + 1] = crc >> 8;
  logFrame("TX", frame, payloadLength + 2);
  uart_write_bytes(uartNumber, frame, payloadLength + 2);
  uart_wait_tx_done(uartNumber, pdMS_TO_TICKS(TX_DONE_TIMEOUT_MS));  // bis das letzte Stoppbit gesendet ist
  lastBusActivityUs = microsSinceBoot();
}

// Slave: wartet bis zu timeout auf den nächsten fehlerfreien Frame und kopiert ihn ohne CRC
// nach frame; payloadLength = dessen Länge. Rückgabe: false bei Zeitüberschreitung.
bool RtuPort::waitForFrame(uint8_t* frame, size_t& payloadLength, TickType_t timeout) {
  ReceivedFrame receivedFrame;
  if (xQueueReceive(receivedFrameQueue, &receivedFrame, timeout) != pdTRUE) return false;
  payloadLength = receivedFrame.length - 2;  // ohne CRC
  memcpy(frame, receivedFrame.data, payloadLength);
  return true;
}

// Master: liest registerCount Register ab startRegister per FC03/FC04 vom Slave slaveAddress
// nach values. Rückgabe: MODBUS_OK, negativer Übertragungsfehler oder positiver Exception-Code.
// Wird statt einer gültigen Antwort nur ein fehlerhafter Frame empfangen, kommt dessen Grund zurück.
int RtuPort::readRegisters(uint8_t slaveAddress, uint8_t functionCode, uint16_t startRegister,
                           uint16_t registerCount, uint16_t* values, uint32_t timeoutMs) {
  if (slaveAddress < 1 || slaveAddress > 247 || registerCount < 1 || registerCount > 125)
    return MODBUS_ERROR_FRAME;

  // --- Anfrage senden ---
  waitForBusIdle();
  xQueueReset(receivedFrameQueue);
  // Merken, ob während der Wartezeit ein fehlerhafter Frame verworfen wird
  uint32_t discardedCountBefore = discardedFrameCount.load();
  uint8_t request[8] = {slaveAddress,
                        functionCode,
                        (uint8_t)(startRegister >> 8),
                        (uint8_t)startRegister,
                        (uint8_t)(registerCount >> 8),
                        (uint8_t)registerCount};
  sendFrame(request, 6);

  // --- Antwort abwarten ---
  ReceivedFrame response;
  if (xQueueReceive(receivedFrameQueue, &response, pdMS_TO_TICKS(timeoutMs)) != pdTRUE)
    return discardedFrameCount.load() != discardedCountBefore ? lastDiscardReason.load() : MODBUS_ERROR_TIMEOUT;

  // --- Antwort prüfen und auswerten ---
  // Antwort: Adresse, FC, Byteanzahl, Daten..., CRC (2)
  const uint8_t* responseData = response.data;
  size_t responseLength = response.length;
  if (responseData[0] != slaveAddress) return MODBUS_ERROR_WRONG_ADDRESS;
  if (responseData[1] == (functionCode | EXCEPTION_FLAG))
    return responseLength == 5 ? responseData[2] : MODBUS_ERROR_FRAME;
  if (responseData[1] != functionCode || responseData[2] != 2 * registerCount ||
      responseLength != (size_t)(5 + 2 * registerCount))
    return MODBUS_ERROR_FRAME;
  for (uint16_t registerIndex = 0; registerIndex < registerCount; registerIndex++)
    values[registerIndex] = (responseData[3 + 2 * registerIndex] << 8) | responseData[4 + 2 * registerIndex];
  return MODBUS_OK;
}

void RtuPort::setLogName(const char* name) { copyString(logName, name, sizeof(logName)); }

void RtuPort::logFrame(const char* direction, const uint8_t* data, size_t length, const char* note) {
  if (!g_logModbusTraffic) return;
  // Hex-Darstellung, bei sehr langen Frames gekürzt (die Logzeile hat eine Maximallänge)
  static const size_t MAX_BYTES_SHOWN = 64;
  char hexText[MAX_BYTES_SHOWN * 3 + 1];
  size_t textLength = 0;
  size_t bytesShown = length < MAX_BYTES_SHOWN ? length : MAX_BYTES_SHOWN;
  for (size_t byteIndex = 0; byteIndex < bytesShown; byteIndex++)
    textLength += snprintf(hexText + textLength, sizeof(hexText) - textLength, "%02X ", data[byteIndex]);
  ESP_LOGI(logName, "%-9s %3u Byte: %s%s%s%s", direction, (unsigned)length, hexText,
           length > bytesShown ? "... " : "", note ? "-> " : "", note ? note : "");
}

int RtuPort::transact(uint8_t unitId, const uint8_t* requestPdu, size_t requestLength, uint8_t* responsePdu,
                      size_t& responseLength, uint32_t timeoutMs) {
  if (requestLength < 1 || requestLength > MODBUS_MAX_FRAME_SIZE - 3) return MODBUS_ERROR_FRAME;
  waitForBusIdle();
  xQueueReset(receivedFrameQueue);
  uint32_t discardedBefore = discardedFrameCount.load();

  uint8_t request[MODBUS_MAX_FRAME_SIZE + 2];
  request[0] = unitId;
  memcpy(request + 1, requestPdu, requestLength);
  sendFrame(request, requestLength + 1);
  if (unitId == 0) {  // Broadcast: keine Antwort zu erwarten
    responseLength = 0;
    return MODBUS_OK;
  }

  ReceivedFrame receivedFrame;
  if (xQueueReceive(receivedFrameQueue, &receivedFrame, pdMS_TO_TICKS(timeoutMs)) != pdTRUE)
    return discardedFrameCount.load() != discardedBefore ? lastDiscardReason.load() : MODBUS_ERROR_TIMEOUT;
  if (receivedFrame.length < 4 || receivedFrame.data[0] != unitId) return MODBUS_ERROR_WRONG_ADDRESS;
  responseLength = receivedFrame.length - 3;  // ohne Adresse und CRC
  memcpy(responsePdu, receivedFrame.data + 1, responseLength);
  return MODBUS_OK;
}

// Parameter für den Identify-Task
struct IdentifyJob {
  RtuPort* port;
  uart_port_t uartNumber;
  bool viaUart;        // true: Port läuft, Null-Bytes über den UART senden
  int txPin;
  int driverEnablePin; // -1 = Modul mit automatischer Richtungsumschaltung
  uint32_t baudRate;
  uint32_t durationMs;
};

// Identify-Task: 5 Hz blinken (100 ms "senden", 100 ms Pause) bis durationMs abgelaufen ist.
static void identifyTask(void* parameter) {
  IdentifyJob job = *static_cast<IdentifyJob*>(parameter);
  delete static_cast<IdentifyJob*>(parameter);
  static const uint32_t HALF_PERIOD_MS = 100;
  // Null-Bytes für 100 ms: 0x00 hält die Leitung fast durchgehend auf Low -> TXD-LED leuchtet
  size_t bytesPerBurst = job.baudRate / 10 * HALF_PERIOD_MS / 1000;
  if (bytesPerBurst > 256) bytesPerBurst = 256;
  static const uint8_t ZERO_BYTES[256] = {};
  if (!job.viaUart) {
    gpio_reset_pin((gpio_num_t)job.txPin);
    gpio_set_direction((gpio_num_t)job.txPin, GPIO_MODE_OUTPUT);
    if (job.driverEnablePin >= 0) {
      gpio_reset_pin((gpio_num_t)job.driverEnablePin);
      gpio_set_direction((gpio_num_t)job.driverEnablePin, GPIO_MODE_OUTPUT);
    }
  }
  uint32_t startMs = millisSinceBoot();
  while (millisSinceBoot() - startMs < job.durationMs) {
    if (job.viaUart) {
      uart_write_bytes(job.uartNumber, ZERO_BYTES, bytesPerBurst);
      uart_wait_tx_done(job.uartNumber, pdMS_TO_TICKS(HALF_PERIOD_MS * 2));
    } else {
      if (job.driverEnablePin >= 0) gpio_set_level((gpio_num_t)job.driverEnablePin, 1);
      gpio_set_level((gpio_num_t)job.txPin, 0);  // Low = "senden" -> LED an
      vTaskDelay(pdMS_TO_TICKS(HALF_PERIOD_MS));
      gpio_set_level((gpio_num_t)job.txPin, 1);
      if (job.driverEnablePin >= 0) gpio_set_level((gpio_num_t)job.driverEnablePin, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(HALF_PERIOD_MS));
  }
  if (!job.viaUart) {
    gpio_reset_pin((gpio_num_t)job.txPin);
    if (job.driverEnablePin >= 0) gpio_reset_pin((gpio_num_t)job.driverEnablePin);
  }
  job.port->identifyActive = false;
  vTaskDelete(nullptr);
}

bool RtuPort::identify(const PortConfig& portConfig, uint32_t durationMs) {
  if (identifyActive.exchange(true)) return false;
  IdentifyJob* job = new IdentifyJob{this,          uartNumber,           isRunning(), portConfig.txPin,
                                     portConfig.driverEnablePin, portConfig.baudRate, durationMs};
  ESP_LOGI(TAG, "%s: Identify für %lu s (TX=GPIO%d)", logName, (unsigned long)(durationMs / 1000), portConfig.txPin);
  if (xTaskCreate(identifyTask, "identify", 2560, job, 2, nullptr) != pdPASS) {
    delete job;
    identifyActive = false;
    return false;
  }
  return true;
}

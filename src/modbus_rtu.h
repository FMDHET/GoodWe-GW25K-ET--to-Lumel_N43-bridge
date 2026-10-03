#pragma once
// Modbus RTU nach "MODBUS over Serial Line Specification V1.02" und
// "MODBUS Application Protocol Specification V1.1b3" auf dem ESP-IDF-UART-Treiber.
//
// Frame-Erkennung (Spec 2.5.1.1) im Empfangs-Task je Port:
//  - Der UART meldet per RX-Timeout (uart_set_rx_timeout, in Zeichenzeiten) jede Ruhepause >= t1,5
//    als UART_DATA-Event mit timeout_flag.
//  - Bleibt die Leitung danach bis insgesamt t3,5 ruhig, ist der Frame komplett.
//  - Kommen vorher weitere Zeichen (Pause zwischen t1,5 und t3,5), ist der Frame unvollständig
//    und wird verworfen.
//  - UART_PARITY_ERR / UART_FRAME_ERR / Überlauf / CRC-Fehler -> Frame wird verworfen.
//  - Baudrate > 19200: feste Werte t1,5 = 750 us, t3,5 = 1750 us.
//  - Der Master sendet erst nach mindestens t3,5 Busruhe.
// Richtungsumschaltung: UART_MODE_RS485_HALF_DUPLEX, RTS-Pin -> DE/!RE des Transceivers.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "config.h"
#include "modbus_master.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Ergebnis einer Master-Anfrage: 0 = OK, < 0 = Übertragungsfehler, > 0 = Modbus-Exception-Code
enum ModbusResult : int {
  MODBUS_OK = 0,
  MODBUS_ERROR_TIMEOUT = -1,
  MODBUS_ERROR_CRC = -2,
  MODBUS_ERROR_FRAME = -3,
  MODBUS_ERROR_WRONG_ADDRESS = -4,
  MODBUS_ERROR_CONNECTION = -5,  // Modbus TCP: keine Verbindung zum Gerät
};

// Exception-Codes (Application Protocol Kap. 7)
enum ModbusException : uint8_t {
  EXCEPTION_ILLEGAL_FUNCTION = 0x01,
  EXCEPTION_ILLEGAL_DATA_ADDRESS = 0x02,
  EXCEPTION_ILLEGAL_DATA_VALUE = 0x03,
  EXCEPTION_SLAVE_DEVICE_FAILURE = 0x04,
};

static const size_t MODBUS_MAX_FRAME_SIZE = 256;  // Adresse + PDU (253) + CRC

// Berechnet die Modbus-CRC16 (Polynom 0xA001, Startwert 0xFFFF) über length Bytes.
uint16_t modbusCrc16(const uint8_t* data, size_t length);

// Klartext zu einem ModbusResult bzw. Exception-Code, z.B. für die Fehleranzeige.
const char* modbusResultText(int result);

struct RtuCounters {
  std::atomic<uint32_t> validFrames{0};
  std::atomic<uint32_t> crcErrors{0};
  std::atomic<uint32_t> characterErrors{0};  // Parität / Framing
  std::atomic<uint32_t> gapErrors{0};        // Pause > t1,5 innerhalb eines Frames
  std::atomic<uint32_t> overruns{0};         // FIFO/Puffer übergelaufen oder Frame > 256 Byte
};

// Ein RS485-Port mit Modbus-RTU-Rahmenerkennung. Kann als Master (readRegisters)
// oder als Slave (waitForFrame + sendFrame) verwendet werden.
class RtuPort : public ModbusMaster {
 public:
  // Richtet den UART-Treiber ein (Baudrate, Parität, Pins, RS485-Modus) und startet den Empfangs-Task.
  bool begin(uart_port_t uartNumber, const PortConfig& portConfig);

  // Master: Register lesen (FC03/FC04). Rückgabe siehe ModbusResult.
  int readRegisters(uint8_t slaveAddress, uint8_t functionCode, uint16_t startRegister,
                    uint16_t registerCount, uint16_t* values, uint32_t timeoutMs) override;

  // Reicht eine beliebige PDU an das Gerät durch (Modbus-TCP-Bridge auf RS485).
  int transact(uint8_t unitId, const uint8_t* requestPdu, size_t requestLength, uint8_t* responsePdu,
               size_t& responseLength, uint32_t timeoutMs) override;

  // Slave: wartet auf einen fehlerfreien Frame (frameLength ohne CRC)
  bool waitForFrame(uint8_t* frame, size_t& frameLength, TickType_t timeout);

  // Sendet frame[0..frameLength) und hängt die CRC an (frame braucht 2 Byte Reserve)
  void sendFrame(uint8_t* frame, size_t frameLength);

  // Name für die Protokollierung des Busverkehrs, z. B. "RTU1" (max. 7 Zeichen)
  void setLogName(const char* name);

  // true, wenn begin() erfolgreich war (Port hat eine Rolle und der UART läuft)
  bool isRunning() const { return receivedFrameQueue != nullptr; }

  // "Identify": lässt die TXD-LED des RS485-Moduls durationMs lang mit 5 Hz blinken, damit man
  // erkennt, welches Modul zu welchem Port gehört. Läuft der Port, werden abwechselnd 100 ms lang
  // Null-Bytes gesendet (Busverkehr ist währenddessen gestört); sonst wird der TX-Pin direkt
  // umgeschaltet. Rückgabe: false, wenn für diesen Port schon ein Identify läuft.
  bool identify(const PortConfig& portConfig, uint32_t durationMs);

  // Zeitgrenzen nach Spec in Mikrosekunden (abhängig von der Baudrate)
  uint32_t charTimeout15Us() const { return t15Us; }   // t1,5: max. Pause zwischen Zeichen
  uint32_t frameTimeout35Us() const { return t35Us; }  // t3,5: Pause, die einen Frame beendet

  RtuCounters counters;
  std::atomic<bool> identifyActive{false};  // Identify läuft gerade
  char lastDiscardedFrameHex[100] = "";  // letzter verworfener Frame (Diagnose)

 private:
  struct ReceivedFrame {
    uint16_t length;
    uint8_t data[MODBUS_MAX_FRAME_SIZE];
  };
  static void receiveTaskEntry(void* port);
  void receiveTask();
  void completeFrame();
  void waitForBusIdle();
  // Schreibt einen Frame als Hex-Zeile ins Log, wenn g_logModbusTraffic aktiv ist.
  // direction: "TX", "RX" oder "verworfen"
  void logFrame(const char* direction, const uint8_t* data, size_t length, const char* note = nullptr);

  char logName[8] = "RTU";

  uart_port_t uartNumber = UART_NUM_1;
  QueueHandle_t uartEventQueue = nullptr;
  QueueHandle_t receivedFrameQueue = nullptr;
  uint32_t characterTimeUs = 1146;
  uint32_t t15Us = 1719;
  uint32_t t35Us = 4010;
  uint32_t hardwareIdleTimeoutUs = 2292;  // Ruhezeit, nach der der UART ein Idle-Event meldet

  // Zustand des Empfangs-Tasks
  uint8_t frameBuffer[MODBUS_MAX_FRAME_SIZE + 8];
  size_t frameLength = 0;
  bool frameIsCorrupt = false;
  int corruptReason = MODBUS_ERROR_FRAME;
  bool idleDetected = false;
  uint32_t idleDetectedAtUs = 0;

  std::atomic<uint32_t> lastBusActivityUs{0};
  std::atomic<int> lastDiscardReason{0};        // Grund des letzten verworfenen Frames (>= 4 Byte)
  std::atomic<uint32_t> discardedFrameCount{0};
};

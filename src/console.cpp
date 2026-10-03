// Serielle Konsole über USB-Serial-JTAG (esp_console-REPL): Status/Konfiguration anzeigen,
// Einstellungen ändern sowie Werkzeuge zur Inbetriebnahme des RS485-Busses
// (Pegel-Scan, automatische Pin-Erkennung, Bus-Diagnose).
//
// "detect" und "diag" brauchen einen freien UART und freie GPIOs. Sie werden deshalb nur
// vorgemerkt (RTC-Speicher), das Gerät startet neu und consoleEarlyBoot() führt sie aus,
// bevor die Modbus-Ports gestartet werden.
#include "console.h"
#include "chip.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "config.h"
#include "log_buffer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "modbus_rtu.h"
#include "util.h"
#include "web.h"

static const char* TAG = "console";

// Diagnose-Anforderung überlebt den Neustart im RTC-Speicher (wird beim Neustart nicht gelöscht).
// diagnosticRequest enthält DETECT_REQUEST_MAGIC oder DIAG_REQUEST_MAGIC, sonst Zufallswert/0.
// Bedeutung der Parameter:
//   detect: parameter1 = Baudrate, parameter2 = Slave-Adresse
//   diag:   parameter1 = RX-Pin, parameter2 = TX-Pin, parameter3 = DE-Pin + 1 (0 = kein DE-Pin)
static RTC_NOINIT_ATTR uint32_t diagnosticRequest;
static RTC_NOINIT_ATTR uint32_t diagnosticParameter1, diagnosticParameter2, diagnosticParameter3;
static const uint32_t DETECT_REQUEST_MAGIC = 0xDE7EC7ED;
static const uint32_t DIAG_REQUEST_MAGIC = 0xD1A6D1A6;

// GPIOs, die bei pinscan/detect durchprobiert werden
static const int* const SCAN_GPIOS = CHIP_SCAN_GPIOS;  // je nach Chip, siehe chip.h
static const int SCAN_GPIO_COUNT = sizeof(CHIP_SCAN_GPIOS) / sizeof(CHIP_SCAN_GPIOS[0]);

// Gibt json formatiert aus und gibt den Baum anschließend frei.
static void printJsonAndFree(cJSON* json) {
  std::string text = jsonToString(json, true);
  cJSON_Delete(json);
  printf("%s\n", text.c_str());
}

// Wendet die (Teil-)Konfiguration changes an, speichert sie, gibt das Ergebnis aus und
// startet bei Bedarf neu. changes wird freigegeben. Rückgabe: true bei Erfolg.
static bool applyConfigAndReport(cJSON* changes) {
  std::string errorMessage;
  bool rebootRequired = false;
  bool success = webApplyConfig(changes, errorMessage, rebootRequired);
  cJSON_Delete(changes);
  if (!success) {
    printf("Fehler: %s\n", errorMessage.c_str());
    return false;
  }
  printf(rebootRequired ? "Gespeichert - Neustart\n" : "Gespeichert\n");
  if (rebootRequired) scheduleReboot(500);
  return true;
}

// Wandelt einen Wert von der Kommandozeile in JSON um: true/false -> Bool,
// vollständig als Zahl lesbar -> Zahl, sonst Text.
static cJSON* parseValue(const char* text) {
  if (!strcmp(text, "true")) return cJSON_CreateTrue();
  if (!strcmp(text, "false")) return cJSON_CreateFalse();
  char* parseEnd;
  double number = strtod(text, &parseEnd);
  if (*text && !*parseEnd) return cJSON_CreateNumber(number);
  return cJSON_CreateString(text);
}

// ---------------------------------------------------------------- Befehle
// Alle Befehle: Rückgabe 0 = OK, 1 = Fehler/falscher Aufruf (Konvention von esp_console).

// "status": Live-Status als JSON ausgeben
static int commandStatus(int, char**) {
  printJsonAndFree(webStatusJson());
  return 0;
}

// "config": Konfiguration als JSON ausgeben (ohne die Liste gültiger GPIOs, die nur die Weboberfläche braucht)
static int commandConfig(int, char**) {
  cJSON* configJson = webConfigJson();
  cJSON_DeleteItemFromObject(configJson, "validGpios");
  printJsonAndFree(configJson);
  return 0;
}

// "set <schlüssel> <wert>": eine Einstellung über ihren JSON-Schlüssel ändern
static int commandSet(int argumentCount, char** arguments) {
  if (argumentCount != 3) {
    printf("Aufruf: set <schlüssel> <wert>   z.B. set mqttHost 192.168.1.10\n");
    return 1;
  }
  cJSON* changes = cJSON_CreateObject();
  cJSON_AddItemToObject(changes, arguments[1], parseValue(arguments[2]));
  return applyConfigAndReport(changes) ? 0 : 1;
}

// "port <1|2> <feld> <wert>": eine Port-Einstellung ändern.
// Es werden immer zwei Port-Objekte geschickt; das des anderen Ports bleibt leer (= unverändert).
static int commandPort(int argumentCount, char** arguments) {
  if (argumentCount != 4 || (strcmp(arguments[1], "1") && strcmp(arguments[1], "2"))) {
    printf("Aufruf: port <1|2> <role|baud|parity|stopBits|addr|rx|tx|de> <wert>\n");
    return 1;
  }
  cJSON* changes = cJSON_CreateObject();
  cJSON* portArray = cJSON_AddArrayToObject(changes, "ports");
  cJSON* port1Changes = cJSON_CreateObject();
  cJSON* port2Changes = cJSON_CreateObject();
  cJSON* targetPort = arguments[1][0] == '1' ? port1Changes : port2Changes;
  cJSON_AddItemToObject(targetPort, arguments[2], parseValue(arguments[3]));
  cJSON_AddItemToArray(portArray, port1Changes);
  cJSON_AddItemToArray(portArray, port2Changes);
  return applyConfigAndReport(changes) ? 0 : 1;
}

// "wifi <ssid> [passwort]": WLAN-Zugangsdaten setzen; ohne Passwort wird ein altes gelöscht (offenes WLAN)
static int commandWifi(int argumentCount, char** arguments) {
  if (argumentCount < 2 || argumentCount > 3) {
    printf("Aufruf: wifi <ssid> [passwort]   (Leerzeichen in Anführungszeichen)\n");
    return 1;
  }
  cJSON* changes = cJSON_CreateObject();
  cJSON_AddStringToObject(changes, "wifiSsid", arguments[1]);
  if (argumentCount == 3) cJSON_AddStringToObject(changes, "wifiPass", arguments[2]);
  else cJSON_AddTrueToObject(changes, "wifiPassClear");
  return applyConfigAndReport(changes) ? 0 : 1;
}

// "reboot": Neustart
// Befehl "trace on|off": schaltet die Protokollierung jedes Modbus-Frames (Hex-Dump) ein oder aus.
// Ohne Argument wird der aktuelle Zustand angezeigt.
static int commandTrace(int argumentCount, char** arguments) {
  if (argumentCount == 2 && (!strcmp(arguments[1], "on") || !strcmp(arguments[1], "off"))) {
    g_logModbusTraffic = !strcmp(arguments[1], "on");
  } else if (argumentCount != 1) {
    printf("Aufruf: trace on|off\n");
    return 1;
  }
  printf("Modbus-Verkehr protokollieren: %s\n", g_logModbusTraffic ? "an" : "aus");
  return 0;
}

static int commandReboot(int, char**) {
  scheduleReboot(100);
  return 0;
}

// "factory": Werkseinstellungen laden und neu starten
static int commandFactoryReset(int, char**) {
  configFactoryReset();
  printf("Werkseinstellungen - Neustart\n");
  scheduleReboot(300);
  return 0;
}

// "pinscan": Pegel jedes GPIO einmal mit Pull-down und einmal mit Pull-up lesen.
// Ausgabe "1/1" = extern High, "0/1" = offen, "0/0" = extern Low.
// So lässt sich z. B. der RO-Ausgang des Transceivers (Ruhepegel High) finden.
// Achtung: setzt alle Pins zurück, auch die der laufenden UARTs.
static int commandPinScan(int, char**) {
  printf("GPIO  PullDown/PullUp\n");
  for (int gpioNumber : CHIP_SCAN_GPIOS) {
    gpio_num_t gpio = (gpio_num_t)gpioNumber;
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(gpio, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(5));
    int levelWithPullDown = gpio_get_level(gpio);
    gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(5));
    int levelWithPullUp = gpio_get_level(gpio);
    const char* hint = levelWithPullDown && levelWithPullUp     ? "<- extern High (RO/Pull-up)"
                       : !levelWithPullDown && !levelWithPullUp ? "<- extern Low"
                                                                : "";
    printf("%4d  %d/%d %s\n", gpioNumber, levelWithPullDown, levelWithPullUp, hint);
  }
  printf("Hinweis: 'reboot' stellt die UARTs wieder her.\n");
  return 0;
}

// "detect [baud] [adresse]": automatische Pin-Erkennung für RTU1 vormerken und neu starten.
// Ungültige Werte werden durch 9600 Baud bzw. Adresse 247 ersetzt.
static int commandDetect(int argumentCount, char** arguments) {
  diagnosticParameter1 = argumentCount > 1 ? atoi(arguments[1]) : 9600;
  diagnosticParameter2 = argumentCount > 2 ? atoi(arguments[2]) : 247;
  if (diagnosticParameter1 < 1200) diagnosticParameter1 = 9600;
  if (diagnosticParameter2 < 1 || diagnosticParameter2 > 247) diagnosticParameter2 = 247;
  diagnosticRequest = DETECT_REQUEST_MAGIC;
  printf("Pin-Erkennung startet nach Neustart ...\n");
  scheduleReboot(200);
  return 0;
}

// "diag <rx> <tx> <de|-1>": Bus-Diagnose mit festen Pins vormerken und neu starten
static int commandDiag(int argumentCount, char** arguments) {
  if (argumentCount != 4) {
    printf("Aufruf: diag <rx> <tx> <de|-1>\n");
    return 1;
  }
  diagnosticParameter1 = atoi(arguments[1]);
  diagnosticParameter2 = atoi(arguments[2]);
  diagnosticParameter3 = (uint32_t)(atoi(arguments[3]) + 1);  // +1, damit -1 als 0 gespeichert werden kann
  diagnosticRequest = DIAG_REQUEST_MAGIC;
  printf("Diagnose startet nach Neustart ...\n");
  scheduleReboot(200);
  return 0;
}

// ---------------------------------------------------------------- Bus-Diagnose (vor Port-Start)

static const uart_port_t DIAGNOSTIC_UART = UART_NUM_1;

// Setzt alle Kandidaten-Pins aus pins[0..pinCount) außer rxPin/txPin:
// level 0/1 = als Ausgang mit diesem Pegel treiben (möglicher DE-Pin), level -1 = Pin wieder freigeben.
// Da der DE-Pin unbekannt ist, werden beim Senden einfach alle anderen Pins auf High gelegt.
static void driveCandidatePins(const int* pins, int pinCount, int rxPin, int txPin, int level) {
  for (int pinIndex = 0; pinIndex < pinCount; pinIndex++) {
    if (pins[pinIndex] == rxPin || pins[pinIndex] == txPin) continue;
    gpio_num_t gpio = (gpio_num_t)pins[pinIndex];
    if (level < 0) {
      gpio_reset_pin(gpio);
    } else {
      gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
      gpio_set_drive_capability(gpio, GPIO_DRIVE_CAP_0);  // ~5 mA, schont evtl. kurzgeschlossene Ausgänge
      gpio_set_level(gpio, level);
    }
  }
}

// Installiert den Diagnose-UART mit den angegebenen Parametern und Pins. Rückgabe: true bei Erfolg.
static bool openDiagnosticUart(uint32_t baudRate, uart_parity_t parity, uart_stop_bits_t stopBits, int rxPin,
                               int txPin) {
  uart_config_t uartConfig = {};
  uartConfig.baud_rate = (int)baudRate;
  uartConfig.data_bits = UART_DATA_8_BITS;
  uartConfig.parity = parity;
  uartConfig.stop_bits = stopBits;
  uartConfig.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uartConfig.source_clk = UART_SCLK_DEFAULT;
  return uart_driver_install(DIAGNOSTIC_UART, 512, 0, 0, nullptr, 0) == ESP_OK &&
         uart_param_config(DIAGNOSTIC_UART, &uartConfig) == ESP_OK &&
         uart_set_pin(DIAGNOSTIC_UART, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK;
}

// Entfernt den Diagnose-UART und gibt RX/TX-Pin wieder frei.
static void closeDiagnosticUart(int rxPin, int txPin) {
  uart_driver_delete(DIAGNOSTIC_UART);
  gpio_reset_pin((gpio_num_t)rxPin);
  gpio_reset_pin((gpio_num_t)txPin);
}

// Sendet FC03 "Register 35000, Anzahl 1" an den GoodWe und prüft auf eine gültige Antwort.
// Während des Sendens werden die driverEnableCandidates (mögliche DE-Pins) auf High gelegt.
// Empfangene Bytes werden zur Diagnose ausgegeben. Rückgabe: true bei gültiger Antwort.
static bool probeGoodwe(uint8_t slaveAddress, const int* driverEnableCandidates, int candidateCount, int rxPin,
                        int txPin) {
  // Anfrage: Adresse, FC03, Startregister 0x88B8 (= 35000), Anzahl 0x0001, CRC
  uint8_t request[8] = {slaveAddress, 0x03, 0x88, 0xB8, 0x00, 0x01};
  uint16_t crc = modbusCrc16(request, 6);
  request[6] = crc & 0xFF;
  request[7] = crc >> 8;

  // Senden: DE-Kandidaten High, kurz warten, Frame raus, danach wieder auf Empfang
  uart_flush_input(DIAGNOSTIC_UART);
  driveCandidatePins(driverEnableCandidates, candidateCount, rxPin, txPin, 1);
  esp_rom_delay_us(200);
  uart_write_bytes(DIAGNOSTIC_UART, request, 8);
  uart_wait_tx_done(DIAGNOSTIC_UART, pdMS_TO_TICKS(100));
  driveCandidatePins(driverEnableCandidates, candidateCount, rxPin, txPin, 0);

  // Empfangen und zur Diagnose ausgeben
  uint8_t response[16];
  int receivedCount = uart_read_bytes(DIAGNOSTIC_UART, response, sizeof(response), pdMS_TO_TICKS(250));
  if (receivedCount > 0) {
    printf("  [%d Byte:", receivedCount);
    for (int byteIndex = 0; byteIndex < receivedCount; byteIndex++) printf(" %02X", response[byteIndex]);
    printf("]\n");
  }

  // Gültige Antwort (Adresse, FC03, 2 Datenbytes, CRC) an beliebiger Stelle suchen -
  // vor der Antwort kann z. B. das Echo der eigenen Anfrage oder Störbytes stehen.
  for (int offset = 0; offset + 7 <= receivedCount; offset++) {
    const uint8_t* candidate = response + offset;
    if (candidate[0] == slaveAddress && candidate[1] == 0x03 && candidate[2] == 2 &&
        modbusCrc16(candidate, 5) == (uint16_t)(candidate[5] | (candidate[6] << 8)))
      return true;
  }
  return false;
}

// Speichert die gefundenen Bus-Parameter als RTU1 (GoodWe-Master). Benutzt RTU2 einen dieser
// Pins, wird RTU2 deaktiviert, damit die Konfiguration gültig bleibt.
static void saveRtu1Settings(uint32_t baudRate, char parity, int stopBits, int slaveAddress, int rxPin, int txPin,
                             int driverEnablePin) {
  PortConfig& rtu1 = g_config.port[0];
  rtu1 = {ROLE_GOODWE_MASTER, baudRate,      parity,       (uint8_t)stopBits,
          (uint8_t)slaveAddress, (int8_t)rxPin, (int8_t)txPin, (int8_t)driverEnablePin};
  PortConfig& rtu2 = g_config.port[1];
  for (int gpioNumber : {rxPin, txPin, driverEnablePin}) {
    if (gpioNumber < 0) continue;
    if (rtu2.rxPin == gpioNumber || rtu2.txPin == gpioNumber || rtu2.driverEnablePin == gpioNumber)
      rtu2.role = ROLE_OFF;
  }
  configSave();
}

// Ermittelt den DE-Pin für bereits gefundene RX/TX-Pins.
// Rückgabe: -1 = kein DE-Pin nötig (automatische Richtungsumschaltung), >= 0 = GPIO,
// -2 = nicht eindeutig ermittelbar.
static int detectDriverEnablePin(uint32_t baudRate, uint8_t slaveAddress, int rxPin, int txPin) {
  int driverEnablePin = -2;
  openDiagnosticUart(baudRate, UART_PARITY_DISABLE, UART_STOP_BITS_1, rxPin, txPin);
  // Antwort ganz ohne getriebenen Pin -> Modul schaltet selbst um
  if (probeGoodwe(slaveAddress, nullptr, 0, rxPin, txPin)) driverEnablePin = -1;
  // sonst jeden Kandidaten einzeln als DE-Pin probieren
  for (int scanIndex = 0; scanIndex < SCAN_GPIO_COUNT && driverEnablePin == -2; scanIndex++) {
    int gpioNumber = SCAN_GPIOS[scanIndex];
    if (gpioNumber == rxPin || gpioNumber == txPin) continue;
    int singleCandidate[1] = {gpioNumber};
    if (probeGoodwe(slaveAddress, singleCandidate, 1, rxPin, txPin)) driverEnablePin = gpioNumber;
    driveCandidatePins(singleCandidate, 1, rxPin, txPin, -1);
  }
  closeDiagnosticUart(rxPin, txPin);
  return driverEnablePin;
}

// "detect": probiert alle RX/TX-Kombinationen der SCAN_GPIOS (alle übrigen Pins als mögliche
// DE-Pins auf High) mit 8N1, bis der GoodWe antwortet. Danach wird der DE-Pin bestimmt und
// das Ergebnis als RTU1 gespeichert.
static void runDetect() {
  uint32_t baudRate = diagnosticParameter1;
  uint8_t slaveAddress = diagnosticParameter2;
  printf("[DETECT] Suche GoodWe (Adr %u, %lu 8N1) an allen RX/TX-Kombinationen ...\n", slaveAddress,
         (unsigned long)baudRate);

  // 1. RX/TX-Paar suchen
  int foundRxPin = -1, foundTxPin = -1;
  for (int rxIndex = 0; rxIndex < SCAN_GPIO_COUNT && foundRxPin < 0; rxIndex++) {
    for (int txIndex = 0; txIndex < SCAN_GPIO_COUNT && foundRxPin < 0; txIndex++) {
      int rxPin = SCAN_GPIOS[rxIndex], txPin = SCAN_GPIOS[txIndex];
      if (rxPin == txPin) continue;
      if (!openDiagnosticUart(baudRate, UART_PARITY_DISABLE, UART_STOP_BITS_1, rxPin, txPin)) continue;
      if (probeGoodwe(slaveAddress, SCAN_GPIOS, SCAN_GPIO_COUNT, rxPin, txPin)) {
        foundRxPin = rxPin;
        foundTxPin = txPin;
      }
      closeDiagnosticUart(rxPin, txPin);
      driveCandidatePins(SCAN_GPIOS, SCAN_GPIO_COUNT, rxPin, txPin, -1);
    }
    printf("[DETECT] RX=%d geprüft\n", SCAN_GPIOS[rxIndex]);
  }
  if (foundRxPin < 0) {
    printf("[DETECT] Keine Antwort gefunden. Baudrate/Adresse/A-B-Verdrahtung prüfen.\n");
    return;
  }

  // 2. DE-Pin bestimmen
  int foundDriverEnablePin = detectDriverEnablePin(baudRate, slaveAddress, foundRxPin, foundTxPin);
  if (foundDriverEnablePin == -2) {
    printf("[DETECT] DE-Pin nicht eindeutig - nicht gespeichert\n");
    return;
  }

  // 3. Speichern
  printf("[DETECT] Ergebnis RTU1: RX=%d TX=%d DE=%d -> gespeichert\n", foundRxPin, foundTxPin, foundDriverEnablePin);
  saveRtu1Settings(baudRate, 'N', 1, slaveAddress, foundRxPin, foundTxPin, foundDriverEnablePin);
}

// Ein UART-Datenformat für die Diagnose
struct SerialFormat {
  uart_parity_t parity;
  uart_stop_bits_t stopBits;
  char parityLetter;  // für PortConfig::parity
  int stopBitCount;   // für PortConfig::stopBits
  const char* name;
};

// "diag": mit den angegebenen Pins (und zusätzlich mit vertauschtem RX/TX)
//  - den Ruhepegel an RX prüfen,
//  - je 3 s bei 9600 und 19200 Baud mithören,
//  - alle Kombinationen aus Baudrate, Format und typischen Adressen abfragen.
// Die erste Kombination, auf die der GoodWe antwortet, wird als RTU1 gespeichert.
static void runDiag() {
  int rxPin = diagnosticParameter1, txPin = diagnosticParameter2;
  int driverEnablePin = (int)diagnosticParameter3 - 1;
  int driverEnablePins[1] = {driverEnablePin};
  int driverEnablePinCount = driverEnablePin >= 0 ? 1 : 0;
  static const uint32_t BAUD_RATES[] = {9600, 19200, 4800, 38400, 115200, 2400};
  static const SerialFormat SERIAL_FORMATS[] = {{UART_PARITY_DISABLE, UART_STOP_BITS_1, 'N', 1, "8N1"},
                                                {UART_PARITY_EVEN, UART_STOP_BITS_1, 'E', 1, "8E1"},
                                                {UART_PARITY_DISABLE, UART_STOP_BITS_2, 'N', 2, "8N2"},
                                                {UART_PARITY_ODD, UART_STOP_BITS_1, 'O', 1, "8O1"}};

  // Durchgang 0: Pins wie angegeben, Durchgang 1: RX und TX vertauscht (häufiger Verdrahtungsfehler)
  for (int pass = 0; pass < 2; pass++) {
    int passRxPin = pass ? txPin : rxPin;
    int passTxPin = pass ? rxPin : txPin;
    printf("[DIAG] === RX=%d TX=%d DE=%d ===\n", passRxPin, passTxPin, driverEnablePin);
    if (driverEnablePinCount) driveCandidatePins(driverEnablePins, 1, passRxPin, passTxPin, 0);  // auf Empfang

    // Ruhepegel: ein korrekt vorgespannter Bus liefert an RO dauerhaft High
    gpio_reset_pin((gpio_num_t)passRxPin);
    gpio_set_direction((gpio_num_t)passRxPin, GPIO_MODE_INPUT);
    printf("[DIAG] Ruhepegel RX=%d (1 = Bus idle korrekt vorgespannt)\n", gpio_get_level((gpio_num_t)passRxPin));

    // Mithören: zeigt, ob überhaupt (z. B. von einem anderen Master) Verkehr auf dem Bus ist
    for (uint32_t listenBaudRate : {9600UL, 19200UL}) {
      openDiagnosticUart(listenBaudRate, UART_PARITY_DISABLE, UART_STOP_BITS_1, passRxPin, passTxPin);
      uint8_t receivedBytes[96];
      int receivedCount = uart_read_bytes(DIAGNOSTIC_UART, receivedBytes, sizeof(receivedBytes), pdMS_TO_TICKS(3000));
      printf("[DIAG] Mithören %lu Bd, 3 s: %d Byte ", (unsigned long)listenBaudRate,
             receivedCount < 0 ? 0 : receivedCount);
      for (int byteIndex = 0; byteIndex < receivedCount; byteIndex++) printf("%02X ", receivedBytes[byteIndex]);
      printf("\n");
      closeDiagnosticUart(passRxPin, passTxPin);
    }

    // Aktiv abfragen: alle Baudraten x Formate x typische Adressen
    for (uint32_t baudRate : BAUD_RATES) {
      for (const SerialFormat& format : SERIAL_FORMATS) {
        openDiagnosticUart(baudRate, format.parity, format.stopBits, passRxPin, passTxPin);
        for (uint8_t slaveAddress : {247, 1, 2, 3, 4, 5, 10, 11, 100}) {
          if (probeGoodwe(slaveAddress, driverEnablePins, driverEnablePinCount, passRxPin, passTxPin)) {
            printf("[DIAG] *** ANTWORT: RX=%d TX=%d DE=%d %lu %s Adresse %u -> in RTU1 gespeichert ***\n",
                   passRxPin, passTxPin, driverEnablePin, (unsigned long)baudRate, format.name, slaveAddress);
            closeDiagnosticUart(passRxPin, passTxPin);
            saveRtu1Settings(baudRate, format.parityLetter, format.stopBitCount, slaveAddress, passRxPin, passTxPin,
                             driverEnablePin);
            return;
          }
        }
        closeDiagnosticUart(passRxPin, passTxPin);
      }
      printf("[DIAG] %lu Bd: keine Antwort\n", (unsigned long)baudRate);
    }
  }
  printf("[DIAG] Keine Antwort in allen Kombinationen.\n");
}

// Führt eine vor dem Neustart vorgemerkte Diagnose ("detect"/"diag") aus.
// Die Anforderung wird vorher gelöscht, damit sie auch bei einem Absturz nur einmal läuft.
void consoleEarlyBoot() {
  uint32_t request = diagnosticRequest;
  diagnosticRequest = 0;
  if (request != DETECT_REQUEST_MAGIC && request != DIAG_REQUEST_MAGIC) return;
  vTaskDelay(pdMS_TO_TICKS(1500));  // USB-Konsole verbinden lassen
  if (request == DETECT_REQUEST_MAGIC) runDetect();
  else runDiag();
}

// ---------------------------------------------------------------- REPL

// Startet die REPL auf USB-Serial-JTAG und registriert alle Befehle (plus "help").
void consoleStart() {
  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t replConfig = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  replConfig.prompt = "bridge>";
  replConfig.max_cmdline_length = 512;
  esp_console_dev_usb_serial_jtag_config_t usbConfig = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  if (esp_console_new_repl_usb_serial_jtag(&usbConfig, &replConfig, &repl) != ESP_OK) {
    ESP_LOGE(TAG, "Konsole konnte nicht gestartet werden");
    return;
  }
  esp_console_register_help_command();

  struct ConsoleCommand {
    const char* name;
    const char* help;
    esp_console_cmd_func_t handler;
  };
  static const ConsoleCommand COMMANDS[] = {
      {"status", "Live-Status als JSON", commandStatus},
      {"config", "Konfiguration anzeigen", commandConfig},
      {"set", "Einstellung ändern: set <schlüssel> <wert>", commandSet},
      {"port", "Port-Einstellung: port <1|2> <role|baud|parity|stopBits|addr|rx|tx|de> <wert>", commandPort},
      {"wifi", "WLAN setzen: wifi <ssid> [passwort]", commandWifi},
      {"pinscan", "Pegel aller GPIOs anzeigen", commandPinScan},
      {"detect", "RTU1-Pins am GoodWe automatisch finden: detect [baud] [adresse]", commandDetect},
      {"diag", "Bus mithören + Baud/Parität/Adressen testen: diag <rx> <tx> <de|-1>", commandDiag},
      {"trace", "Modbus-Verkehr als Hex protokollieren: trace on|off", commandTrace},
      {"reboot", "Neustart", commandReboot},
      {"factory", "Werkseinstellungen", commandFactoryReset},
  };
  for (const ConsoleCommand& command : COMMANDS) {
    esp_console_cmd_t registration = {};
    registration.command = command.name;
    registration.help = command.help;
    registration.func = command.handler;
    esp_console_cmd_register(&registration);
  }
  esp_console_start_repl(repl);
}

// Führt eine Befehlszeile aus und fängt alles ab, was der Befehl per printf ausgibt.
// stdout ist in ESP-IDF je Task getrennt; umgeleitet wird daher nur die Ausgabe des aufrufenden
// Tasks, die USB-Konsole bleibt unberührt. Ein Mutex verhindert, dass zwei Web-Clients
// gleichzeitig denselben Ausgabepuffer benutzen.
bool consoleRunCommand(const char* commandLine, std::string& output) {
  static SemaphoreHandle_t runLock = xSemaphoreCreateMutex();
  static char outputBuffer[8192];
  output.clear();
  if (xSemaphoreTake(runLock, pdMS_TO_TICKS(5000)) != pdTRUE) {
    output = "Konsole belegt, bitte erneut versuchen\n";
    return false;
  }
  memset(outputBuffer, 0, sizeof(outputBuffer));
  FILE* capture = fmemopen(outputBuffer, sizeof(outputBuffer) - 1, "w");
  FILE* previousStdout = stdout;
  if (capture) stdout = capture;

  int commandResult = 0;
  esp_err_t runResult = esp_console_run(commandLine, &commandResult);

  if (capture) {
    fflush(capture);
    stdout = previousStdout;
    fclose(capture);
  }
  output = outputBuffer;
  if (runResult == ESP_ERR_NOT_FOUND) output += "Unbekannter Befehl - 'help' zeigt alle Befehle\n";
  else if (runResult == ESP_ERR_INVALID_ARG) output += "";  // leere Zeile
  else if (runResult != ESP_OK) output += std::string("Fehler: ") + esp_err_to_name(runResult) + "\n";
  xSemaphoreGive(runLock);
  return runResult == ESP_OK && commandResult == 0;
}

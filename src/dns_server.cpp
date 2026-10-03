// Minimaler DNS-Server für das Captive Portal: jede A-Anfrage wird mit der Adresse des
// Access-Points beantwortet, damit Smartphones/Laptops die Weboberfläche öffnen.
#include "dns_server.h"
#include <cstring>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* TAG = "dns";

static const uint16_t DNS_PORT = 53;
static const uint32_t DNS_TASK_STACK_SIZE = 3072;
static const UBaseType_t DNS_TASK_PRIORITY = 2;

// Aufbau des DNS-Headers (RFC 1035, Kap. 4.1.1), Byte-Offsets in der Nachricht:
//   0..1   ID       (unverändert zurückgeben)
//   2      Flags 1  QR | Opcode(4) | AA | TC | RD
//   3      Flags 2  RA | Z(3) | RCODE(4)
//   4..5   QDCOUNT  Anzahl Fragen
//   6..7   ANCOUNT  Anzahl Antworten
//   8..9   NSCOUNT  Anzahl Authority-Einträge
//   10..11 ARCOUNT  Anzahl Additional-Einträge
// Danach folgt der Frageteil: QNAME (Labels mit Längenbyte, Ende = 0-Byte), QTYPE (2), QCLASS (2).
static const int DNS_OFFSET_FLAGS1 = 2;
static const int DNS_OFFSET_FLAGS2 = 3;
static const int DNS_OFFSET_QDCOUNT = 4;
static const int DNS_OFFSET_ANCOUNT = 6;
static const int DNS_OFFSET_NSCOUNT = 8;
static const int DNS_OFFSET_ARCOUNT = 10;
static const int DNS_HEADER_SIZE = 12;

static const uint8_t DNS_FLAG_QR_RESPONSE = 0x80;  // in Flags 1: Nachricht ist eine Antwort
static const uint8_t DNS_FLAG_AA = 0x04;           // in Flags 1: autoritative Antwort
static const uint8_t DNS_FLAG_RD = 0x01;           // in Flags 1: Rekursion gewünscht
static const uint8_t DNS_FLAG_RA = 0x80;           // in Flags 2: Rekursion verfügbar, RCODE 0 = kein Fehler

static const uint16_t DNS_TYPE_A = 1;
// Länge von QTYPE + QCLASS hinter dem abschließenden 0-Byte des QNAME
static const int DNS_QTYPE_QCLASS_SIZE = 4;

// Antwort-Eintrag ohne die eigentliche IPv4-Adresse (die folgt mit 4 Byte direkt dahinter)
static const uint8_t DNS_ANSWER_TEMPLATE[] = {
    0xC0, 0x0C,  // NAME: Kompressionszeiger auf den QNAME bei Offset 12
    0, 1,        // TYPE  A
    0, 1,        // CLASS IN
    0, 0, 0, 60, // TTL 60 s
    0, 4,        // RDLENGTH: 4 Byte IPv4-Adresse
};
static const int IPV4_ADDRESS_SIZE = 4;

static const size_t DNS_BUFFER_SIZE = 512;
// Platz am Pufferende für den Antwort-Eintrag (12 + 4 Byte), der an die Anfrage angehängt wird
static const size_t DNS_ANSWER_RESERVE = 16;

static TaskHandle_t dnsTaskHandle = nullptr;
static volatile bool dnsServerRunning = false;
static uint32_t accessPointIp = 0;  // Netzwerk-Byte-Reihenfolge
static int dnsSocket = -1;

// Schließt den Socket, meldet den Task als beendet und löscht den aufrufenden Task (kehrt nicht zurück)
static void closeSocketAndEndTask() {
  if (dnsSocket >= 0) close(dnsSocket);
  dnsSocket = -1;
  dnsTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// Wandelt die DNS-Anfrage in message (messageLength Byte, < 0 bei Empfangsfehler) direkt im Puffer
// in die Antwort um. Der Puffer braucht DNS_ANSWER_RESERVE Byte Platz hinter der Anfrage.
// Rückgabe: Länge der Antwort oder 0, wenn die Anfrage verworfen wird.
static int buildAnswer(uint8_t* message, int messageLength) {
  if (messageLength < DNS_HEADER_SIZE) return 0;
  // nur Standard-Anfragen mit genau einer Frage beantworten
  bool isResponse = message[DNS_OFFSET_FLAGS1] & DNS_FLAG_QR_RESPONSE;
  bool hasExactlyOneQuestion = message[DNS_OFFSET_QDCOUNT] == 0 && message[DNS_OFFSET_QDCOUNT + 1] == 1;
  if (isResponse || !hasExactlyOneQuestion) return 0;

  // QNAME überspringen: jedes Label beginnt mit seinem Längenbyte, das 0-Byte beendet den Namen
  int nameEndOffset = DNS_HEADER_SIZE;
  while (nameEndOffset < messageLength && message[nameEndOffset])
    nameEndOffset += message[nameEndOffset] + 1;
  int questionEndOffset = nameEndOffset + 1 + DNS_QTYPE_QCLASS_SIZE;
  if (questionEndOffset > messageLength) return 0;
  uint16_t questionType = (message[nameEndOffset + 1] << 8) | message[nameEndOffset + 2];
  bool answerWithAddress = questionType == DNS_TYPE_A;

  // Header zur Antwort umbauen: Antwort, autoritativ, RD aus der Anfrage übernehmen
  message[DNS_OFFSET_FLAGS1] = DNS_FLAG_QR_RESPONSE | DNS_FLAG_AA | (message[DNS_OFFSET_FLAGS1] & DNS_FLAG_RD);
  message[DNS_OFFSET_FLAGS2] = DNS_FLAG_RA;
  message[DNS_OFFSET_ANCOUNT] = 0;
  message[DNS_OFFSET_ANCOUNT + 1] = answerWithAddress ? 1 : 0;
  message[DNS_OFFSET_NSCOUNT] = message[DNS_OFFSET_NSCOUNT + 1] = 0;
  message[DNS_OFFSET_ARCOUNT] = message[DNS_OFFSET_ARCOUNT + 1] = 0;

  // Antwort = Header + Frage (unverändert) + ggf. ein A-Eintrag; andere Typen bekommen eine leere Antwort
  int answerLength = questionEndOffset;
  if (answerWithAddress) {
    memcpy(message + answerLength, DNS_ANSWER_TEMPLATE, sizeof(DNS_ANSWER_TEMPLATE));
    answerLength += sizeof(DNS_ANSWER_TEMPLATE);
    memcpy(message + answerLength, &accessPointIp, IPV4_ADDRESS_SIZE);
    answerLength += IPV4_ADDRESS_SIZE;
  }
  return answerLength;
}

// FreeRTOS-Task: öffnet UDP-Port 53 und beantwortet Anfragen, bis dnsServerStop() aufgerufen wird
static void dnsTask(void*) {
  dnsSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in listenAddress = {};
  listenAddress.sin_family = AF_INET;
  listenAddress.sin_port = htons(DNS_PORT);
  listenAddress.sin_addr.s_addr = htonl(INADDR_ANY);
  if (dnsSocket < 0 || bind(dnsSocket, (sockaddr*)&listenAddress, sizeof(listenAddress)) < 0) {
    ESP_LOGE(TAG, "Socket/bind fehlgeschlagen");
    closeSocketAndEndTask();
    return;
  }
  // Empfangs-Timeout, damit dnsServerStop() spätestens nach 1 s wirkt
  timeval receiveTimeout = {1, 0};
  setsockopt(dnsSocket, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout));

  uint8_t message[DNS_BUFFER_SIZE];
  while (dnsServerRunning) {
    sockaddr_in clientAddress;
    socklen_t clientAddressLength = sizeof(clientAddress);
    int receivedLength = recvfrom(dnsSocket, message, sizeof(message) - DNS_ANSWER_RESERVE, 0,
                                  (sockaddr*)&clientAddress, &clientAddressLength);
    int answerLength = buildAnswer(message, receivedLength);
    if (answerLength == 0) continue;
    sendto(dnsSocket, message, answerLength, 0, (sockaddr*)&clientAddress, clientAddressLength);
  }
  closeSocketAndEndTask();
}

// Startet den DNS-Task (nur einmal); accessPointIpNetworkOrder ist die Adresse für alle A-Antworten
void dnsServerStart(uint32_t accessPointIpNetworkOrder) {
  if (dnsTaskHandle) return;
  accessPointIp = accessPointIpNetworkOrder;
  dnsServerRunning = true;
  xTaskCreate(dnsTask, "dns", DNS_TASK_STACK_SIZE, nullptr, DNS_TASK_PRIORITY, &dnsTaskHandle);
}

// Fordert das Beenden an; der Task räumt nach spätestens 1 s (Empfangs-Timeout) selbst auf
void dnsServerStop() { dnsServerRunning = false; }

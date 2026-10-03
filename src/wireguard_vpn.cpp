// WireGuard-VPN-Client: Uhrzeit per SNTP holen, Tunnel aufbauen und überwachen.
#include "wireguard_vpn.h"
#include <cstdio>
#include <cstring>
#include <string>
#include "config.h"
#include "diagnostics.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wireguard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "net.h"
#include "ota.h"
#include "time_sync.h"
#include "util.h"

static const char* TAG = "vpn";

static const uint32_t TIME_SYNC_TIMEOUT_MS = 30000;
static const uint32_t STATUS_CHECK_INTERVAL_MS = 2000;
static const uint32_t RECONNECT_AFTER_DOWN_MS = 180000;
static const uint32_t STABLE_AFTER_START_MS = 120000;  // so lange ohne Absturz = VPN-Start gilt als sicher  // Tunnel nach 3 min ohne Handshake neu aufbauen
static const int MAX_ALLOWED_NETWORKS = 8;

// Zustand für die Statusanzeige
static const char* tunnelState = "aus";
static char lastErrorMessage[96] = "";
static bool tunnelUp = false;
static uint32_t tunnelUpSinceMs = 0;

// Tunnel-Adresse und erlaubte Zielnetze (Netzwerk-Byte-Reihenfolge)
static uint32_t tunnelAddress = 0;
static uint32_t tunnelNetmask = 0;
struct AllowedNetwork {
  uint32_t network;
  uint32_t netmask;
};
static AllowedNetwork allowedNetworks[MAX_ALLOWED_NETWORKS];
static int allowedNetworkCount = 0;

// esp_wireguard speichert nur Zeiger auf diese Texte -> müssen dauerhaft gültig bleiben
static std::string privateKeyText, publicKeyText, presharedKeyText, addressText, netmaskText, endpointText;
static wireguard_config_t wireguardConfig = ESP_WIREGUARD_CONFIG_DEFAULT();
static wireguard_ctx_t wireguardContext = ESP_WIREGUARD_CONTEXT_DEFAULT();
static time_t lastHandshakeTime = 0;

// Wandelt eine Präfixlänge (0..32) in eine Netzmaske in Netzwerk-Byte-Reihenfolge um.
static uint32_t prefixToNetmask(int prefixLength) {
  if (prefixLength <= 0) return 0;
  if (prefixLength >= 32) return 0xFFFFFFFF;
  return htonl(0xFFFFFFFFu << (32 - prefixLength));
}

// Zerlegt "a.b.c.d/nn" in Adresse und Maske. Ohne "/nn" gilt /32. Rückgabe: false bei ungültiger Angabe.
static bool parseCidr(const char* text, uint32_t& address, uint32_t& netmask) {
  char addressPart[20];
  int prefixLength = 32;
  const char* slash = strchr(text, '/');
  size_t addressLength = slash ? (size_t)(slash - text) : strlen(text);
  if (addressLength == 0 || addressLength >= sizeof(addressPart)) return false;
  memcpy(addressPart, text, addressLength);
  addressPart[addressLength] = 0;
  if (slash) prefixLength = atoi(slash + 1);
  if (prefixLength < 0 || prefixLength > 32) return false;
  ip4_addr_t parsed;
  if (!ip4addr_aton(addressPart, &parsed)) return false;
  address = parsed.addr;
  netmask = prefixToNetmask(prefixLength);
  return true;
}

// Liest die kommagetrennte Liste "AllowedIPs" (IPv6-Einträge werden übersprungen).
static void parseAllowedNetworks(const char* list) {
  allowedNetworkCount = 0;
  std::string remaining = list;
  size_t start = 0;
  while (start < remaining.size() && allowedNetworkCount < MAX_ALLOWED_NETWORKS) {
    size_t end = remaining.find(',', start);
    if (end == std::string::npos) end = remaining.size();
    std::string entry = remaining.substr(start, end - start);
    entry.erase(0, entry.find_first_not_of(" \t"));
    entry.erase(entry.find_last_not_of(" \t") + 1);
    uint32_t network, netmask;
    if (!entry.empty() && entry.find(':') == std::string::npos && parseCidr(entry.c_str(), network, netmask)) {
      allowedNetworks[allowedNetworkCount++] = {network & netmask, netmask};
    }
    start = end + 1;
  }
}

bool wireguardSourceAddressFor(uint32_t destinationIp, uint32_t& sourceIp) {
  if (!tunnelUp || !tunnelAddress) return false;
  // Ziel im eigenen Tunnel-Subnetz: die normale Netz-Route des WireGuard-Interfaces genügt
  if ((destinationIp & tunnelNetmask) == (tunnelAddress & tunnelNetmask)) return false;
  for (int networkIndex = 0; networkIndex < allowedNetworkCount; networkIndex++) {
    const AllowedNetwork& allowed = allowedNetworks[networkIndex];
    // 0.0.0.0/0 nicht berücksichtigen: sonst liefe der gesamte Verkehr durch den Tunnel
    if (allowed.netmask == 0) continue;
    if ((destinationIp & allowed.netmask) == allowed.network) {
      sourceIp = tunnelAddress;
      return true;
    }
  }
  return false;
}

// Merkt sich einen Fehler für die Statusanzeige und schreibt ihn ins Log.
static void setError(const char* message) {
  copyString(lastErrorMessage, message, sizeof(lastErrorMessage));
  ESP_LOGW(TAG, "%s", message);
}

// Wartet, bis die Uhrzeit per NTP gestellt ist (time_sync). WireGuard braucht sie für den
// Zeitstempel im Handshake; mit falscher Uhr lehnt die Gegenstelle die Verbindung ab.
static bool waitForTimeSync() { return timeWaitForSync(TIME_SYNC_TIMEOUT_MS); }

// Überträgt die Einstellungen in die Struktur von esp_wireguard. Rückgabe: false bei ungültigen Werten.
static bool prepareConfiguration() {
  uint32_t address, netmask;
  if (!parseCidr(g_config.wgAddress, address, netmask)) {
    setError("Ungültige Tunnel-Adresse (Format a.b.c.d/nn)");
    return false;
  }
  tunnelAddress = address;
  tunnelNetmask = netmask;
  char addressBuffer[16], netmaskBuffer[16];
  ip4_addr_t addressValue = {address}, netmaskValue = {netmask};
  ip4addr_ntoa_r(&addressValue, addressBuffer, sizeof(addressBuffer));
  ip4addr_ntoa_r(&netmaskValue, netmaskBuffer, sizeof(netmaskBuffer));
  addressText = addressBuffer;
  netmaskText = netmaskBuffer;
  privateKeyText = g_config.wgPrivateKey;
  publicKeyText = g_config.wgPeerPublicKey;
  presharedKeyText = g_config.wgPresharedKey;
  endpointText = g_config.wgEndpoint;
  parseAllowedNetworks(g_config.wgAllowedIps);

  wireguardConfig.private_key = privateKeyText.c_str();
  wireguardConfig.public_key = publicKeyText.c_str();
  wireguardConfig.preshared_key = presharedKeyText.empty() ? nullptr : presharedKeyText.c_str();
  wireguardConfig.address = addressText.c_str();  // eigene Tunnel-Adresse
  wireguardConfig.netmask = netmaskText.c_str();
  wireguardConfig.endpoint = endpointText.c_str();
  wireguardConfig.port = g_config.wgEndpointPort;
  wireguardConfig.persistent_keepalive = g_config.wgKeepaliveSec;
  return true;
}

// ---- Aufrufe im Netzwerk-Task (tcpip) ausführen ----
// Die lwIP-Kernsperre ist in dieser Konfiguration aus; netif_add & Co. dürfen dann nur im
// tcpip-Task laufen. esp_netif_tcpip_exec() führt die Funktion dort synchron aus.
static esp_err_t initInNetworkTask(void*) { return esp_wireguard_init(&wireguardConfig, &wireguardContext); }
static esp_err_t connectInNetworkTask(void*) { return esp_wireguard_connect(&wireguardContext); }
static esp_err_t disconnectInNetworkTask(void*) { return esp_wireguard_disconnect(&wireguardContext); }

// Trägt die erlaubten Netze beim Peer ein. Ohne diesen Eintrag verwirft WireGuard Pakete aus
// diesen Netzen (z. B. die Antworten des GoodWe-Gateways).
static esp_err_t addAllowedNetworksInNetworkTask(void*) {
  esp_err_t overallResult = ESP_OK;
  // eigenes Tunnel-Subnetz immer erlauben
  char networkText[16], netmaskBuffer[16];
  ip4_addr_t tunnelNetwork = {tunnelAddress & tunnelNetmask}, tunnelMask = {tunnelNetmask};
  ip4addr_ntoa_r(&tunnelNetwork, networkText, sizeof(networkText));
  ip4addr_ntoa_r(&tunnelMask, netmaskBuffer, sizeof(netmaskBuffer));
  if (esp_wireguard_add_allowed_ip(&wireguardContext, networkText, netmaskBuffer) != ESP_OK) overallResult = ESP_FAIL;
  for (int networkIndex = 0; networkIndex < allowedNetworkCount; networkIndex++) {
    ip4_addr_t network = {allowedNetworks[networkIndex].network}, netmask = {allowedNetworks[networkIndex].netmask};
    ip4addr_ntoa_r(&network, networkText, sizeof(networkText));
    ip4addr_ntoa_r(&netmask, netmaskBuffer, sizeof(netmaskBuffer));
    if (esp_wireguard_add_allowed_ip(&wireguardContext, networkText, netmaskBuffer) != ESP_OK) overallResult = ESP_FAIL;
  }
  return overallResult;
}

// Startet die Verbindung. Die Gegenstelle wird per DNS aufgelöst; solange das läuft,
// liefert esp_wireguard_connect ESP_ERR_RETRY. Rückgabe: true, wenn der Tunnel gestartet ist.
static bool startConnection() {
  tunnelState = "verbinde";
  for (int attempt = 0; attempt < 30; attempt++) {
    esp_err_t result = esp_netif_tcpip_exec(connectInNetworkTask, nullptr);
    if (result == ESP_OK) {
      if (esp_netif_tcpip_exec(addAllowedNetworksInNetworkTask, nullptr) != ESP_OK)
        setError("Erlaubte Netze (AllowedIPs) konnten nicht eingetragen werden");
      ESP_LOGI(TAG, "Tunnel %s/%s -> %s:%u gestartet", addressText.c_str(), netmaskText.c_str(),
               endpointText.c_str(), g_config.wgEndpointPort);
      return true;
    }
    if (result != ESP_ERR_RETRY) {
      char message[96];
      snprintf(message, sizeof(message), "Tunnel-Start fehlgeschlagen: %s (Endpunkt auflösbar?)", esp_err_to_name(result));
      setError(message);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));  // DNS-Auflösung des Endpunkts läuft noch
  }
  setError("Endpunkt konnte nicht per DNS aufgelöst werden");
  return false;
}

// Task: wartet auf bestätigte Firmware, WLAN und Uhrzeit, baut den Tunnel auf und überwacht ihn.
static void wireguardTask(void*) {
  // Erst nach der Bestätigung einer neuen Firmware starten: ein Fehler im Tunnelaufbau darf
  // nie die ganze Firmware zurückrollen (Absturzschutz und Absturzbericht greifen dann).
  tunnelState = "warte auf Firmware-Bestätigung";
  while (otaIsAwaitingConfirmation()) vTaskDelay(pdMS_TO_TICKS(1000));

  tunnelState = "warte auf WLAN";
  while (!netStaConnected()) vTaskDelay(pdMS_TO_TICKS(1000));

  tunnelState = "warte auf Uhrzeit (NTP)";
  if (!waitForTimeSync()) setError("Uhrzeit konnte nicht per NTP gestellt werden - Handshake kann scheitern");

  if (!prepareConfiguration()) {
    tunnelState = "Fehler";
    vTaskDelete(nullptr);
    return;
  }

  diagnosticsVpnStartBegin();  // stürzt der ESP ab jetzt ab, wird das beim nächsten Start erkannt
  uint32_t taskStartedMs = millisSinceBoot();
  bool startConsideredStable = false;
  esp_err_t initResult = esp_netif_tcpip_exec(initInNetworkTask, nullptr);
  if (initResult != ESP_OK) {
    char message[96];
    snprintf(message, sizeof(message), "WireGuard-Initialisierung fehlgeschlagen: %s (Schlüssel prüfen)",
             esp_err_to_name(initResult));
    setError(message);
    tunnelState = "Fehler";
    diagnosticsVpnStartSucceeded();  // kein Absturz, nur falsche Daten
    vTaskDelete(nullptr);
    return;
  }

  bool connected = false;
  uint32_t connectStartedMs = 0;
  for (;;) {
    // --- Verbindung starten (beim ersten Mal und nach längerem Ausfall) ---
    if (!connected) {
      connected = startConnection();
      if (!connected) {
        tunnelState = "Fehler";
        vTaskDelay(pdMS_TO_TICKS(30000));
        continue;
      }
      connectStartedMs = millisSinceBoot();
    }

    // --- Zustand prüfen: ist der Handshake mit der Gegenstelle erfolgt? ---
    bool peerUp = esp_wireguard_peer_is_up(&wireguardContext) == ESP_OK;
    if (peerUp != tunnelUp) {
      tunnelUp = peerUp;
      if (peerUp) {
        tunnelUpSinceMs = millisSinceBoot();
        lastErrorMessage[0] = 0;
        ESP_LOGI(TAG, "Tunnel verbunden (Handshake erfolgreich)");
      } else {
        connectStartedMs = millisSinceBoot();
        ESP_LOGW(TAG, "Tunnel getrennt - warte auf neuen Handshake");
      }
    }
    tunnelState = peerUp ? "verbunden" : "warte auf Handshake";
    time_t handshakeTime = 0;
    if (esp_wireguard_latest_handshake(&wireguardContext, &handshakeTime) == ESP_OK) lastHandshakeTime = handshakeTime;

    // Tunnel läuft oder seit 2 min kein Absturz: Schutz vor Absturzschleifen zurücksetzen
    if (!startConsideredStable && (peerUp || millisSinceBoot() - taskStartedMs > STABLE_AFTER_START_MS)) {
      startConsideredStable = true;
      diagnosticsVpnStartSucceeded();
    }

    // Kein Handshake über längere Zeit: Verbindung neu starten (z. B. neue öffentliche IP der Gegenstelle)
    if (!peerUp && millisSinceBoot() - connectStartedMs > RECONNECT_AFTER_DOWN_MS) {
      setError("Kein Handshake - Verbindung wird neu gestartet (Schlüssel/Endpunkt prüfen)");
      esp_netif_tcpip_exec(disconnectInNetworkTask, nullptr);
      connected = false;
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(STATUS_CHECK_INTERVAL_MS));
  }
}

void wireguardStart() {
  if (!g_config.wgEnabled) return;
  if (diagnosticsVpnStartBlocked()) {
    tunnelState = "nach Abstürzen deaktiviert";
    setError("Der Tunnelaufbau hat wiederholt zum Absturz geführt - bitte Einstellungen prüfen und erneut speichern");
    return;
  }
  xTaskCreate(wireguardTask, "wireguard", 8192, nullptr, 4, nullptr);
}

void wireguardStatusToJson(cJSON* statusObject) {
  cJSON_AddBoolToObject(statusObject, "enabled", g_config.wgEnabled);
  cJSON_AddStringToObject(statusObject, "state", g_config.wgEnabled ? tunnelState : "aus");
  cJSON_AddBoolToObject(statusObject, "up", tunnelUp);
  cJSON_AddNumberToObject(statusObject, "upSince", tunnelUp ? (int)((millisSinceBoot() - tunnelUpSinceMs) / 1000) : -1);
  cJSON_AddStringToObject(statusObject, "address", g_config.wgAddress);
  std::string endpoint = std::string(g_config.wgEndpoint) + ":" + std::to_string(g_config.wgEndpointPort);
  cJSON_AddStringToObject(statusObject, "endpoint", endpoint.c_str());
  cJSON_AddStringToObject(statusObject, "allowedIps", g_config.wgAllowedIps);
  cJSON_AddStringToObject(statusObject, "lastError", lastErrorMessage);
  cJSON_AddBoolToObject(statusObject, "timeValid", timeIsValid());
  cJSON_AddNumberToObject(statusObject, "handshakeAgo", lastHandshakeTime ? (double)(time(nullptr) - lastHandshakeTime) : -1);
}

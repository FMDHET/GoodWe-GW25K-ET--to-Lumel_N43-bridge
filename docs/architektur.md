# Architektur

Überblick über Aufbau und Datenfluss der Firmware. Reines ESP-IDF 5.5 (FreeRTOS), Code in `src/`.

![Systemübersicht](images/overview.svg)

## Datenfluss

```
               ModbusMaster (Schnittstelle)
              ┌──────────────┴──────────────┐
          RtuPort                    ModbusTcpClient
       (RS485, Master)        (Gateway, ggf. über WireGuard)
              └──────────────┬──────────────┘
                             │  Mutex (geteilt mit der TCP-Bridge)
                     goodwe-Task (Polling)
                             │
              g_goodweRegisters (Rohregister je Block)
              g_meterData / g_goodweInfo (dekodiert)
          ┌──────────┬───────┴───────┬──────────────┐
      lumel-Task   Web (/api)      MQTT          TCP-Bridge
   (RS485, Slave)                               (reicht Anfragen
                                                 direkt durch)
```

- **Abfrage:** Der GoodWe-Task kennt nur die Schnittstelle `ModbusMaster` (`readRegisters`,
  `transact`). Ob der GoodWe per RS485 (`RtuPort`) oder per Modbus TCP (`ModbusTcpClient`) angebunden
  ist, entscheidet `main.cpp` beim Start.
- **Geteilte Daten:** Die Rohregister liegen in `g_goodweRegisters`, die dekodierten Werte in
  `g_meterData` (für Lumel) und `g_goodweInfo` (Übersicht). Zugriff nur unter `sharedLock()`.
- **Sensortabelle:** `GOODWE_SENSORS` in `goodwe_sensors.cpp` beschreibt jeden Wert (ID, Register,
  Typ, Teiler, Einheit, HA-Klassen). Web-Tabelle, MQTT und Home-Assistant-Discovery nutzen sie.
  Daraus erzeugt: [goodwe_et_register.md](goodwe_et_register.md).

## Abfrageintervalle

![Reiter GoodWe mit Intervall-Zuordnung](images/goodwe.png)

| | Intervall 1 (schnell) | Intervall 2 (langsam) |
|---|---|---|
| Takt | `gwPollMs` (Standard 1000 ms) | `gwSlowPollMs` (Standard 10 000 ms) |
| Gelesen | nur Register der Werte in Gruppe 1 | alle Blöcke vollständig |
| Gruppe 1 | vom Benutzer gewählte Werte + Werte für die Lumel-Simulation (fest) | – |

- **Wenige Anfragen:** Für Intervall 1 baut `buildFastRanges()` je Block möglichst wenige Bereiche.
  Lücken bis 40 Register werden mitgelesen, weil eine zusätzliche Anfrage 100–300 ms kostet, ein
  zusätzliches Register bei 9600 Baud nur ~2 ms.
- **Nur Gruppe 1 übernehmen:** Aus mitgelesenen Lücken übernimmt `readFastRanges()` nur die Register
  der Gruppe 1. Werte aus Intervall 2 ändern sich daher nur im Takt von Intervall 2.
- **Ausfälle:** Blöcke, die mit Exception antworten, gelten als nicht vorhanden und werden nach 60 s
  erneut versucht. Erst nach 3 Fehlzyklen in Folge gelten die Daten als ungültig.
- **Abgeschaltete Geräte:** Abgeschaltete Geräte (Smart-Meter, Batterien, Wechselrichter) werden nicht
  abgefragt. `goodweSensorRead()` blendet ihre Werte überall aus.

## Tasks

| Task | Datei | Aufgabe |
|---|---|---|
| `goodwe` | goodwe.cpp | Polling beider Intervalle, Dekodierung, geteilte Daten |
| `lumel` | lumel.cpp | Lumel-N43-Slave: beantwortet Anfragen aus `g_meterData` |
| `rtuN_rx` | modbus_rtu.cpp | Frame-Erkennung je RS485-Port (t1,5 / t3,5, CRC, Fehlerzähler) |
| `mb_bridge` | modbus_tcp_server.cpp | Modbus-TCP-Server, reicht PDUs an den GoodWe durch |
| `mqtt_pub` | mqtt.cpp | zyklisches Publizieren, HA-Discovery |
| `wireguard` | wireguard_vpn.cpp | Tunnelaufbau nach OTA-Bestätigung, WLAN und NTP |
| `web_console` | web_console.cpp | verteilt das Log per WebSocket an die Browser |
| `dns` | dns_server.cpp | Captive-Portal-DNS im Access-Point-Modus |
| `identify` | modbus_rtu.cpp | kurzlebig: lässt die TXD-LED eines Ports 10 s blinken |
| `app_main` | main.cpp | Start, danach Überwachungsschleife (WLAN, OTA-Bewährung, Watchdog) |

Startreihenfolge in `app_main()`: Log-Puffer → Diagnose (Reset-Grund, Coredump) → NVS → Netzwerk-Stack
→ OTA → Konfiguration → RS485-Ports → WLAN → NTP → WireGuard → GoodWe über TCP → TCP-Bridge →
Webserver → MQTT → Konsole.

## Module

| Datei | Inhalt |
|---|---|
| `chip.h` | chipabhängige Werte (ESP32-C6 / -C3): GPIOs, Standard-Pins, Sendeleistung, Log-Puffer |
| `config.*` | Einstellungen als JSON im NVS, Validierung, Zertifikate |
| `modbus_master.h` | gemeinsame Master-Schnittstelle |
| `modbus_rtu.*` | RS485-Treiber nach *MODBUS over Serial Line V1.02* (Master und Slave) |
| `modbus_tcp.*` | Modbus-TCP-Client (MBAP, Transaktions-IDs, Wiederverbindung) |
| `modbus_tcp_server.*` | transparente Modbus-TCP-Bridge zum GoodWe |
| `goodwe.*`, `goodwe_sensors.*` | Polling, Dekodierung, Sensortabelle |
| `lumel.*` | Lumel-N43-Simulation ([lumel_n43_register.md](lumel_n43_register.md)) |
| `net.*`, `dns_server.*` | WLAN (Station + Access-Point), mDNS, Captive Portal, Sendeleistung |
| `wireguard_vpn.*`, `time_sync.*` | WireGuard-Client, NTP |
| `web.*`, `web_ui.h`, `web_console.*` | HTTP-API, Weboberfläche (eine Seite), Live-Konsole |
| `mqtt.*` | MQTT/MQTTS, Home-Assistant-Discovery |
| `ota.*`, `diagnostics.*` | OTA mit Rollback, Reset-Grund, Coredump, Absturzschutz |
| `console.*`, `log_buffer.*` | USB-/Web-Konsole, Log-Ringpuffer |

## Verdrahtung

![Verdrahtung RS485](images/wiring.svg)

## HTTP-API (Auswahl)

| Pfad | Methode | Inhalt |
|---|---|---|
| `/api/status` | GET | Gesamtstatus (Ports, GoodWe, Zähler, WLAN, MQTT, VPN, OTA, Heap) |
| `/api/goodwe` | GET | alle GoodWe-Werte inkl. Intervall-Zuordnung (gestreamt) |
| `/api/lumel` | GET | Register, die die Lumel-Simulation ausgibt |
| `/api/config` | GET/POST | Einstellungen lesen / (teilweise) ändern |
| `/api/update` | POST | Firmware-Image (OTA) |
| `/api/identify?port=1` | POST | TXD-LED eines RS485-Moduls 10 s blinken lassen |
| `/api/reboot` | POST | Neustart |
| `/ws/console` | WebSocket | Live-Log und Konsolenbefehle |

## Speicher (ESP32-C3)

Der C3 hat nur ca. 230 kB Heap. Große Antworten (`/api/goodwe`) werden deshalb gestreamt
(`httpd_resp_send_chunk`). `sdkconfig.defaults.esp32c3` aktiviert dynamische TLS-Puffer und
reduziert die WLAN-Puffer, der Log-Puffer ist 8 kB statt 24 kB groß.

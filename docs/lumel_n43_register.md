# Lumel N43 – Modbus-RTU-Registerbelegung

Quelle: *Rail mounted power network meter N43 – User's manual* (Lumel, Rev. A, Tabellen 6–10),
gegengeprüft am 03.10.2026 mit einem echten N43 über einen USB-RS485-Adapter.

Die Firmware zeigt alle Register, die sie als Lumel ausgibt, live auf dem Reiter *Lumel*:

![Reiter Lumel](images/lumel.png)

## Schnittstelle

| Parameter | Standard | Register |
|---|---|---|
| Adresse | 1 | 4039 (1…247) |
| Format | **8N2** | 4040: 0 = 8N2, 1 = 8E1, 2 = 8O1, 3 = 8N1 |
| Baudrate | 9600 | 4041: 0 = 4800, 1 = 9600, 2 = 19200, 3 = 38400 |
| Funktionen | 03 / 04 lesen, 06 / 16 schreiben (nur 4000-Bereich) | |

Werte außerhalb des Messbereichs bzw. nicht definierte Werte (z. B. Leistungsfaktor ohne Strom,
THD bei zu kleiner Spannung) liefert das Gerät als **1e20** (Float `0x60AD78EC`).

## Registerbereiche

| Bereich | Typ | Inhalt |
|---|---|---|
| 4000–4066 | 16 Bit Integer, RW/R | Konfiguration, Energiezähler (100 Wh), Status, Seriennummer |
| 4300–4386 | 16 Bit Integer, RW | Konfiguration der Anzeigeseiten |
| 6000–6129 | Float, 2 × 16 Bit, **Wortreihenfolge 1-0-3-2** (Low-Word zuerst) | wie 7500–7564 |
| 7000–7129 | Float, 2 × 16 Bit, **Wortreihenfolge 3-2-1-0** (High-Word zuerst) | wie 7500–7564 |
| 7500–7564 | Float, **ein 32-Bit-Register je Wert** (4 Byte pro Register in der Antwort) | Messwerte |

Am echten Gerät antworten 7500–7574 (7565–7574 nicht dokumentiert); ab 7575 Exception 02.
Adressumrechnung: 32-Bit-Register `7500 + n` ⇔ 16-Bit-Paar `7000 + 2n` bzw. `6000 + 2n`.

## Messwerte (7500 + n)

| Register | n | Inhalt | Einheit |
|---|---|---|---|
| 7500 | 0 | Spannung L1 | V |
| 7501 | 1 | Strom L1 | A |
| 7502 | 2 | Wirkleistung L1 | W |
| 7503 | 3 | Blindleistung L1 | var |
| 7504 | 4 | Scheinleistung L1 | VA |
| 7505 | 5 | Leistungsfaktor L1 (P1/S1) | – |
| 7506 | 6 | tgφ L1 (Q1/P1) | – |
| 7507 | 7 | THD U1 | % |
| 7508 | 8 | THD I1 | % |
| 7509–7517 | 9–17 | wie oben für L2 | |
| 7518–7526 | 18–26 | wie oben für L3 | |
| 7527 | 27 | mittlere Spannung 3-phasig | V |
| 7528 | 28 | mittlerer Strom 3-phasig | A |
| 7529 | 29 | Wirkleistung 3-phasig (P1+P2+P3) | W |
| 7530 | 30 | Blindleistung 3-phasig | var |
| 7531 | 31 | Scheinleistung 3-phasig | VA |
| 7532 | 32 | Leistungsfaktor 3-phasig (P/S) | – |
| 7533 | 33 | mittlerer tgφ (Q/P) | – |
| 7534 | 34 | Frequenz | Hz |
| 7535 | 35 | Leiterspannung L1-L2 | V |
| 7536 | 36 | Leiterspannung L2-L3 | V |
| 7537 | 37 | Leiterspannung L3-L1 | V |
| 7538 | 38 | mittlere Leiterspannung | V |
| 7539 | 39 | Wirkleistung gemittelt (P Demand) | W |
| 7540 | 40 | Scheinleistung gemittelt (S Demand) | VA |
| 7541 | 41 | Strom gemittelt (I Demand) | A |
| 7542 | 42 | THD U Mittelwert | % |
| 7543 | 43 | THD I Mittelwert | % |
| 7544 | 44 | Neutralleiterstrom (berechnet) | A |
| 7545 | 45 | Wirkenergie Bezug – Überläufe von 7546 | × 100 MWh |
| 7546 | 46 | Wirkenergie Bezug – Zähler (bis 99999,9) | kWh |
| 7547 | 47 | Wirkenergie Lieferung – Überläufe | × 100 MWh |
| 7548 | 48 | Wirkenergie Lieferung – Zähler | kWh |
| 7549 / 7550 | 49 / 50 | Blindenergie induktiv (Überläufe / Zähler) | 100 Mvarh / kvarh |
| 7551 / 7552 | 51 / 52 | Blindenergie kapazitiv (Überläufe / Zähler) | 100 Mvarh / kvarh |
| 7553 / 7554 | 53 / 54 | Scheinenergie (Überläufe / Zähler) | 100 MVAh / kVAh |
| 7555 | 55 | Uhrzeit – Sekunden | |
| 7556 | 56 | Uhrzeit – Stunden,Minuten (z. B. 6.24) | |
| 7557 | 57 | reserviert (am Gerät: Monat.Tag, z. B. 10.03) | |
| 7558 | 58 | reserviert (am Gerät: Jahr, z. B. 2026) | |
| 7559 | 59 | mittlerer Strom 3-phasig (max) | A |
| 7560 | 60 | max. Spannung (Phase bzw. Leiter) | V |
| 7561 | 61 | P Demand min | W |
| 7562 | 62 | P Demand max | W |
| 7563 | 63 | S Demand max | VA |
| 7564 | 64 | I Demand max | A |

Energie gesamt = Überläufe × 100 000 kWh + Zählerwert.

## Konfiguration und Zähler (4000–4066, 16 Bit)

| Register | Zugriff | Inhalt | Standard |
|---|---|---|---|
| 4000 | RW | Passwort (0…30000) | 0 |
| 4003 | RW | Anschluss: 0 = 3Ph/4W, 1 = 3Ph/3W | 0 |
| 4004 | RW | Stromeingang: 0 = 1 A, 1 = 5 A (bzw. 63-A-Variante) | 1 |
| 4005 | RW | Stromwandler-Übersetzung (1…10000) | 1 |
| 4006 | RW | Spannungswandler-Übersetzung × 10 (1…40000) | 10 |
| 4007 | RW | Mittelungszeit P/S/I: 0 = 15, 1 = 30, 2 = 60 min | 0 |
| 4008 | RW | Synchronisation mit Uhr | 1 |
| 4010 | RW | Energiezähler löschen (1 Wirk, 2 Blind, 3 Schein, 4 alle) | 0 |
| 4011 / 4012 / 4013 | RW | Demand / Min-Max / Alarm-Speicher löschen | 0 |
| 4014–4037 | RW | Alarmausgänge 1–3 (Größe, Typ, Schwellen in ‰, Verzögerungen) | |
| 4038 | RW | Impulse für Impulsausgang (100…20000) | 1000 |
| 4039 | RW | Modbus-Adresse | 1 |
| 4040 | RW | Format (0 = 8N2, 1 = 8E1, 2 = 8O1, 3 = 8N1) | 0 |
| 4041 | RW | Baudrate (0 = 4800, 1 = 9600, 2 = 19200, 3 = 38400) | 1 |
| 4042 | RW | neue Schnittstellenparameter übernehmen | 0 |
| 4043 | RW | Werkseinstellungen (löscht Energien, Min/Max) | 0 |
| 4045 | RW | Uhrzeit Stunde × 100 + Minute | |
| 4048 / 4049 | R | Wirkenergie Bezug (High/Low-Word) | × 100 Wh |
| 4050 / 4051 | R | Wirkenergie Lieferung (High/Low-Word) | × 100 Wh |
| 4052 / 4053 | R | Blindenergie induktiv | × 100 varh |
| 4054 / 4055 | R | Blindenergie kapazitiv | × 100 varh |
| 4056 / 4057 | R | Scheinenergie | × 100 VAh |
| 4058 / 4059 | R | Statusregister 1 / 2 | |
| 4061 / 4062 | R | Seriennummer (High/Low-Word) | |
| 4063 | R | Softwareversion × 100 | |

Energie aus den 16-Bit-Registern: `(High × 65536 + Low) / 10` kWh.

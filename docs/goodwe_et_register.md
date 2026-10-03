# GoodWe ET – Modbus-Registerbelegung

Registerbelegung, wie die Firmware sie liest (ARM-Protokoll der ET/EH/BT/BH-Serie, vgl. Python-Bibliothek
[goodwe](https://github.com/marcelblijleven/goodwe), `et.py`). Geprüft an einem GW25K-ET.
Diese Tabelle wird aus `GOODWE_SENSORS` in [src/goodwe_sensors.cpp](../src/goodwe_sensors.cpp) erzeugt.

## Schnittstelle

| Parameter | Wert |
|---|---|
| Modbus-Adresse (Unit-ID) | 247 |
| Format | 9600 Baud, 8N1 |
| Funktion | FC03 (Holding Register lesen) |
| 32-Bit-Werte | höherwertiges Wort zuerst |

Physikalischer Wert = Rohwert / Teiler. Die **ID** ist zugleich der MQTT-/JSON-Schlüssel.

## Besonderheiten (am GW25K-ET geprüft)

- Modellname steht in 35060–35067 (ältere Firmware: 35011–35015), Seriennummer in 35003–35010, Firmware in 35021–35032.
- Blindleistung 35141 und Scheinleistung 35143 sind je 32 Bit (S32).
- Die Energiezähler des Smart-Meters (36015/36017, Float in Wh) liefern bei manchen Firmwares immer 0;
  die Bridge verwendet dann Einspeisung 35195 und Bezug 35200 des Wechselrichters.
- Batterie 2 liegt außerhalb des Wechselrichterblocks (35262 ff.), ihr BMS bei 39000 ff.
- Blöcke, die mit Exception antworten (z. B. keine zweite Batterie), werden erst nach 60 s erneut versucht.
- Eine Wallbox (z. B. HCA G2) am GoodWe wird nicht durchgereicht (Register 10000 → Exception 02).

## Geräteinfo (35000 ×68)

| Register | ID | Bezeichnung | Typ | Teiler | Einheit |
|---|---|---|---|---|---|
| 35001 | `rated_power` | Nennleistung | U16 | 1 | W |

## Wechselrichter (35100 ×125)

| Register | ID | Bezeichnung | Typ | Teiler | Einheit |
|---|---|---|---|---|---|
| 35103 | `vpv1` | PV1 Spannung | U16 | 10 | V |
| 35104 | `ipv1` | PV1 Strom | U16 | 10 | A |
| 35105 | `ppv1` | PV1 Leistung | U32 | 1 | W |
| 35107 | `vpv2` | PV2 Spannung | U16 | 10 | V |
| 35108 | `ipv2` | PV2 Strom | U16 | 10 | A |
| 35109 | `ppv2` | PV2 Leistung | U32 | 1 | W |
| 35111 | `vpv3` | PV3 Spannung | U16 | 10 | V |
| 35112 | `ipv3` | PV3 Strom | U16 | 10 | A |
| 35113 | `ppv3` | PV3 Leistung | U32 | 1 | W |
| 35115 | `vpv4` | PV4 Spannung | U16 | 10 | V |
| 35116 | `ipv4` | PV4 Strom | U16 | 10 | A |
| 35117 | `ppv4` | PV4 Leistung | U32 | 1 | W |
| 35119 | `pv_mode` | PV Modus | U16 | 1 |  |
| 35121 | `vgrid1` | Netz L1 Spannung | U16 | 10 | V |
| 35122 | `igrid1` | Netz L1 Strom | U16 | 10 | A |
| 35123 | `fgrid1` | Netz L1 Frequenz | U16 | 100 | Hz |
| 35124 | `pgrid1` | Netz L1 Leistung | S32 | 1 | W |
| 35126 | `vgrid2` | Netz L2 Spannung | U16 | 10 | V |
| 35127 | `igrid2` | Netz L2 Strom | U16 | 10 | A |
| 35128 | `fgrid2` | Netz L2 Frequenz | U16 | 100 | Hz |
| 35129 | `pgrid2` | Netz L2 Leistung | S32 | 1 | W |
| 35131 | `vgrid3` | Netz L3 Spannung | U16 | 10 | V |
| 35132 | `igrid3` | Netz L3 Strom | U16 | 10 | A |
| 35133 | `fgrid3` | Netz L3 Frequenz | U16 | 100 | Hz |
| 35134 | `pgrid3` | Netz L3 Leistung | S32 | 1 | W |
| 35136 | `grid_mode` | Netzstatus | U16 | 1 | Statuscode |
| 35137 | `total_inverter_power` | Wechselrichter Leistung | S32 | 1 | W |
| 35139 | `active_power` | Netzleistung | S32 | 1 | W |
| 35141 | `reactive_power` | Blindleistung | S32 | 1 | var |
| 35143 | `apparent_power` | Scheinleistung | S32 | 1 | VA |
| 35145 | `backup_v1` | Backup L1 Spannung | U16 | 10 | V |
| 35146 | `backup_i1` | Backup L1 Strom | U16 | 10 | A |
| 35147 | `backup_f1` | Backup L1 Frequenz | U16 | 100 | Hz |
| 35148 | `load_mode1` | Backup L1 Modus | U16 | 1 | Statuscode |
| 35149 | `backup_p1` | Backup L1 Leistung | S32 | 1 | W |
| 35151 | `backup_v2` | Backup L2 Spannung | U16 | 10 | V |
| 35152 | `backup_i2` | Backup L2 Strom | U16 | 10 | A |
| 35153 | `backup_f2` | Backup L2 Frequenz | U16 | 100 | Hz |
| 35154 | `load_mode2` | Backup L2 Modus | U16 | 1 | Statuscode |
| 35155 | `backup_p2` | Backup L2 Leistung | S32 | 1 | W |
| 35157 | `backup_v3` | Backup L3 Spannung | U16 | 10 | V |
| 35158 | `backup_i3` | Backup L3 Strom | U16 | 10 | A |
| 35159 | `backup_f3` | Backup L3 Frequenz | U16 | 100 | Hz |
| 35160 | `load_mode3` | Backup L3 Modus | U16 | 1 | Statuscode |
| 35161 | `backup_p3` | Backup L3 Leistung | S32 | 1 | W |
| 35163 | `load_p1` | Last L1 | S32 | 1 | W |
| 35165 | `load_p2` | Last L2 | S32 | 1 | W |
| 35167 | `load_p3` | Last L3 | S32 | 1 | W |
| 35169 | `backup_ptotal` | Backup Leistung gesamt | S32 | 1 | W |
| 35171 | `load_ptotal` | Last gesamt | S32 | 1 | W |
| 35173 | `ups_load` | Backup Auslastung | U16 | 1 | % |
| 35174 | `temperature_air` | Temperatur Luft | S16 | 10 | °C |
| 35175 | `temperature_module` | Temperatur Modul | S16 | 10 | °C |
| 35176 | `temperature` | Temperatur Kühlkörper | S16 | 10 | °C |
| 35177 | `function_bit` | Funktionsbits | U16 | 1 |  |
| 35178 | `bus_voltage` | Busspannung | U16 | 10 | V |
| 35179 | `nbus_voltage` | NBus-Spannung | U16 | 10 | V |
| 35180 | `vbattery1` | Batterie 1 Spannung | U16 | 10 | V |
| 35181 | `ibattery1` | Batterie 1 Strom | S16 | 10 | A |
| 35182 | `pbattery1` | Batterie 1 Leistung | S32 | 1 | W |
| 35184 | `battery_mode` | Batterie 1 Modus | U16 | 1 | Statuscode |
| 35185 | `warning_code` | Warncode | U16 | 1 |  |
| 35186 | `safety_country` | Ländernorm | U16 | 1 |  |
| 35187 | `work_mode` | Betriebszustand | U16 | 1 | Statuscode |
| 35188 | `operation_mode` | Betriebsmodus | U16 | 1 |  |
| 35189 | `error_codes` | Fehlercodes | U32 | 1 |  |
| 35191 | `e_total` | PV Erzeugung gesamt | U32 | 10 | kWh |
| 35193 | `e_day` | PV Erzeugung heute | U32 | 10 | kWh |
| 35195 | `e_total_exp` | Einspeisung gesamt | U32 | 10 | kWh |
| 35197 | `h_total` | Betriebsstunden | U32 | 1 | h |
| 35199 | `e_day_exp` | Einspeisung heute | U16 | 10 | kWh |
| 35200 | `e_total_imp` | Bezug gesamt | U32 | 10 | kWh |
| 35202 | `e_day_imp` | Bezug heute | U16 | 10 | kWh |
| 35203 | `e_load_total` | Verbrauch gesamt | U32 | 10 | kWh |
| 35205 | `e_load_day` | Verbrauch heute | U16 | 10 | kWh |
| 35206 | `e_bat_charge_total` | Batterie Ladung gesamt | U32 | 10 | kWh |
| 35208 | `e_bat_charge_day` | Batterie Ladung heute | U16 | 10 | kWh |
| 35209 | `e_bat_discharge_total` | Batterie Entladung gesamt | U32 | 10 | kWh |
| 35211 | `e_bat_discharge_day` | Batterie Entladung heute | U16 | 10 | kWh |
| 35220 | `diagnose_result` | Diagnose | U32 | 1 |  |

## Batterie 2 (35262 ×5)

| Register | ID | Bezeichnung | Typ | Teiler | Einheit |
|---|---|---|---|---|---|
| 35262 | `vbattery2` | Batterie 2 Spannung | U16 | 10 | V |
| 35263 | `ibattery2` | Batterie 2 Strom | S16 | 10 | A |
| 35264 | `pbattery2` | Batterie 2 Leistung | S32 | 1 | W |
| 35266 | `battery2_mode` | Batterie 2 Modus | U16 | 1 | Statuscode |

## Smart-Meter (36000 ×58, ältere Firmware ×45)

| Register | ID | Bezeichnung | Typ | Teiler | Einheit |
|---|---|---|---|---|---|
| 36004 | `meter_comm_status` | Meter Kommunikation | U16 | 1 |  |
| 36010 | `meter_pf1` | Meter L1 Leistungsfaktor | S16 | 1000 |  |
| 36011 | `meter_pf2` | Meter L2 Leistungsfaktor | S16 | 1000 |  |
| 36012 | `meter_pf3` | Meter L3 Leistungsfaktor | S16 | 1000 |  |
| 36013 | `meter_pf` | Meter Leistungsfaktor | S16 | 1000 |  |
| 36014 | `meter_freq` | Meter Frequenz | U16 | 100 | Hz |
| 36015 | `meter_e_total_exp` | Meter Einspeisung gesamt | Float32 | 1000 | kWh |
| 36017 | `meter_e_total_imp` | Meter Bezug gesamt | Float32 | 1000 | kWh |
| 36019 | `meter_p1` | Meter L1 Leistung | S32 | 1 | W |
| 36021 | `meter_p2` | Meter L2 Leistung | S32 | 1 | W |
| 36023 | `meter_p3` | Meter L3 Leistung | S32 | 1 | W |
| 36025 | `meter_p` | Meter Leistung gesamt | S32 | 1 | W |
| 36027 | `meter_q1` | Meter L1 Blindleistung | S32 | 1 | var |
| 36029 | `meter_q2` | Meter L2 Blindleistung | S32 | 1 | var |
| 36031 | `meter_q3` | Meter L3 Blindleistung | S32 | 1 | var |
| 36033 | `meter_q` | Meter Blindleistung gesamt | S32 | 1 | var |
| 36035 | `meter_s1` | Meter L1 Scheinleistung | S32 | 1 | VA |
| 36037 | `meter_s2` | Meter L2 Scheinleistung | S32 | 1 | VA |
| 36039 | `meter_s3` | Meter L3 Scheinleistung | S32 | 1 | VA |
| 36041 | `meter_s` | Meter Scheinleistung gesamt | S32 | 1 | VA |
| 36043 | `meter_type` | Meter Typ | U16 | 1 |  |
| 36044 | `meter_sw_version` | Meter SW-Version | U16 | 1 |  |
| 36052 | `meter_v1` | Meter L1 Spannung | U16 | 10 | V |
| 36053 | `meter_v2` | Meter L2 Spannung | U16 | 10 | V |
| 36054 | `meter_v3` | Meter L3 Spannung | U16 | 10 | V |
| 36055 | `meter_i1` | Meter L1 Strom | U16 | 10 | A |
| 36056 | `meter_i2` | Meter L2 Strom | U16 | 10 | A |
| 36057 | `meter_i3` | Meter L3 Strom | U16 | 10 | A |

## BMS Batterie 1 (37000 ×24)

| Register | ID | Bezeichnung | Typ | Teiler | Einheit |
|---|---|---|---|---|---|
| 37000 | `battery_bms` | BMS 1 Typ | U16 | 1 |  |
| 37002 | `battery_status` | BMS 1 Status | U16 | 1 |  |
| 37003 | `battery_temperature` | Batterie 1 Temperatur | S16 | 10 | °C |
| 37004 | `battery_charge_limit` | Batterie 1 Ladestromgrenze | U16 | 1 | A |
| 37005 | `battery_discharge_limit` | Batterie 1 Entladestromgrenze | U16 | 1 | A |
| 37006 | `battery_error` | BMS 1 Fehler | U16 | 1 |  |
| 37007 | `battery_soc` | Batterie 1 Ladezustand | U16 | 1 | % |
| 37008 | `battery_soh` | Batterie 1 Gesundheit | U16 | 1 | % |
| 37009 | `battery_modules` | Batterie 1 Module | U16 | 1 |  |
| 37010 | `battery_warning` | BMS 1 Warnung | U16 | 1 |  |
| 37011 | `battery_protocol` | BMS 1 Protokoll | U16 | 1 |  |

## BMS Batterie 2 (39000 ×22)

| Register | ID | Bezeichnung | Typ | Teiler | Einheit |
|---|---|---|---|---|---|
| 39000 | `battery2_status` | BMS 2 Status | U16 | 1 |  |
| 39001 | `battery2_temperature` | Batterie 2 Temperatur | S16 | 10 | °C |
| 39002 | `battery2_charge_limit` | Batterie 2 Ladestromgrenze | U16 | 1 | A |
| 39003 | `battery2_discharge_limit` | Batterie 2 Entladestromgrenze | U16 | 1 | A |
| 39004 | `battery2_error_l` | BMS 2 Fehler | U16 | 1 |  |
| 39005 | `battery2_soc` | Batterie 2 Ladezustand | U16 | 1 | % |
| 39006 | `battery2_soh` | Batterie 2 Gesundheit | U16 | 1 | % |
| 39007 | `battery2_modules` | Batterie 2 Module | U16 | 1 |  |
| 39008 | `battery2_warning_l` | BMS 2 Warnung | U16 | 1 |  |
| 39009 | `battery2_protocol` | BMS 2 Protokoll | U16 | 1 |  |
| 39012 | `battery2_sw_version` | BMS 2 Softwareversion | U16 | 1 |  |

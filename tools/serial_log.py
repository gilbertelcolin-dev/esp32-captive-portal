import serial, sys, time, datetime
port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
s = serial.Serial(port, 115200, timeout=1)
s.setDTR(False); s.setRTS(False); time.sleep(0.2)
while True:
    try:
        line = s.readline()
    except Exception as e:
        print("ERR", e, flush=True); time.sleep(1); continue
    if line:
        print(f"{datetime.datetime.now():%H:%M:%S} {line.decode('utf8','replace').rstrip()}", flush=True)

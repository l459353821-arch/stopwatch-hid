import serial, time, struct, statistics
PORT = '/dev/cu.usbmodem2101'
total = bytearray()
start = time.time()
while time.time() - start < 90:
    try:
        s = serial.Serial(PORT, 115200, timeout=0.5)
    except Exception:
        time.sleep(1)
        continue
    t0 = time.time()
    while time.time() - t0 < 1.0:
        c = s.read(4096)
        if c:
            total.extend(c)
    s.close()
    if len(total) > 2000:
        break
print('total bytes:', len(total))
if len(total) >= 2:
    vals = struct.unpack('<%dh' % (len(total)//2), total[:len(total)//2*2])
    print('min', min(vals), 'max', max(vals), 'mean_abs', int(statistics.mean([abs(v) for v in vals])))
    print('nonzero %.1f%%' % (100*sum(1 for v in vals if v)/max(1, len(vals))))
print('PROBE_DONE')

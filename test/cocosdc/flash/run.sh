# Program bank 3 of the flash, then check the saved flash file
xr -cart-opt sdc-flash=flash.bin -type "$TYPE"
out_txt
python3 -I - flash.bin <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
b = d[3 * 16384:4 * 16384]
print('flash', len(d), 'bank 3', 'ok' if b[:256] == bytes(range(256)) and b[256:] == b'\xff' * (16384 - 256) else 'BAD')
PY

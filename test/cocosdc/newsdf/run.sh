xr -type "$TYPE"
out_txt
# The new image: header, size, and an empty formatted disk
python3 -I - sd/NEW.SDF <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
print('NEW.SDF', len(d), d[:8].hex())
PY
# Like the real controller, a failed N: leaves an empty file
echo "BAD.SDF $(stat -c %s sd/BAD.SDF)"

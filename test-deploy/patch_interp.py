import sys

src, dst, new_interp = sys.argv[1], sys.argv[2], sys.argv[3]
d = bytearray(open(src, 'rb').read())
i = d.find(b'/lib/ld-linux-aarch64.so.1')
if i < 0:
    print('interp string not found')
    sys.exit(1)
if len(new_interp) + 1 > 28:
    print('new interp too long')
    sys.exit(1)
d[i:i + len(new_interp) + 1] = new_interp.encode() + b'\x00'
open(dst, 'wb').write(d)
print('patched %s -> %s' % (src, dst))
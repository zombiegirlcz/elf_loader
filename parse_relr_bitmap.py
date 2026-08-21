relr_hex = "80d01a0000000000cf0ffcfe0000c02357b8e37fffffffff0f802ffcff09fcff070ef981ffffffff1f32e11f38030cfcc0fffffffffffffdffffffffffffffffffffffffff66dbbaffffffffffffffdfffbfffffffffffffffff7fffffffff00000000f8ffffff070000005087c30888a2aa02feff2bffffffffffffffffffffffffffff7f3333e7666e660280dffffff9ffffff3ff7ffeff9ffffff3ff7ffeff9ffffff3ff7ffeff9ffffff3ff7ffeff1f11002002001c010f3c0000000000d0fd1a0000000000030014000810390101000000000000c06ddbb601000000c0c562b1582c168bc5c562f1270000000401000001002410000100040000000000b0121b0000000000e123fdfeff7f00002510004002210000"
import struct
relr_data = bytes.fromhex(relr_hex)
words = [struct.unpack("<Q", relr_data[i:i+8])[0] for i in range(0, len(relr_data), 8)]

where = None
relocated_addresses = []
for i, entry in enumerate(words):
    if (entry & 1) == 0:
        where = entry
        relocated_addresses.append(where)
        # Note: Do we increment where by 8?
        where += 8
    else:
        bitmap = entry >> 1
        j = 0
        while bitmap > 0:
            if bitmap & 1:
                relocated_addresses.append(where + j * 8)
            bitmap >>= 1
            j += 1
        where += 63 * 8

for addr in sorted(relocated_addresses):
    if 0x1ad0a0 <= addr <= 0x1ad0e0:
        print(f"Relocated: {hex(addr)}")

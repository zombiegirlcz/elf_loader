#!/usr/bin/env python3
import sys
import os

OLD_INTERP = b"/lib/ld-linux-aarch64.so.1"
NEW_INTERP = b"/data/local/tmp/ldl"

def patch_interp(path, target_interp):
    with open(path, 'r+b') as f:
        data = f.read()
        # Find ELF header
        if data[:4] != b'\x7fELF':
            print(f"Not an ELF file: {path}")
            return False
        
        # Parse ELF header
        ei_class = data[4]
        if ei_class != 2:  # ELFCLASS64
            print(f"Not 64-bit ELF: {path}")
            return False
        
        # e_phoff at offset 0x20 (32), e_phnum at 0x38 (56), e_phentsize at 0x36 (54)
        e_phoff = int.from_bytes(data[0x20:0x28], 'little')
        e_phnum = int.from_bytes(data[0x38:0x3a], 'little')
        e_phentsize = int.from_bytes(data[0x36:0x38], 'little')
        
        # Iterate program headers
        for i in range(e_phnum):
            ph_off = e_phoff + i * e_phentsize
            p_type = int.from_bytes(data[ph_off:ph_off+4], 'little')
            if p_type == 3:  # PT_INTERP
                p_offset = int.from_bytes(data[ph_off+8:ph_off+16], 'little')
                p_filesz = int.from_bytes(data[ph_off+32:ph_off+40], 'little')
                
                # Read current interp
                current = data[p_offset:p_offset+p_filesz].rstrip(b'\x00')
                print(f"Current INTERP: {current.decode('ascii', errors='replace')}")
                
                # Check if it matches OLD_INTERP
                if current == b"/data/local/tmp/ldl":
                    target = b"/lib/ld-linux-aarch64.so.1"
                elif current == b"/lib/ld-linux-aarch64.so.1":
                    target = b"/data/local/tmp/ldl"
                else:
                    target = target_interp.encode() if isinstance(target_interp, str) else target_interp
                
                if len(target) + 1 > p_filesz:
                    print(f"Target INTERP too long: {len(target)+1} > {p_filesz}")
                    return False
                
                # Write new INTERP
                f.seek(p_offset)
                f.write(target + b'\x00')
                print(f"Patched INTERP to: {target.decode()}")
                return True
        
        print("PT_INTERP not found")
        return False

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python3 patch_interp.py <file> <target_interp>")
        sys.exit(1)
    
    path = sys.argv[1]
    target = sys.argv[2].encode() if len(sys.argv) > 2 else None
    
    if not os.path.exists(path):
        print(f"File not found: {path}")
        sys.exit(1)
    
    success = patch_interp(path, target)
    sys.exit(0 if success else 1)
import sys
import struct

# Let's read the .init_array section in libc.so.6 or check relocation maps
# We can read DT_RELR from readelf -rW or write a quick parser to confirm if 0x1ad0b0 was relocated by RELR.

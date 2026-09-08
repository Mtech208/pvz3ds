import struct, zlib

XOR_TABLE = bytes(b ^ 0xF7 for b in range(256))
with open('../PAQUETE_3DS/PlantsvsZombies/main.pak', 'rb') as f:
    f.seek(156019)
    data = f.read(276).translate(XOR_TABLE)
    decompressed = zlib.decompress(data[8:])

print('Total decompressed length:', len(decompressed))
print('From offset 650 to end:')
for i in range(650, len(decompressed), 4):
    chunk = decompressed[i:i+4]
    val_i, = struct.unpack('<i', chunk)
    val_f, = struct.unpack('<f', chunk)
    hex_str = ' '.join(f'{b:02x}' for b in chunk)
    print(f'Offset {i:4d}: hex=[{hex_str}] int={val_i:11d} float={val_f:12.4f}')

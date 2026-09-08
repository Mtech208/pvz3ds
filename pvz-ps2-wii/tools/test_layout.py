import struct, zlib

XOR_TABLE = bytes(b ^ 0xF7 for b in range(256))
with open('../PAQUETE_3DS/PlantsvsZombies/main.pak', 'rb') as f:
    f.seek(156019)
    data = f.read(276).translate(XOR_TABLE)
    decompressed = zlib.decompress(data[8:])

pos = 372
def read(n):
    global pos
    res = decompressed[pos:pos+n]
    pos += n
    return res

# Let's inspect the emitter struct at offset 16 (length 356)
emitter_raw = decompressed[16:16+356]
print('Emitter raw length:', len(emitter_raw))

# In C++, TodEmitterDefinition fields:
# Let's see the offset of mParticleFields and mSystemFields in TodEmitterDefinition!

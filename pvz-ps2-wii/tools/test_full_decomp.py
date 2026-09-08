import struct, zlib

XOR_TABLE = bytes(b ^ 0xF7 for b in range(256))
with open('../PAQUETE_3DS/PlantsvsZombies/main.pak', 'rb') as f:
    f.seek(156019)
    data = f.read(276).translate(XOR_TABLE)
    decompressed = zlib.decompress(data[8:])

pos = 0
def read(n):
    global pos
    res = decompressed[pos:pos+n]
    pos += n
    return res

crc, = struct.unpack('<I', read(4))
print(f'CRC: {hex(crc)}')
ptr, count = struct.unpack('<II', read(8))
print(f'Top level: ptr={hex(ptr)}, count={count}')
def_size, = struct.unpack('<I', read(4))
print(f'def_size: {def_size}')
emitter_bytes = read(def_size)

def read_float_track(name):
    cnt, = struct.unpack('<i', read(4))
    print(f'  {name:20}: (TRACK_FLOAT) nodes={cnt} [offset {pos}]')
    if cnt > 0:
        read(cnt * 20)

def read_string(name):
    l, = struct.unpack('<i', read(4))
    s = read(l).decode('latin1', errors='replace')
    print(f'  {name:20}: (STRING) len={l} str="{s}" [offset {pos}]')

def read_image(name):
    l, = struct.unpack('<i', read(4))
    s = read(l).decode('latin1', errors='replace')
    print(f'  {name:20}: (IMAGE) len={l} str="{s}" [offset {pos}]')

def read_particle_field(i):
    print(f'    Reading ParticleField {i} at offset {pos}:')
    # ParticleField has: FieldType (enum, not in stream), x (TRACK_FLOAT), y (TRACK_FLOAT)
    read_float_track('x')
    read_float_track('y')

# Emitter def fields
read_image('Image')
read_string('Name')
read_float_track('SystemDuration')
read_string('OnDuration')
read_float_track('CrossFadeDuration')
read_float_track('SpawnRate')
read_float_track('SpawnMinActive')
read_float_track('SpawnMaxActive')
read_float_track('SpawnMaxLaunched')
read_float_track('EmitterRadius')
read_float_track('EmitterOffsetX')
read_float_track('EmitterOffsetY')
read_float_track('EmitterBoxX')
read_float_track('EmitterBoxY')
read_float_track('EmitterPath')
read_float_track('EmitterSkewX')
read_float_track('EmitterSkewY')
read_float_track('ParticleDuration')
read_float_track('SystemRed')
read_float_track('SystemGreen')
read_float_track('SystemBlue')
read_float_track('SystemAlpha')
read_float_track('SystemBrightness')
read_float_track('LaunchSpeed')
read_float_track('LaunchAngle')

# Field (ARRAY of ParticleField, count=2)
sz, = struct.unpack('<i', read(4))
print(f'Field: def_size={sz} [offset {pos}]')
raw_pfield = read(2 * sz)
read_particle_field(0)
read_particle_field(1)

# SystemField (ARRAY of ParticleField, count=0)
sz, = struct.unpack('<i', read(4))
print(f'SystemField: def_size={sz} [offset {pos}]')

# Next fields:
read_float_track('ParticleRed')
read_float_track('ParticleGreen')
read_float_track('ParticleBlue')
read_float_track('ParticleAlpha')
read_float_track('ParticleBrightness')
read_float_track('ParticleSpinAngle')
read_float_track('ParticleSpinSpeed')
read_float_track('ParticleScale')
read_float_track('ParticleStretch')
read_float_track('CollisionReflect')
read_float_track('CollisionSpin')
read_float_track('ClipTop')
read_float_track('ClipBottom')
read_float_track('ClipLeft')
read_float_track('ClipRight')
read_float_track('AnimationRate')

print('FINISHED! Final pos:', pos, 'Total len:', len(decompressed))

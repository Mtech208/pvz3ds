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

fields = [
    ('Image', 'IMAGE'),
    ('ImageRow', 'INT'),
    ('ImageCol', 'INT'),
    ('ImageFrames', 'INT'),
    ('Animated', 'INT'),
    ('ParticleFlags', 'FLAGS'),
    ('EmitterType', 'ENUM'),
    ('Name', 'STRING'),
    ('SystemDuration', 'TRACK_FLOAT'),
    ('OnDuration', 'STRING'),
    ('CrossFadeDuration', 'TRACK_FLOAT'),
    ('SpawnRate', 'TRACK_FLOAT'),
    ('SpawnMinActive', 'TRACK_FLOAT'),
    ('SpawnMaxActive', 'TRACK_FLOAT'),
    ('SpawnMaxLaunched', 'TRACK_FLOAT'),
    ('EmitterRadius', 'TRACK_FLOAT'),
    ('EmitterOffsetX', 'TRACK_FLOAT'),
    ('EmitterOffsetY', 'TRACK_FLOAT'),
    ('EmitterBoxX', 'TRACK_FLOAT'),
    ('EmitterBoxY', 'TRACK_FLOAT'),
    ('EmitterPath', 'TRACK_FLOAT'),
    ('EmitterSkewX', 'TRACK_FLOAT'),
    ('EmitterSkewY', 'TRACK_FLOAT'),
    ('ParticleDuration', 'TRACK_FLOAT'),
    ('SystemRed', 'TRACK_FLOAT'),
    ('SystemGreen', 'TRACK_FLOAT'),
    ('SystemBlue', 'TRACK_FLOAT'),
    ('SystemAlpha', 'TRACK_FLOAT'),
    ('SystemBrightness', 'TRACK_FLOAT'),
    ('LaunchSpeed', 'TRACK_FLOAT'),
    ('LaunchAngle', 'TRACK_FLOAT'),
    ('Field', 'ARRAY'),
    ('SystemField', 'ARRAY'),
    ('ParticleRed', 'TRACK_FLOAT'),
    ('ParticleGreen', 'TRACK_FLOAT'),
    ('ParticleBlue', 'TRACK_FLOAT'),
    ('ParticleAlpha', 'TRACK_FLOAT'),
    ('ParticleBrightness', 'TRACK_FLOAT'),
    ('ParticleSpinAngle', 'TRACK_FLOAT'),
    ('ParticleSpinSpeed', 'TRACK_FLOAT'),
    ('ParticleScale', 'TRACK_FLOAT'),
    ('ParticleStretch', 'TRACK_FLOAT'),
    ('CollisionReflect', 'TRACK_FLOAT'),
    ('CollisionSpin', 'TRACK_FLOAT'),
    ('ClipTop', 'TRACK_FLOAT'),
    ('ClipBottom', 'TRACK_FLOAT'),
    ('ClipLeft', 'TRACK_FLOAT'),
    ('ClipRight', 'TRACK_FLOAT'),
    ('AnimationRate', 'TRACK_FLOAT'),
]

for name, ftype in fields:
    if ftype in ('IMAGE', 'FONT', 'STRING'):
        l, = struct.unpack('<i', read(4))
        s = read(l).decode('latin1', errors='replace')
        print(f'{name:20}: ({ftype}) len={l} str="{s}" [offset {pos}]')
    elif ftype == 'TRACK_FLOAT':
        cnt, = struct.unpack('<i', read(4))
        print(f'{name:20}: (TRACK_FLOAT) nodes={cnt} [offset {pos}]')
        if cnt > 0:
            nodes_data = read(cnt * 20)
    elif ftype == 'ARRAY':
        sz, = struct.unpack('<i', read(4))
        print(f'{name:20}: (ARRAY) def_size={sz} [offset {pos}]')
print('Finished fields! Final pos:', pos, 'Total len:', len(decompressed))

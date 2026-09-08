import subprocess, sys

targets = [0x32e9c4, 0x32e91c, 0x253190, 0x2c7afc]

# run nm
r = subprocess.run(
    [r'C:\devkitPro\devkitARM\bin\arm-none-eabi-nm', '--numeric-sort',
     r'build\3ds-release\re-plants-vs-zombies.elf'],
    capture_output=True, text=True, errors='ignore'
)

entries = []
for line in r.stdout.splitlines():
    parts = line.split()
    if len(parts) >= 3:
        try:
            addr = int(parts[0], 16)
            entries.append((addr, parts[1], ' '.join(parts[2:])))
        except:
            pass

for target in targets:
    # Find nearest symbol below target
    below = [(a, t, n) for a, t, n in entries if a <= target]
    if below:
        best = max(below, key=lambda x: x[0])
        print(f'0x{target:08X} -> +0x{target - best[0]:04X} from {best[2]} (0x{best[0]:08X})')
    else:
        print(f'0x{target:08X} -> not found')

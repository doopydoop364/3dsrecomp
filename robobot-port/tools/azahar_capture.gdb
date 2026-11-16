# Capture GPU submissions of one command list from Azahar (gdb stub on 127.0.0.1:24689).
# Run: gdb -batch -x tools/azahar_capture.gdb ; send SIGINT to gdb once the title has settled.
set pagination off
set architecture arm
target remote 127.0.0.1:24689
continue
python
import gdb, struct, os
OUT = os.environ.get('CAPTURE_DIR', 'build/ref/capture')
TRIGGER = 0x17edbc          # bl to the svc wrapper inside the game's TriggerCmdReqQueue call
AFTER = 0x17edc0
# which submissions to capture: CAPTURE_LIST=<addr> (exact) or CAPTURE_MIN_SIZE=<bytes> for any list in
# the linear heap at least that big; CAPTURE_COUNT submissions are captured in a row
LIST = int(os.environ.get('CAPTURE_LIST', '0'), 16)
MIN_SIZE = int(os.environ.get('CAPTURE_MIN_SIZE', '0'), 16)
COUNT = int(os.environ.get('CAPTURE_COUNT', '2'))
inf = gdb.selected_inferior()
gdb.execute(f'break *{TRIGGER:#x}')
captured = 0
for n in range(2000):
    gdb.execute('continue', to_string=True)
    q = bytes(inf.read_memory(0x10002800, 0x200))
    idx, cnt = q[0], q[1]
    c = struct.unpack_from('<8I', q, 0x20 + (idx % 15) * 0x20)
    if cnt != 1 or (c[0] & 0xff) != 1:
        continue
    if LIST and c[1] != LIST:
        continue
    if MIN_SIZE and not (c[2] >= MIN_SIZE and 0x30000000 <= c[1] < 0x30190000):
        continue
    k = captured
    gdb.execute(f'dump binary memory {OUT}/before_{k}_vram.bin 0x1f000000 0x1f600000')
    gdb.execute(f'dump binary memory {OUT}/before_{k}_linear.bin 0x30000000 0x33500000')
    gdb.execute(f'dump binary memory {OUT}/before_{k}_gsp.bin 0x10002000 0x10003000')
    gdb.execute(f'tbreak *{AFTER:#x}')
    gdb.execute('continue', to_string=True)
    gdb.execute(f'dump binary memory {OUT}/after_{k}_vram.bin 0x1f000000 0x1f600000')
    print(f'captured submission {k} at trigger hit {n}: list {c[1]:#x} size {c[2]:#x}')
    open(f'{OUT}/list_{k}.txt', 'w').write(f'{c[1]:08x} {c[2]:x}\n')
    captured += 1
    if captured == COUNT:
        break
gdb.execute('delete')
end
detach

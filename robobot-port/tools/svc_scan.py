#!/usr/bin/env python3
"""Scan decompressed code for ARM-mode SVC instructions (cond=AL) and summarise by SVC number."""
import struct, sys, collections

NAMES = {
 0x01:"ControlMemory",0x02:"QueryMemory",0x03:"ExitProcess",0x08:"CreateThread",0x09:"ExitThread",
 0x0A:"SleepThread",0x0B:"GetThreadPriority",0x0C:"SetThreadPriority",0x13:"CreateMutex",0x14:"ReleaseMutex",
 0x15:"CreateSemaphore",0x16:"ReleaseSemaphore",0x17:"CreateEvent",0x18:"SignalEvent",0x19:"ClearEvent",
 0x1A:"CreateTimer",0x1B:"SetTimer",0x1C:"CancelTimer",0x1D:"ClearTimer",0x1E:"CreateMemoryBlock",
 0x1F:"MapMemoryBlock",0x20:"UnmapMemoryBlock",0x21:"CreateAddressArbiter",0x22:"ArbitrateAddress",
 0x23:"CloseHandle",0x24:"WaitSynchronization1",0x25:"WaitSynchronizationN",0x26:"SignalAndWait",
 0x27:"DuplicateHandle",0x28:"GetSystemTick",0x29:"GetHandleInfo",0x2A:"GetSystemInfo",0x2B:"GetProcessInfo",
 0x2C:"GetThreadInfo",0x2D:"ConnectToPort",0x32:"SendSyncRequest",0x35:"GetProcessId",0x37:"GetThreadId",
 0x38:"GetResourceLimit",0x39:"GetResourceLimitLimitValues",0x3A:"GetResourceLimitCurrentValues",
 0x3C:"Break",0x3D:"OutputDebugString",0x2E:"GetProcessIdOfThread",0x34:"GetProcessIdUnused",
 0x7D:"QueryProcessMemory",0xFF:"(debug)",
}
d = open(sys.argv[1], "rb").read()
BASE = 0x100000
hits = collections.defaultdict(list)
for i in range(0, len(d) - 3, 4):
    w = struct.unpack_from("<I", d, i)[0]
    if (w >> 24) == 0xEF and (w & 0xFFFF00) == 0:   # svc AL, small immediate
        hits[w & 0xFF].append(BASE + i)
print(f"distinct SVC numbers in ARM-mode code: {len(hits)}")
for n in sorted(hits):
    addrs = hits[n]
    print(f"  {n:#04x} {NAMES.get(n,'?'):28s} x{len(addrs):<4d} e.g. {', '.join(hex(a) for a in addrs[:3])}")

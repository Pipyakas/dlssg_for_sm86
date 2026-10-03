"""Read-only check that the running launcher applied all seven preset hooks."""
import ctypes
import struct
import sys

pid = int(sys.argv[1])
base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x140000000
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
kernel.OpenProcess.restype = ctypes.c_void_p
kernel.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel.CloseHandle.argtypes = [ctypes.c_void_p]
process = kernel.OpenProcess(0x410, False, pid)
if not process:
    raise ctypes.WinError(ctypes.get_last_error())

def read(address, length):
    buffer = ctypes.create_string_buffer(length)
    count = ctypes.c_size_t()
    if not kernel.ReadProcessMemory(process, address, buffer, length, ctypes.byref(count)) or count.value != length:
        raise ctypes.WinError(ctypes.get_last_error())
    return buffer.raw

try:
    cave = base + 0x6cdebd6
    hook = read(cave, 17)
    assert hook[:13] == bytes.fromhex('41 8d 40 fe 83 f8 02 77 03 41 ff c0 e9')
    assert cave + 17 + struct.unpack('<i', hook[13:])[0] == base + 0xa851a70
    for rva in (0x6cdb7ba, 0x6cde11b, 0x6cde3c9, 0x6ce060c, 0x6ce08d7, 0x6ce3162, 0x6ce346c):
        call = read(base + rva, 5)
        assert call[0] == 0xe8 and base + rva + 5 + struct.unpack('<i', call[1:])[0] == cave, hex(rva)
    print(f'PASS: PID {pid} has the correct cave and all seven DLSS preset hooks')
finally:
    kernel.CloseHandle(process)

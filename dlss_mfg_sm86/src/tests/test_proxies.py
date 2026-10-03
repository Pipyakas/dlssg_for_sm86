"""Check export coverage and exercise forwarded Windows APIs, not just LoadLibrary."""
import ctypes
import pathlib
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from generate_proxies import exports, KINDS, PROXIES, SYSTEM

kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
kernel.GetProcAddress.restype = ctypes.c_void_p

def address(module, ordinal):
    return kernel.GetProcAddress(module._handle, ordinal)

modules = {}
for kind in KINDS:
    path = PROXIES / kind / f'{kind}.dll'
    expected = exports((SYSTEM / f'{kind}.dll').read_bytes())
    actual = dict(exports(path.read_bytes()))
    for ordinal, names in expected:
        assert ordinal in actual and set(names).issubset(actual[ordinal]), (kind, ordinal, names)
    proxy = ctypes.WinDLL(str(path))
    for ordinal, _ in expected:
        assert address(proxy, ordinal), (kind, ordinal)
    modules[kind] = (proxy, ctypes.WinDLL(str(SYSTEM / f'{kind}.dll')))
    print('PASS:', kind, len(expected), 'System32 ordinal slots and names covered')

proxy, original = modules['version']
for module in (proxy, original):
    module.GetFileVersionInfoSizeW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)]
    module.GetFileVersionInfoSizeW.restype = ctypes.c_ulong
dummy = ctypes.c_ulong()
size = original.GetFileVersionInfoSizeW(str(SYSTEM / 'kernel32.dll'), ctypes.byref(dummy))
assert size > 0 and proxy.GetFileVersionInfoSizeW(str(SYSTEM / 'kernel32.dll'), ctypes.byref(dummy)) == size
for module in (proxy, original):
    module.GetFileVersionInfoW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
    module.GetFileVersionInfoW.restype = ctypes.c_int
left, right = ctypes.create_string_buffer(size), ctypes.create_string_buffer(size)
assert proxy.GetFileVersionInfoW(str(SYSTEM / 'kernel32.dll'), 0, size, left)
assert original.GetFileVersionInfoW(str(SYSTEM / 'kernel32.dll'), 0, size, right)
assert left.raw == right.raw

proxy, original = modules['winmm']
proxy.timeGetTime.restype = original.timeGetTime.restype = ctypes.c_ulong
assert abs(proxy.timeGetTime() - original.timeGetTime()) < 1000
proxy.waveOutGetNumDevs.restype = original.waveOutGetNumDevs.restype = ctypes.c_uint
assert proxy.waveOutGetNumDevs() == original.waveOutGetNumDevs()

proxy, original = modules['dbghelp']
proxy.ImagehlpApiVersion.restype = original.ImagehlpApiVersion.restype = ctypes.c_void_p
assert ctypes.string_at(proxy.ImagehlpApiVersion(), 8) == ctypes.string_at(original.ImagehlpApiVersion(), 8)

proxy, original = modules['dinput8']
proxy.GetdfDIJoystick.restype = original.GetdfDIJoystick.restype = ctypes.c_void_p
assert proxy.GetdfDIJoystick() == original.GetdfDIJoystick()

class GUID(ctypes.Structure):
    _fields_ = [('a', ctypes.c_uint32), ('b', ctypes.c_uint16), ('c', ctypes.c_uint16), ('d', ctypes.c_ubyte * 8)]

def release(pointer):
    table = ctypes.cast(pointer, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(table[2])(pointer)

proxy, original = modules['dxgi']
iid = GUID(0x7b7166ec, 0x21c7, 0x44ae, (ctypes.c_ubyte * 8)(0xb2,0x1a,0xc9,0xae,0x32,0x1a,0xe3,0x69))
for module in (proxy, original):
    module.CreateDXGIFactory.argtypes = [ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p)]
    module.CreateDXGIFactory.restype = ctypes.c_long
    factory = ctypes.c_void_p()
    assert module.CreateDXGIFactory(ctypes.byref(iid), ctypes.byref(factory)) == 0
    release(factory)

class RootDescription(ctypes.Structure):
    _fields_ = [('parameters', ctypes.c_uint), ('parameter_ptr', ctypes.c_void_p),
                ('samplers', ctypes.c_uint), ('sampler_ptr', ctypes.c_void_p), ('flags', ctypes.c_uint)]

proxy, original = modules['d3d12']
blobs = []
for module in (proxy, original):
    module.D3D12SerializeRootSignature.argtypes = [ctypes.POINTER(RootDescription), ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
    module.D3D12SerializeRootSignature.restype = ctypes.c_long
    description, blob, errors = RootDescription(), ctypes.c_void_p(), ctypes.c_void_p()
    assert module.D3D12SerializeRootSignature(ctypes.byref(description), 1, ctypes.byref(blob), ctypes.byref(errors)) == 0
    table = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    get_pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(table[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(table[4])
    blobs.append(ctypes.string_at(get_pointer(blob), get_size(blob)))
    release(blob)
    if errors: release(errors)
assert blobs[0] == blobs[1]
print('PASS: real version, multimedia, debug, input, DXGI, and D3D12 calls forwarded correctly')

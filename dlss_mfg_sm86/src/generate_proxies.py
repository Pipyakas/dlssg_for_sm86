"""Generate complete x64 forwarding stubs from the machine's System32 exports.

Only generated metadata goes into build/; no Microsoft DLLs are distributed.
"""
import ctypes
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parent
PROXIES = ROOT.parent / 'build' / 'proxies'
KINDS = ('version', 'winmm', 'dbghelp', 'dinput8', 'dxgi', 'd3d12')
buffer = ctypes.create_unicode_buffer(260)
if not ctypes.windll.kernel32.GetSystemDirectoryW(buffer, len(buffer)):
    raise ctypes.WinError()
SYSTEM = pathlib.Path(buffer.value)

def exports(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert struct.unpack_from('<H', data, pe + 4)[0] == 0x8664
    optional = pe + 24
    directory = optional + 112
    table_rva = struct.unpack_from('<I', data, directory)[0]
    start = optional + struct.unpack_from('<H', data, pe + 20)[0]
    sections = [struct.unpack_from('<IIII', data, start + i * 40 + 8)
                for i in range(struct.unpack_from('<H', data, pe + 6)[0])]
    def offset(rva):
        for size, address, raw_size, raw in sections:
            if address <= rva < address + max(size, raw_size):
                return raw + rva - address
        raise ValueError(hex(rva))
    table = offset(table_rva)
    base, count, named, functions, names, ordinals = struct.unpack_from('<IIIIII', data, table + 16)
    mapping = {}
    for i in range(named):
        name_rva = struct.unpack_from('<I', data, offset(names) + i * 4)[0]
        ordinal = struct.unpack_from('<H', data, offset(ordinals) + i * 2)[0] + base
        raw = offset(name_rva)
        mapping.setdefault(ordinal, []).append(data[raw:data.index(b'\0', raw)].decode())
    return [(base + i, mapping.get(base + i, [])) for i in range(count)
            if struct.unpack_from('<I', data, offset(functions) + i * 4)[0]]

def generate():
    for kind in KINDS:
        entries = exports((SYSTEM / f'{kind}.dll').read_bytes())
        directory = PROXIES / kind
        directory.mkdir(parents=True, exist_ok=True)
        header = ['#pragma once', f'#define PROXY_EXPORT_COUNT {len(entries)}',
                  f'#define PROXY_SYSTEM_DLL L"{kind}.dll"',
                  'static const unsigned short kProxyOrdinals[] = {' + ','.join(str(o) for o, _ in entries) + '};']
        definition = [f'LIBRARY {kind}', 'EXPORTS']
        assembly = ['option casemap:none', 'EXTERN ProxyResolve:PROC', '.code']
        for index, (ordinal, names) in enumerate(entries):
            symbol = f'Forward{index}'
            if names:
                for alias in names:
                    private = ' PRIVATE' if alias.startswith(('DllCanUnloadNow', 'DllGetClassObject', 'DllRegisterServer', 'DllUnregisterServer')) else ''
                    definition.append(f'    {alias}={symbol} @{ordinal}{private}')
            else:
                definition.append(f'    ordinal_{ordinal}={symbol} @{ordinal} NONAME')
            assembly += [f'{symbol} PROC FRAME', '    sub rsp, 088h', '    .allocstack 088h',
                         '    .endprolog', '    mov [rsp+020h], rcx', '    mov [rsp+028h], rdx',
                         '    mov [rsp+030h], r8', '    mov [rsp+038h], r9',
                         '    movdqu [rsp+040h], xmm0', '    movdqu [rsp+050h], xmm1',
                         '    movdqu [rsp+060h], xmm2', '    movdqu [rsp+070h], xmm3',
                         f'    mov ecx, {index}', '    call ProxyResolve',
                         '    mov rcx, [rsp+020h]', '    mov rdx, [rsp+028h]',
                         '    mov r8, [rsp+030h]', '    mov r9, [rsp+038h]',
                         '    movdqu xmm0, [rsp+040h]', '    movdqu xmm1, [rsp+050h]',
                         '    movdqu xmm2, [rsp+060h]', '    movdqu xmm3, [rsp+070h]',
                         '    add rsp, 088h', '    jmp rax', f'{symbol} ENDP']
        assembly.append('END')
        (directory / 'proxy_exports.h').write_text('\n'.join(header) + '\n')
        (directory / 'proxy.def').write_text('\n'.join(definition) + '\n')
        (directory / 'proxy.asm').write_text('\n'.join(assembly) + '\n')
        print(kind, len(entries), 'forwarded ordinal slots')


if __name__ == '__main__':
    generate()

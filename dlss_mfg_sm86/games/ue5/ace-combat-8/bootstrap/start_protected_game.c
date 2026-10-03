#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// ac8_bootstrap.ini sits beside this launcher in the game root.
#define AC8_CONFIG_NAME L"ac8_bootstrap.ini"

// Strict decimal parsing: malformed and out-of-range settings use the default,
// rather than accidentally turning a feature off through the Win32 int parser.
static DWORD ReadConfigUInt(const WCHAR* path, const WCHAR* section,
    const WCHAR* key, DWORD fallback, DWORD minimum, DWORD maximum) {
    WCHAR text[32];
    DWORD length = GetPrivateProfileStringW(section, key, L"", text, 32, path);
    if (!length || length >= 31) return fallback;
    DWORD value = 0;
    const WCHAR* p = text;
    while (*p == L' ' || *p == L'\t') p++;
    BOOL any = FALSE;
    while (*p >= L'0' && *p <= L'9') {
        DWORD digit = (DWORD)(*p++ - L'0');
        if (digit > maximum || value > (maximum - digit) / 10) return fallback;
        value = value * 10 + digit;
        any = TRUE;
    }
    while (*p == L' ' || *p == L'\t') p++;
    if (!any || *p || value < minimum || value > maximum) return fallback;
    return value;
}

#pragma function(memcpy)
void* memcpy(void* dest, const void* src, size_t count) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    while (count--) *d++ = *s++;
    return dest;
}

#pragma function(memset)
void* memset(void* dest, int val, size_t count) {
    char* d = (char*)dest;
    while (count--) *d++ = (char)val;
    return dest;
}

// Copy explicit keys only: preserve unrelated destination settings. @remove is
// an explicit deletion, so the user never needs to open the destination INI.
static BOOL SyncSection(const WCHAR* configPath, const WCHAR* sourceSection,
    const WCHAR* destinationPath, const WCHAR* destinationSection) {
    const DWORD capacity = 32768;
    WCHAR* buffer = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity * sizeof(WCHAR));
    if (!buffer) return FALSE;
    DWORD length = GetPrivateProfileSectionW(sourceSection, buffer, capacity, configPath);
    BOOL ok = length < capacity - 2;
    for (WCHAR* row = buffer; ok && *row;) {
        WCHAR* next = row + lstrlenW(row) + 1;
        WCHAR* equals = row;
        while (*equals && *equals != L'=') equals++;
        if (*equals) {
            *equals = 0;
            const WCHAR* value = equals + 1;
            if (lstrcmpiW(value, L"@remove") == 0) value = NULL;
            WCHAR expanded[MAX_PATH];
            // The mod does not expand environment variables itself. Allow the
            // consolidated INI to use portable paths for its path-valued keys.
            if (value && lstrlenW(sourceSection) >= 5 && CompareStringOrdinal(sourceSection, 5, L"SM86.", 5, TRUE) == CSTR_EQUAL &&
                (!lstrcmpiW(row, L"Directory") || !lstrcmpiW(row, L"CacheDirectory") || !lstrcmpiW(row, L"Path"))) {
                DWORD count = ExpandEnvironmentStringsW(value, expanded, MAX_PATH);
                if (!count || count > MAX_PATH) { ok = FALSE; break; }
                value = expanded;
            }
            ok = WritePrivateProfileStringW(destinationSection, row, value, destinationPath);
        }
        row = next;
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    return ok;
}

static BOOL SyncModSettings(const WCHAR* configPath, const WCHAR* modIniPath) {
    const DWORD capacity = 32768;
    WCHAR* sections = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity * sizeof(WCHAR));
    if (!sections) return FALSE;
    DWORD length = GetPrivateProfileSectionNamesW(sections, capacity, configPath);
    BOOL ok = length < capacity - 2;
    for (WCHAR* section = sections; ok && *section; section += lstrlenW(section) + 1) {
        if (lstrlenW(section) > 5 && CompareStringOrdinal(section, 5, L"SM86.", 5, TRUE) == CSTR_EQUAL) {
            ok = SyncSection(configPath, section, modIniPath, section + 5);
        }
    }
    HeapFree(GetProcessHeap(), 0, sections);
    return ok;
}

// Frame generation mode/count control lives in the dlss_mfg_sm86 proxy
// DLL. This launcher only writes the plain CVars, e.g. turning FG on at all,
// because AC8 has no in-game FG toggle.
static BOOL ConfigureEngine(const WCHAR* engineIniPath, const WCHAR* configPath) {
    if (!SyncSection(configPath, L"ConsoleVariables", engineIniPath, L"ConsoleVariables")) return FALSE;
    if (ReadConfigUInt(configPath, L"Launcher", L"RemoveScreenPercentage", 1, 0, 1)) {
        // Use the INI parser rather than substring matching, including UTF-16.
        if (!WritePrivateProfileStringW(L"ConsoleVariables", L"r.ScreenPercentage", NULL, engineIniPath)) return FALSE;
        if (!WritePrivateProfileStringW(L"SystemSettings", L"r.ScreenPercentage", NULL, engineIniPath)) return FALSE;
    }
    return TRUE;
}

typedef struct {
    BOOL fixDLSSPresets, lockEngineIni, enableDynamicResolution;
} LauncherSettings;

__declspec(noinline) static BOOL PrepareConfiguration(const WCHAR* gameDir,
    WCHAR* engineIniPath, LauncherSettings* settings) {
    WCHAR configPath[MAX_PATH], modIniPath[MAX_PATH], localAppData[MAX_PATH];
    lstrcpyW(configPath, gameDir); lstrcatW(configPath, L"\\" AC8_CONFIG_NAME);
    if (GetFileAttributesW(configPath) == INVALID_FILE_ATTRIBUTES) return FALSE;
    settings->fixDLSSPresets = ReadConfigUInt(configPath, L"Launcher", L"FixDLSSPresets", 1, 0, 1);
    settings->lockEngineIni = ReadConfigUInt(configPath, L"Launcher", L"LockEngineIni", 1, 0, 1);
    settings->enableDynamicResolution = ReadConfigUInt(configPath, L"Launcher", L"EnableDynamicResolution", 1, 0, 1);
    lstrcpyW(modIniPath, gameDir); lstrcatW(modIniPath, L"\\Game\\Binaries\\Win64\\dlssg_sm86.ini");
    if (!SyncModSettings(configPath, modIniPath)) return FALSE;
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (!length || length >= MAX_PATH) return FALSE;
    lstrcpyW(engineIniPath, localAppData);
    lstrcatW(engineIniPath, L"\\BANDAI NAMCO Entertainment\\ACE COMBAT 8\\Saved\\Config\\Windows\\Engine.ini");
    DWORD attributes = GetFileAttributesW(engineIniPath);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
        if (!SetFileAttributesW(engineIniPath, attributes & ~FILE_ATTRIBUTE_READONLY)) return FALSE;
    }
    return ConfigureEngine(engineIniPath, configPath);
}

// In-memory binary patch for AceCombat8.exe
// Hooks every DLSS PerfQualityValue setup call to fix the game's UI off-by-one scale enum bug
//
// All 7 call sites pass the UI-selected UDLSSMode to NVSDK_NGX_Parameter_SetUI("PerfQualityValue"):
//   0x6cdb7ba - optimal settings query (drives the actual render scale via the
//               DLSSOptimalSettingsCallback / DLSS.Get.Dynamic.*.Render.* readback)
//   0x6cde11b, 0x6cde3c9, 0x6ce060c, 0x6ce08d7, 0x6ce3162, 0x6ce346c - feature create paths
//
// Game sends {Quality=2, Balanced=3, Performance=4, UltraPerformance=5|6}.
// Shifting 2..4 up by one yields {3,4,5,6} = the correct NGX enums, so every
// in-game dropdown entry maps to its real DLSS render scale.
static const ULONG_PTR kPerfQualityCallSites[] = {
    0x6cdb7ba, 0x6cde11b, 0x6cde3c9, 0x6ce060c, 0x6ce08d7, 0x6ce3162, 0x6ce346c
};
#define PERF_QUALITY_CALL_COUNT (sizeof(kPerfQualityCallSites) / sizeof(kPerfQualityCallSites[0]))

static BOOL PatchGameProcess(HANDLE hProcess, ULONG_PTR imageBase) {
    // RVAs calculated from AceCombat8.exe PE header
    // Cave RVA: 0x6cdebd6 (26 bytes of INT3 padding between functions)
    // Target SetUI RVA: 0xa851a70
    ULONG_PTR caveVA  = imageBase + 0x6cdebd6;
    ULONG_PTR destSetUI = imageBase + 0xa851a70;

    // Hook code:
    // 41 8d 40 fe   lea eax, [r8 - 2]
    // 83 f8 02      cmp eax, 2
    // 77 03         ja skip (+3)
    // 41 ff c0      inc r8d
    // e9 <disp32>   jmp destSetUI
    BYTE hookBytes[17];
    hookBytes[0] = 0x41; hookBytes[1] = 0x8d; hookBytes[2] = 0x40; hookBytes[3] = 0xfe;
    hookBytes[4] = 0x83; hookBytes[5] = 0xf8; hookBytes[6] = 0x02;
    hookBytes[7] = 0x77; hookBytes[8] = 0x03;
    hookBytes[9] = 0x41; hookBytes[10] = 0xff; hookBytes[11] = 0xc0;
    hookBytes[12] = 0xe9;

    LONG jmpDisp = (LONG)(destSetUI - (caveVA + 17));
    *(LONG*)(&hookBytes[13]) = jmpDisp;

    DWORD oldProt;
    SIZE_T written = 0;
    SIZE_T readCount = 0;

    // 0. The code cave must still be untouched INT3 padding (or already ours)
    BYTE caveProbe[17];
    if (!ReadProcessMemory(hProcess, (LPCVOID)caveVA, caveProbe, sizeof(caveProbe), &readCount) || readCount != sizeof(caveProbe)) {
        return FALSE;
    }
    BOOL caveFresh = TRUE;
    BOOL caveOurs = TRUE;
    for (int i = 0; i < 17; i++) {
        if (caveProbe[i] != 0xCC) caveFresh = FALSE;
        if (caveProbe[i] != hookBytes[i]) caveOurs = FALSE;
    }
    if (!caveFresh && !caveOurs) return FALSE;

    // 1. Write hook into code cave
    if (!caveOurs) {
        if (!VirtualProtectEx(hProcess, (LPVOID)caveVA, sizeof(hookBytes), PAGE_EXECUTE_READWRITE, &oldProt)) return FALSE;
        if (!WriteProcessMemory(hProcess, (LPVOID)caveVA, hookBytes, sizeof(hookBytes), &written) || written != sizeof(hookBytes)) {
            VirtualProtectEx(hProcess, (LPVOID)caveVA, sizeof(hookBytes), oldProt, &oldProt);
            return FALSE;
        }
        VirtualProtectEx(hProcess, (LPVOID)caveVA, sizeof(hookBytes), oldProt, &oldProt);
    }

    // 2. Redirect every PerfQualityValue call site into the cave.
    //    Each site must still be the expected `call SetUI` (5-byte E8 rel32) so a
    //    game update that moves the code simply disables the hook instead of corrupting it.
    int patched = 0;
    for (unsigned i = 0; i < PERF_QUALITY_CALL_COUNT; i++) {
        ULONG_PTR callVA = imageBase + kPerfQualityCallSites[i];
        BYTE callBytes[5];
        callBytes[0] = 0xe8;

        BYTE original[5];
        if (!ReadProcessMemory(hProcess, (LPCVOID)callVA, original, sizeof(original), &readCount) || readCount != sizeof(original)) {
            continue;
        }
        if (original[0] != 0xe8) continue;

        LONG origDisp = 0;
        for (int b = 0; b < 4; b++) ((BYTE*)&origDisp)[b] = original[1 + b];
        ULONG_PTR origTarget = callVA + 5 + (LONG)origDisp;

        if (origTarget == caveVA) { patched++; continue; } // already applied
        if (origTarget != destSetUI) continue;             // not the expected call: skip

        LONG callDisp = (LONG)(caveVA - (callVA + 5));
        for (int b = 0; b < 4; b++) callBytes[1 + b] = (BYTE)((callDisp >> (8 * b)) & 0xFF);

        if (!VirtualProtectEx(hProcess, (LPVOID)callVA, sizeof(callBytes), PAGE_EXECUTE_READWRITE, &oldProt)) continue;
        BOOL ok = WriteProcessMemory(hProcess, (LPVOID)callVA, callBytes, sizeof(callBytes), &written) && written == sizeof(callBytes);
        VirtualProtectEx(hProcess, (LPVOID)callVA, sizeof(callBytes), oldProt, &oldProt);
        if (ok) patched++;
    }

    if (patched == 0) return FALSE;

    FlushInstructionCache(hProcess, NULL, 0);
    return TRUE;
}

// Dynamic resolution: AC8's D3D12 renderer never sets GRHISupportsDynamicResolution
// (nothing in the game writes it), so UE treats dynamic resolution as unsupported:
// the viewport never applies it and r.DynamicRes.* has no effect. The flag is found
// through FDefaultDynamicResolutionState::IsSupported, which returns it with
// `movzx eax, byte ptr [rip+disp]`: verify the function's bytes, take the flag's
// address from that instruction, then set the flag while the game runs.
// (DLSS Ultra Performance still reports a fixed resolution; Performance and up
// allow 50-100%.)
#define DRS_IS_SUPPORTED_RVA 0x43f0dc0
static const BYTE kIsSupported[47] = {
    0x48, 0x83, 0xEC, 0x28,                     // sub rsp, 28h
    0x48, 0x8B, 0x05, 0, 0, 0, 0,               // mov rax, [rip+GEngine]      (disp: any)
    0x48, 0x8B, 0x88, 0x30, 0x0E, 0x00, 0x00,   // mov rcx, [rax+0E30h]        (XR system)
    0x48, 0x85, 0xC9, 0x74, 0x11,               // test rcx, rcx / jz
    0x48, 0x8B, 0x01, 0xFF, 0x50, 0x08,         // call [vtable+8]
    0x84, 0xC0, 0x74, 0x07, 0x32, 0xC0,         // test al, al / jz / xor al, al
    0x48, 0x83, 0xC4, 0x28, 0xC3,               // add rsp, 28h / ret
    0x0F, 0xB6, 0x05, 0, 0, 0, 0                // movzx eax, byte [rip+flag]  (disp: the flag)
};

// Returns the flag's address, or 0 when the code is not the expected function
// (a game update): the fix is then skipped rather than writing elsewhere.
static ULONG_PTR FindDynamicResolutionFlag(const BYTE* code, ULONG_PTR codeVA) {
    for (int i = 0; i < (int)sizeof(kIsSupported); i++) {
        if ((i >= 7 && i <= 10) || i >= 43) continue;
        if (code[i] != kIsSupported[i]) return 0;
    }
    LONG disp = 0;
    for (int b = 0; b < 4; b++) ((BYTE*)&disp)[b] = code[43 + b];
    return codeVA + sizeof(kIsSupported) + disp;
}

typedef struct _PROCESS_BASIC_INFORMATION {
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
} PROCESS_BASIC_INFORMATION;

typedef LONG (NTAPI *pfnNtQueryInformationProcess)(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength
);

static ULONG_PTR GetProcessImageBase(HANDLE hProcess) {
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtDll) return 0x140000000;

    pfnNtQueryInformationProcess pNtQIP = (pfnNtQueryInformationProcess)GetProcAddress(hNtDll, "NtQueryInformationProcess");
    if (!pNtQIP) return 0x140000000;

    PROCESS_BASIC_INFORMATION pbi;
    ULONG retLen = 0;
    if (pNtQIP(hProcess, 0, &pbi, sizeof(pbi), &retLen) < 0 || !pbi.PebBaseAddress) {
        return 0x140000000;
    }

    // PEB + 0x10 contains ImageBaseAddress on 64-bit Windows
    PVOID imageBasePtr = (PVOID)((ULONG_PTR)pbi.PebBaseAddress + 0x10);
    ULONG_PTR imageBase = 0;
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(hProcess, imageBasePtr, &imageBase, sizeof(imageBase), &bytesRead) && imageBase) {
        return imageBase;
    }

    return 0x140000000;
}

void entry(void) {
    WCHAR exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        ExitProcess(1);
    }

    WCHAR* lastSlash = NULL;
    for (WCHAR* p = exePath; *p; p++) {
        if (*p == L'\\') lastSlash = p;
    }
    if (!lastSlash) {
        ExitProcess(1);
    }
    *lastSlash = L'\0';

    WCHAR targetDir[MAX_PATH];
    WCHAR targetExe[MAX_PATH];
    lstrcpyW(targetDir, exePath);
    lstrcatW(targetDir, L"\\Game\\Binaries\\Win64");

    lstrcpyW(targetExe, targetDir);
    lstrcatW(targetExe, L"\\AceCombat8.exe");

    DWORD attrib = GetFileAttributesW(targetExe);
    if (attrib == INVALID_FILE_ATTRIBUTES || (attrib & FILE_ATTRIBUTE_DIRECTORY)) {
        ExitProcess(2);
    }

    WCHAR engineIniPath[MAX_PATH];
    engineIniPath[0] = L'\0';
    LauncherSettings settings;
    if (!PrepareConfiguration(exePath, engineIniPath, &settings)) ExitProcess(4);

    HANDLE hIniLock = INVALID_HANDLE_VALUE;
    if (settings.lockEngineIni && engineIniPath[0] != L'\0') {
        hIniLock = CreateFileW(
            engineIniPath,
            GENERIC_READ,
            FILE_SHARE_READ,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
    }

    LPCWSTR cmdLine = GetCommandLineW();
    while (*cmdLine == L' ' || *cmdLine == L'\t') cmdLine++;
    if (*cmdLine == L'\"') {
        cmdLine++;
        while (*cmdLine && *cmdLine != L'\"') cmdLine++;
        if (*cmdLine == L'\"') cmdLine++;
    } else {
        while (*cmdLine && *cmdLine != L' ' && *cmdLine != L'\t') cmdLine++;
    }

    DWORD fullLen = lstrlenW(targetExe) + 4 + lstrlenW(cmdLine) + 8;
    WCHAR* fullCmd = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, fullLen * sizeof(WCHAR));
    if (!fullCmd) {
        if (hIniLock != INVALID_HANDLE_VALUE) CloseHandle(hIniLock);
        ExitProcess(3);
    }

    fullCmd[0] = L'\"';
    fullCmd[1] = L'\0';
    lstrcatW(fullCmd, targetExe);
    lstrcatW(fullCmd, L"\"");
    if (*cmdLine) {
        lstrcatW(fullCmd, cmdLine);
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    for (BYTE* p = (BYTE*)&si; p < (BYTE*)&si + sizeof(si); p++) *p = 0;
    si.cb = sizeof(si);
    for (BYTE* p = (BYTE*)&pi; p < (BYTE*)&pi + sizeof(pi); p++) *p = 0;

    BOOL success = CreateProcessW(
        targetExe,
        fullCmd,
        NULL,
        NULL,
        FALSE,
        CREATE_SUSPENDED,
        NULL,
        targetDir,
        &si,
        &pi
    );

    if (!success) {
        DWORD err = GetLastError();
        if (hIniLock != INVALID_HANDLE_VALUE) CloseHandle(hIniLock);
        ExitProcess(err);
    }

    ULONG_PTR imageBase = GetProcessImageBase(pi.hProcess);
    if (settings.fixDLSSPresets) PatchGameProcess(pi.hProcess, imageBase);

    ULONG_PTR drsFlag = 0;
    if (settings.enableDynamicResolution) {
        BYTE code[sizeof(kIsSupported)];
        SIZE_T got = 0;
        if (ReadProcessMemory(pi.hProcess, (LPCVOID)(imageBase + DRS_IS_SUPPORTED_RVA), code, sizeof(code), &got) && got == sizeof(code))
            drsFlag = FindDynamicResolutionFlag(code, imageBase + DRS_IS_SUPPORTED_RVA);
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Set the flag once a second for the first 5 minutes: the renderer comes up
    // after the game's startup, and nothing in the game writes the flag afterwards.
    for (int i = 0; drsFlag && i < 300 && WaitForSingleObject(pi.hProcess, 1000) == WAIT_TIMEOUT; i++) {
        BYTE value = 1;
        SIZE_T count = 0;
        if (ReadProcessMemory(pi.hProcess, (LPCVOID)drsFlag, &value, 1, &count) && count == 1 && value == 0)
            WriteProcessMemory(pi.hProcess, (LPVOID)drsFlag, "\x01", 1, &count);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    if (hIniLock != INVALID_HANDLE_VALUE) {
        CloseHandle(hIniLock);
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    ExitProcess(exitCode);
}

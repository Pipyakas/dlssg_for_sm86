#define entry launcher_entry
#include "../start_protected_game.c"
#undef entry

void testEntry(void) {
    static WCHAR temp[MAX_PATH], path[MAX_PATH], config[MAX_PATH], mod[MAX_PATH], value[64];
    DWORD length = GetTempPathW(MAX_PATH, temp);
    if (!length || length >= MAX_PATH) ExitProcess(10);
    if (!GetTempFileNameW(temp, L"ac8", 0, path) || !GetTempFileNameW(temp, L"ac8", 0, config) || !GetTempFileNameW(temp, L"ac8", 0, mod)) ExitProcess(10);
    // Consolidation: raw CVars, extra keys, explicit deletion, unrelated data.
    WritePrivateProfileStringW(L"ConsoleVariables", L"r.Streamline.DLSSG.Enable", L"2", config);
    WritePrivateProfileStringW(L"ConsoleVariables", L"r.Streamline.DLSSG.FramesToGenerate", L"2", config);
    WritePrivateProfileStringW(L"ConsoleVariables", L"t.Streamline.Reflex.Mode", L"1", config);
    WritePrivateProfileStringW(L"ConsoleVariables", L"custom.test", L"7", config);
    WritePrivateProfileStringW(L"ConsoleVariables", L"obsolete.test", L"@remove", config);
    WritePrivateProfileStringW(L"ConsoleVariables", L"obsolete.test", L"99", path);
    WritePrivateProfileStringW(L"Other", L"Keep", L"untouched", path);
    WritePrivateProfileStringW(L"ConsoleVariables", L"r.ScreenPercentage", L"50", path);
    if (!ConfigureEngine(path, config)) ExitProcess(20);
    if (GetPrivateProfileIntW(L"ConsoleVariables", L"r.Streamline.DLSSG.Enable", 0, path) != 2 ||
        GetPrivateProfileIntW(L"ConsoleVariables", L"r.Streamline.DLSSG.FramesToGenerate", 0, path) != 2 ||
        GetPrivateProfileIntW(L"ConsoleVariables", L"t.Streamline.Reflex.Mode", 0, path) != 1 ||
        GetPrivateProfileIntW(L"ConsoleVariables", L"custom.test", 0, path) != 7) ExitProcess(21);
    GetPrivateProfileStringW(L"ConsoleVariables", L"obsolete.test", L"missing", value, 64, path);
    if (lstrcmpW(value, L"missing")) ExitProcess(22);
    GetPrivateProfileStringW(L"ConsoleVariables", L"r.ScreenPercentage", L"missing", value, 64, path);
    if (lstrcmpW(value, L"missing")) ExitProcess(23);
    GetPrivateProfileStringW(L"Other", L"Keep", L"", value, 64, path);
    if (lstrcmpW(value, L"untouched")) ExitProcess(24);
    // Repeat writes keep the configured fixed count; nothing here rewrites it.
    if (!ConfigureEngine(path, config) || GetPrivateProfileIntW(L"ConsoleVariables", L"r.Streamline.DLSSG.FramesToGenerate", 0, path) != 2) ExitProcess(25);
    WritePrivateProfileStringW(L"Launcher", L"RemoveScreenPercentage", L"0", config);
    WritePrivateProfileStringW(L"ConsoleVariables", L"r.ScreenPercentage", L"65", config);
    if (!ConfigureEngine(path, config) || GetPrivateProfileIntW(L"ConsoleVariables", L"r.ScreenPercentage", 0, path) != 65) ExitProcess(26);
    // Prefix-based mod synchronization supports extra sections and @remove.
    WritePrivateProfileStringW(L"SM86.FrameGeneration", L"MaxGeneratedFrames", L"2", config);
    WritePrivateProfileStringW(L"SM86.Debug", L"MarkGeneratedFrames", L"1", config);
    WritePrivateProfileStringW(L"SM86.Runtime", L"Obsolete", L"@remove", config);
    WritePrivateProfileStringW(L"Runtime", L"Obsolete", L"1", mod);
    WritePrivateProfileStringW(L"Other", L"Keep", L"yes", mod);
    if (!SyncModSettings(config, mod) || GetPrivateProfileIntW(L"FrameGeneration", L"MaxGeneratedFrames", 0, mod) != 2 || GetPrivateProfileIntW(L"Debug", L"MarkGeneratedFrames", 0, mod) != 1) ExitProcess(27);
    GetPrivateProfileStringW(L"Runtime", L"Obsolete", L"missing", value, 64, mod);
    if (lstrcmpW(value, L"missing")) ExitProcess(28);
    GetPrivateProfileStringW(L"Other", L"Keep", L"", value, 64, mod);
    if (lstrcmpW(value, L"yes")) ExitProcess(29);
    WritePrivateProfileStringW(L"SM86.Logging", L"Directory", L"%TEMP%", config);
    if (!SyncModSettings(config, mod)) ExitProcess(37);
    GetPrivateProfileStringW(L"Logging", L"Directory", L"", value, 64, mod);
    WCHAR expanded[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%TEMP%", expanded, MAX_PATH) || lstrcmpW(value, expanded)) ExitProcess(38);
    // Strict numeric settings: garbage and overflow keep the default.
    WritePrivateProfileStringW(L"Launcher", L"FixDLSSPresets", L"garbage", config);
    if (ReadConfigUInt(config, L"Launcher", L"FixDLSSPresets", 1, 0, 1) != 1) ExitProcess(31);
    WritePrivateProfileStringW(L"Launcher", L"FixDLSSPresets", L"42949672960", config);
    if (ReadConfigUInt(config, L"Launcher", L"FixDLSSPresets", 1, 0, 1) != 1) ExitProcess(32);
    WritePrivateProfileStringW(L"Launcher", L"FixDLSSPresets", L"0", config);
    if (ReadConfigUInt(config, L"Launcher", L"FixDLSSPresets", 1, 0, 1) != 0) ExitProcess(33);
    // Win32 INI APIs preserve Unicode destination data.
    HANDLE file = CreateFileW(mod, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    const WCHAR unicodeIni[] = L"\xfeff[Other]\r\nKeep=\x65e5\x672c\r\n";
    DWORD written;
    if (file == INVALID_HANDLE_VALUE || !WriteFile(file, unicodeIni, sizeof(unicodeIni) - sizeof(WCHAR), &written, NULL)) ExitProcess(34);
    CloseHandle(file);
    WritePrivateProfileStringW(NULL, NULL, NULL, mod);
    if (!SyncModSettings(config, mod)) ExitProcess(35);
    GetPrivateProfileStringW(L"Other", L"Keep", L"", value, 64, mod);
    if (lstrcmpW(value, L"\x65e5\x672c")) ExitProcess(36);
    // Dynamic resolution flag: the shipped IsSupported bytes resolve to RVA 0xe2f6f32;
    // any other code (a game update) resolves to nothing.
    static const BYTE shipped[47] = {
        0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x05, 0x1d, 0x43, 0x0d, 0x0a, 0x48, 0x8b, 0x88, 0x30, 0x0e,
        0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x11, 0x48, 0x8b, 0x01, 0xff, 0x50, 0x08, 0x84, 0xc0, 0x74,
        0x07, 0x32, 0xc0, 0x48, 0x83, 0xc4, 0x28, 0xc3, 0x0f, 0xb6, 0x05, 0x43, 0x61, 0xf0, 0x09 };
    if (FindDynamicResolutionFlag(shipped, 0x140000000 + DRS_IS_SUPPORTED_RVA) != 0x140000000 + 0xe2f6f32) ExitProcess(39);
    BYTE changed[47];
    memcpy(changed, shipped, sizeof(changed));
    changed[13] = 0x28; // a different field offset
    if (FindDynamicResolutionFlag(changed, 0x140000000 + DRS_IS_SUPPORTED_RVA) != 0) ExitProcess(40);
    const char report[] = "PASS: CVar/mod sync, deletion, preservation, Unicode, strict settings, DRS flag lookup\r\n";
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), report, sizeof(report) - 1, &written, NULL);
    DeleteFileW(path);
    DeleteFileW(config);
    DeleteFileW(mod);
    ExitProcess(0);
}

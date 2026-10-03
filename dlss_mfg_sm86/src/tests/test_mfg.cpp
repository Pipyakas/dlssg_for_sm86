#define DLSS_MFG_SM86_TEST
#include "../dlss_mfg_sm86.cpp"

static unsigned g_calls, g_seenCount, g_seenVersion;
static sl::DLSSGMode g_seenMode;
static float g_seenTarget;
static bool g_reject;

static void Reset(void) {
    g_calls = 0; g_reject = false; g_disabled = 0;
}

// ---- Mode 2 fakes ----
static sl::Result FakeOptions(const sl::ViewportHandle&, const sl::DLSSGOptions& options) {
    ++g_calls; g_seenCount = options.numFramesToGenerate; g_seenMode = options.mode;
    if (g_reject && g_calls == 1) return sl::Result::eErrorInvalidParameter;
    return sl::Result::eOk;
}
static sl::Result FakeState(const sl::ViewportHandle&, sl::DLSSGState& state, const sl::DLSSGOptions*) {
    state.numFramesActuallyPresented = 4;
    state.numFramesToGenerateMax = 3;
    return sl::Result::eOk;
}

// ---- Mode 1 fakes ----
static sl::Result DynamicFakeState(const sl::ViewportHandle&, sl::DLSSGState& state, const sl::DLSSGOptions*) {
    if (state.structVersion != 4) return sl::Result::eErrorInvalidParameter;
    state.bIsDynamicMFGSupported = sl::eTrue;
    state.numFramesToGenerateMax = 3;
    state.numFramesActuallyPresented = 2;
    return sl::Result::eOk;
}
static sl::Result DynamicFakeOptions(const sl::ViewportHandle&, const sl::DLSSGOptions& options) {
    ++g_calls; g_seenMode = options.mode; g_seenVersion = (unsigned)options.structVersion;
    g_seenTarget = options.dynamicTargetFrameRate; g_seenCount = options.numFramesToGenerate;
    if (options.mode == sl::DLSSGMode::eDynamic && g_reject) return sl::Result::eErrorInvalidParameter;
    return sl::Result::eOk;
}

static int TestRefreshBands(void) {
    WCHAR temp[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp) || !GetTempFileNameW(temp, L"mfg", 0, path)) return 1;
    struct { DWORD hz, count; } cases[] = {{0,0},{1,0},{59,1},{60,1},{61,2},{89,2},{90,2},{91,3},{119,3},{120,3}};
    for (auto item : cases) if (FramesAt(path, item.hz) != item.count) return 2;
    WritePrivateProfileStringW(L"FrameGeneration", L"LowRefreshMaxHz", L"90", path);
    if (FramesAt(path, 90) != 1 || FramesAt(path, 91) != 3) return 3;
    WritePrivateProfileStringW(L"FrameGeneration", L"LowRefreshMaxHz", L"garbage", path);
    if (FramesAt(path, 60) != 1 || FramesAt(path, 61) != 2) return 4;
    // An inverted pair clamps medium up to low, leaving no 3x band.
    WritePrivateProfileStringW(L"FrameGeneration", L"LowRefreshMaxHz", L"120", path);
    WritePrivateProfileStringW(L"FrameGeneration", L"MediumRefreshMaxHz", L"60", path);
    if (FramesAt(path, 120) != 1 || FramesAt(path, 121) != 3) return 5;
    DeleteFileW(path);
    return 0;
}

static int TestFixedMode(void) {
    Reset();
    g_options = FakeOptions; g_state = FakeState;
    g_runtimeMFG = TRUE; g_requested = 3; g_maximum = 3;
    sl::ViewportHandle viewport(0);
    SYSTEM_INFO system; GetSystemInfo(&system);
    BYTE* allocation = (BYTE*)VirtualAlloc(NULL, system.dwPageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD previous;
    if (!allocation || !VirtualProtect(allocation + system.dwPageSize, system.dwPageSize, PAGE_NOACCESS, &previous)) return 20;
    // Each historic input ends at a guard page. Any accidental read of newer
    // fields from the old object faults, rather than silently passing a canary.
    for (size_t version = 1; version <= 5; ++version) {
        size_t bytes = OptionsExtent(version);
        BYTE* location = allocation + system.dwPageSize - bytes;
        sl::DLSSGOptions seed;
        seed.structVersion = version;
        seed.mode = sl::DLSSGMode::eOn;
        seed.numFramesToGenerate = 1;
        memcpy(location, &seed, bytes);
        auto& incoming = *(sl::DLSSGOptions*)location;
        if (FixedSetOptions(viewport, incoming) != sl::Result::eOk || g_seenCount != 3 ||
            incoming.mode != sl::DLSSGMode::eOn || incoming.numFramesToGenerate != 1 || incoming.structVersion != version) return 21;
    }
    sl::DLSSGOptions incoming;
    incoming.mode = sl::DLSSGMode::eOff;
    FixedSetOptions(viewport, incoming);
    if (g_seenMode != sl::DLSSGMode::eOff || g_seenCount != 1) return 22;
    incoming.mode = sl::DLSSGMode::eDynamic;
    FixedSetOptions(viewport, incoming);
    if (g_seenMode != sl::DLSSGMode::eOn || g_seenCount != 3) return 23;
    g_maximum = 2; FixedSetOptions(viewport, incoming);
    if (g_seenCount != 2) return 24;
    g_requested = 0; FixedSetOptions(viewport, incoming);
    if (g_seenMode != sl::DLSSGMode::eDynamic || g_seenCount != 1) return 25;
    g_requested = 3; g_reject = true; g_calls = 0;
    FixedSetOptions(viewport, incoming);
    if (g_calls != 2 || g_disabled != 1 || g_seenMode != sl::DLSSGMode::eDynamic || g_seenCount != 1) return 26;
    g_disabled = 0; g_reject = false; g_runtimeMFG = FALSE;
    FixedSetOptions(viewport, incoming);
    if (g_seenCount != 1 || g_seenMode != sl::DLSSGMode::eDynamic) return 27;
    g_runtimeMFG = TRUE;
    // Historical state buffers are adapted without overwriting their headers
    // or touching their padding/canary bytes, and without an extra state query.
    for (size_t version : {1u, 3u, 4u}) {
        size_t body = StateBodyExtent(version);
        BYTE buffer[96]; memset(buffer, 0x5a, sizeof(buffer));
        sl::DLSSGState seed; seed.structVersion = version;
        memcpy(buffer, &seed, 32 + body);
        auto& state = *(sl::DLSSGState*)buffer;
        if (FixedGetState(viewport, state, NULL) != sl::Result::eOk || state.structVersion != version || state.numFramesActuallyPresented != 4 || g_maximum != 3) return 28;
        for (size_t i = 32 + body; i < sizeof(buffer); ++i) if (buffer[i] != 0x5a) return 29;
    }
    VirtualFree(allocation, 0, MEM_RELEASE);
    return 0;
}

static int TestDynamicMode(void) {
    Reset();
    g_options = DynamicFakeOptions; g_state = DynamicFakeState;
    g_support = -1; g_targetFPS = 0.0f;
    sl::ViewportHandle viewport(0);
    // Canary directly after an OLD 80-byte state object detects output overruns.
    alignas(8) char storage[88] = {};
    sl::DLSSGState initial;
    memcpy(storage, &initial, 80);
    sl::DLSSGState& oldState = *(sl::DLSSGState*)storage;
    oldState.structVersion = 3;
    memset(storage + 80, 0x5a, 8);
    if (DynamicGetState(viewport, oldState, NULL) != sl::Result::eOk || oldState.structVersion != 3 ||
        oldState.numFramesActuallyPresented != 2 || g_support != 1) return 30;
    for (int i = 80; i < 88; i++) if ((unsigned char)storage[i] != 0x5a) return 31;
    sl::DLSSGOptions oldOptions;
    oldOptions.structVersion = 3;
    oldOptions.mode = sl::DLSSGMode::eOn;
    oldOptions.numFramesToGenerate = 3;
    DynamicSetOptions(viewport, oldOptions);
    if (g_seenMode != sl::DLSSGMode::eDynamic || g_seenVersion != 5 || g_seenCount != 3 || g_seenTarget != 0.0f ||
        oldOptions.mode != sl::DLSSGMode::eOn || oldOptions.structVersion != 3) return 32;
    g_targetFPS = 60.0f;
    DynamicSetOptions(viewport, oldOptions);
    if (g_seenMode != sl::DLSSGMode::eDynamic || g_seenTarget != 60.0f) return 33;
    oldOptions.mode = sl::DLSSGMode::eOff;
    DynamicSetOptions(viewport, oldOptions);
    if (g_seenMode != sl::DLSSGMode::eOff) return 34;
    oldOptions.mode = sl::DLSSGMode::eOn;
    g_support = 0;
    DynamicSetOptions(viewport, oldOptions);
    if (g_seenMode != sl::DLSSGMode::eOn) return 35;
    g_support = 1; g_reject = true; g_calls = 0;
    DynamicSetOptions(viewport, oldOptions);
    if (g_seenMode != sl::DLSSGMode::eOn || g_calls != 2 || g_disabled != 1) return 36;
    g_calls = 0;
    DynamicSetOptions(viewport, oldOptions);
    if (g_seenMode != sl::DLSSGMode::eOn || g_calls != 1) return 37;
    return 0;
}

// Pre-2.7 SDKs (NG2B, Khazan) send v1 options and state. Each old options object
// ends at a guard page, as in TestFixedMode; the v1 state keeps its canary.
static int TestDynamicHistoric(void) {
    Reset();
    g_options = DynamicFakeOptions; g_state = DynamicFakeState;
    g_support = -1; g_targetFPS = 0.0f;
    sl::ViewportHandle viewport(0);
    BYTE buffer[96]; memset(buffer, 0x5a, sizeof(buffer));
    sl::DLSSGState seed; seed.structVersion = 1;
    memcpy(buffer, &seed, 32 + StateBodyExtent(1));
    auto& state = *(sl::DLSSGState*)buffer;
    if (DynamicGetState(viewport, state, NULL) != sl::Result::eOk || state.structVersion != 1 ||
        state.numFramesActuallyPresented != 2 || g_support != 1) return 110;
    for (size_t i = 32 + StateBodyExtent(1); i < sizeof(buffer); ++i) if (buffer[i] != 0x5a) return 111;
    SYSTEM_INFO system; GetSystemInfo(&system);
    BYTE* allocation = (BYTE*)VirtualAlloc(NULL, system.dwPageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD previous;
    if (!allocation || !VirtualProtect(allocation + system.dwPageSize, system.dwPageSize, PAGE_NOACCESS, &previous)) return 112;
    for (size_t version = 1; version <= 5; ++version) {
        size_t bytes = OptionsExtent(version);
        BYTE* location = allocation + system.dwPageSize - bytes;
        sl::DLSSGOptions options;
        options.structVersion = version;
        options.mode = sl::DLSSGMode::eOn;
        memcpy(location, &options, bytes);
        auto& incoming = *(sl::DLSSGOptions*)location;
        g_seenMode = sl::DLSSGMode::eOff; g_seenVersion = 0;
        if (DynamicSetOptions(viewport, incoming) != sl::Result::eOk || g_seenMode != sl::DLSSGMode::eDynamic ||
            g_seenVersion != 5 || incoming.mode != sl::DLSSGMode::eOn || incoming.structVersion != version) return 113;
    }
    VirtualFree(allocation, 0, MEM_RELEASE);
    return 0;
}

static int TestConfiguration(void) {
    WCHAR temp[MAX_PATH], config[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp) || !GetTempFileNameW(temp, L"mfg", 0, config)) return 40;
    ReadConfiguration(config);
    if (g_mode != 2 || !g_logging || g_sampleMs != 10000 || g_targetFPS != 0.0f) return 41;
    for (auto item : {std::make_pair(L"0", 0u), std::make_pair(L"1", 1u), std::make_pair(L" 2 ", 2u), std::make_pair(L"3", 3u)}) {
        WritePrivateProfileStringW(L"FrameGeneration", L"Mode", item.first, config);
        ReadConfiguration(config);
        if (g_mode != item.second) return 42;
    }
    // Anything unusable falls back to the documented default, Mode 2.
    for (const WCHAR* text : {L"4", L"-1", L"", L"junk", L"9999999999999999999"}) {
        WritePrivateProfileStringW(L"FrameGeneration", L"Mode", text, config);
        ReadConfiguration(config);
        if (g_mode != 2) return 43;
    }
    WritePrivateProfileStringW(L"FrameGeneration", L"TargetFPS", L"59.94", config);
    WritePrivateProfileStringW(L"Logging", L"Enabled", L"0", config);
    WritePrivateProfileStringW(L"Logging", L"SampleIntervalMs", L"250", config);
    ReadConfiguration(config);
    if (g_logging || g_sampleMs != 250 || g_targetFPS < 59.93f || g_targetFPS > 59.95f) return 44;
    for (const WCHAR* text : {L"nan", L"inf", L"-1", L"1001", L"60fps", L""}) {
        WritePrivateProfileStringW(L"FrameGeneration", L"TargetFPS", text, config);
        ReadConfiguration(config);
        if (g_targetFPS != 0) return 45;
    }
    WritePrivateProfileStringW(L"Logging", L"Directory", L"%TEMP%", config);
    if (!ConfigureLogPath(config)) return 46;
    WCHAR expandedDirectory[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%TEMP%", expandedDirectory, MAX_PATH) ||
        wcsncmp(g_log, expandedDirectory, wcslen(expandedDirectory))) return 47;
    // An omitted Directory must resolve beside the DLL's co-located INI,
    // regardless of the process working directory or machine/user name.
    WritePrivateProfileStringW(L"Logging", L"Directory", NULL, config);
    if (!ConfigureLogPath(config)) return 48;
    WCHAR actualLog[MAX_PATH], expectedLog[MAX_PATH];
    wcscpy_s(expectedLog, config);
    WCHAR* slash = wcsrchr(expectedLog, L'\\');
    if (!slash) return 49;
    slash[1] = 0;
    wcscat_s(expectedLog, MFG_LOG_NAME);
    if (!GetFullPathNameW(g_log, MAX_PATH, actualLog, NULL) || _wcsicmp(actualLog, expectedLog)) return 50;
    DeleteFileW(config);
    g_log[0] = 0;
    return 0;
}

static const WCHAR* volatile g_marker = L"dlss_mfg_sm86 image marker 7f3a";

static int TestImageScan(void) {
    HMODULE self = GetModuleHandleW(NULL);
    // The literal lives in a read-only data section of this test executable.
    if (!ImageHas(self, (const WCHAR*)g_marker)) return 60;
    // A string assembled at runtime exists only on the stack, never in the image.
    WCHAR absent[32];
    wcscpy_s(absent, L"dlss_mfg_sm86 absent ");
    wcscat_s(absent, L"9c1e");
    if (ImageHas(self, absent)) return 61;
    return 0;
}

static int TestLiveReload(void) {
    WCHAR temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp) || !GetTempFileNameW(temp, L"mfg", 0, g_config)) return 70;
    g_log[0] = 0;
    // Shipped bands at 120 Hz: 4x. Then the "one notch lower" bands: 3x at 120 Hz.
    ApplyConfiguration(120, "Test");
    if (g_mode != 2 || g_requested != 3) return 71;
    WritePrivateProfileStringW(L"FrameGeneration", L"LowRefreshMaxHz", L"80", g_config);
    WritePrivateProfileStringW(L"FrameGeneration", L"MediumRefreshMaxHz", L"120", g_config);
    g_disabled = 1;
    ApplyConfiguration(120, "Test");
    if (g_requested != 2 || g_disabled != 0) return 72;
    ApplyConfiguration(60, "Test");
    if (g_requested != 1) return 73;
    ApplyConfiguration(144, "Test");
    if (g_requested != 3) return 74;
    // Hooks dispatch on the live mode: 0 passes the request through untouched.
    g_options = FakeOptions; g_state = FakeState; g_runtimeMFG = TRUE; g_maximum = 3;
    sl::ViewportHandle viewport(0);
    sl::DLSSGOptions incoming;
    incoming.mode = sl::DLSSGMode::eOn; incoming.numFramesToGenerate = 1;
    WritePrivateProfileStringW(L"FrameGeneration", L"Mode", L"0", g_config);
    ApplyConfiguration(144, "Test");
    HookSetOptions(viewport, incoming);
    if (g_mode != 0 || g_requested != 0 || g_seenMode != sl::DLSSGMode::eOn || g_seenCount != 1) return 75;
    WritePrivateProfileStringW(L"FrameGeneration", L"Mode", L"2", g_config);
    ApplyConfiguration(144, "Test");
    HookSetOptions(viewport, incoming);
    if (g_seenMode != sl::DLSSGMode::eOn || g_seenCount != 3) return 76;
    // Mode 1's state query keeps the ceiling current for a later switch to Mode 2.
    g_maximum = 1; g_state = DynamicFakeState;
    sl::DLSSGState state; state.structVersion = 4;
    WritePrivateProfileStringW(L"FrameGeneration", L"Mode", L"1", g_config);
    ApplyConfiguration(144, "Test");
    HookGetState(viewport, state, NULL);
    if (g_maximum != 3) return 78;
    g_options = DynamicFakeOptions; g_support = 1;
    WritePrivateProfileStringW(L"FrameGeneration", L"Mode", L"1", g_config);
    ApplyConfiguration(144, "Test");
    incoming.structVersion = 3;
    HookSetOptions(viewport, incoming);
    if (g_seenMode != sl::DLSSGMode::eDynamic) return 77;
    // [ConsoleVariables]: ignored outside Unreal; integers, floats, comments and
    // malformed values.
    WritePrivateProfileStringW(L"ConsoleVariables", L"r.Test.Ignored", L"1", g_config);
    g_unreal = FALSE; ApplyForcedConsoleVariables(TRUE);
    if (g_forcedCount != 0) return 79;
    WritePrivateProfileStringW(L"ConsoleVariables", NULL, NULL, g_config);
    g_unreal = TRUE;
    WritePrivateProfileStringW(L"ConsoleVariables", L"r.Test.Integer ", L"1", g_config);
    WritePrivateProfileStringW(L"ConsoleVariables", L"r.Test.Float", L"1.5", g_config);
    WritePrivateProfileStringW(L"ConsoleVariables", L"r.Test.Bad", L"on", g_config);
    WritePrivateProfileStringW(L"ConsoleVariables", L";r.Test.Comment", L"1", g_config);
    ApplyForcedConsoleVariables(TRUE);
    if (g_forcedCount != 2 || wcscmp(g_forcedNames[0], L"r.Test.Integer") || g_forcedBits[0] != 1 || g_forced[0].isFloat ||
        wcscmp(g_forcedNames[1], L"r.Test.Float") || g_forcedBits[1] != FloatBits(1.5f) || !g_forced[1].isFloat ||
        !g_forced[0].searched || g_forced[0].data) return 79;
    WritePrivateProfileStringW(L"ConsoleVariables", NULL, NULL, g_config);
    ApplyForcedConsoleVariables(TRUE);
    if (g_forcedCount != 0) return 79;
    DeleteFileW(g_config);
    g_config[0] = 0;
    return 0;
}

// Stand-in for a UE console object: its first slot is a vtable inside this image.
static void FakeVirtual(void) {}
struct FakeConsoleObject { void* vtable; };

static int TestGovernor(void) {
    LONG pending = 0, streak = 0;
    // CPU-limited 40 fps into 120 Hz: 3x (2 generated) stays within the target.
    if (ChooseFrames(40, 120, 2, 1, 3, 0.1, &pending, &streak, 1) != 2) return 80;
    // 30 fps base: 4x after one settle decision; with settle=2 it waits a decision.
    if (ChooseFrames(30, 120, 1, 1, 3, 0.1, &pending, &streak, 2) != 1) return 81;
    if (ChooseFrames(30, 120, 1, 1, 3, 0.1, &pending, &streak, 2) != 3) return 82;
    // 70 fps base: 2x; 125 fps base: clamped to the 1-frame minimum.
    pending = streak = 0;
    if (ChooseFrames(70, 120, 3, 1, 3, 0.1, &pending, &streak, 1) != 1) return 83;
    if (ChooseFrames(125, 120, 2, 1, 3, 0.1, &pending, &streak, 1) != 1) return 84;
    // Ceiling and unknown inputs.
    if (ChooseFrames(20, 120, 2, 1, 2, 0.1, &pending, &streak, 1) != 2) return 85;
    if (ChooseFrames(0, 120, 2, 1, 3, 0.1, &pending, &streak, 1) != 2) return 86;

    // Cost model, Halo's numbers: CPU limit 8 ms, generation 9 ms per frame.
    // Low resolution (render 4 ms): 2x ~154 fps beats 3x ~136 and 4x ~129.
    if (ChooseByCost(8, 4, 9, 120, 0.1, 1, 3) != 1) return 97;
    // Cheap generation (2 ms) with a slow CPU (20 ms, 50 fps): 3x already passes the
    // 132 fps cap (150), so 4x adds nothing; 2x (100) falls short.
    if (ChooseByCost(20, 6, 2, 120, 0.1, 1, 3) != 2) return 98;
    // Slower CPU (30 ms, 33 fps): only 4x reaches the cap (133 → 132).
    if (ChooseByCost(30, 6, 2, 120, 0.1, 1, 3) != 3) return 101;
    // Frames first: 4x at 120 Hz (30 fps base), fewer when the base would drop
    // below the minimum.
    if (ChooseMostFrames(120, 30, 1, 3) != 3 || ChooseMostFrames(120, 40, 1, 3) != 2 ||
        ChooseMostFrames(60, 30, 1, 3) != 1 || ChooseMostFrames(144, 30, 1, 5) != 3 ||
        ChooseMostFrames(120, 30, 2, 2) != 2) return 103;
    // DLSS levels: down after 2 decisions over the goal, then a cooldown; up after
    // `settle` decisions with the level above predicted to fit; range limits.
    LevelState ls = {};
    if (ChooseDlssLevel(1, 36, 30, 33, 67, &ls, 2, 3) != 1 || ChooseDlssLevel(1, 36, 30, 33, 67, &ls, 2, 3) != 0 ||
        ls.cooldown != 3) return 104;
    // gpuAt[1] remembers ~36 ms at Performance: no step up while that wouldn't fit.
    for (int i = 0; i < 6; ++i) if (ChooseDlssLevel(0, 20, 30, 33, 67, &ls, 2, 3) != 0) return 104;
    ls = LevelState{};  // nothing known: 20 ms at 33% predicts 20 * (0.5 + 0.5 * 2.25) = 32.5 > 29.1
    if (ChooseDlssLevel(0, 20, 30, 33, 67, &ls, 2, 3) != 0 || ChooseDlssLevel(0, 20, 30, 33, 67, &ls, 2, 3) != 0) return 104;
    ls = LevelState{};  // 16 ms predicts 26 ms: up after two decisions
    if (ChooseDlssLevel(0, 16, 30, 33, 67, &ls, 2, 3) != 0 || ChooseDlssLevel(0, 16, 30, 33, 67, &ls, 2, 3) != 1) return 104;
    ls = LevelState{};  // Quality is the top of 33-67; DLAA is skipped
    if (ChooseDlssLevel(3, 10, 30, 33, 67, &ls, 1, 3) != 3 || ChooseDlssLevel(4, 10, 30, 33, 67, &ls, 1, 3) != 3) return 104;
    if (NearestDlssLevel(33.33f) != 0 || NearestDlssLevel(52) != 1 || NearestDlssLevel(67) != 3) return 104;
    // Two counts both reach the cap: the smaller one wins.
    if (ChooseByCost(8, 2, 1, 120, 0.1, 1, 3) != 1) return 99;
    LONG p = 0, k = 0;
    if (Settle(2, 1, &p, &k, 2) != 1 || Settle(2, 1, &p, &k, 2) != 2) return 100;

    // Reflex summary: four 12.5 ms frames, 6 ms simulation and 8 ms submit on
    // pipelined threads (CPU limit 8 ms, even though sim start → submit end spans
    // 20 ms of mostly waiting), 10 ms GPU busy.
    static sl::ReflexState state;
    for (int i = 0; i < 4; ++i) {
        sl::ReflexReport& r = state.frameReport[60 + i];
        r.frameID = 100 + i; r.simStartTime = 1000000 + i * 12500;
        r.simEndTime = r.simStartTime + 6000;
        r.renderSubmitStartTime = r.simStartTime + 12000; r.renderSubmitEndTime = r.renderSubmitStartTime + 8000;
        r.gpuActiveRenderTimeUs = 10000;
    }
    FrameTimes t;
    if (!SummarizeReports(state, &t) || fabs(t.cpuMs - 8.0) > 0.01 || fabs(t.simMs - 6.0) > 0.01 ||
        fabs(t.submitMs - 8.0) > 0.01 || fabs(t.gpuMs - 10.0) > 0.01 ||
        fabs(t.frameMs - 12.5) > 0.01 || t.samples != 4) return 87;
    // The CPU limit ignores frames stretched by GPU stalls (25th percentile).
    state.frameReport[63].renderSubmitEndTime = state.frameReport[63].renderSubmitStartTime + 30000;
    if (!SummarizeReports(state, &t) || fabs(t.cpuMs - 8.0) > 0.01) return 96;
    state.frameReport[61].frameID = 0; // a gap leaves too few consecutive frames
    state.frameReport[62].frameID = 0;
    if (SummarizeReports(state, &t)) return 88;

    // Console variable validation on synthetic objects, both accepted layouts.
    FakeConsoleObject target = {(void*)FakeVirtual};
    BYTE* image = (BYTE*)GetModuleHandleW(NULL);
    size_t imageSize = ((IMAGE_NT_HEADERS64*)(image + ((IMAGE_DOS_HEADER*)image)->e_lfanew))->OptionalHeader.SizeOfImage;
    LONG budget[2]; float value = 33.3f; memcpy(&budget[0], &value, 4); budget[1] = budget[0];
    void* withVtable[3] = {(void*)FakeVirtual, &target, budget};
    void* plain[3] = {&target, budget, NULL};
    if (ConsoleVariableData((BYTE*)withVtable, image, imageSize, TRUE) != budget) return 89;
    if (ConsoleVariableData((BYTE*)plain, image, imageSize, TRUE) != budget) return 90;
    budget[1] = 0; // thread copies disagree: rejected
    if (ConsoleVariableData((BYTE*)withVtable, image, imageSize, TRUE)) return 91;
    LONG mode[2] = {7, 7}; // out of range for OperationMode
    void* badMode[3] = {(void*)FakeVirtual, &target, mode};
    if (ConsoleVariableData((BYTE*)badMode, image, imageSize, FALSE)) return 92;
    FakeConsoleObject foreign = {&mode}; // vtable outside the image
    void* badTarget[3] = {(void*)FakeVirtual, &foreign, budget};
    budget[1] = budget[0];
    if (ConsoleVariableData((BYTE*)badTarget, image, imageSize, TRUE)) return 93;

    // Write then restore through a found variable.
    ConsoleVariable cvar = {L"test", TRUE, budget, 0, FALSE};
    WriteConsoleVariable(cvar, FloatBits(12.0f));
    float written; memcpy(&written, &budget[1], 4);
    if (written != 12.0f || !cvar.changed) return 94;
    g_cvars[0] = cvar;
    RestoreConsoleVariables();
    float restored; memcpy(&restored, &budget[0], 4);
    if (restored != 33.3f || budget[0] != budget[1] || g_cvars[0].changed) return 95;
    g_cvars[0].data = NULL;
    return 0;
}

int main(void) {
    int result = TestImageScan();
    if (!result) result = TestGovernor();
    if (!result) result = TestLiveReload();
    if (!result) result = TestRefreshBands();
    if (!result) result = TestFixedMode();
    if (!result) result = TestDynamicMode();
    if (!result) result = TestDynamicHistoric();
    if (!result) result = TestConfiguration();
    if (result) { printf("FAIL: code %d\n", result); return result; }
    puts("PASS: image marker scan; Mode 3 governor, Reflex summary, console variable validation; live reload and mode dispatch; refresh bands; Mode 2 v1-v5 guard-page conversion, state canaries, Off/cap/unsupported/rejection; "
         "Mode 1 dynamic upgrade, v1-v5 guard-page conversion, v1 state canary, target FPS, Off/unsupported/rejection; mode, target FPS and log-path config");
    return 0;
}

// dlss_mfg_sm86: multi-frame generation control for Streamline games that
// have no MFG UI. Loaded as a forwarding proxy DLL (or injected), it hooks the
// documented Streamline DLSS-G options/state API and rewrites only the frame
// generation requests the game itself makes. FG on/off and Reflex stay under
// the game's control.
//   Mode 0  pass-through
//   Mode 1  NVIDIA Dynamic MFG: on/auto requests become eDynamic
//   Mode 2  fixed count chosen from the primary display refresh rate
//   Mode 3  experimental governor: by default the most generated frames that keep
//           the base rate >= MinBaseFPS; in Unreal games dynamic resolution then
//           fills the GPU time left at that base rate
// Active in any game that loads Streamline; the console-variable features
// (Mode 3 dynamic resolution, [ConsoleVariables]) need an Unreal executable.
// INI edits apply live (re-read every [Reload] IntervalMs), and Mode 2 follows
// refresh rate changes of the primary display without a restart.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <float.h>
#include <math.h>
#include <algorithm>
#include "sl_dlss_g.h"
#include "sl_reflex.h"
#include "MinHook.h"

// These checks are deliberately tied to the official 2.14.1 x64 headers.
static_assert(sizeof(sl::BaseStructure) == 32, "Unexpected x64 SL header");
static_assert(sizeof(sl::DLSSGState) == 88, "Unexpected state ABI");
static_assert(sizeof(sl::DLSSGOptions) == 120, "Unexpected options ABI");
static_assert(offsetof(sl::DLSSGOptions, numFramesToGenerate) == 36, "Unexpected count offset");
static_assert(offsetof(sl::DLSSGOptions, bReserved15) == 104, "Unexpected v1 extent");
static_assert(offsetof(sl::DLSSGOptions, enableUserInterfaceRecomposition) == 112, "Unexpected v3 prefix");
static_assert(offsetof(sl::DLSSGState, numFramesToGenerateMax) == 52, "Unexpected v1 state extent");
static_assert(offsetof(sl::DLSSGState, bIsDynamicMFGSupported) == 80, "Unexpected v3 state prefix");

#define MFG_CONFIG_NAME L"dlss_mfg_sm86.ini"
#define MFG_LOG_NAME L"dlss_mfg_sm86.log"

static WCHAR g_directory[MAX_PATH], g_config[MAX_PATH], g_log[MAX_PATH];
static SRWLOCK g_logLock = SRWLOCK_INIT;
// Mode and target FPS are re-read on a live INI reload while hooks run.
static volatile DWORD g_mode = 2;
static DWORD g_sampleMs = 10000;
static BOOL g_logging = TRUE, g_runtimeMFG = FALSE;
static volatile float g_targetFPS = 0.0f;
// Mode 2: requested generated frames and the runtime's reported ceiling.
static volatile LONG g_requested = 0, g_maximum = 1;
// Mode 1: whether the runtime reports Dynamic MFG support (-1 = not yet known).
static volatile LONG g_support = -1;
// Set when the runtime rejects an override; the original request is then used.
static volatile LONG g_disabled = 0;
// Bumped by every configuration (re)load so the hooks log their next request.
static volatile LONG g_epoch = 0;
// Mode 3: while it drives UE dynamic resolution, frame generation must be told
// (DLSSGFlags::eDynamicResolutionEnabled + the fixed internal resolution, here a
// percent of the output; 0 = runtime default, half).
static volatile LONG g_dynamicResolution = 0, g_dynamicResPercent = 0;
// Unreal Engine executable (UE4.22+ / UE5): enables the console-variable
// features (Mode 3 dynamic resolution, [ConsoleVariables]). Other Streamline
// games get the frame generation modes only.
static BOOL g_unreal = FALSE;
static volatile LONG g_stateInstalled = 0, g_optionsInstalled = 0;
static PFun_slGetFeatureFunction* g_resolve;
static PFun_slDLSSGSetOptions* g_options;
static PFun_slDLSSGGetState* g_state;

// Strict decimal parsing: malformed and out-of-range settings use the default,
// rather than accidentally turning a feature off through the Win32 int parser.
static DWORD Setting(const WCHAR* path, const WCHAR* section, const WCHAR* key,
    DWORD fallback, DWORD minimum, DWORD maximum) {
    WCHAR text[32];
    DWORD length = GetPrivateProfileStringW(section, key, L"", text, 32, path);
    if (!length || length >= 31) return fallback;
    DWORD value = 0;
    BOOL digits = FALSE;
    const WCHAR* p = text;
    while (*p == L' ' || *p == L'\t') ++p;
    while (*p >= L'0' && *p <= L'9') {
        DWORD digit = *p++ - L'0';
        if (digit > maximum || value > (maximum - digit) / 10) return fallback;
        value = value * 10 + digit;
        digits = TRUE;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    return digits && !*p && value >= minimum && value <= maximum ? value : fallback;
}

// Thresholds are inclusive upper bounds: 2x up to LowRefreshMaxHz, 3x up to
// MediumRefreshMaxHz, 4x above. An unknown refresh rate (0) selects nothing.
static DWORD FramesAt(const WCHAR* path, DWORD hz) {
    if (hz < 2) return 0;
    DWORD low = Setting(path, L"FrameGeneration", L"LowRefreshMaxHz", 60, 2, 1000);
    DWORD medium = Setting(path, L"FrameGeneration", L"MediumRefreshMaxHz", 90, 2, 1000);
    if (medium < low) medium = low;
    const WCHAR* key = hz > medium ? L"HighRefreshGeneratedFrames" :
        hz > low ? L"MediumRefreshGeneratedFrames" : L"LowRefreshGeneratedFrames";
    return Setting(path, L"FrameGeneration", key, hz > medium ? 3 : hz > low ? 2 : 1, 1, 3);
}

// Choose the PRIMARY display, not an arbitrary adapter output. Streaming makes
// its virtual monitor primary before launching the game.
static DWORD PrimaryHz(void) {
    DISPLAY_DEVICEW device;
    DEVMODEW mode;
    for (DWORD i = 0; ; ++i) {
        ZeroMemory(&device, sizeof(device)); device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(NULL, i, &device, 0)) return 0;
        if (!(device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) continue;
        ZeroMemory(&mode, sizeof(mode)); mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode) &&
            (mode.dmFields & DM_DISPLAYFREQUENCY) && mode.dmDisplayFrequency > 1)
            return mode.dmDisplayFrequency;
        return 0;
    }
}

static void ReadConfiguration(const WCHAR* path) {
    g_mode = Setting(path, L"FrameGeneration", L"Mode", 2, 0, 3);
    g_logging = Setting(path, L"Logging", L"Enabled", 1, 0, 1);
    g_sampleMs = Setting(path, L"Logging", L"SampleIntervalMs", 10000, 100, 3600000);
    WCHAR text[64];
    DWORD length = GetPrivateProfileStringW(L"FrameGeneration", L"TargetFPS", L"0", text, 64, path);
    WCHAR* end;
    double value = wcstod(text, &end);
    while (*end == L' ' || *end == L'\t') end++;
    g_targetFPS = length && length < 63 && end != text && !*end && _finite(value) &&
        value >= 0.0 && value <= 1000.0 ? (float)value : 0.0f;
}

// Relative directories resolve beside the INI, never the game's working directory.
static bool ConfigureLogPath(const WCHAR* configPath) {
    WCHAR directory[MAX_PATH], expanded[MAX_PATH];
    DWORD length = GetPrivateProfileStringW(L"Logging", L"Directory", L".", directory, MAX_PATH, configPath);
    if (!length || length >= MAX_PATH - 1) return false;
    DWORD count = ExpandEnvironmentStringsW(directory, expanded, MAX_PATH);
    if (!count || count > MAX_PATH) return false;
    if (expanded[0] != L'\\' && !(expanded[0] && expanded[1] == L':')) {
        WCHAR parent[MAX_PATH];
        wcscpy_s(parent, configPath);
        WCHAR* slash = wcsrchr(parent, L'\\');
        if (!slash) return false;
        slash[1] = 0;
        if (wcslen(parent) + wcslen(expanded) >= MAX_PATH) return false;
        wcscat_s(parent, expanded);
        wcscpy_s(expanded, parent);
    }
    if (wcslen(expanded) + wcslen(MFG_LOG_NAME) + 2 >= MAX_PATH) return false;
    if (!CreateDirectoryW(expanded, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    DWORD attributes = GetFileAttributesW(expanded);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    wcscpy_s(g_log, expanded);
    if (g_log[wcslen(g_log) - 1] != L'\\') wcscat_s(g_log, L"\\");
    wcscat_s(g_log, MFG_LOG_NAME);
    return true;
}

static void Log(const char* format, ...) {
    if (!g_logging || !g_log[0]) return;
    char text[2048];
    int prefix = sprintf_s(text, "pid=%lu tick=%llu ", GetCurrentProcessId(), GetTickCount64());
    va_list args; va_start(args, format);
    vsnprintf_s(text + prefix, sizeof(text) - prefix, _TRUNCATE, format, args);
    va_end(args); strcat_s(text, "\r\n");
    AcquireSRWLockExclusive(&g_logLock);
    HANDLE file = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written; WriteFile(file, text, (DWORD)strlen(text), &written, NULL);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

// ---- Structure versions -----------------------------------------------------

static size_t OptionsExtent(size_t version) {
    // Copy valid members only, not a historical structure's trailing padding:
    // new fields can reuse that padding. Never read beyond the old allocation.
    const size_t bytes[] = {0, 104, 105, 112, 113, 120};
    return version < _countof(bytes) ? bytes[version] : 0;
}

static size_t StateBodyExtent(size_t version) {
    // v2 is left untouched: only the verified v1/v3/v4 layouts are adapted.
    return version == 1 ? 20 : version == 3 ? 48 : version == 4 ? 56 : 0;
}

// ---- Mode 1: NVIDIA Dynamic MFG -------------------------------------------

// Upgrade the output object on the same thread and same GetState call that the
// game already makes. This avoids an extra query consuming its frame counters.
// Pre-2.7 SDKs (NG2B, Khazan) pass v1 objects; they get the v1 members back.
static sl::Result DynamicGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state, const sl::DLSSGOptions* options) {
    size_t version = state.structVersion;
    const size_t bytes = StateBodyExtent(version);
    if (!bytes) return g_state(viewport, state, options);
    sl::DLSSGState upgraded;
    upgraded.next = state.next;
    sl::Result result = g_state(viewport, upgraded, options);
    if (result == sl::Result::eOk) {
        // Keep the caller's header/version and copy only its allocated body.
        memcpy((char*)&state + 32, (char*)&upgraded + 32, bytes);
        LONG support = upgraded.bIsDynamicMFGSupported == sl::eTrue ? 1 : 0;
        LONG previous = InterlockedExchange(&g_support, support);
        // Keep the ceiling current so a live switch to Mode 2 starts at the right count.
        LONG maximum = (LONG)upgraded.numFramesToGenerateMax;
        InterlockedExchange(&g_maximum, maximum >= 1 && maximum <= 16 ? maximum : 1);
        static DWORD previousStatus = ~0u, previousMax = ~0u;
        static ULONGLONG lastSample = 0;
        ULONGLONG now = GetTickCount64();
        if (previous != support || previousStatus != (DWORD)upgraded.status || previousMax != upgraded.numFramesToGenerateMax || now - lastSample >= g_sampleMs) {
            Log("State callerVersion=%zu dynamicSupported=%ld maxGenerated=%u status=0x%x presented=%u vsyncAvailable=%u", version, support, upgraded.numFramesToGenerateMax, (unsigned)upgraded.status, upgraded.numFramesActuallyPresented, (unsigned)upgraded.bIsVsyncSupportAvailable);
            previousStatus = (DWORD)upgraded.status;
            previousMax = upgraded.numFramesToGenerateMax;
            lastSample = now;
        }
    } else {
        static int previousResult = -1;
        if (previousResult != (int)result) Log("State result=%d callerVersion=%zu", (int)result, version);
        previousResult = (int)result;
    }
    return result;
}

static sl::Result DynamicSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
    const size_t bytes = OptionsExtent(options.structVersion);
    bool dynamic = !g_disabled && g_support == 1 && bytes &&
        (options.mode == sl::DLSSGMode::eOn || options.mode == sl::DLSSGMode::eAuto);
    sl::Result result;
    if (dynamic) {
        // Members newer than the caller's version keep their v5 defaults.
        sl::DLSSGOptions upgraded;
        memcpy(&upgraded, &options, bytes);
        upgraded.structVersion = 5;
        upgraded.mode = sl::DLSSGMode::eDynamic;
        upgraded.dynamicTargetFrameRate = g_targetFPS;
        result = g_options(viewport, upgraded);
        if (result != sl::Result::eOk) {
            Log("Dynamic request rejected result=%d; override disabled, original request retried", (int)result);
            InterlockedExchange(&g_disabled, 1);
            dynamic = false;
            result = g_options(viewport, options);
        }
    } else {
        result = g_options(viewport, options);
    }
    static int previousMode = -1, previousResult = -1, previousFrames = -1, previousDynamic = -1;
    static LONG previousEpoch = -1;
    if (previousEpoch != g_epoch || previousMode != (int)options.mode || previousResult != (int)result || previousFrames != (int)options.numFramesToGenerate || previousDynamic != (int)dynamic) {
        previousEpoch = g_epoch;
        Log("Options callerVersion=%zu mode=%u incomingGenerated=%u effectiveMode=%u dynamicOverride=%u targetFPS=%.3f result=%d", options.structVersion, (unsigned)options.mode, options.numFramesToGenerate, dynamic ? (unsigned)sl::DLSSGMode::eDynamic : (unsigned)options.mode, (unsigned)dynamic, g_targetFPS, (int)result);
        previousMode = (int)options.mode;
        previousFrames = (int)options.numFramesToGenerate;
        previousResult = (int)result;
        previousDynamic = dynamic;
    }
    return result;
}

// ---- Mode 2: refresh-based fixed count ------------------------------------

static sl::Result FixedGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& output,
    const sl::DLSSGOptions* options) {
    const size_t bytes = StateBodyExtent(output.structVersion);
    if (!g_runtimeMFG || !bytes) return g_state(viewport, output, options);
    sl::DLSSGState current;
    current.structVersion = output.structVersion == 4 ? 4 : 3;
    current.next = output.next;
    sl::Result result = g_state(viewport, current, options);
    if (result != sl::Result::eOk) {
        Log("State upgrade rejected result=%d; retrying original state", (int)result);
        return g_state(viewport, output, options);
    }
    memcpy((char*)&output + 32, (char*)&current + 32, bytes);
    LONG maximum = (LONG)current.numFramesToGenerateMax;
    if (maximum < 1 || maximum > 16) maximum = 1;
    LONG previous = InterlockedExchange(&g_maximum, maximum);
    static ULONGLONG sampled = 0;
    static DWORD lastStatus = ~0u;
    ULONGLONG now = GetTickCount64();
    if (previous != maximum || lastStatus != (DWORD)current.status || now - sampled >= g_sampleMs) {
        Log("State callerVersion=%zu maxGenerated=%ld status=0x%x presented=%u",
            output.structVersion, maximum, (unsigned)current.status, current.numFramesActuallyPresented);
        sampled = now; lastStatus = (DWORD)current.status;
    }
    return result;
}

static sl::Result FixedSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& incoming) {
    const size_t extent = OptionsExtent(incoming.structVersion);
    bool overrideCount = g_runtimeMFG && !g_disabled && g_requested > 0 && extent &&
        (incoming.mode == sl::DLSSGMode::eOn || incoming.mode == sl::DLSSGMode::eAuto ||
         incoming.mode == sl::DLSSGMode::eDynamic);
    DWORD selected = (DWORD)InterlockedCompareExchange(&g_requested, 0, 0);
    DWORD maximum = (DWORD)InterlockedCompareExchange(&g_maximum, 0, 0);
    DWORD frames = std::min(selected, maximum);
    sl::Result result;
    if (overrideCount) {
        sl::DLSSGOptions replacement;
        memcpy(&replacement, &incoming, extent);
        // Upgrade v1/v2 callers to a verified fixed-MFG layout. v4/v5 callers
        // retain their version and members. Incoming data is never modified.
        if (replacement.structVersion < 3) replacement.structVersion = 3;
        replacement.mode = sl::DLSSGMode::eOn;
        replacement.numFramesToGenerate = frames;
        if (g_dynamicResolution) {
            // Streamline DLSS-G guide §10: required while depth/motion-vector extents
            // change per frame; the internal resolution must stay fixed.
            replacement.flags = replacement.flags | sl::DLSSGFlags::eDynamicResolutionEnabled;
            LONG percent = g_dynamicResPercent;
            replacement.dynamicResWidth = percent && replacement.colorWidth ? replacement.colorWidth * percent / 100 : 0;
            replacement.dynamicResHeight = percent && replacement.colorHeight ? replacement.colorHeight * percent / 100 : 0;
        }
        result = g_options(viewport, replacement);
        if (result != sl::Result::eOk) {
            Log("Fixed MFG rejected result=%d; override disabled, original request retried", (int)result);
            InterlockedExchange(&g_disabled, 1);
            overrideCount = false;
            result = g_options(viewport, incoming);
        }
    } else result = g_options(viewport, incoming);
    static int previousMode = -1, previousCount = -1, previousResult = -1;
    static LONG previousEpoch = -1;
    DWORD effective = overrideCount ? frames : incoming.numFramesToGenerate;
    if (previousEpoch != g_epoch || previousMode != (int)incoming.mode || previousCount != (int)effective || previousResult != (int)result) {
        previousEpoch = g_epoch;
        Log("Options callerVersion=%zu mode=%u incomingGenerated=%u selectedGenerated=%ld effectiveGenerated=%u override=%u result=%d",
            incoming.structVersion, (unsigned)incoming.mode, incoming.numFramesToGenerate,
            g_requested, effective, (unsigned)overrideCount, (int)result);
        previousMode = (int)incoming.mode; previousCount = (int)effective; previousResult = (int)result;
    }
    return result;
}

// ---- Hook installation -----------------------------------------------------

// Fixed multi-frame counts need Streamline 2.7 or newer.
static BOOL HasMFGVersion(void* function) {
    WCHAR path[MAX_PATH]; HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)function, &module) || !GetModuleFileNameW(module, path, MAX_PATH)) return FALSE;
    DWORD size = GetFileVersionInfoSizeW(path, NULL);
    if (!size) return FALSE;
    void* data = HeapAlloc(GetProcessHeap(), 0, size);
    if (!data) return FALSE;
    VS_FIXEDFILEINFO* version = NULL; UINT length;
    BOOL ok = GetFileVersionInfoW(path, 0, size, data) && VerQueryValueW(data, L"\\", (void**)&version, &length);
    BOOL supported = ok && HIWORD(version->dwFileVersionMS) == 2 && LOWORD(version->dwFileVersionMS) >= 7;
    if (ok) Log("Runtime %ls version=%u.%u.%u.%u fixedMFG=%u", path,
        HIWORD(version->dwFileVersionMS), LOWORD(version->dwFileVersionMS),
        HIWORD(version->dwFileVersionLS), LOWORD(version->dwFileVersionLS), (unsigned)supported);
    HeapFree(GetProcessHeap(), 0, data);
    return supported;
}

// ---- Live configuration --------------------------------------------------

// Re-reads the INI and picks the Mode 2 count for the given refresh rate. A
// fresh configuration also clears an earlier override rejection.
static void ApplyConfiguration(DWORD hz, const char* reason) {
    ReadConfiguration(g_config);
    // Mode 3 starts from the refresh band and keeps its governor's choice afterwards.
    LONG frames = g_mode == 2 ? (LONG)FramesAt(g_config, hz) :
        g_mode == 3 ? (g_requested > 0 ? g_requested : (LONG)FramesAt(g_config, hz)) : 0;
    InterlockedExchange(&g_requested, frames);
    InterlockedExchange(&g_disabled, 0);
    InterlockedIncrement(&g_epoch);
    Log("%s Mode=%lu refreshHz=%lu selectedGenerated=%ld multiplier=%ld targetFPS=%.3f",
        reason, g_mode, hz, frames, frames ? frames + 1 : 0, g_targetFPS);
}

static BOOL ConfigStamp(FILETIME* stamp) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(g_config, GetFileExInfoStandard, &data)) return FALSE;
    *stamp = data.ftLastWriteTime;
    return TRUE;
}

// Applies INI edits and, in Mode 2, primary display refresh changes (e.g. a
// stream starting mid-game) without a restart.
static void ApplyForcedConsoleVariables(BOOL reread);

static DWORD WINAPI Reloader(void* initialHz) {
    DWORD hz = (DWORD)(ULONG_PTR)initialHz;
    FILETIME seen = {};
    ConfigStamp(&seen);
    ApplyForcedConsoleVariables(TRUE);
    for (;;) {
        DWORD interval = Setting(g_config, L"Reload", L"IntervalMs", 1000, 0, 60000);
        if (!interval) return 0;
        Sleep(interval);
        FILETIME stamp = {};
        BOOL edited = ConfigStamp(&stamp) && CompareFileTime(&stamp, &seen) != 0;
        DWORD current = hz;
        if (g_mode == 2 && Setting(g_config, L"Reload", L"FollowRefreshRate", 1, 0, 1)) {
            DWORD probe = PrimaryHz();
            if (probe) current = probe;
        }
        if (edited) {
            seen = stamp;
            // Let the editor finish writing before reading.
            Sleep(100);
            ApplyConfiguration(current, "Reload");
        } else if (current != hz) {
            ApplyConfiguration(current, "RefreshChange");
        }
        ApplyForcedConsoleVariables(edited);
        hz = current;
    }
}

// Both hooks are always installed and dispatch on the current mode, so a live
// reload can switch between all modes. Mode 3 applies its governor's count
// through the fixed-count path.
static sl::Result HookGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state, const sl::DLSSGOptions* options) {
    DWORD mode = g_mode;
    return mode == 1 ? DynamicGetState(viewport, state, options) :
        mode >= 2 ? FixedGetState(viewport, state, options) : g_state(viewport, state, options);
}

static sl::Result HookSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
    DWORD mode = g_mode;
    return mode == 1 ? DynamicSetOptions(viewport, options) :
        mode >= 2 ? FixedSetOptions(viewport, options) : g_options(viewport, options);
}

static void Install(const char* name, void* function) {
    if (!name || !function) return;
    volatile LONG* flag;
    void* hook; void** original;
    if (!strcmp(name, "slDLSSGGetState")) {
        flag = &g_stateInstalled; original = (void**)&g_state; hook = (void*)HookGetState;
    } else if (!strcmp(name, "slDLSSGSetOptions")) {
        flag = &g_optionsInstalled; original = (void**)&g_options; hook = (void*)HookSetOptions;
    } else return;
    if (InterlockedCompareExchange(flag, 1, 0)) return;
    g_runtimeMFG = HasMFGVersion(function);
    MH_STATUS status = MH_CreateHook(function, hook, original);
    if (status == MH_OK) status = MH_EnableHook(function);
    Log("Hook %s %s", name, MH_StatusToString(status));
    if (status != MH_OK) InterlockedExchange(flag, 0);
}

static sl::Result Resolve(sl::Feature feature, const char* name, void*& function) {
    sl::Result result = g_resolve(feature, name, function);
    if (result == sl::Result::eOk && feature == sl::kFeatureDLSS_G) Install(name, function);
    return result;
}

// Searches the image's initialized, readable, non-executable sections. Section
// names are not trusted: protected executables (AC8) rename .rdata. Only
// committed, accessible pages are read, so encrypted or guarded ranges are skipped.
static BOOL ImageHas(HMODULE module, const WCHAR* needle) {
    BYTE* image = (BYTE*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    IMAGE_NT_HEADERS64* pe = (IMAGE_NT_HEADERS64*)(image + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(pe);
    const BYTE* pattern = (const BYTE*)needle;
    size_t length = wcslen(needle) * sizeof(WCHAR);
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    for (unsigned i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
        DWORD flags = sections[i].Characteristics;
        if (!(flags & IMAGE_SCN_CNT_INITIALIZED_DATA) || !(flags & IMAGE_SCN_MEM_READ) ||
            (flags & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE* cursor = image + sections[i].VirtualAddress;
        BYTE* end = cursor + sections[i].Misc.VirtualSize;
        if (end > image + pe->OptionalHeader.SizeOfImage) continue;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION region;
            if (!VirtualQuery(cursor, &region, sizeof(region))) break;
            BYTE* stop = std::min(end, (BYTE*)region.BaseAddress + region.RegionSize);
            if (region.State == MEM_COMMIT && (region.Protect & readable) && !(region.Protect & PAGE_GUARD) &&
                std::search(cursor, stop, pattern, pattern + length) != stop) return TRUE;
            cursor = stop;
        }
    }
    return FALSE;
}

// ---- Mode 3: CPU-bound governor (experimental) ---------------------------
// Reflex latency reports give CPU work and GPU busy time per rendered frame.
// Prefer=1 picks the most generated frames whose base rate stays >= MinBaseFPS;
// Prefer=0 the fewest that reach the target. In Unreal games the engine's own
// dynamic resolution is driven live so the total GPU time (rendering + frame
// generation) matches the base frame time. Every [Mode3] key is re-read on each
// decision; leaving Mode 3 restores the console variables.

static BOOL Accessible(const void* address, size_t bytes, BOOL write) {
    MEMORY_BASIC_INFORMATION region;
    if (!address || !VirtualQuery(address, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return FALSE;
    DWORD allowed = write ? (PAGE_READWRITE | PAGE_EXECUTE_READWRITE) :
        (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    return (region.Protect & allowed) && (const BYTE*)address + bytes <= (BYTE*)region.BaseAddress + region.RegionSize;
}

// A UE TAutoConsoleVariable<T> found in the executable. `data` points at its
// TConsoleVariableData<T>: the game-thread and render-thread copies of the value.
struct ConsoleVariable {
    const WCHAR* name;
    BOOL isFloat;
    LONG* data;
    LONG original;
    BOOL changed;
    LONG maxInt;   // largest plausible integer value; 0 = 2
    BOOL searched;
    LONG written;  // last value written, to notice the game writing its own
    int resets;
};
enum { CvarMode, CvarBudget, CvarMinPercent, CvarMaxPercent, CvarScreen, CvarCount };
static ConsoleVariable g_cvars[CvarCount] = {
    {L"r.DynamicRes.OperationMode", FALSE},
    {L"r.DynamicRes.FrameTimeBudget", TRUE},
    {L"r.DynamicRes.MinScreenPercentage", TRUE},
    {L"r.DynamicRes.MaxScreenPercentage", TRUE},
    {L"r.ScreenPercentage", TRUE},
};
static BOOL g_cvarsSearched = FALSE;
// [ConsoleVariables] name=value pins, re-applied every reload interval so the
// game cannot switch them back (e.g. r.Streamline.DLSSG.FullScreenMenuDetection).
enum { MaxForced = 16, ForcedNameLength = 96 };
static ConsoleVariable g_forced[MaxForced];
static WCHAR g_forcedNames[MaxForced][ForcedNameLength];
static LONG g_forcedBits[MaxForced];
static int g_forcedCount = 0;
static int g_forcedResets[MaxForced];
static SRWLOCK g_cvarLock = SRWLOCK_INIT;

static BOOL PlausibleValue(const LONG* data, BOOL isFloat, LONG maxInt) {
    if (data[0] != data[1]) return FALSE; // both thread copies agree when idle
    if (!isFloat) return data[0] >= 0 && data[0] <= maxInt;
    float value; memcpy(&value, data, sizeof(value));
    return _finite(value) && value >= 0.0f && value <= 1000.0f;
}

// Validates a candidate TAutoConsoleVariable object and returns its data. Two
// layouts are accepted: [vtable][Target][Ref] (virtual destructor) and [Target][Ref].
// Target must be an object whose vtable lies in the image; Ref must be writable
// data holding two equal, plausible values.
static LONG* ConsoleVariableData(const BYTE* object, const BYTE* image, size_t imageSize, BOOL isFloat, LONG maxInt = 2) {
    if (!Accessible(object, 24, FALSE)) return NULL;
    void* const* slots = (void* const*)object;
    for (int refSlot : {2, 1}) {
        const BYTE* target = (const BYTE*)slots[refSlot - 1];
        LONG* data = (LONG*)slots[refSlot];
        if (!Accessible(target, 8, FALSE) || !Accessible(data, 8, TRUE)) continue;
        const BYTE* vtable = *(const BYTE* const*)target;
        if (vtable < image || vtable >= image + imageSize) continue;
        if (PlausibleValue(data, isFloat, maxInt)) return data;
    }
    return NULL;
}

static void WriteConsoleVariable(ConsoleVariable& cvar, LONG bits) {
    if (!cvar.data) return;
    if (!cvar.changed) { cvar.original = cvar.data[0]; cvar.changed = TRUE; cvar.resets = 0; }
    else if (cvar.data[0] != cvar.written && cvar.resets < 5) {
        ++cvar.resets; // the game set its own value since the last write
        Log("CVar %ls was changed by the game (0x%lx, ours 0x%lx)", cvar.name, cvar.data[0], cvar.written);
    }
    cvar.written = bits;
    InterlockedExchange(&cvar.data[0], bits);
    InterlockedExchange(&cvar.data[1], bits);
}

// Restores g_cvars[first .. first + count); true if any value changed.
static BOOL RestoreConsoleVariableRange(int first, int count) {
    BOOL any = FALSE;
    for (int i = first; i < first + count; ++i) {
        ConsoleVariable& cvar = g_cvars[i];
        if (!cvar.changed || !cvar.data) continue;
        InterlockedExchange(&cvar.data[0], cvar.original);
        InterlockedExchange(&cvar.data[1], cvar.original);
        cvar.changed = FALSE;
        any = TRUE;
        Log("Mode3 restored %ls", cvar.name);
    }
    return any;
}

static void RestoreConsoleVariables(void) {
    // Turn the game's dynamic resolution back off first (OperationMode is index 0),
    // then the other values, and only then stop flagging frame generation.
    BOOL any = FALSE;
    for (ConsoleVariable& cvar : g_cvars) {
        if (!cvar.changed || !cvar.data) continue;
        InterlockedExchange(&cvar.data[0], cvar.original);
        InterlockedExchange(&cvar.data[1], cvar.original);
        cvar.changed = FALSE;
        any = TRUE;
        Log("Mode3 restored %ls", cvar.name);
    }
    if (any) Sleep(250); // let the engine return to its fixed resolution
    InterlockedExchange(&g_dynamicResolution, 0);
}

static LONG FloatBits(float value) { LONG bits; memcpy(&bits, &value, sizeof(bits)); return bits; }

static BOOL InWritableSection(const BYTE* address, BYTE* image, IMAGE_NT_HEADERS64* pe) {
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(pe);
    for (unsigned s = 0; s < pe->FileHeader.NumberOfSections; ++s) {
        const BYTE* start = image + sections[s].VirtualAddress;
        if (address >= start && address < start + sections[s].Misc.VirtualSize)
            return (sections[s].Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    }
    return FALSE;
}

// Looks for the static console object near a name reference: any RIP-relative
// lea or store (REX.W 8D/89 [rip+disp]) into writable image data, trying the
// address and the two slots before it (the store may target the Target or Ref
// field). The first candidate the validator accepts wins; data already claimed
// by another variable is skipped.
static LONG* ObjectNear(BYTE* from, BYTE* to, int step, BYTE* image, IMAGE_NT_HEADERS64* pe, BOOL isFloat, LONG maxInt) {
    size_t imageSize = pe->OptionalHeader.SizeOfImage;
    for (BYTE* q = from; step > 0 ? q <= to : q >= to; q += step) {
        if ((q[0] != 0x48 && q[0] != 0x4C) || (q[1] != 0x8D && q[1] != 0x89) || (q[2] & 0xC7) != 0x05) continue;
        BYTE* object = q + 7 + *(const INT32*)(q + 3);
        if (object < image + 16 || object >= image + imageSize || !InWritableSection(object, image, pe)) continue;
        for (int delta : {0, 8, 16}) {
            LONG* data = ConsoleVariableData(object - delta, image, imageSize, isFloat, maxInt);
            BOOL claimed = FALSE;
            for (const ConsoleVariable& other : g_cvars) claimed |= other.data == data;
            for (int f = 0; f < g_forcedCount; ++f) claimed |= g_forced[f].data == data;
            if (data && !claimed) return data;
        }
    }
    return NULL;
}

// Finds each console variable's registration: the code that loads its name
// string (lea r, [rip+name]) also loads or stores its static object nearby.
// Registration code stores the object after the name, so the forward window
// is searched first and the backward window only as a fallback.
// Searches only entries not searched before; `tag` prefixes the log lines.
static void LocateConsoleVariables(ConsoleVariable* list, int count, const char* tag) {
    BYTE* image = (BYTE*)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64* pe = (IMAGE_NT_HEADERS64*)(image + ((IMAGE_DOS_HEADER*)image)->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(pe);
    const BYTE* names[MaxForced] = {};
    BOOL pending = FALSE;
    for (int i = 0; i < count; ++i) {
        if (list[i].searched) continue;
        pending = TRUE;
        size_t bytes = (wcslen(list[i].name) + 1) * sizeof(WCHAR);
        const BYTE* pattern = (const BYTE*)list[i].name;
        for (unsigned s = 0; s < pe->FileHeader.NumberOfSections && !names[i]; ++s) {
            DWORD flags = sections[s].Characteristics;
            if (!(flags & IMAGE_SCN_CNT_INITIALIZED_DATA) || (flags & IMAGE_SCN_MEM_EXECUTE)) continue;
            BYTE* cursor = image + sections[s].VirtualAddress;
            BYTE* end = cursor + sections[s].Misc.VirtualSize;
            while (cursor < end && !names[i]) {
                MEMORY_BASIC_INFORMATION region;
                if (!VirtualQuery(cursor, &region, sizeof(region))) break;
                BYTE* stop = std::min(end, (BYTE*)region.BaseAddress + region.RegionSize);
                if (Accessible(cursor, 1, FALSE)) {
                    for (BYTE* hit = cursor; (hit = std::search(hit, stop, pattern, pattern + bytes)) != stop; hit += 2) {
                        // Whole string only: the previous character must be a terminator.
                        if (hit - 2 < image || *(const WCHAR*)(hit - 2) == 0) { names[i] = hit; break; }
                    }
                }
                cursor = stop;
            }
        }
    }
    for (unsigned s = 0; pending && s < pe->FileHeader.NumberOfSections; ++s) {
        if (!(sections[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE* cursor = image + sections[s].VirtualAddress;
        BYTE* end = cursor + sections[s].Misc.VirtualSize;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION region;
            if (!VirtualQuery(cursor, &region, sizeof(region))) break;
            BYTE* stop = std::min(end, (BYTE*)region.BaseAddress + region.RegionSize);
            if (Accessible(cursor, 1, FALSE)) {
                for (BYTE* p = cursor; p + 7 <= stop; ++p) {
                    if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;
                    const BYTE* target = p + 7 + *(const INT32*)(p + 3);
                    for (int i = 0; i < count; ++i) {
                        if (target != names[i] || list[i].data) continue;
                        BOOL isFloat = list[i].isFloat;
                        LONG maxInt = list[i].maxInt ? list[i].maxInt : 2;
                        list[i].data = ObjectNear(p + 7, std::min(stop - 7, p + 96), 1, image, pe, isFloat, maxInt);
                        if (!list[i].data)
                            list[i].data = ObjectNear(p - 1, std::max(cursor, p - 96), -1, image, pe, isFloat, maxInt);
                    }
                }
            }
            cursor = stop;
        }
    }
    for (int i = 0; i < count; ++i) {
        ConsoleVariable& cvar = list[i];
        if (cvar.searched) continue;
        cvar.searched = TRUE;
        if (!cvar.data) { Log("%s %ls not found", tag, cvar.name); continue; }
        if (cvar.isFloat) { float v; memcpy(&v, cvar.data, sizeof(v)); Log("%s found %ls=%.2f", tag, cvar.name, v); }
        else Log("%s found %ls=%ld", tag, cvar.name, cvar.data[0]);
    }
}

// Re-reads [ConsoleVariables] when `reread` (an INI edit), unpins entries that
// were removed, locates new ones, then writes every pinned value. A value with a
// '.' is a float ("1.0"); otherwise an integer ("1").
static void ApplyForcedConsoleVariables(BOOL reread) {
    if (!g_unreal) return;
    AcquireSRWLockExclusive(&g_cvarLock);
    if (reread) {
        static WCHAR section[4096];
        DWORD length = GetPrivateProfileSectionW(L"ConsoleVariables", section, 4096, g_config);
        ConsoleVariable next[MaxForced] = {};
        WCHAR names[MaxForced][ForcedNameLength];
        LONG bits[MaxForced];
        int count = 0;
        for (const WCHAR* line = section; length && *line && count < MaxForced; line += wcslen(line) + 1) {
            const WCHAR* equals = wcschr(line, L'=');
            size_t nameLength = equals ? (size_t)(equals - line) : 0;
            if (*line == L';' || !nameLength || nameLength >= ForcedNameLength) continue;
            WCHAR* end;
            BOOL isFloat = wcschr(equals + 1, L'.') != NULL;
            double value = wcstod(equals + 1, &end);
            while (*end == L' ' || *end == L'\t') ++end;
            if (end == equals + 1 || *end || !_finite(value) || value < -1.0 || value > 1000.0) {
                Log("CVar %.*ls: invalid value ignored", (int)nameLength, line); continue;
            }
            wcsncpy_s(names[count], line, nameLength);
            while (nameLength && names[count][nameLength - 1] == L' ') names[count][--nameLength] = 0;
            bits[count] = isFloat ? FloatBits((float)value) : (LONG)value;
            next[count].isFloat = isFloat;
            next[count].maxInt = 16;
            for (int f = 0; f < g_forcedCount; ++f)
                if (!_wcsicmp(g_forcedNames[f], names[count]) && g_forced[f].isFloat == isFloat) {
                    next[count] = g_forced[f]; g_forced[f].changed = FALSE; g_forced[f].data = NULL;
                }
            ++count;
        }
        // Removed entries keep their value: the one seen at the first write may
        // predate the game's own settings (Halo: DLSSG.Enable read 0 at startup).
        for (int f = 0; f < g_forcedCount; ++f)
            if (g_forced[f].changed && g_forced[f].data) Log("CVar unpinned %ls (value kept)", g_forcedNames[f]);
        for (int f = 0; f < count; ++f) {
            wcscpy_s(g_forcedNames[f], names[f]);
            g_forced[f] = next[f];
            g_forced[f].name = g_forcedNames[f];
            g_forcedBits[f] = bits[f];
            g_forcedResets[f] = 0;
        }
        g_forcedCount = count;
        LocateConsoleVariables(g_forced, g_forcedCount, "CVar");
    }
    for (int f = 0; f < g_forcedCount; ++f) {
        ConsoleVariable& cvar = g_forced[f];
        if (!cvar.data) continue;
        if (!reread && cvar.changed && cvar.data[0] != g_forcedBits[f] && g_forcedResets[f] < 5) {
            ++g_forcedResets[f];
            Log("CVar %ls was changed by the game (0x%lx); pinned again", cvar.name, cvar.data[0]);
        }
        if (!cvar.changed) Log("CVar %ls pinned", cvar.name);
        WriteConsoleVariable(cvar, g_forcedBits[f]);
    }
    ReleaseSRWLockExclusive(&g_cvarLock);
}

// Largest generated-frame count whose output stays within the target, allowing
// `tolerance` overshoot. A change must repeat for `settle` decisions in a row.
static LONG ChooseFrames(double baseFps, double targetFps, LONG current, LONG minimum, LONG maximum,
    double tolerance, LONG* pending, LONG* streak, LONG settle) {
    if (baseFps <= 0.0 || targetFps <= 0.0) return current;
    LONG candidate = (LONG)floor(targetFps * (1.0 + tolerance) / baseFps) - 1;
    candidate = std::max(minimum, std::min(maximum, candidate));
    if (candidate == current) { *streak = 0; return current; }
    if (candidate != *pending) { *pending = candidate; *streak = 0; }
    return ++*streak >= settle ? (*streak = 0, candidate) : current;
}

// Cost model: a base frame costs max(CPU limit, render + n * generation) of time,
// and yields n + 1 output frames. Picks the count with the highest predicted
// output, capped at the target (frames beyond it are wasted). Counts within 2% of
// the best lose to fewer generated frames: lower latency, fewer artifacts.
static LONG ChooseByCost(double cpuMs, double renderMs, double genMs, double targetFps, double tolerance,
    LONG minimum, LONG maximum) {
    double cap = targetFps * (1.0 + tolerance), best = -1.0;
    double output[6] = {};
    for (LONG n = minimum; n <= maximum && n <= 5; ++n) {
        output[n] = std::min(cap, (n + 1) * 1000.0 / std::max(cpuMs, renderMs + n * genMs));
        best = std::max(best, output[n]);
    }
    for (LONG n = minimum; n <= maximum && n <= 5; ++n)
        if (output[n] >= best * 0.98) return n;
    return minimum;
}

// Frames first: the most generated frames whose base rate (target / (n + 1))
// stays at or above `minBaseFps`. GPU time then goes to generation, and the
// lower base rate frees CPU; resolution only gets what is left.
static LONG ChooseMostFrames(double targetFps, double minBaseFps, LONG minimum, LONG maximum) {
    for (LONG n = maximum; n > minimum; --n)
        if (targetFps / (n + 1) >= minBaseFps) return n;
    return minimum;
}

// DynamicResolution=2: DLSS quality levels as r.ScreenPercentage values, for
// games whose DLSS setting ignores engine dynamic resolution.
static const double kDlssLevels[] = {33.33, 50.0, 58.0, 66.67, 100.0};
static const char* const kDlssLevelNames[] = {"UltraPerformance", "Performance", "Balanced", "Quality", "DLAA"};
enum { DlssLevelCount = 5 };

struct LevelState {
    double gpuAt[DlssLevelCount]; // smoothed total GPU time measured at each level
    LONG over, up, cooldown;      // decisions over the goal / fitting one level up / hold after a step down
};

// One decision. Down a level after 2 decisions over the goal, then hold
// `cooldownDecisions` before trying higher again; up a level after `settle`
// decisions in which the level above is predicted to fit with 3% to spare —
// from GPU time measured there before, else assuming half of today's GPU time
// scales with pixel count. Levels outside [lowest, highest] percent are skipped.
static int ChooseDlssLevel(int level, double gpuMs, double goalMs, double lowest, double highest,
    LevelState* s, LONG settle, LONG cooldownDecisions) {
    int low = 0, high = DlssLevelCount - 1;
    while (low < high && kDlssLevels[low] < lowest - 0.5) ++low;
    while (high > low && kDlssLevels[high] > highest + 0.5) --high;
    level = std::max(low, std::min(high, level));
    if (gpuMs <= 0.0) return level;
    s->gpuAt[level] = s->gpuAt[level] > 0.0 ? s->gpuAt[level] * 0.7 + gpuMs * 0.3 : gpuMs;
    if (s->cooldown > 0) --s->cooldown;
    if (gpuMs > goalMs * 1.02) {
        s->up = 0;
        if (++s->over >= 2 && level > low) { s->over = 0; s->cooldown = cooldownDecisions; return level - 1; }
        return level;
    }
    s->over = 0;
    if (level >= high || s->cooldown > 0) { s->up = 0; return level; }
    double area = (kDlssLevels[level + 1] * kDlssLevels[level + 1]) / (kDlssLevels[level] * kDlssLevels[level]);
    double predicted = s->gpuAt[level + 1] > 0.0 ? s->gpuAt[level + 1] : gpuMs * (0.5 + 0.5 * area);
    if (predicted < goalMs * 0.97) {
        if (++s->up >= settle) { s->up = 0; return level + 1; }
    } else s->up = 0;
    return level;
}

static int NearestDlssLevel(double percent) {
    int best = 0;
    for (int i = 1; i < DlssLevelCount; ++i)
        if (fabs(kDlssLevels[i] - percent) < fabs(kDlssLevels[best] - percent)) best = i;
    return best;
}

// Applies a candidate count only after it wins `settle` decisions in a row.
static LONG Settle(LONG candidate, LONG current, LONG* pending, LONG* streak, LONG settle) {
    if (candidate == current) { *streak = 0; return current; }
    if (candidate != *pending) { *pending = candidate; *streak = 0; }
    return ++*streak >= settle ? (*streak = 0, candidate) : current;
}

struct FrameTimes { double cpuMs, simMs, submitMs, gpuMs, frameMs; int samples; };

// Averages the completed reports. The game thread (simulation) and render thread
// (submit) run pipelined, so the CPU-limited frame time is the slower of the two
// stages — not the span from simulation start to submit end, which includes
// waiting. Frame time is the spacing of consecutive simulation starts. Report
// times are in microseconds.
// The CPU limit uses the 25th percentile of the per-frame CPU time: while GPU-bound
// the render thread's submit window also contains waiting for the GPU, which would
// inflate a mean.
static BOOL SummarizeReports(const sl::ReflexState& state, FrameTimes* out) {
    double cpu = 0, sim = 0, submit = 0, gpu = 0, frame = 0; int count = 0, frames = 0;
    double perFrame[sl::kReflexFrameReportCount];
    const sl::ReflexReport* previous = NULL;
    for (const sl::ReflexReport& r : state.frameReport) {
        if (!r.frameID || !r.simStartTime || r.simEndTime <= r.simStartTime ||
            r.renderSubmitEndTime <= r.renderSubmitStartTime || !r.renderSubmitStartTime || !r.gpuActiveRenderTimeUs) {
            previous = NULL; continue;
        }
        double simUs = (double)(r.simEndTime - r.simStartTime);
        double submitUs = (double)(r.renderSubmitEndTime - r.renderSubmitStartTime);
        sim += simUs; submit += submitUs;
        perFrame[count] = std::max(simUs, submitUs);
        gpu += r.gpuActiveRenderTimeUs;
        ++count;
        if (previous && r.frameID == previous->frameID + 1 && r.simStartTime > previous->simStartTime) {
            frame += (double)(r.simStartTime - previous->simStartTime);
            ++frames;
        }
        previous = &r;
    }
    if (count < 4 || frames < 2) return FALSE;
    std::sort(perFrame, perFrame + count);
    cpu = perFrame[count / 4];
    out->cpuMs = cpu / 1000.0; out->gpuMs = gpu / count / 1000.0;
    out->simMs = sim / count / 1000.0; out->submitMs = submit / count / 1000.0;
    out->frameMs = frame / frames / 1000.0; out->samples = count;
    return TRUE;
}

static DWORD WINAPI Governor(void*) {
    static sl::ReflexState state;
    PFun_slReflexGetState* reflexState = NULL;
    LONG pending = 0, streak = 0;
    double gpuAt[6] = {}; // smoothed total GPU time per base frame, by generated count
    double budgetMs = 0.0; // dynamic resolution budget; 0 = start from the CPU limit
    int dlssLevel = -1; // DynamicResolution=2; -1 = start from the game's value
    LevelState levels = {};
    ULONGLONG lastLog = 0;
    BOOL warned = FALSE;
    for (;;) {
        Sleep(Setting(g_config, L"Mode3", L"DecisionIntervalMs", 1000, 100, 60000));
        if (g_mode != 3) { RestoreConsoleVariables(); budgetMs = 0.0; dlssLevel = -1; continue; }
        if (!g_resolve) continue;
        if (!reflexState) {
            void* function = NULL;
            if (g_resolve(sl::kFeatureReflex, "slReflexGetState", function) != sl::Result::eOk || !function) continue;
            reflexState = (PFun_slReflexGetState*)function;
        }
        if (!g_cvarsSearched) {
            if (g_unreal) {
                AcquireSRWLockExclusive(&g_cvarLock);
                LocateConsoleVariables(g_cvars, CvarCount, "Mode3");
                ReleaseSRWLockExclusive(&g_cvarLock);
            }
            g_cvarsSearched = TRUE;
            if (!g_cvars[CvarBudget].data) Log("Mode3 dynamic resolution unavailable: frame count only");
        }
        FrameTimes t;
        if (reflexState(state) != sl::Result::eOk || !state.latencyReportAvailable || !SummarizeReports(state, &t)) {
            if (!warned) Log("Mode3 waiting for Reflex latency reports (turn Reflex on in the game)");
            warned = TRUE;
            continue;
        }
        warned = FALSE;
        const WCHAR* s = L"Mode3";
        DWORD hz = PrimaryHz();
        // [Mode3] TargetFPS overrides the shared [FrameGeneration] TargetFPS.
        DWORD target = Setting(g_config, s, L"TargetFPS", (DWORD)g_targetFPS, 0, 1000);
        double targetFps = target ? target : hz;
        LONG minimum = (LONG)Setting(g_config, s, L"MinGeneratedFrames", 1, 1, 5);
        LONG maximum = (LONG)Setting(g_config, s, L"MaxGeneratedFrames", 3, 1, 5);
        LONG ceiling = std::max(1L, std::min((LONG)InterlockedCompareExchange(&g_maximum, 0, 0), maximum));
        double tolerance = Setting(g_config, s, L"TolerancePercent", 10, 0, 100) / 100.0;
        LONG settle = (LONG)Setting(g_config, s, L"SettleDecisions", 2, 1, 60);
        // 0 = off; 1 = engine dynamic resolution (r.DynamicRes.*); 2 = step the
        // static r.ScreenPercentage between DLSS quality levels, for games whose
        // DLSS setting ignores engine dynamic resolution (AC8). Opt in per game.
        DWORD resolution = Setting(g_config, s, L"DynamicResolution", 0, 0, 2);
        double lowestPercent = Setting(g_config, s, L"MinScreenPercentage", 33, 10, 100);
        double highestPercent = std::max(lowestPercent, (double)Setting(g_config, s, L"MaxScreenPercentage", 67, 10, 200));
        LONG lowest = std::min(minimum, ceiling);
        // Prefer=1: most generated frames, resolution fills the GPU time left at
        // that base rate. Prefer=0: fewest generated frames that reach the target.
        BOOL preferFrames = Setting(g_config, s, L"Prefer", 1, 0, 1);
        LONG mostFrames = ChooseMostFrames(targetFps, Setting(g_config, s, L"MinBaseFPS", 30, 1, 1000), lowest, ceiling);
        // UE's budget is compared with its own render time, which excludes frame
        // generation; Reflex's GPU time includes it. So the budget is a feedback
        // loop: scale it by (goal / total GPU time) each decision, within -15% / +10%
        // per step. The goal is the CPU limit, or with Prefer=1 the base frame time
        // of the chosen count when that is longer.
        // The budget never drops below MinBudgetPercent of the goal (a budget near
        // 2 ms once turned Halo's scene black). UE's own controller reacts over
        // seconds, so the budget keeps following the goal rather than holding when
        // GPU time is slow to respond (that held AC8 at full resolution).
        double baseMs = preferFrames ? std::max(t.cpuMs, 1000.0 * (mostFrames + 1) / targetFps) : t.cpuMs;
        double goalMs = baseMs * Setting(g_config, s, L"BudgetPercent", 100, 10, 400) / 100.0;
        double floorMs = goalMs * Setting(g_config, s, L"MinBudgetPercent", 50, 10, 100) / 100.0;
        if (budgetMs <= 0.0) budgetMs = goalMs;
        else if (t.gpuMs > 0.0) {
            budgetMs *= std::max(0.85, std::min(1.10, goalMs / t.gpuMs));
        }
        // Never above the goal: UE compares the budget with its own render time,
        // which is below the total, so a higher budget only winds up (and the
        // stall guard then holds it there, pinning the maximum resolution).
        budgetMs = std::max(std::max(2.0, floorMs), std::min(goalMs, budgetMs));
        if (resolution == 1) {
            if (RestoreConsoleVariableRange(CvarScreen, 1)) dlssLevel = -1;
            // Tell frame generation first, so no frame with varying extents goes out unflagged.
            InterlockedExchange(&g_dynamicResPercent, (LONG)Setting(g_config, s, L"FrameGenResolutionPercent", 0, 0, 100));
            InterlockedExchange(&g_dynamicResolution, (LONG)Setting(g_config, s, L"FlagFrameGen", 1, 0, 1));
            WriteConsoleVariable(g_cvars[CvarMinPercent], FloatBits((float)lowestPercent));
            WriteConsoleVariable(g_cvars[CvarMaxPercent], FloatBits((float)highestPercent));
            WriteConsoleVariable(g_cvars[CvarBudget], FloatBits((float)budgetMs));
            WriteConsoleVariable(g_cvars[CvarMode], 2);
        } else if (resolution == 2 && g_cvars[CvarScreen].data) {
            // Engine dynamic resolution off first (OperationMode is index 0).
            if (RestoreConsoleVariableRange(CvarMode, CvarScreen)) Sleep(250);
            InterlockedExchange(&g_dynamicResolution, 0);
            budgetMs = 0.0;
            if (dlssLevel < 0) { // start from the level nearest the game's own value
                float own; memcpy(&own, g_cvars[CvarScreen].data, sizeof(own));
                dlssLevel = NearestDlssLevel(own);
                levels = LevelState{};
            }
            int next = ChooseDlssLevel(dlssLevel, t.gpuMs, goalMs, lowestPercent, highestPercent, &levels, settle, 30);
            if (next != dlssLevel || !g_cvars[CvarScreen].changed) {
                if (next != dlssLevel) Log("Mode3 DLSS level %s -> %s", kDlssLevelNames[dlssLevel], kDlssLevelNames[next]);
                dlssLevel = next;
                WriteConsoleVariable(g_cvars[CvarScreen], FloatBits((float)kDlssLevels[dlssLevel]));
            }
        } else {
            RestoreConsoleVariables();
            budgetMs = 0.0;
            dlssLevel = -1;
        }
        LONG current = InterlockedCompareExchange(&g_requested, 0, 0);
        // Generation cost: the GPU-time slope between the two most distant counts
        // observed so far (smoothed per count); a configured prior until then.
        // A slope outside 3..20 ms is taken as confounded (resolution changed between
        // the samples, or the base rate was pinned by the output cap) and the prior used.
        double genMs = Setting(g_config, s, L"FrameGenCostMs", 7, 1, 50);
        if (current >= 1 && current <= 5)
            gpuAt[current] = gpuAt[current] > 0.0 ? gpuAt[current] * 0.7 + t.gpuMs * 0.3 : t.gpuMs;
        LONG low = 0, high = 0;
        for (LONG n = 1; n <= 5; ++n) if (gpuAt[n] > 0.0) { if (!low) low = n; high = n; }
        if (high > low) {
            double slope = (gpuAt[high] - gpuAt[low]) / (high - low);
            if (slope >= 3.0 && slope <= 20.0) genMs = slope;
        }
        double renderMs = std::max(0.5, t.gpuMs - std::max(0L, current) * genMs);
        LONG frames;
        if (preferFrames) {
            frames = mostFrames;
        } else if (Setting(g_config, s, L"CostModel", 1, 0, 1)) {
            LONG candidate = ChooseByCost(t.cpuMs, renderMs, genMs, targetFps, tolerance, lowest, ceiling);
            // At the output cap with GPU headroom the base rate is pinned by the cap
            // (e.g. 4x at 120 Hz = 30 fps), so GPU time no longer reflects the cost.
            // Step down while one fewer generated frame still reaches the target.
            double output = 1000.0 / t.frameMs * (current + 1);
            if (current > lowest && output >= targetFps * 0.95 && t.gpuMs < t.frameMs * 0.85) {
                double fewer = current * 1000.0 / std::max(t.cpuMs, t.gpuMs - genMs);
                if (fewer >= targetFps * 0.95) candidate = std::min(candidate, current - 1);
            }
            frames = Settle(candidate, current, &pending, &streak, settle);
        } else {
            // Fill from the CPU limit, not the achieved base rate: a GPU-bound base that
            // falls must not raise the count.
            frames = ChooseFrames(1000.0 / t.cpuMs, targetFps, current, lowest, ceiling,
                tolerance, &pending, &streak, settle);
        }
        if (frames != current) InterlockedExchange(&g_requested, frames);
        ULONGLONG now = GetTickCount64();
        if (frames != current || now - lastLog >= g_sampleMs) {
            Log("Mode3 cpu=%.2fms (sim=%.2f submit=%.2f) gpu=%.2fms (render=%.2f gen=%.2f/frame) frame=%.2fms base=%.1ffps cpuLimit=%.1ffps bound=%s target=%.0f "
                "generated=%ld output=%.0ffps budget=%.2fms goal=%.2fms prefer=%s resolution=%s%s%s samples=%d",
                t.cpuMs, t.simMs, t.submitMs, t.gpuMs, renderMs, genMs, t.frameMs, 1000.0 / t.frameMs, 1000.0 / t.cpuMs,
                t.gpuMs > t.cpuMs * 0.95 ? "GPU" : "CPU", targetFps, frames, 1000.0 / t.frameMs * (frames + 1),
                budgetMs, goalMs, preferFrames ? "frames" : "latency",
                !resolution ? "off" : resolution == 2 ? (g_cvars[CvarScreen].data ? "screen" : "advisory") :
                g_cvars[CvarBudget].data ? "driven" : "advisory",
                resolution == 2 && dlssLevel >= 0 ? " level=" : "", resolution == 2 && dlssLevel >= 0 ? kDlssLevelNames[dlssLevel] : "",
                t.samples);
            lastLog = now;
        }
    }
}

static DWORD WINAPI Worker(void*) {
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)Worker, &pinned);
    WCHAR marker[80]; swprintf_s(marker, L"Local\\DLSSMFGSM86-%lu", GetCurrentProcessId());
    HANDLE singleton = CreateMutexW(NULL, FALSE, marker);
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleton) CloseHandle(singleton);
        return 0; // a second copy keeps forwarding its exports, but installs no hooks
    }
    // Hold the singleton for the process lifetime; never race a second copy.
    wcscpy_s(g_config, g_directory); wcscat_s(g_config, MFG_CONFIG_NAME);
    ReadConfiguration(g_config);
    if (g_logging && !ConfigureLogPath(g_config)) g_logging = FALSE;
    HMODULE interposer = NULL;
    DWORD waitMs = Setting(g_config, L"Hooks", L"WaitTimeoutMs", 120000, 1000, 600000);
    ULONGLONG deadline = GetTickCount64() + waitMs;
    while (!interposer && GetTickCount64() < deadline) {
        interposer = GetModuleHandleW(L"sl.interposer.dll");
        if (!interposer) Sleep(1);
    }
    if (!interposer) { Log("Interposer did not load within %lu ms", waitMs); return 2; }
    // Checked only once Streamline is loading: a protected executable has
    // unpacked its data by then, and the scan never delays a non-Streamline game.
    // Any game that loads Streamline is supported; Unreal is detected by a console
    // variable name present in every UE4.22+/UE5 executable.
    g_unreal = ImageHas(GetModuleHandleW(NULL), L"r.DynamicRes.OperationMode");
    Log("Streamline game, engine=%s", g_unreal ? "Unreal" : "other (console variable features off)");
    DWORD hz = PrimaryHz();
    ApplyConfiguration(hz, "Start");
    HANDLE reloader = CreateThread(NULL, 0, Reloader, (void*)(ULONG_PTR)hz, 0, NULL);
    if (reloader) CloseHandle(reloader);
    HANDLE governor = CreateThread(NULL, 0, Governor, NULL, 0, NULL);
    if (governor) CloseHandle(governor);
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) { Log("Initialize %s", MH_StatusToString(status)); return 1; }
    void* function = (void*)GetProcAddress(interposer, "slGetFeatureFunction");
    if (!function) { Log("Feature resolver missing"); return 3; }
    status = MH_CreateHook(function, (void*)Resolve, (void**)&g_resolve);
    if (status == MH_OK) status = MH_EnableHook(function);
    Log("Resolver %s", MH_StatusToString(status));
    if (status != MH_OK) return 4;
    // Usually the game's own lookups install both hooks. If they happened before
    // the resolver hook, look the functions up again after startup.
    Sleep(Setting(g_config, L"Hooks", L"LookupDelayMs", 10000, 0, 120000));
    DWORD retries = Setting(g_config, L"Hooks", L"RetryCount", 30, 1, 300);
    DWORD delay = Setting(g_config, L"Hooks", L"RetryDelayMs", 1000, 100, 10000);
    for (DWORD i = 0; i < retries && (!g_stateInstalled || !g_optionsInstalled); ++i) {
        for (const char* name : {"slDLSSGGetState", "slDLSSGSetOptions"}) {
            void* resolved = NULL;
            sl::Result result = g_resolve(sl::kFeatureDLSS_G, name, resolved);
            if (result == sl::Result::eOk) Install(name, resolved);
            else if (i == 0 || i == retries - 1) Log("Lookup %s result=%d", name, (int)result);
        }
        Sleep(delay);
    }
    return 0;
}

#ifndef DLSS_MFG_SM86_TEST
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DWORD length = GetModuleFileNameW(instance, g_directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return FALSE;
    WCHAR* slash = wcsrchr(g_directory, L'\\');
    if (!slash) return FALSE;
    slash[1] = 0;
    HANDLE thread = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    if (!thread) return FALSE;
    CloseHandle(thread);
    return TRUE;
}
#endif

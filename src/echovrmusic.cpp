// EchoVRMusic: plays your Windows audio through Echo VR's in-game music speakers.
//
// How it works (details in docs/REVERSING.md):
//   1. Echo's Wwise 2018 sound engine is linked into echovr.exe and its API is exported, so the
//      plugin hooks PostEvent / SetPosition / SetMultiplePositions / UnregisterGameObj /
//      RenderAudio by export name.
//   2. A game object that gets one of the stock music Play events is a music emitter (a speaker
//      or a set of speakers). Its stock music is paused, and a mirror game object takes the same
//      position(s).
//   3. On each mirror the plugin posts Echo's own voice-chat event, whose sound is Wwise's Audio
//      Input source, and registers that playing ID with Echo's CAudioInputCallbackRegistry, the
//      same way voice chat does. Wwise then pulls PCM from us: Windows audio, captured by WASAPI.
//   All Wwise calls the plugin makes run inside the RenderAudio hook, on the game's own thread, so
//   a voice is always registered before Wwise first asks it for audio.
#include <windows.h>
#include <detours.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "capture.h"
#include "level_names.h"
#include "builtin_data.h"

// ---- echovr.exe build check (35,397,120 bytes, May 2023) -------------------------------------

static const uintptr_t RVA_AUDIOINPUT_EXECUTE = 0x205440;   // static Execute callback given to Wwise
static const uintptr_t RVA_REGISTRY_LOAD = 0x205466;        // mov rbx, [g_AudioInputRegistry]
static const uintptr_t RVA_REGISTRY_PTR = 0x20A2FE0;        // CAudioInputCallbackRegistry*
static const uintptr_t RVA_REGISTRY_REGISTER = 0x20CA30;    // vtable slot 6
static const uintptr_t RVA_REGISTRY_UNREGISTER = 0x211E00;  // vtable slot 7
static const uint8_t SIG_EXECUTE[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89,
                                      0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf2, 0x8b};
static const uint8_t SIG_REGISTRY_LOAD[] = {0x48, 0x8b, 0x1d, 0x73, 0xdb, 0xe9, 0x01, 0x48, 0x83, 0xc3, 0x38};
// Level load: (this, CSymbol64 level, ...); the symbol is stored at this+0x58.
static const uintptr_t RVA_LOAD_LEVEL = 0x4FE050;
static const uint8_t SIG_LOAD_LEVEL[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48,
                                         0x89, 0x78, 0x18, 0x55};

// ---- Wwise types (2018.1, as built into echovr.exe) ------------------------------------------

struct AkVector { float x, y, z; };
struct AkTransform { AkVector front, top, pos; };   // Wwise 2017+ member order
static bool operator==(const AkTransform& a, const AkTransform& b) { return memcmp(&a, &b, sizeof a) == 0; }
struct AkChannelEmitter { AkTransform t; uint32_t inputChannels; };
enum { AK_Success = 1 };
enum { ActionPause = 1, ActionResume = 2 };   // AkActionOnEventType
static const int CurveLinear = 4;              // AkCurveInterpolation_Linear

typedef uint32_t (*PostEventId_t)(uint32_t, uint64_t, uint32_t, void*, void*, uint32_t, void*, uint32_t);
typedef uint32_t (*PostEventStr_t)(const char*, uint64_t, uint32_t, void*, void*, uint32_t, void*, uint32_t);
typedef int (*RegisterGameObj_t)(uint64_t, const char*);
typedef int (*UnregisterGameObj_t)(uint64_t);
typedef int (*UnregisterAllGameObj_t)();
typedef int (*SetPosition_t)(uint64_t, const AkTransform*);
typedef int (*SetMultiplePositions_t)(uint64_t, const AkTransform*, uint16_t, int);
typedef int (*SetMultipleChannelPositions_t)(uint64_t, const AkChannelEmitter*, uint16_t, int);
typedef int (*GetPosition_t)(uint64_t, AkTransform*);
typedef int (*GetPlayingIDsFromGameObject_t)(uint64_t, uint32_t*, uint32_t*);
typedef int (*SetScalingFactor_t)(uint64_t, float);
typedef void (*StopPlayingID_t)(uint32_t, int32_t, int);
typedef void (*ExecuteActionOnPlayingID_t)(int, uint32_t, int32_t, int);
typedef uint32_t (*GetIDFromString_t)(const char*);
typedef int (*RenderAudio_t)(bool);
typedef uint32_t (*GetBufferTick_t)();
typedef int (*GetListeners_t)(uint64_t, uint64_t*, uint32_t*);
typedef int (*GetListenerPosition_t)(uint64_t, AkTransform*);

#define AK_EXPORT(var, type, name) static type var = nullptr; static const char* var##_name = name;
AK_EXPORT(Real_PostEventId, PostEventId_t, "?PostEvent@SoundEngine@AK@@YAKK_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z")
AK_EXPORT(Real_PostEventStr, PostEventStr_t, "?PostEvent@SoundEngine@AK@@YAKPEBD_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z")
AK_EXPORT(Real_RegisterGameObj, RegisterGameObj_t, "?RegisterGameObj@SoundEngine@AK@@YA?AW4AKRESULT@@_KPEBD@Z")
AK_EXPORT(Real_UnregisterGameObj, UnregisterGameObj_t, "?UnregisterGameObj@SoundEngine@AK@@YA?AW4AKRESULT@@_K@Z")
AK_EXPORT(Real_UnregisterAllGameObj, UnregisterAllGameObj_t, "?UnregisterAllGameObj@SoundEngine@AK@@YA?AW4AKRESULT@@XZ")
AK_EXPORT(Real_SetPosition, SetPosition_t, "?SetPosition@SoundEngine@AK@@YA?AW4AKRESULT@@_KAEBVAkTransform@@@Z")
AK_EXPORT(Real_SetMultiplePositions, SetMultiplePositions_t, "?SetMultiplePositions@SoundEngine@AK@@YA?AW4AKRESULT@@_KPEBVAkTransform@@GW4MultiPositionType@12@@Z")
AK_EXPORT(Real_SetMultipleChannelPositions, SetMultipleChannelPositions_t, "?SetMultiplePositions@SoundEngine@AK@@YA?AW4AKRESULT@@_KPEBUAkChannelEmitter@@GW4MultiPositionType@12@@Z")
AK_EXPORT(Real_RenderAudio, RenderAudio_t, "?RenderAudio@SoundEngine@AK@@YA?AW4AKRESULT@@_N@Z")
AK_EXPORT(AkGetPosition, GetPosition_t, "?GetPosition@Query@SoundEngine@AK@@YA?AW4AKRESULT@@_KAEAVAkTransform@@@Z")
AK_EXPORT(AkGetPlayingIDs, GetPlayingIDsFromGameObject_t, "?GetPlayingIDsFromGameObject@Query@SoundEngine@AK@@YA?AW4AKRESULT@@_KAEAKPEAK@Z")
AK_EXPORT(AkSetScalingFactor, SetScalingFactor_t, "?SetScalingFactor@SoundEngine@AK@@YA?AW4AKRESULT@@_KM@Z")
AK_EXPORT(AkStopPlayingID, StopPlayingID_t, "?StopPlayingID@SoundEngine@AK@@YAXKJW4AkCurveInterpolation@@@Z")
AK_EXPORT(AkExecuteActionOnPlayingID, ExecuteActionOnPlayingID_t, "?ExecuteActionOnPlayingID@SoundEngine@AK@@YAXW4AkActionOnEventType@12@KJW4AkCurveInterpolation@@@Z")
AK_EXPORT(AkGetIDFromString, GetIDFromString_t, "?GetIDFromString@SoundEngine@AK@@YAKPEBD@Z")
AK_EXPORT(AkGetBufferTick, GetBufferTick_t, "?GetBufferTick@SoundEngine@AK@@YAKXZ")
AK_EXPORT(AkGetListeners, GetListeners_t, "?GetListeners@Query@SoundEngine@AK@@YA?AW4AKRESULT@@_KPEA_KAEAK@Z")
AK_EXPORT(AkGetListenerPosition, GetListenerPosition_t, "?GetListenerPosition@Query@SoundEngine@AK@@YA?AW4AKRESULT@@_KAEAVAkTransform@@@Z")

// Play events whose action targets a Music Switch / Music Playlist container, from the extracted
// soundbanks (tools/find_music_events.py).
static const uint32_t STOCK_MUSIC_EVENTS[] = {
    0xcbcdb503,                                                  // bank 0x98f91c32 (arena music)
    0x4edefef9,                                                  // bank 0x2d28fc86
    0x0547368c, 0x09fdd4e2, 0x0a11dfbe, 0x62af636f, 0x6de4a4b6,  // bank 0x69b27427 (positioned music)
    0xa6796127, 0xceca2d18, 0xd4c0a6d4, 0xf7702747,
    0x0e3c34d1, 0x8dcb9269,                                      // bank 0xcc62f7a0
    0x2f22c384, 0xb1b06cfb,                                      // bank 0xb96c33b5 (lobby music)
};

// ---- Echo's Audio Input bridge (CAudioInputCallbackRegistry) ---------------------------------

// The registry stores this 24-byte delegate per playing ID and, on Wwise's audio thread, calls
// fn(&delegate, pcm, maxFrames). The return value is the number of frames written.
struct Voice;
struct AudioDelegate { Voice* voice; void* unused; int64_t (*fn)(AudioDelegate*, int16_t*, uint16_t); };
struct AudioFormat { uint32_t sampleRate, bitsPerSample, type /*0 int, 1 float*/, pad; };   // always mono
typedef void (*RegistryRegister_t)(void*, uint32_t, const AudioDelegate*, const AudioFormat*);
typedef void (*RegistryUnregister_t)(void*, uint32_t);

static uintptr_t g_base;

static void* Registry() { return *(void**)(g_base + RVA_REGISTRY_PTR); }

// ---- settings ----------------------------------------------------------------------------------

struct Settings {
    bool enabled = true;
    std::string source = "system";
    float gain = 1.0f;
    int latencyMs = 120;
    uint32_t voiceEvent = 0xf5b8b08d;
    bool muteStock = true;
    float scale = 1.0f;
    bool stereo = true;
    bool logEvents = false;
    bool follow2D = true;      // 2D music with no configured speakers: speakers follow your head
    float followSpread = 1.5f; // metres from your head to each of the two following speakers
    bool shaping = true;       // our own fade per speaker, on top of Echo's gentle voice fade
    float fullVolume = 3.0f;   // metres: full volume within this distance of a speaker
    float cutoff = 30.0f;      // metres: silent from this distance on
    int nearest = 0;           // only the N nearest speakers play (0 = all)
    std::set<uint32_t> extraMusicEvents;
    std::vector<std::pair<uint32_t, AkVector>> speakers;   // (music event or 0 = any, position)
    struct Level { std::string name; std::vector<AkVector> speakers; const char* from = ""; };
    std::unordered_map<uint64_t, Level> levels;            // maps\<level>.txt, keyed by CSymbol64
};

// CSymbol64, the engine's 64-bit name hash (case-insensitive), as in lone_echo_blender.
static uint64_t Symbol64(const std::string& text) {
    static uint64_t seeds[256];
    static bool init = false;
    if (!init) {
        const uint64_t MASK = 0x95AC9329AC4BC9B5ull;
        for (int i = 0; i < 256; i++) {
            uint64_t v = (i & 0x80) ? 0x2B5926535897936Aull : 0;
            if (i & 0x40) v ^= MASK;
            for (int shift = 0x20; shift; shift >>= 1) { v <<= 1; if (i & shift) v ^= MASK; }
            seeds[i] = v << 1;
        }
        init = true;
    }
    uint64_t r = ~0ull;
    for (unsigned char b : text) {
        if (b >= 0x41 && b <= 0x5A) b += 0x20;
        r = (r << 8) ^ seeds[r >> 56] ^ b;
    }
    return r;
}

// Name of a level symbol, from KNOWN_LEVELS ("" if unknown).
static std::string LevelName(uint64_t level) {
    static std::unordered_map<uint64_t, std::string> names;
    if (names.empty()) for (const char* n : KNOWN_LEVELS) names[Symbol64(n)] = n;
    auto it = names.find(level);
    return it == names.end() ? "" : it->second;
}

static std::atomic<uint64_t> g_level{0};          // current level's CSymbol64
static std::atomic<bool> g_levelChanged{false};
static std::atomic<bool> g_settingsChanged{false};

static Settings g_S;
static std::mutex g_settingsLock;
static std::atomic<bool> g_enabled{true};
static std::atomic<float> g_gain{1.0f};
static std::atomic<uint32_t> g_latencyMs{120};
static std::string g_dir;

// ---- log -------------------------------------------------------------------------------------

static std::mutex g_logLock;
static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logLock);
    FILE* f = nullptr;
    if (fopen_s(&f, (g_dir + "EchoVRMusic.log").c_str(), "a") || !f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---- settings sources --------------------------------------------------------------------------
// In order, later ones winning: built-in defaults and arena speakers (builtin_data.h), built-in
// map speakers, then the registry (HKCU\Software\EchoVRMusic, written by the EchoVRMusic app).

static const char* REG_KEY = "Software\\EchoVRMusic";

static std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static bool ParseVec(const std::string& v, AkVector& p) {
    return sscanf_s(v.c_str(), "%f %f %f", &p.x, &p.y, &p.z) == 3 ||
           sscanf_s(v.c_str(), "%f,%f,%f", &p.x, &p.y, &p.z) == 3;
}

static void ApplySetting(Settings& s, std::string k, const std::string& v) {
    for (auto& c : k) c = (char)tolower((unsigned char)c);
    if (k == "enabled") s.enabled = atoi(v.c_str()) != 0;
    else if (k == "source") s.source = v;
    else if (k == "gain") s.gain = (float)atof(v.c_str());
    else if (k == "latencyms") s.latencyMs = max(20, min(1000, atoi(v.c_str())));
    else if (k == "voiceevent") s.voiceEvent = (uint32_t)strtoul(v.c_str(), nullptr, 0);
    else if (k == "mutestock") s.muteStock = atoi(v.c_str()) != 0;
    else if (k == "scale") s.scale = (float)atof(v.c_str());
    else if (k == "stereo") s.stereo = atoi(v.c_str()) != 0;
    else if (k == "logevents") s.logEvents = atoi(v.c_str()) != 0;
    else if (k == "follow2d") s.follow2D = atoi(v.c_str()) != 0;
    else if (k == "followspread") s.followSpread = (float)atof(v.c_str());
    else if (k == "shaping") s.shaping = atoi(v.c_str()) != 0;
    else if (k == "fullvolume") s.fullVolume = max(0.0f, (float)atof(v.c_str()));
    else if (k == "cutoff") s.cutoff = max(1.0f, (float)atof(v.c_str()));
    else if (k == "nearest") s.nearest = max(0, atoi(v.c_str()));
    else if (k == "musicevents") {
        std::string t = v; char* p = &t[0];
        while (*p) { char* e; uint32_t id = (uint32_t)strtoul(p, &e, 0); if (e == p) { p++; continue; } s.extraMusicEvents.insert(id); p = e; }
    } else if (k == "speaker" || k.rfind("speaker@", 0) == 0) {   // Speaker@<music event> = x y z
        uint32_t ev = k.size() > 8 ? (uint32_t)strtoul(k.c_str() + 8, nullptr, 0) : 0;
        AkVector p = {};
        if (ParseVec(v, p)) s.speakers.push_back({ev, p});
    }
}

// Calls fn(key, value) for each `key = value` line, `#` comments removed.
template <class F> static void ForEachLine(const std::string& text, F fn) {
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i); if (j == std::string::npos) j = text.size();
        std::string l = text.substr(i, j - i);
        i = j + 1;
        size_t hash = l.find('#'); if (hash != std::string::npos) l.resize(hash);
        size_t eq = l.find('='); if (eq == std::string::npos) continue;
        fn(Trim(l.substr(0, eq)), Trim(l.substr(eq + 1)));
    }
}

// A speaker map: `Level = <name>` and/or `LevelId = 0x...`, then `Speaker = x y z` lines (Echo
// coordinates). `fallbackName` names the level when the text doesn't.
static void AddMap(Settings& s, const std::string& text, const std::string& fallbackName, const char* from) {
    Settings::Level lv;
    uint64_t id = 0;
    ForEachLine(text, [&](std::string k, const std::string& v) {
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        if (k == "level") { lv.name = v; if (!id) id = Symbol64(v); }
        else if (k == "levelid") id = strtoull(v.c_str(), nullptr, 0);
        else if (k == "speaker") { AkVector p = {}; if (ParseVec(v, p)) lv.speakers.push_back(p); }
    });
    if (!id && !fallbackName.empty()) { lv.name = fallbackName; id = Symbol64(fallbackName); }
    if (!id) return;
    if (lv.name.empty()) lv.name = LevelName(id);
    lv.from = from;
    if (lv.speakers.empty()) s.levels.erase(id);   // an emptied file switches the built-in off too
    else s.levels[id] = lv;
}

static void LoadRegistrySettings(Settings& s) {
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    char name[128], data[512];
    for (DWORD i = 0;; i++) {
        DWORD nn = sizeof name, dn = sizeof data - 1, type = 0;
        if (RegEnumValueA(key, i, name, &nn, nullptr, &type, (BYTE*)data, &dn) != ERROR_SUCCESS) break;
        if (type == REG_SZ) { data[dn] = 0; ApplySetting(s, name, data); }
        else if (type == REG_DWORD) ApplySetting(s, name, std::to_string(*(DWORD*)data));
    }
    RegCloseKey(key);
}

#ifdef EVM_DEV
// ---- dev build only (build_dev.bat): speaker files placed in game -----------------------------
// plugins\EchoVRMusic\maps\<level>.txt, written by keys 1 and 2, replace that level's speakers.
// Release builds leave all of this out.

static std::string ReadFile(const std::string& path) {
    std::string out;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) return out;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static void LoadUserMaps(Settings& s) {
    WIN32_FIND_DATAA fd;
    std::string dir = g_dir + "EchoVRMusic\\maps\\";
    HANDLE h = FindFirstFileA((dir + "*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = fd.cFileName; n.resize(n.size() - 4);
        AddMap(s, ReadFile(dir + fd.cFileName), n, "placed in game (dev)");
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static uint64_t UserMapsTime() {
    uint64_t sum = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_dir + "EchoVRMusic\\maps\\*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do sum = sum * 31 + (((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime) +
                 fd.nFileSizeLow;
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return sum;
}
#endif

// Changes when the registry settings change.
static uint64_t ConfigTime() {
    uint64_t sum = 0;
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        FILETIME t = {};
        RegQueryInfoKeyA(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &t);
        sum += ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
        RegCloseKey(key);
    }
#ifdef EVM_DEV
    sum = sum * 31 + UserMapsTime();
#endif
    return sum;
}

static void LoadConfig(bool first) {
    Settings s;
    ForEachLine(BUILTIN_SETTINGS, [&](const std::string& k, const std::string& v) { ApplySetting(s, k, v); });
    for (const auto& m : BUILTIN_MAPS) AddMap(s, m.text, m.level, "built in");
    LoadRegistrySettings(s);
#ifdef EVM_DEV
    LoadUserMaps(s);
#endif
    std::string oldSource;
    {
        std::lock_guard<std::mutex> lk(g_settingsLock);
        oldSource = g_S.source;
        g_S = s;
    }
    g_gain = s.gain;
    g_latencyMs = (uint32_t)s.latencyMs;
    g_enabled = s.enabled;
    Log("settings: Enabled=%d Source=%s Gain=%.2f LatencyMs=%d MuteStock=%d Stereo=%d Shaping=%d FullVolume=%.1f "
        "Cutoff=%.1f Nearest=%d Scale=%.2f Follow2D=%d", (int)s.enabled, s.source.c_str(), s.gain, s.latencyMs,
        (int)s.muteStock, (int)s.stereo, (int)s.shaping, s.fullVolume, s.cutoff, s.nearest, s.scale, (int)s.follow2D);
    for (auto& kv : s.levels)
        Log("map speakers: %s (0x%016llx), %zu speaker(s), %s", kv.second.name.c_str(), (unsigned long long)kv.first,
            kv.second.speakers.size(), kv.second.from);
    g_settingsChanged = true;   // re-place speakers with the new settings
    if (first || s.source != oldSource) capture::Start(s.source, Log);
}
static Settings Snapshot() { std::lock_guard<std::mutex> lk(g_settingsLock); return g_S; }

// ---- state -----------------------------------------------------------------------------------

struct Voice {
    uint64_t obj = 0;         // our game object
    uint32_t playing = 0;     // its Audio Input playing ID
    int channel = 0;          // 0 = L+R, 1 = L, 2 = R
    AudioDelegate del = {};
    AkVector pos = {};        // where it is (for distance shaping)
    bool follows = false;     // follows your head: not distance shaped
    std::atomic<float> target{1.0f};   // distance gain, set each frame by Tick
    float current = 1.0f;     // audio thread: ramps to target over one buffer
};

struct Emitter {                         // a game object that the game plays stock music on
    std::string name;
    std::set<uint32_t> music;            // stock music playing IDs on it
    std::set<uint32_t> events;           // stock music events posted on it
    std::vector<AkTransform> positions;  // from SetMultiplePositions; empty = single, ask Wwise
    int multiType = 1;                   // MultiPositionType of those positions
    bool positionsChanged = true;
    std::vector<AkTransform> logged;     // positions last written to the log
    bool follow = false;                 // 2D music: our speakers follow the listener
    std::vector<std::unique_ptr<Voice>> voices;   // our mirror voices (empty = not mirrored)
    uint32_t rate = 0;                   // sample rate the voices were registered with
    bool paused = false;                 // stock music paused by us
};

static std::mutex g_lock;   // guards everything below (game threads + RenderAudio)
static std::unordered_map<uint64_t, Emitter> g_emitters;
static std::unordered_map<uint64_t, std::string> g_names;
static std::set<std::pair<uint32_t, uint64_t>> g_loggedEvents;
static std::atomic<uint64_t> g_nextObj{0x45564D5553494300ull};   // "EVMUSIC\0"
static bool g_ready = false;

static bool IsStockMusic(uint32_t ev, const Settings& s) {
    for (uint32_t id : STOCK_MUSIC_EVENTS) if (id == ev) return true;
    return s.extraMusicEvents.count(ev) != 0;
}

static bool IsMirror(uint64_t obj) { return (obj & ~0xFFFFull) == (0x45564D5553494300ull & ~0xFFFFull); }

// ---- the PCM callback (Wwise audio thread) ---------------------------------------------------

// One playhead for every speaker, advanced once per Wwise audio buffer (GetBufferTick), so all
// voices play the same samples in the same buffer. Per-voice cursors drifted apart (voices start,
// go virtual and come back at different times) and the speakers sounded like delayed copies.
// Execute calls are serialised by the registry mutex, so these are only touched by one thread.
static uint32_t g_tick = 0;
static bool g_haveTick = false;
static uint64_t g_play = 0;

static int64_t FillVoice(AudioDelegate* d, int16_t* out, uint16_t maxFrames) {
    Voice* v = d->voice;
    uint64_t w = capture::WriteCursor();
    uint32_t rate = capture::SampleRate();
    if (!rate || !w) { memset(out, 0, maxFrames * sizeof(int16_t)); return maxFrames; }
    uint64_t lat = (uint64_t)rate * g_latencyMs.load() / 1000;
    uint32_t tick = AkGetBufferTick();
    if (!g_haveTick || tick != g_tick) {
        if (g_haveTick && g_play) g_play += (uint64_t)maxFrames * (uint32_t)(tick - g_tick);
        g_tick = tick; g_haveTick = true;
        // Stay `lat` behind the writer; resync if we fall too far behind or run ahead of it.
        if (g_play == 0 || g_play > w || w - g_play > lat * 2 + maxFrames || w - g_play > capture::kMask)
            g_play = w > lat ? w - lat : 1;
    }
    const float* ring = capture::Ring();
    float gain = g_gain.load();
    float g0 = v->current, g1 = v->target.load(), step = (g1 - g0) / (maxFrames ? maxFrames : 1);
    v->current = g1;
    for (uint16_t i = 0; i < maxFrames; i++) {
        float s = 0.0f;
        uint64_t pos = g_play + i;
        if (pos < w) {
            const float* f = ring + (pos & capture::kMask) * 2;
            s = v->channel == 1 ? f[0] : v->channel == 2 ? f[1] : 0.5f * (f[0] + f[1]);
        }
        s *= gain * (g0 + step * i);
        // Volume above 100% can push past full scale: round peaks off above 0.9 instead of clipping.
        float a = fabsf(s);
        if (a > 0.9f) s = (s < 0 ? -1.0f : 1.0f) * (0.9f + 0.1f * tanhf((a - 0.9f) / 0.1f));
        out[i] = (int16_t)(s * 32767.0f);
    }
    return maxFrames;
}

// ---- mirror voices (RenderAudio hook only) ---------------------------------------------------

static void DestroyVoices(Emitter& e) {
    void* reg = Registry();
    for (auto& v : e.voices) {
        AkStopPlayingID(v->playing, 0, CurveLinear);
        if (reg) ((RegistryUnregister_t)(*(void***)reg)[7])(reg, v->playing);   // no callbacks after this
        Real_UnregisterGameObj(v->obj);
    }
    e.voices.clear();
}

// Two speakers either side of the listener's head (left, right), or the head itself for one.
// Before the voices exist there is no listener to ask; two placeholders size the voice count.
static std::vector<AkTransform> FollowPositions(const Emitter& e, const Settings& s) {
    AkTransform head = {{0, 0, 1}, {0, 1, 0}, {0, 0, 0}};
    if (!e.voices.empty()) {
        uint64_t ids[4]; uint32_t n = 4;
        if (AkGetListeners(e.voices[0]->obj, ids, &n) == AK_Success && n) AkGetListenerPosition(ids[0], &head);
    }
    // Wwise is left-handed (x right, y up, z front): right = up x front.
    const AkVector& f = head.front; const AkVector& u = head.top;
    AkVector r = {u.y * f.z - u.z * f.y, u.z * f.x - u.x * f.z, u.x * f.y - u.y * f.x};
    float d = s.followSpread;
    AkTransform L = head, R = head;
    L.pos = {head.pos.x - r.x * d, head.pos.y - r.y * d, head.pos.z - r.z * d};
    R.pos = {head.pos.x + r.x * d, head.pos.y + r.y * d, head.pos.z + r.z * d};
    if (s.stereo) return {L, R};
    return {head};
}

// Where the emitter's music comes from: its own position(s), or, for a 2D emitter (no position,
// or the origin), the configured speakers for its music event (else the ones for any event).
static std::vector<AkTransform> ResolvePositions(uint64_t src, const Emitter& e, const Settings& s, int& type,
                                                 bool* follow = nullptr) {
    if (follow) *follow = false;
    // Speakers placed for this level in Blender replace everything else.
    auto lv = s.levels.find(g_level.load());
    if (lv != s.levels.end()) {
        std::vector<AkTransform> pos;
        for (const AkVector& p : lv->second.speakers) pos.push_back({{0, 0, 1}, {0, 1, 0}, p});
        type = 1;   // MultiSources
        return pos;
    }
    std::vector<AkTransform> pos = e.positions;
    type = e.multiType;
    if (pos.empty()) {
        AkTransform t = {};
        if (AkGetPosition(src, &t) == AK_Success) pos.push_back(t);
    }
    bool zero = pos.size() == 1 && fabsf(pos[0].pos.x) + fabsf(pos[0].pos.y) + fabsf(pos[0].pos.z) < 1e-3f;
    if (pos.empty() || zero) {
        std::vector<AkTransform> cfg, any;
        for (auto& sp : s.speakers) {
            AkTransform t = {{0, 0, 1}, {0, 1, 0}, sp.second};
            if (sp.first == 0) any.push_back(t);
            else if (e.events.count(sp.first)) cfg.push_back(t);
        }
        if (cfg.empty()) cfg.swap(any);
        if (!cfg.empty()) { pos.swap(cfg); type = 1; }   // MultiSources
        else if (s.follow2D) { pos = FollowPositions(e, s); type = 1; if (follow) *follow = true; }
    }
    return pos;
}

static void PlaceVoices(uint64_t src, Emitter& e, const Settings& s) {
    int type;
    bool follow;
    std::vector<AkTransform> pos = ResolvePositions(src, e, s, type, &follow);
    if (pos.empty()) return;
    if (follow != e.follow) {
        e.follow = follow;
        if (follow) Log("speaker 0x%llx \"%s\": 2D music, %zu speaker(s) following your head", (unsigned long long)src,
                        e.name.c_str(), pos.size());
    }
    if (!e.follow && pos != e.logged) {
        for (size_t i = 0; i < pos.size(); i++)
            Log("speaker 0x%llx \"%s\" position %zu/%zu: x=%.2f y=%.2f z=%.2f  (facing %.2f %.2f %.2f)",
                (unsigned long long)src, e.name.c_str(), i + 1, pos.size(), pos[i].pos.x, pos[i].pos.y, pos[i].pos.z,
                pos[i].front.x, pos[i].front.y, pos[i].front.z);
        e.logged = pos;
    }
    // One voice per speaker, each at a single position. (One voice with several positions, via
    // SetMultiplePositions, did not come out positioned in game.)
    for (size_t vi = 0; vi < e.voices.size(); vi++) {
        const AkTransform& t = pos[min(vi, pos.size() - 1)];
        Real_SetPosition(e.voices[vi]->obj, &t);
        e.voices[vi]->pos = t.pos;
        e.voices[vi]->follows = e.follow;
        AkSetScalingFactor(e.voices[vi]->obj, s.scale);
    }
}

static const size_t MAX_SPEAKERS = 64;

static size_t SpeakerCount(uint64_t src, const Emitter& e, const Settings& s) {
    int type;
    return min(MAX_SPEAKERS, max((size_t)1, ResolvePositions(src, e, s, type).size()));
}

static void CreateVoices(uint64_t src, Emitter& e, const Settings& s, uint32_t rate) {
    void* reg = Registry();
    if (!reg) return;
    size_t positions = SpeakerCount(src, e, s);
    int count = (int)positions;
    for (int c = 0; c < count; c++) {
        auto v = std::make_unique<Voice>();
        v->obj = g_nextObj++;
        v->channel = (s.stereo && count >= 2) ? 1 + (c & 1) : 0;   // stereo: speakers alternate L, R
        Real_RegisterGameObj(v->obj, "EchoVRMusic");
        e.voices.push_back(std::move(v));
    }
    e.positionsChanged = true;
    PlaceVoices(src, e, s);
    e.positionsChanged = false;
    AudioFormat fmt = {rate, 16, 0, 0};   // what Echo's voice chat registers: int16 mono
    for (auto& v : e.voices) {
        v->playing = Real_PostEventId(s.voiceEvent, v->obj, 0, nullptr, nullptr, 0, nullptr, 0);
        if (!v->playing) { Log("PostEvent(VoiceEvent 0x%08x) failed: is its bank loaded?", s.voiceEvent); continue; }
        v->del = {v.get(), nullptr, FillVoice};
        // Registered before this frame's RenderAudio runs, so Wwise never sees an unknown ID.
        ((RegistryRegister_t)(*(void***)reg)[6])(reg, v->playing, &v->del, &fmt);
    }
    e.rate = rate;
    Log("speaker on %s (0x%llx): %zu voice(s), %zu position(s), %u Hz", e.name.empty() ? "?" : e.name.c_str(),
        (unsigned long long)src, e.voices.size(), positions, rate);
}

// Distance shaping: each placed speaker plays at full volume within FullVolume metres of your head,
// fades out (quadratically) by Cutoff metres, and only the Nearest N play when Nearest > 0. Echo's
// own voice fade is too gentle to tell speakers apart (still audible at 40-50 m).
static void ShapeByDistance(const Settings& s) {
    std::vector<Voice*> all;
    for (auto& kv : g_emitters) for (auto& v : kv.second.voices) all.push_back(v.get());
    if (all.empty()) return;
    uint64_t ids[4]; uint32_t n = 4;
    AkTransform head = {};
    bool haveHead = AkGetListeners(all[0]->obj, ids, &n) == AK_Success && n &&
                    AkGetListenerPosition(ids[0], &head) == AK_Success;
    std::vector<std::pair<float, Voice*>> shaped;
    for (Voice* v : all) {
        if (!s.shaping || v->follows || !haveHead) { v->target = 1.0f; continue; }
        float dx = v->pos.x - head.pos.x, dy = v->pos.y - head.pos.y, dz = v->pos.z - head.pos.z;
        shaped.push_back({sqrtf(dx * dx + dy * dy + dz * dz), v});
    }
    std::sort(shaped.begin(), shaped.end(),
              [](const std::pair<float, Voice*>& a, const std::pair<float, Voice*>& b) { return a.first < b.first; });
    float r1 = s.cutoff, r0 = min(s.fullVolume, r1 - 0.1f);
    for (size_t i = 0; i < shaped.size(); i++) {
        float d = shaped[i].first, g;
        if (s.nearest > 0 && (int)i >= s.nearest) g = 0.0f;
        else if (d <= r0) g = 1.0f;
        else if (d >= r1) g = 0.0f;
        else { float t = 1.0f - (d - r0) / (r1 - r0); g = t * t; }
        shaped[i].second->target = g;
    }
}

#ifdef EVM_DEV
// Dev build: key 1 adds a speaker where your head is to this level's file in maps\ (the first press
// on a level each session starts the file over); key 2 removes the last one.
static std::atomic<bool> g_devAdd{false}, g_devUndo{false};

static std::string LevelFile(const Settings& s, uint64_t level, std::string* name) {
    auto lv = s.levels.find(level);
    char hex[32]; snprintf(hex, sizeof hex, "level_%016llx", (unsigned long long)level);
    std::string known = LevelName(level);
    *name = lv != s.levels.end() && !lv->second.name.empty() ? lv->second.name : !known.empty() ? known : hex;
    return g_dir + "EchoVRMusic\\maps\\" + *name + ".txt";
}

static void AddSpeakerAtHead(const Settings& s) {
    uint64_t obj = 0;
    for (auto& kv : g_emitters) if (!kv.second.voices.empty()) { obj = kv.second.voices[0]->obj; break; }
    uint64_t ids[4]; uint32_t n = 4;
    AkTransform head = {};
    if (!obj || AkGetListeners(obj, ids, &n) != AK_Success || !n || AkGetListenerPosition(ids[0], &head) != AK_Success) {
        Log("add speaker: no music is playing yet, so there is no listener to ask");
        return;
    }
    uint64_t level = g_level.load();
    std::string name, path = LevelFile(s, level, &name);
    static std::set<uint64_t> started;
    bool fresh = started.insert(level).second;
    CreateDirectoryA((g_dir + "EchoVRMusic").c_str(), nullptr);
    CreateDirectoryA((g_dir + "EchoVRMusic\\maps").c_str(), nullptr);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), fresh ? "w" : "a") || !f) { Log("add speaker: can't write %s", path.c_str()); return; }
    if (fresh)
        fprintf(f, "# EchoVRMusic speakers for %s, placed in game with key 1 (your head position)\n"
                   "# Echo coordinates in metres: x, y (up), z\nLevel = %s\nLevelId = 0x%016llx\n",
                name.c_str(), name.c_str(), (unsigned long long)level);
    fprintf(f, "Speaker = %.3f %.3f %.3f\n", head.pos.x, head.pos.y, head.pos.z);
    fclose(f);
    Log("added speaker at x=%.2f y=%.2f z=%.2f to %s%s", head.pos.x, head.pos.y, head.pos.z, path.c_str(),
        fresh ? " (file started over)" : "");
}

static void UndoSpeaker(const Settings& s) {
    std::string name, path = LevelFile(s, g_level.load(), &name);
    std::vector<std::string> lines;
    {
        std::string text = ReadFile(path);
        size_t i = 0;
        while (i < text.size()) {
            size_t j = text.find('\n', i);
            if (j == std::string::npos) j = text.size() - 1;
            lines.push_back(text.substr(i, j - i + 1));
            i = j + 1;
        }
    }
    for (size_t i = lines.size(); i-- > 0;) {
        if (lines[i].rfind("Speaker", 0) == 0) {
            Log("removed %s", Trim(lines[i]).c_str());
            lines.erase(lines.begin() + i);
            FILE* f = nullptr;
            if (fopen_s(&f, path.c_str(), "w") || !f) return;
            for (auto& l : lines) fputs(l.c_str(), f);
            fclose(f);
            return;
        }
    }
    Log("undo: no speakers left in %s", path.c_str());
}
#endif

static void Tick() {
    static uint32_t frame = 0;
    frame++;
    Settings s = Snapshot();
    uint32_t rate = capture::SampleRate();
    // rate is 0 for a moment while the capture restarts (music source changed). Keep the speakers
    // through that: they play silence meanwhile. Tearing them all down and back up on each change
    // crashed Wwise (a voice list still pointing at a removed voice).
    bool on = g_enabled.load();

    std::lock_guard<std::mutex> lk(g_lock);
#ifdef EVM_DEV
    if (g_devAdd.exchange(false)) AddSpeakerAtHead(s);
    if (g_devUndo.exchange(false)) UndoSpeaker(s);
#endif
    // New level or settings: rebuild every speaker (the number of voices can change).
    if (g_levelChanged.exchange(false))
        for (auto& kv : g_emitters) { DestroyVoices(kv.second); kv.second.rate = 0; kv.second.logged.clear(); }
    if (g_settingsChanged.exchange(false))
        for (auto& kv : g_emitters) kv.second.positionsChanged = true;
    // A level with Blender speakers plays them once, from its first music emitter only (the lobby
    // has nine), while every stock music emitter is still paused.
    bool mapSpeakers = s.levels.count(g_level.load()) != 0;
    uint64_t primary = 0;
    if (mapSpeakers)
        for (auto& kv : g_emitters) if (!kv.second.music.empty() && (!primary || kv.first < primary)) primary = kv.first;
    // Music that would only follow your head (no position, no speakers) stays quiet while any music
    // on the level has real speakers. Echo keeps the main menu's music (Pooled Emitter 1024)
    // running in the background of a match, and following-head voices aren't distance faded, so
    // they drowned out the arena's speakers.
    std::set<uint64_t> followers;
    bool anyPlaced = false;
    for (auto& kv : g_emitters) {
        if (kv.second.music.empty()) continue;
        int type;
        bool follow = false;
        ResolvePositions(kv.first, kv.second, s, type, &follow);
        if (follow) followers.insert(kv.first);
        else anyPlaced = true;
    }

    for (auto it = g_emitters.begin(); it != g_emitters.end();) {
        uint64_t src = it->first;
        Emitter& e = it->second;

        // Every half second, drop stock music IDs that have ended.
        if (frame % 45 == 0 && !e.music.empty()) {
            uint32_t ids[256], n = 256;
            std::set<uint32_t> alive;
            if (AkGetPlayingIDs(src, &n, ids) == AK_Success)
                for (uint32_t i = 0; i < n && i < 256; i++) if (e.music.count(ids[i])) alive.insert(ids[i]);
            if (alive.size() != e.music.size())
                Log("stock music on 0x%llx: %zu of %zu still playing", (unsigned long long)src, alive.size(), e.music.size());
            e.music.swap(alive);
        }

        bool playing = on && !e.music.empty();
        bool want = playing && (!mapSpeakers || src == primary) && !(anyPlaced && followers.count(src));
        if (!want || (rate && e.rate && e.rate != rate)) {
            if (!e.voices.empty()) { DestroyVoices(e); Log("speaker on 0x%llx stopped", (unsigned long long)src); }
            e.rate = 0;
        }
        // The number of speakers changed (speaker file edited, follow mode on/off): rebuild.
        if (want && !e.voices.empty() && (e.positionsChanged || frame % 30 == 0) &&
            e.voices.size() != SpeakerCount(src, e, s)) {
            DestroyVoices(e);
            e.rate = 0;
        }
        if (want && e.voices.empty() && rate) CreateVoices(src, e, s, rate);
        else if (want && (e.positionsChanged || e.follow || (e.positions.empty() && frame % 10 == 0))) {
            PlaceVoices(src, e, s);
            e.positionsChanged = false;
        }

        bool pause = playing && s.muteStock;
        if (pause != e.paused) {
            for (uint32_t id : e.music) AkExecuteActionOnPlayingID(pause ? ActionPause : ActionResume, id, 300, CurveLinear);
            e.paused = pause;
        }

        if (e.music.empty() && e.voices.empty()) it = g_emitters.erase(it);
        else ++it;
    }
    ShapeByDistance(s);
}

// ---- hooks -----------------------------------------------------------------------------------

static void OnPost(uint32_t ev, uint64_t obj, uint32_t playing) {
    if (IsMirror(obj)) return;
    Settings s = Snapshot();
    bool music = IsStockMusic(ev, s);
    if (!music && !s.logEvents) return;
    std::lock_guard<std::mutex> lk(g_lock);
    auto nm = g_names.find(obj);
    const char* name = nm == g_names.end() ? "" : nm->second.c_str();
    if (s.logEvents && g_loggedEvents.insert({ev, obj}).second) {
        AkTransform t = {};
        AkGetPosition(obj, &t);
        Log("PostEvent 0x%08x on 0x%llx \"%s\" at (%.1f %.1f %.1f) -> %u%s", ev, (unsigned long long)obj, name,
            t.pos.x, t.pos.y, t.pos.z, playing, music ? "  [music]" : "");
    }
    if (!music || !playing) return;
    Emitter& e = g_emitters[obj];
    if (e.name.empty()) e.name = name;
    e.events.insert(ev);
    e.music.insert(playing);
    if (e.paused) AkExecuteActionOnPlayingID(ActionPause, playing, 0, CurveLinear);
    Log("stock music 0x%08x on 0x%llx \"%s\" (playing %u)", ev, (unsigned long long)obj, name, playing);
}

static uint32_t Hook_PostEventId(uint32_t ev, uint64_t obj, uint32_t flags, void* cb, void* cookie, uint32_t nExt,
                                 void* ext, uint32_t playingId) {
    uint32_t r = Real_PostEventId(ev, obj, flags, cb, cookie, nExt, ext, playingId);
    if (g_ready) OnPost(ev, obj, r);
    return r;
}

static uint32_t Hook_PostEventStr(const char* ev, uint64_t obj, uint32_t flags, void* cb, void* cookie, uint32_t nExt,
                                  void* ext, uint32_t playingId) {
    uint32_t r = Real_PostEventStr(ev, obj, flags, cb, cookie, nExt, ext, playingId);
    if (g_ready && ev) OnPost(AkGetIDFromString(ev), obj, r);
    return r;
}

// Both RegisterGameObj overloads share this address, so `name` is garbage for the one-argument
// form. Only keep it if it reads as a short printable string.
static bool CopyName(const char* name, char (&buf)[64]) {
    __try {
        for (int i = 0; i < 63; i++) {
            char c = name[i];
            if (!c) return i > 0;
            if (c < 32 || c > 126) return false;
            buf[i] = c;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int Hook_RegisterGameObj(uint64_t obj, const char* name) {
    int r = Real_RegisterGameObj(obj, name);
    if (!g_ready || IsMirror(obj) || !name) return r;
    char buf[64] = {};
    if (CopyName(name, buf)) { std::lock_guard<std::mutex> lk(g_lock); g_names[obj] = buf; }
    return r;
}

static int Hook_UnregisterGameObj(uint64_t obj) {
    if (g_ready && !IsMirror(obj)) {
        std::lock_guard<std::mutex> lk(g_lock);
        g_names.erase(obj);
        auto it = g_emitters.find(obj);
        if (it != g_emitters.end()) it->second.music.clear();   // Tick stops its voices next frame
    }
    return Real_UnregisterGameObj(obj);
}

static int Hook_UnregisterAllGameObj() {
    if (g_ready) {
        std::lock_guard<std::mutex> lk(g_lock);
        for (auto& kv : g_emitters) DestroyVoices(kv.second);   // our objects go too
        g_emitters.clear();
        g_names.clear();
    }
    return Real_UnregisterAllGameObj();
}

static void NotePositions(uint64_t obj, const AkTransform* p, uint16_t n, int type) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_emitters.find(obj);
    if (it == g_emitters.end()) return;
    it->second.positions.assign(p, p + n);
    it->second.multiType = type;
    it->second.positionsChanged = true;
}

static int Hook_SetPosition(uint64_t obj, const AkTransform* t) {
    int r = Real_SetPosition(obj, t);
    if (g_ready && !IsMirror(obj)) {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_emitters.find(obj);
        if (it != g_emitters.end()) { it->second.positions.clear(); it->second.positionsChanged = true; }
    }
    return r;
}

static int Hook_SetMultiplePositions(uint64_t obj, const AkTransform* p, uint16_t n, int type) {
    int r = Real_SetMultiplePositions(obj, p, n, type);
    if (g_ready && p && n && !IsMirror(obj)) NotePositions(obj, p, n, type);
    return r;
}

static int Hook_SetMultipleChannelPositions(uint64_t obj, const AkChannelEmitter* p, uint16_t n, int type) {
    int r = Real_SetMultipleChannelPositions(obj, p, n, type);
    if (g_ready && p && n && !IsMirror(obj)) {
        std::vector<AkTransform> t(n);
        for (uint16_t i = 0; i < n; i++) t[i] = p[i].t;
        NotePositions(obj, t.data(), n, type);
    }
    return r;
}

typedef uint64_t (*LoadLevel_t)(void*, uint64_t, void*, void*);
static LoadLevel_t Real_LoadLevel = nullptr;

// Each match loads mnu_master_mp_ingame, then the map (mpl_combat_dyson, mpl_lobby_b2, ...), then
// extra rooms (mpl_combat_war_room, celebration rooms). The map is the first load after
// mnu_master_mp_ingame; a later load only takes over if it has its own speaker file.
static uint64_t Hook_LoadLevel(void* self, uint64_t level, void* a, void* b) {
    static const uint64_t INGAME_MENU = Symbol64("mnu_master_mp_ingame");
    static bool expectMap = true;
    Settings s = Snapshot();
    auto lv = s.levels.find(level);
    bool hasFile = lv != s.levels.end();
    const char* role;
    if (level == INGAME_MENU) { expectMap = true; role = "in-game menu"; }
    else if (expectMap || hasFile) {
        expectMap = false;
        role = "map";
        if (g_level.exchange(level) != level) g_levelChanged = true;
    } else role = "extra room, ignored";
    std::string nm = hasFile ? lv->second.name : LevelName(level);
    Log("level 0x%016llx%s%s (%s%s)", (unsigned long long)level, nm.empty() ? "" : " = ", nm.c_str(), role,
        hasFile ? ", has a speaker file" : "");
    return Real_LoadLevel(self, level, a, b);
}

static int Hook_RenderAudio(bool allowSync) {
    if (g_ready) {
        __try { Tick(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("exception in Tick, disabling"); g_ready = false; }
    }
    return Real_RenderAudio(allowSync);
}

// ---- install ---------------------------------------------------------------------------------

static bool SigMatches(uintptr_t addr, const uint8_t* sig, size_t n) {
    __try { return memcmp((const void*)addr, sig, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

template <class T> static bool Resolve(HMODULE exe, T& fn, const char* name) {
    fn = (T)GetProcAddress(exe, name);
    if (!fn) Log("echovr.exe has no export %s", name);
    return fn != nullptr;
}

static void Install() {
    HMODULE exe = GetModuleHandleA(nullptr);
    g_base = (uintptr_t)exe;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    Log("EchoVRMusic loaded into %s (base %p)", path, exe);
#ifdef EVM_DEV
    Log("DEV BUILD: key 1 places a speaker at your head, key 2 removes the last one (plugins\\EchoVRMusic\\maps\\)");
#endif

    if (!SigMatches(g_base + RVA_AUDIOINPUT_EXECUTE, SIG_EXECUTE, sizeof SIG_EXECUTE) ||
        !SigMatches(g_base + RVA_REGISTRY_LOAD, SIG_REGISTRY_LOAD, sizeof SIG_REGISTRY_LOAD)) {
        Log("not the expected echovr.exe build (Audio Input bridge signature mismatch), not hooking");
        return;
    }

    bool ok = true;
#define R(var) ok &= Resolve(exe, var, var##_name)
    R(Real_PostEventId); R(Real_PostEventStr); R(Real_RegisterGameObj); R(Real_UnregisterGameObj);
    R(Real_UnregisterAllGameObj); R(Real_SetPosition); R(Real_SetMultiplePositions);
    R(Real_SetMultipleChannelPositions); R(Real_RenderAudio); R(AkGetPosition); R(AkGetPlayingIDs);
    R(AkSetScalingFactor); R(AkStopPlayingID); R(AkExecuteActionOnPlayingID); R(AkGetIDFromString);
    R(AkGetBufferTick); R(AkGetListeners); R(AkGetListenerPosition);
#undef R
    if (!ok) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)Real_PostEventId, Hook_PostEventId);
    DetourAttach(&(PVOID&)Real_PostEventStr, Hook_PostEventStr);
    DetourAttach(&(PVOID&)Real_RegisterGameObj, Hook_RegisterGameObj);
    DetourAttach(&(PVOID&)Real_UnregisterGameObj, Hook_UnregisterGameObj);
    DetourAttach(&(PVOID&)Real_UnregisterAllGameObj, Hook_UnregisterAllGameObj);
    DetourAttach(&(PVOID&)Real_SetPosition, Hook_SetPosition);
    DetourAttach(&(PVOID&)Real_SetMultiplePositions, Hook_SetMultiplePositions);
    DetourAttach(&(PVOID&)Real_SetMultipleChannelPositions, Hook_SetMultipleChannelPositions);
    DetourAttach(&(PVOID&)Real_RenderAudio, Hook_RenderAudio);
    // echo_editor_loader.dll hooks this function too: its first 5 bytes are then a jmp, and the
    // rest still matches. Detours follows the jmp, so both hooks run.
    uintptr_t ll = g_base + RVA_LOAD_LEVEL;
    bool hooked = SigMatches(ll, (const uint8_t*)"\xE9", 1) &&
                  SigMatches(ll + 5, SIG_LOAD_LEVEL + 5, sizeof SIG_LOAD_LEVEL - 5);
    if (hooked || SigMatches(ll, SIG_LOAD_LEVEL, sizeof SIG_LOAD_LEVEL)) {
        if (hooked) Log("level load is already hooked by another plugin, chaining onto it");
        Real_LoadLevel = (LoadLevel_t)ll;
        DetourAttach(&(PVOID&)Real_LoadLevel, Hook_LoadLevel);
    } else {
        Log("level load signature mismatch: per-map speaker files are off");
    }
    LONG err = DetourTransactionCommit();
    if (err != NO_ERROR) { Log("DetourTransactionCommit failed: %ld", err); return; }

    g_ready = true;
    Log("hooked Wwise (PostEvent, RegisterGameObj, positions, RenderAudio). Ctrl+Alt+M toggles the music.");

    // Registry check once Echo has created it (the sound engine starts after the plugin loads).
    for (int i = 0; i < 600 && !Registry(); i++) Sleep(100);
    void* reg = Registry();
    if (!reg) { Log("CAudioInputCallbackRegistry not created after 60 s"); return; }
    void** vt = *(void***)reg;
    if ((uintptr_t)vt[6] != g_base + RVA_REGISTRY_REGISTER || (uintptr_t)vt[7] != g_base + RVA_REGISTRY_UNREGISTER) {
        Log("CAudioInputCallbackRegistry vtable is not the expected one, disabling");
        g_ready = false;
        return;
    }
    Log("Audio Input registry at %p", reg);
}

static void WatchThread() {
    uint64_t last = ConfigTime();
    bool down = false;
    for (;;) {
        Sleep(50);
        bool chord = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                     (GetAsyncKeyState('M') & 0x8000);
        if (chord && !down) { g_enabled = !g_enabled; Log("music through speakers %s", g_enabled ? "ON" : "OFF"); }
        down = chord;
#ifdef EVM_DEV
        {   // keys 1 and 2, only while Echo has focus
            DWORD fgPid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
            bool focused = fgPid == GetCurrentProcessId();
            static bool d1 = false, d2 = false;
            bool k1 = focused && ((GetAsyncKeyState('1') | GetAsyncKeyState(VK_NUMPAD1)) & 0x8000);
            bool k2 = focused && ((GetAsyncKeyState('2') | GetAsyncKeyState(VK_NUMPAD2)) & 0x8000);
            if (k1 && !d1) g_devAdd = true;
            if (k2 && !d2) g_devUndo = true;
            d1 = k1; d2 = k2;
        }
#endif
        static int n = 0;
        if (++n % 10 == 0) {
            uint64_t t = ConfigTime();
            if (t != last) { last = t; LoadConfig(false); }
        }
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        char p[MAX_PATH] = {};
        GetModuleFileNameA(module, p, MAX_PATH);
        std::string s = p;
        size_t slash = s.find_last_of("\\/");
        g_dir = slash == std::string::npos ? "" : s.substr(0, slash + 1);
        DeleteFileA((g_dir + "EchoVRMusic.log").c_str());
        std::thread([] {
            LoadConfig(true);
            Install();
        }).detach();
        std::thread(WatchThread).detach();
    }
    return TRUE;
}

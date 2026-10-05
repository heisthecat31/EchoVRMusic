// WASAPI process loopback. Runs on its own thread and fills a stereo float ring.
#include "capture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <tlhelp32.h>
#include <wrl/implements.h>
#include <wrl/ftm.h>

#include <atomic>
#include <thread>
#include <algorithm>
#include <cwctype>
#include <cmath>

namespace capture {

static float g_ring[kFrames * 2];
static std::atomic<uint64_t> g_write{0};
static std::atomic<uint32_t> g_rate{0};
static std::atomic<bool> g_run{false};
static std::thread g_thread;
static void (*Log)(const char*, ...) = [](const char*, ...) {};

uint32_t SampleRate() { return g_rate.load(); }
uint64_t WriteCursor() { return g_write.load(std::memory_order_acquire); }
const float* Ring() { return g_ring; }

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

static std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static DWORD FindPid(const std::wstring& exe) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {sizeof(pe)};
    std::wstring want = Lower(exe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (Lower(pe.szExeFile) == want) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

class ActivateHandler : public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
        Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT hr = E_FAIL;
    IAudioClient* client = nullptr;
    ~ActivateHandler() { CloseHandle(done); }
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override {
        IUnknown* unk = nullptr; HRESULT act = E_FAIL;
        hr = op->GetActivateResult(&act, &unk);
        if (SUCCEEDED(hr)) hr = act;
        if (SUCCEEDED(hr) && unk) hr = unk->QueryInterface(IID_PPV_ARGS(&client));
        if (unk) unk->Release();
        SetEvent(done);
        return S_OK;
    }
};

static IAudioClient* ActivateProcessLoopback(DWORD pid, bool include) {
    AUDIOCLIENT_ACTIVATION_PARAMS ap = {};
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = pid;
    ap.ProcessLoopbackParams.ProcessLoopbackMode =
        include ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv = {}; pv.vt = VT_BLOB; pv.blob.cbSize = sizeof(ap); pv.blob.pBlobData = (BYTE*)&ap;

    auto h = Microsoft::WRL::Make<ActivateHandler>();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                             &pv, h.Get(), &op);
    if (FAILED(hr)) { Log("ActivateAudioInterfaceAsync failed 0x%08lx", hr); return nullptr; }
    WaitForSingleObject(h->done, 5000);
    if (op) op->Release();
    if (FAILED(h->hr) || !h->client) { Log("process loopback activation failed 0x%08lx", h->hr); return nullptr; }
    return h->client;
}

// Limiter on the captured mix. With "all of Windows", several apps playing at once add up past
// full scale, and hard clipping that is what distorted the music. The gain drops at once to keep
// peaks under kCeiling, then recovers over about 150 ms.
static const float kCeiling = 0.89f;                  // about -1 dBFS
static const float kRelease = 0.99986f;               // per sample at 48 kHz: ~150 ms
static float g_envelope = 0.0f;

static void Push(const float* data, UINT32 frames, bool silent) {
    uint64_t w = g_write.load(std::memory_order_relaxed);
    for (UINT32 i = 0; i < frames; i++) {
        float* dst = g_ring + ((w + i) & kMask) * 2;
        float l = silent ? 0.0f : data[i * 2], r = silent ? 0.0f : data[i * 2 + 1];
        float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        g_envelope = peak > g_envelope ? peak : g_envelope * kRelease + peak * (1.0f - kRelease);
        float g = g_envelope > kCeiling ? kCeiling / g_envelope : 1.0f;
        dst[0] = l * g;
        dst[1] = r * g;
    }
    g_write.store(w + frames, std::memory_order_release);
}

static void Run(std::string source) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IAudioClient* ac = nullptr;
    IAudioCaptureClient* cc = nullptr;
    // We pick the format: process loopback has no mix format, and converts to this for us.
    WAVEFORMATEX wf = {WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 48000 * 8, 8, 32, 0};

    bool include = source.rfind("process:", 0) == 0;
    DWORD pid = include ? FindPid(Widen(source.substr(8))) : GetCurrentProcessId();
    if (!include && source != "system") Log("unknown Source \"%s\", using system", source.c_str());
    if (!pid) { Log("\"%s\" is not running", source.substr(8).c_str()); goto out; }
    Log("capturing %s (process loopback)", include ? source.substr(8).c_str() : "all Windows audio except Echo");

    ac = ActivateProcessLoopback(pid, include);
    if (!ac) goto out;
    {
        HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                    200 * 10000, 0, &wf, nullptr);
        if (FAILED(hr)) { Log("IAudioClient::Initialize failed 0x%08lx", hr); goto out; }
        if (FAILED(ac->GetService(IID_PPV_ARGS(&cc)))) { Log("GetService(IAudioCaptureClient) failed"); goto out; }
        g_rate = wf.nSamplesPerSec;
        ac->Start();
        while (g_run) {
            Sleep(5);
            UINT32 packet = 0;
            while (g_run && SUCCEEDED(cc->GetNextPacketSize(&packet)) && packet) {
                BYTE* data; UINT32 frames; DWORD fl;
                if (FAILED(cc->GetBuffer(&data, &frames, &fl, nullptr, nullptr))) break;
                Push((const float*)data, frames, (fl & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
                cc->ReleaseBuffer(frames);
            }
        }
        ac->Stop();
    }

out:
    g_rate = 0;
    if (cc) cc->Release();
    if (ac) ac->Release();
    CoUninitialize();
}

void Start(const std::string& source, void (*log)(const char*, ...)) {
    Stop();
    Log = log;
    g_run = true;
    g_thread = std::thread(Run, source);
}

void Stop() {
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace capture

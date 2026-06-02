// TwoAudioSessions.cpp
//
// 在同一個 process / 同一個 default render endpoint 上，
// 用兩個不同的 AudioSessionGuid 同時建立兩個 audio session，
// 分別播放兩段不同的旋律：
//   Session A : 一閃一閃亮晶晶  (Twinkle Twinkle Little Star)
//   Session B : 兩隻老虎       (Frère Jacques，高八度)
//
// 搭配 AudioSessionMonitor.exe 一起執行，可以看到:
//   1. 兩個獨立的 SessionInstanceIdentifier (代表兩個 session)
//   2. APO (MFX/SFX) 的 debug log 與哪個 session 對應
//      - MFX (Mode Effects) 通常是 per-session  -> 會看到兩份
//      - SFX/EFX (Stream/Endpoint Effects) 通常是 per-endpoint -> 只會看到一份
//
// Build (Developer Command Prompt):
//   cl /EHsc /std:c++17 TwoAudioSessions.cpp
//
// Run as the same user (and same elevation) as AudioSessionMonitor.exe.

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmreg.h>
#include <ksmedia.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace
{
    constexpr double PI = 3.14159265358979323846;

    std::atomic<bool> g_running = true;

    // ---- 音符與旋律 -------------------------------------------------------
    struct Note
    {
        double freqHz;       // 0 = 休止符
        double durationSec;
    };

    // 音名 (12-TET, A4 = 440)
    constexpr double C4 = 261.63;
    constexpr double D4 = 293.66;
    constexpr double E4 = 329.63;
    constexpr double F4 = 349.23;
    constexpr double G4 = 392.00;
    constexpr double A4 = 440.00;
    constexpr double C5 = 523.25;
    constexpr double D5 = 587.33;
    constexpr double E5 = 659.25;
    constexpr double F5 = 698.46;
    constexpr double G5 = 783.99;

    // Session A: 一閃一閃亮晶晶 (Twinkle Twinkle Little Star)
    const std::vector<Note> kMelodyTwinkle = {
        { C4, 0.40 }, { C4, 0.40 }, { G4, 0.40 }, { G4, 0.40 },
        { A4, 0.40 }, { A4, 0.40 }, { G4, 0.80 },
        { F4, 0.40 }, { F4, 0.40 }, { E4, 0.40 }, { E4, 0.40 },
        { D4, 0.40 }, { D4, 0.40 }, { C4, 0.80 },
        { 0.0, 0.50 },   // 段落間休止
    };

    // Session B: 兩隻老虎 (Frère Jacques) — 用高八度，更容易和 Session A 區分
    const std::vector<Note> kMelodyFrere = {
        { C5, 0.35 }, { D5, 0.35 }, { E5, 0.35 }, { C5, 0.35 },
        { C5, 0.35 }, { D5, 0.35 }, { E5, 0.35 }, { C5, 0.35 },
        { E5, 0.35 }, { F5, 0.35 }, { G5, 0.70 },
        { E5, 0.35 }, { F5, 0.35 }, { G5, 0.70 },
        { 0.0, 0.50 },
    };

    struct StreamConfig
    {
        GUID              sessionGuid;
        std::vector<Note> melody;
        std::wstring      label;
    };

    std::wstring GuidToString(const GUID& guid)
    {
        wchar_t buffer[64] = {};
        StringFromGUID2(guid, buffer, ARRAYSIZE(buffer));
        return buffer;
    }

    void Log(const std::wstring& label, const std::wstring& message)
    {
        LARGE_INTEGER qpc = {};
        QueryPerformanceCounter(&qpc);
        std::wcout << L"[QPC=" << qpc.QuadPart << L"]"
            << label << L" " << message << std::endl;
    }

    bool DetectIsFloat(const WAVEFORMATEX* fmt)
    {
        if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        {
            return true;
        }
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            const WAVEFORMATEXTENSIBLE* ext =
                reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
            return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        }
        return false;
    }

    void WriteFrame(BYTE* dst, float sample, bool isFloat, DWORD bytesPerSample)
    {
        if (isFloat && bytesPerSample == 4)
        {
            *reinterpret_cast<float*>(dst) = sample;
        }
        else if (bytesPerSample == 2)
        {
            int32_t v = static_cast<int32_t>(sample * 32767.0f);
            if (v > 32767)  v = 32767;
            if (v < -32768) v = -32768;
            *reinterpret_cast<int16_t*>(dst) = static_cast<int16_t>(v);
        }
        else if (bytesPerSample == 4)
        {
            // 32-bit PCM
            int64_t v = static_cast<int64_t>(sample * 2147483647.0f);
            if (v > INT32_MAX) v = INT32_MAX;
            if (v < INT32_MIN) v = INT32_MIN;
            *reinterpret_cast<int32_t*>(dst) = static_cast<int32_t>(v);
        }
        else
        {
            std::memset(dst, 0, bytesPerSample);
        }
    }

    void RenderMelody(StreamConfig config)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr))
        {
            std::wcerr << config.label << L" CoInitializeEx failed: 0x"
                << std::hex << hr << std::endl;
            return;
        }

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* audioClient = nullptr;
        IAudioRenderClient* renderClient = nullptr;
        WAVEFORMATEX* mixFormat = nullptr;

        auto cleanup = [&]()
            {
                if (renderClient) renderClient->Release();
                if (mixFormat)    CoTaskMemFree(mixFormat);
                if (audioClient)  audioClient->Release();
                if (device)       device->Release();
                if (enumerator)   enumerator->Release();
                CoUninitialize();
            };

        auto fail = [&](const wchar_t* step)
            {
                std::wcerr << config.label << L" " << step
                    << L" failed: 0x" << std::hex << hr << std::endl;
                cleanup();
            };

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) { fail(L"CoCreateInstance(MMDeviceEnumerator)"); return; }

        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) { fail(L"GetDefaultAudioEndpoint"); return; }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&audioClient));
        if (FAILED(hr)) { fail(L"Activate(IAudioClient)"); return; }

        hr = audioClient->GetMixFormat(&mixFormat);
        if (FAILED(hr)) { fail(L"GetMixFormat"); return; }

        // 2 秒的 buffer，share mode
        const REFERENCE_TIME bufferDuration = 2LL * 10000000LL;

        // <<< 重點: 傳入不同的 sessionGuid 才會分出獨立的 audio session >>>
        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            bufferDuration,
            0,
            mixFormat,
            &config.sessionGuid);
        if (FAILED(hr)) { fail(L"IAudioClient::Initialize"); return; }

        UINT32 bufferFrameCount = 0;
        hr = audioClient->GetBufferSize(&bufferFrameCount);
        if (FAILED(hr)) { fail(L"GetBufferSize"); return; }

        hr = audioClient->GetService(__uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(&renderClient));
        if (FAILED(hr)) { fail(L"GetService(IAudioRenderClient)"); return; }

        const DWORD channels = mixFormat->nChannels;
        const DWORD sampleRate = mixFormat->nSamplesPerSec;
        const DWORD bytesPerSample = mixFormat->wBitsPerSample / 8;
        const bool  isFloat = DetectIsFloat(mixFormat);

        // 先填一段靜音，避免一開始 underrun
        BYTE* initialData = nullptr;
        hr = renderClient->GetBuffer(bufferFrameCount, &initialData);
        if (SUCCEEDED(hr))
        {
            std::memset(initialData, 0,
                static_cast<size_t>(bufferFrameCount) * mixFormat->nBlockAlign);
            renderClient->ReleaseBuffer(bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        hr = audioClient->Start();
        if (FAILED(hr)) { fail(L"Start"); return; }

        {
            std::wstringstream ss;
            ss << L"Started SessionGUID=" << GuidToString(config.sessionGuid)
                << L" Notes=" << config.melody.size()
                << L" SampleRate=" << sampleRate
                << L" Channels=" << channels
                << L" Bits=" << mixFormat->wBitsPerSample
                << L" Float=" << (isFloat ? L"yes" : L"no");
            Log(config.label, ss.str());
        }

        // ---- 旋律播放狀態 ----------------------------------------------
        const float  amplitude = 0.15f;
        const UINT64 envelopeSamples = sampleRate / 100;  // 10 ms 漸入/漸出避免 click

        size_t   noteIndex = 0;
        UINT64   noteSamplePos = 0;
        UINT64   noteDurationSamples =
            static_cast<UINT64>(config.melody[0].durationSec * sampleRate);
        if (noteDurationSamples == 0) noteDurationSamples = 1;
        double   phase = 0.0;
        double   phaseInc = (config.melody[0].freqHz > 0.0)
            ? 2.0 * PI * config.melody[0].freqHz / sampleRate
            : 0.0;

        const DWORD sleepMs =
            static_cast<DWORD>((bufferDuration / 10000) / 4);  // ~ buffer/4

        while (g_running.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

            UINT32 padding = 0;
            if (FAILED(audioClient->GetCurrentPadding(&padding))) break;

            UINT32 framesAvailable = bufferFrameCount - padding;
            if (framesAvailable == 0) continue;

            BYTE* buffer = nullptr;
            if (FAILED(renderClient->GetBuffer(framesAvailable, &buffer))) break;

            for (UINT32 frame = 0; frame < framesAvailable; ++frame)
            {
                // 推進到下一個音符 (循環播放)
                if (noteSamplePos >= noteDurationSamples)
                {
                    noteIndex = (noteIndex + 1) % config.melody.size();
                    noteSamplePos = 0;
                    noteDurationSamples = static_cast<UINT64>(
                        config.melody[noteIndex].durationSec * sampleRate);
                    if (noteDurationSamples == 0) noteDurationSamples = 1;
                    phase = 0.0;
                    phaseInc = (config.melody[noteIndex].freqHz > 0.0)
                        ? 2.0 * PI * config.melody[noteIndex].freqHz / sampleRate
                        : 0.0;
                }

                float sample = 0.0f;
                if (phaseInc > 0.0)
                {
                    // 音符前後加 ~10ms envelope，避免換音瞬間 click/pop
                    double env = 1.0;
                    if (noteSamplePos < envelopeSamples)
                    {
                        env = static_cast<double>(noteSamplePos) /
                              static_cast<double>(envelopeSamples);
                    }
                    else if (noteDurationSamples > envelopeSamples &&
                             noteSamplePos > noteDurationSamples - envelopeSamples)
                    {
                        env = static_cast<double>(noteDurationSamples - noteSamplePos) /
                              static_cast<double>(envelopeSamples);
                    }
                    sample = static_cast<float>(amplitude * env * std::sin(phase));
                    phase += phaseInc;
                    if (phase > 2.0 * PI) phase -= 2.0 * PI;
                }

                for (DWORD ch = 0; ch < channels; ++ch)
                {
                    BYTE* dst = buffer + (frame * channels + ch) * bytesPerSample;
                    WriteFrame(dst, sample, isFloat, bytesPerSample);
                }

                ++noteSamplePos;
            }

            renderClient->ReleaseBuffer(framesAvailable, 0);
        }

        audioClient->Stop();
        Log(config.label, L"Stopped");
        cleanup();
    }
}

int wmain()
{
    SetConsoleOutputCP(CP_UTF8);

    StreamConfig configA;
    configA.melody = kMelodyTwinkle;
    configA.label  = L"[SessionA]";
    CoCreateGuid(&configA.sessionGuid);

    StreamConfig configB;
    configB.melody = kMelodyFrere;
    configB.label  = L"[SessionB]";
    CoCreateGuid(&configB.sessionGuid);

    std::wcout << L"=== TwoAudioSessions ===" << std::endl;
    std::wcout << L"PID            : " << GetCurrentProcessId() << std::endl;
    std::wcout << L"SessionA GUID  : " << GuidToString(configA.sessionGuid)
        << L"  (Twinkle Twinkle Little Star)" << std::endl;
    std::wcout << L"SessionB GUID  : " << GuidToString(configB.sessionGuid)
        << L"  (Frere Jacques, +1 octave)" << std::endl;
    std::wcout << L"Start AudioSessionMonitor.exe BEFORE this program "
        L"so the [Created] event is captured." << std::endl;
    std::wcout << L"Press Enter to stop both sessions." << std::endl;
    std::wcout << std::endl;

    std::thread threadA(RenderMelody, configA);

    // 故意錯開一點，讓 monitor log 比較好讀
    //std::this_thread::sleep_for(std::chrono::milliseconds(750));
    //std::this_thread::sleep_for(std::chrono::milliseconds(10));


    std::thread threadB(RenderMelody, configB);

    std::wcin.get();
    g_running.store(false);

    if (threadA.joinable()) threadA.join();
    if (threadB.joinable()) threadB.join();

    std::wcout << L"Bye." << std::endl;
    return 0;
}

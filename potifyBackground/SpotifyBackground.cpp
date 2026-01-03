#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <vector>
#include <cmath>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "mmdevapi.lib")
#include <wrl/client.h>
#include <fstream>
#include <string>
#include <random>
#include <chrono>

struct StreamingServiceConfig {
    std::string exe{ "Spotify.exe" };
    float       volumeToIncrease = 0.65;
    float       volumeToDecrease = 0.3;
};

enum AppNumerator {
    Brave = 0,
    Telegram = 1,
    Discord = 2,
	Spotify = 3
};

using Microsoft::WRL::ComPtr;
ComPtr<IMMDeviceEnumerator> m_devEnum;
ComPtr<IMMDevice> m_device;
ComPtr<IAudioSessionManager2> m_mgr;
ComPtr<IAudioSessionEnumerator> m_sessEnum;

std::vector<ComPtr<ISimpleAudioVolume>> volume;
std::vector<ComPtr<IAudioSessionControl>> ctrl;
std::vector<ComPtr<IAudioMeterInformation>> meter;
std::vector<const wchar_t*> proccesses = { L"brave.exe", L"Telegram.exe", L"Discord.exe", L"Spotify.exe" };

bool load(const std::string& path, StreamingServiceConfig& s)
{
    std::ifstream f(path);
    if (!f) return false;

    std::getline(f, s.exe);   // line 1: executable name
    f >> s.volumeToIncrease;            // line 2: volume
    f >> s.volumeToDecrease;              // line 3: peak

    return f.good();
}

void FindProccess(const wchar_t* nameproccess, int enumeration, int countofsessions, bool& found) {
    found = false;
    if (enumeration < 0 || static_cast<size_t>(enumeration) >= ctrl.size()) {
        return;
    }
    ctrl[enumeration].Reset();

    for (int i = 0; i < countofsessions; ++i) {
        ComPtr<IAudioSessionControl> session;
        if (FAILED(m_sessEnum->GetSession(i, session.GetAddressOf()))) continue;

        ComPtr<IAudioSessionControl2> session2;
        if (FAILED(session.As(&session2))) continue;

        DWORD pid = 0;
        session2->GetProcessId(&pid);

        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE, pid);
        if (!hProc) continue;

        wchar_t name[512]{};
        DWORD nameLen = _countof(name);
        if (QueryFullProcessImageNameW(hProc, 0, name, &nameLen)) {
            if (wcsstr(name, nameproccess)) {
                ctrl[enumeration] = session;
                found = true;
                CloseHandle(hProc);
                return;
            }
        }
        CloseHandle(hProc);
    }
}

void SetVolume(float volumePercent, const wchar_t* nameproccess, int enumeration, int countofsessions) {
    if (volumePercent < 0.0f) volumePercent = 0.0f;
    if (volumePercent > 1.0f) volumePercent = 1.0f;

    bool found = false;
    FindProccess(nameproccess, enumeration, countofsessions, found);
    if (!found) {
        std::wcout << L"Process " << nameproccess << L" not found, cannot set volume.\n";
        return;
    }

    if (SUCCEEDED(ctrl[enumeration].As(&volume[enumeration]))) {
        if (volume[enumeration]) {
            volume[enumeration]->SetMasterVolume(volumePercent, nullptr);
            std::wcout << L"Volume set to " << int(volumePercent * 100.0f) << L"% for " << nameproccess << L"\n";
        }
    }
    else {
        std::wcout << L"Failed to get ISimpleAudioVolume for " << nameproccess << L"\n";
    }
}

void FadeToVolume(float targetVolume, float step, int delayMs, const wchar_t* nameproccess, int enumeration, int countofsessions) {
    if (targetVolume < 0.0f) targetVolume = 0.0f;
    if (targetVolume > 1.0f) targetVolume = 1.0f;
    if (step <= 0.0f) step = 0.01f;

    // Ensure session control and volume interface available
    bool found = false;
    FindProccess(nameproccess, enumeration, countofsessions, found);
    if (!found) {
        std::wcout << L"Process " << nameproccess << L" not found, cannot fade volume.\n";
        return;
    }

    if (FAILED(ctrl[enumeration].As(&volume[enumeration])) || !volume[enumeration]) {
        std::wcout << L"Failed to get ISimpleAudioVolume for " << nameproccess << L"\n";
        return;
    }

    float current = 0.0f;
    if (FAILED(volume[enumeration]->GetMasterVolume(&current))) {
        volume[enumeration]->SetMasterVolume(targetVolume, nullptr);
        std::wcout << L"Could not read current volume, set directly to " << int(targetVolume * 100.0f) << L"%\n";
        return;
    }

    if (fabsf(current - targetVolume) <= (step * 0.5f)) {
        return;
    }

    float direction = (targetVolume > current) ? 1.0f : -1.0f;
    float stepSigned = step * direction;

    std::wcout << L"Fading volume for " << nameproccess << L" from " << int(current * 100.0f) << L"% to " << int(targetVolume * 100.0f) << L"%\n";

    while (true) {
        float next = current + stepSigned;

        if ((direction > 0 && next >= targetVolume) || (direction < 0 && next <= targetVolume)) {
            volume[enumeration]->SetMasterVolume(targetVolume, nullptr);
            std::wcout << L"Set final volume to " << int(targetVolume * 100.0f) << L"%\n";
            break;
        }

        if (next < 0.0f) next = 0.0f;
        if (next > 1.0f) next = 1.0f;
        volume[enumeration]->SetMasterVolume(next, nullptr);

        current = next;
        Sleep(delayMs);

        if (!ctrl[enumeration] || !volume[enumeration]) {
            std::wcout << L"Audio session lost for " << nameproccess << L", aborting fade.\n";
            break;
        }
    }
}

void GetVolume(float& volumePercent, int enumeration, const wchar_t* nameproccess, int countofsessions) {
    volumePercent = 0.0f;
    bool found = false;
    FindProccess(nameproccess, enumeration, countofsessions, found);
    if (found) {
        if (SUCCEEDED(ctrl[enumeration].As(&meter[enumeration]))) {
            if (meter[enumeration]) {
                meter[enumeration]->GetPeakValue(&volumePercent);
                std::wprintf(L"%ls volume peak: %.4f\n", nameproccess, volumePercent);
            }
        } else {
            std::wprintf(L"Failed to get IAudioMeterInformation for %ls\n", nameproccess);
        }
    }
}

int main()
{
    HRESULT hr = CoInitialize(nullptr);
    if (FAILED(hr)) {
        std::cerr << "CoInitialize failed\n";
        return 1;
    }

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)m_devEnum.GetAddressOf()))) {
        CoUninitialize();
        return 1;
    }

    if (FAILED(m_devEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, m_device.GetAddressOf()))) {
        CoUninitialize();
        return 1;
    }

    if (FAILED(m_device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
        nullptr, (void**)m_mgr.GetAddressOf()))) {
        CoUninitialize();
        return 1;
    }

    if (FAILED(m_mgr->GetSessionEnumerator(m_sessEnum.GetAddressOf()))) {
        CoUninitialize();
        return 1;
    }

    int count = 0;
    m_sessEnum->GetCount(&count);

    printf("Running Spotify volume adjuster. To safely exit press ESC\n");
	StreamingServiceConfig config;
	if (load("spotifybackground.cfg", config)) {
        std::wcout << L"Loaded config: " << std::wstring(config.exe.begin(), config.exe.end())
            << L", volume to increase: " << config.volumeToIncrease
            << L", volume to decrease: " << config.volumeToDecrease << L"\n";
    } else {
        std::wcout << L"Using default config.\n";
    }
    size_t appCount = proccesses.size();
    volume.resize(appCount);
    ctrl.resize(appCount);
    meter.resize(appCount);

    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_int_distribution<int> dist(1000, 2000);

    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            break;
        }

        int randDelay = dist(mt);
        printf("%d random delay\n", randDelay);
        bool foundBackgroundNoise = false;
        bool tooLoud = false;
        for (uint32_t i = 0; i < proccesses.size() - 1; ++i) {
            float peak;
            GetVolume(peak, i, proccesses[i], count);
            if (peak > 0.0001f) {
                foundBackgroundNoise = true;
                if (peak > 0.2500f) tooLoud = true;
                break;
            }
        }

        if (!tooLoud) {
            if (foundBackgroundNoise) {
                FadeToVolume(config.volumeToDecrease, 0.01f, 100, L"Spotify.exe", Spotify, count);
                printf("something good may happen\n");
            }
            else {
                FadeToVolume(config.volumeToIncrease, 0.01f, 100, L"Spotify.exe", Spotify, count);
                printf("back in depression\n");
            }
        }
        else {
			printf("too loud \n");
            FadeToVolume(0.1f, 0.1f, 50, L"Spotify.exe", Spotify, count);
        }
        Sleep(randDelay);
    }

    for (size_t i = 0; i < volume.size(); ++i) volume[i].Reset();
    for (size_t i = 0; i < ctrl.size(); ++i) ctrl[i].Reset();
    for (size_t i = 0; i < meter.size(); ++i) meter[i].Reset();

    m_sessEnum.Reset();
    m_mgr.Reset();
    m_device.Reset();
    m_devEnum.Reset();

    CoUninitialize();
    return 0;
}
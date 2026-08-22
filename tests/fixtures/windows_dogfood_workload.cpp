#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

int cpu_starvation() {
    const auto workers = (std::max)(2U, std::thread::hardware_concurrency());
    std::atomic<bool> start{};
    std::atomic<std::uint64_t> combined{};
    std::vector<std::jthread> threads;
    threads.reserve(workers);
    for (unsigned worker = 0U; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            std::uint64_t value = worker + 1U;
            while (std::chrono::steady_clock::now() < deadline) {
                for (std::size_t iteration = 0U; iteration < 100'000U; ++iteration) {
                    value = value * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
                }
            }
            combined.fetch_xor(value, std::memory_order_relaxed);
        });
    }
    start.store(true, std::memory_order_release);
    threads.clear();
    return combined.load(std::memory_order_relaxed) == 0U ? 1 : 0;
}

class NativeHandle final {
public:
    explicit NativeHandle(const HANDLE value = INVALID_HANDLE_VALUE) : value(value) {}
    ~NativeHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    HANDLE value{INVALID_HANDLE_VALUE};
};

int disk_stall() {
    wchar_t directory[MAX_PATH]{};
    wchar_t path[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, directory) == 0U ||
        GetTempFileNameW(directory, L"bbd", 0U, path) == 0U) return 1;
    NativeHandle file{CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0U, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_NO_BUFFERING |
                           FILE_FLAG_WRITE_THROUGH, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return 1;
    constexpr DWORD chunk_size = 1024U * 1024U;
    auto* buffer = VirtualAlloc(nullptr, chunk_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (buffer == nullptr) return 1;
    std::memset(buffer, 0xA5, chunk_size);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::uint64_t bytes{};
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD completed{};
        if (WriteFile(file.value, buffer, chunk_size, &completed, nullptr) == 0 ||
            completed != chunk_size) {
            VirtualFree(buffer, 0U, MEM_RELEASE);
            return 1;
        }
        bytes += completed;
        if (bytes % (64ULL * chunk_size) == 0U) FlushFileBuffers(file.value);
    }
    FlushFileBuffers(file.value);
    VirtualFree(buffer, 0U, MEM_RELEASE);
    CloseHandle(file.value);
    file.value = INVALID_HANDLE_VALUE;
    DeleteFileW(path);
    return bytes == 0U ? 1 : 0;
}

class Socket final {
public:
    explicit Socket(const SOCKET value = INVALID_SOCKET) : value(value) {}
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    SOCKET value{INVALID_SOCKET};
};

int network_interruption() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    std::size_t resets{};
    for (std::size_t attempt = 0U; attempt < 512U; ++attempt) {
        Socket listener{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        if (listener.value == INVALID_SOCKET) break;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0U;
        if (bind(listener.value, reinterpret_cast<const sockaddr*>(&address),
                 sizeof(address)) != 0 || listen(listener.value, 1) != 0) break;
        int size = sizeof(address);
        if (getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &size) != 0) break;
        Socket client{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        if (client.value == INVALID_SOCKET ||
            connect(client.value, reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) != 0) break;
        Socket server{accept(listener.value, nullptr, nullptr)};
        if (server.value == INVALID_SOCKET) break;
        linger abortive{1, 0};
        setsockopt(server.value, SOL_SOCKET, SO_LINGER,
                   reinterpret_cast<const char*>(&abortive), sizeof(abortive));
        closesocket(server.value);
        server.value = INVALID_SOCKET;
        char byte{'x'};
        send(client.value, &byte, 1, 0);
        recv(client.value, &byte, 1, 0);
        ++resets;
    }
    WSACleanup();
    return resets < 100U ? 1 : 0;
}

LRESULT CALLBACK window_proc(const HWND window, const UINT message,
                             const WPARAM wparam, const LPARAM lparam) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

[[nodiscard]] HWND dogfood_window(const wchar_t* title) {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"BlackBoxDogfoodWorkload";
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&window_class);
    const auto window = CreateWindowExW(0U, window_class.lpszClassName, title,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 360,
        nullptr, nullptr, instance, nullptr);
    if (window != nullptr) {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
        SetForegroundWindow(window);
    }
    return window;
}

int application_hang() {
    const auto window = dogfood_window(L"BlackBox dogfood: deliberate application hang");
    if (window == nullptr) return 1;
    Sleep(5'000U); // Deliberately do not dispatch the window queue.
    DestroyWindow(window);
    return 0;
}

int game_stutter() {
    const auto window = dogfood_window(L"BlackBox dogfood: controlled frame stutter");
    if (window == nullptr) return 1;
    const auto started = std::chrono::steady_clock::now();
    bool stalled{};
    std::size_t frame{};
    while (std::chrono::steady_clock::now() - started < 5s) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (!stalled && elapsed >= 1500ms) {
            stalled = true;
            Sleep(1'500U); // Ground-truth presentation stall.
        }
        const auto device = GetDC(window);
        RECT area{};
        GetClientRect(window, &area);
        const auto brush = CreateSolidBrush(frame++ % 2U == 0U ? RGB(30, 120, 220)
                                                               : RGB(220, 120, 30));
        FillRect(device, &area, brush);
        DeleteObject(brush);
        ReleaseDC(window, device);
        Sleep(16U);
    }
    DestroyWindow(window);
    return 0;
}

[[nodiscard]] bool play_tone(HWAVEOUT output, const double seconds,
                             const double frequency) {
    constexpr std::size_t sample_rate = 48'000U;
    const auto sample_count = static_cast<std::size_t>(seconds * sample_rate);
    std::vector<std::int16_t> samples(sample_count);
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        samples[index] = static_cast<std::int16_t>(
            3'000.0 * std::sin(2.0 * pi * frequency *
                               static_cast<double>(index) / sample_rate));
    }
    WAVEHDR header{};
    header.lpData = reinterpret_cast<LPSTR>(samples.data());
    header.dwBufferLength = static_cast<DWORD>(samples.size() * sizeof(samples.front()));
    if (waveOutPrepareHeader(output, &header, sizeof(header)) != MMSYSERR_NOERROR)
        return false;
    const auto written = waveOutWrite(output, &header, sizeof(header)) == MMSYSERR_NOERROR;
    while (written && (header.dwFlags & WHDR_DONE) == 0U) Sleep(10U);
    const auto unprepared = waveOutUnprepareHeader(output, &header, sizeof(header));
    return written && unprepared == MMSYSERR_NOERROR;
}

int audio_interruption() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1U;
    format.nSamplesPerSec = 48'000U;
    format.wBitsPerSample = 16U;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8U;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    HWAVEOUT output{};
    if (waveOutOpen(&output, WAVE_MAPPER, &format, 0U, 0U,
                    CALLBACK_NULL) != MMSYSERR_NOERROR) return 1;
    const auto before = play_tone(output, 1.5, 440.0);
    Sleep(1'500U); // Deliberately starve playback across the incident marker.
    const auto after = play_tone(output, 2.0, 440.0);
    waveOutReset(output);
    const auto closed = waveOutClose(output) == MMSYSERR_NOERROR;
    return before && after && closed ? 0 : 1;
}

} // namespace

int main(const int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: blackbox_dogfood_workload "
                     "<cpu|disk|network|hang|game-stutter|audio-glitch>\n";
        return 2;
    }
    const std::string_view mode{argv[1]};
    if (mode == "cpu") return cpu_starvation();
    if (mode == "disk") return disk_stall();
    if (mode == "network") return network_interruption();
    if (mode == "hang") return application_hang();
    if (mode == "game-stutter") return game_stutter();
    if (mode == "audio-glitch") return audio_interruption();
    std::cerr << "Unknown dogfood workload mode.\n";
    return 2;
}

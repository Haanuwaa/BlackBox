#include "core/clock.hpp"
#include "telemetry/windows/windows_telemetry_provider.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace core = blackbox::core;
namespace telemetry = blackbox::telemetry;
namespace windows = blackbox::telemetry::windows;

namespace {

struct HandleGuard {
    HANDLE value{INVALID_HANDLE_VALUE};
    [[nodiscard]] bool close() noexcept {
        const auto handle = std::exchange(value, INVALID_HANDLE_VALUE);
        return handle == INVALID_HANDLE_VALUE || CloseHandle(handle) != 0;
    }
    ~HandleGuard() {
        static_cast<void>(close());
    }
};

struct SocketGuard {
    SOCKET value{INVALID_SOCKET};
    ~SocketGuard() {
        if (value != INVALID_SOCKET) {
            closesocket(value);
        }
    }
};

struct VirtualAllocationGuard {
    void* value{};
    ~VirtualAllocationGuard() {
        if (value != nullptr) {
            static_cast<void>(VirtualFree(value, 0U, MEM_RELEASE));
        }
    }
};

} // namespace

TEST_CASE("Windows disk counters observe an unbuffered file workload",
          "[telemetry][windows][integration][workload]") {
    core::SystemMonotonicClock clock;
    windows::WindowsTelemetryProvider provider{clock};
    telemetry::RawTelemetrySnapshot raw;
    REQUIRE(provider.sample({}, raw).status !=
            telemetry::ProviderSampleStatus::temporarily_failed);
    REQUIRE(raw.system.disk_write_bytes.has_value());
    const auto before = raw.system.disk_write_bytes.value.value;

    wchar_t directory[MAX_PATH]{};
    wchar_t path[MAX_PATH]{};
    REQUIRE(GetTempPathW(MAX_PATH, directory) != 0U);
    REQUIRE(GetTempFileNameW(directory, L"bbx", 0U, path) != 0U);
    HandleGuard file;
    file.value = CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE, 0U, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    REQUIRE(file.value != INVALID_HANDLE_VALUE);
    if (file.value == INVALID_HANDLE_VALUE) return;

    constexpr DWORD chunk_size = 1024U * 1024U;
    constexpr std::uint64_t workload_size = 32ULL * chunk_size;
    VirtualAllocationGuard buffer{VirtualAlloc(
        nullptr, chunk_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)};
    REQUIRE(buffer.value != nullptr);
    if (buffer.value == nullptr) return;
    std::memset(buffer.value, 0xA5, chunk_size);
    for (std::uint64_t written = 0U; written < workload_size; written += chunk_size) {
        DWORD completed = 0U;
        REQUIRE(WriteFile(file.value, buffer.value, chunk_size, &completed, nullptr) != 0);
        REQUIRE(completed == chunk_size);
    }
    REQUIRE(FlushFileBuffers(file.value) != 0);
    REQUIRE(provider.sample({}, raw).status !=
            telemetry::ProviderSampleStatus::temporarily_failed);
    REQUIRE(raw.system.disk_write_bytes.has_value());
    const auto observed = raw.system.disk_write_bytes.value.value - before;
    // Physical counters may include metadata and unrelated host I/O. The lower
    // bound proves the payload reached the selected layer; the generous upper
    // bound avoids treating concurrent activity as a source failure.
    CHECK(observed >= workload_size * 9U / 10U);
    CHECK(observed <= workload_size * 4U);
    REQUIRE(raw.system.disk_quality.read_latency.has_value());
    REQUIRE(raw.system.disk_quality.write_latency.has_value());
    REQUIRE(raw.system.disk_quality.service_time.has_value());
    REQUIRE(raw.system.disk_quality.queue_depth.has_value());
    REQUIRE(raw.system.disk_quality.worst_device_id.has_value());
    CHECK(raw.system.disk_quality.read_latency.value.value >= 0.0);
    CHECK(raw.system.disk_quality.write_latency.value.value >= 0.0);
    CHECK(raw.system.disk_quality.service_time.value.value >= 0.0);
    CHECK(raw.system.disk_quality.queue_depth.value >= 0.0);

    REQUIRE(file.close());
    CHECK(DeleteFileW(path) != 0);
}

TEST_CASE("Windows network policy excludes controlled loopback traffic",
          "[telemetry][windows][integration][workload]") {
    WSADATA data{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);

    SocketGuard listener{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    REQUIRE(listener.value != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0U;
    REQUIRE(bind(listener.value, reinterpret_cast<const sockaddr*>(&address),
                 sizeof(address)) == 0);
    REQUIRE(listen(listener.value, 1) == 0);
    int address_size = sizeof(address);
    REQUIRE(getsockname(listener.value, reinterpret_cast<sockaddr*>(&address),
                        &address_size) == 0);

    SocketGuard client{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    REQUIRE(client.value != INVALID_SOCKET);
    REQUIRE(connect(client.value, reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) == 0);
    SocketGuard server{accept(listener.value, nullptr, nullptr)};
    REQUIRE(server.value != INVALID_SOCKET);

    core::SystemMonotonicClock clock;
    windows::WindowsTelemetryProvider provider{clock};
    telemetry::RawTelemetrySnapshot raw;
    REQUIRE(provider.sample({}, raw).status !=
            telemetry::ProviderSampleStatus::temporarily_failed);
    REQUIRE(raw.system.network_receive_bytes.has_value());
    REQUIRE(raw.system.network_transmit_bytes.has_value());
    REQUIRE(raw.system.network_quality.connectivity.has_value());
    REQUIRE(raw.system.network_quality.active_interfaces.has_value());
    REQUIRE(raw.system.network_quality.interface_change_counter.has_value());
    REQUIRE(raw.system.network_quality.tcp_out_segments.has_value());
    REQUIRE(raw.system.network_quality.tcp_retransmitted_segments.has_value());
    REQUIRE(raw.system.network_quality.tcp_failed_connections.has_value());
    REQUIRE(raw.system.network_quality.tcp_established_resets.has_value());
    const auto provider_before = raw.system.network_receive_bytes.value.value +
                                 raw.system.network_transmit_bytes.value.value;

    constexpr std::size_t chunk_size = 64U * 1024U;
    constexpr std::uint64_t payload_size = 64ULL * 1024ULL * 1024ULL;
    std::vector<char> sent(chunk_size);
    std::vector<char> received(chunk_size);
    for (std::uint64_t transferred = 0U; transferred < payload_size;
         transferred += chunk_size) {
        std::size_t sent_total = 0U;
        while (sent_total < sent.size()) {
            const auto amount = send(
                client.value, sent.data() + sent_total,
                static_cast<int>(sent.size() - sent_total), 0);
            REQUIRE(amount > 0);
            sent_total += static_cast<std::size_t>(amount);
        }
        std::size_t received_total = 0U;
        while (received_total < received.size()) {
            const auto amount = recv(
                server.value, received.data() + received_total,
                static_cast<int>(received.size() - received_total), 0);
            REQUIRE(amount > 0);
            received_total += static_cast<std::size_t>(amount);
        }
    }

    REQUIRE(provider.sample({}, raw).status !=
            telemetry::ProviderSampleStatus::temporarily_failed);
    REQUIRE(raw.system.network_receive_bytes.has_value());
    REQUIRE(raw.system.network_transmit_bytes.has_value());
    const auto provider_after = raw.system.network_receive_bytes.value.value +
                                raw.system.network_transmit_bytes.value.value;
    const auto provider_delta = provider_after - provider_before;
    // GetIfTable2 software-loopback rows do not consistently account local
    // TCP across Windows builds. The completed payload is therefore the
    // control: it must not appear in the selected hardware aggregate.
    CHECK(provider_delta < payload_size / 2U);

    WSACleanup();
}

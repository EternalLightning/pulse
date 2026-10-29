#include "../index/index_protocol.h"
#include "../index/index_transport_security.h"
#include "../ipc/protocol.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>

using namespace pulse::index;
using pulse::ipc::MsgHeader;
using pulse::ipc::PayloadReader;
using pulse::ipc::PayloadWriter;
using pulse::ipc::PipeRead;
using pulse::ipc::PipeWrite;

namespace {

struct Sample {
    double ms = 0;
    uint32_t total = 0;
    uint32_t returned = 0;
    uint32_t error = 0;
    bool incomplete = false;
};

bool ReadFrame(HANDLE pipe, MsgHeader& header, std::vector<uint8_t>& payload) {
    if (!PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header)) ||
        header.magic != kIndexMagic || header.payload_size > kIndexMaxPayload)
        return false;
    payload.resize(header.payload_size);
    return payload.empty() || PipeRead(pipe, payload.data(), header.payload_size);
}

HANDLE Connect() {
    HANDLE pipe = CreateFileW(kPipeName, transport::kClientPipeAccess, 0, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) {
        WaitNamedPipeW(kPipeName, 2000);
        pipe = CreateFileW(kPipeName, transport::kClientPipeAccess, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    return pipe;
}

bool QueryStatus(uint32_t& count, std::wstring& status) {
    HANDLE pipe = Connect();
    if (pipe == INVALID_HANDLE_VALUE) return false;
    const MsgHeader request = MakeIndexHdr(REQ_IDX_STATUS, 1, 0);
    bool ok = PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&request), sizeof(request));
    MsgHeader response{};
    std::vector<uint8_t> payload;
    while (ok && ReadFrame(pipe, response, payload)) {
        if (response.type != RSP_IDX_STATUS || response.request_id != 1) continue;
        PayloadReader reader(payload.data(), payload.size());
        uint32_t ready = 0;
        ok = reader.GetU32(ready) && reader.GetU32(count) && reader.GetString(status) && ready != 0;
        break;
    }
    CloseHandle(pipe);
    return ok;
}

bool RunSearch(const std::wstring& needle, uint32_t request_id, Sample& sample) {
    HANDLE pipe = Connect();
    if (pipe == INVALID_HANDLE_VALUE) return false;
    PayloadWriter writer;
    writer.PutU32(1u); // ranked
    writer.PutU32(static_cast<uint32_t>(ResultSort::Index));
    writer.PutU32(48);
    writer.PutU32(0);
    writer.PutString(needle);
    writer.PutString(L"");
    const MsgHeader request = MakeIndexHdr(REQ_IDX_SEARCH, request_id,
                                            static_cast<uint32_t>(writer.data().size()));
    const auto start = std::chrono::steady_clock::now();
    bool ok = PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&request), sizeof(request)) &&
              PipeWrite(pipe, writer.data().data(), static_cast<DWORD>(writer.data().size()));
    MsgHeader response{};
    std::vector<uint8_t> payload;
    while (ok && ReadFrame(pipe, response, payload)) {
        if (response.type != RSP_IDX_SEARCH || response.request_id != request_id) continue;
        PayloadReader reader(payload.data(), payload.size());
        ok = reader.GetU32(sample.total) && reader.GetU32(sample.returned) && sample.returned <= 48;
        for (uint32_t i = 0; ok && i < sample.returned; ++i) {
            std::wstring path, name;
            uint32_t value = 0;
            ok = reader.GetString(path) && reader.GetString(name);
            for (int field = 0; ok && field < 5; ++field) ok = reader.GetU32(value);
        }
        uint64_t revision = 0;
        uint32_t incomplete = 0;
        ok = ok && reader.GetU64(revision) && reader.GetU32(sample.error) &&
            reader.GetU32(incomplete) && !reader.remaining();
        sample.incomplete = incomplete != 0;
        break;
    }
    sample.ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    CloseHandle(pipe);
    return ok;
}

double Percentile(std::vector<double> values, double percentile) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    // Nearest rank: a five-sample p95 includes the slowest observation.
    const size_t rank = static_cast<size_t>(std::ceil(percentile * static_cast<double>(values.size())));
    const size_t index = (std::min)(values.size() - 1, rank ? rank - 1 : size_t{0});
    return values[index];
}

double Median(std::vector<double> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--statistics-only") {
        const bool ok = Percentile({1, 2, 3, 4, 100}, .95) == 100 &&
            Percentile({7}, .95) == 7 && Percentile({}, .95) == 0 &&
            Median({4, 1, 3, 2}) == 2.5 && Median({5, 1, 3}) == 3;
        std::wcout << (ok ? L"[PASS] " : L"[FAIL] ") << L"nearest-rank p95 and median boundary cases\n";
        return ok ? 0 : 1;
    }
    const bool enforce = argc > 1 && std::wstring_view(argv[1]) == L"--enforce";
    const bool diagnose = argc > 1 && std::wstring_view(argv[1]) == L"--diagnose";
    uint32_t indexed = 0;
    std::wstring status;
    if (!QueryStatus(indexed, status)) {
        std::wcerr << L"[FAIL] PulseIndex is unavailable or not ready\n";
        return 2;
    }
    // Avoid writing the localized status through a narrow/legacy console code
    // page; the numeric result is enough for automated performance checks.
    (void)status;
    std::wcout << L"index items: " << indexed << L"\n\n";
    std::wcout << (diagnose ? L"Single observation per query; no percentile estimate.\n" :
        L"5 observations per query; nearest-rank p95; not a long-run latency estimate.\n");
    std::wcout << L"query       samples   min(ms) median(ms)   max(ms) "
                  L"p95(ms)   matches   rows   quality   time-budget\n";

    struct Case { const wchar_t* text; double budget; };
    const Case cases[] = {{L"a", 150.0}, {L"ab", 50.0}, {L"abc", 50.0},
                          {L"report", 100.0}, {L"pdf", 100.0},
                          {L"pulse-no-match-9f64b73ad180", 100.0}};
    bool passed = true;
    uint32_t request_id = 100;
    for (const Case& test : cases) {
        std::vector<double> times;
        uint32_t matches = 0;
        Sample last;
        bool quality = true;
        for (int i = 0; i < (diagnose ? 1 : 5); ++i) {
            Sample sample;
            if (!RunSearch(test.text, request_id++, sample)) {
                std::wcerr << L"[FAIL] query transport failed: " << test.text << L"\n";
                return 2;
            }
            times.push_back(sample.ms);
            std::wcout << L"[SAMPLE] query=" << test.text << L" number=" << i + 1
                << L" ms=" << std::fixed << std::setprecision(3) << sample.ms
                << L" matches=" << sample.total << L" rows=" << sample.returned
                << L" error=" << sample.error << L" incomplete=" << sample.incomplete << L'\n';
            matches = sample.total;
            quality = quality && !sample.error && !sample.incomplete;
            last = sample;
        }
        const double p50 = Median(times);
        const double p95 = Percentile(times, 0.95);
        const bool within = p95 <= test.budget;
        passed = passed && within && quality;
        std::wcout << std::left << std::setw(10) << test.text << std::right
                   << std::setw(8) << times.size()
                   << std::setw(10) << std::fixed << std::setprecision(2) << *std::min_element(times.begin(), times.end())
                   << std::setw(11) << p50 << std::setw(10) << *std::max_element(times.begin(), times.end());
        if (diagnose) std::wcout << std::setw(9) << L"single";
        else std::wcout << std::setw(9) << p95;
        std::wcout << std::setw(10) << matches << std::setw(7) << last.returned
                   << std::setw(10) << (quality ? L"PASS" : L"FAIL")
                   << L"   " << (within ? L"PASS" : L"FAIL") << L" <= "
                   << test.budget << L" ms\n";
    }
    if (!enforce) std::wcout << L"Diagnostic exit only; use --enforce for combined quality and time-budget acceptance.\n";
    return enforce && !passed ? 1 : 0;
}

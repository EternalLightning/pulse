#include "ops/archive.h"
#include "ops/archive_retry.h"
#include <windows.h>
#include <winioctl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <algorithm>
#include <thread>
#include <chrono>
#include <iterator>
#include <array>

namespace fs = std::filesystem;
using namespace pulse::ops;
namespace {
int failures = 0;
void Check(bool value, const char* name) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!value) ++failures;
}
std::string DiagnosticUtf8(const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
bool Pack(const fs::path& archive, const fs::path& source, const std::wstring& options) {
    fs::path exe;
#ifdef PULSE_ARCHIVE_RUNTIME_DIR
    exe = fs::path(PULSE_ARCHIVE_RUNTIME_DIR) / L"7z.exe";
#else
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, 32768);
    exe = fs::path(module).parent_path() / L"7zip/7z.exe";
#endif
    std::wstring command = L"\"" + exe.wstring() + L"\" a -y -bd " + options + L" \"" + archive.wstring() + L"\" .\\*";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, source.c_str(), &startup, &process)) {
        std::cerr << "[DIAG] archive fixture process creation failed: " << GetLastError() << '\n';
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 2; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess); CloseHandle(process.hThread);
    if (code != 0) std::cerr << "[DIAG] archive fixture process exit: " << code << '\n';
    return code == 0;
}
void ProgressSamplingTests() {
    ArchiveProgressTracker tracker;
    tracker.Begin(ArchivePhase::Extracting, 10000000, 2, 0);
    tracker.Feed("0%\r", 0);
    tracker.Feed("  2", 300);
    tracker.Feed("5% 1 - file.txt\r", 1000);
    Check(tracker.Status().percent == 25 && tracker.Status().processed_bytes == 2500000 &&
        tracker.Status().bytes_per_second > 0 && tracker.Status().eta_seconds > 0,
        "split 7-Zip progress yields estimated unpacked bytes, speed and ETA");
    tracker.Feed("  10%\r999%\rfilename 100%\r", 1500);
    Check(tracker.Status().percent == 25, "backwards and malformed percentages do not advance progress");
    tracker.Feed("\b 60%", 2000);
    Check(tracker.Status().percent == 60 && tracker.Status().processed_bytes == 6000000,
        "carriage return and backspace progress updates are parsed");
    tracker.Begin(ArchivePhase::Extracting, 10000000, 2, 2000);
    Check(tracker.Status().processed_bytes == 0 && tracker.Status().bytes_per_second == 0 &&
        tracker.Status().eta_seconds == 0, "a new extraction starts a fresh rate estimate");
    tracker.Advance(3000000, 1, L"file.txt", 3000);
    Check(tracker.Status().percent == 30 && tracker.Status().eta_seconds > 0 &&
        tracker.Status().current_item == L"file.txt", "byte progress tracks rate and item name");
    tracker.Begin(ArchivePhase::Extracting, 0, 2, 3000);
    tracker.Advance(0, 1, L"empty", 4000);
    Check(tracker.Status().percent == 50 && tracker.Status().bytes_per_second == 0,
        "zero-byte archives report item progress without invented throughput");
    tracker.Begin(ArchivePhase::Extracting, UINT64_MAX, 1, 0);
    tracker.Feed("100%", 1000);
    Check(tracker.Status().processed_bytes == UINT64_MAX, "large archive progress does not overflow");
    tracker.Begin(ArchivePhase::Completed, 0, 0, 0);
    tracker.Advance(0, 0, {}, 0);
    Check(tracker.Status().percent == 100, "empty or fully skipped archives finish at 100 percent");
    constexpr uint64_t mib = 1024ull * 1024;
    tracker.Begin(ArchivePhase::Extracting, 1000 * mib, 1, 0);
    tracker.Feed("35%\r", 1000);
    Check(tracker.Status().bytes_per_second == 0 && tracker.Status().eta_seconds == 0,
        "first percentage establishes a baseline instead of inventing a startup speed spike");
    tracker.Feed("37%\r", 2000);
    bool stable = true;
    for (ULONGLONG tick = 2020; tick <= 7000; tick += 20) {
        if (tick % 1000 == 0) {
            const auto percent = 37 + (tick - 2000) / 1000 * 2;
            tracker.Feed(std::to_string(percent) + "%\r", tick);
        } else {
            tracker.Feed(tick % 100 == 0 ? "1%\rfilename 100%\r" : "", tick);
        }
        stable = stable && std::abs(tracker.Status().bytes_per_second - 20 * mib) < 1.0;
    }
    Check(stable && tracker.Status().eta_seconds > 0,
        "sparse percentages with empty, backwards and diagnostic polls retain steady throughput");
    tracker.Feed("49%\r", 9000);
    Check(tracker.Status().bytes_per_second > 0 && tracker.Status().bytes_per_second < 20 * mib,
        "measured slowdown still lowers the rate when advancing percentages arrive more slowly");
    tracker.Begin(ArchivePhase::Extracting, 1000 * mib, 1, 0);
    tracker.Feed("0%\r", 0);
    tracker.Feed("1%\r", 6000);
    const auto slow_speed = tracker.Status().bytes_per_second;
    bool slow_stable = slow_speed > 0;
    for (ULONGLONG tick = 6020; tick <= 18000; tick += 20) {
        if (tick % 6000 == 0) tracker.Feed(std::to_string(tick / 6000) + "%\r", tick);
        else tracker.Feed("1%\r", tick);
        slow_stable = slow_stable && std::abs(tracker.Status().bytes_per_second - slow_speed) < 1.0;
    }
    Check(slow_stable, "slow one-percent updates do not produce recurring spikes or decay between reports");
    tracker.Feed("3%\r", 30000);
    Check(tracker.Status().bytes_per_second == 0 && tracker.Status().eta_seconds == 0,
        "a prolonged extraction stall clears stale speed and ETA");
    tracker.Feed("4%\r", 31000);
    Check(tracker.Status().bytes_per_second > 0 && tracker.Status().bytes_per_second < slow_speed,
        "resumed progress includes the stalled interval without a recovery spike");
}
void ProgressTests() {
    ProgressSamplingTests();
    ArchiveProgressTracker tracker;
#ifdef PULSE_ARCHIVE_PROCESS_TEST
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const auto fixture = (fs::path(module).parent_path() / L"pulse_archive_process_fixture.exe").wstring();
    for (const auto mode : {L"progress", L"progress-error"}) {
        tracker.Begin(ArchivePhase::Extracting, 10000000, 1, GetTickCount64());
        const auto run = RunArchiveProcessForTest(fixture, {mode}, 64, nullptr,
            [&](std::string_view chunk) { tracker.Feed(chunk, GetTickCount64()); });
        Check(tracker.Status().percent == 100 && (std::wstring_view(mode) == L"progress"
            ? run.outcome == ArchiveOutcome::Success && run.output.empty()
            : run.outcome == ArchiveOutcome::Failed && run.exit_code == 2u && run.output == L"fixture-error"),
            "progress pipe drains final updates without consuming diagnostic budget or hiding errors");
    }
    std::atomic_bool pipe_cancel = false;
    const auto interrupted = RunArchiveProcessForTest(fixture, {L"progress-cancel"}, 64, &pipe_cancel,
        [&](std::string_view chunk) { if (!chunk.empty()) pipe_cancel = true; });
    Check(interrupted.outcome == ArchiveOutcome::Cancelled,
        "in-flight progress cancellation terminates the archive process");
#endif

    const auto root = fs::absolute(fs::path(L"bench_data") /
        (L"archive-progress-" + std::to_wstring(GetCurrentProcessId())));
    fs::create_directories(root / L"source/sub/empty");
    std::ofstream(root / L"source/top.txt") << "preserve me";
    {
        std::ofstream file(root / L"source/sub/data.bin", std::ios::binary);
        const std::string block(1024 * 1024, 'x');
        for (int i = 0; i < 16; ++i) file.write(block.data(), block.size());
    }
    for (const auto& extension : {L"zip", L"7z"}) {
        const auto archive = root / (std::wstring(L"test.") + extension);
        Check(Pack(archive, root / L"source", L""), "pack real archive progress fixture");
        std::vector<ArchiveProgress> updates;
        const auto capture = [&](const ArchiveProgress& p) { updates.push_back(p); };
        auto result = ExtractArchive(archive.wstring(), (root / (std::wstring(L"selected-") + extension)).wstring(),
            {L"sub/"}, L"", nullptr, capture);
        if (result.outcome != ArchiveOutcome::Success)
            std::cout << "[DIAG] selected extraction: exit=" << (result.exit_code ? std::to_string(*result.exit_code) : "none")
                << " error=" << DiagnosticUtf8(result.error) << '\n';
        Check(result.outcome == ArchiveOutcome::Success && !updates.empty() &&
            updates.front().phase == ArchivePhase::Preparing && updates.back().phase == ArchivePhase::Completed &&
            updates.back().total_bytes == 16 * 1024 * 1024 && updates.back().processed_bytes == updates.back().total_bytes &&
            fs::exists(root / (std::wstring(L"selected-") + extension) / L"sub/data.bin") &&
            !fs::exists(root / (std::wstring(L"selected-") + extension) / L"top.txt"),
            "real selected ZIP/7z extraction reports selected totals and completion after direct extraction");

        updates.clear();
        std::atomic_bool cancel = false;
        result = ExtractArchive(archive.wstring(), (root / (std::wstring(L"cancel-") + extension)).wstring(), {}, L"", &cancel,
            [&](const ArchiveProgress& p) { updates.push_back(p); if (p.phase == ArchivePhase::Extracting) cancel = true; });
        Check(result.outcome == ArchiveOutcome::Cancelled && std::none_of(updates.begin(), updates.end(), [](const auto& p) {
            return p.phase == ArchivePhase::Completed;
        }), "cancelled extraction never reports completed progress");
        updates.clear();
        result = ExtractArchive(archive.wstring(), (root / (std::wstring(L"empty-") + extension)).wstring(), {L"sub/empty/"}, L"", nullptr, capture);
        Check(result.outcome == ArchiveOutcome::Success && updates.back().total_bytes == 0 &&
            updates.back().completed_items == updates.back().total_items && updates.back().percent == 100,
            "empty-directory extraction completes using item counts");
    }
    if (!failures) { std::error_code error; fs::remove_all(root, error); }
}
void Word(std::ofstream& stream, uint32_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) stream.put(static_cast<char>(value >> (i * 8)));
}
void UnsafeZip(const fs::path& file, const std::string& name, uint32_t attributes = 0) {
    std::ofstream stream(file, std::ios::binary);
    Word(stream, 0x04034b50, 4); Word(stream, 20, 2); Word(stream, 0, 2); Word(stream, 0, 2);
    Word(stream, 0, 4); Word(stream, 0, 4); Word(stream, 0, 4); Word(stream, 0, 4);
    Word(stream, static_cast<uint32_t>(name.size()), 2); Word(stream, 0, 2); stream << name;
    auto offset = static_cast<uint32_t>(stream.tellp());
    Word(stream, 0x02014b50, 4); Word(stream, 0x0314, 2); Word(stream, 20, 2);
    Word(stream, 0, 2); Word(stream, 0, 2); Word(stream, 0, 4); Word(stream, 0, 4);
    Word(stream, 0, 4); Word(stream, 0, 4); Word(stream, static_cast<uint32_t>(name.size()), 2);
    Word(stream, 0, 2); Word(stream, 0, 2); Word(stream, 0, 2); Word(stream, 0, 2);
    Word(stream, attributes, 4); Word(stream, 0, 4); stream << name;
    auto size = static_cast<uint32_t>(stream.tellp()) - offset;
    Word(stream, 0x06054b50, 4); Word(stream, 0, 2); Word(stream, 0, 2); Word(stream, 1, 2);
    Word(stream, 1, 2); Word(stream, size, 4); Word(stream, offset, 4); Word(stream, 0, 2);
}
bool Junction(const fs::path& link, const fs::path& target, DWORD sharing = 0) {
    fs::create_directories(link);
    std::wstring substitute = L"\\??\\" + target.wstring();
    struct MountPoint {
        DWORD tag; WORD length; WORD reserved;
        WORD substitute_offset; WORD substitute_length;
        WORD print_offset; WORD print_length;
        wchar_t paths[2048];
    } data{};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data.print_length = static_cast<WORD>(target.wstring().size() * sizeof(wchar_t));
    data.length = static_cast<WORD>(8 + data.print_offset + data.print_length + sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), data.paths);
    auto display = target.wstring();
    std::copy(display.begin(), display.end(), data.paths + substitute.size() + 1);
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, sharing, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    bool result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(data.length + 8), nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    return result;
}
bool ClearJunction(const fs::path& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    struct Header { DWORD tag; WORD length; WORD reserved; } data{IO_REPARSE_TAG_MOUNT_POINT, 0, 0};
    DWORD returned = 0;
    const bool cleared = DeviceIoControl(handle, FSCTL_DELETE_REPARSE_POINT, &data, sizeof(data),
        nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    return cleared;
}

int PasswordTests() {
    std::cout << "[DIAG] starting password tests\n";
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    const fs::path root = fs::path(module).parent_path().parent_path() / L"bench_data" / (L"archive-password-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::cout << "[DIAG] fixture root: " << DiagnosticUtf8(root.wstring()) << '\n';
    std::error_code fixture_error;
    fs::create_directories(root / L"source", fixture_error);
    if (fixture_error) {
        std::cerr << "[DIAG] fixture setup failed: " << fixture_error.value() << '\n';
        return 1;
    }
    struct RestoreTemp {
        std::wstring tmp, temp;
        RestoreTemp() {
            wchar_t value[32768]{};
            if (GetEnvironmentVariableW(L"TMP", value, 32768)) tmp = value;
            if (GetEnvironmentVariableW(L"TEMP", value, 32768)) temp = value;
        }
        ~RestoreTemp() {
            SetEnvironmentVariableW(L"TMP", tmp.empty() ? nullptr : tmp.c_str());
            SetEnvironmentVariableW(L"TEMP", temp.empty() ? nullptr : temp.c_str());
        }
    } restore_temp;
    const auto owned_temp = root / L"temp";
    fs::create_directory(owned_temp, fixture_error);
    if (fixture_error || !SetEnvironmentVariableW(L"TMP", owned_temp.c_str()) ||
        !SetEnvironmentVariableW(L"TEMP", owned_temp.c_str())) {
        std::cerr << "[DIAG] isolated temporary directory setup failed\n";
        return 1;
    }
    std::ofstream(root / L"source/password encrypted.txt") << "password encrypted";
    for (const auto name : {L"password-missing.7z", L"Password-missing.zip", L"encrypted-missing.7z"}) {
        const auto result = ListArchive((root / name).wstring());
        Check(result.outcome == ArchiveOutcome::Failed && !result.error.empty() && result.password_state == ArchivePasswordState::Unknown,
            "missing archive with password keywords does not request a password");
    }
    const auto corrupt = root / L"encrypted password broken.7z";
    std::ofstream(corrupt) << "not an archive";
    const auto broken = ListArchive(corrupt.wstring());
    Check(broken.outcome == ArchiveOutcome::Failed && !broken.error.empty() && broken.password_state == ArchivePasswordState::Unknown,
        "corrupt archive with password keywords stays a generic failure");
    for (const auto extension : {L"7z", L"zip"}) {
        const auto plain_archive = root / (std::wstring(L"无密码 archive.") + extension);
        Check(Pack(plain_archive, root / L"source", L""), "create unencrypted password fixture");
        const auto plain_listing = ListArchive(plain_archive.wstring(), L"arbitrary-password");
        const auto plain_destination = root / (std::wstring(L"plain-") + extension);
        const auto plain_extraction = ExtractArchive(plain_archive.wstring(), plain_destination.wstring(), {}, L"arbitrary-password");
        std::ifstream plain_file(plain_destination / L"password encrypted.txt", std::ios::binary);
        const std::string plain_content{std::istreambuf_iterator<char>(plain_file), std::istreambuf_iterator<char>()};
        Check(plain_listing.outcome == ArchiveOutcome::Success &&
            plain_listing.password_state == ArchivePasswordState::NotRequired &&
            plain_extraction.outcome == ArchiveOutcome::Success && plain_extraction.exit_code == 0u &&
            plain_extraction.error.empty() && plain_content == "password encrypted",
            "an unused password does not prevent exact extraction of unencrypted content");
        const auto archive = root / (std::wstring(L"加密 password archive.") + extension);
        Check(Pack(archive, root / L"source", std::wstring(L"-psecret ") +
            (std::wstring(extension) == L"7z" ? L"-mhe=on" : L"-mem=AES256")),
            "create password diagnostic fixture");
        const auto locked = ListArchive(archive.wstring());
        Check(std::wstring(extension) == L"7z"
            ? locked.outcome == ArchiveOutcome::Failed && locked.password_state == ArchivePasswordState::Unknown && locked.exit_code == 2u
            : locked.outcome == ArchiveOutcome::Success && locked.password_state == ArchivePasswordState::Required,
            "hidden encrypted headers remain unknown; successful metadata provides encryption evidence");
        const auto wrong = ListArchive(archive.wstring(), L"wrong");
        if (std::wstring(extension) == L"7z")
            Check(wrong.outcome == ArchiveOutcome::Failed && !wrong.error.empty() && wrong.password_state == ArchivePasswordState::Unknown && wrong.exit_code == 2u,
                "encrypted-header failure cannot classify a supplied password from human diagnostics");
        else
            Check(wrong.outcome == ArchiveOutcome::Success && wrong.error.empty() && wrong.password_state == ArchivePasswordState::Required,
                "zip metadata listing does not verify a supplied password");
        const auto opened = ListArchive(archive.wstring(), L"secret");
        Check(opened.outcome == ArchiveOutcome::Success && opened.error.empty() && opened.password_state == ArchivePasswordState::Required && opened.entries.size() == 1,
            "correct password opens archive with keyword member names");
        const auto extracted_wrong = ExtractArchive(archive.wstring(), (root / (std::wstring(L"wrong-") + extension)).wstring(), {}, L"wrong");
        Check(extracted_wrong.outcome == ArchiveOutcome::Failed && extracted_wrong.exit_code == 2u && !extracted_wrong.error.empty() &&
            extracted_wrong.password_state == (std::wstring(extension) == L"7z" ? ArchivePasswordState::Unknown : ArchivePasswordState::Required),
            "typed extraction preserves exit failure without labeling its cause as a wrong password");
        const auto extracted = ExtractArchive(archive.wstring(), (root / (std::wstring(L"correct-") + extension)).wstring(), {}, L"secret");
        if (extracted.outcome != ArchiveOutcome::Success || !extracted.error.empty())
            std::cerr << "[DIAG] extraction outcome=" << static_cast<int>(extracted.outcome)
                << " exit=" << (extracted.exit_code ? std::to_string(*extracted.exit_code) : "none") << " error=" << DiagnosticUtf8(extracted.error) << '\n';
        Check(extracted.outcome == ArchiveOutcome::Success && extracted.exit_code == 0u && extracted.error.empty() &&
            fs::exists(root / (std::wstring(L"correct-") + extension) / L"password encrypted.txt"),
            "typed extraction reports success only after the files are committed");
        std::atomic_bool cancel{true};
        const auto cancelled = ListArchive(archive.wstring(), L"", &cancel);
        Check(cancelled.outcome == ArchiveOutcome::Cancelled && !cancelled.exit_code && !cancelled.error.empty() && cancelled.password_state == ArchivePasswordState::Unknown,
            "cancelled listing does not request a password");
    }
    if (failures) std::wcout << L"Fixtures retained at " << root.wstring() << L'\n';
    else { std::error_code error; fs::remove_all(root, error); }
    return failures ? 1 : 0;
}
}
#ifdef PULSE_ARCHIVE_PROCESS_TEST
// Audit reproducer: PASS means the documented retry hazard was reproduced,
// not that extraction integrity is correct. Kept out of the normal suite.
void RetryIntegrityAudit() {
    const auto root = fs::absolute(fs::path(L"bench_data") /
        (L"archive-retry-audit-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    fs::create_directories(root / L"source");
    const std::string payload(128 * 1024 * 1024 + 7, 'p');
    std::ofstream(root / L"source/member.bin", std::ios::binary) << payload;
    std::ofstream(root / L"source/existing.txt") << "archive existing entry";
    const auto archive = root / L"plain.zip";
    if (!Pack(archive, root / L"source", L"-mx=0")) {
        Check(false, "audit fixture creation");
        return;
    }
    const auto read = [](const fs::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    const auto damaged = root / L"damaged.zip";
    fs::copy_file(archive, damaged);
    {
        const auto bytes = read(damaged);
        const auto offset = bytes.find(std::string(64, 'p'));
        if (offset == std::string::npos) {
            Check(false, "audit stored payload location");
            return;
        }
        std::fstream stream(damaged, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(static_cast<std::streamoff>(offset));
        stream.put('q');
    }
    fs::create_directories(root / L"failure");
    std::ofstream(root / L"failure/unrelated.txt") << "user-data";
    std::ofstream(root / L"failure/existing.txt") << "existing user content";
    const auto first = ExtractArchive(damaged.wstring(), (root / L"failure").wstring(), {}, L"");
    const auto corrupt = read(root / L"failure/member.bin");
    Check(first.outcome == ArchiveOutcome::Failed && first.exit_code == 2u &&
        corrupt.size() == payload.size() && corrupt != payload,
        "AUDIT: CRC failure leaves a full-size corrupt final-name member");
    bool extraction_started = false;
    fs::copy_file(archive, damaged, fs::copy_options::overwrite_existing);
    const auto retry = ExtractArchiveForTest(damaged.wstring(), (root / L"failure").wstring(),
        [&](ArchiveExtractionPoint point, const std::wstring&, const std::wstring&) {
            if (point == ArchiveExtractionPoint::BeforeExtract) extraction_started = true;
        });
    {
        if (retry.outcome != ArchiveOutcome::Success) std::cout << "[DIAG] retry: " << DiagnosticUtf8(retry.error) << '\n';
        Check(retry.outcome == ArchiveOutcome::Success && extraction_started &&
            read(root / L"failure/member.bin") == payload && read(root / L"failure/unrelated.txt") == "user-data" &&
            read(root / L"failure/existing.txt") == "existing user content" &&
            !fs::exists(root / L"failure/.pulse-extract-state"),
            "retry repairs CRC residual, preserves unrelated data and clears journal");
    }
    std::atomic_bool cancel = false, finished = false, partial_seen = false;
    std::thread observer([&] {
        while (!finished.load()) {
            std::error_code ec;
            const auto size = fs::file_size(root / L"cancel/member.bin", ec);
            // 7-Zip can preallocate the final size before writing its payload.
            if (!ec && size > 0) {
                partial_seen = true;
                cancel = true;
                return;
            }
            Sleep(0);
        }
    });
    const auto interrupted = ExtractArchive(archive.wstring(), (root / L"cancel").wstring(), {}, L"", &cancel);
    finished = true;
    observer.join();
    const auto partial = read(root / L"cancel/member.bin");
    const auto resumed = ExtractArchive(archive.wstring(), (root / L"cancel").wstring(), {}, L"");
    std::cout << "[INFO] cancellation partial_seen=" << partial_seen.load()
        << " cancelled=" << (interrupted.outcome == ArchiveOutcome::Cancelled)
        << " retained_bytes=" << partial.size() << " expected_bytes=" << payload.size()
        << " retry_success=" << (resumed.outcome == ArchiveOutcome::Success) << '\n';
    if (partial_seen && interrupted.outcome == ArchiveOutcome::Cancelled && !partial.empty() && partial != payload) {
        {
            if (resumed.outcome != ArchiveOutcome::Success) std::cout << "[DIAG] cancel retry: " << DiagnosticUtf8(resumed.error) << '\n';
            Check(resumed.outcome == ArchiveOutcome::Success && read(root / L"cancel/member.bin") == payload,
                "retry repairs cancelled residual completely");
        }
    } else {
        std::cout << "[LIMIT] cancellation did not retain a partial member in this timing window\n";
    }
    std::cout << "[INFO] retained isolated audit fixture: " << DiagnosticUtf8(root.wstring()) << '\n';
}

void JournalErrorTests(const wchar_t* archive) {
    const auto root = fs::absolute(fs::path(L"bench_data") /
        (L"archive-journal-errors-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    fs::create_directories(root / L"target");
    const auto target = root / L"target";
    const auto marker = target / ArchiveRetry::kName;
    const auto source = (root / L"source.zip").wstring();
    std::wstring error;
    {
        ArchiveRetry journal;
        Check(journal.Open(target, source, error), "writable extraction directory accepts recovery journal");
        ArchiveRetry concurrent;
        Check(!concurrent.Open(target, source, error) && error.find(L"0x80070020") != std::wstring::npos &&
            error.find(L"占用") != std::wstring::npos && error.find(marker.wstring()) != std::wstring::npos,
            "busy journal reports sharing violation and its path");
    }
    Check(!fs::exists(marker), "empty successful journal is removed");
    std::ofstream(marker) << "user-owned marker";
    Check(SetFileAttributesW(marker.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY) != FALSE,
        "prepare read-only recovery record");
    {
        ArchiveRetry denied;
        Check(!denied.Open(target, source, error) && error.find(L"0x80070005") != std::wstring::npos &&
            error.find(L"管理员") != std::wstring::npos && error.find(marker.wstring()) != std::wstring::npos,
            "journal access denial reports permissions guidance, system error and path");
    }
    Check(SetFileAttributesW(marker.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "restore fixture attributes");
    {
        ArchiveRetry invalid;
        Check(!invalid.Open(target, source, error) && error.find(L"无法安全读取") != std::wstring::npos &&
            error.find(L"管理员") == std::wstring::npos,
            "invalid recovery data is not misreported as a permissions failure");
    }
    {
        std::ifstream stream(marker);
        const std::string contents(std::istreambuf_iterator<char>(stream), {});
        Check(contents == "user-owned marker", "refused recovery record remains unchanged");
    }
    fs::create_directories(root / L"source");
    std::ofstream(root / L"source/member.txt") << "fixture-data";
    if (Pack(root / L"source.zip", root / L"source", L"")) {
        const auto result = ExtractArchive(source, (root / L"extracted").wstring(), {}, L"", nullptr);
        if (result.outcome != ArchiveOutcome::Success) std::cout << "[DIAG] " << DiagnosticUtf8(result.error) << '\n';
        Check(result.outcome == ArchiveOutcome::Success && fs::exists(root / L"extracted/member.txt") &&
            !fs::exists(root / L"extracted" / ArchiveRetry::kName),
            "real archive extracts into writable directory and clears journal");
    } else Check(false, "pack isolated journal extraction fixture");
    if (archive) {
        const auto listing = ListArchive(archive);
        const auto output = root / L"user-archive";
        const auto result = ExtractArchive(archive, output.wstring(), {}, L"", nullptr);
        if (result.outcome != ArchiveOutcome::Success) std::cout << "[DIAG] " << DiagnosticUtf8(result.error) << '\n';
        bool matches = listing.outcome == ArchiveOutcome::Success && result.outcome == ArchiveOutcome::Success;
        for (const auto& entry : listing.entries) {
            const auto path = output / entry.path;
            matches = matches && (entry.is_dir ? fs::is_directory(path) :
                fs::is_regular_file(path) && fs::file_size(path) == entry.size);
        }
        Check(matches && !fs::exists(output / ArchiveRetry::kName),
            "user archive extracts with matching entry types and sizes into isolated directory");
    }
    if (!failures) {
        std::error_code cleanup;
        fs::remove_all(root, cleanup);
        Check(!cleanup, "isolated journal diagnostics fixtures are cleaned");
    }
}

void RetryOwnershipTests() {
    const auto root = fs::absolute(fs::path(L"bench_data") /
        (L"archive-retry-ownership-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    fs::create_directories(root);
    const std::vector<ArchiveEntry> members{{L"member.txt", false, 4}};
    const auto read = [](const fs::path& file) {
        std::ifstream stream(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    auto prepare = [&](const wchar_t* name, bool reported = true) {
        const auto target = root / name;
        fs::create_directories(target);
        ArchiveRetry journal;
        std::wstring error;
        bool ok = journal.Open(target, (root / L"source.zip").wstring(), error) && journal.Begin(members, error);
        std::ofstream(target / L"member.txt") << "part";
        if (std::wstring_view(name) == L"readonly") SetFileAttributesW((target / L"member.txt").c_str(), FILE_ATTRIBUTE_READONLY);
        if (reported) journal.Feed("- member.txt\n");
        ok = ok && journal.Finish(false, error);
        Check(ok, "persist isolated interrupted extraction record");
        return target;
    };
    for (const auto name : {L"replacement", L"modified", L"unknown", L"normal", L"readonly"}) {
        const auto target = prepare(name, std::wstring_view(name) != L"unknown");
        if (std::wstring_view(name) == L"replacement") {
            fs::rename(target / L"member.txt", target / L"retained.txt");
            std::ofstream(target / L"member.txt") << "user replacement";
        } else if (std::wstring_view(name) == L"modified") std::ofstream(target / L"member.txt") << "user edited content";
        const auto before = read(target / L"member.txt");
        ArchiveRetry journal;
        DestinationGuard guard;
        std::wstring error;
        const bool recovered = journal.Open(target, (root / L"source.zip").wstring(), error) && journal.Recover(members, guard, error);
        if (std::wstring_view(name) == L"normal" || std::wstring_view(name) == L"readonly")
            Check(recovered && !fs::exists(target / L"member.txt"), "matching residual including readonly output is admitted for replacement");
        else Check(!recovered && read(target / L"member.txt") == before, "replaced modified or unreported files cannot be overwritten");
    }
    const auto target = prepare(L"concurrent");
    ArchiveRetry owner, other;
    std::wstring error;
    Check(owner.Open(target, (root / L"source.zip").wstring(), error) &&
        !other.Open(target, (root / L"source.zip").wstring(), error), "concurrent retry cannot acquire the same journal");
    fs::create_directories(root / L"foreign");
    std::ofstream(root / L"foreign/.pulse-extract-state") << "";
    ArchiveRetry foreign;
    Check(!foreign.Open(root / L"foreign", (root / L"source.zip").wstring(), error), "preexisting empty user marker is not adopted");
    const auto torn = prepare(L"torn");
    { std::ofstream output(torn / ArchiveRetry::kName, std::ios::binary | std::ios::app); output << "{\"incomplete"; }
    ArchiveRetry restored;
    DestinationGuard guard;
    Check(restored.Open(torn, (root / L"source.zip").wstring(), error) && restored.Recover(members, guard, error),
        "torn trailing record retains the previous complete retry snapshot");
    const auto different = prepare(L"different-source");
    ArchiveRetry wrong_source;
    Check(!wrong_source.Open(different, (root / L"other.zip").wstring(), error) && read(different / L"member.txt") == "part",
        "another archive cannot consume the pending recovery record");
}

void DirectExtractionTests() {
    const fs::path root = fs::absolute(fs::path(L"bench_data") /
        (L"archive-direct-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    fs::create_directories(root / L"source/nested/empty");
    fs::create_directories(root / L"temporary");
    fs::create_directories(root / L"outside/nested");
    const std::string payload(32 * 1024 * 1024 + 7, 'p');
    std::ofstream(root / L"source/nested/file.txt", std::ios::binary) << payload;
    std::ofstream(root / L"source/top.txt") << "top-data";
    std::ofstream(root / L"outside/nested/file.txt") << "outside-original";
    struct RestoreTemp {
        std::wstring value;
        RestoreTemp(const fs::path& path) {
            wchar_t existing[32768]{};
            if (GetEnvironmentVariableW(L"TMP", existing, ARRAYSIZE(existing))) value = existing;
            SetEnvironmentVariableW(L"TMP", path.c_str());
        }
        ~RestoreTemp() { SetEnvironmentVariableW(L"TMP", value.empty() ? nullptr : value.c_str()); }
    } restore_temp(root / L"temporary");
    const auto read = [](const fs::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    const auto outside_unchanged = [&] { return read(root / L"outside/nested/file.txt") == "outside-original"; };
    const auto archive = root / L"plain.zip";
    Check(Pack(archive, root / L"source", L"-mx=0"), "create direct-extraction ZIP fixture");
    bool direct_seen = false, no_list = true;
    std::vector<ArchiveProgress> updates;
    auto result = ExtractArchiveForTest(archive.wstring(), (root / L"direct").wstring(),
        [&](ArchiveExtractionPoint point, const std::wstring& destination, const std::wstring&) {
            if (point != ArchiveExtractionPoint::ProcessProgress) return;
            direct_seen |= fs::exists(fs::path(destination) / L"nested/file.txt");
            no_list &= fs::is_empty(root / L"temporary");
        }, nullptr, [&](const auto& p) { updates.push_back(p); });
    if (result.outcome != ArchiveOutcome::Success)
        std::cout << "[DIAG] direct error=" << DiagnosticUtf8(result.error) << '\n';
    Check(result.outcome == ArchiveOutcome::Success && direct_seen && no_list &&
        read(root / L"direct/nested/file.txt") == payload && fs::is_directory(root / L"direct/nested/empty") &&
        fs::is_empty(root / L"temporary"), "7-Zip writes directly to destination without temporary payload or a list for extract-all");
    Check(!updates.empty() && updates.back().phase == ArchivePhase::Completed && updates.back().percent == 100 &&
        updates.back().processed_bytes == payload.size() + 8, "direct extraction reports completion after successful extractor exit");

    fs::create_directories(root / L"replaced");
    bool replaced = false;
    result = ExtractArchiveForTest(archive.wstring(), (root / L"replaced").wstring(),
        [&](ArchiveExtractionPoint point, const std::wstring& destination, const std::wstring&) {
            if (point == ArchiveExtractionPoint::DestinationCaptured)
                replaced = MoveFileExW(destination.c_str(), (root / L"original-target").c_str(), 0) &&
                    Junction(destination, root / L"outside");
        });
    Check(replaced && result.outcome == ArchiveOutcome::Failed && outside_unchanged(),
        "destination swapped between preflight and pinning is rejected before direct extraction");
    if (replaced) fs::remove(root / L"replaced");

    bool before_seen = false, before_blocked = false, during_seen = false, during_blocked = false;
    result = ExtractArchiveForTest(archive.wstring(), (root / L"stable").wstring(),
        [&](ArchiveExtractionPoint point, const std::wstring& destination, const std::wstring&) {
            const auto parent = fs::path(destination) / L"nested";
            if (point == ArchiveExtractionPoint::BeforeExtract) {
                before_seen = true;
                before_blocked = !MoveFileExW(parent.c_str(), (root / L"moved-before").c_str(), 0) &&
                    !Junction(parent, root / L"outside");
            } else if (point == ArchiveExtractionPoint::ProcessProgress && !during_seen) {
                during_seen = true;
                during_blocked = !MoveFileExW(parent.c_str(), (root / L"moved-during").c_str(), 0) &&
                    !Junction(parent, root / L"outside");
            }
        });
    Check(result.outcome == ArchiveOutcome::Success && before_seen && before_blocked && during_seen && during_blocked &&
        read(root / L"stable/nested/file.txt") == payload && outside_unchanged(),
        "pinned destination directories block rename and junction replacement before and during 7-Zip execution");

    fs::create_directories(root / L"collision/nested");
    Check(CreateHardLinkW((root / L"collision/nested/file.txt").c_str(), (root / L"outside/nested/file.txt").c_str(), nullptr) != FALSE,
        "create existing hard-linked collision");
    std::ofstream(root / L"collision/top.txt") << "keep-top";
    SetFileAttributesW((root / L"collision/top.txt").c_str(), FILE_ATTRIBUTE_READONLY);
    updates.clear();
    result = ExtractArchiveForTest(archive.wstring(), (root / L"collision").wstring(), {}, nullptr,
        [&](const auto& p) { updates.push_back(p); });
    Check(result.outcome == ArchiveOutcome::Success && outside_unchanged() &&
        read(root / L"collision/nested/file.txt") == "outside-original" && read(root / L"collision/top.txt") == "keep-top" &&
        updates.back().total_bytes == 0, "preflight excludes existing files including readonly and hard-linked collisions from extraction and byte totals");
    SetFileAttributesW((root / L"collision/top.txt").c_str(), FILE_ATTRIBUTE_NORMAL);

    bool late_collision = false;
    result = ExtractArchiveForTest(archive.wstring(), (root / L"late").wstring(),
        [&](ArchiveExtractionPoint point, const std::wstring& destination, const std::wstring&) {
            if (point == ArchiveExtractionPoint::BeforeExtract) {
                std::ofstream(fs::path(destination) / L"nested/file.txt") << "late-collision";
                late_collision = true;
            }
        });
    Check(late_collision && result.outcome == ArchiveOutcome::Success &&
        read(root / L"late/nested/file.txt") == "late-collision", "7-Zip skip mode preserves a collision created after preflight");

    Check(Junction(root / L"linked/nested", root / L"outside"), "create linked destination subtree");
    result = ExtractArchiveForTest(archive.wstring(), (root / L"linked").wstring(), {});
    Check(result.outcome == ArchiveOutcome::Failed && outside_unchanged() && !fs::exists(root / L"linked/top.txt"),
        "linked destination subtree rejects the whole plan before any file is extracted");
    fs::remove(root / L"linked/nested");

    std::atomic_bool cancel = false;
    bool cancellation_point = false;
    updates.clear();
    result = ExtractArchiveForTest(archive.wstring(), (root / L"cancel").wstring(),
        [&](ArchiveExtractionPoint point, const std::wstring&, const std::wstring&) {
            if (point == ArchiveExtractionPoint::ProcessProgress && !cancellation_point) {
                cancellation_point = true;
                cancel = true;
            }
        }, &cancel, [&](const auto& p) { updates.push_back(p); });
    Check(cancellation_point && result.outcome == ArchiveOutcome::Cancelled &&
        std::none_of(updates.begin(), updates.end(), [](const auto& p) { return p.phase == ArchivePhase::Completed; }) &&
        fs::is_empty(root / L"temporary"), "cancellation stops direct extraction and cleans its list without claiming completion");

    const auto damaged = root / L"damaged.zip";
    fs::copy_file(archive, damaged);
    {
        std::fstream stream(damaged, std::ios::binary | std::ios::in | std::ios::out);
        const auto bytes = read(damaged);
        const auto data = bytes.find(std::string(64, 'p'));
        Check(data != std::string::npos, "locate stored payload without damaging archive metadata");
        if (data != std::string::npos) {
            stream.seekp(static_cast<std::streamoff>(data));
            stream.put(static_cast<char>(bytes[data] ^ 1));
        }
    }
    updates.clear();
    const auto listing = ListArchive(damaged.wstring());
    result = ExtractArchive(damaged.wstring(), (root / L"failure").wstring(), {}, L"", nullptr,
        [&](const auto& p) { updates.push_back(p); });
    Check(listing.outcome == ArchiveOutcome::Success && result.outcome == ArchiveOutcome::Failed &&
        result.exit_code == 2u && !result.error.empty() &&
        std::none_of(updates.begin(), updates.end(), [](const auto& p) { return p.phase == ArchivePhase::Completed; }) &&
        fs::is_empty(root / L"temporary"), "payload CRC failure stays a failure even after progress reaches 100 and leaves no temporary payload");

    SetEnvironmentVariableW(L"TMP", (root / L"missing-temp").c_str());
    result = ExtractArchive(archive.wstring(), (root / L"no-temp").wstring(), {L"top.txt"}, L"");
    Check(result.outcome == ArchiveOutcome::Success && read(root / L"no-temp/top.txt") == "top-data" &&
        !fs::exists(root / L"missing-temp"), "short selections use in-memory arguments and work without a temporary directory");
    SetEnvironmentVariableW(L"TMP", (root / L"temporary").c_str());

    fs::create_directories(root / L"many-source/many");
    for (int i = 0; i < 700; ++i)
        std::ofstream(root / L"many-source/many" / (std::wstring(64, L'x') + std::to_wstring(i) + L".txt")) << "list-data";
    const auto many_archive = root / L"many.zip";
    Check(Pack(many_archive, root / L"many-source", L""), "pack a selection longer than the Windows command line limit");
    bool fallback_seen = false, locked = false;
    result = ExtractArchiveForTest(many_archive.wstring(), (root / L"many").wstring(),
        [&](ArchiveExtractionPoint point, const std::wstring&, const std::wstring& selection) {
            if (point != ArchiveExtractionPoint::BeforeExtract) return;
            fallback_seen = !selection.empty();
            const HANDLE overwrite = CreateFileW(selection.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            const DWORD overwrite_error = GetLastError();
            if (overwrite != INVALID_HANDLE_VALUE) CloseHandle(overwrite);
            locked = overwrite == INVALID_HANDLE_VALUE && overwrite_error == ERROR_SHARING_VIOLATION &&
                !MoveFileExW(selection.c_str(), (root / L"replaced-list.txt").c_str(), 0) && !DeleteFileW(selection.c_str());
        }, nullptr, {}, {L"many/"});
    if (result.outcome != ArchiveOutcome::Success)
        std::cout << "[DIAG] long selection error=" << DiagnosticUtf8(result.error) << '\n';
    Check(result.outcome == ArchiveOutcome::Success && fallback_seen && locked && fs::is_empty(root / L"temporary") &&
        std::distance(fs::directory_iterator(root / L"many/many"), fs::directory_iterator{}) == 700,
        "oversized selections use a locked private list that cannot be overwritten, renamed or deleted during extraction");

    bool list_edited = false, extraction_started = false;
    result = ExtractArchiveForTest(many_archive.wstring(), (root / L"edited-list").wstring(),
        [&](ArchiveExtractionPoint point, const std::wstring&, const std::wstring& selection) {
            if (point == ArchiveExtractionPoint::SelectionWritten) {
                std::fstream list(fs::path(selection), std::ios::binary | std::ios::in | std::ios::out);
                list.put('z');
                list.flush();
                list_edited = list.good();
            }
            if (point == ArchiveExtractionPoint::BeforeExtract) extraction_started = true;
        }, nullptr, {}, {L"many/"});
    Check(list_edited && !extraction_started && result.outcome == ArchiveOutcome::Failed && !result.error.empty() &&
        fs::is_empty(root / L"edited-list/many") && fs::is_empty(root / L"temporary"),
        "same-size list edits during handle transition are rejected before extraction");

    std::array<ArchiveOperationResult, 2> concurrent_results;
    std::array<std::wstring, 2> concurrent_lists;
    std::array<std::thread, 2> concurrent_workers;
    for (size_t i = 0; i < concurrent_workers.size(); ++i) {
        concurrent_workers[i] = std::thread([&, i] {
            concurrent_results[i] = ExtractArchiveForTest(many_archive.wstring(), (root / (L"concurrent-" + std::to_wstring(i))).wstring(),
                [&](ArchiveExtractionPoint point, const std::wstring&, const std::wstring& selection) {
                    if (point == ArchiveExtractionPoint::BeforeExtract) { concurrent_lists[i] = selection; Sleep(50); }
                }, nullptr, {}, {L"many/"});
        });
    }
    for (auto& worker : concurrent_workers) worker.join();
    Check(concurrent_results[0].outcome == ArchiveOutcome::Success && concurrent_results[1].outcome == ArchiveOutcome::Success &&
        !concurrent_lists[0].empty() && !concurrent_lists[1].empty() && concurrent_lists[0] != concurrent_lists[1] &&
        fs::is_empty(root / L"temporary"), "concurrent oversized selections use separate list files and clean only their own directories");

    SetEnvironmentVariableW(L"TMP", (root / L"missing-temp").c_str());
    updates.clear();
    result = ExtractArchive(many_archive.wstring(), (root / L"list-failure").wstring(), {L"many/"}, L"", nullptr,
        [&](const auto& p) { updates.push_back(p); });
    Check(result.outcome == ArchiveOutcome::Failed && !result.error.empty() &&
        std::none_of(updates.begin(), updates.end(), [](const auto& p) { return p.phase == ArchivePhase::Completed; }) &&
        fs::is_empty(root / L"list-failure/many"), "unavailable list storage fails explicitly without launching extraction or claiming success");
    SetEnvironmentVariableW(L"TMP", (root / L"temporary").c_str());
    if (!failures) { std::error_code ec; fs::remove_all(root, ec); }
    else std::wcout << L"Direct extraction fixtures retained at " << root.wstring() << L'\n';
}
void ProcessTests() {
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    const std::wstring fixture = (fs::path(module).parent_path() / L"pulse_archive_process_fixture.exe").wstring();
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE unrelated = CreateEventW(&security, TRUE, FALSE, nullptr);
    Check(unrelated != nullptr, "create unrelated inheritable event");
    if (unrelated) {
        auto result = RunArchiveProcessForTest(fixture, {L"inherit", std::to_wstring(reinterpret_cast<uintptr_t>(unrelated))}, 1024);
        if (result.outcome != ArchiveOutcome::Success || result.exit_code != 0u)
            std::cerr << "[DIAG] inherit outcome=" << static_cast<int>(result.outcome)
                << " exit=" << (result.exit_code ? std::to_string(*result.exit_code) : "none") << " output=" << DiagnosticUtf8(result.output) << '\n';
        Check(result.outcome == ArchiveOutcome::Success && result.exit_code == 0u && WaitForSingleObject(unrelated, 0) == WAIT_TIMEOUT,
            "archive child does not inherit unrelated handles");
        CloseHandle(unrelated);
    }
    auto excessive = RunArchiveProcessForTest(fixture, {L"output"}, 1024);
    Check(excessive.outcome == ArchiveOutcome::OutputLimit && excessive.exit_code.has_value() && excessive.output == L"压缩包输出超过安全大小限制。",
        "archive output limit terminates writer and fails explicitly");
    auto final = RunArchiveProcessForTest(fixture, {L"final"}, 1024);
    Check(final.outcome == ArchiveOutcome::Success && final.exit_code == 0u && final.output == L"final-output", "archive final output is drained after exit");
    const auto streaming_started = GetTickCount64();
    const auto streaming = RunArchiveProcessForTest(fixture, {L"stream"}, 16 * 1024 * 1024);
    Check(streaming.outcome == ArchiveOutcome::Success && streaming.output.size() == 16 * 1024 * 1024 &&
        streaming.output.front() == L'x' && streaming.output.back() == L'x',
        "large archive listing output drains completely without losing bytes");
    const auto streaming_ms = GetTickCount64() - streaming_started;
    std::cout << "[INFO] archive_stream_16MiB_ms=" << streaming_ms << '\n';
    Check(streaming_ms < 10000, "large listing output is not throttled by small-pipe polling stalls");
    const auto unavailable = RunArchiveProcessForTest(fixture + L".missing", {}, 1024);
    Check(unavailable.outcome == ArchiveOutcome::Failed && !unavailable.exit_code && !unavailable.output.empty(),
        "unstarted archive process does not fabricate an exit code 2");
    std::atomic_bool before_launch{true};
    const auto early_cancel = RunArchiveProcessForTest(fixture, {L"final"}, 1024, &before_launch);
    Check(early_cancel.outcome == ArchiveOutcome::Cancelled && !early_cancel.exit_code,
        "cancellation before launch is distinct from a process failure");
    const std::wstring ready_name = L"Local\\PulseArchiveCancel-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    Check(ready != nullptr, "create archive cancellation fixture event");
    if (ready) {
        std::atomic_bool cancel{false}, launched{false};
        std::thread canceller([&]() {
            launched = WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0;
            cancel = true;
        });
        auto interrupted = RunArchiveProcessForTest(fixture, {L"wait", ready_name}, 1024, &cancel);
        canceller.join();
        Check(launched && interrupted.outcome == ArchiveOutcome::Cancelled && interrupted.exit_code != 0u && interrupted.output == L"已取消解压。",
            "archive cancellation terminates a silent running child");
        CloseHandle(ready);
    }
}
void OutcomeTests() {
    for (const auto* diagnostic : {L"ERROR: archive.7z : Cannot open encrypted archive. Wrong password?",
                                  L"错误：无法打开加密压缩包。密码错误？", L"Echec de lecture", L""}) {
        const auto result = ParseArchiveListingForTest(diagnostic, 2);
        Check(result.outcome == ArchiveOutcome::Failed && result.exit_code == 2u &&
            result.password_state == ArchivePasswordState::Unknown && result.entries.empty(),
            "exit 2 and localized or empty human diagnostics never classify password state");
    }
    const auto encrypted = ParseArchiveListingForTest(L"Path = secret.txt\nSize = 4\nEncrypted = +\n", 0);
    Check(encrypted.outcome == ArchiveOutcome::Success && encrypted.password_state == ArchivePasswordState::Required &&
        encrypted.entries.size() == 1 && encrypted.entries.front().encrypted,
        "successful slt machine encryption field provides positive evidence");
    const auto named = ParseArchiveListingForTest(L"Path = Wrong password encrypted.txt\nSize = 1\nEncrypted = -\n", 0);
    Check(named.outcome == ArchiveOutcome::Success && named.password_state == ArchivePasswordState::NotRequired &&
        named.entries.size() == 1 && !named.entries.front().encrypted,
        "member names never provide encryption or password evidence");
    const auto warning = ParseArchiveListingForTest(L"Path = partial.txt\nEncrypted = +\n", 1);
    Check(warning.outcome == ArchiveOutcome::Failed && warning.exit_code == 1u && warning.entries.empty() &&
        warning.password_state == ArchivePasswordState::Unknown,
        "a warning exit cannot publish a complete successful listing");
    const auto unsafe = ParseArchiveListingForTest(L"Path = ../outside.txt\nEncrypted = -\n", 0);
    Check(unsafe.outcome == ArchiveOutcome::Failed && unsafe.exit_code == 0u && unsafe.entries.empty() && !unsafe.error.empty(),
        "adapter rejection stays failed even when the process exit is zero");
    const auto deletion = ParseArchiveListingForTest(L"Path = existing-folder\nFolder = +\nAnti = +\nEncrypted = -\n", 0);
    Check(deletion.outcome == ArchiveOutcome::Failed && deletion.entries.empty(),
        "7-Zip deletion records are rejected before direct target mutation");
}

void RecoveryTests() {
    const std::wstring healthy = L"Path = download/good.txt\nFolder = -\nSize = 4\nEncrypted = -\nCharacteristics = Local : Descriptor UTF8\n\n";
    const std::wstring damaged = L"Path = download/bad.m4s\nFolder = -\nSize = \nEncrypted = -\nCharacteristics = Local : Descriptor UTF8 Descriptor_ERROR\n\n";
    const auto partial = ParseArchiveListingForTest(healthy + damaged, 2);
    Check(partial.outcome == ArchiveOutcome::PartialListing && partial.exit_code == 2u &&
          partial.password_state == ArchivePasswordState::NotRequired && partial.entries.size() == 1 &&
          partial.entries[0].path == L"download/good.txt" && partial.entries[0].size == 4 &&
          !partial.error.empty() && partial.error.size() < 1024,
          "recoverable ZIP scan preserves healthy records, real exit code and a short warning");
    const auto member_name = ParseArchiveListingForTest(
        L"Path = Descriptor_ERROR.txt\nFolder = -\nSize = 4\nEncrypted = -\nCharacteristics = Local : Descriptor UTF8\n", 2);
    Check(member_name.outcome == ArchiveOutcome::Failed && member_name.entries.empty(),
          "member name cannot fabricate a structured ZIP recovery marker");
    const auto empty = ParseArchiveListingForTest(damaged, 2);
    Check(empty.outcome == ArchiveOutcome::Failed && empty.entries.empty(),
          "a ZIP without any healthy member remains a failure");
    const auto incomplete = ParseArchiveListingForTest(L"Path = incomplete.txt\nEncrypted = -\n\n" + damaged, 2);
    Check(incomplete.outcome == ArchiveOutcome::Failed && incomplete.entries.empty(),
          "partial scan requires complete metadata before publishing a member");
    const auto unsafe = ParseArchiveListingForTest(healthy +
        L"Path = ../outside.txt\nFolder = -\nSize = \nEncrypted = -\nCharacteristics = Local : Descriptor_ERROR\n", 2);
    Check(unsafe.outcome == ArchiveOutcome::Failed && unsafe.entries.empty(),
          "even omitted damaged records cannot bypass unsafe path rejection");
    const auto linked = ParseArchiveListingForTest(healthy + L"Path = link\nFolder = -\nSize = 0\nAttributes = L\n\n" + damaged, 2);
    Check(linked.outcome == ArchiveOutcome::Failed && linked.entries.empty(),
          "ZIP recovery cannot bypass link rejection");
    const auto encrypted = ParseArchiveListingForTest(
        L"Path = locked.txt\nFolder = -\nSize = 4\nEncrypted = +\nCharacteristics = Local : Descriptor UTF8\n\n" + damaged, 2);
    Check(encrypted.outcome == ArchiveOutcome::PartialListing && encrypted.entries.size() == 1 &&
          encrypted.entries[0].encrypted && encrypted.password_state == ArchivePasswordState::Required,
          "recovered metadata retains positive encryption evidence");
    const auto fatal = ParseArchiveListingForTest(healthy + damaged, 7);
    Check(fatal.outcome == ArchiveOutcome::Failed && fatal.entries.empty() && fatal.error.size() < 1024,
          "unsupported process failure is never reclassified as a partial ZIP listing");
    const auto diagnostic = ParseArchiveListingForTest(std::wstring(32768, L'x'), 2);
    Check(diagnostic.outcome == ArchiveOutcome::Failed && diagnostic.error.size() <= 1026,
          "large failure diagnostics are bounded before they reach the window thread");

    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    const auto root = fs::path(module).parent_path().parent_path() / L"bench_data" /
        (L"archive-recovery-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root);
    const auto fixture = root / L"truncated.zip";
    {
        std::ofstream stream(fixture, std::ios::binary);
        const auto local = [&](const std::string& name, bool truncated) {
            Word(stream, 0x04034b50, 4); Word(stream, 20, 2);
            Word(stream, truncated ? 8 : 0, 2); Word(stream, truncated ? 8 : 0, 2);
            Word(stream, 0, 4); Word(stream, 0, 4); Word(stream, 0, 4); Word(stream, 0, 4);
            Word(stream, static_cast<uint32_t>(name.size()), 2); Word(stream, 0, 2); stream << name;
        };
        local("download/", false);
        local("download/readable.txt", false);
        local("download/incomplete.m4s", true);
    }
    const auto real = ListArchive(fixture.wstring());
    Check(real.outcome == ArchiveOutcome::PartialListing && real.exit_code == 2u && real.entries.size() == 2 &&
          real.password_state == ArchivePasswordState::NotRequired,
          "real bundled 7-Zip recovers two readable entries from an isolated truncated ZIP");
    if (real.outcome != ArchiveOutcome::PartialListing)
        std::cout << "[DIAG] recovery outcome=" << static_cast<int>(real.outcome) << " entries=" << real.entries.size()
                  << " error=" << DiagnosticUtf8(real.error) << '\n';
    const auto extract = ExtractArchive(fixture.wstring(), (root / L"output").wstring(), {}, L"", nullptr);
    Check(extract.outcome == ArchiveOutcome::Failed && extract.exit_code == 2u &&
          !fs::exists(root / L"output"), "partial listing is never treated as a successful complete extraction");
    std::atomic_bool cancelled{true};
    const auto cancelled_scan = ListArchive(fixture.wstring(), L"", &cancelled);
    Check(cancelled_scan.outcome == ArchiveOutcome::Cancelled && cancelled_scan.entries.empty(),
          "cancelled recovery does not publish stale archive entries");
    std::error_code error;
    fs::remove_all(root, error);
    Check(!error && !fs::exists(root), "isolated ZIP recovery fixtures are cleaned");
}
#endif
int wmain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--progress-sampling-only") {
        ProgressSamplingTests();
        return failures ? 1 : 0;
    }
    if ((argc == 2 || argc == 3) && std::wstring_view(argv[1]) == L"--journal-errors-only") {
        try {
            JournalErrorTests(argc == 3 ? argv[2] : nullptr);
        } catch (const fs::filesystem_error& exception) {
            std::cerr << "[FAIL] journal diagnostic fixture: " << exception.what() << '\n';
            return 1;
        }
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--retry-recovery-only") {
        RetryIntegrityAudit();
        RetryOwnershipTests();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--password-only") return PasswordTests();
#ifdef PULSE_ARCHIVE_PROCESS_TEST
    if (argc == 2 && std::wstring(argv[1]) == L"--retry-integrity-only") {
        RetryIntegrityAudit();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--progress-only") {
        try {
            ProgressTests();
        } catch (const fs::filesystem_error& exception) {
            std::cerr << "[FAIL] archive progress fixture: " << exception.what() << '\n';
            return 1;
        }
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--direct-only") {
        DirectExtractionTests();
        return failures ? 1 : 0;
    }
    if (argc >= 2 && std::wstring(argv[1]) == L"--recovery-only") {
        RecoveryTests();
        if (argc == 3) {
            const auto started = std::chrono::steady_clock::now();
            const auto real = ListArchive(argv[2]);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            Check(real.outcome == ArchiveOutcome::PartialListing && real.exit_code == 2u &&
                  !real.entries.empty() && real.error.size() < 1024,
                  "read-only user ZIP scan completes with explicit partial-listing warning");
            std::cout << "[INFO] read-only archive scan: entries=" << real.entries.size() << " elapsed_ms=" << elapsed << '\n';
        }
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--outcome-only") {
        OutcomeTests();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--process-only") {
        ProcessTests();
        return failures ? 1 : 0;
    }
#else
    (void)argc;
    (void)argv;
#endif
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    fs::path root = fs::path(temp) / (L"PulseArchiveTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root / L"source/子目录/空目录");
    std::ofstream(root / L"source/子目录/你好.txt") << "unicode-data";
    std::ofstream(root / L"source/top.txt") << "top-data";
    std::wstring error;
    Check(IsArchivePath(L"TEST.RAR") && IsArchivePath(L"a.ZIP") && !IsArchivePath(L"a.txt"), "archive extension detection");
    Check(IsArchivePath(L"backup.tar.gz") && IsArchivePath(L"backup.TXZ") && IsArchivePath(L"backup.cab"), "additional archive extensions");
    Check(IsSafeArchiveMember(L"子目录/你好.txt") && !IsSafeArchiveMember(L"../outside") && !IsSafeArchiveMember(L"C:/outside") && !IsSafeArchiveMember(L"a/NUL.txt") && !IsSafeArchiveMember(L"a/b:stream"), "unsafe member validation");
    for (const auto& extension : {L"zip", L"7z"}) {
        fs::path archive = root / (std::wstring(L"plain.") + extension);
        Check(Pack(archive, root / L"source", L""), "create archive");
        auto list = ListArchive(archive.wstring());
        bool unicode = false, directory = false;
        for (const auto& entry : list.entries) { if (entry.path == L"子目录/你好.txt") unicode = true; if (entry.path == L"子目录/空目录/" && entry.is_dir) directory = true; }
        Check(list.error.empty() && unicode && directory, "list unicode and empty directories");
        Check(std::any_of(list.entries.begin(), list.entries.end(), [](const ArchiveEntry& entry) {
            return entry.mtime.dwLowDateTime || entry.mtime.dwHighDateTime;
        }), "archive modification time retained");
        fs::path output = root / (std::wstring(L"selected-") + extension);
        Check(ExtractArchive(archive.wstring(), output.wstring(), {L"子目录/"}, L"", error) && fs::exists(output / L"子目录/你好.txt") && fs::is_directory(output / L"子目录/空目录") && !fs::exists(output / L"top.txt"), "recursive directory selection");
        std::ofstream(output / L"子目录/你好.txt", std::ios::trunc) << "keep";
        Check(ExtractArchive(archive.wstring(), output.wstring(), {}, L"", error) && fs::exists(output / L"top.txt"), "extract all and skip collisions");
        std::ifstream existing(output / L"子目录/你好.txt"); std::string text; existing >> text;
        Check(text == "keep", "existing content preserved");
        fs::path encrypted = root / (std::wstring(L"encrypted.") + extension);
        Check(Pack(encrypted, root / L"source", std::wstring(L"-psecret ") + (std::wstring(extension) == L"7z" ? L"-mhe=on" : L"-mem=AES256")), "create encrypted archive");
        auto locked = ListArchive(encrypted.wstring());
        Check(locked.password_state == ArchivePasswordState::Required ||
            (locked.outcome == ArchiveOutcome::Failed && locked.password_state == ArchivePasswordState::Unknown),
            "encrypted listing uses machine evidence or an explicit unknown failure without interactive prompt");
        if (std::wstring(extension) == L"zip") {
            bool member_encrypted = false;
            for (const auto& entry : locked.entries) if (!entry.is_dir && entry.encrypted) member_encrypted = true;
            Check(locked.error.empty() && member_encrypted, "encrypted zip permits browsing and flags encrypted members");
        }
        Check(!ExtractArchive(encrypted.wstring(), (root / L"wrong").wstring(), {}, L"wrong", error) && !error.empty(), "wrong password rejected");
        Check(ExtractArchive(encrypted.wstring(), (root / (std::wstring(L"correct-") + extension)).wstring(), {}, L"secret", error), "correct password extraction");
        std::atomic_bool cancel{true};
        Check(!ExtractArchive(archive.wstring(), (root / L"cancelled").wstring(), {}, L"", error, &cancel) && !fs::exists(root / L"cancelled"), "cancellation before launch");
        cancel = false;
        std::thread canceller([&cancel]() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); cancel = true; });
        auto interrupted = ListArchive(archive.wstring(), L"", &cancel);
        canceller.join();
        Check(interrupted.error == L"已取消解压。", "in-flight archive listing cancellation");
    }
    for (const auto& name : {"../outside.txt", "/outside.txt", "C:/outside.txt", "safe/../../outside.txt"}) {
        fs::path unsafe = root / L"unsafe.zip"; UnsafeZip(unsafe, name);
        Check(!ListArchive(unsafe.wstring()).error.empty() && !ExtractArchive(unsafe.wstring(), (root / L"unsafe-output").wstring(), {}, L"", error), "archive traversal rejected");
    }
    UnsafeZip(root / L"link.zip", "symlink", 0120777u << 16);
    Check(!ListArchive((root / L"link.zip").wstring()).error.empty(), "archive symbolic link rejected");
    UnsafeZip(root / L"link-zero-permissions.zip", "symlink", 0120000u << 16);
    Check(!ListArchive((root / L"link-zero-permissions.zip").wstring()).error.empty(), "symbolic link without permission bits rejected");
    fs::create_directories(root / L"outside");
    if (Junction(root / L"junction-output/子目录", root / L"outside")) {
        Check(!ExtractArchive((root / L"plain.zip").wstring(), (root / L"junction-output").wstring(), {}, L"", error) && !fs::exists(root / L"outside/你好.txt"), "destination junction traversal rejected");
        fs::remove(root / L"junction-output/子目录");
    } else std::cout << "[SKIP] destination junction test: environment cannot create a junction\n";
    std::ofstream(root / L"broken.zip") << "not an archive";
    Check(!ListArchive((root / L"broken.zip").wstring()).error.empty(), "corrupt archive error");
    if (failures) std::wcout << L"Fixtures retained at " << root.wstring() << L'\n';
    else { std::error_code ec; fs::remove_all(root, ec); }
    return failures ? 1 : 0;
}

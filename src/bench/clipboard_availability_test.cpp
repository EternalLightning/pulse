#include "../app/clipboard_availability.h"
#include "../app/clipboard_paste_choice.h"
#include "../ui/toolbar_command_state.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
int failures = 0;
int checks = 0;
void Check(bool passed, const char* name) {
    ++checks;
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed) ++failures;
}
}

int wmain() {
    using namespace pulse;
    using PasteSource = app::ClipboardPasteSource;
    const ops::ClipboardData empty_paths{{}, false, 12};
    const ops::ClipboardData valid_drop{{L"source.txt"}, false, 12};
    const auto choose = [&](bool read, const ops::ClipboardData& data, bool shell, bool tray,
                            uint32_t current = 12) {
        return app::ChooseClipboardPasteSource(read, data, shell, tray, 12, current);
    };
    Check(choose(false, empty_paths, true, false) == PasteSource::Shell,
          "failed HDROP read falls back to virtual clipboard contents");
    Check(choose(true, empty_paths, true, false) == PasteSource::Shell,
          "empty HDROP cannot mask virtual clipboard contents");
    Check(choose(false, empty_paths, true, true) == PasteSource::Shell &&
          choose(true, empty_paths, true, true) == PasteSource::Shell,
          "failed or empty HDROP with virtual files does not release the old tray");
    Check(choose(true, valid_drop, true, false) == PasteSource::Paths,
          "valid HDROP takes the local file operation path before virtual formats");
    Check(choose(true, valid_drop, true, true) == PasteSource::Tray &&
          choose(true, valid_drop, false, true) == PasteSource::Tray,
          "valid path clipboard preserves the existing tray priority");
    Check(choose(false, empty_paths, false, true) == PasteSource::Tray &&
          choose(false, empty_paths, false, false) == PasteSource::None,
          "ordinary tray fallback and empty clipboard behavior remain unchanged");
    Check(choose(false, empty_paths, true, true, 13) == PasteSource::None &&
          choose(true, valid_drop, true, true, 13) == PasteSource::None,
          "clipboard replacement rejects virtual and tray requests before dispatch");
    auto stale_drop = valid_drop;
    stale_drop.sequence = 11;
    Check(choose(true, stale_drop, true, true) == PasteSource::None,
          "mismatched decoded path sequence cannot fall through to shell or tray");
    const ui::ToolbarCommandState drive_commands{false, 1, true, false};
    Check(!drive_commands.Enabled(0) && !drive_commands.Enabled(1) && !drive_commands.Enabled(4),
          "drive toolbar disables cut copy and delete");
    Check(drive_commands.Enabled(3) && drive_commands.Enabled(5),
          "single drive toolbar keeps volume label rename and properties");
    Check(!ui::ToolbarCommandState{false, 2, true, false}.Enabled(3),
          "mixed or multiple drive selection disables toolbar rename");
    Check(!drive_commands.Enabled(2) && ui::ToolbarCommandState{false, 0, false, true}.Enabled(2),
          "toolbar paste follows availability independently of selection");
    const ui::ToolbarCommandState file_commands{false, 1, false, true};
    Check(file_commands.Enabled(0) && file_commands.Enabled(1) && file_commands.Enabled(3) && file_commands.Enabled(4),
          "ordinary file toolbar retains file operations");
    Check(!ui::ToolbarCommandState{true, 1, false, true}.Enabled(2), "archive toolbar forbids paste");
    app::ClipboardAvailability cached;
    unsigned collections = 0;
    std::vector<std::wstring> tray_paths{L"one"};
    const auto collect = [&] { ++collections; return tray_paths; };
    cached.UpdateVersioned(1, true, true, 1, collect);
    const auto cached_request = cached.BeginRefresh(0);
    if (!cached_request) return 2;
    cached.mailbox()->Publish({cached_request->generation, true, true});
    cached.Consume();
    for (int i = 0; i < 10000; ++i) cached.UpdateVersioned(1, true, true, 1, collect);
    Check(collections == 1 && cached.Available(), "unchanged tray inputs do not recollect paths on 10k UI updates");
    cached.UpdateVersioned(1, true, true, 2, collect);
    Check(collections == 2 && cached.Available(), "unrelated tray revision keeps validated identical paths available");
    tray_paths = {L"two"};
    cached.UpdateVersioned(1, true, true, 3, collect);
    cached.mailbox()->Publish({cached_request->generation, true, true});
    cached.Consume();
    Check(!cached.Available(), "changed tray paths invalidate validation and reject the old result");
    cached.Update(1, true, true, {L"direct"});
    cached.UpdateVersioned(1, true, true, 3, collect);
    const auto mixed = cached.BeginRefresh(1000);
    Check(mixed && mixed->tray_paths == tray_paths && collections == 4,
          "direct updates invalidate the revision shortcut for mixed callers");
    cached.UpdateVersioned(2, true, true, 3, collect);
    Check(collections == 5, "clipboard sequence change still refreshes the input snapshot");
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return 2;
    const auto fixture = std::filesystem::path(executable).parent_path().parent_path() /
        L"bench_data" / (L"clipboard_availability_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::create_directories(fixture, error);
    if (error) return 2;
    const auto source = fixture / L"source.txt";
    std::ofstream(source) << "clipboard source";

    app::ClipboardAvailability availability;
    ops::ClipboardData clipboard{{source.wstring()}, false, 1};
    unsigned reads = 0;
    const auto reader = [&](ops::ClipboardData& out) { ++reads; out = clipboard; return !out.paths.empty(); };
    const DWORD ui_thread = GetCurrentThreadId();
    bool probed_on_worker = false;
    const auto exists = [&](const std::wstring& path) {
        probed_on_worker = GetCurrentThreadId() != ui_thread;
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    const auto complete = [&](const app::ClipboardAvailabilityRequest& request) {
        const auto mailbox = availability.mailbox();
        std::thread worker([&] { mailbox->Publish(app::ProbeClipboardAvailability(request, reader, exists)); });
        worker.join();
        availability.Consume();
    };

    availability.Update(1, false, false, {});
    Check(!availability.Available() && !availability.BeginRefresh(0),
          "empty or text clipboard disables paste without source probes");
    availability.Update(1, false, false, {}, true);
    const auto virtual_files = availability.BeginRefresh(0);
    if (!virtual_files) return 2;
    const unsigned reads_before_virtual = reads;
    const auto virtual_result = app::ProbeClipboardAvailability(*virtual_files, reader, exists);
    availability.mailbox()->Publish(virtual_result);
    availability.Consume();
    Check(availability.Available() && reads == reads_before_virtual,
          "virtual-file formats enable paste without treating them as filesystem paths");
    availability.Update(1, false, true, {}, true);
    const auto virtual_without_tray_paths = availability.BeginRefresh(1000);
    Check(virtual_without_tray_paths.has_value(),
          "new virtual clipboard remains available when the older tray batch has no live paths");
    if (!virtual_without_tray_paths) return 2;
    availability.mailbox()->Publish(app::ProbeClipboardAvailability(
        *virtual_without_tray_paths, reader, exists));
    availability.Consume();
    Check(availability.Available(), "virtual clipboard takes priority over unavailable older tray paths");
    app::ClipboardAvailability mixed_formats;
    mixed_formats.Update(1, true, true, {}, true);
    const auto mixed_request = mixed_formats.BeginRefresh(0);
    if (!mixed_request) return 2;
    const auto failed_reader = [](ops::ClipboardData&) { return false; };
    mixed_formats.mailbox()->Publish(app::ProbeClipboardAvailability(*mixed_request, failed_reader, exists));
    mixed_formats.Consume();
    Check(mixed_formats.Available(),
          "mixed HDROP and virtual formats enable paste after the path reader fails despite an empty tray");
    const auto empty_reader = [](ops::ClipboardData& out) { out = {{}, false, 1}; return true; };
    mixed_formats.mailbox()->Publish(app::ProbeClipboardAvailability(*mixed_request, empty_reader, exists));
    mixed_formats.Consume();
    Check(mixed_formats.Available(),
          "mixed HDROP and virtual formats enable paste when decoded paths are empty");
    mixed_formats.mailbox()->Publish(app::ProbeClipboardAvailability(*mixed_request, reader, exists));
    mixed_formats.Consume();
    Check(!mixed_formats.Available(),
          "valid HDROP with a virtual format still requires the selected tray paths to be available");
    mixed_formats.Update(2, false, false, {});
    mixed_formats.mailbox()->Publish(app::ProbeClipboardAvailability(*mixed_request, failed_reader, exists));
    mixed_formats.Consume();
    Check(!mixed_formats.Available(), "stale mixed-format availability cannot enable a replacement clipboard");
    availability.Update(1, true, false, {});
    const auto first = availability.BeginRefresh(0);
    Check(first.has_value() && !availability.Available(), "file clipboard waits for worker validation");
    if (!first) return 2;
    complete(*first);
    Check(availability.Available() && probed_on_worker, "existing clipboard file enables paste after worker probe");
    Check(!availability.BeginRefresh(999), "unchanged clipboard is not reprobed on every UI tick");
    std::filesystem::remove(source, error);
    const auto deleted = availability.BeginRefresh(1000);
    if (!deleted) return 2;
    complete(*deleted);
    Check(!availability.Available(), "deleted source disables paste with unchanged clipboard sequence");

    std::ofstream(source) << "new clipboard source";
    availability.Update(2, true, false, {});
    const auto stale = availability.BeginRefresh(2000);
    availability.Update(3, false, false, {});
    if (!stale) return 2;
    availability.mailbox()->Publish({stale->generation, true, false});
    availability.Consume();
    Check(!availability.Available(), "stale worker result cannot reenable paste after clipboard changed to text");

    availability.Update(4, true, false, {});
    const auto mismatched = availability.BeginRefresh(3000);
    clipboard.sequence = 5;
    if (!mismatched) return 2;
    complete(*mismatched);
    Check(!availability.Available(), "clipboard change during decode rejects mismatched sequence");
    clipboard.sequence = 4;
    clipboard.paths.clear();
    const auto empty_drop = availability.BeginRefresh(4000);
    if (!empty_drop) return 2;
    complete(*empty_drop);
    Check(!availability.Available(), "empty file drop disables paste even when format is present");

    const unsigned previous_reads = reads;
    const auto tray_source = fixture / L"tray.txt";
    std::ofstream(tray_source) << "live tray source";
    availability.Update(5, false, true, {tray_source.wstring()});
    Check(!availability.Available() && !availability.BeginRefresh(5000) && reads == previous_reads,
          "text or empty clipboard disables paste despite a live old tray batch");
    clipboard.sequence = 6; clipboard.paths = {source.wstring()};
    availability.Update(6, true, true, {tray_source.wstring()});
    const auto tray = availability.BeginRefresh(5000);
    if (!tray) return 2;
    complete(*tray);
    Check(availability.Available() && reads > previous_reads, "valid file clipboard and tray sources enable paste");
    std::filesystem::remove(source, error);
    const auto deleted_clipboard = availability.BeginRefresh(6000);
    if (!deleted_clipboard) return 2;
    complete(*deleted_clipboard);
    Check(!availability.Available(), "deleted clipboard source disables paste despite live old tray sources");
    std::ofstream(source) << "restored clipboard source";
    std::filesystem::remove(tray_source, error);
    const auto deleted_tray = availability.BeginRefresh(7000);
    if (!deleted_tray) return 2;
    complete(*deleted_tray);
    Check(!availability.Available(), "deleted tray source disables paste instead of trusting cached exists flag");
    availability.Update(7, true, true, {});
    Check(!availability.Available() && !availability.BeginRefresh(8000), "empty newest tray cannot incorrectly enable clipboard fallback");

    std::filesystem::remove(source, error);
    std::filesystem::remove(fixture, error);
    Check(!error, "isolated clipboard fixture cleaned up");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

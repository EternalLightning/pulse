#include "../app/text_diff.h"
#include "../app/text_compare_selection.h"
#include "../app/app_model.h"
#include "../app/context_menu.h"
#include "../common/localization.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }

int wmain() {
    namespace diff = pulse::diff;
    namespace app = pulse::app;
    int failed = 0;
    auto check = [&](bool value, const char* name) {
        std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!value) ++failed;
    };
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    app::Tab tab;
    tab.current_path = L"C:\\first";
    auto entries = std::make_shared<std::vector<pulse::fs::DirEntry>>(2);
    (*entries)[0].name = L"a.txt";
    (*entries)[1].name = L"b.md";
    tab.snapshot = entries;
    tab.ToggleSelect(0);
    tab.selected_index = 0;
    check(app::TextCompareMenuPath(tab) == L"C:\\first\\a.txt", "single text file uses snapshot-only full path");
    auto has = [](const auto& items, int command) {
        return std::any_of(items.begin(), items.end(), [command](const auto& item) { return item.command == command; });
    };
    std::vector<pulse::ui::FluentMenuItem> menu;
    app::AppendTextCompareCommands(menu, true, app::TextCompareMenuPath(tab), false);
    check(has(menu, app::CmdSelectTextCompare) && !has(menu, app::CmdCompareSelectedText), "initial context menu only offers selecting source");
    menu.clear();
    app::AppendTextCompareCommands(menu, true, app::TextCompareMenuPath(tab), true);
    check(has(menu, app::CmdSelectTextCompare) && has(menu, app::CmdCompareSelectedText), "source selection keeps select command and adds compare command");
    menu.clear();
    app::AppendTextCompareCommands(menu, false, app::TextCompareMenuPath(tab), true);
    check(menu.empty(), "setting disables both context menu commands");
    (*entries)[0].is_dir = true;
    check(app::TextCompareMenuPath(tab).empty(), "directory named txt is ineligible");
    (*entries)[0].is_dir = false;
    tab.ToggleSelect(1);
    check(app::TextCompareMenuPath(tab).empty(), "multi-selection has no text compare commands");
    tab.ToggleSelect(1);
    (*entries)[0].change_record_only = true;
    check(app::TextCompareMenuPath(tab).empty(), "historical change record is ineligible");
    (*entries)[0].change_record_only = false;
    tab.current_path = L"pulse:archive:example.zip";
    check(app::TextCompareMenuPath(tab).empty(), "archive entries cannot be read as ordinary paths");
    app::TextCompareSelection source;
    check(!source.ready(), "no second compare action before source selection");
    check(source.Select(L"C:\\first\\a.TXT") && source.ready(), "select first text source");
    const std::wstring retained = source.source();
    check(!source.Select(L"C:\\second\\picture.png") && source.source() == retained,
          "invalid source does not clear the retained selection");
    check(source.Select(L"\\\\server\\share\\next.md") &&
          source.source() == L"\\\\server\\share\\next.md", "selecting again replaces source across directories");
    check(!app::IsTextCompareFile(L"C:\\folder.txt", true) &&
          !app::IsTextCompareFile(L"pulse:archive:fake.txt") &&
          app::IsTextCompareFile(L"C:\\source\\.gitignore"), "text eligibility excludes folders and virtual entries");

    diff::DiffResult result;
    check(diff::ComputeLineDiff({}, {}, {}, result) && result.rows.empty() && result.hunks.empty(),
          "empty files compare without invalid pointer arithmetic");
    check(diff::ComputeLineDiff({L"same"}, {L"same"}, {}, result) && result.hunks.empty(), "identical lines");
    check(diff::ComputeLineDiff({}, {L"added"}, {}, result) && result.added == 1, "added line");
    check(diff::ComputeLineDiff({L"deleted"}, {}, {}, result) && result.deleted == 1, "deleted line");
    check(diff::ComputeLineDiff({L"start", L"old", L"end"}, {L"start", L"new", L"end"}, {}, result) &&
          result.modified == 1 && result.hunks.size() == 1, "modified line retains unchanged context");
    check(diff::ComputeLineDiff({L"a\tb"}, {L"a   b"}, {}, result) && result.modified == 1,
          "tabs remain a meaningful content difference");
    check(diff::ComputeLineDiff({L"A\tb"}, {L"a b"}, {true, true}, result) && result.hunks.empty(),
          "ignore whitespace and case apply together");
    const std::atomic<bool> cancelled{true};
    check(!diff::ComputeLineDiff({L"a"}, {L"b"}, {}, result, &cancelled), "cancelled computation is discarded");
    std::vector<diff::CharSpan> a_spans, b_spans;
    diff::ComputeCharDiff(L"abc old xyz", L"abc new xyz", {}, a_spans, b_spans);
    check(a_spans.size() == 1 && b_spans.size() == 1 && a_spans[0].begin == 4 && a_spans[0].end == 7,
          "character highlights isolate the edit");
    std::vector<std::wstring> many_a(3000, L"a"), many_b(3000, L"b");
    check(diff::ComputeLineDiff(many_a, many_b, {}, result) && result.capped && result.modified == 3000,
          "large edit distance uses bounded fallback");

    const auto fixture = std::filesystem::path(L"bench_data") /
        (L"text-compare-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(fixture);
    const auto file = fixture / L"unicode.txt";
    auto write = [&](const std::string& bytes) {
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    diff::DiffSide side;
    write("hello\tworld\n");
    diff::LoadDiffSide(file.wstring(), side);
    check(side.status == diff::LoadStatus::Ok && side.lines.size() == 2 &&
          side.lines[0] == L"hello\tworld" && side.lines[1].empty() && side.eol == L"LF",
          "file loading preserves tabs and final newline");
    const auto with_eol = side.lines;
    write("hello\tworld");
    diff::LoadDiffSide(file.wstring(), side);
    check(diff::ComputeLineDiff(with_eol, side.lines, {}, result) && result.deleted == 1,
          "missing final newline is visible as a difference");
    write(std::string("\xFF\xFE\x2D\x4E\x87\x65", 6));
    diff::LoadDiffSide(file.wstring(), side);
    check(side.status == diff::LoadStatus::Ok && side.lines.size() == 1 && side.lines[0] == L"中文",
          "existing text decoder handles UTF-16 Chinese");
    write(std::string("a\0b", 3));
    diff::LoadDiffSide(file.wstring(), side);
    check(side.status == diff::LoadStatus::Binary, "binary text-extension file fails explicitly");
    write("");
    std::filesystem::resize_file(file, diff::kMaxDiffFileBytes + 1);
    diff::LoadDiffSide(file.wstring(), side);
    check(side.status == diff::LoadStatus::TooLarge, "oversized files rejected before allocation");
    write(std::string(diff::kMaxDiffLines, '\n'));
    diff::LoadDiffSide(file.wstring(), side);
    check(side.status == diff::LoadStatus::TooLarge, "line count limit bounds result and UI row work");
    diff::LoadDiffSide((fixture / L"missing.txt").wstring(), side);
    check(side.status == diff::LoadStatus::ReadFailed, "deleted source is a read error");
    std::filesystem::remove(file);
    std::filesystem::remove(fixture);
    return failed ? 1 : 0;
}

// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../model/search_query.h"
#include "../model/batch_rename.h"
#include "../common/file_identity.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
bool BoundsMatch(const pulse::index::CompiledQuery& query, uint64_t size, uint64_t date) {
    if (query.groups.empty()) return true;
    for (const auto& group : query.groups) {
        bool match = true;
        for (const auto& term : group) {
            const bool size_match = pulse::index::MatchSize(size, term);
            const bool date_match = pulse::index::MatchDate(date, term);
            match &= term.size_not ? !size_match : size_match;
            match &= term.date_not ? !date_match : date_match;
        }
        if (match) return true;
    }
    return false;
}
void SearchCases() {
    using namespace pulse::model;
    constexpr uint64_t mb = 1024 * 1024;
    const std::wstring queries[] = {
        L"size:>42mb dm:2026-01-01..2026-06-30",
        L"大小：>=42mb 日期：2026-01",
        L"SIZE:<42MB DATEMODIFIED:2026",
        L"size:>=1mb size:<=10mb size:>4mb dm:2026 !dm:2026-06",
        L"!size:<1mb !dm:today",
        L"size:<1mb dm:2026-01 | size:>42mb dm:2026-06",
        L"| size:>42mb dm:2026-01-01..2026-06-30 |",
        L"size:>42mb | content:todo",
        L"size:bogus dm:2026-02-30",
    };
    const uint64_t sizes[] = {0, 1, mb - 1, mb, 4 * mb, 5 * mb, 10 * mb,
        42 * mb, 42 * mb + 1, 100 * mb};
    std::vector<uint64_t> dates;
    for (const auto* value : {L"2025-12-31", L"2026-01-01", L"2026-03-15",
            L"2026-06-01", L"2026-06-30", L"2026-07-01", L"2027-01-01", L"today"}) {
        const auto date = pulse::index::ParseQuery(std::wstring(L"dm:") + value);
        dates.push_back(date.groups[0][0].date_lo);
    }
    for (const auto& raw : queries) {
        auto spec = ParseSearchQuery(raw);
        spec.kind = pulse::index::SearchKind::Image;
        spec.name = L"edited";
        const auto result = pulse::index::ParseQuery(CompileSearchQuery(spec));
        const auto expected = pulse::index::ParseQuery(raw);
        bool same = true;
        for (auto size : sizes) for (auto date : dates)
            same &= BoundsMatch(expected, size, date) == BoundsMatch(result, size, date);
        Check(same, "size/date predicates survive unrelated kind/name edits");
    }
    auto spec = ParseSearchQuery(queries[0]);
    Check(spec.size == SizePreset::Custom && spec.size_custom == L">42mb" &&
        spec.date == DatePreset::Custom && spec.date_from == L"2026-01-01" &&
        spec.date_to == L"2026-06-30", "custom bounds populate editable form fields");
    spec.size = SizePreset::Any;
    auto result = pulse::index::ParseQuery(CompileSearchQuery(spec));
    Check(std::all_of(result.groups[0].begin(), result.groups[0].end(), [](const auto& term) {
        return term.size_how == pulse::index::SizeHow::Any;
    }) && !BoundsMatch(result, 0, dates.front()), "explicit size removal retains the date restriction");
    spec = ParseSearchQuery(queries[0]);
    spec.date_from = L"2026-03-01";
    spec.date_to = L"2026-03-31";
    result = pulse::index::ParseQuery(CompileSearchQuery(spec));
    Check(BoundsMatch(result, 100 * mb, dates[2]) && !BoundsMatch(result, 100 * mb, dates[1]) &&
        !BoundsMatch(result, 0, dates[2]), "editing custom date replaces its bounds and keeps size");
    spec = ParseSearchQuery(queries[0]);
    spec.size_custom = L"SIZE:>84MB";
    result = pulse::index::ParseQuery(CompileSearchQuery(spec));
    Check(BoundsMatch(result, 100 * mb, dates[2]) && !BoundsMatch(result, 50 * mb, dates[2]) &&
        !BoundsMatch(result, 100 * mb, dates.front()),
        "editing custom size accepts uppercase modifiers and keeps the date restriction");
    spec = ParseSearchQuery(L"content:\"dm:thismonth size:>42mb\"");
    Check(spec.date == DatePreset::Any && spec.size == SizePreset::Any,
        "quoted content is never captured as size/date modifiers");
    const auto preset = ParseSearchQuery(L"size:>=1mb size:<=10mb dm:thismonth");
    Check(preset.size == SizePreset::From1To10MB && preset.date == DatePreset::ThisMonth,
        "existing size/date presets remain editable");
}
void RenameCases(const std::filesystem::path& parent) {
    using namespace pulse::model;
    const auto root = parent / (L"rename-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const auto source = root / L"report.txt";
    const auto target = root / L"REPORT.txt";
    std::ofstream(source) << "case-only fixture";
    BatchRenameRule rule;
    rule.find = L"report";
    rule.replace = L"REPORT";
    auto preview = PreviewBatchRename({source.wstring()}, rule);
    Check(preview.size() == 1 && preview[0].status == BatchRenameStatus::Ok &&
        preview[0].new_name == L"REPORT.txt", "case-only rename is an actionable preview");
    Check(PreviewBatchRename({source.wstring()}, {})[0].status == BatchRenameStatus::Unchanged,
        "identical name remains unchanged");
    std::ofstream(root / L"other.txt") << "existing target";
    rule.replace = L"other";
    Check(PreviewBatchRename({source.wstring()}, rule)[0].status == BatchRenameStatus::Collision,
        "different existing destination remains a collision");
    pulse::FileIdentity before, after;
    pulse::ReadPathFileIdentity(source.wstring(), before);
    Check(MoveFileW(source.c_str(), target.c_str()) && pulse::ReadPathFileIdentity(target.wstring(), after) &&
        before == after, "isolated Win32 case-only rename preserves the source identity");
    const HANDLE directory = CreateFileW(root.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    FILE_CASE_SENSITIVE_INFO info{FILE_CS_FLAG_CASE_SENSITIVE_DIR};
    const bool sensitive = directory != INVALID_HANDLE_VALUE &&
        SetFileInformationByHandle(directory, FileCaseSensitiveInfo, &info, sizeof(info));
    const DWORD error = sensitive ? ERROR_SUCCESS : GetLastError();
    if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
    if (sensitive) {
        std::ofstream(source) << "distinct lower-case identity";
        rule.replace = L"REPORT";
        Check(PreviewBatchRename({source.wstring()}, rule)[0].status == BatchRenameStatus::Collision,
            "case-sensitive distinct file cannot bypass destination collision detection");
    } else {
        std::printf("[SKIP] case-sensitive directory unavailable, Win32 error=%lu\n", error);
    }
    std::filesystem::remove_all(root);
    Check(!std::filesystem::exists(root), "isolated rename fixtures are removed");
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    SearchCases();
    RenameCases(std::filesystem::absolute(argv[1]));
    std::printf("COMMON failures=%d\n", failures);
    return failures ? 1 : 0;
}

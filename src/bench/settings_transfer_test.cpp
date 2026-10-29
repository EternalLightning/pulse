// settings_transfer_test.cpp - Settings import / export (settings_transfer.h):
// bundle contents, staged import with backup, restore, and the guards.
// Works only in bench_data\ fixtures built here; never the real data dir.
#include "../app/settings_transfer.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include "../index/content_search_protocol.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace pulse::app::settings_transfer;
namespace fs = std::filesystem;

namespace {
int passed = 0;
int failed = 0;
void Check(bool condition, const wchar_t* name) {
    ++(condition ? passed : failed);
    wprintf(L"[%s] %s\n", condition ? L"PASS" : L"FAIL", name);
}

void Write(const fs::path& dir, const wchar_t* name, const std::wstring& text) {
    pulse::WriteUtf8FileAtomic((dir / name).wstring(), text);
}
std::wstring Read(const fs::path& dir, const wchar_t* name) {
    std::wstring text;
    pulse::ReadUtf8File((dir / name).wstring(), text);
    return text;
}
bool Has(const fs::path& dir, const wchar_t* name) { return fs::exists(dir / name); }
bool Contains(const std::wstring& text, const wchar_t* part) {
    return text.find(part) != std::wstring::npos;
}

// The source PC: every file, machine-local members, legacy colours.
void SeedSource(const fs::path& dir) {
    fs::create_directories(dir);
    Write(dir, L"app.json",
          L"{\n  \"version\":4,\n  \"launch_on_startup\":true,\n  \"integration_enabled\":true,\n"
          L"  \"theme_mode\":2,\n  \"language\":\"zh-CN\",\n  \"picker_folders\":[\"a|C:\\\\src\"],\n"
          L"  \"shortcuts\":[\"copy_path|Ctrl+Shift+C\"],\n  \"row_height\":32\n}\n");
    Write(dir, L"context_menu.json", L"{\"builtin_hidden\":12}");
    Write(dir, L"places.json",
          L"{\n  \"places_version\":2,\n  \"tag_version\":2,\n  \"tags_generation\":\"5\",\n"
          L"  \"tags\":[{\"id\":\"t1\",\"name\":\"工作\",\"rgb\":0xFF0000}],\n"
          L"  \"quick_access_paths\":[\"D:\\\\Projects\"],\n"
          L"  \"recent_items\":[{\"path\":\"C:\\\\secret.txt\",\"opened_at\":1}]\n}\n");
    Write(dir, L"tags.json",
          L"{\n  \"version\":3,\n  \"tags_generation\":\"7\",\n"
          L"  \"tags\":[{\"id\":\"t1\",\"name\":\"工作\",\"rgb\":0xFF0000}]\n}\n");
    Write(dir, L"aliases.json", L"{\"aliases\":{\"D:\\\\Projects\":\"项目\"}}");
    Write(dir, L"saved_searches.json", L"{\"version\":1,\"items\":[]}");
}

// The target PC: its own integration, history and lists.
void SeedTarget(const fs::path& dir) {
    fs::remove_all(dir);
    fs::create_directories(dir);
    Write(dir, L"app.json",
          L"{\n  \"version\":4,\n  \"launch_on_startup\":false,\n  \"integration_enabled\":false,\n"
          L"  \"theme_mode\":1,\n  \"picker_folders\":[\"b|E:\\\\mine\"],\n  \"newer_key\":7\n}\n");
    Write(dir, L"places.json",
          L"{\n  \"places_version\":2,\n  \"tags_generation\":\"9\",\n"
          L"  \"tags\":[{\"id\":\"x\",\"name\":\"旧\",\"rgb\":0x00FF00}],\n"
          L"  \"quick_access_paths\":[\"E:\\\\Mine\"],\n"
          L"  \"recent_items\":[{\"path\":\"E:\\\\keep.txt\",\"opened_at\":2}]\n}\n");
    Write(dir, L"tags.json", L"{\"version\":3,\"tags_generation\":\"8\",\"tags\":[]}");
    Write(dir, L"tag_rules.json", L"{\"rules\":[]}");
    Write(dir, L"tags.pending.json", L"{\"save_generation\":\"99\",\"tags\":[]}");
    Write(dir, L"aliases.json", L"{\"aliases\":{}}");
    Write(dir, L"saved_searches.json", L"{\"version\":1,\"items\":[{\"name\":\"mine\"}]}");
}

std::wstring Member(const std::wstring& object, const wchar_t* key) {
    const pulse::json::Document doc(object, true);
    const auto* value = doc.Find(key);
    return value ? doc.Raw(*value) : L"";
}
} // namespace

int wmain() {
    const fs::path root = fs::absolute(L"bench_data/settings-transfer-" +
                                       std::to_wstring(GetCurrentProcessId()));
    const fs::path source = root / L"source";
    const fs::path target = root / L"target";
    SeedSource(source);

    // ---- export ----
    Check(AvailableCategories(source.wstring()) == kAllCategories, L"every category has data");
    std::wstring bundle_text;
    uint32_t included = 0;
    Check(BuildBundle(source.wstring(), kAllCategories, {L"1.0.57", L"2026-10-21T23:40:05"},
                      bundle_text, &included) && included == kAllCategories,
          L"export builds a bundle with every category");
    Check(!Contains(bundle_text, L"launch_on_startup") && !Contains(bundle_text, L"integration_enabled") &&
          !Contains(bundle_text, L"picker_folders"),
          L"export leaves machine-local preferences out");
    Check(!Contains(bundle_text, L"secret.txt"), L"export leaves recent items out");
    Check(Contains(bundle_text, L"\"theme_mode\":2") && Contains(bundle_text, L"copy_path|Ctrl+Shift+C"),
          L"export keeps preferences and shortcuts");
    Check(pulse::json::Document(bundle_text).Is(pulse::json::Kind::Object), L"bundle is strict JSON despite legacy colours");
    std::wstring prefs_only;
    Check(BuildBundle(source.wstring(), kPrefs, {L"1.0.57", L"x"}, prefs_only, &included) &&
          included == kPrefs && !Contains(prefs_only, L"aliases.json") &&
          !Contains(prefs_only, L"places.json"),
          L"export of one category carries only its files");
    std::wstring tags_only;
    BuildBundle(source.wstring(), kTags, {L"1.0.57", L"x"}, tags_only, nullptr);
    Bundle tags_bundle;
    Check(ParseBundle(tags_only, tags_bundle) == ParseResult::Ok && tags_bundle.File(L"places.json") &&
          !Contains(*tags_bundle.File(L"places.json"), L"quick_access_paths") &&
          Contains(*tags_bundle.File(L"places.json"), L"t1"),
          L"tags-only export keeps places.json's tag members only");

    // ---- parse ----
    Bundle bundle;
    Check(ParseBundle(bundle_text, bundle) == ParseResult::Ok, L"bundle parses");
    Check(bundle.categories == kAllCategories && bundle.app_version == L"1.0.57" &&
          DisplayTime(bundle.exported_at) == L"2026-10-21 23:40",
          L"bundle header round trips");
    Check(bundle.File(L"places.json") && Contains(*bundle.File(L"places.json"), L"0xFF0000"),
          L"legacy colours survive as text");
    Bundle rejected;
    Check(ParseBundle(L"{\"aliases\":{}}", rejected) == ParseResult::Invalid, L"a plain aliases file is rejected");
    Check(ParseBundle(L"not json", rejected) == ParseResult::Invalid, L"garbage is rejected");
    Check(ParseBundle(L"{\"format\":\"pulse-settings\",\"format_version\":2,\"files\":{}}", rejected) ==
          ParseResult::Newer, L"a newer format is reported as newer");
    Check(ParseBundle(L"{\"format\":\"pulse-settings\",\"format_version\":1,\"files\":{"
                      L"\"session.json\":{\"left\":1},\"aliases.json\":{\"aliases\":{}}}}", rejected) ==
          ParseResult::Ok && !rejected.File(L"session.json") && rejected.categories == kAliases,
          L"unknown files are ignored, categories follow the files");
    Check(ParseBundle(L"{\"format\":\"pulse-settings\",\"format_version\":1,\"files\":{"
                      L"\"aliases.json\":\"{broken\"}}", rejected) == ParseResult::Invalid,
          L"a damaged embedded file is rejected");

    // ---- import: settings, tags and aliases (not places, not searches) ----
    SeedTarget(target);
    const std::wstring before_app = Read(target, L"app.json");
    Check(StageImport(target.wstring(), bundle, kPrefs | kTags | kAliases), L"import stages");
    Check(HasPendingImport(target.wstring()), L"staged import is pending");
    Check(Member(Read(target, L"app.json"), L"theme_mode") == L"1", L"staging leaves live files alone");
    const ApplyResult applied = ApplyPendingImport(target.wstring(), L"20261021-120000");
    Check(applied.outcome == ApplyOutcome::Imported && !HasPendingImport(target.wstring()),
          L"start applies the staged import once");
    const std::wstring app = Read(target, L"app.json");
    Check(Member(app, L"theme_mode") == L"2" && Contains(app, L"copy_path"), L"imported preferences arrive");
    Check(Member(app, L"launch_on_startup") == L"false" && Member(app, L"integration_enabled") == L"false" &&
          Contains(app, L"E:\\\\mine"),
          L"this PC's integration and picker history stay");
    Check(Member(app, L"newer_key") == L"7", L"preferences the bundle lacks stay");
    Check(pulse::json::Document(app).Is(pulse::json::Kind::Object), L"merged app.json is strict JSON");
    Check(Member(Read(target, L"context_menu.json"), L"builtin_hidden") == L"12",
          L"context menu layout arrives with the settings");
    const std::wstring places = Read(target, L"places.json");
    Check(Contains(places, L"E:\\\\Mine") && !Contains(places, L"D:\\\\Projects"),
          L"quick access stays when places is not chosen");
    Check(Contains(places, L"keep.txt"), L"recent items of this PC stay");
    Check(Contains(places, L"t1") && !Contains(places, L"\"旧\""), L"tags in places.json are replaced");
    const std::wstring tags = Read(target, L"tags.json");
    Check(Member(places, L"tags_generation") == L"\"10\"" && Member(tags, L"tags_generation") == L"\"10\"",
          L"both tag copies move one generation above everything seen");
    Check(!Has(target, L"tags.pending.json"), L"a stale tag recovery file is dropped");
    Check(!Has(target, L"tag_rules.json"), L"tag rules the source lacks are removed");
    Check(Contains(Read(target, L"aliases.json"), L"项目"), L"aliases arrive");
    Check(Contains(Read(target, L"saved_searches.json"), L"mine"), L"saved searches stay when not chosen");

    // ---- backup and restore ----
    const auto backups = ListBackups(target.wstring());
    Check(backups.size() == 1 && backups[0].stamp == L"20261021-120000" &&
          DisplayTime(backups[0].stamp) == L"2026-10-21 12:00",
          L"import left one backup");
    Check(!backups.empty() && Read(backups[0].dir, L"app.json") == before_app &&
          Has(backups[0].dir, L"tag_rules.json"),
          L"backup holds the files from before the import");
    Check(!backups.empty() && StageRestore(target.wstring(), backups[0].dir), L"restore stages");
    const ApplyResult restored = ApplyPendingImport(target.wstring(), L"20261021-130000");
    Check(restored.outcome == ApplyOutcome::Restored, L"restore applies");
    Check(Member(Read(target, L"app.json"), L"theme_mode") == L"1" &&
          !Contains(Read(target, L"app.json"), L"copy_path"),
          L"restored preferences are the old ones");
    Check(!Has(target, L"context_menu.json"), L"files absent before the import are removed again");
    Check(Has(target, L"tag_rules.json") && Contains(Read(target, L"places.json"), L"\"旧\""),
          L"tag rules and old tags come back");
    Check(ListBackups(target.wstring()).size() == 2, L"a restore also backs up the settings it replaces");

    // ---- retention ----
    for (int i = 0; i < 7; ++i) {
        StageImport(target.wstring(), bundle, kAliases);
        wchar_t stamp[32]{};
        swprintf_s(stamp, L"20261022-1000%02d", i);
        ApplyPendingImport(target.wstring(), stamp);
    }
    const auto kept = ListBackups(target.wstring());
    Check(kept.size() == kBackupsKept && kept[0].stamp == L"20261022-100006",
          L"only the newest backups are kept");

    // ---- guards ----
    fs::create_directories(target / L"settings_import");
    Write(target / L"settings_import", L"evil.json", L"{}");
    Write(target / L"settings_import", L"apply.list", L"mode import\nput ..\\evil.json\nput session.json\n");
    const ApplyResult guarded = ApplyPendingImport(target.wstring(), L"20261023-000000");
    Check(guarded.outcome == ApplyOutcome::Failed && !Has(target, L"evil.json") &&
          !Has(target, L"session.json") && !HasPendingImport(target.wstring()),
          L"a list naming other files applies nothing and is dropped");
    Check(ApplyPendingImport(target.wstring(), L"x").outcome == ApplyOutcome::Nothing,
          L"no stage, nothing to do");
    StageImport(target.wstring(), bundle, kAliases);
    CancelPendingImport(target.wstring());
    Check(!HasPendingImport(target.wstring()), L"a staged import can be cancelled");
    Check(!StageImport(target.wstring(), bundle, 0) && !HasPendingImport(target.wstring()),
          L"nothing chosen, nothing staged");

    // ---- merge helpers ----
    Check(MergePrefs(L"{\"a\":1,\"launch_on_startup\":true}", L"{\"a\":2,\"launch_on_startup\":false,\"b\":3}") ==
          L"{\n  \"a\":2,\n  \"launch_on_startup\":true,\n  \"b\":3\n}\n",
          L"MergePrefs overlays choices and keeps this PC's members");
    Check(MergePrefs(L"garbage", L"{\"a\":1}") == L"{\n  \"a\":1\n}\n", L"MergePrefs survives an unreadable local file");
    Check(MergePrefs(L"{}", L"garbage").empty(), L"MergePrefs refuses an unreadable import");
    Check(RestorePrefs(L"{\"a\":9,\"shell_tag_menu\":true}", L"{\"a\":1,\"shell_tag_menu\":false}") ==
          L"{\n  \"a\":1,\n  \"shell_tag_menu\":true\n}\n",
          L"RestorePrefs brings choices back and keeps this PC's members");
    Check(DisplayTime(L"20261021-234005-2") == L"2026-10-21 23:40", L"display time of a suffixed backup");

    // Local settings are more extensive than the upstream whitelist. Their
    // portable members travel while old bundles leave destination additions.
    SeedTarget(target);
    Write(source, L"preferences.json", L"{\"version\":8,\"left\":900,\"tray\":[],\"undo\":[],\"sidebarHidden\":5,\"detailsPanelWidth\":410}");
    Write(target, L"preferences.json", L"{\"version\":8,\"left\":40,\"tray\":[\"local\"],\"undo\":[],\"sidebarHidden\":1,\"detailsPanelWidth\":340}");
    Write(source, L"network-index.json", L"{\"version\":1,\"roots\":[\"\\\\\\\\server\\\\share\"]}");
    Write(target, L"network-index.json", L"{\"version\":1,\"roots\":[]}");
    Write(target, L"context_menu.json", L"{\"builtin_hidden\":0,\"local_only_context\":true}");
    pulse::ipc::PayloadWriter config_bytes;
    config_bytes.PutU32(0x58494350);
    config_bytes.PutU32(1);
    pulse::index::ContentIndexConfig content_config;
    content_config.excluded_directories = {L"node_modules", L"本地目录"};
    pulse::index::content::PutConfig(config_bytes, content_config);
    const auto binary_path = source / L"content-index.config";
    HANDLE binary_file = CreateFileW(binary_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD bytes_written = 0;
    Check(binary_file != INVALID_HANDLE_VALUE && WriteFile(binary_file, config_bytes.data().data(),
        static_cast<DWORD>(config_bytes.data().size()), &bytes_written, nullptr), L"binary config fixture writes");
    if (binary_file != INVALID_HANDLE_VALUE) CloseHandle(binary_file);
    Check(BuildBundle(source.wstring(), kAllCategories, {L"local", L"x"}, bundle_text) &&
        ParseBundle(bundle_text, bundle) == ParseResult::Ok && bundle.File(L"preferences.json") &&
        bundle.File(L"network-index.json") && bundle.File(L"content-index.config"),
        L"local window, network and binary content configurations export");
    Check(!Member(*bundle.File(L"preferences.json"), L"left").size() &&
        Member(*bundle.File(L"preferences.json"), L"tray").empty(), L"window geometry and tray never travel");
    Check(StageImport(target.wstring(), bundle, kAllCategories) &&
        ApplyPendingImport(target.wstring(), L"20261024-000000").outcome == ApplyOutcome::Imported,
        L"local settings apply after staging");
    Check(Member(Read(target, L"preferences.json"), L"left") == L"40" &&
        Member(Read(target, L"preferences.json"), L"sidebarHidden") == L"5" &&
        Contains(Read(target, L"preferences.json"), L"local"), L"portable window choices merge without replacing local window state");
    Check(Contains(Read(target, L"context_menu.json"), L"local_only_context"),
        L"context menu import preserves settings introduced only by this checkout");
    std::vector<uint8_t> binary_result(config_bytes.data().size());
    binary_file = CreateFileW((target / L"content-index.config").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD bytes_read = 0;
    const bool binary_read = binary_file != INVALID_HANDLE_VALUE && ReadFile(binary_file, binary_result.data(),
        static_cast<DWORD>(binary_result.size()), &bytes_read, nullptr);
    if (binary_file != INVALID_HANDLE_VALUE) CloseHandle(binary_file);
    Check(binary_read && binary_result == config_bytes.data(), L"binary config round trips byte for byte");
    const std::wstring local_preferences = Read(target, L"preferences.json");
    Bundle old_bundle;
    Check(ParseBundle(prefs_only, old_bundle) == ParseResult::Ok &&
        StageImport(target.wstring(), old_bundle, kPrefs) &&
        ApplyPendingImport(target.wstring(), L"20261024-010000").outcome == ApplyOutcome::Imported &&
        Read(target, L"preferences.json") == local_preferences && Has(target, L"content-index.config"),
        L"older bundles preserve local extensions they do not carry");

    // A late replacement failure must undo earlier atomic writes.
    SeedTarget(target);
    const auto rollback_before = Read(target, L"app.json");
    Check(StageImport(target.wstring(), bundle, kPrefs), L"failure fixture stages");
    HANDLE locked = CreateFileW((target / L"tags.json").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    // Hold a later selected file without FILE_SHARE_DELETE: backups can read it,
    // but atomic replacement fails after app.json has already changed.
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    Write(target, L"context_menu.json", L"{\"builtin_hidden\":0}");
    locked = CreateFileW((target / L"context_menu.json").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const auto rolled_back = ApplyPendingImport(target.wstring(), L"20261024-020000");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    Check(locked != INVALID_HANDLE_VALUE && rolled_back.outcome == ApplyOutcome::Failed &&
        rolled_back.rollback_ok && rolled_back.changed == 0 && Read(target, L"app.json") == rollback_before,
        L"late file replacement failure rolls earlier settings back");

    SeedTarget(target);
    StageImport(target.wstring(), bundle, kPrefs);
    Write(target / L"settings_import", L"context_menu.json", L"{broken");
    const auto malformed_stage = ApplyPendingImport(target.wstring(), L"20261024-030000");
    Check(malformed_stage.outcome == ApplyOutcome::Failed &&
        Read(target, L"app.json") == rollback_before && ListBackups(target.wstring()).empty(),
        L"preflight rejects corrupt later payload before changing any settings");
    Check(!StageRestore(target.wstring(), source.wstring()), L"restore rejects directories outside committed automatic backups");
    Check(ParseBundle(L"{\"format\":\"pulse-settings\",\"format_version\":1,\"files\":{\"app.json\":{},\"app.json\":{}}}",
        rejected) == ParseResult::Invalid, L"duplicate bundled file names are rejected");
    Check(IsMachineLocalPref(L"experimental_explorer_takeover"), L"local Explorer takeover integration stays on this PC");

    // Export uses immutable UI snapshots, including a change whose disk write
    // is still queued, without accessing a live model on its worker thread.
    const std::vector<std::pair<std::wstring, std::wstring>> snapshots = {
        {L"app.json", L"{\"text_compare_enabled\":true,\"text_render\":2}"}
    };
    Check(BuildBundle(source.wstring(), kPrefs, {L"local", L"x"}, bundle_text, nullptr, &snapshots) &&
        Contains(bundle_text, L"text_compare_enabled") && Contains(bundle_text, L"text_render"),
        L"export captures unsaved local settings from an immutable snapshot");

    fs::remove_all(root);
    wprintf(L"%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

inline int RunPlacesTagLoadingCases() {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        failures += !ok;
    };
    const auto parent = std::filesystem::current_path() / L"bench_data";
    const auto root = parent / (L"places-tags-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    storage::OverrideDefaultRootForTesting(root.wstring());
    const auto tag_path = (root / L"tags.json").wstring();
    const std::wstring legacy = LR"({"tags":[{"id":"legacy","name":"legacy","paths":[]}]})";
    check(WriteUtf8FileAtomic((root / L"places.json").wstring(), legacy), "prepare isolated legacy Places fallback");
    const std::wstring empty = LR"({"tags":[]})";
    check(WriteUtf8FileAtomic(tag_path, empty), "write authoritative empty tag list");
    {
        model::PlacesCatalog catalog;
        check(catalog.Load() && catalog.tags.empty(), "empty tag file overrides legacy tags without inserting defaults");
    }
    std::wstring text;
    check(ReadUtf8File(tag_path, text) && text == empty, "loading empty list does not rewrite tag file");
    check(WriteUtf8FileAtomic(tag_path, legacy), "prepare final-tag deletion fixture");
    {
        model::PlacesCatalog catalog;
        const bool loaded = catalog.Load();
        catalog.DeleteTag(L"legacy");
        check(loaded && catalog.tags.empty(), "last tag can be deleted through production catalog");
    }
    {
        model::PlacesCatalog restarted;
        check(restarted.Load() && restarted.tags.empty(), "last deleted tag stays deleted after writer flush and restart");
    }
    const std::wstring damaged[] = {
        L"", LR"({"tags":[)", LR"({"tags":{}})", LR"({})", LR"({"tags":[1]})",
        LR"({"tags":[{"id":"x","name":"x","paths":[1]}]})",
        LR"({"tags":[{"id":"x","name":"x"},{"id":"x","name":"other"}]})",
        LR"({"tags":[{"id":"x","name":"x","rgb":"red"}]})",
        LR"({"tags":[{"id":"x","name":"x","rgb":-1}]})",
        LR"({"tags":[{"id":"x","name":"x","rgb":999999999999999999999}]})"
    };
    bool rejected = true, preserved = true;
    for (const auto& bytes : damaged) {
        check(WriteUtf8FileAtomic(tag_path, bytes), "prepare damaged tag input");
        {
            model::PlacesCatalog catalog;
            rejected &= !catalog.Load();
            catalog.CreateTag(L"unsaved after error", 0x123456);
        }
        preserved &= ReadUtf8File(tag_path, text) && text == bytes;
    }
    check(rejected, "malformed, absent-schema, non-array and invalid-member tag documents reject load");
    check(preserved, "invalid existing tag files survive loading and later catalog mutations byte-for-byte");
    check(WriteUtf8FileAtomic(tag_path, legacy), "prepare read sharing failure fixture");
    HANDLE locked = CreateFileW(tag_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "hold isolated tag file without read sharing");
    if (locked != INVALID_HANDLE_VALUE) {
        {
            model::PlacesCatalog catalog;
            check(!catalog.Load(), "tag read failure remains a failed load despite valid legacy Places");
            catalog.CreateTag(L"unsaved after I/O error", 0x123456);
        }
        CloseHandle(locked);
        check(ReadUtf8File(tag_path, text) && text == legacy, "tag read failure does not queue fallback overwrite");
    }
    check(DeleteFileW(tag_path.c_str()) != FALSE, "remove owned tag file to test first migration");
    {
        model::PlacesCatalog catalog;
        check(catalog.Load() && catalog.tags.size() == 1 && catalog.tags[0].id == L"legacy",
            "missing tag file still migrates legacy Places tags");
    }
    check(ReadUtf8File(tag_path, text) && json::Document(text).Is(json::Kind::Object),
        "missing-file migration writes standard tag document");
    storage::OverrideDefaultRootForTesting(L"");
    std::error_code error;
    const auto actual = std::filesystem::weakly_canonical(root, error);
    const auto allowed = std::filesystem::weakly_canonical(parent, error);
    if (!error && actual.parent_path() == allowed && actual.filename().wstring().starts_with(L"places-tags-"))
        std::filesystem::remove_all(actual, error);
    else check(false, "cleanup stays in isolated bench_data directory");
    check(!error, "isolated tag fixture cleaned without user settings");
    return failures ? 1 : 0;
}

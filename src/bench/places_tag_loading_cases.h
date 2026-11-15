// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../model/tag_id.h"

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
    const auto tagged_path = (root / L"tagged.txt").wstring();
    const auto other_path = (root / L"other.txt").wstring();
    check(WriteUtf8FileAtomic(tagged_path, L"owned ADS fixture"), "prepare isolated tagged file");
    std::wstring escaped_path;
    json::Escape(tagged_path, escaped_path);
    const std::wstring legacy = L"{\"tags\":[{\"id\":\"legacy\",\"name\":\"legacy\",\"paths\":[\"" +
        escaped_path + L"\"]}]}";
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
    const std::vector<model::TagAdsRecord> original_ads = {{L"legacy", L"legacy", 0x123456}};
    check(model::WriteTagAdsV2(tagged_path, original_ads), "prepare original ADS membership");
    bool rejected = true, preserved = true, read_only = true, ads_preserved = true;
    for (const auto& bytes : damaged) {
        check(WriteUtf8FileAtomic(tag_path, bytes), "prepare damaged tag input");
        {
            model::PlacesCatalog catalog;
            rejected &= !catalog.Load();
            const auto snapshot = catalog.ToJson();
            const auto revision = catalog.TagRevision();
            std::vector<model::TagAdsUpdate> updates;
            read_only &= !catalog.CanEditTags() && catalog.CreateTag(L"unsaved after error", 0x123456).empty();
            read_only &= !catalog.RenameTag(L"legacy", L"renamed") && !catalog.SetTagColor(L"legacy", 0x654321);
            read_only &= !catalog.SetTagged(0, tagged_path, false) && !catalog.SetTagged(0, other_path, true);
            read_only &= !catalog.SetTaggedBatch(0, {tagged_path}, false, &updates);
            read_only &= !catalog.ToggleTagBatch(0, {tagged_path}, &updates);
            read_only &= !catalog.SetTagsBatch(L"legacy", {other_path}, true, &updates);
            read_only &= catalog.DeleteTag(L"legacy", &updates) == 0;
            catalog.MergeAdsRecords(other_path, {{L"imported", L"imported", 0x654321}});
            catalog.MergeAdsRecords(other_path, {}, {L"legacy"});
            catalog.ReadAdsIntoCatalog(tagged_path);
            catalog.RemapPaths(tagged_path, other_path);
            catalog.CloneAssignments(tagged_path, other_path);
            catalog.RemoveAssignments(tagged_path, true);
            catalog.TagsReordered();
            read_only &= updates.empty() && catalog.ToJson() == snapshot && catalog.TagRevision() == revision;
            const auto ads = model::ReadTagAdsV2(tagged_path);
            ads_preserved &= ads.size() == 1 && ads[0].id == L"legacy" &&
                ads[0].name == L"legacy" && ads[0].rgb == 0x123456;
        }
        preserved &= ReadUtf8File(tag_path, text) && text == bytes;
    }
    check(rejected, "malformed, absent-schema, non-array and invalid-member tag documents reject load");
    check(preserved, "invalid existing tag files survive loading and later catalog mutations byte-for-byte");
    check(read_only, "damaged tags reject every mutation, ADS import and path synchronization without changing memory or revision");
    check(ads_preserved, "rejected tag mutations neither alter ADS nor emit deferred ADS writes");
    check(WriteUtf8FileAtomic(tag_path, legacy), "prepare read sharing failure fixture");
    HANDLE locked = CreateFileW(tag_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "hold isolated tag file without read sharing");
    if (locked != INVALID_HANDLE_VALUE) {
        {
            model::PlacesCatalog catalog;
            check(!catalog.Load(), "tag read failure remains a failed load despite valid legacy Places");
            check(!catalog.CanEditTags() && catalog.CreateTag(L"unsaved after I/O error", 0x123456).empty(),
                "unreadable tag file also makes tag edits fail explicitly");
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
    model::TagId edited_id;
    {
        model::PlacesCatalog catalog;
        const bool loaded = catalog.Load();
        edited_id = catalog.CreateTag(L"created", 0x123456);
        std::vector<model::TagAdsUpdate> updates;
        check(loaded && catalog.CanEditTags() && !edited_id.empty() &&
            catalog.RenameTag(edited_id, L"renamed") && catalog.SetTagColor(edited_id, 0x654321) &&
            catalog.SetTagsBatch(edited_id, {tagged_path}, true, &updates) && updates.size() == 1,
            "valid tag data remains editable and emits assignment updates");
    }
    {
        model::PlacesCatalog restarted;
        const bool loaded = restarted.Load();
        const auto* tag = restarted.FindTag(edited_id);
        const auto expected_path = fs::NormalizePath(tagged_path);
        const bool persisted = loaded && tag && tag->name == L"renamed" && tag->rgb == 0x654321 &&
            tag->paths == std::vector<std::wstring>{expected_path};
        if (!persisted) {
            std::wcout << L"[INFO] restarted tags: loaded=" << loaded << L" found=" << (tag != nullptr)
                << L" expected path=" << expected_path << L'\n';
            if (tag) {
                std::wcout << L"[INFO] name=" << tag->name << L" rgb=" << tag->rgb << L'\n';
                for (const auto& path : tag->paths) std::wcout << L"[INFO] actual path=" << path << L'\n';
            }
        }
        check(persisted,
            "successful tag edits persist unchanged across restart");
    }
    bool guid_failure_rejected = true;
    for (int i = 0; i < 64; ++i)
        guid_failure_rejected &= model::detail::NewTagId(+[](GUID*) -> HRESULT { return E_FAIL; }).empty();
    check(guid_failure_rejected, "repeated GUID failures return no ID instead of colliding tick-count IDs");
    const auto first_id = model::detail::NewTagId();
    const auto second_id = model::detail::NewTagId();
    check(first_id.size() == 36 && second_id.size() == 36 && first_id != second_id,
        "successful GUID generation retains unique normalized tag IDs");
    const auto places_path = (root / L"places.json").wstring();
    const std::wstring saved_places = LR"({"quick_access_paths":["C:\\saved"],"workspaces":[]})";
    check(WriteUtf8FileAtomic(places_path, saved_places), "prepare existing Places document");
    locked = CreateFileW(places_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "hold Places file without read sharing");
    if (locked != INVALID_HANDLE_VALUE) {
        model::PlacesCatalog catalog;
        check(!catalog.Load(), "Places read failure is not hidden by a valid tag file");
        CloseHandle(locked);
        catalog.RecordRecent(tagged_path, model::PlaceItemKind::File);
        check(!catalog.Save(), "shutdown save rejects the fallback after a Places read failure");
        catalog.FlushPendingSave(true);
    }
    check(ReadUtf8File(places_path, text) && text == saved_places,
        "unreadable Places survives unlock, catalog mutations and destruction");
    for (const auto& damaged_places : {std::wstring{}, std::wstring(L"{\"workspaces\":["), std::wstring(L"[]")}) {
        check(WriteUtf8FileAtomic(places_path, damaged_places), "prepare malformed Places document");
        {
            model::PlacesCatalog catalog;
            check(!catalog.Load() && !catalog.Save(), "invalid Places cannot be overwritten by fallback data");
            catalog.RecordRecent(tagged_path, model::PlaceItemKind::File);
            catalog.FlushPendingSave();
        }
        check(ReadUtf8File(places_path, text) && text == damaged_places,
            "malformed Places remains byte-for-byte intact");
    }
    check(DeleteFileW(places_path.c_str()) != FALSE, "remove owned Places to test first startup");
    {
        model::PlacesCatalog catalog;
        check(catalog.Load() && catalog.Save(), "missing Places permits initial creation with existing tags");
    }
    {
        model::PlacesCatalog catalog;
        check(catalog.Load(), "newly created Places loads normally");
        catalog.RecordRecent(tagged_path, model::PlaceItemKind::File);
        check(catalog.Save(), "successful Places load retains normal saving");
    }
    {
        model::PlacesCatalog catalog;
        check(catalog.Load() && catalog.FindRecent(tagged_path),
            "normal Places mutations survive restart after initial creation");
    }
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

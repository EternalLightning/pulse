// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

int CacheResolutionTests() {
    namespace fs = std::filesystem;
    const auto root = fs::absolute(fs::path(L"bench_data") /
        (L"cache-resolution-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64())));
    fs::create_directories(root);
    SetMachineIndexScope(true);
    const auto setup = [&](const wchar_t* name, ShardPaths& paths, ShardManifest& manifest) {
        const auto directory = root / name;
        fs::create_directory(directory);
        SetActiveIndexDirectory(directory.wstring());
        paths = MakeShardPaths((directory / L"v9").wstring(), L"aggregate");
        Engine source;
        if (!EngineTestAccess::Build(source) || !EngineTestAccess::Save(source, paths.base_a)) return false;
        manifest.source_id = L"aggregate";
        manifest.active_built = 123456;
        manifest.active_bytes = fs::file_size(paths.base_a);
        manifest.active_crc64 = FileCrc64(paths.base_a);
        return manifest.active_crc64 && SaveShardManifest(paths.manifest, manifest);
    };
    {
        ShardPaths paths;
        ShardManifest manifest;
        const bool ready = setup(L"crc", paths, manifest);
        Check(ready, "prepare structurally valid cache with manifest checksum");
        if (ready) {
            ++manifest.active_crc64;
            const bool saved = SaveShardManifest(paths.manifest, manifest);
            Engine cache;
            Check(saved && !EngineTestAccess::LoadCache(cache),
                  "cache loader cannot bypass rejected checksum by opening base A directly");
        }
    }
    {
        ShardPaths paths;
        ShardManifest manifest;
        const bool ready = setup(L"manifest-lock", paths, manifest);
        Check(ready, "prepare isolated cache for manifest sharing failure");
        if (ready) {
            HANDLE held = CreateFileW(paths.manifest.c_str(), GENERIC_READ, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Check(held != INVALID_HANDLE_VALUE, "hold manifest without read sharing");
            if (held != INVALID_HANDLE_VALUE) {
                Engine cache;
                Check(!EngineTestAccess::LoadCache(cache),
                      "unreadable manifest does not permit unchecked cache adoption");
                CloseHandle(held);
            }
            Engine cache;
            Check(EngineTestAccess::LoadCache(cache), "cache loads normally after manifest unlock");
        }
    }
    {
        const auto directory = root / L"legacy";
        fs::create_directory(directory);
        SetActiveIndexDirectory(directory.wstring());
        Engine source;
        const bool ready = EngineTestAccess::Build(source) && EngineTestAccess::Save(source, CacheFilePath());
        Check(ready, "prepare legacy cache without shard manifest");
        if (ready) {
            Engine cache;
            Check(EngineTestAccess::LoadCache(cache), "missing shard still imports validated legacy cache");
            ShardManifest manifest;
            const auto paths = MakeShardPaths((directory / L"v9").wstring(), L"aggregate");
            Check(LoadShardManifest(paths.manifest, manifest) && !manifest.source_id.empty() &&
                  manifest.active_crc64 != 0, "legacy import publishes a checksummed shard manifest");
        }
    }
    {
        ShardPaths paths;
        ShardManifest manifest;
        const bool ready = setup(L"rebuild", paths, manifest);
        Check(ready, "prepare real snapshot for double-slot rebuild");
        if (ready) {
            std::ofstream(fs::path(paths.base_a), std::ios::binary | std::ios::trunc) << "bad-a";
            std::ofstream(fs::path(paths.base_b), std::ios::binary | std::ios::trunc) << "bad-b";
            Engine replacement;
            const bool staged = EngineTestAccess::Build(replacement) &&
                EngineTestAccess::Save(replacement, paths.base_a + L".tmp");
            Check(staged && EngineTestAccess::CommitCache(replacement, paths.base_a),
                  "engine publishes mapped replacement when both old slots are confirmed invalid");
            Engine reloaded;
            Check(EngineTestAccess::LoadCache(reloaded), "rebuilt machine cache reloads through manifest validation");
        }
    }
    SetMachineIndexScope(false);
    SetActiveIndexDirectory(L"");
    std::wcout << L"[INFO] retained isolated cache fixtures: " << root.wstring() << L'\n';
    return failures ? 1 : 0;
}

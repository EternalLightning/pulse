#include "../index/network_index.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <iostream>

int main() {
    using namespace pulse::index;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"network-config-reload-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root);
    testing::NetworkIndexHooks hooks;
    hooks.config_path = (root / L"network-index.json").wstring();
    hooks.shard_base = (root / L"shards").wstring();
    testing::SetNetworkIndexHooks(&hooks);
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
        if (!ok) ++failures;
    };
    {
        NetworkIndex index;
        check(SaveNetworkRootsFile(hooks.config_path, {L"\\\\fixture.invalid\\before"}), "prepare an isolated network configuration");
        index.RefreshConfiguration();
        auto roots = index.Roots();
        check(roots.size() == 1 && roots.front().path == NormalizeNetworkRoot(L"\\\\fixture.invalid\\before"),
            "a live agent can load configured roots without starting network I/O");
        SaveNetworkRootsFile(hooks.config_path, {L"\\\\fixture.invalid\\imported", L"\\\\fixture.invalid\\second"});
        std::wstring imported, unchanged;
        pulse::ReadUtf8File(hooks.config_path, imported);
        index.RefreshConfiguration();
        roots = index.Roots();
        pulse::ReadUtf8File(hooks.config_path, unchanged);
        check(roots.size() == 2 && roots.front().path == NormalizeNetworkRoot(L"\\\\fixture.invalid\\imported") &&
            unchanged == imported, "imported roots replace the agent cache without rewriting the bundle's configuration");
        pulse::WriteUtf8FileAtomic(hooks.config_path, L"{broken");
        index.RefreshConfiguration();
        check(index.Roots().size() == 2 && !index.ConfigurationError().empty(),
            "a corrupt external configuration preserves the last working roots and reports an error");
        pulse::WriteUtf8FileAtomic(hooks.config_path, L"{\"version\":1,\"roots\":[]}");
        index.RefreshConfiguration();
        check(index.Roots().empty() && index.ConfigurationError().empty(),
            "restoring an empty root list clears cached roots and the earlier error");
        DeleteFileW(hooks.config_path.c_str());
        index.RefreshConfiguration();
        check(index.Roots().empty(), "restoring an originally absent configuration leaves no stale roots");
    }
    testing::SetNetworkIndexHooks(nullptr);
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}

#include "../app/server_address.h"
#include "../app/network_recent.h"
#include "../app/network_recent_store.h"
#include "../app/network_locations.h"
#include "../app/lan_device.h"
#include "../app/lan_discovery.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <iostream>
#include <shlobj.h>
#include <wrl/client.h>

int wmain() {
    using namespace pulse::app;
    int failures = 0;
    const auto check = [&](bool okay, const char* name) {
        std::cout << (okay ? "[PASS] " : "[FAIL] ") << name << std::endl;
        if (!okay) ++failures;
    };
    check(ParseServerAddress(L"nas\\share", true).unc == L"\\\\nas\\share" &&
        ParseServerAddress(L"nas\\share").kind == ServerAddressKind::Invalid,
        "bare names are accepted only by the connect dialog");
    const auto smb = ParseServerAddress(L"smb://user:secret@nas/Shared%20Docs/child", true);
    check(smb.kind == ServerAddressKind::Share && smb.unc == L"\\\\nas\\Shared Docs\\child" && smb.user == L"user",
        "SMB URL becomes a UNC path and never retains the password");
    check(ParseServerAddress(L"192.168.1.20/share").unc == L"\\\\192.168.1.20\\share" &&
        ParseServerAddress(L"//nas/share").unc == L"\\\\nas\\share" &&
        ParseServerAddress(L"\\\\?\\UNC\\nas\\share").unc == L"\\\\nas\\share",
        "IP, forward slashes and extended UNC normalize to navigable locations");
    check(ParseServerAddress(L"ftp://nas/share", true).kind == ServerAddressKind::Unsupported &&
        ParseServerAddress(L"C:\\folder", true).kind == ServerAddressKind::Invalid &&
        ParseServerAddress(L"\\\\nas\\..\\secret").kind == ServerAddressKind::Invalid &&
        ParseServerAddress(L"smb://nas/share%00evil").kind == ServerAddressKind::Invalid,
        "unsupported protocols, local drives, traversal and encoded controls cannot connect");
    check(NetworkRecentRoot(L"\\\\?\\UNC\\NAS\\share\\child") == L"\\\\NAS\\share" &&
        NetworkRecentRoot(L"\\\\NAS\\") == L"\\\\NAS" && NetworkRecentRoot(L"C:\\x").empty(),
        "history stores server/share roots and rejects local paths");

    NetworkRecentList history;
    check(history.Touch(L"\\\\nas\\share\\child", 1) && history.Touch(L"\\\\NAS\\SHARE", 2) &&
        history.Items().size() == 1 && history.Items().front().last_visit == 2,
        "repeat visits deduplicate case-insensitively and update visit time");
    for (int i = 0; i < 25; ++i) history.Touch(L"\\\\nas\\share" + std::to_wstring(i), 3 + i);
    NetworkRecentList restored;
    check(history.Items().size() == 20 && restored.FromJson(history.ToJson()) &&
        restored.Items().size() == 20 && restored.Items().front().path == L"\\\\nas\\share24",
        "history is bounded and serializes newest first");
    const auto valid = restored.ToJson();
    check(!restored.FromJson(L"{\"items\":[") && restored.ToJson() == valid,
        "malformed history cannot replace the current snapshot");
    check(restored.FromJson(LR"({"version":1,"items":[{"path":"C:\\x"},{"path":"\\\\nas\\share","t":"-1"}]})") &&
        restored.Items().empty(), "invalid local paths and malformed timestamps are skipped");

    std::vector<LanDevice> devices;
    MergeLanDevice(devices, {L"NAS", L"nas.local", L"", L"DS920+", kLanSourceMdns});
    MergeLanDevice(devices, {L"NAS", L"NAS", L"192.168.1.2", L"", kLanSourceWsd});
    check(devices.size() == 1 && devices.front().sources == (kLanSourceMdns | kLanSourceWsd) &&
        devices.front().ip == L"192.168.1.2" && LanDeviceKind(devices.front()) == kLanKindNas,
        "mDNS and WSD answers merge without losing NAS metadata");
    check(MdnsInstanceName(L"Mac\\032mini._smb._tcp.local") == L"Mac mini" &&
        LanDeviceUnc({L"bad", L"host\\share", L"192.168.1.4", L"", 0}) == L"\\\\192.168.1.4",
        "device labels decode DNS escapes and untrusted host paths cannot become UNC navigation");
    check(RunLanDiscovery([] { return true; }, 0).cancelled,
        "canceled discovery exits before reading network identity or starting protocols");

    const std::filesystem::path root = std::filesystem::current_path() / L"bench_data" /
        (L"network-locations-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(root, error);
    check(!error, "isolated local fixture is available");
    if (error) return 1;
    const auto recent_file = (root / L"network_recent.json").wstring();
    {
        NetworkRecentStore store(true, recent_file);
        store.Touch(L"\\\\nas\\first\\child", 1);
        store.Touch(L"\\\\nas\\second", 2);
        store.Touch(L"\\\\NAS\\FIRST", 3);
        store.Flush();
        const auto snapshot = store.Snapshot();
        check(snapshot.Items().size() == 2 && snapshot.Items().front().path == L"\\\\NAS\\FIRST",
            "serial history worker preserves visits from multiple windows in order");
    }
    {
        NetworkRecentStore store(true, recent_file);
        store.Flush();
        check(store.Snapshot().Items().size() == 2, "recent visits survive closing and reopening the store");
    }
    {
        NetworkRecentStore store(false, (root / L"must-not-exist.json").wstring());
        store.Touch(L"\\\\nas", 4);
        store.Flush();
        check(!std::filesystem::exists(root / L"must-not-exist.json"), "isolated mode never writes user history");
    }
    const auto hood = root / L"NetHood";
    std::filesystem::create_directory(hood, error);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    bool shortcut = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->SetPath(L"\\\\fixture.invalid\\share")) && SUCCEEDED(link.As(&persist)) &&
        SUCCEEDED(persist->Save((hood / L"Fixture NAS.lnk").c_str(), TRUE));
    auto scan = ReadNetworkLocations(hood.wstring());
    check(shortcut && !scan.error && scan.items.size() == 1 && !scan.items.front().shell_link &&
        scan.items.front().path == L"\\\\fixture.invalid\\share",
        "local NetHood metadata imports UNC shortcuts without resolving their hosts");
    check(ReadNetworkLocations(hood.wstring(), [] { return true; }).cancelled &&
        ReadNetworkLocations(L"\\\\fixture.invalid\\share").error == ERROR_BAD_PATHNAME,
        "canceled metadata reads and UNC fixture roots never probe a server");
    const auto merged = MergeNetworkLocations({{L"pin", L"\\\\NAS\\share", false}},
        {{L"system", L"\\\\?\\UNC\\nas\\share\\", false}});
    check(merged.size() == 1 && merged.front().name == L"pin", "Pulse pins win over duplicate Windows network locations");
    persist.Reset();
    link.Reset();
    if (SUCCEEDED(initialized)) CoUninitialize();
    std::filesystem::remove_all(root, error);
    check(!error, "isolated network fixtures are cleaned up");
    return failures ? 1 : 0;
}

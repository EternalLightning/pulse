#include "src/fs/device_source.h"
#include "src/ipc/shell_client.h"
#include "src/common/shell_id.h"
#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <iterator>
#include <clocale>

using Microsoft::WRL::ComPtr;
using namespace pulse;

namespace {
void Require(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    std::fflush(stdout);
    if (!ok) throw std::runtime_error(message);
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Sleep(20);
}
std::vector<fs::DirEntry> List(fs::DeviceSource& source, const fs::DevicePath& path) {
    std::vector<fs::DirEntry> rows;
    std::atomic_bool cancelled{false};
    const auto result = source.Enumerate(path, [&](std::vector<fs::DirEntry>&& batch) {
        for (auto& row : batch) rows.push_back(std::move(row));
        return true;
    }, cancelled);
    if (result != fs::DeviceError::None) throw std::runtime_error("device enumeration failed");
    return rows;
}
bool Exists(fs::DeviceSource& source, const fs::DevicePath& path, const std::wstring& name) {
    for (const auto& row : List(source, path)) if (row.name == name) return true;
    return false;
}
bool AwaitExists(fs::DeviceSource& source, const fs::DevicePath& path, const std::wstring& name) {
    const auto deadline = GetTickCount64() + 20000;
    do {
        if (Exists(source, path, name)) return true;
        Pump();
    } while (GetTickCount64() < deadline);
    return false;
}
ComPtr<IShellItem> Item(fs::DeviceSource& source, const fs::DevicePath& path) {
    ComPtr<IShellItem> item;
    if (FAILED(source.ShellItem(path, &item))) throw std::runtime_error("resolve device item failed");
    return item;
}
HRESULT NativeMutation(IShellItem* item, const wchar_t* rename) {
    ComPtr<IFileOperation> operation;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
    if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMMKDIR);
    if (SUCCEEDED(hr)) hr = rename ? operation->RenameItem(item, rename, nullptr) : operation->DeleteItem(item, nullptr);
    if (SUCCEEDED(hr)) hr = operation->PerformOperations();
    BOOL aborted = FALSE;
    if (SUCCEEDED(hr)) hr = operation->GetAnyOperationsAborted(&aborted);
    return SUCCEEDED(hr) && aborted ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : hr;
}
HRESULT NativeCopy(IShellItem* item, IShellItem* destination) {
    ComPtr<IFileOperation> operation;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
    if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMMKDIR);
    if (SUCCEEDED(hr)) hr = operation->CopyItem(item, destination, nullptr, nullptr);
    if (SUCCEEDED(hr)) hr = operation->PerformOperations();
    BOOL aborted = FALSE;
    if (SUCCEEDED(hr)) hr = operation->GetAnyOperationsAborted(&aborted);
    std::printf("  Native copy: 0x%08lX aborted=%d\n", static_cast<unsigned long>(hr), aborted);
    return SUCCEEDED(hr) && aborted ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : hr;
}
}

int wmain(int argc, wchar_t** argv) {
    std::setlocale(LC_ALL, ".UTF8");
    if (argc != 3) return 2;
    const bool rename_only = wcscmp(argv[1], L"--run-rename-device-fixture") == 0;
    const bool native = rename_only || wcscmp(argv[1], L"--run-native-device-fixture") == 0;
    if (!native && wcscmp(argv[1], L"--run-connected-device-fixture") != 0) return 2;
    const std::wstring local_root = argv[2];
    if (FAILED(OleInitialize(nullptr))) return 2;
    auto source = fs::MakeShellDeviceSource();
    auto& client = ipc::ShellClient::Instance();
    std::mutex mutex;
    std::map<uint32_t, HRESULT> completions;
    ipc::ShellClient::Callbacks callbacks;
    callbacks.done = [&](uint32_t id, uint32_t hr, bool cancelled, std::wstring) {
        std::lock_guard lock(mutex);
        completions[id] = cancelled ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : static_cast<HRESULT>(hr);
    };
    auto owner = client.Acquire(std::move(callbacks));
    const auto wait = [&](uint32_t id) {
        if (!id) return E_FAIL;
        const auto deadline = GetTickCount64() + 30000;
        do {
            {
                std::lock_guard lock(mutex);
                if (auto found = completions.find(id); found != completions.end()) {
                    const auto hr = found->second;
                    completions.erase(found);
                    std::printf("  Shell completion: 0x%08lX\n", static_cast<unsigned long>(hr));
                    return hr;
                }
            }
            Pump();
        } while (GetTickCount64() < deadline);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    };
    const auto device_op = [&](const wchar_t* verb, const std::wstring& id, const std::wstring& name = L"") {
        HRESULT hr = E_FAIL;
        for (int attempt = 0; attempt < 3; ++attempt) {
            hr = wait(client.DeviceOperation(nullptr, verb, {id}, name, owner));
            // OleGetClipboard failed before the host can submit any mutation.
            if (hr != CLIPBRD_E_CANT_OPEN) break;
            std::printf("[INFO] clipboard unavailable before mutation; retry after pumping messages\n");
            const auto deadline = GetTickCount64() + 500;
            while (GetTickCount64() < deadline) Pump();
        }
        return hr;
    };
    const auto set_clipboard = [](IDataObject* data) {
        HRESULT hr = E_FAIL;
        const auto deadline = GetTickCount64() + 3000;
        do {
            hr = OleSetClipboard(data);
            if (hr != CLIPBRD_E_CANT_OPEN) break;
            Pump();
        } while (GetTickCount64() < deadline);
        return hr;
    };
    fs::DevicePath storage, fixture;
    ComPtr<IShellItem> fixture_item;
    ComPtr<IDataObject> saved_clipboard;
    const bool saved = !native && SUCCEEDED(OleGetClipboard(&saved_clipboard));
    bool clipboard_changed = false;
    bool created = false;
    bool extra_failure = false;
    int result = 1;
    try {
        for (const auto& device : source->ListDevices()) {
            if (device.kind != fs::DeviceKind::Android || device.locked || device.storages.empty()) continue;
            if (device.storages.front().read_only) continue;
            storage = {device.key, device.storages.front().name, {}};
            std::printf("  Device: %ls\n", device.name.c_str());
            break;
        }
        Require(!storage.device.empty(), "connected writable Android storage found");
        const auto fixture_name = L"Pulse_Test_20261029_" + std::to_wstring(GetCurrentProcessId());
        Require(!Exists(*source, storage, fixture_name), "unique fixture does not pre-exist on the phone");
        fixture = fs::DeviceChild(storage, fixture_name);
        auto storage_item = Item(*source, storage);
        Require(SUCCEEDED(device_op(L"newfolder", shell_id::FromItem(storage_item.Get()), fixture_name)),
            "Pulse Shell IPC creates the dedicated phone test folder");
        created = true;
        Require(AwaitExists(*source, storage, fixture_name),
            "created phone test folder is visible to production device enumeration");
        fixture_item = Item(*source, fixture);
        const auto fixture_id = shell_id::FromItem(fixture_item.Get());
        if (rename_only) {
            const auto renamed_name = fixture_name + L"_renamed";
            const auto rename_hr = NativeMutation(fixture_item.Get(), renamed_name.c_str());
            const bool renamed = Exists(*source, storage, renamed_name);
            const bool original = Exists(*source, storage, fixture_name);
            std::printf("[%s] native fixture folder rename; hr=0x%08lX new_exists=%d old_exists=%d\n",
                SUCCEEDED(rename_hr) && renamed && !original ? "PASS" : "FAIL",
                static_cast<unsigned long>(rename_hr), renamed, original);
            if (renamed && !original) {
                fixture = fs::DeviceChild(storage, renamed_name);
                fixture_item = Item(*source, fixture);
            }
            result = SUCCEEDED(rename_hr) && renamed && !original ? 0 : 1;
        } else {
        ComPtr<IShellItem> local_file;
        Require(SUCCEEDED(SHCreateItemFromParsingName((local_root + L"\\source\\样本.txt").c_str(),
            nullptr, IID_PPV_ARGS(&local_file))), "resolve the locally generated UTF-8 sample");
        ComPtr<IShellItemArray> array;
        ComPtr<IDataObject> data;
        if (native) {
            Require(SUCCEEDED(NativeCopy(local_file.Get(), fixture_item.Get())), "native Shell copies the sample from PC to phone");
        } else {
            Require(saved && SUCCEEDED(SHCreateShellItemArrayFromShellItem(local_file.Get(), IID_PPV_ARGS(&array))) &&
                SUCCEEDED(array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))) &&
                SUCCEEDED(set_clipboard(data.Get())), "stage the test sample and preserve the previous clipboard");
            clipboard_changed = true;
            Require(SUCCEEDED(device_op(L"paste", fixture_id)), "Pulse Shell IPC pastes the sample from PC to phone");
        }
        Require(AwaitExists(*source, fixture, L"样本.txt"), "uploaded Unicode filename appears on the phone");
        Require(SUCCEEDED(device_op(L"newfolder", fixture_id, L"子目录")) &&
            AwaitExists(*source, fixture, L"子目录"), "Pulse Shell IPC creates a Unicode subfolder");
        auto uploaded = Item(*source, fs::DeviceChild(fixture, L"样本.txt"));
        auto subfolder = Item(*source, fs::DeviceChild(fixture, L"子目录"));
        if (native) {
            const auto internal_hr = NativeCopy(uploaded.Get(), subfolder.Get());
            const bool internal_visible = Exists(*source, fs::DeviceChild(fixture, L"子目录"), L"样本.txt");
            std::printf("[%s] native phone-to-phone copy; hr=0x%08lX destination_visible=%d\n",
                SUCCEEDED(internal_hr) && internal_visible ? "PASS" : "FAIL",
                static_cast<unsigned long>(internal_hr), internal_visible);
            extra_failure |= FAILED(internal_hr) || !internal_visible;
            ComPtr<IShellItem> local_destination;
            Require(SUCCEEDED(SHCreateItemFromParsingName((local_root + L"\\download").c_str(),
                nullptr, IID_PPV_ARGS(&local_destination))) &&
                SUCCEEDED(NativeCopy(uploaded.Get(), local_destination.Get())),
                "native Shell copies the phone sample back to PC");
        } else {
            Require(SUCCEEDED(device_op(L"copy", shell_id::FromItem(uploaded.Get()))),
                "Pulse Shell IPC copies a phone file to the Shell clipboard");
            Require(SUCCEEDED(device_op(L"paste", shell_id::FromItem(subfolder.Get()))) &&
                AwaitExists(*source, fs::DeviceChild(fixture, L"子目录"), L"样本.txt"),
                "Pulse Shell IPC copies a file between phone folders");
            Require(SUCCEEDED(device_op(L"copy", shell_id::FromItem(uploaded.Get()))) &&
                SUCCEEDED(device_op(L"paste", local_root + L"\\download")),
                "Pulse Shell IPC copies the phone file back to a local folder");
        }
        const auto downloaded = local_root + L"\\download\\样本.txt";
        const auto deadline = GetTickCount64() + 20000;
        while (GetFileAttributesW(downloaded.c_str()) == INVALID_FILE_ATTRIBUTES && GetTickCount64() < deadline) Pump();
        std::ifstream original(local_root + L"\\source\\样本.txt", std::ios::binary);
        std::ifstream received(downloaded, std::ios::binary);
        const std::string first((std::istreambuf_iterator<char>(original)), {});
        const std::string second((std::istreambuf_iterator<char>(received)), {});
        Require(!first.empty() && first == second, "round-trip file contents are byte-for-byte identical");
        Require(SUCCEEDED(NativeMutation(uploaded.Get(), L"已重命名.txt")) &&
            AwaitExists(*source, fixture, L"已重命名.txt") && !Exists(*source, fixture, L"样本.txt"),
            "native Shell can rename the fixture (Pulse toolbar rename is currently disabled)");
        result = extra_failure ? 1 : 0;
        }
    } catch (const std::exception& error) {
        std::printf("[ERROR] %s\n", error.what());
    }
    if (created) {
        if (!fixture_item) {
            try { fixture_item = Item(*source, fixture); } catch (...) {}
        }
        const auto hr = fixture_item ? NativeMutation(fixture_item.Get(), nullptr) : E_FAIL;
        bool absent = false;
        try { absent = !Exists(*source, storage, fixture.parts.front()); } catch (...) {}
        std::printf("[%s] remove only the dedicated phone fixture; hr=0x%08lX\n",
            SUCCEEDED(hr) && absent ? "PASS" : "FAIL", static_cast<unsigned long>(hr));
        if (FAILED(hr) || !absent) {
            std::printf("[CLEANUP REQUIRED] %ls\n", fs::FormatDevicePath(fixture).c_str());
            result = 1;
        }
    }
    if (clipboard_changed) {
        const auto restored = set_clipboard(saved_clipboard.Get());
        std::printf("[%s] restore the previous clipboard; hr=0x%08lX\n", SUCCEEDED(restored) ? "PASS" : "FAIL",
            static_cast<unsigned long>(restored));
        if (FAILED(restored)) result = 1;
    }
    client.Release(owner);
    owner.reset();
    client.Stop();
    saved_clipboard.Reset();
    fixture_item.Reset();
    source.reset();
    OleUninitialize();
    return result;
}

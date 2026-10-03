#include "recycle_fileop.h"
#include "deletion_identity.h"
#include "../common/current_user_security.h"
#include "../common/path_utils.h"
#include <shlobj.h>
#include <sherrors.h>
#include <cstring>
#include <cwctype>
#include <memory>

namespace pulse::shell {
namespace {
template<class T> struct ComRelease { void operator()(T* value) const { if (value) value->Release(); } };
template<class T> using ComPtr = std::unique_ptr<T, ComRelease<T>>;
struct CloseHandleDeleter { void operator()(void* value) const { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
class TransferSink : public ITransferAdviseSink {
public:
    explicit TransferSink(const RecycleCancelled& cancelled) : cancelled_(cancelled) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        if (id == IID_IUnknown || id == IID_ITransferAdviseSink) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(ULONGLONG, ULONGLONG, int, int, int, int) override { return CheckCancel(); }
    HRESULT STDMETHODCALLTYPE UpdateTransferState(TRANSFER_ADVISE_STATE) override { return CheckCancel(); }
    HRESULT STDMETHODCALLTYPE ConfirmOverwrite(IShellItem*, IShellItem*, LPCWSTR) override { return COPYENGINE_E_USER_CANCELLED; }
    HRESULT STDMETHODCALLTYPE ConfirmEncryptionLoss(IShellItem*) override { return COPYENGINE_E_USER_CANCELLED; }
    HRESULT STDMETHODCALLTYPE FileFailure(IShellItem*, LPCWSTR, HRESULT error, LPWSTR, ULONG) override { return Fail(error); }
    HRESULT STDMETHODCALLTYPE SubStreamFailure(IShellItem*, LPCWSTR, HRESULT error) override { return Fail(error); }
    HRESULT STDMETHODCALLTYPE PropertyFailure(IShellItem*, const PROPERTYKEY*, HRESULT error) override { return Fail(error); }
    HRESULT Failure() const { return failure_; }
private:
    HRESULT CheckCancel() const { return cancelled_ && cancelled_() ? COPYENGINE_E_USER_CANCELLED : S_OK; }
    HRESULT Fail(HRESULT error) {
        if (SUCCEEDED(failure_)) failure_ = error;
        return error; // unhandled failure goes back to Pulse; never show/approve native UI
    }
    const RecycleCancelled& cancelled_;
    HRESULT failure_ = S_OK;
};
ComPtr<IShellItem> Item(const std::wstring& path, HRESULT& error) {
    IShellItem* raw = nullptr;
    error = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&raw));
    return ComPtr<IShellItem>(raw);
}
bool ReadIdentity(const std::wstring& path, FILE_ID_INFO& identity) {
    Handle file(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    return file.get() != INVALID_HANDLE_VALUE && file &&
        GetFileInformationByHandleEx(file.get(), FileIdInfo, &identity, sizeof(identity));
}
bool SameIdentity(const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
    return a.VolumeSerialNumber == b.VolumeSerialNumber && std::memcmp(&a.FileId, &b.FileId, sizeof(a.FileId)) == 0;
}
bool ExactOriginal(const std::wstring& payload, const std::wstring& original) {
    const auto slash = payload.find_last_of(L'\\');
    if (slash == std::wstring::npos) return false;
    auto index = payload;
    if (slash + 2 >= index.size() || index[slash + 1] != L'$' || (index[slash + 2] != L'R' && index[slash + 2] != L'r')) return false;
    index[slash + 2] = index[slash + 2] == L'R' ? L'I' : L'i';
    Handle file(CreateFileW(index.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file || file.get() == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.get(), &length) || length.QuadPart < 24 || length.QuadPart > 64 * 1024) return false;
    std::vector<BYTE> bytes(static_cast<size_t>(length.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) return false;
    uint64_t version = 0; std::memcpy(&version, bytes.data(), sizeof(version));
    const size_t offset = version == 2 ? 28 : version == 1 ? 24 : 0;
    if (!offset || bytes.size() < offset || (bytes.size() - offset) % sizeof(wchar_t)) return false;
    size_t chars = (bytes.size() - offset) / sizeof(wchar_t);
    if (version == 2) {
        DWORD declared = 0; std::memcpy(&declared, bytes.data() + 24, sizeof(declared));
        if (!declared || declared > chars) return false;
        chars = declared;
    }
    std::wstring recorded(chars, L'\0');
    std::memcpy(recorded.data(), bytes.data() + offset, chars * sizeof(wchar_t));
    while (!recorded.empty() && recorded.back() == L'\0') recorded.pop_back();
    return path::StripExtendedPathPrefix(recorded) == original;
}
bool ValidDestination(const std::wstring& original, const std::wstring& payload, const FILE_ID_INFO& source_id) {
    const auto sid = CurrentUserSidString();
    if (sid.empty() || original.size() < 3 || payload.size() < 3 || original[1] != L':' || payload[1] != L':' ||
        towupper(original[0]) != towupper(payload[0])) return false;
    const auto slash = payload.find_last_of(L'\\');
    const auto expected = payload.substr(0, 2) + L"\\$Recycle.Bin\\" + sid;
    if (slash == std::wstring::npos || !path::EqualInsensitive(payload.substr(0, slash), expected)) return false;
    FILE_ID_INFO recycled_id{};
    if (!ReadIdentity(payload, recycled_id) || !SameIdentity(source_id, recycled_id) || !ExactOriginal(payload, original)) return false;
    if (GetFileAttributesW(original.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const auto absent = GetLastError();
    return absent == ERROR_FILE_NOT_FOUND || absent == ERROR_PATH_NOT_FOUND;
}
}

RecycleResult RecycleAuthorized(const std::vector<std::wstring>& paths,
    const RecycleCancelled& cancelled, const RecycleProgress& progress) {
    RecycleResult result;
    if (paths.empty()) { result.error = E_INVALIDARG; return result; }
    for (const auto& requested : paths) {
        if (cancelled && cancelled()) { result.cancelled = true; result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED); break; }
        result.failed_path = requested;
        const auto source_path = path::StripExtendedPathPrefix(requested);
        FILE_ID_INFO source_id{};
        if (!ReadIdentity(source_path, source_id)) { result.error = E_ACCESSDENIED; break; }
        auto source = Item(source_path, result.error);
        if (!source || FAILED(result.error)) { if (SUCCEEDED(result.error)) result.error = E_UNEXPECTED; break; }
        PWSTR actual = nullptr;
        result.error = source->GetDisplayName(SIGDN_FILESYSPATH, &actual);
        const bool same = SUCCEEDED(result.error) && actual && IsSameDeletionItem(requested, actual);
        if (actual) CoTaskMemFree(actual);
        if (!same) { result.error = E_ACCESSDENIED; break; }
        IShellItem* parent_raw = nullptr;
        result.error = source->GetParent(&parent_raw);
        ComPtr<IShellItem> parent(parent_raw);
        if (!parent || FAILED(result.error)) { if (SUCCEEDED(result.error)) result.error = E_UNEXPECTED; break; }
        ITransferSource* transfer_raw = nullptr;
        result.error = parent->BindToHandler(nullptr, BHID_Transfer, IID_PPV_ARGS(&transfer_raw));
        ComPtr<ITransferSource> transfer(transfer_raw);
        if (!transfer || FAILED(result.error)) { if (SUCCEEDED(result.error)) result.error = E_UNEXPECTED; break; }
        IShellItem* bin_raw = nullptr;
        result.error = SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin_raw));
        ComPtr<IShellItem> bin(bin_raw);
        if (!bin || FAILED(result.error)) { if (SUCCEEDED(result.error)) result.error = E_UNEXPECTED; break; }
        if (cancelled && cancelled()) { result.cancelled = true; result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED); break; }
        FILE_ID_INFO current_id{};
        if (!ReadIdentity(source_path, current_id) || !SameIdentity(source_id, current_id)) { result.error = E_ACCESSDENIED; break; }
        TransferSink sink(cancelled);
        DWORD cookie = 0;
        result.error = transfer->Advise(&sink, &cookie);
        if (FAILED(result.error)) break; // no unguarded provider/UI fallback
        IShellItem* destination_raw = nullptr;
        result.error = transfer->RecycleItem(source.get(), bin.get(), TSF_NORMAL, &destination_raw);
        transfer->Unadvise(cookie);
        ComPtr<IShellItem> destination(destination_raw);
        if (FAILED(sink.Failure())) result.error = sink.Failure();
        if (result.error == COPYENGINE_E_USER_CANCELLED) result.cancelled = true;
        if (FAILED(result.error)) {
            if (cancelled && cancelled()) result.cancelled = true;
            break;
        }
        PWSTR returned = nullptr;
        const HRESULT named = destination ? destination->GetDisplayName(SIGDN_FILESYSPATH, &returned) : E_UNEXPECTED;
        const auto payload = returned ? path::StripExtendedPathPrefix(returned) : std::wstring{};
        if (returned) CoTaskMemFree(returned);
        if (FAILED(named) || !ValidDestination(source_path, payload, source_id)) { result.error = E_UNEXPECTED; break; }
        result.sources.push_back(requested); result.destinations.push_back(payload);
        if (progress) progress(result.sources.size(), paths.size(), requested);
        if (cancelled && cancelled()) { result.cancelled = true; result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED); break; }
    }
    if (result.sources.size() == paths.size() && !result.cancelled) { result.error = S_OK; result.failed_path.clear(); }
    return result;
}
}

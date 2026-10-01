#include "../common/known_folder_labels.h"
#include <iostream>

int wmain() {
    using namespace pulse;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    path::InitializeKnownFolderLabels();
    bool ok = true;
    auto check = [&](bool passed, const char* name) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
        ok &= passed;
    };
    const std::array<path::KnownFolderLabel, 3> redirected{{
        {L"D:\\Photos", l10n::StringId::KnownFolderPictures},
        {L"E:\\My Downloads", l10n::StringId::Downloads},
        {L"\\\\server\\share\\Clips", l10n::StringId::KnownFolderVideos}}};
    const auto* photos = path::FindKnownFolderLabel(L"\\\\?\\d:\\PHOTOS\\", redirected);
    check(photos && photos->label == l10n::StringId::KnownFolderPictures,
        "redirected pictures match actual path despite physical basename");
    check(path::FindKnownFolderLabel(L"e:/My Downloads/", redirected) == &redirected[1],
        "redirected downloads accept slash and trailing separator");
    check(path::FindKnownFolderLabel(L"\\\\?\\UNC\\server\\share\\Clips", redirected) == &redirected[2],
        "redirected UNC videos accept extended paths");
    check(!path::FindKnownFolderLabel(L"C:\\Photos", redirected) &&
          !path::FindKnownFolderLabel(L"D:\\Photos\\Vacation", redirected) &&
          !path::FindKnownFolderLabel(L"D:\\Photos2", redirected),
        "ordinary same-named folders and children retain physical names");
    check(l10n::Get(l10n::StringId::KnownFolderPictures) == L"图片" &&
          l10n::Get(l10n::StringId::KnownFolderVideos) == L"视频",
        "Chinese pictures and videos labels exist");
    for (const auto& folder : path::KnownFolderLabels()) {
        if (!folder.path.empty()) {
            check(path::KnownFolderDisplayName(folder.path, L"physical name") == l10n::Get(folder.label),
                "actual known folder location uses localized label");
        }
    }
    l10n::SetLanguage(L"en-US");
    check(l10n::Get(l10n::StringId::KnownFolderPictures) == L"Pictures" &&
          l10n::Get(l10n::StringId::KnownFolderVideos) == L"Videos",
        "English labels follow language switching");
    check(path::KnownFolderDisplayName(L"Z:\\PulseOrdinaryFolder", L"PulseOrdinaryFolder") == L"PulseOrdinaryFolder",
        "unknown location preserves fallback");
    return ok ? 0 : 1;
}

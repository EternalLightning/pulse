#include "../app/duplicate_scan.h"
#include <algorithm>

#include <cstdio>

using namespace pulse;

namespace {

int passed = 0;
int failed = 0;

void Check(bool condition, const wchar_t* name) {
    ++(condition ? passed : failed);
    wprintf(L"[%s] %s\n", condition ? L"PASS" : L"FAIL", name);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--ignore-only") {
        app::DuplicateScanSession ignored;
        ignored.generation = 1;
        index::ContentSearchProgress progress;
        progress.generation = 1;
        index::ContentHit first, second, other_first, other_second;
        first.path = L"C:\\fixture\\one.txt"; first.group = 1; first.size = 100;
        second = first; second.path = L"D:\\fixture\\two.txt";
        other_first = first; other_first.path = L"C:\\fixture\\other.txt"; other_first.group = 2;
        other_second = other_first; other_second.path = L"D:\\fixture\\other.txt";
        ignored.ApplyUpdate(progress, {first, second, other_first, other_second});
        const auto epoch = ignored.result_epoch;
        ignored.IgnoreGroup(0);
        Check(ignored.groups.size() == 1 && ignored.groups[0].id == 2 &&
            ignored.result_epoch > epoch, L"ignore removes only the selected group and refreshes the view");
        Check(ignored.ExtraCount() == 1 && ignored.AllFilesToDelete().size() == 1 &&
            ignored.AllFilesToDelete()[0].find(L"other.txt") != std::wstring::npos,
            L"ignored files are excluded from delete-all and its count");
        ignored.ApplyUpdate(progress, {first, second});
        Check(ignored.groups.size() == 1, L"later scan batches cannot restore an ignored group");
        auto third = other_first; third.path = L"E:\\fixture\\other.txt";
        ignored.ApplyUpdate(progress, {third});
        Check(ignored.groups.size() == 1 && ignored.groups[0].files.size() == 3,
            L"remaining group still receives updates after indices change");
        const auto before_invalid = ignored.result_epoch;
        ignored.IgnoreGroup(99);
        Check(ignored.result_epoch == before_invalid && ignored.groups.size() == 1,
            L"out-of-range ignore leaves results unchanged");
        ignored.IgnoreGroup(0);
        Check(ignored.groups.empty() && ignored.AllFilesToDelete().empty() && ignored.ExtraCount() == 0,
            L"ignoring all groups leaves no deletable files");
        ignored.ResetResults();
        ignored.generation = ++progress.generation;
        ignored.ApplyUpdate(progress, {first, second, other_first, other_second});
        Check(ignored.groups.size() == 2 && ignored.ExtraCount() == 2,
            L"new scan clears temporary ignores and restores matching groups");
        wprintf(L"%d passed, %d failed\n", passed, failed);
        return failed == 0 ? 0 : 1;
    }
    app::DuplicateScanSession session;
    Check(app::DuplicateScanSession::DefaultMinimumBytes(app::DuplicateScanScope::Folder) == 1024,
          L"folder default minimum is 1 KB");
    Check(app::DuplicateScanSession::DefaultMinimumBytes(app::DuplicateScanScope::Drive) ==
              1024ull * 1024ull,
          L"drive default minimum is 1 MB");
    Check(app::DuplicateScanSession::NormalizeDriveRoot(L"c:") == L"C:\\" &&
              app::DuplicateScanSession::NormalizeDriveRoot(L"D:\\") == L"D:\\",
          L"drive roots normalize to X:\\");

    index::VolumeInfo fixed;
    fixed.mount_point = L"C:\\";
    fixed.kind = index::VolumeKind::Fixed;
    index::VolumeInfo usb;
    usb.mount_point = L"E:\\";
    usb.kind = index::VolumeKind::Removable;
    auto all = app::DuplicateScanSession::ResolveRoots(
        app::DuplicateScanScope::AllFixed, L"", L"", {fixed, usb});
    Check(all.size() == 1 && all[0] == L"C:\\", L"all-local-disks uses fixed volumes only");
    auto drive = app::DuplicateScanSession::ResolveRoots(
        app::DuplicateScanScope::Drive, L"", L"E:\\", {fixed, usb});
    Check(drive.size() == 1 && drive[0] == L"E:\\", L"drive scope uses the selected root");
    auto folder = app::DuplicateScanSession::ResolveRoots(
        app::DuplicateScanScope::Folder, L"C:\\Windows", L"", {fixed, usb});
    Check(folder.size() == 1 && folder[0] == L"C:\\Windows", L"folder scope uses the folder path");

    session.generation = 1;
    session.scanning = true;
    index::ContentHit older;
    older.path = L"C:\\old.bin";
    older.name = L"old.bin";
    older.size = 100;
    older.modified = 10;
    older.group = 3;
    index::ContentHit newer;
    newer.path = L"D:\\new.bin";
    newer.name = L"new.bin";
    newer.size = 100;
    newer.modified = 20;
    newer.group = 3;
    index::ContentSearchProgress progress;
    progress.generation = 1;
    progress.done = true;
    session.ApplyUpdate(progress, {older, newer});
    Check(session.groups.size() == 1 && session.groups[0].files.size() == 2,
          L"hits with the same group become one card");
    Check(session.groups[0].keep_index == 0 &&
              session.groups[0].files[0].path == L"D:\\new.bin",
          L"default keep is the newest modified file");
    auto extra = session.FilesToDelete(0);
    Check(extra.size() == 1 && extra[0] == L"C:\\old.bin", L"FilesToDelete omits the kept file");
    session.SetKeep(0, 1);
    extra = session.FilesToDelete(0);
    Check(extra.size() == 1 && extra[0] == L"D:\\new.bin", L"SetKeep changes the file to delete");
    Check(session.AllFilesToDelete().size() == 1 && session.ExtraCount() == 1,
          L"all-extras matches the remaining copy");
    session.RemoveDeleted({L"D:\\new.bin"});
    Check(session.groups.empty(), L"groups drop when fewer than two files remain");

    app::DuplicateScanSession batched;
    batched.generation = 9;
    index::ContentSearchProgress batch_progress;
    batch_progress.generation = 9;
    batch_progress.done = false;
    index::ContentHit third = older;
    third.path = L"E:\\third.bin";
    third.name = L"third.bin";
    third.modified = 30;
    batched.ApplyUpdate(batch_progress, {older});
    Check(batched.groups.empty(), L"a partial duplicate group stays hidden until its second file");
    batched.ApplyUpdate(batch_progress, {newer});
    Check(batched.groups.size() == 1 && batched.groups[0].files.size() == 2,
          L"separate batches merge into one duplicate group");
    batched.SetKeep(0, 1);
    batched.ApplyUpdate(batch_progress, {third});
    Check(batched.groups[0].files.size() == 3 &&
              batched.groups[0].files[batched.groups[0].keep_index].path == L"C:\\old.bin",
          L"later batches preserve the chosen file to keep");

    app::DuplicateScanSession deletion;
    deletion.generation = 10;
    index::ContentSearchProgress deletion_progress;
    deletion_progress.generation = 10;
    index::ContentHit a = older;
    a.path = L"A:\\first.bin";
    a.name = L"first.bin";
    a.modified = 40;
    index::ContentHit b = older;
    b.path = L"B:\\second.bin";
    b.name = L"second.bin";
    b.modified = 30;
    index::ContentHit c = older;
    c.path = L"C:\\keeper.bin";
    c.name = L"keeper.bin";
    c.modified = 20;
    index::ContentHit d = older;
    d.path = L"D:\\last.bin";
    d.name = L"last.bin";
    d.modified = 10;
    deletion.ApplyUpdate(deletion_progress, {a, b, c, d});
    for (size_t i = 0; i < deletion.groups[0].files.size(); ++i) {
        if (deletion.groups[0].files[i].path == c.path) deletion.SetKeep(0, i);
    }
    deletion.RemoveDeleted({a.path});
    const auto kept_after_delete = deletion.FilesToDelete(0);
    Check(deletion.groups[0].files[deletion.groups[0].keep_index].path == c.path &&
              std::find(kept_after_delete.begin(), kept_after_delete.end(), c.path) == kept_after_delete.end(),
          L"deleting before the keeper preserves keeper identity");

    app::DuplicateScanSession reforming;
    reforming.generation = 11;
    index::ContentSearchProgress reform_progress;
    reform_progress.generation = 11;
    reforming.ApplyUpdate(reform_progress, {older, newer});
    reforming.RemoveDeleted({older.path});
    reforming.ApplyUpdate(reform_progress, {third});
    Check(reforming.groups.size() == 1 && reforming.groups[0].files.size() == 2 &&
              std::any_of(reforming.groups[0].files.begin(), reforming.groups[0].files.end(),
                          [&](const app::DuplicateFile& file) { return file.path == newer.path; }),
          L"a surviving file reforms its group after a later scan hit");

    app::DuplicateScanSession epoch;
    epoch.generation = 1;
    epoch.ApplyUpdate(progress, {older, newer});
    Check(epoch.result_epoch > 0, L"results bump the view epoch");
    const uint64_t after_hits = epoch.result_epoch;
    epoch.SetKeep(0, 1);
    Check(epoch.result_epoch > after_hits, L"keep changes bump the view epoch");
    const uint64_t after_keep = epoch.result_epoch;
    epoch.SetKeep(0, 1);
    Check(epoch.result_epoch == after_keep, L"unchanged keep leaves the view epoch");
    epoch.RemoveDeleted({L"D:\\new.bin"});
    Check(epoch.result_epoch > after_keep, L"deletes bump the view epoch");
    const uint64_t after_delete = epoch.result_epoch;
    epoch.ResetResults();
    Check(epoch.result_epoch > after_delete && epoch.groups.empty(),
          L"reset bumps the view epoch");

    wprintf(L"%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// Operation recovery journals and undo history persistence.
#include "ops_manager_internal.h"
#include "delete_operation.h"
#include "produced_undo.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include "../common/localization.h"
#include <algorithm>
#include <string_view>

namespace pulse::ops {

namespace {

void AppendJsonString(std::wstring& out, const std::wstring& value) {
    out += L'"';
    json::Escape(value, out);
    out += L'"';
}

void AppendStringArray(std::wstring& out, const std::vector<std::wstring>& values) {
    out += L'[';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += L',';
        AppendJsonString(out, values[i]);
    }
    out += L']';
}

bool ParseRecoveryEntry(const json::Value& doc, RecoveryEntry& entry) {
    if (doc.kind != json::Kind::Object) return false;
    const int type = json::Int(doc.Find(L"type"), -1);
    const int policy = json::Int(doc.Find(L"policy"), -1);
    if (type < static_cast<int>(OpType::Copy) || type > static_cast<int>(OpType::BatchRename) ||
        policy < static_cast<int>(CollisionPolicy::System) ||
        policy > static_cast<int>(CollisionPolicy::KeepBoth)) return false;
    if (!json::ParseUnsigned(json::String(doc.Find(L"seq")), entry.sequence) ||
        entry.sequence == UINT64_MAX) return false;
    entry.was_active = json::Bool(doc.Find(L"active"), false);
    entry.outcome_unknown = json::Bool(doc.Find(L"outcome_unknown"), false);
    entry.request.type = static_cast<OpType>(type);
    entry.request.collision_policy = static_cast<CollisionPolicy>(policy);
    entry.request.sources = json::Strings(doc.Find(L"sources"));
    entry.request.recycle_paths = json::Strings(doc.Find(L"recycle_paths"));
    entry.request.dest_dir = json::String(doc.Find(L"dest"));
    entry.request.new_name = json::String(doc.Find(L"name"));
    entry.request.new_names = json::Strings(doc.Find(L"names"));
    const int action = json::Int(doc.Find(L"folder_action"), 0);
    if (action < 0 || action > static_cast<int>(FolderAction::DissolveFolders)) return false;
    entry.request.folder_action = static_cast<FolderAction>(action);
    if (action && entry.request.type != OpType::Move) return false;
    entry.request.undo_label = json::String(doc.Find(L"undo_label"));
    entry.request.is_undo = json::Bool(doc.Find(L"undo"), false);
    const auto* targets = doc.Find(L"delete_targets");
    if (targets && targets->kind == json::Kind::Array &&
        std::all_of(targets->children.begin(), targets->children.end(),
            [](const auto& target) { return target.kind == json::Kind::Object; })) {
        for (const auto& target_value : targets->children) {
            DeleteRequestTarget target;
            target.path = json::String(target_value.Find(L"path"));
            target.physical_paths = json::Strings(target_value.Find(L"roots"));
            target.recycle_item = json::Bool(target_value.Find(L"recycle_item"));
            entry.request.delete_targets.push_back(std::move(target));
        }
    }
    if (entry.request.type == OpType::EmptyRecycle) return entry.sequence != 0;
    return entry.sequence != 0 && !entry.request.sources.empty();
}

} // namespace

struct OpsManager::JournalState {
    std::vector<std::wstring> paths;
    std::map<uint64_t, std::wstring> recovery_sources;
    std::map<std::wstring, std::vector<std::wstring>> opaque_entries;
    std::set<std::wstring> opaque_files;
};

OpsManager::OpsManager() = default;

OpsManager::~OpsManager() {
    Stop();
}

void OpsManager::SetJournalPath(std::wstring path) {
    SetJournalPaths({std::move(path)});
}

void OpsManager::SetJournalPaths(std::vector<std::wstring> paths) {
    if (running_) return;
    if (!journal_) journal_ = std::make_unique<JournalState>();
    journal_->paths.clear();
    for (auto& path : paths) {
        if (path.empty() || std::find(journal_->paths.begin(), journal_->paths.end(), path) != journal_->paths.end())
            continue;
        journal_->paths.push_back(std::move(path));
    }
    LoadRecoveryJournal();
}

void OpsManager::LoadRecoveryJournal() {
    pending_recovery_.clear();
    journal_->recovery_sources.clear();
    journal_->opaque_entries.clear();
    journal_->opaque_files.clear();
    next_seq_ = 1;
    std::set<uint64_t> used_sequences;
    uint64_t remapped_sequence = 1;
    for (const auto& path : journal_->paths) {
        std::wstring text;
        if (!ReadUtf8File(path, text)) {
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                journal_->opaque_files.insert(path);
            continue;
        }
        if (text.empty()) continue;
        const json::Document doc(text);
        const auto* entries_value = doc.Find(L"entries");
        if (!doc.Is(json::Kind::Object) || json::ExtractInt(doc, L"schema", 0) != 1 ||
            !entries_value || entries_value->kind != json::Kind::Array ||
            std::any_of(entries_value->children.begin(), entries_value->children.end(),
                [](const json::Value& value) { return value.kind != json::Kind::Object; })) {
            journal_->opaque_files.insert(path);
            continue;
        }
        for (const auto& object : entries_value->children) {
            RecoveryEntry entry;
            if (!ParseRecoveryEntry(object, entry)) {
                journal_->opaque_entries[path].push_back(doc.Raw(object));
                continue;
            }
            constexpr uint64_t kHighSequenceThreshold = UINT64_MAX / 2;
            if (used_sequences.contains(entry.sequence) || entry.sequence >= kHighSequenceThreshold) {
                while (remapped_sequence < kHighSequenceThreshold &&
                       used_sequences.contains(remapped_sequence)) ++remapped_sequence;
                if (remapped_sequence >= kHighSequenceThreshold) {
                    journal_->opaque_entries[path].push_back(doc.Raw(object));
                    continue;
                }
                entry.sequence = remapped_sequence++;
            }
            used_sequences.insert(entry.sequence);
            next_seq_ = (std::max)(next_seq_, entry.sequence + 1);
            journal_->recovery_sources.insert_or_assign(entry.sequence, path);
            pending_recovery_.push_back(std::move(entry));
        }
    }
}

RecoverySnapshot OpsManager::PendingRecovery() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RecoverySnapshot result;
    result.entries = pending_recovery_;
    result.has_uncertain_destructive = std::any_of(
        result.entries.begin(), result.entries.end(), [](const RecoveryEntry& entry) {
            return entry.was_active && IsDeleteOperation(entry.request.type);
        });
    return result;
}

bool OpsManager::RetryRecovery() {
    bool all_retryable = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) return false;
        for (const auto& recovery : pending_recovery_) {
            if (scheduled_recovery_.contains(recovery.sequence)) continue;
            if (recovery.outcome_unknown) {
                all_retryable = false;
                status_.last_error = l10n::Get(l10n::StringId::PermissionOperationOutcomeUnknown);
                status_.summary = status_.last_error; status_.phase = OpPhase::Failed;
                continue;
            }
            if (recovery.was_active && IsDeleteOperation(recovery.request.type)) {
                all_retryable = false;
                status_.last_error = l10n::Get(l10n::StringId::DeleteRecoveryPartial).c_str();
                status_.summary = status_.last_error; status_.phase = OpPhase::Failed;
                continue;
            }
            if (recovery.was_active && (recovery.request.type == OpType::Rename ||
                recovery.request.type == OpType::BatchRename)) {
                all_retryable = false;
                status_.last_error = l10n::Get(l10n::StringId::RenameRecoveryUnverified).c_str();
                status_.summary = status_.last_error; status_.phase = OpPhase::Failed;
                continue;
            }
            if (recovery.request.is_undo && IsMutationOperation(recovery.request.type)) {
                all_retryable = false;
                status_.last_error = l10n::Get(l10n::StringId::OperationUndoReservationMissing);
                status_.summary = status_.last_error; status_.phase = OpPhase::Failed;
                continue;
            }
            // A matching .pulse-copy/.pulse-backup name is not ownership proof.
            // Recovery that first requires destructive reconciliation stops.
            if (recovery.was_active && !recovery.request.dest_dir.empty()) {
                all_retryable = false;
                status_.last_error = l10n::Get(l10n::StringId::TemporaryRecoveryUnverified).c_str();
                status_.summary = status_.last_error;
                status_.phase = OpPhase::Failed;
                continue;
            }
            QueueItem item;
            item.seq = next_seq_++;
            item.req = recovery.request;
            item.req.delete_origin = DeleteOrigin::Recovery;
            item.recovery_sequence = recovery.sequence;
            scheduled_recovery_.insert(recovery.sequence);
            queue_.push_back(std::move(item));
        }
    }
    cv_.notify_one();
    if (notify_) notify_();
    return all_retryable;
}

void OpsManager::DiscardRecovery() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Discard forgets the journal only. Never delete files by a temporary
        // name pattern, which may also match user-owned files.
        pending_recovery_.clear();
        scheduled_recovery_.clear();
        std::erase_if(queue_, [](const QueueItem& item) { return item.recovery_sequence != 0; });
        if (journal_) {
            journal_->recovery_sources.clear();
            journal_->opaque_entries.clear();
            journal_->opaque_files.clear();
        }
    }
    if (journal_) {
        for (const auto& path : journal_->paths) DeleteFileW(path.c_str());
    }
    cv_.notify_one();
}

std::wstring OpsManager::JournalJsonLocked(const std::wstring& path) const {
    const std::wstring_view primary_path = journal_->paths.empty() ? std::wstring_view{} : journal_->paths.front();
    std::wstring out;
    const auto begin_entry = [&] {
        if (out.empty()) out = L"{\n  \"schema\":1,\n  \"entries\":[";
        else out += L',';
        out += L"\n    ";
    };
    const auto append = [&](const OpRequest& request, uint64_t sequence, bool active, bool outcome_unknown = false) {
        if (request.sources.empty() && request.type != OpType::EmptyRecycle) return;
        begin_entry();
        out += L"{\"seq\":";
        AppendJsonString(out, std::to_wstring(sequence));
        out += L",\"active\":";
        out += active ? L"true" : L"false";
        out += L",\"outcome_unknown\":";
        out += outcome_unknown ? L"true" : L"false";
        out += L",\"type\":";
        out += std::to_wstring(static_cast<int>(request.type));
        out += L",\"policy\":";
        out += std::to_wstring(static_cast<int>(request.collision_policy));
        out += L",\"undo\":";
        out += request.is_undo ? L"true" : L"false";
        out += L",\"sources\":";
        AppendStringArray(out, request.sources);
        out += L",\"dest\":";
        AppendJsonString(out, request.dest_dir);
        out += L",\"name\":";
        AppendJsonString(out, request.new_name);
        out += L",\"names\":";
        AppendStringArray(out, request.new_names);
        out += L",\"folder_action\":";
        out += std::to_wstring(static_cast<int>(request.folder_action));
        out += L",\"undo_label\":";
        AppendJsonString(out, request.undo_label);
        out += L",\"recycle_paths\":";
        AppendStringArray(out, request.recycle_paths);
        if (!request.delete_targets.empty()) {
            out += L",\"delete_targets\":[";
            for (size_t i = 0; i < request.delete_targets.size(); ++i) {
                const auto& target = request.delete_targets[i];
                if (i != 0) out += L',';
                out += L"{\"path\":";
                AppendJsonString(out, target.path);
                out += L",\"roots\":";
                AppendStringArray(out, target.physical_paths);
                out += L",\"recycle_item\":";
                out += target.recycle_item ? L"true" : L"false";
                out += L'}';
            }
            out += L']';
        }
        out += L'}';
    };
    if (path == primary_path) {
        if (active_item_ && active_item_->recovery_sequence == 0)
            append(active_item_->req, active_item_->seq, true, active_item_->elevated_retry);
        for (const auto& item : queue_)
            if (!IsDeleteOperation(item.req.type) && item.recovery_sequence == 0) append(item.req, item.seq, false);
    }
    if (active_item_ && active_item_->recovery_sequence != 0) {
        const auto source = journal_->recovery_sources.find(active_item_->recovery_sequence);
        const std::wstring_view source_path = source == journal_->recovery_sources.end()
            ? primary_path : std::wstring_view(source->second);
        if (source_path == path)
            append(active_item_->req, active_item_->seq, true, active_item_->elevated_retry);
    }
    for (const auto& recovery : pending_recovery_) {
        if (active_item_ && active_item_->recovery_sequence == recovery.sequence) continue;
        const auto source = journal_->recovery_sources.find(recovery.sequence);
        const std::wstring_view source_path = source == journal_->recovery_sources.end()
            ? primary_path : std::wstring_view(source->second);
        if (source_path != path) continue;
        append(recovery.request, recovery.sequence, recovery.was_active, recovery.outcome_unknown);
    }
    if (const auto opaque = journal_->opaque_entries.find(path);
        opaque != journal_->opaque_entries.end()) {
        for (const auto& object : opaque->second) {
            begin_entry();
            out += object;
        }
    }
    if (out.empty()) return {};
    out += L"\n  ]\n}\n";
    return out;
}

void OpsManager::PersistJournal() {
    if (!journal_) return;
    for (const auto& path : journal_->paths) {
        std::wstring text;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (journal_->opaque_files.contains(path)) continue;
            text = JournalJsonLocked(path);
        }
        if (!text.empty()) WriteUtf8FileAtomic(path, text);
        else DeleteFileW(path.c_str());
    }
}

// ---------------------------------------------------------------------------
// Undo stack JSON persistence.
// ---------------------------------------------------------------------------
std::wstring OpsManager::UndoToJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::wstring out = L"[\n";
    for (size_t i = 0; i < undo_.size(); ++i) {
        const auto& e = undo_[i];
        wchar_t head[64];
        swprintf_s(head, L"  {\"type\":%d,\"sup\":%s,", (int)e.type, e.supported && !e.session_only ? L"true" : L"false");
        out += head;
        out += L"\"dest\":";
        AppendJsonString(out, e.dest_dir);
        out += L",\"name\":";
        AppendJsonString(out, e.new_name);
        out += L",\"src\":";
        AppendStringArray(out, e.sources);
        out += L",\"dst\":";
        AppendStringArray(out, e.destinations);
        out += L",\"produced\":[";
        for (size_t j = 0; j < e.produced_identities.size(); ++j) {
            if (j) out += L',';
            AppendJsonString(out, ProducedIdentityText(e.produced_identities[j]));
        }
        out += L"],\"cleanup\":";
        AppendStringArray(out, e.cleanup_directories);
        out += L",\"undo_group\":";
        AppendJsonString(out, std::to_wstring(e.undo_group));
        out += L",\"undo_label\":";
        AppendJsonString(out, e.undo_label);
        if (e.folder_metadata) {
            const auto& basic = *e.folder_metadata;
            out += L",\"folder_basic\":";
            AppendStringArray(out, {
                std::to_wstring(basic.CreationTime.QuadPart), std::to_wstring(basic.LastAccessTime.QuadPart),
                std::to_wstring(basic.LastWriteTime.QuadPart), std::to_wstring(basic.ChangeTime.QuadPart),
                std::to_wstring(basic.FileAttributes)});
        }
        out += L"}";
        if (i + 1 < undo_.size()) out += L",";
        out += L"\n";
    }
    out += L"]";
    return out;
}

bool OpsManager::UndoFromJson(const std::wstring& in) {
    const json::Document doc(in);
    if (!doc.Is(json::Kind::Array)) return false;
    std::deque<UndoEntry> parsed;
    for (const auto& object : doc.Root().children) {
        if (object.kind != json::Kind::Object) return false;
        UndoEntry e;
        const int type = json::Int(object.Find(L"type"), -1);
        if (type < static_cast<int>(OpType::Copy) || type > static_cast<int>(OpType::RemoveEmptyFolder)) return false;
        e.type = static_cast<OpType>(type);
        const auto group = json::String(object.Find(L"undo_group"));
        if (!group.empty() && !json::ParseUnsigned(group, e.undo_group)) return false;
        e.undo_label = json::String(object.Find(L"undo_label"));
        const auto* supported = object.Find(L"sup");
        const auto* destination = object.Find(L"dest");
        const auto* name = object.Find(L"name");
        const auto* sources = object.Find(L"src");
        if (!supported || supported->kind != json::Kind::Boolean ||
            !destination || destination->kind != json::Kind::String ||
            !name || name->kind != json::Kind::String ||
            !sources || sources->kind != json::Kind::Array) return false;
        e.supported = json::Bool(supported);
        e.dest_dir = json::String(destination);
        e.new_name = json::String(name);
        if (const auto* metadata = object.Find(L"folder_basic")) {
            if (metadata->kind != json::Kind::Array || metadata->children.size() != 5 ||
                e.type != OpType::RemoveEmptyFolder) return false;
            uint64_t values[5]{};
            for (size_t i = 0; i < 5; ++i) {
                if (metadata->children[i].kind != json::Kind::String ||
                    !json::ParseUnsigned(metadata->children[i].text, values[i]) ||
                    (i < 4 && values[i] > INT64_MAX) || (i == 4 && values[i] > MAXDWORD)) return false;
            }
            FILE_BASIC_INFO basic{};
            basic.CreationTime.QuadPart = static_cast<LONGLONG>(values[0]);
            basic.LastAccessTime.QuadPart = static_cast<LONGLONG>(values[1]);
            basic.LastWriteTime.QuadPart = static_cast<LONGLONG>(values[2]);
            basic.ChangeTime.QuadPart = static_cast<LONGLONG>(values[3]);
            basic.FileAttributes = static_cast<DWORD>(values[4]);
            e.folder_metadata = basic;
        }
        for (const auto& source : sources->children) {
            if (source.kind != json::Kind::String) return false;
            e.sources.push_back(source.text);
        }
        if (const auto* destinations = object.Find(L"dst")) {
            if (destinations->kind != json::Kind::Array) return false;
            for (const auto& value : destinations->children) {
                if (value.kind != json::Kind::String) return false;
                e.destinations.push_back(value.text);
            }
        }
        if (const auto* produced = object.Find(L"produced")) {
            if (produced->kind != json::Kind::Array) return false;
            for (const auto& value : produced->children) {
                FileIdentity identity;
                if (value.kind != json::Kind::String || !ParseProducedIdentity(value.text, identity)) return false;
                e.produced_identities.push_back(identity);
            }
        }
        if (const auto* cleanup = object.Find(L"cleanup")) {
            if (cleanup->kind != json::Kind::Array) return false;
            for (const auto& value : cleanup->children) {
                if (value.kind != json::Kind::String || e.type != OpType::Move ||
                    std::find(e.destinations.begin(), e.destinations.end(), value.text) == e.destinations.end()) return false;
                e.cleanup_directories.push_back(value.text);
            }
        }
        if (RequiresProducedIdentity(e.type)) {
            const size_t count = e.type == OpType::Copy ? e.destinations.size() : e.sources.size();
            e.supported = e.supported && HasProducedIdentities(count, e.produced_identities) &&
                (!IsMutationOperation(e.type) || e.destinations.size() == count);
        }
        parsed.push_back(std::move(e));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    undo_ = std::move(parsed);
    undo_validation_requested_.store(true);
    ++undo_revision_;
    return true;
}

} // namespace pulse::ops

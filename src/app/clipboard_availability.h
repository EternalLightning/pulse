#pragma once
#include "../ops/clipboard.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace pulse::app {

struct ClipboardAvailabilityRequest {
    uint64_t generation = 0;
    uint32_t sequence = 0;
    bool file_format = false;
    bool shell_format = false;
    bool tray_present = false;
    std::vector<std::wstring> tray_paths;
};

struct ClipboardAvailabilityResult {
    uint64_t generation = 0;
    bool clipboard_available = false;
    bool tray_available = false;
    bool shell_available = false;
};

inline ClipboardAvailabilityResult ProbeClipboardAvailability(
    const ClipboardAvailabilityRequest& request,
    const std::function<bool(ops::ClipboardData&)>& read_clipboard,
    const std::function<bool(const std::wstring&)>& source_exists,
    const std::function<bool()>& cancelled = {}) {
    ClipboardAvailabilityResult result;
    result.generation = request.generation;
    bool shell_fallback = !request.file_format;
    const auto valid_paths = [&](const auto& paths) {
        if (paths.empty()) return false;
        for (const auto& path : paths) {
            if ((cancelled && cancelled()) || path.empty() || !source_exists(path)) return false;
        }
        return true;
    };
    if (request.file_format && !(cancelled && cancelled())) {
        ops::ClipboardData clipboard;
        const bool paths_read = read_clipboard(clipboard);
        shell_fallback = !paths_read || clipboard.paths.empty();
        result.clipboard_available = !shell_fallback && clipboard.sequence == request.sequence &&
            valid_paths(clipboard.paths);
    }
    // Virtual files have no paths to validate. Their formats and clipboard
    // sequence are captured by the UI pump; tray paths still get worker checks.
    if (shell_fallback && request.shell_format && !(cancelled && cancelled())) {
        result.clipboard_available = true;
        result.shell_available = true;
    }
    if (request.tray_present && result.clipboard_available) result.tray_available = valid_paths(request.tray_paths);
    return result;
}

class ClipboardAvailability {
public:
    struct Mailbox {
        std::mutex mutex;
        std::optional<ClipboardAvailabilityResult> result;
        std::atomic<uint64_t> generation{0};

        void Publish(ClipboardAvailabilityResult value) {
            std::lock_guard<std::mutex> lock(mutex);
            if (value.generation == generation.load()) result = std::move(value);
        }
    };

    bool Update(uint32_t sequence, bool file_format, bool tray_present,
                std::vector<std::wstring> tray_paths, bool shell_format = false) {
        tray_revision_.reset();
        if (initialized_ && inputs_.sequence == sequence && inputs_.file_format == file_format &&
            inputs_.shell_format == shell_format && inputs_.tray_present == tray_present &&
            inputs_.tray_paths == tray_paths) return false;
        const bool previously_available = Available();
        initialized_ = true;
        inputs_.sequence = sequence; inputs_.file_format = file_format;
        inputs_.shell_format = shell_format;
        inputs_.tray_present = tray_present; inputs_.tray_paths = std::move(tray_paths);
        inputs_.generation = mailbox_->generation.fetch_add(1) + 1;
        clipboard_available_ = tray_available_ = shell_available_ = false;
        pending_ = false; next_probe_ = 0;
        return previously_available;
    }

    template<class CollectPaths>
    bool UpdateVersioned(uint32_t sequence, bool file_format, bool tray_present,
                         uint64_t tray_revision, CollectPaths&& collect_paths,
                         bool shell_format = false) {
        if (initialized_ && tray_revision_ == tray_revision && inputs_.sequence == sequence &&
            inputs_.file_format == file_format && inputs_.shell_format == shell_format &&
            inputs_.tray_present == tray_present) return false;
        const bool changed = Update(sequence, file_format, tray_present, collect_paths(), shell_format);
        tray_revision_ = tray_revision;
        return changed;
    }

    bool Consume() {
        std::optional<ClipboardAvailabilityResult> result;
        {
            std::lock_guard<std::mutex> lock(mailbox_->mutex);
            result.swap(mailbox_->result);
        }
        if (!result || result->generation != inputs_.generation) return false;
        const bool previous = Available();
        pending_ = false;
        clipboard_available_ = result->clipboard_available;
        tray_available_ = result->tray_available;
        shell_available_ = result->shell_available;
        return previous != Available();
    }

    std::optional<ClipboardAvailabilityRequest> BeginRefresh(uint64_t now) {
        if (!initialized_ || pending_ || now < next_probe_ ||
            (!inputs_.file_format && !inputs_.shell_format) ||
            (inputs_.tray_present && inputs_.tray_paths.empty() && !inputs_.shell_format)) return {};
        pending_ = true;
        next_probe_ = now + 1000;
        return inputs_;
    }

    void RetryRefresh(uint64_t generation) {
        if (generation == inputs_.generation) pending_ = false;
    }

    bool Available() const noexcept {
        return clipboard_available_ &&
            (!inputs_.tray_present || tray_available_ || shell_available_);
    }

    std::shared_ptr<Mailbox> mailbox() const { return mailbox_; }

private:
    ClipboardAvailabilityRequest inputs_;
    std::optional<uint64_t> tray_revision_;
    std::shared_ptr<Mailbox> mailbox_ = std::make_shared<Mailbox>();
    uint64_t next_probe_ = 0;
    bool initialized_ = false;
    bool pending_ = false;
    bool clipboard_available_ = false;
    bool tray_available_ = false;
    bool shell_available_ = false;
};

} // namespace pulse::app

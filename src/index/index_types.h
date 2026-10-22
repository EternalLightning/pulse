#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::index {

inline constexpr size_t kSearchPageCap = 100000;
inline constexpr size_t kSearchUiPageSize = 2048;

struct Hit {
    std::wstring path;
    std::wstring name;
    bool is_dir = false;
    uint64_t size = 0;
    uint64_t mtime = 0; // FILETIME as u64
};

enum class ResultSort : uint8_t { Index, Name, Size, Mtime };

struct Query {
    uint64_t session_id = 0;
    bool subscribe = false;
    std::wstring needle;
    std::wstring path_prefix;
    bool folders_only = false;
    size_t offset = 0;
    size_t limit = 48;
    bool rank = true;
    ResultSort sort = ResultSort::Index;
    bool sort_desc = false;
};

struct SearchResult {
    uint64_t revision = 0;
    std::vector<Hit> hits;
    size_t total = 0;
    bool incomplete = false;
    bool scanning = false; // UI-side recursive enumeration is still running.
    uint32_t error = 0;
};

} // namespace pulse::index

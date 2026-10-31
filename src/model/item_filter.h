// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../fs/fs_enum.h"
#include "../index/index_query.h"
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::model {

enum class ItemFilterError { None, UnterminatedQuote, UnknownCondition, InvalidCondition };

// Resolve names once while compiling. Explicit tag: names are exact; legacy #
// names are fuzzy and retain the built-in color aliases.
using ItemFilterTagResolver = std::function<std::vector<int>(std::wstring_view, bool fuzzy)>;
using ItemFilterTagLookup = std::function<const std::vector<int>*(const std::wstring&)>;

bool ItemFilterTagNameMatches(std::wstring_view name, int index,
                              std::wstring_view needle, bool fuzzy);

struct CompiledItemFilter {
    struct TagClause {
        std::vector<int> alternatives;
        bool negated = false;
    };
    ItemFilterError error = ItemFilterError::None;
    std::wstring error_token;
    std::wstring name; // Joined ordinary tokens preserve directory-filter semantics.
    bool wildcard = false;
    std::vector<index::Term> metadata;
    std::vector<TagClause> tags; // AND of clauses, OR within each clause.

    bool Valid() const { return error == ItemFilterError::None; }
    bool HasTagConditions() const { return !tags.empty(); }
    // full_path is supplied by the owning view; no filesystem or ADS reads occur.
    // lookup may be empty. Missing assignments satisfy only negative tag clauses.
    bool Matches(const fs::DirEntry& entry, const std::wstring& full_path,
                 const ItemFilterTagLookup& lookup = {}) const;
};

// Supports tag:, #, ext:, type:/kind:, size:, dm:/datemodified:,
// folder:, file:, path: and their existing Chinese metadata aliases. Conditions
// are ANDed. tag:A,B is OR; multiple legacy # terms share one OR clause.
CompiledItemFilter CompileItemFilter(std::wstring_view text,
                                     const ItemFilterTagResolver& resolve_tag = {});

} // namespace pulse::model

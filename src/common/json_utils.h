#pragma once
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::json {
inline constexpr size_t kMaxDocumentChars = 8 * 1024 * 1024;
inline constexpr size_t kMaxDepth = 128;
inline constexpr size_t kMaxValues = 262144;
inline void SkipWhitespace(std::wstring_view input, size_t& pos) {
    while (pos < input.size() && (input[pos] == L' ' || input[pos] == L'\n' || input[pos] == L'\r' || input[pos] == L'\t')) ++pos;
}
inline void Escape(const std::wstring& input, std::wstring& output) {
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    for (wchar_t c : input) {
        if (c == L'\\') output += L"\\\\";
        else if (c == L'\"') output += L"\\\"";
        else if (c == L'\b') output += L"\\b";
        else if (c == L'\f') output += L"\\f";
        else if (c == L'\n') output += L"\\n";
        else if (c == L'\r') output += L"\\r";
        else if (c == L'\t') output += L"\\t";
        else if (static_cast<uint32_t>(c) < 0x20) {
            output += L"\\u00";
            output += hex[(static_cast<uint32_t>(c) >> 4) & 15];
            output += hex[static_cast<uint32_t>(c) & 15];
        } else output += c;
    }
}
namespace detail {
inline int Hex(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}
inline bool Unit(std::wstring_view input, size_t& pos, uint32_t& value) {
    if (input.size() - pos < 4) return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const int digit = Hex(input[pos++]);
        if (digit < 0) return false;
        value = value * 16 + static_cast<uint32_t>(digit);
    }
    return true;
}
inline bool String(std::wstring_view input, size_t& pos, std::wstring& output) {
    if (pos >= input.size() || input[pos++] != L'\"') return false;
    output.clear();
    while (pos < input.size()) {
        uint32_t c = static_cast<uint32_t>(input[pos++]);
        if (c == L'\"') return true;
        if (c < 0x20) return false;
        if (c == L'\\') {
            if (pos == input.size()) return false;
            c = static_cast<uint32_t>(input[pos++]);
            switch (c) {
            case L'\"': case L'\\': case L'/': break;
            case L'b': c = L'\b'; break;
            case L'f': c = L'\f'; break;
            case L'n': c = L'\n'; break;
            case L'r': c = L'\r'; break;
            case L't': c = L'\t'; break;
            case L'u': {
                if (!Unit(input, pos, c)) return false;
                if (c >= 0xD800 && c <= 0xDBFF) {
                    if (input.size() - pos < 6 || input[pos] != L'\\' || input[pos + 1] != L'u') return false;
                    pos += 2;
                    uint32_t low = 0;
                    if (!Unit(input, pos, low) || low < 0xDC00 || low > 0xDFFF) return false;
                    c = 0x10000 + ((c - 0xD800) << 10) + low - 0xDC00;
                } else if (c >= 0xDC00 && c <= 0xDFFF) return false;
                break;
            }
            default: return false;
            }
        } else if (c >= 0xD800 && c <= 0xDBFF) {
            if (pos == input.size()) return false;
            const uint32_t low = static_cast<uint32_t>(input[pos++]);
            if (low < 0xDC00 || low > 0xDFFF) return false;
            c = 0x10000 + ((c - 0xD800) << 10) + low - 0xDC00;
        } else if (c >= 0xDC00 && c <= 0xDFFF) return false;
        if (c > 0x10FFFF) return false;
        if constexpr (sizeof(wchar_t) == 2) {
            if (c > 0xFFFF) {
                output += static_cast<wchar_t>(0xD800 + ((c - 0x10000) >> 10));
                output += static_cast<wchar_t>(0xDC00 + ((c - 0x10000) & 1023));
            } else output += static_cast<wchar_t>(c);
        } else output += static_cast<wchar_t>(c);
    }
    return false;
}
} // namespace detail
// pos points at the opening quote; failure leaves pos/output unchanged.
inline bool ParseString(std::wstring_view input, size_t& pos, std::wstring& output) {
    size_t next = pos;
    std::wstring decoded;
    if (!detail::String(input, next, decoded)) return false;
    pos = next; output = std::move(decoded); return true;
}
enum class Kind { Null, Object, Array, String, Number, Boolean };
struct Value {
    Kind kind = Kind::Null;
    size_t begin = 0, end = 0;
    std::wstring name, text;
    std::vector<Value> children;
    const Value* Find(std::wstring_view key) const {
        if (kind == Kind::Object) for (const auto& child : children) if (child.name == key) return &child;
        return nullptr;
    }
};
class Document {
public:
    // Input must outlive this document; decoded values are owned, raw spans borrow it.
    // Only Places opts into its historical unquoted 0xRRGGBB "rgb" member.
    explicit Document(std::wstring_view input, bool legacy_rgb_hex = false) : input_(input), legacy_rgb_hex_(legacy_rgb_hex) {
        if (input.size() > kMaxDocumentChars) return;
        if (!input.empty() && input.front() == 0xFEFF) ++pos_;
        valid_ = Parse(root_, 0);
        SkipWhitespace(input_, pos_);
        valid_ = valid_ && pos_ == input_.size();
    }
    bool Is(Kind kind) const { return valid_ && root_.kind == kind; }
    bool Valid() const { return valid_; }
    const Value& Root() const { return root_; }
    const Value* Find(std::wstring_view key) const { return valid_ ? root_.Find(key) : nullptr; }
    std::wstring Raw(const Value& value) const { return std::wstring(input_.substr(value.begin, value.end - value.begin)); }
private:
    bool Take(wchar_t c) {
        SkipWhitespace(input_, pos_);
        if (pos_ == input_.size() || input_[pos_] != c) return false;
        ++pos_; return true;
    }
    bool Parse(Value& value, size_t depth) {
        if (depth > kMaxDepth || ++values_ > kMaxValues) return false;
        SkipWhitespace(input_, pos_);
        if (pos_ == input_.size()) return false;
        value.begin = pos_;
        const wchar_t token = input_[pos_];
        if (token == L'{' || token == L'[') {
            const bool object = token == L'{';
            value.kind = object ? Kind::Object : Kind::Array;
            const wchar_t close = object ? L'}' : L']';
            ++pos_;
            if (!Take(close)) for (;;) {
                Value child;
                if (object) {
                    SkipWhitespace(input_, pos_);
                    if (!detail::String(input_, pos_, child.name) || !Take(L':')) return false;
                }
                if (!Parse(child, depth + 1)) return false;
                value.children.push_back(std::move(child));
                if (Take(close)) break;
                if (!Take(L',')) return false;
            }
        } else if (token == L'\"') {
            value.kind = Kind::String;
            if (!detail::String(input_, pos_, value.text)) return false;
        } else if (token == L't' || token == L'f' || token == L'n') {
            const std::wstring_view word = token == L't' ? L"true" : token == L'f' ? L"false" : L"null";
            if (input_.substr(pos_, word.size()) != word) return false;
            pos_ += word.size(); value.kind = token == L'n' ? Kind::Null : Kind::Boolean; value.text = word;
        } else {
            value.kind = Kind::Number;
            if (legacy_rgb_hex_ && value.name == L"rgb" && (input_.substr(pos_, 2) == L"0x" || input_.substr(pos_, 2) == L"0X")) {
                pos_ += 2;
                const size_t start = pos_;
                while (pos_ < input_.size() && detail::Hex(input_[pos_]) >= 0) ++pos_;
                if (pos_ == start || pos_ - start > 8) return false;
            } else {
                if (token == L'-') ++pos_;
                if (pos_ == input_.size() || input_[pos_] < L'0' || input_[pos_] > L'9') return false;
                if (input_[pos_] == L'0') ++pos_;
                else while (pos_ < input_.size() && input_[pos_] >= L'0' && input_[pos_] <= L'9') ++pos_;
                if (pos_ < input_.size() && input_[pos_] == L'.') {
                    const size_t start = ++pos_;
                    while (pos_ < input_.size() && input_[pos_] >= L'0' && input_[pos_] <= L'9') ++pos_;
                    if (pos_ == start) return false;
                }
                if (pos_ < input_.size() && (input_[pos_] == L'e' || input_[pos_] == L'E')) {
                    ++pos_;
                    if (pos_ < input_.size() && (input_[pos_] == L'+' || input_[pos_] == L'-')) ++pos_;
                    const size_t start = pos_;
                    while (pos_ < input_.size() && input_[pos_] >= L'0' && input_[pos_] <= L'9') ++pos_;
                    if (pos_ == start) return false;
                }
            }
            value.text = input_.substr(value.begin, pos_ - value.begin);
        }
        value.end = pos_; return true;
    }
    std::wstring_view input_;
    size_t pos_ = 0, values_ = 0;
    bool legacy_rgb_hex_ = false, valid_ = false;
    Value root_;
};
inline size_t ValuePosition(const Document& doc, const std::wstring& key) {
    const auto* value = doc.Find(key); return value ? value->begin : std::wstring::npos;
}
inline size_t ValuePosition(const std::wstring& input, const std::wstring& key) { return ValuePosition(Document(input), key); }
// Historical callers pass a position immediately after the opening quote.
inline std::wstring UnescapeString(const std::wstring& input, size_t& pos) {
    if (!pos || pos > input.size()) return {};
    size_t start = pos - 1; std::wstring output;
    if (!ParseString(input, start, output)) { pos = input.size(); return {}; }
    pos = start; return output;
}
inline bool ParseUnsigned(std::wstring_view text, uint64_t& output, unsigned base = 10) {
    if (text.empty() || (base != 10 && base != 16)) return false;
    uint64_t value = 0;
    for (wchar_t c : text) {
        const int digit = base == 16 ? detail::Hex(c) : c >= L'0' && c <= L'9' ? c - L'0' : -1;
        if (digit < 0 || static_cast<unsigned>(digit) >= base || value > ((std::numeric_limits<uint64_t>::max)() - static_cast<unsigned>(digit)) / base) return false;
        value = value * base + static_cast<unsigned>(digit);
    }
    output = value; return true;
}
inline bool ParseInteger(std::wstring_view text, int& output) {
    const bool negative = text.starts_with(L"-");
    if (negative) text.remove_prefix(1);
    uint64_t magnitude = 0;
    const uint64_t maximum = static_cast<uint64_t>((std::numeric_limits<int>::max)()) + (negative ? 1 : 0);
    if (!ParseUnsigned(text, magnitude) || magnitude > maximum) return false;
    output = negative && magnitude == maximum ? (std::numeric_limits<int>::min)()
        : negative ? -static_cast<int>(magnitude) : static_cast<int>(magnitude);
    return true;
}
inline int Int(const Value* value, int fallback = 0) {
    int output = 0;
    return value && value->kind == Kind::Number && ParseInteger(value->text, output) ? output : fallback;
}
inline uint64_t Unsigned(const Value* value, uint64_t fallback = 0) {
    uint64_t output = 0;
    return value && value->kind == Kind::Number && ParseUnsigned(value->text, output) ? output : fallback;
}
inline std::wstring String(const Value* value, std::wstring fallback = {}) { return value && value->kind == Kind::String ? value->text : std::move(fallback); }
inline bool Bool(const Value* value, bool fallback = false) { return value && value->kind == Kind::Boolean ? value->text == L"true" : fallback; }
inline std::vector<std::wstring> Strings(const Value* value) {
    std::vector<std::wstring> output;
    if (!value || value->kind != Kind::Array) return output;
    for (const auto& item : value->children) { if (item.kind != Kind::String) return {}; output.push_back(item.text); }
    return output;
}
inline std::wstring ExtractString(const Document& doc, const std::wstring& key, std::wstring fallback = {}) { return String(doc.Find(key), std::move(fallback)); }
inline std::wstring ExtractString(const std::wstring& input, const std::wstring& key, std::wstring fallback = {}) { return ExtractString(Document(input), key, std::move(fallback)); }
inline int ExtractInt(const Document& doc, const std::wstring& key, int fallback = 0) { return Int(doc.Find(key), fallback); }
inline int ExtractInt(const std::wstring& input, const std::wstring& key, int fallback = 0) { return ExtractInt(Document(input), key, fallback); }
inline bool ExtractBool(const Document& doc, const std::wstring& key, bool fallback = false) { return Bool(doc.Find(key), fallback); }
inline bool ExtractBool(const std::wstring& input, const std::wstring& key, bool fallback = false) { return ExtractBool(Document(input), key, fallback); }
inline std::vector<std::wstring> ExtractStringArray(const Document& doc, const std::wstring& key) { return Strings(doc.Find(key)); }
inline std::vector<std::wstring> ExtractStringArray(const std::wstring& input, const std::wstring& key) { return ExtractStringArray(Document(input), key); }
inline std::wstring ExtractRaw(const Document& doc, const std::wstring& key, Kind kind) {
    const auto* value = doc.Find(key); return value && value->kind == kind ? doc.Raw(*value) : std::wstring{};
}
inline std::wstring ExtractRaw(const std::wstring& input, const std::wstring& key, Kind kind) { return ExtractRaw(Document(input), key, kind); }
inline std::vector<std::wstring> Objects(const Document& doc, const Value* array) {
    std::vector<std::wstring> output;
    if (!doc.Valid() || !array || array->kind != Kind::Array) return output;
    for (const auto& value : array->children) { if (value.kind != Kind::Object) return {}; output.push_back(doc.Raw(value)); }
    return output;
}
inline std::vector<std::wstring> ExtractObjectArray(const Document& doc, const std::wstring& key) { return Objects(doc, doc.Find(key)); }
inline std::vector<std::wstring> ExtractObjectArray(const std::wstring& input, const std::wstring& key) { return ExtractObjectArray(Document(input), key); }
} // namespace pulse::json

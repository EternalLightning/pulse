#pragma once
#include <utility>

namespace pulse {
// A small scoped owner for Win32/CNG resources. Destruction never throws.
// Instantiate with an explicit invalid value and a stateless closer.
template<typename T, T Invalid, typename Closer>
class UniqueResource {
public:
    explicit UniqueResource(T value = Invalid) noexcept : value_(value) {}
    ~UniqueResource() noexcept { reset(); }
    UniqueResource(const UniqueResource&) = delete;
    UniqueResource& operator=(const UniqueResource&) = delete;
    UniqueResource(UniqueResource&& other) noexcept : value_(other.release()) {}
    UniqueResource& operator=(UniqueResource&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    T get() const noexcept { return value_; }
    T release() noexcept { return std::exchange(value_, Invalid); }
    void reset(T next = Invalid) noexcept {
        if (value_ != Invalid) Closer{}(value_);
        value_ = next;
    }
private:
    T value_;
};
}

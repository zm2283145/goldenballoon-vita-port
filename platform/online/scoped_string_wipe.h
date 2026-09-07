#ifndef MDKR_SCOPED_STRING_WIPE_H
#define MDKR_SCOPED_STRING_WIPE_H

#include <cstddef>
#include <string>

// Guards the caller-owned buffer on ordinary return and stack unwinding.
// The string must outlive the guard. Reserve its final capacity before adding
// secrets: a guard cannot erase copies already released by string growth.
// Erase must be a nonthrowing, optimization-resistant erasure operation.
template <void (*Erase)(void *, std::size_t)>
class MdkrScopedStringWipe {
public:
    explicit MdkrScopedStringWipe(std::string &value) noexcept : value_(value) {}
    MdkrScopedStringWipe(const MdkrScopedStringWipe &) = delete;
    MdkrScopedStringWipe &operator=(const MdkrScopedStringWipe &) = delete;
    ~MdkrScopedStringWipe() noexcept { wipe(); }
    void wipe() noexcept {
        if (value_.empty()) return;
        Erase(&value_[0], value_.size());
        value_.clear();
    }
private:
    std::string &value_;
};

#endif

#pragma once

#include <unknwn.h>

#include <memory>
#include <utility>

// Minimal owning COM pointer (MinGW has no usable WRL ComPtr).
template <class T>
class Com {
public:
    Com() = default;
    ~Com() { Reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com& operator=(Com&& other) noexcept {
        std::swap(p_, other.p_);
        return *this;
    }

    T** Put() { Reset(); return &p_; }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    void Reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }

private:
    T* p_ = nullptr;
};

// Copyable shared reference to a COM object, for values that travel
// between threads in plain structs. Only valid across MTA threads (or
// threads implicitly in the MTA), which is all rout's COM code uses.
template <class T>
std::shared_ptr<T> ShareCom(T* p) {
    if (!p) return {};
    p->AddRef();
    return std::shared_ptr<T>(p, [](T* q) { q->Release(); });
}

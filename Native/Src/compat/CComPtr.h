#pragma once

#include <Windows.h>
#include <objbase.h>

// Minimal ATL CComPtr stand-in when the VS "C++ ATL" component is not installed.
template<typename T>
class CComPtr {
    T* p_{ nullptr };

    void InternalRelease() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

    void InternalAddRef(T* p) {
        p_ = p;
        if (p_) p_->AddRef();
    }

public:
    CComPtr() = default;
    CComPtr(T* p) { InternalAddRef(p); }
    CComPtr(const CComPtr& other) { InternalAddRef(other.p_); }
    ~CComPtr() { InternalRelease(); }

    CComPtr& operator=(T* p) {
        if (p != p_) {
            InternalRelease();
            InternalAddRef(p);
        }
        return *this;
    }

    CComPtr& operator=(const CComPtr& other) {
        if (this != &other) *this = other.p_;
        return *this;
    }

    template<typename U>
    CComPtr& operator=(const CComPtr<U>& other) {
        if (!other) {
            InternalRelease();
            return *this;
        }
        if constexpr (std::is_convertible_v<U*, T*>) {
            return *this = static_cast<T*>(static_cast<U*>(other));
        }
        else {
            T* q = nullptr;
            if (FAILED(other->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&q)))) {
                InternalRelease();
                return *this;
            }
            InternalRelease();
            p_ = q;
            return *this;
        }
    }

    CComPtr& operator=(int zero) {
        if (zero == 0) InternalRelease();
        return *this;
    }

    operator T*() const { return p_; }
    T* operator->() const { return p_; }
    T** operator&() {
        InternalRelease();
        return &p_;
    }

    T* Detach() {
        T* tmp = p_;
        p_ = nullptr;
        return tmp;
    }

    void Attach(T* p) {
        InternalRelease();
        p_ = p;
    }

    void Release() { InternalRelease(); }

    HRESULT CoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter = nullptr, DWORD dwClsContext = CLSCTX_INPROC_SERVER) {
        InternalRelease();
        return ::CoCreateInstance(rclsid, pUnkOuter, dwClsContext, __uuidof(T), reinterpret_cast<void**>(&p_));
    }

    bool operator!() const { return p_ == nullptr; }
    explicit operator bool() const { return p_ != nullptr; }
};

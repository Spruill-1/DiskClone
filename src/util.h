#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dc {

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

inline std::wstring ErrorMessage(DWORD err) {
    wchar_t* buf = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    struct LocalFreeGuard {
        wchar_t* p;
        ~LocalFreeGuard() { if (p) LocalFree(p); }
    } guard{ buf };
    std::wstring msg = len && buf ? std::wstring(buf, len) : L"unknown error";
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' '))
        msg.pop_back();
    return msg;
}

// Exit codes (see README): thrown as typed exceptions, mapped in main.
enum class ExitCode : int {
    Ok = 0,
    Usage = 1,
    NotElevated = 2,
    SafetyRefusal = 3,
    VssFailure = 4,
    CopyFailure = 5,
    FinalizeFailure = 6,
    UserAbort = 130,
};

class Error : public std::runtime_error {
public:
    Error(ExitCode code, const std::wstring& message)
        : std::runtime_error("dc::Error"), code_(code), message_(message) {}
    ExitCode code() const { return code_; }
    const std::wstring& message() const { return message_; }
private:
    ExitCode code_;
    std::wstring message_;
};

[[noreturn]] inline void ThrowWin32Err(ExitCode code, const std::wstring& what, DWORD err) {
    throw Error(code, what + L": (" + std::to_wstring(err) + L") " + ErrorMessage(err));
}

// Captures GetLastError() BEFORE the message expression is evaluated — message
// construction heap-allocates and can clobber the thread's last-error value,
// and function-argument evaluation order would not guarantee the capture.
#define ThrowWin32(code, ...)                                    \
    do {                                                         \
        const ::DWORD dcErr_ = ::GetLastError();                 \
        ::dc::ThrowWin32Err((code), __VA_ARGS__, dcErr_);        \
    } while (0)

[[noreturn]] inline void ThrowHr(ExitCode code, const std::wstring& what, HRESULT hr) {
    wchar_t buf[16];
    swprintf_s(buf, L"0x%08X", static_cast<unsigned>(hr));
    throw Error(code, what + L": HRESULT " + buf + L" " + ErrorMessage(static_cast<DWORD>(hr)));
}

// ---------------------------------------------------------------------------
// RAII wrappers
// ---------------------------------------------------------------------------

class unique_handle {
public:
    unique_handle() = default;
    explicit unique_handle(HANDLE h) : h_(h) {}
    ~unique_handle() { reset(); }
    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;
    unique_handle(unique_handle&& o) noexcept : h_(o.h_) { o.h_ = INVALID_HANDLE_VALUE; }
    unique_handle& operator=(unique_handle&& o) noexcept {
        if (this != &o) { reset(); h_ = o.h_; o.h_ = INVALID_HANDLE_VALUE; }
        return *this;
    }
    HANDLE get() const { return h_; }
    bool valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
    void reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (valid()) CloseHandle(h_);
        h_ = h;
    }
    HANDLE release() { HANDLE h = h_; h_ = INVALID_HANDLE_VALUE; return h; }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

// COM interop policy: this project uses C++/WinRT (winrt/base.h, inbox in the
// Windows SDK) as the interaction layer for all COM interfaces — see vss.cpp
// for winrt::com_ptr / winrt::init_apartment usage. No hand-rolled COM RAII.

// Page-aligned buffer: page alignment satisfies any logical sector size,
// as required by FILE_FLAG_NO_BUFFERING I/O.
class AlignedBuffer {
public:
    explicit AlignedBuffer(size_t size) : size_(size) {
        p_ = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p_) ThrowWin32(ExitCode::CopyFailure, L"VirtualAlloc failed");
    }
    ~AlignedBuffer() { if (p_) VirtualFree(p_, 0, MEM_RELEASE); }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    uint8_t* data() const { return static_cast<uint8_t*>(p_); }
    size_t size() const { return size_; }
private:
    void* p_ = nullptr;
    size_t size_ = 0;
};

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

inline std::wstring FormatBytes(uint64_t bytes) {
    static const wchar_t* units[] = { L"B", L"KiB", L"MiB", L"GiB", L"TiB" };
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    wchar_t buf[64];
    if (u == 0) swprintf_s(buf, L"%llu %s", static_cast<unsigned long long>(bytes), units[u]);
    else        swprintf_s(buf, L"%.1f %s", v, units[u]);
    return buf;
}

inline std::wstring GuidToString(const GUID& g) {
    wchar_t buf[64];
    swprintf_s(buf, L"{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

constexpr uint64_t kMiB = 1024ull * 1024ull;

inline uint64_t AlignDown(uint64_t x, uint64_t a) { return x - (x % a); }
inline uint64_t AlignUp(uint64_t x, uint64_t a) { return AlignDown(x + a - 1, a); }

// ---------------------------------------------------------------------------
// Progress reporting (single-line \r refresh on stderr)
// ---------------------------------------------------------------------------

class Progress {
public:
    void Begin(uint64_t totalBytes) {
        total_ = totalBytes;
        done_ = 0;
        startTick_ = lastTick_ = GetTickCount64();
        lastDone_ = 0;
    }
    void Add(uint64_t bytes) {
        done_ += bytes;
        ULONGLONG now = GetTickCount64();
        if (now - lastTick_ >= 1000) {
            double secs = (now - lastTick_) / 1000.0;
            double mbps = (done_ - lastDone_) / secs / (1024.0 * 1024.0);
            double pct = total_ ? 100.0 * done_ / total_ : 0.0;
            uint64_t remain = total_ > done_ ? total_ - done_ : 0;
            uint64_t etaSec = mbps > 0.01 ? static_cast<uint64_t>(remain / (mbps * 1024.0 * 1024.0)) : 0;
            fwprintf(stderr, L"\r  %5.1f%%  %s / %s  %.0f MB/s  ETA %llu:%02llu   ",
                pct, FormatBytes(done_).c_str(), FormatBytes(total_).c_str(), mbps,
                static_cast<unsigned long long>(etaSec / 60),
                static_cast<unsigned long long>(etaSec % 60));
            fflush(stderr);
            lastTick_ = now;
            lastDone_ = done_;
        }
    }
    void End() {
        ULONGLONG now = GetTickCount64();
        double secs = (now - startTick_) / 1000.0;
        double mbps = secs > 0.01 ? done_ / secs / (1024.0 * 1024.0) : 0.0;
        fwprintf(stderr, L"\r  100.0%%  %s copied in %.0fs (%.0f MB/s avg)          \n",
            FormatBytes(done_).c_str(), secs, mbps);
        fflush(stderr);
    }
private:
    uint64_t total_ = 0;
    uint64_t done_ = 0;
    uint64_t lastDone_ = 0;
    ULONGLONG startTick_ = 0;
    ULONGLONG lastTick_ = 0;
};

// Global cancellation flag set by the Ctrl+C handler (see main.cpp).
extern volatile long g_cancelled;

inline void CheckCancelled() {
    if (g_cancelled)
        throw Error(ExitCode::UserAbort, L"operation cancelled by user");
}

} // namespace dc

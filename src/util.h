#pragma once

// Shared plumbing for diskclone: typed errors that carry process exit codes,
// aligned-buffer allocation for unbuffered I/O, formatting helpers, and the
// single-line progress reporter used by the copy engine.
//
// Resource management policy: prefer well-defined upstream helpers over
// custom RAII — WIL (wil/resource.h, header-only, vendored as a submodule)
// for Win32 handles/allocations, std::unique_ptr for pimpl, and C++/WinRT
// (winrt/base.h) as the interaction layer for all COM interfaces (see
// vss.cpp). Custom RAII appears only where no upstream helper fits.

#include <windows.h>

#include <wil/resource.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace DiskClone
{
    // -----------------------------------------------------------------------
    // Errors
    // -----------------------------------------------------------------------

    inline std::wstring ErrorMessage(DWORD errorCode)
    {
        wchar_t* rawBuffer = nullptr;
        DWORD length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<wchar_t*>(&rawBuffer), 0, nullptr);
        wil::unique_hlocal_string messageBuffer{ rawBuffer };

        std::wstring message = length && messageBuffer
            ? std::wstring(messageBuffer.get(), length)
            : L"unknown error";
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
        {
            message.pop_back();
        }

        return message;
    }

    // Process exit codes (documented in the README). Failures are thrown as
    // typed exceptions carrying one of these; wmain maps them at the boundary.
    enum class ExitCode : int
    {
        Ok = 0,
        Usage = 1,
        NotElevated = 2,
        SafetyRefusal = 3,     // nothing was written to the target
        VssFailure = 4,        // snapshots are taken before target prep, so also nothing written
        CopyFailure = 5,       // the error text states whether the target was already wiped
        FinalizeFailure = 6,   // data copy succeeded; message carries manual recovery commands
        UserAbort = 130,
    };

    class Error : public std::runtime_error
    {
    public:
        Error(ExitCode code, const std::wstring& message)
            : std::runtime_error("DiskClone::Error"), m_code(code), m_message(message)
        {
        }

        ExitCode Code() const { return m_code; }
        const std::wstring& Message() const { return m_message; }

    private:
        ExitCode m_code;
        std::wstring m_message;
    };

    // Call-site contract: capture GetLastError() into a local BEFORE building
    // the message string — message construction heap-allocates and can clobber
    // the thread's last-error value, and function-argument evaluation order
    // would not guarantee the capture:
    //
    //     const DWORD lastError = GetLastError();
    //     ThrowWin32Error(ExitCode::CopyFailure, L"WriteFile failed", lastError);
    [[noreturn]] inline void ThrowWin32Error(ExitCode code, const std::wstring& what, DWORD errorCode)
    {
        throw Error(code, what + L": (" + std::to_wstring(errorCode) + L") " + ErrorMessage(errorCode));
    }

    [[noreturn]] inline void ThrowHr(ExitCode code, const std::wstring& what, HRESULT hr)
    {
        wchar_t hexBuffer[16];
        swprintf_s(hexBuffer, L"0x%08X", static_cast<unsigned>(hr));
        throw Error(code, what + L": HRESULT " + hexBuffer + L" " + ErrorMessage(static_cast<DWORD>(hr)));
    }

    // -----------------------------------------------------------------------
    // Aligned I/O buffers
    // -----------------------------------------------------------------------

    // Page-aligned allocation: page alignment satisfies any logical sector
    // size, as required by FILE_FLAG_NO_BUFFERING I/O. Freed via VirtualFree
    // by the WIL smart pointer.
    inline wil::unique_virtualalloc_ptr<uint8_t> AllocateAlignedBuffer(size_t size)
    {
        void* data = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!data)
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::CopyFailure, L"VirtualAlloc failed", lastError);
        }

        return wil::unique_virtualalloc_ptr<uint8_t>{ static_cast<uint8_t*>(data) };
    }

    // -----------------------------------------------------------------------
    // Formatting helpers
    // -----------------------------------------------------------------------

    inline std::wstring FormatBytes(uint64_t bytes)
    {
        static const wchar_t* units[] = { L"B", L"KiB", L"MiB", L"GiB", L"TiB" };
        double value = static_cast<double>(bytes);
        int unitIndex = 0;
        while (value >= 1024.0 && unitIndex < 4)
        {
            value /= 1024.0;
            ++unitIndex;
        }

        wchar_t formatted[64];
        if (unitIndex == 0)
        {
            swprintf_s(formatted, L"%llu %s", static_cast<unsigned long long>(bytes), units[unitIndex]);
        }
        else
        {
            swprintf_s(formatted, L"%.1f %s", value, units[unitIndex]);
        }

        return formatted;
    }

    inline std::wstring GuidToString(const GUID& guid)
    {
        wchar_t formatted[64];
        swprintf_s(formatted, L"{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
            guid.Data1, guid.Data2, guid.Data3,
            guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
            guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
        return formatted;
    }

    constexpr uint64_t kMiB = 1024ull * 1024ull;

    inline uint64_t AlignDown(uint64_t value, uint64_t alignment) { return value - (value % alignment); }
    inline uint64_t AlignUp(uint64_t value, uint64_t alignment) { return AlignDown(value + alignment - 1, alignment); }

    // -----------------------------------------------------------------------
    // Progress reporting
    // -----------------------------------------------------------------------

    // Single-line progress display, refreshed in place on stderr (\r) at most
    // once a second: percentage, bytes copied, throughput, and ETA.
    class Progress
    {
    public:
        void Begin(uint64_t totalBytes)
        {
            m_total = totalBytes;
            m_done = 0;
            m_startTick = m_lastTick = GetTickCount64();
            m_lastDone = 0;
        }

        void Add(uint64_t bytes)
        {
            m_done += bytes;
            ULONGLONG now = GetTickCount64();
            if (now - m_lastTick >= 1000)
            {
                double seconds = (now - m_lastTick) / 1000.0;
                double megabytesPerSecond = (m_done - m_lastDone) / seconds / (1024.0 * 1024.0);
                double percent = m_total ? 100.0 * m_done / m_total : 0.0;
                uint64_t remainingBytes = m_total > m_done ? m_total - m_done : 0;
                uint64_t etaSeconds = megabytesPerSecond > 0.01
                    ? static_cast<uint64_t>(remainingBytes / (megabytesPerSecond * 1024.0 * 1024.0))
                    : 0;
                fwprintf(stderr, L"\r  %5.1f%%  %s / %s  %.0f MB/s  ETA %llu:%02llu   ",
                    percent, FormatBytes(m_done).c_str(), FormatBytes(m_total).c_str(), megabytesPerSecond,
                    static_cast<unsigned long long>(etaSeconds / 60),
                    static_cast<unsigned long long>(etaSeconds % 60));
                fflush(stderr);
                m_lastTick = now;
                m_lastDone = m_done;
            }
        }

        void End()
        {
            ULONGLONG now = GetTickCount64();
            double seconds = (now - m_startTick) / 1000.0;
            double megabytesPerSecond = seconds > 0.01 ? m_done / seconds / (1024.0 * 1024.0) : 0.0;
            fwprintf(stderr, L"\r  100.0%%  %s copied in %.0fs (%.0f MB/s avg)          \n",
                FormatBytes(m_done).c_str(), seconds, megabytesPerSecond);
            fflush(stderr);
        }

    private:
        uint64_t m_total{ 0 };
        uint64_t m_done{ 0 };
        uint64_t m_lastDone{ 0 };
        ULONGLONG m_startTick{ 0 };
        ULONGLONG m_lastTick{ 0 };
    };

    // Global cancellation flag set by the Ctrl+C handler (see main.cpp).
    extern volatile long g_cancelled;

    inline void CheckCancelled()
    {
        if (g_cancelled)
        {
            throw Error(ExitCode::UserAbort, L"operation cancelled by user");
        }
    }
}

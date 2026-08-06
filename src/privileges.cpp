#include "privileges.h"
#include "util.h"

namespace DiskClone
{
    namespace
    {
        void EnablePrivilege(HANDLE token, const wchar_t* privilegeName)
        {
            TOKEN_PRIVILEGES privileges{};
            privileges.PrivilegeCount = 1;
            privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (!LookupPrivilegeValueW(nullptr, privilegeName, &privileges.Privileges[0].Luid))
            {
                const DWORD lastError = GetLastError();
                ThrowWin32Error(ExitCode::NotElevated,
                    std::wstring(L"LookupPrivilegeValue(") + privilegeName + L") failed", lastError);
            }

            if (!AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr))
            {
                const DWORD lastError = GetLastError();
                ThrowWin32Error(ExitCode::NotElevated,
                    std::wstring(L"AdjustTokenPrivileges(") + privilegeName + L") failed", lastError);
            }

            // AdjustTokenPrivileges succeeds even when it assigned nothing;
            // the real verdict is in the last-error value.
            if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
            {
                throw Error(ExitCode::NotElevated,
                    std::wstring(L"privilege not held: ") + privilegeName + L" (run from an elevated prompt)");
            }
        }
    }

    void EnsureElevatedAndEnablePrivileges()
    {
        wil::unique_handle token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::NotElevated, L"OpenProcessToken failed", lastError);
        }

        // The manifest requests requireAdministrator, but verify the token
        // anyway — defense in depth against a stripped or bypassed manifest.
        TOKEN_ELEVATION elevation{};
        DWORD returnedLength = 0;
        if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &returnedLength))
        {
            const DWORD lastError = GetLastError();
            ThrowWin32Error(ExitCode::NotElevated, L"GetTokenInformation(TokenElevation) failed", lastError);
        }

        if (!elevation.TokenIsElevated)
        {
            throw Error(ExitCode::NotElevated, L"diskclone requires an elevated (Administrator) prompt");
        }

        EnablePrivilege(token.get(), SE_BACKUP_NAME);
        EnablePrivilege(token.get(), SE_RESTORE_NAME);
        EnablePrivilege(token.get(), SE_MANAGE_VOLUME_NAME);
    }
}

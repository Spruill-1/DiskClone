#include "privileges.h"
#include "util.h"

namespace dc {

static void EnablePrivilege(HANDLE token, const wchar_t* name) {
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid))
        ThrowWin32(ExitCode::NotElevated, std::wstring(L"LookupPrivilegeValue(") + name + L") failed");
    if (!AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr))
        ThrowWin32(ExitCode::NotElevated, std::wstring(L"AdjustTokenPrivileges(") + name + L") failed");
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
        throw Error(ExitCode::NotElevated,
            std::wstring(L"privilege not held: ") + name + L" (run from an elevated prompt)");
}

void EnsureElevatedAndEnablePrivileges() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw))
        ThrowWin32(ExitCode::NotElevated, L"OpenProcessToken failed");
    unique_handle token(raw);

    TOKEN_ELEVATION elev{};
    DWORD len = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elev, sizeof(elev), &len))
        ThrowWin32(ExitCode::NotElevated, L"GetTokenInformation(TokenElevation) failed");
    if (!elev.TokenIsElevated)
        throw Error(ExitCode::NotElevated, L"diskclone requires an elevated (Administrator) prompt");

    EnablePrivilege(token.get(), SE_BACKUP_NAME);
    EnablePrivilege(token.get(), SE_RESTORE_NAME);
    EnablePrivilege(token.get(), SE_MANAGE_VOLUME_NAME);
}

} // namespace dc

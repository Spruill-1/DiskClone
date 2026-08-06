#pragma once

namespace DiskClone
{
    // Verifies the process token is elevated (throws ExitCode::NotElevated) and
    // enables the privileges raw disk work needs: SeBackupPrivilege,
    // SeRestorePrivilege (also required for RegLoadKey during --new-ids
    // finalization), and SeManageVolumePrivilege (FSCTL_EXTEND_VOLUME).
    void EnsureElevatedAndEnablePrivileges();
}

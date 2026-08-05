#pragma once

namespace dc {

// Verifies the process token is elevated (throws ExitCode::NotElevated) and
// enables SeBackupPrivilege, SeRestorePrivilege and SeManageVolumePrivilege.
void EnsureElevatedAndEnablePrivileges();

} // namespace dc

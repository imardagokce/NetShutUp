#pragma once

// elevation.h — UAC administrator privilege detection and self-elevation

#include <windows.h>

namespace netshutup::elevation {

// Returns true if the current process token has elevated administrator privileges.
bool is_elevated() noexcept;

// Re-launches the current executable with the "runas" verb, requesting UAC elevation.
// Passes through the command-line arguments, waits for the elevated child process
// to finish, and returns the child's exit code.
// If the user rejects the UAC prompt (ERROR_CANCELLED), prints an informative message
// and returns 1223 (ERROR_CANCELLED).
int restart_elevated(int argc, wchar_t* argv[]) noexcept;

} // namespace netshutup::elevation

#pragma once

// menu.h — Interactive Win32 console menu for NetShutUp Legacy
//
// Rules:
//  - Uses direct Win32 Console APIs (SetConsoleTextAttribute, ReadConsoleInputW).
//  - Does not rely on ANSI escape sequences (100% compatible with Windows 7/8 cmd.exe).
//  - Fully navigable with arrow keys, Enter, and Esc/q.

#include "version.h"

namespace netshutup::menu {

// Runs the interactive console menu loop until the user exits.
// Returns process exit code (0 on clean exit).
int run(const OsVersion& os);

} // namespace netshutup::menu

// menu.cpp — Interactive Win32 console menu implementation for NetShutUp Legacy

#include "menu.h"
#include "scan.h"
#include "preview.h"
#include "apply.h"
#include "elevation.h"
#include "version.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace netshutup::menu {

// ── Color Attributes (Standard Win32 Console NT/Win7 Compatible) ─────────────

static constexpr WORD COLOR_DEFAULT    = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
static constexpr WORD COLOR_MUTED      = FOREGROUND_INTENSITY; // Dark gray
static constexpr WORD COLOR_CYAN       = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
static constexpr WORD COLOR_WHITE_BOLD = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
static constexpr WORD COLOR_GREEN      = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static constexpr WORD COLOR_YELLOW     = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static constexpr WORD COLOR_SELECTED   = BACKGROUND_BLUE | BACKGROUND_INTENSITY |
                                         FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;

static void set_color(HANDLE hOut, WORD attr) {
    SetConsoleTextAttribute(hOut, attr);
}

// ── Screen and Cursor Management ─────────────────────────────────────────────

static void clear_screen(HANDLE hOut) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hOut, &csbi)) {
        return;
    }

    DWORD cell_count = csbi.dwSize.X * csbi.dwSize.Y;
    DWORD written = 0;
    COORD home = { 0, 0 };

    FillConsoleOutputCharacterW(hOut, L' ', cell_count, home, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, cell_count, home, &written);
    SetConsoleCursorPosition(hOut, home);
}

static void wait_for_key(HANDLE hIn, HANDLE hOut) {
    wprintf(L"\n");
    set_color(hOut, COLOR_MUTED);
    wprintf(L"Press any key to return to the menu...");
    set_color(hOut, COLOR_DEFAULT);
    wprintf(L"\n");

    FlushConsoleInputBuffer(hIn);
    INPUT_RECORD rec;
    DWORD read_count = 0;
    while (ReadConsoleInputW(hIn, &rec, 1, &read_count)) {
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            break;
        }
    }
}

// ── Menu Items ────────────────────────────────────────────────────────────────

struct MenuItem {
    std::wstring title;
    std::wstring description;
};

static const std::vector<MenuItem> k_menu_items = {
    { L"Scan Network Settings",      L"Inspect current network settings vs defaults" },
    { L"Preview a Profile",          L"See what would change without modifying anything" },
    { L"Apply: Recommended Profile", L"Safe, broadly beneficial network tweaks" },
    { L"Apply: Privacy Profile",     L"Disable LLMNR, CEIP telemetry, and WER" },
    { L"Apply: Performance Profile", L"Remove multimedia network throttling" },
    { L"Revert Last Change",         L"Undo changes from the most recent backup" },
    { L"Exit",                       L"Quit NetShutUp Legacy" }
};

// ── Key Navigation ────────────────────────────────────────────────────────────

enum class NavAction {
    UP,
    DOWN,
    ENTER,
    ESCAPE,
    NONE
};

static NavAction read_nav_key(HANDLE hIn, int& selected_index, int max_index) {
    INPUT_RECORD rec;
    DWORD read_count = 0;

    while (ReadConsoleInputW(hIn, &rec, 1, &read_count)) {
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) {
            continue;
        }

        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        wchar_t ch = rec.Event.KeyEvent.uChar.UnicodeChar;

        if (vk == VK_UP) {
            return NavAction::UP;
        }
        if (vk == VK_DOWN) {
            return NavAction::DOWN;
        }
        if (vk == VK_RETURN) {
            return NavAction::ENTER;
        }
        if (vk == VK_ESCAPE || ch == L'q' || ch == L'Q') {
            return NavAction::ESCAPE;
        }

        // Direct number selection (1-7)
        if (ch >= L'1' && ch <= L'7') {
            int idx = ch - L'1';
            if (idx <= max_index) {
                selected_index = idx;
                return NavAction::ENTER;
            }
        }
    }

    return NavAction::NONE;
}

// ── Rendering ─────────────────────────────────────────────────────────────────

static void render_menu(HANDLE hOut, int selected_index, const OsVersion& os, bool is_admin) {
    clear_screen(hOut);

    // Header box
    set_color(hOut, COLOR_CYAN);
    wprintf(L"  +--------------------------------------------------------------+\n");
    wprintf(L"  |                NetShutUp Legacy -- Win7/8/8.1                |\n");
    wprintf(L"  |     Network privacy and performance tweaks -- safe & undoable|\n");
    wprintf(L"  +--------------------------------------------------------------+\n\n");

    set_color(hOut, COLOR_MUTED);
    wprintf(L"  Use UP/DOWN arrow keys to navigate, ENTER to select, ESC to exit.\n\n");

    // Items
    for (std::size_t i = 0; i < k_menu_items.size(); ++i) {
        bool is_selected = (static_cast<int>(i) == selected_index);

        if (is_selected) {
            set_color(hOut, COLOR_SELECTED);
            wprintf(L"   > [%zu] %-28ls", i + 1, k_menu_items[i].title.c_str());
            wprintf(L" %-30ls \n", k_menu_items[i].description.c_str());
            set_color(hOut, COLOR_DEFAULT);
        } else {
            set_color(hOut, COLOR_DEFAULT);
            wprintf(L"     [%zu] %-28ls", i + 1, k_menu_items[i].title.c_str());
            set_color(hOut, COLOR_MUTED);
            wprintf(L" %ls\n", k_menu_items[i].description.c_str());
            set_color(hOut, COLOR_DEFAULT);
        }
    }

    // Status bar
    wprintf(L"\n");
    set_color(hOut, COLOR_CYAN);
    wprintf(L"  ----------------------------------------------------------------\n");
    set_color(hOut, COLOR_MUTED);
    wprintf(L"  OS: Windows %lu.%lu build %lu | Administrator: ", os.major, os.minor, os.build);
    if (is_admin) {
        set_color(hOut, COLOR_GREEN);
        wprintf(L"Yes\n");
    } else {
        set_color(hOut, COLOR_YELLOW);
        wprintf(L"No (auto-elevates when needed)\n");
    }
    set_color(hOut, COLOR_DEFAULT);
}

// ── Action Handlers ───────────────────────────────────────────────────────────

static void do_scan(HANDLE hIn, HANDLE hOut, const OsVersion& os) {
    clear_screen(hOut);
    auto results = scan::scan_all();
    scan::print_results(results, os.major, os.minor, os.build);
    wait_for_key(hIn, hOut);
}

static void do_preview(HANDLE hIn, HANDLE hOut, const OsVersion& os) {
    clear_screen(hOut);
    set_color(hOut, COLOR_CYAN);
    wprintf(L"Select a profile to preview:\n");
    set_color(hOut, COLOR_DEFAULT);
    wprintf(L"  [1] Recommended\n");
    wprintf(L"  [2] Privacy\n");
    wprintf(L"  [3] Performance\n");
    wprintf(L"  [Esc] Back to menu\n\n");
    wprintf(L"Choice: ");

    FlushConsoleInputBuffer(hIn);
    INPUT_RECORD rec;
    DWORD read_count = 0;
    const wchar_t* profile = nullptr;

    while (ReadConsoleInputW(hIn, &rec, 1, &read_count)) {
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            wchar_t ch = rec.Event.KeyEvent.uChar.UnicodeChar;
            WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
            if (vk == VK_ESCAPE) return;
            if (ch == L'1') { profile = L"recommended"; break; }
            if (ch == L'2') { profile = L"privacy"; break; }
            if (ch == L'3') { profile = L"performance"; break; }
        }
    }

    if (profile) {
        clear_screen(hOut);
        auto prev = preview::generate(profile, os.os_mask());
        if (prev.has_value()) {
            preview::print_preview(*prev);
        }
        wait_for_key(hIn, hOut);
    }
}

static void do_apply(HANDLE hIn, HANDLE hOut, const OsVersion& os, const wchar_t* profile) {
    clear_screen(hOut);
    set_color(hOut, COLOR_CYAN);
    wprintf(L"NetShutUp Legacy -- Apply Profile: %ls\n", profile);
    wprintf(L"======================================================================\n\n");
    set_color(hOut, COLOR_DEFAULT);

    wprintf(L"Are you sure you want to apply the '%ls' profile?\n", profile);
    set_color(hOut, COLOR_MUTED);
    wprintf(L"A verified backup will be saved automatically before any changes.\n");
    set_color(hOut, COLOR_WHITE_BOLD);
    wprintf(L"Proceed? [y/N]: ");
    set_color(hOut, COLOR_DEFAULT);

    FlushConsoleInputBuffer(hIn);
    INPUT_RECORD rec;
    DWORD read_count = 0;
    bool confirmed = false;

    while (ReadConsoleInputW(hIn, &rec, 1, &read_count)) {
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            wchar_t ch = rec.Event.KeyEvent.uChar.UnicodeChar;
            if (ch == L'y' || ch == L'Y') {
                confirmed = true;
                break;
            }
            if (ch == L'n' || ch == L'N' || rec.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE ||
                rec.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) {
                confirmed = false;
                break;
            }
        }
    }

    if (!confirmed) {
        wprintf(L"\nApply cancelled. No changes were made.\n");
        wait_for_key(hIn, hOut);
        return;
    }

    // Check elevation
    if (!elevation::is_elevated()) {
        wprintf(L"\nElevating to administrator rights...\n");
        wchar_t* argv[] = {
            const_cast<wchar_t*>(L"netshutup-legacy"),
            const_cast<wchar_t*>(L"apply"),
            const_cast<wchar_t*>(profile),
            const_cast<wchar_t*>(L"--ignore-os")
        };
        elevation::restart_elevated(4, argv);
        wait_for_key(hIn, hOut);
        return;
    }

    auto res = apply::apply_profile(profile, os.os_mask(), /*dry_run=*/false);
    apply::print_apply_result(res);
    wait_for_key(hIn, hOut);
}

static void do_revert(HANDLE hIn, HANDLE hOut) {
    clear_screen(hOut);
    set_color(hOut, COLOR_CYAN);
    wprintf(L"NetShutUp Legacy -- Revert Network Settings\n");
    wprintf(L"======================================================================\n\n");
    set_color(hOut, COLOR_DEFAULT);

    // Check elevation
    if (!elevation::is_elevated()) {
        wprintf(L"Elevating to administrator rights...\n");
        wchar_t* argv[] = {
            const_cast<wchar_t*>(L"netshutup-legacy"),
            const_cast<wchar_t*>(L"revert"),
            const_cast<wchar_t*>(L"--ignore-os")
        };
        elevation::restart_elevated(3, argv);
        wait_for_key(hIn, hOut);
        return;
    }

    auto res = apply::revert();
    apply::print_revert_result(res);
    wait_for_key(hIn, hOut);
}

// ── Main Loop ─────────────────────────────────────────────────────────────────

int run(const OsVersion& os) {
    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    if (hIn == INVALID_HANDLE_VALUE || hOut == INVALID_HANDLE_VALUE) {
        return 1;
    }

    // Save initial console state
    DWORD orig_in_mode = 0;
    GetConsoleMode(hIn, &orig_in_mode);

    CONSOLE_SCREEN_BUFFER_INFO orig_csbi;
    GetConsoleScreenBufferInfo(hOut, &orig_csbi);

    CONSOLE_CURSOR_INFO orig_cursor;
    GetConsoleCursorInfo(hOut, &orig_cursor);

    // Set raw key input mode (no echo, no line input)
    SetConsoleMode(hIn, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT);

    // Hide blinking cursor during menu navigation
    CONSOLE_CURSOR_INFO cur_info = orig_cursor;
    cur_info.bVisible = FALSE;
    SetConsoleCursorInfo(hOut, &cur_info);

    int selected_index = 0;
    const int max_index = static_cast<int>(k_menu_items.size()) - 1;
    bool running = true;

    while (running) {
        bool is_admin = elevation::is_elevated();
        render_menu(hOut, selected_index, os, is_admin);

        NavAction act = read_nav_key(hIn, selected_index, max_index);

        if (act == NavAction::UP) {
            selected_index = (selected_index == 0) ? max_index : selected_index - 1;
        } else if (act == NavAction::DOWN) {
            selected_index = (selected_index == max_index) ? 0 : selected_index + 1;
        } else if (act == NavAction::ESCAPE) {
            running = false;
        } else if (act == NavAction::ENTER) {
            // Restore visible cursor for command execution output
            cur_info.bVisible = TRUE;
            SetConsoleCursorInfo(hOut, &cur_info);

            switch (selected_index) {
                case 0: // Scan
                    do_scan(hIn, hOut, os);
                    break;
                case 1: // Preview
                    do_preview(hIn, hOut, os);
                    break;
                case 2: // Apply: Recommended
                    do_apply(hIn, hOut, os, L"recommended");
                    break;
                case 3: // Apply: Privacy
                    do_apply(hIn, hOut, os, L"privacy");
                    break;
                case 4: // Apply: Performance
                    do_apply(hIn, hOut, os, L"performance");
                    break;
                case 5: // Revert
                    do_revert(hIn, hOut);
                    break;
                case 6: // Exit
                    running = false;
                    break;
            }

            // Hide cursor again for menu navigation
            cur_info.bVisible = FALSE;
            SetConsoleCursorInfo(hOut, &cur_info);
        }
    }

    // Restore original console state before returning
    SetConsoleMode(hIn, orig_in_mode);
    SetConsoleTextAttribute(hOut, orig_csbi.wAttributes);
    SetConsoleCursorInfo(hOut, &orig_cursor);
    clear_screen(hOut);

    return 0;
}

} // namespace netshutup::menu

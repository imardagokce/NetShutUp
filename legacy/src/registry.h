#pragma once

// registry.h — Low-level Win32 registry helpers for NetShutUp Legacy
//
// Rules:
//  - Read operations never require elevation.
//  - Write / delete operations require KEY_SET_VALUE; the caller is
//    responsible for ensuring the process has the right privilege level.
//  - Predefined HKEY handles (HKLM, HKCU, ...) are never closed by RegKey.
//  - Localized strings are never parsed.

#include <windows.h>
#include <optional>
#include <string>
#include <vector>

namespace netshutup::registry {

// ── RAII wrapper ─────────────────────────────────────────────────────────────
class RegKey {
public:
    RegKey() = default;
    explicit RegKey(HKEY key) noexcept : key_(key) {}
    ~RegKey() { close(); }

    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;

    RegKey(RegKey&& o) noexcept : key_(o.key_) { o.key_ = nullptr; }
    RegKey& operator=(RegKey&& o) noexcept {
        close();
        key_ = o.key_;
        o.key_ = nullptr;
        return *this;
    }

    // Opens a subkey under a predefined hive handle.
    // Returns an invalid RegKey (valid() == false) if the key does not exist
    // or the caller lacks the required access.
    static RegKey open(HKEY hive, const wchar_t* subkey,
                       REGSAM access = KEY_READ) noexcept;

    void close() noexcept;
    bool valid()  const noexcept { return key_ != nullptr; }
    HKEY get()    const noexcept { return key_; }

private:
    HKEY key_ = nullptr;
};

// ── Read ─────────────────────────────────────────────────────────────────────

// Reads a REG_DWORD value.
// Returns std::nullopt if the key or value does not exist, or if the stored
// type is not REG_DWORD.
std::optional<DWORD> read_dword(HKEY hive,
                                 const wchar_t* subkey,
                                 const wchar_t* value_name) noexcept;

// Returns true if the key can be opened for reading (exists and is accessible).
bool key_exists(HKEY hive, const wchar_t* subkey) noexcept;

// Enumerates the immediate subkey names of a key.
// Returns an empty vector if the key does not exist or cannot be opened.
std::vector<std::wstring> enum_subkeys(HKEY hive,
                                        const wchar_t* subkey);

// ── Write / delete (require elevation for HKLM keys) ─────────────────────────

// Writes a REG_DWORD value. Creates the subkey if it does not exist.
// Returns false on any error (access denied, out of memory, …).
bool write_dword(HKEY hive,
                 const wchar_t* subkey,
                 const wchar_t* value_name,
                 DWORD data) noexcept;

// Deletes a named value.
// Returns true if the value was deleted or if it did not exist.
// Returns false only on a genuine I/O error (not ERROR_FILE_NOT_FOUND).
bool delete_value(HKEY hive,
                  const wchar_t* subkey,
                  const wchar_t* value_name) noexcept;

// ── Sandbox key (tests only) ──────────────────────────────────────────────────
// Tests must use this prefix instead of real system keys.
// Defined here so both production code and test code can reference the same
// constant without coupling tests to private headers.
constexpr wchar_t SANDBOX_SUBKEY[] =
    L"Software\\NetShutUp-Test";

} // namespace netshutup::registry

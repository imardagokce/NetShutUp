// registry.cpp — Win32 registry helpers (NetShutUp Legacy)

#include "registry.h"

namespace netshutup::registry {

// ── RegKey ───────────────────────────────────────────────────────────────────

RegKey RegKey::open(HKEY hive, const wchar_t* subkey, REGSAM access) noexcept {
    HKEY h = nullptr;
    if (RegOpenKeyExW(hive, subkey, 0, access, &h) != ERROR_SUCCESS)
        return RegKey{};
    return RegKey{h};
}

void RegKey::close() noexcept {
    if (key_) {
        RegCloseKey(key_);
        key_ = nullptr;
    }
}

// ── Read ─────────────────────────────────────────────────────────────────────

std::optional<DWORD> read_dword(HKEY hive,
                                 const wchar_t* subkey,
                                 const wchar_t* value_name) noexcept {
    auto key = RegKey::open(hive, subkey, KEY_QUERY_VALUE);
    if (!key.valid()) return std::nullopt;

    DWORD data  = 0;
    DWORD size  = sizeof(data);
    DWORD type  = 0;
    LONG  rc    = RegQueryValueExW(
        key.get(), value_name,
        nullptr, &type,
        reinterpret_cast<BYTE*>(&data), &size);

    if (rc != ERROR_SUCCESS || type != REG_DWORD)
        return std::nullopt;

    return data;
}

std::optional<std::wstring> read_string(HKEY hive, const wchar_t* subkey,
                                        const wchar_t* value_name) noexcept {
    auto key = RegKey::open(hive, subkey, KEY_QUERY_VALUE);
    if (!key.valid()) return std::nullopt;

    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key.get(), value_name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || size > 64 * 1024) {
        return std::nullopt;
    }
    std::wstring buf(size / sizeof(wchar_t) + 1, wchar_t(0));
    DWORD got = size;
    if (RegQueryValueExW(key.get(), value_name, nullptr, &type,
                         reinterpret_cast<BYTE*>(buf.data()), &got) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    buf.resize(wcsnlen(buf.c_str(), buf.size()));
    return buf;
}

bool key_exists(HKEY hive, const wchar_t* subkey) noexcept {
    return RegKey::open(hive, subkey, KEY_READ).valid();
}

std::vector<std::wstring> enum_subkeys(HKEY hive, const wchar_t* subkey) {
    std::vector<std::wstring> result;

    auto key = RegKey::open(hive, subkey, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE);
    if (!key.valid()) return result;

    // Query the maximum subkey name length once to size the buffer.
    DWORD max_name_chars = 0;
    if (RegQueryInfoKeyW(key.get(),
                         nullptr, nullptr, nullptr,  // class info (unused)
                         nullptr,                    // subkey count (unused)
                         &max_name_chars,            // longest subkey name (chars, excl. NUL)
                         nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr) != ERROR_SUCCESS) {
        return result;
    }

    std::wstring name(max_name_chars + 1, L'\0');

    for (DWORD idx = 0; ; ++idx) {
        DWORD name_len = static_cast<DWORD>(name.size());
        LONG rc = RegEnumKeyExW(
            key.get(), idx,
            name.data(), &name_len,
            nullptr, nullptr, nullptr, nullptr);

        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc == ERROR_SUCCESS)
            result.emplace_back(name.data(), name_len);
        // On any other error, skip the entry and continue.
    }

    return result;
}

// ── Write / delete ───────────────────────────────────────────────────────────

bool write_dword(HKEY hive,
                 const wchar_t* subkey,
                 const wchar_t* value_name,
                 DWORD data) noexcept {
    HKEY    h           = nullptr;
    DWORD   disposition = 0;
    LONG    rc          = RegCreateKeyExW(
        hive, subkey,
        0, nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr, &h, &disposition);

    if (rc != ERROR_SUCCESS) return false;
    RegKey key{h};

    rc = RegSetValueExW(
        key.get(), value_name,
        0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&data),
        sizeof(data));

    return rc == ERROR_SUCCESS;
}

bool delete_value(HKEY hive,
                  const wchar_t* subkey,
                  const wchar_t* value_name) noexcept {
    auto key = RegKey::open(hive, subkey, KEY_SET_VALUE);
    if (!key.valid()) return true; // key absent ⇒ value absent ⇒ success

    LONG rc = RegDeleteValueW(key.get(), value_name);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

} // namespace netshutup::registry

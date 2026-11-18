// backup.cpp — Backup and restore engine implementation for NetShutUp Legacy

#include "backup.h"
#include "registry.h"
#include "store.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <sstream>

namespace netshutup::backup {

// ── UTF-8 / UTF-16 Conversion Helpers ─────────────────────────────────────────

static std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string str(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                        str.data(), size, nullptr, nullptr);
    return str;
}

static std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                                   nullptr, 0);
    if (size <= 0) return {};
    std::wstring wstr(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                        wstr.data(), size);
    return wstr;
}

// ── Directory Helpers ─────────────────────────────────────────────────────────

static std::wstring get_exe_dir() {
    wchar_t buffer[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return L".";
    for (DWORD i = len; i > 0; --i) {
        if (buffer[i - 1] == L'\\') {
            buffer[i - 1] = L'\0';
            return buffer;
        }
    }
    return L".";
}

static std::wstring get_backups_dir() {
    std::wstring dir = get_exe_dir() + L"\\backups";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// ── File I/O Helpers ──────────────────────────────────────────────────────────

static bool write_text_file(const std::wstring& path, const std::string& content) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytes_written = 0;
    BOOL ok = WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()),
                        &bytes_written, nullptr);
    if (ok) {
        FlushFileBuffers(hFile);
    }
    CloseHandle(hFile);
    return ok && (bytes_written == content.size());
}

static std::optional<std::string> read_text_file(const std::wstring& path) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart > 10 * 1024 * 1024) { // limit 10MB
        CloseHandle(hFile);
        return std::nullopt;
    }

    std::string content(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD bytes_read = 0;
    BOOL ok = ReadFile(hFile, content.data(), static_cast<DWORD>(content.size()),
                       &bytes_read, nullptr);
    CloseHandle(hFile);

    if (!ok || bytes_read != content.size()) {
        return std::nullopt;
    }

    return content;
}

// ── save ──────────────────────────────────────────────────────────────────────

bool save(const std::vector<const catalog::Entry*>& entries,
          const std::wstring&                        profile_name,
          const std::wstring&                        path) {
    // No adapter list: per-adapter entries expand to nothing and are skipped.
    return save(targets::expand(entries, {}), profile_name, path);
}

bool save(const std::vector<targets::Target>& target_list,
          const std::wstring&                 profile_name,
          const std::wstring&                 path) {
    std::vector<BackupEntry> recorded;
    recorded.reserve(target_list.size());

    for (const auto& t : target_list) {
        if (!t.entry) continue;

        auto current = store::read(t);
        BackupEntry be;
        be.id = t.id;
        be.existed = current.has_value();
        be.value = current.value_or(0);
        recorded.push_back(be);
    }

    // Format timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts_buf[64];
    swprintf_s(ts_buf, L"%04u-%02u-%02u_%02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    std::wstring timestamp = ts_buf;

    // Serialize to plain text (.nsub)
    std::ostringstream ss;
    ss << "NETSHUTUP_BACKUP_V1\n";
    ss << "PROFILE " << wide_to_utf8(profile_name) << "\n";
    ss << "TIMESTAMP " << wide_to_utf8(timestamp) << "\n";
    ss << "ENTRIES " << recorded.size() << "\n";

    for (const auto& be : recorded) {
        ss << wide_to_utf8(be.id) << " "
           << (be.existed ? "1" : "0") << " "
           << be.value << "\n";
    }
    ss << "END\n";

    std::string serialized = ss.str();

    // Write file to disk
    if (!write_text_file(path, serialized)) {
        return false;
    }

    // ── Verification Step ─────────────────────────────────────────────────────
    // "Yedek diske yazılıp doğrulanmadan hiçbir değişiklik başlamaz."
    // Read the file back and verify every entry matches what was recorded.
    auto loaded = load(path);
    if (!loaded.has_value()) {
        return false;
    }

    if (loaded->profile != profile_name) {
        return false;
    }

    if (loaded->entries.size() != recorded.size()) {
        return false;
    }

    for (std::size_t i = 0; i < recorded.size(); ++i) {
        const auto& r = recorded[i];
        const auto& l = loaded->entries[i];
        if (r.id != l.id) return false;
        if (r.existed != l.existed) return false;
        if (r.existed && r.value != l.value) return false;
    }

    return true;
}

// ── load ──────────────────────────────────────────────────────────────────────

std::optional<Backup> load(const std::wstring& path) {
    auto content_opt = read_text_file(path);
    if (!content_opt.has_value()) {
        return std::nullopt;
    }

    std::istringstream ss(*content_opt);
    std::string line;

    // Line 1: Header
    if (!std::getline(ss, line)) return std::nullopt;
    // Trim potential carriage return
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != "NETSHUTUP_BACKUP_V1") {
        return std::nullopt;
    }

    Backup backup;
    std::size_t expected_count = 0;
    bool has_entries_header = false;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line == "END") {
            break;
        }

        if (line.rfind("PROFILE ", 0) == 0) {
            backup.profile = utf8_to_wide(line.substr(8));
        } else if (line.rfind("TIMESTAMP ", 0) == 0) {
            backup.timestamp = utf8_to_wide(line.substr(10));
        } else if (line.rfind("ENTRIES ", 0) == 0) {
            expected_count = static_cast<std::size_t>(std::stoul(line.substr(8)));
            has_entries_header = true;
        } else {
            // Entry line: <id> <existed: 1 or 0> <value>
            std::istringstream line_ss(line);
            std::string id_str, existed_str;
            DWORD val = 0;
            if (!(line_ss >> id_str >> existed_str >> val)) {
                return std::nullopt;
            }

            BackupEntry be;
            be.id = utf8_to_wide(id_str);
            be.existed = (existed_str == "1");
            be.value = val;
            backup.entries.push_back(be);
        }
    }

    if (!has_entries_header || backup.entries.size() != expected_count) {
        return std::nullopt;
    }

    return backup;
}

// ── restore ───────────────────────────────────────────────────────────────────

bool restore_with_lookup(const Backup& backup, LookupFn lookup) {
    if (!lookup) return false;

    for (const auto& item : backup.entries) {
        // Per-adapter ids look like "<entry id>@{GUID}".
        const auto parts = targets::split_id(item.id);
        const auto* entry = lookup(parts.first.c_str());
        if (!entry) {
            // Setting is no longer in catalog; skip it safely.
            continue;
        }
        const bool per_adapter = (entry->scope == catalog::Scope::PER_ADAPTER);
        if (per_adapter == parts.second.empty()) {
            continue;   // malformed: a GUID on a global entry, or a per-adapter entry without one
        }
        const targets::Target t = targets::make_target(*entry, parts.second);

        if (item.existed) {
            // Restore previously existing value
            if (!store::write(t, item.value)) {
                return false;
            }
        } else {
            // Restore "not set" state by deleting the value
            if (!store::remove(t)) {
                return false;
            }
        }
    }

    return true;
}

bool restore(const Backup& backup) {
    return restore_with_lookup(backup, catalog::find);
}

// ── Paths ─────────────────────────────────────────────────────────────────────

std::wstring latest_backup_path() {
    std::wstring dir = get_backups_dir();
    std::wstring pattern = dir + L"\\*.nsub";

    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return L"";
    }

    FILETIME latest_time = {0, 0};
    std::wstring latest_filename;

    do {
        if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (CompareFileTime(&ffd.ftLastWriteTime, &latest_time) > 0) {
                latest_time = ffd.ftLastWriteTime;
                latest_filename = ffd.cFileName;
            }
        }
    } while (FindNextFileW(hFind, &ffd));

    FindClose(hFind);

    if (latest_filename.empty()) {
        return L"";
    }

    return dir + L"\\" + latest_filename;
}

std::wstring new_backup_path() {
    std::wstring dir = get_backups_dir();

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t filename[128];
    swprintf_s(filename, L"backup_%04u%02u%02u_%02u%02u%02u.nsub",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);

    std::wstring full_path = dir + L"\\" + filename;
    DWORD attrs = GetFileAttributesW(full_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return full_path;
    }

    // Disambiguate if file exists in the same second
    for (int suffix = 1; suffix < 1000; ++suffix) {
        swprintf_s(filename, L"backup_%04u%02u%02u_%02u%02u%02u_%d.nsub",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond, suffix);
        full_path = dir + L"\\" + filename;
        if (GetFileAttributesW(full_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return full_path;
        }
    }

    return full_path;
}

} // namespace netshutup::backup

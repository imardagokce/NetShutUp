// targets.cpp — see targets.h.

#include "targets.h"
#include "adapters.h"

namespace netshutup::targets {

Target make_target(const catalog::Entry& entry, const std::wstring& guid) {
    Target t;
    t.entry = &entry;
    if (entry.scope == catalog::Scope::PER_ADAPTER) {
        t.id = std::wstring(entry.id) + L"@" + guid;
        t.subkey = adapters::format_subkey_for_adapter(entry.subkey, guid);
    } else {
        t.id = entry.id;
        t.subkey = entry.subkey ? entry.subkey : L"";
    }
    return t;
}

std::vector<Target> expand(const std::vector<const catalog::Entry*>& entries,
                           const std::vector<std::wstring>& adapter_guids) {
    std::vector<Target> out;
    for (const catalog::Entry* e : entries) {
        if (!e) continue;
        if (e->scope == catalog::Scope::PER_ADAPTER) {
            for (const auto& guid : adapter_guids) out.push_back(make_target(*e, guid));
        } else {
            out.push_back(make_target(*e, L""));
        }
    }
    return out;
}

std::pair<std::wstring, std::wstring> split_id(const std::wstring& id) {
    std::size_t at = id.find(L'@');
    if (at == std::wstring::npos) return {id, L""};
    return {id.substr(0, at), id.substr(at + 1)};
}

std::wstring display_label(const std::wstring& id) {
    const std::size_t at = id.find(L'@');
    if (at == std::wstring::npos) return id;
    const std::wstring guid = id.substr(at + 1);
    // "{XXXXXXXX-" is 10 characters; anything shorter is not a GUID we know how to shorten.
    if (guid.size() < 10 || guid.front() != L'{' || guid[9] != L'-') return id;
    return id.substr(0, at + 1) + guid.substr(0, 9) + L"..}";
}

std::vector<std::wstring> physical_adapter_guids() {
    std::vector<std::wstring> out;
    for (const auto& a : adapters::get_physical_adapters()) out.push_back(a.guid);
    return out;
}

} // namespace netshutup::targets

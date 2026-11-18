// store.cpp — dispatch between registry and firewall backends.

#include "store.h"
#include "firewall.h"
#include "registry.h"

namespace netshutup::store {

std::optional<DWORD> read(const catalog::Entry& e) {
    if (e.scope == catalog::Scope::FIREWALL) return firewall::read(e.subkey, e.value_name);
    return registry::read_dword(e.hive, e.subkey, e.value_name);
}

bool write(const catalog::Entry& e, DWORD value) {
    if (e.scope == catalog::Scope::FIREWALL) return firewall::write(e.subkey, e.value_name, value);
    return registry::write_dword(e.hive, e.subkey, e.value_name, value);
}

bool remove(const catalog::Entry& e) {
    if (e.scope == catalog::Scope::FIREWALL) return true;
    return registry::delete_value(e.hive, e.subkey, e.value_name);
}

std::optional<DWORD> read(const targets::Target& t) {
    if (!t.entry) return std::nullopt;
    if (t.entry->scope == catalog::Scope::FIREWALL) return read(*t.entry);
    return registry::read_dword(t.entry->hive, t.subkey.c_str(), t.entry->value_name);
}

bool write(const targets::Target& t, DWORD value) {
    if (!t.entry) return false;
    if (t.entry->scope == catalog::Scope::FIREWALL) return write(*t.entry, value);
    return registry::write_dword(t.entry->hive, t.subkey.c_str(), t.entry->value_name, value);
}

bool remove(const targets::Target& t) {
    if (!t.entry) return false;
    if (t.entry->scope == catalog::Scope::FIREWALL) return remove(*t.entry);
    return registry::delete_value(t.entry->hive, t.subkey.c_str(), t.entry->value_name);
}

} // namespace netshutup::store

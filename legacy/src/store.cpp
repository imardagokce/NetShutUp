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

} // namespace netshutup::store

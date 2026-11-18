#pragma once

// store.h — one read/write path for every catalog entry, whatever backs it.
//   Scope::GLOBAL / PER_ADAPTER -> registry (registry.h)
//   Scope::FIREWALL             -> Windows Firewall COM API (firewall.h)
//
// For FIREWALL entries: hive is unused (nullptr), subkey is the firewall
// profile ("domain"|"private"|"public") and value_name is "enabled"|"inbound".

#include "catalog.h"
#include <optional>
#include <windows.h>

namespace netshutup::store {

std::optional<DWORD> read(const catalog::Entry& e);
bool write(const catalog::Entry& e, DWORD value);

// Restores "not set". The firewall has no "not set" state (reads return the
// effective value), so for FIREWALL this is a no-op that returns true.
bool remove(const catalog::Entry& e);

} // namespace netshutup::store

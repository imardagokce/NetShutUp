#pragma once

// dns_flow.h — the user-facing DNS stage that runs after the registry/firewall part
// of a REAL apply/revert (CLI and menu). It builds the real dns::System, runs
// dns::apply_dns / dns::revert_dns and prints the outcome.
//
// Deliberately NOT called from apply::apply_profile(): tests and selftest call that
// function, and they must never install services or change adapter DNS.

#include "apply.h"

namespace netshutup::dnsflow {

// After a successful, non-dry-run apply. Needs administrator rights (apply already does).
void after_apply(const apply::ApplyResult& res, const wchar_t* profile);

// After a revert attempt. Uses the DNS side-file of the reverted backup; if that backup
// has none but our DNS service is still installed (an older apply installed it), the
// newest side-file in the backups folder is used instead.
void after_revert(const apply::RevertResult& res);

// `netshutup-legacy dns-status` (read-only): service state and the status file.
int print_status();

} // namespace netshutup::dnsflow

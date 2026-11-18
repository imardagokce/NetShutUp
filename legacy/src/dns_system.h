#pragma once

// dns_system.h — the REAL dns::System: talks to the Service Control Manager, netsh,
// the registry, Winsock and the network. Never used by unit tests (they use fakes);
// only the real apply/revert flow constructs it.
// Design: docs/design/2026-11-18-legacy-dns-stub.md

#include "dns_apply.h"

namespace netshutup::dns {

class RealSystem final : public System {
public:
    RealSystem();
    ~RealSystem() override;
    RealSystem(const RealSystem&) = delete;
    RealSystem& operator=(const RealSystem&) = delete;

    bool internet_ok() override;
    std::vector<AdapterDns> adapters() override;
    bool tls12_ok() override;
    bool port53_free() override;
    bool service_installed() override;
    std::optional<Upstream> probe_chain(const std::vector<Upstream>& chain, std::wstring& err) override;
    bool install_service(const Config& cfg, std::wstring& err) override;
    bool local_resolver_ok() override;
    bool set_dns(const AdapterDns& a, const std::vector<std::wstring>& servers, std::wstring& err) override;
    bool set_dns_automatic(const AdapterDns& a, std::wstring& err) override;
    bool remove_service(std::wstring& err) override;
    bool write_text(const std::wstring& path, const std::string& bytes) override;
    std::optional<std::string> read_text(const std::wstring& path) override;

    // "cloudflare" or "google": whichever answered a DoH query faster (Performance
    // profile ordering). Empty if neither answered. Sends a few DNS queries.
    std::wstring fastest_provider();

private:
    bool wsa_ok_ = false;
};

// Read-only: "running", "stopped", "starting", "stopping", "not installed" or "unknown".
std::wstring service_state();

} // namespace netshutup::dns

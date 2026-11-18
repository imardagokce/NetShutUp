// test_dns_apply.cpp — unit tests for the DNS stage of apply/revert (NetShutUp Legacy).
// Everything runs against FakeSystem: no netsh, registry, services or file I/O.

#include "dns_apply.h"
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace netshutup::dns;

static int g_failed = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

namespace {

using Servers = std::vector<std::wstring>;
using StateOpt = std::optional<Servers>;

const std::wstring kGuidA = L"{11111111-1111-1111-1111-111111111111}";
const std::wstring kGuidB = L"{22222222-2222-2222-2222-222222222222}";
const std::wstring kBackupPath = L"C:\\fake\\backup.nsub";
const std::wstring kSidePath = L"C:\\fake\\backup.nsub.dns";
const std::wstring kLoopback = L"127.0.0.1";
const std::wstring kStaticSrv = L"192.168.1.1";

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}

std::string join(const Servers& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i) out += ",";
        out += narrow(s[i]);
    }
    return out;
}

Upstream up(const wchar_t* id, Method m) {
    Upstream u;
    u.provider_id = id;
    u.method = m;
    return u;
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.rfind(p, 0) == 0;
}

// Calls that change the machine (anything that is not a pure query).
bool touched_machine(const std::vector<std::string>& log) {
    for (const auto& c : log) {
        if (starts_with(c, "write_text") || starts_with(c, "install_service") ||
            starts_with(c, "set_dns") || starts_with(c, "remove_service"))
            return true;
    }
    return false;
}

class FakeSystem : public System {
public:
    std::vector<std::string> log;

    bool fails_this_dns_call() {
        ++dns_calls;
        return std::find(fail_dns_calls.begin(), fail_dns_calls.end(), dns_calls) != fail_dns_calls.end();
    }

    // Scripted internet answers for the first calls; afterwards `internet`.
    bool internet = true;
    std::vector<bool> internet_answers;
    size_t internet_calls = 0;

    bool tls12 = true;
    bool port53 = true;
    bool service = false;
    bool local_ok = true;

    bool install_ok = true;
    std::wstring install_err;
    bool remove_ok = true;

    std::optional<Upstream> probe_result;
    std::wstring probe_err;

    bool write_fail = false;
    bool read_corrupt = false;

    // 1-based numbers of the set_dns / set_dns_automatic calls that fail.
    std::vector<size_t> fail_dns_calls;
    size_t dns_calls = 0;

    std::vector<AdapterDns> adapter_list;
    std::map<std::wstring, StateOpt> dns_state;        // current per-adapter DNS (nullopt = automatic)
    std::map<std::wstring, std::string> files;         // in-memory file map
    std::optional<Config> installed_cfg;

    bool internet_ok() override {
        log.push_back("internet_ok");
        bool v = internet_calls < internet_answers.size() ? internet_answers[internet_calls] : internet;
        ++internet_calls;
        return v;
    }
    std::vector<AdapterDns> adapters() override {
        log.push_back("adapters");
        return adapter_list;
    }
    bool tls12_ok() override {
        log.push_back("tls12_ok");
        return tls12;
    }
    bool port53_free() override {
        log.push_back("port53_free");
        return port53;
    }
    bool service_installed() override {
        log.push_back("service_installed");
        return service;
    }
    std::optional<Upstream> probe_chain(const std::vector<Upstream>&, std::wstring& err) override {
        log.push_back("probe_chain");
        err = probe_err;
        return probe_result;
    }
    bool install_service(const Config& cfg, std::wstring& err) override {
        log.push_back("install_service");
        installed_cfg = cfg;
        err = install_err;
        return install_ok;
    }
    bool local_resolver_ok() override {
        log.push_back("local_resolver_ok");
        return local_ok;
    }
    bool set_dns(const AdapterDns& a, const std::vector<std::wstring>& servers, std::wstring& err) override {
        log.push_back("set_dns " + narrow(a.guid) + " " + join(servers));
        if (fails_this_dns_call()) {
            err = L"injected failure";
            return false;
        }
        dns_state[a.guid] = servers;
        return true;
    }
    bool set_dns_automatic(const AdapterDns& a, std::wstring& err) override {
        log.push_back("set_dns_automatic " + narrow(a.guid));
        if (fails_this_dns_call()) {
            err = L"injected failure";
            return false;
        }
        dns_state[a.guid] = std::nullopt;
        return true;
    }
    bool remove_service(std::wstring& err) override {
        log.push_back("remove_service");
        err = L"";
        if (remove_ok) service = false;
        return remove_ok;
    }
    bool write_text(const std::wstring& path, const std::string& bytes) override {
        log.push_back("write_text " + narrow(path));
        if (write_fail) return false;
        files[path] = bytes;
        return true;
    }
    std::optional<std::string> read_text(const std::wstring& path) override {
        log.push_back("read_text " + narrow(path));
        auto it = files.find(path);
        if (it == files.end()) return std::nullopt;
        if (read_corrupt) return std::string("NETSHUTUP_DNS_BACKUP_V1\nEND\n");  // parses, but differs
        return it->second;
    }
};

void add_adapter(FakeSystem& f, const std::wstring& guid, unsigned long idx, StateOpt static_list,
                 const Servers& effective) {
    AdapterDns a;
    a.guid = guid;
    a.if_index = idx;
    a.static_servers = static_list;
    a.effective_servers = effective;
    f.adapter_list.push_back(a);
    f.dns_state[guid] = static_list;
}

// Two adapters: A static {192.168.1.1}, B automatic (DHCP gives 10.0.0.1).
// Probe answers quad9 over DoT. Everything else healthy.
void setup_happy(FakeSystem& f) {
    add_adapter(f, kGuidA, 5, Servers{kStaticSrv}, Servers{kStaticSrv});
    add_adapter(f, kGuidB, 7, std::nullopt, Servers{L"10.0.0.1"});
    f.probe_result = up(L"quad9", Method::DoT);
}

std::vector<std::string> with(std::vector<std::string> base, std::initializer_list<std::string> more) {
    base.insert(base.end(), more.begin(), more.end());
    return base;
}

// Calls made before any mutation (rules 2-9 up to and including the side-file read-back).
std::vector<std::string> prefix_log() {
    return {"service_installed", "internet_ok", "adapters", "tls12_ok", "port53_free", "probe_chain",
            "write_text " + narrow(kSidePath), "read_text " + narrow(kSidePath)};
}

std::vector<std::string> happy_log() {
    return with(prefix_log(), {"install_service", "local_resolver_ok",
                               "set_dns " + narrow(kGuidA) + " 127.0.0.1",
                               "set_dns " + narrow(kGuidB) + " 127.0.0.1", "internet_ok"});
}

bool check_log(const FakeSystem& f, const std::vector<std::string>& expected) {
    if (f.log == expected) return true;
    wprintf(L"  call log mismatch, actual (%zu calls):\n", f.log.size());
    for (const auto& c : f.log) wprintf(L"    %hs\n", c.c_str());
    return false;
}

void check_skipped(const DnsResult& r, const wchar_t* msg, const FakeSystem& f) {
    EXPECT(r.outcome == DnsOutcome::Skipped);
    EXPECT(r.message == msg);
    EXPECT(!r.upstream.has_value());
    EXPECT(!r.rolled_back);
    EXPECT(!touched_machine(f.log));
}

std::string backup_text() {
    DnsBackup b;
    AdapterBackup a;
    a.guid = kGuidA;
    a.if_index = 5;
    a.static_servers = Servers{kStaticSrv};
    AdapterBackup d;
    d.guid = kGuidB;
    d.if_index = 7;
    d.static_servers = std::nullopt;
    b.adapters = {a, d};
    return serialize_backup(b);
}

// ---------------------------------------------------------------- apply: success

static void test_happy_path_privacy() {
    FakeSystem f;
    setup_happy(f);
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);

    EXPECT(r.outcome == DnsOutcome::Applied);
    EXPECT(r.upstream.has_value() && *r.upstream == up(L"quad9", Method::DoT));
    EXPECT(r.message == L"Encrypted DNS active: quad9 over dot via the local resolver (127.0.0.1)");
    EXPECT(!r.rolled_back);
    EXPECT(check_log(f, happy_log()));

    // Config handed to the service.
    EXPECT(f.installed_cfg.has_value());
    if (f.installed_cfg) {
        const Config& c = *f.installed_cfg;
        std::vector<Upstream> want = {up(L"quad9", Method::DoT), up(L"cloudflare", Method::DoH),
                                      up(L"cloudflare", Method::DoT), up(L"quad9", Method::DoH)};
        EXPECT(c.chain == want);
        EXPECT(c.legacy_dns == (Servers{kStaticSrv, L"10.0.0.1"}));
        EXPECT_EQ(c.fail_threshold, 3u);
    }

    // Side-file written at backup_path + ".dns" and parses back to what was expected.
    auto it = f.files.find(kSidePath);
    EXPECT(it != f.files.end());
    if (it != f.files.end()) {
        auto parsed = parse_backup(it->second);
        EXPECT(parsed.has_value());
        if (parsed) {
            DnsBackup want;
            AdapterBackup a{kGuidA, 5, Servers{kStaticSrv}};
            AdapterBackup b{kGuidB, 7, std::nullopt};
            want.adapters = {a, b};
            EXPECT(*parsed == want);
        }
    }

    // Both adapters now point at the local resolver.
    EXPECT(f.dns_state[kGuidA] == StateOpt(Servers{kLoopback}));
    EXPECT(f.dns_state[kGuidB] == StateOpt(Servers{kLoopback}));
}

static void test_chain_rotation_cloudflare_doh() {
    FakeSystem f;
    setup_happy(f);
    f.probe_result = up(L"cloudflare", Method::DoH);
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);

    EXPECT(r.outcome == DnsOutcome::Applied);
    EXPECT(r.upstream.has_value() && *r.upstream == up(L"cloudflare", Method::DoH));
    EXPECT(r.message == L"Encrypted DNS active: cloudflare over doh via the local resolver (127.0.0.1)");
    EXPECT(f.installed_cfg.has_value());
    if (f.installed_cfg) {
        std::vector<Upstream> want = {up(L"cloudflare", Method::DoH), up(L"cloudflare", Method::DoT),
                                      up(L"quad9", Method::DoH), up(L"quad9", Method::DoT)};
        EXPECT(f.installed_cfg->chain == want);
    }
}

static void test_legacy_dns_dedup_and_ipv4_only() {
    FakeSystem f;
    setup_happy(f);
    // A: static list plus junk; B: DHCP with a duplicate and an IPv6 address.
    f.adapter_list[0].effective_servers = Servers{kStaticSrv, L"not-an-ip", L"8.8.8.8"};
    f.adapter_list[1].effective_servers = Servers{L"8.8.8.8", L"10.0.0.1", kStaticSrv, L"fe80::1"};
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);

    EXPECT(r.outcome == DnsOutcome::Applied);
    EXPECT(f.installed_cfg.has_value());
    if (f.installed_cfg) {
        EXPECT(f.installed_cfg->legacy_dns == (Servers{kStaticSrv, L"8.8.8.8", L"10.0.0.1"}));
    }
}

// ---------------------------------------------------------------- apply: skips (rules 1-7)

static void test_rule1_unknown_profile() {
    FakeSystem f;
    setup_happy(f);
    DnsResult r = apply_dns(L"bogus", kBackupPath, f);
    check_skipped(r, L"unknown profile", f);
    EXPECT(f.log.empty());
}

static void test_rule2_service_installed() {
    FakeSystem f;
    setup_happy(f);
    f.service = true;
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);
    check_skipped(r, L"DNS service is already installed; revert first", f);
    EXPECT(check_log(f, {"service_installed"}));
}

static void test_rule3_no_internet() {
    FakeSystem f;
    setup_happy(f);
    f.internet = false;
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);
    check_skipped(r, L"no internet connection; DNS left untouched", f);
    EXPECT(check_log(f, {"service_installed", "internet_ok"}));
}

static void test_rule4_no_adapters() {
    FakeSystem f;
    setup_happy(f);
    f.adapter_list.clear();
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);
    check_skipped(r, L"no physical network adapter found", f);
    EXPECT(check_log(f, {"service_installed", "internet_ok", "adapters"}));
}

static void test_rule5_no_tls12() {
    FakeSystem f;
    setup_happy(f);
    f.tls12 = false;
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);
    check_skipped(r,
                  L"TLS 1.2 is not available (Windows 7 needs update KB3140245); DNS left untouched", f);
    EXPECT(check_log(f, {"service_installed", "internet_ok", "adapters", "tls12_ok"}));
}

static void test_rule6_port53_busy() {
    FakeSystem f;
    setup_happy(f);
    f.port53 = false;
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);
    check_skipped(r, L"port 53 on 127.0.0.1 is in use; DNS left untouched", f);
    EXPECT(check_log(f, {"service_installed", "internet_ok", "adapters", "tls12_ok", "port53_free"}));
}

static void test_rule7_no_working_method() {
    FakeSystem f;
    setup_happy(f);
    f.probe_result = std::nullopt;
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);
    check_skipped(r, L"no encrypted DNS method works on this network; original DNS kept", f);
    EXPECT(check_log(f, {"service_installed", "internet_ok", "adapters", "tls12_ok", "port53_free",
                         "probe_chain"}));
}

static void test_rule7_probe_error_appended() {
    FakeSystem f;
    setup_happy(f);
    f.probe_result = std::nullopt;
    f.probe_err = L"timeout";
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);
    check_skipped(r, L"no encrypted DNS method works on this network; original DNS kept (timeout)", f);
}

// ---------------------------------------------------------------- apply: side-file (rule 9)

static void test_rule9_write_failure() {
    FakeSystem f;
    setup_happy(f);
    f.write_fail = true;
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);
    EXPECT(r.outcome == DnsOutcome::Skipped);
    EXPECT(r.message == L"could not write the DNS backup; DNS left untouched");
    EXPECT(check_log(f, {"service_installed", "internet_ok", "adapters", "tls12_ok", "port53_free",
                         "probe_chain", "write_text " + narrow(kSidePath)}));
}

static void test_rule9_readback_mismatch() {
    FakeSystem f;
    setup_happy(f);
    f.read_corrupt = true;
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);
    EXPECT(r.outcome == DnsOutcome::Skipped);
    EXPECT(r.message == L"could not write the DNS backup; DNS left untouched");
    EXPECT(!r.rolled_back);
    EXPECT(check_log(f, with(prefix_log(), {"write_text " + narrow(kSidePath)})));   // side-file neutralised
}

// ---------------------------------------------------------------- apply: failures and rollback

static void test_rule10_install_failure() {
    FakeSystem f;
    setup_happy(f);
    f.install_ok = false;
    f.install_err = L"disk full";
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);
    EXPECT(r.outcome == DnsOutcome::Failed);
    EXPECT(r.rolled_back);
    EXPECT(!r.upstream.has_value());
    EXPECT(r.message == L"could not install the DNS service: disk full");
    EXPECT(check_log(f, with(prefix_log(), {"install_service", "remove_service", "write_text " + narrow(kSidePath)})));
    EXPECT(f.dns_state[kGuidA] == StateOpt(Servers{kStaticSrv}));
}

static void test_rule11_resolver_not_answering() {
    FakeSystem f;
    setup_happy(f);
    f.local_ok = false;
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);
    EXPECT(r.outcome == DnsOutcome::Failed);
    EXPECT(r.rolled_back);
    EXPECT(r.message == L"the local DNS resolver did not answer; everything was rolled back");
    EXPECT(check_log(f, with(prefix_log(), {"install_service", "local_resolver_ok", "remove_service", "write_text " + narrow(kSidePath)})));
}

static void test_rule12_second_adapter_fails_rolled_back() {
    FakeSystem f;
    setup_happy(f);
    f.fail_dns_calls = {2};  // set_dns on B fails
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);

    EXPECT(r.outcome == DnsOutcome::Failed);
    EXPECT(r.rolled_back);
    EXPECT(r.message.find(L"rolled back") != std::wstring::npos);
    EXPECT(r.message.find(L"ROLLBACK INCOMPLETE") == std::wstring::npos);
    EXPECT(check_log(f, with(prefix_log(), {"install_service", "local_resolver_ok",
                                            "set_dns " + narrow(kGuidA) + " 127.0.0.1",
                                            "set_dns " + narrow(kGuidB) + " 127.0.0.1",
                                            "set_dns " + narrow(kGuidA) + " " + narrow(kStaticSrv),
                                            "set_dns_automatic " + narrow(kGuidB), "remove_service", "write_text " + narrow(kSidePath)})));
    // Original DNS back on both adapters.
    EXPECT(f.dns_state[kGuidA] == StateOpt(Servers{kStaticSrv}));
    EXPECT(f.dns_state[kGuidB] == StateOpt(std::nullopt));
}

static void test_rule12_restore_fails_incomplete() {
    FakeSystem f;
    setup_happy(f);
    f.fail_dns_calls = {2, 3};  // set_dns on B fails, then the restore of A fails
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);

    EXPECT(r.outcome == DnsOutcome::Failed);
    EXPECT(!r.rolled_back);
    EXPECT(r.message.find(L"ROLLBACK INCOMPLETE") != std::wstring::npos);
    // Restores keep going after a failure and remove_service is still attempted.
    EXPECT(check_log(f, with(prefix_log(), {"install_service", "local_resolver_ok",
                                            "set_dns " + narrow(kGuidA) + " 127.0.0.1",
                                            "set_dns " + narrow(kGuidB) + " 127.0.0.1",
                                            "set_dns " + narrow(kGuidA) + " " + narrow(kStaticSrv),
                                            "set_dns_automatic " + narrow(kGuidB), "remove_service"})));
    EXPECT(f.dns_state[kGuidA] == StateOpt(Servers{kLoopback}));
    EXPECT(f.dns_state[kGuidB] == StateOpt(std::nullopt));
}

static void test_rule13_internet_lost_after_switch() {
    FakeSystem f;
    setup_happy(f);
    f.internet_answers = {true, false};  // rule 3 ok, check after the switch fails
    DnsResult r = apply_dns(L"privacy", kBackupPath, f);

    EXPECT(r.outcome == DnsOutcome::Failed);
    EXPECT(r.rolled_back);
    EXPECT(r.message == L"internet stopped working after the DNS switch; everything was rolled back");
    EXPECT(check_log(f, with(prefix_log(), {"install_service", "local_resolver_ok",
                                            "set_dns " + narrow(kGuidA) + " 127.0.0.1",
                                            "set_dns " + narrow(kGuidB) + " 127.0.0.1", "internet_ok",
                                            "set_dns " + narrow(kGuidA) + " " + narrow(kStaticSrv),
                                            "set_dns_automatic " + narrow(kGuidB), "remove_service", "write_text " + narrow(kSidePath)})));
    EXPECT(f.dns_state[kGuidA] == StateOpt(Servers{kStaticSrv}));
    EXPECT(f.dns_state[kGuidB] == StateOpt(std::nullopt));
}

// ---------------------------------------------------------------- revert

static void test_revert_no_sidecar() {
    FakeSystem f;
    DnsRevertResult r = revert_dns(kBackupPath, f);
    EXPECT(r.ok);
    EXPECT(!r.had_dns);
    EXPECT(r.message.empty());
    EXPECT(check_log(f, {"read_text " + narrow(kSidePath)}));
}

static void test_revert_corrupt_sidecar() {
    FakeSystem f;
    f.files[kSidePath] = "garbage";
    DnsRevertResult r = revert_dns(kBackupPath, f);
    EXPECT(!r.ok);
    EXPECT(r.had_dns);
    EXPECT(r.message == L"DNS backup is corrupt");
    EXPECT(check_log(f, {"read_text " + narrow(kSidePath)}));
}

static void setup_revert(FakeSystem& f) {
    f.files[kSidePath] = backup_text();
    f.dns_state[kGuidA] = StateOpt(Servers{kLoopback});
    f.dns_state[kGuidB] = StateOpt(Servers{kLoopback});
}

static std::vector<std::string> revert_log() {
    return {"read_text " + narrow(kSidePath), "set_dns " + narrow(kGuidA) + " " + narrow(kStaticSrv),
            "set_dns_automatic " + narrow(kGuidB), "remove_service"};
}

static void test_revert_valid() {
    FakeSystem f;
    setup_revert(f);
    DnsRevertResult r = revert_dns(kBackupPath, f);
    EXPECT(r.ok);
    EXPECT(r.had_dns);
    EXPECT(r.message == L"DNS restored");
    EXPECT(check_log(f, revert_log()));
    EXPECT(f.dns_state[kGuidA] == StateOpt(Servers{kStaticSrv}));
    EXPECT(f.dns_state[kGuidB] == StateOpt(std::nullopt));
}

static void test_revert_one_restore_fails_continues() {
    FakeSystem f;
    setup_revert(f);
    f.fail_dns_calls = {1};  // restore of A fails
    DnsRevertResult r = revert_dns(kBackupPath, f);
    EXPECT(!r.ok);
    EXPECT(r.had_dns);
    EXPECT(!r.message.empty());
    EXPECT(check_log(f, revert_log()));  // B and remove_service still attempted
}

static void test_revert_idempotent() {
    FakeSystem f;
    setup_revert(f);
    DnsRevertResult first = revert_dns(kBackupPath, f);
    std::vector<std::string> log1 = f.log;
    f.log.clear();
    DnsRevertResult second = revert_dns(kBackupPath, f);

    EXPECT(first.ok && second.ok);
    EXPECT(first.message == second.message);
    EXPECT(check_log(f, revert_log()));
    EXPECT(log1 == revert_log());
    EXPECT(f.files.count(kSidePath) == 1);
}

} // namespace

int main() {
    wprintf(L"Running DNS apply/revert tests...\n");

    test_happy_path_privacy();
    test_chain_rotation_cloudflare_doh();
    test_legacy_dns_dedup_and_ipv4_only();

    test_rule1_unknown_profile();
    test_rule2_service_installed();
    test_rule3_no_internet();
    test_rule4_no_adapters();
    test_rule5_no_tls12();
    test_rule6_port53_busy();
    test_rule7_no_working_method();
    test_rule7_probe_error_appended();

    test_rule9_write_failure();
    test_rule9_readback_mismatch();

    test_rule10_install_failure();
    test_rule11_resolver_not_answering();
    test_rule12_second_adapter_fails_rolled_back();
    test_rule12_restore_fails_incomplete();
    test_rule13_internet_lost_after_switch();

    test_revert_no_sidecar();
    test_revert_corrupt_sidecar();
    test_revert_valid();
    test_revert_one_restore_fails_continues();
    test_revert_idempotent();

    if (g_failed == 0) {
        wprintf(L"All DNS apply tests passed.\n");
        return EXIT_SUCCESS;
    }
    wprintf(L"%d DNS apply test(s) FAILED.\n", g_failed);
    return EXIT_FAILURE;
}

// test_dns_backup.cpp — Unit tests for the DNS side-file format and netsh builders (NetShutUp Legacy).
// Pure tests: nothing here runs netsh or touches system settings.

#include "dns_backup.h"
#include <cstdio>
#include <cstdlib>
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

static const std::string kHeader = "NETSHUTUP_DNS_BACKUP_V1\n";
static const std::string kGuid1  = "{B92C2366-D0BB-4241-8664-9D97B8964720}";
static const std::string kGuid2  = "{11112222-3333-4444-5555-666677778888}";

static DnsBackup sample_backup() {
    AdapterBackup a1;
    a1.guid = L"{B92C2366-D0BB-4241-8664-9D97B8964720}";
    a1.if_index = 5;
    a1.static_servers = std::vector<std::wstring>{L"192.168.1.1", L"8.8.8.8"};

    AdapterBackup a2;
    a2.guid = L"{11112222-3333-4444-5555-666677778888}";
    a2.if_index = 7;  // static_servers = nullopt -> DHCP

    DnsBackup b;
    b.adapters = {a1, a2};
    return b;
}

static bool rejects(const std::string& text) {
    return !parse_backup(text).has_value();
}

// ── serialize ────────────────────────────────────────────────────────────────

static void test_serialize_exact() {
    const std::string expected_sample =
        "NETSHUTUP_DNS_BACKUP_V1\n"
        "ADAPTER {B92C2366-D0BB-4241-8664-9D97B8964720} 5 static 192.168.1.1,8.8.8.8\n"
        "ADAPTER {11112222-3333-4444-5555-666677778888} 7 dhcp\n"
        "END\n";
    EXPECT_EQ(serialize_backup(sample_backup()), expected_sample);

    const std::string expected_empty = "NETSHUTUP_DNS_BACKUP_V1\nEND\n";
    EXPECT_EQ(serialize_backup(DnsBackup{}), expected_empty);
}

// ── round trip ───────────────────────────────────────────────────────────────

static void test_round_trip() {
    DnsBackup sample = sample_backup();
    auto p1 = parse_backup(serialize_backup(sample));
    EXPECT(p1.has_value());
    if (p1) EXPECT(*p1 == sample);

    DnsBackup empty;
    auto p2 = parse_backup(serialize_backup(empty));
    EXPECT(p2.has_value());
    if (p2) EXPECT(*p2 == empty);
}

// ── parse: accepted inputs ───────────────────────────────────────────────────

static void test_parse_accepts() {
    // CRLF line endings
    std::string crlf =
        "NETSHUTUP_DNS_BACKUP_V1\r\n"
        "ADAPTER " + kGuid1 + " 5 static 192.168.1.1,8.8.8.8\r\n"
        "ADAPTER " + kGuid2 + " 7 dhcp\r\n"
        "END\r\n";
    auto p = parse_backup(crlf);
    EXPECT(p.has_value());
    if (p) EXPECT(*p == sample_backup());

    // Zero ADAPTER lines
    auto z = parse_backup(kHeader + "END\n");
    EXPECT(z.has_value());
    if (z) EXPECT(z->adapters.empty());

    // Maximum 32-bit interface index
    auto m = parse_backup(kHeader + "ADAPTER " + kGuid1 + " 4294967295 dhcp\nEND\n");
    EXPECT(m.has_value());
    if (m) EXPECT_EQ(m->adapters[0].if_index, 4294967295UL);
}

// ── parse: rejected inputs ───────────────────────────────────────────────────

static void test_parse_rejects_structure() {
    EXPECT(rejects(""));
    EXPECT(rejects("NETSHUTUP_DNS_BACKUP_V2\nEND\n"));                  // wrong header
    EXPECT(rejects("END\n"));                                           // header missing
    EXPECT(rejects(kHeader));                                           // missing END
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5 dhcp\n"));       // missing END
    EXPECT(rejects(kHeader + "END\nADAPTER " + kGuid1 + " 5 dhcp\n"));  // content after END
    EXPECT(rejects(kHeader + "END\njunk\n"));                           // content after END
    EXPECT(rejects(kHeader + "END\n\n"));                               // blank line after END
    EXPECT(rejects(kHeader + "FOO " + kGuid1 + " 5 dhcp\nEND\n"));      // unknown keyword
    EXPECT(rejects(kHeader + "END \n"));                                // END with junk
}

static void test_parse_rejects_guid() {
    // no braces
    EXPECT(rejects(kHeader + "ADAPTER B92C2366-D0BB-4241-8664-9D97B8964720 5 dhcp\nEND\n"));
    // wrong group lengths
    EXPECT(rejects(kHeader + "ADAPTER {B92C2366-D0BB-4241-8664-9D97B896472} 5 dhcp\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER {B92C2366-D0BB-424-8664-9D97B8964720} 5 dhcp\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER {B92C2366-D0BB-4241-8664-9D97B89647200} 5 dhcp\nEND\n"));
    // non-hex
    EXPECT(rejects(kHeader + "ADAPTER {B92C2366-D0BB-4241-8664-9D97B896472Z} 5 dhcp\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER {B92C2366-D0BB-4241-8664-9D97B896472G} 5 dhcp\nEND\n"));
}

static void test_parse_rejects_index() {
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 0 dhcp\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " -1 dhcp\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " +5 dhcp\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5a dhcp\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 4294967296 dhcp\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 99999999999999999999 dhcp\nEND\n"));
}

static void test_parse_rejects_mode_and_servers() {
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5 manual\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5 static\nEND\n"));          // no list
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5 static 1.2.3.4,\nEND\n")); // empty entry
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5 static ,1.2.3.4\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5 static 1.2.3.4,,8.8.8.8\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5 static 256.1.1.1\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5 static 1.2.3\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5 static 1.2.3.4.5\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5 static a.b.c.d\nEND\n"));
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5 static 1.2.3.4 8.8.8.8\nEND\n")); // space-separated
    EXPECT(rejects(kHeader + "ADAPTER " + kGuid1 + " 5 dhcp 1.2.3.4\nEND\n"));           // dhcp + extra
}

static void test_parse_rejects_duplicate_guid() {
    EXPECT(rejects(kHeader +
                   "ADAPTER " + kGuid1 + " 5 dhcp\n" +
                   "ADAPTER " + kGuid1 + " 6 dhcp\n" +
                   "END\n"));
}

// ── is_ipv4 ──────────────────────────────────────────────────────────────────

static void test_is_ipv4() {
    EXPECT(is_ipv4(L"0.0.0.0"));
    EXPECT(is_ipv4(L"255.255.255.255"));
    EXPECT(is_ipv4(L"10.0.0.1"));
    EXPECT(is_ipv4(L"01.2.3.4"));       // leading zeros accepted
    EXPECT(is_ipv4(L"001.002.003.004"));

    EXPECT(!is_ipv4(L""));
    EXPECT(!is_ipv4(L"1.2.3"));
    EXPECT(!is_ipv4(L"1.2.3.4 "));
    EXPECT(!is_ipv4(L" 1.2.3.4"));
    EXPECT(!is_ipv4(L"1.2.3.256"));
    EXPECT(!is_ipv4(L"256.1.1.1"));
    EXPECT(!is_ipv4(L"1..3.4"));
    EXPECT(!is_ipv4(L"1.2.3.4.5"));
    EXPECT(!is_ipv4(L"a.b.c.d"));
    EXPECT(!is_ipv4(L"1.2.3.1234"));    // more than 3 digits
    EXPECT(!is_ipv4(L"1.2.3.4."));
    EXPECT(!is_ipv4(L".1.2.3.4"));
    EXPECT(!is_ipv4(L"1.2.3.-4"));
}

// ── netsh builders ───────────────────────────────────────────────────────────

static void test_builders_exact() {
    EXPECT_EQ(netsh_set_static_args(5, L"127.0.0.1"),
              std::wstring(L"interface ipv4 set dnsservers name=5 source=static address=127.0.0.1 register=primary"));
    EXPECT_EQ(netsh_add_args(5, L"8.8.4.4", 2),
              std::wstring(L"interface ipv4 add dnsservers name=5 address=8.8.4.4 index=2"));
    EXPECT_EQ(netsh_set_dhcp_args(5),
              std::wstring(L"interface ipv4 set dnsservers name=5 source=dhcp"));
}

static void test_builders_reject() {
    EXPECT(netsh_set_static_args(0, L"127.0.0.1").empty());
    EXPECT(netsh_set_static_args(5, L"1.2.3").empty());
    EXPECT(netsh_set_static_args(5, L"").empty());

    EXPECT(netsh_add_args(0, L"8.8.4.4", 2).empty());
    EXPECT(netsh_add_args(5, L"1.2.3.256", 2).empty());
    EXPECT(netsh_add_args(5, L"8.8.4.4", 1).empty());
    EXPECT(netsh_add_args(5, L"8.8.4.4", 0).empty());

    EXPECT(netsh_set_dhcp_args(0).empty());
}

static void test_restore_commands() {
    const std::vector<std::wstring> servers{L"1.1.1.1", L"8.8.8.8", L"9.9.9.9"};
    auto cmds = netsh_restore_commands(5, servers);
    EXPECT_EQ(cmds.size(), static_cast<size_t>(3));
    if (cmds.size() == 3) {
        EXPECT_EQ(cmds[0], std::wstring(L"interface ipv4 set dnsservers name=5 source=static address=1.1.1.1 register=primary"));
        EXPECT_EQ(cmds[1], std::wstring(L"interface ipv4 add dnsservers name=5 address=8.8.8.8 index=2"));
        EXPECT_EQ(cmds[2], std::wstring(L"interface ipv4 add dnsservers name=5 address=9.9.9.9 index=3"));
    }

    auto one = netsh_restore_commands(5, std::vector<std::wstring>{L"1.1.1.1"});
    EXPECT_EQ(one.size(), static_cast<size_t>(1));
    if (one.size() == 1) {
        EXPECT_EQ(one[0], std::wstring(L"interface ipv4 set dnsservers name=5 source=static address=1.1.1.1 register=primary"));
    }

    EXPECT(netsh_restore_commands(5, std::vector<std::wstring>{}).empty());
    EXPECT(netsh_restore_commands(5, std::vector<std::wstring>{L"1.1.1.1", L"bad", L"9.9.9.9"}).empty());
    EXPECT(netsh_restore_commands(5, std::vector<std::wstring>{L"1.1.1.1", L""}).empty());
    EXPECT(netsh_restore_commands(0, servers).empty());
}

int main() {
    wprintf(L"Running DNS backup tests...\n");

    test_serialize_exact();
    test_round_trip();
    test_parse_accepts();
    test_parse_rejects_structure();
    test_parse_rejects_guid();
    test_parse_rejects_index();
    test_parse_rejects_mode_and_servers();
    test_parse_rejects_duplicate_guid();
    test_is_ipv4();
    test_builders_exact();
    test_builders_reject();
    test_restore_commands();

    if (g_failed == 0) {
        wprintf(L"All DNS backup tests passed.\n");
        return EXIT_SUCCESS;
    } else {
        wprintf(L"%d DNS backup test(s) FAILED.\n", g_failed);
        return EXIT_FAILURE;
    }
}

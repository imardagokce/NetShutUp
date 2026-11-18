// test_adapters.cpp — Unit tests for the physical adapter module (NetShutUp Legacy)

#include "adapters.h"
#include <cstdio>
#include <cstdlib>

using namespace netshutup;

static int g_failed = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

static void test_format_subkey() {
    std::wstring tpl = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\{ADAPTER-GUID}";
    std::wstring guid = L"{11112222-3333-4444-5555-666677778888}";

    std::wstring formatted = adapters::format_subkey_for_adapter(tpl.c_str(), guid);
    std::wstring expected = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\{11112222-3333-4444-5555-666677778888}";

    EXPECT_EQ(formatted, expected);

    // Template without token
    std::wstring plain = L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\DNSClient";
    EXPECT_EQ(adapters::format_subkey_for_adapter(plain.c_str(), guid), plain);

    // Nullptr / empty
    EXPECT_EQ(adapters::format_subkey_for_adapter(nullptr, guid), L"");
}

static void test_enumeration_contract() {
    auto list = adapters::get_physical_adapters();

    // The host running this test may or may not have physical adapters (e.g. in a container/VM or real host).
    // The test must ensure that ANY adapter in the list strictly complies with our safety rules:
    for (const auto& nic : list) {
        EXPECT(!nic.guid.empty());
        EXPECT_EQ(nic.guid.front(), L'{');
        EXPECT_EQ(nic.guid.back(), L'}');
        EXPECT(nic.type == adapters::AdapterType::ETHERNET || nic.type == adapters::AdapterType::WIFI);

        // Verify no blacklisted terms slipped through in description or friendly name
        EXPECT(nic.description.find(L"VMware") == std::wstring::npos);
        EXPECT(nic.description.find(L"VirtualBox") == std::wstring::npos);
        EXPECT(nic.description.find(L"WAN Miniport") == std::wstring::npos);
        EXPECT(nic.description.find(L"Loopback") == std::wstring::npos);
    }
}

int main() {
    wprintf(L"Running adapter enumeration tests...\n");

    test_format_subkey();
    test_enumeration_contract();

    if (g_failed == 0) {
        wprintf(L"All adapter tests passed.\n");
        return EXIT_SUCCESS;
    } else {
        wprintf(L"%d adapter test(s) FAILED.\n", g_failed);
        return EXIT_FAILURE;
    }
}

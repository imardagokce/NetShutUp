// test_selftest_mask.cpp — Unit tests for selftest report masking (NetShutUp Legacy)

#include "selftest_mask.h"
#include <cstdio>
#include <cstdlib>

using namespace netshutup::selftest::mask;

static int g_failed = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

static void test_ipv4() {
    // Known provider addresses stay unchanged
    EXPECT_EQ(mask_ipv4(L"1.1.1.1"), L"1.1.1.1");
    EXPECT_EQ(mask_ipv4(L"1.0.0.1"), L"1.0.0.1");
    EXPECT_EQ(mask_ipv4(L"8.8.8.8"), L"8.8.8.8");
    EXPECT_EQ(mask_ipv4(L"8.8.4.4"), L"8.8.4.4");
    EXPECT_EQ(mask_ipv4(L"9.9.9.9"), L"9.9.9.9");
    EXPECT_EQ(mask_ipv4(L"149.112.112.112"), L"149.112.112.112");

    // 10.x.y.z keeps 10.0 prefix
    EXPECT_EQ(mask_ipv4(L"10.0.3.7"), L"10.0.x.x");

    // Everything else is fully masked
    EXPECT_EQ(mask_ipv4(L"192.168.1.20"), L"x.x.x.x");
    EXPECT_EQ(mask_ipv4(L"203.0.113.5"), L"x.x.x.x");

    // Masked in place inside a sentence
    EXPECT_EQ(mask_ipv4(L"fail at 192.168.0.4 now"), L"fail at x.x.x.x now");

    // Version strings are not IPv4 addresses
    EXPECT_EQ(mask_ipv4(L"6.1.7601"), L"6.1.7601");
    // 4 groups but build-number shaped: may be masked or kept, must not crash
    std::wstring build = mask_ipv4(L"10.0.19045.1");
    EXPECT(!build.empty());

    // Odd input
    EXPECT_EQ(mask_ipv4(L""), L"");
    EXPECT_EQ(mask_ipv4(L"1.2.3"), L"1.2.3");
    EXPECT_EQ(mask_ipv4(L"..."), L"...");
}

static void test_ipv6() {
    EXPECT_EQ(mask_ipv6(L"fe80::1c2:3ff:fe4:5"), L"x:x::x");
    EXPECT_EQ(mask_ipv6(L"2606:4700:4700::1111"), L"2606:4700:4700::1111");

    // Masked in place inside a sentence
    EXPECT_EQ(mask_ipv6(L"via fe80::1c2:3ff:fe4:5 ok"), L"via x:x::x ok");

    // Odd input
    EXPECT_EQ(mask_ipv6(L""), L"");
    EXPECT_EQ(mask_ipv6(L"::"), L"::");
    EXPECT_EQ(mask_ipv6(L"C:\\Users\\"), L"C:\\Users\\");
}

static void test_mac() {
    EXPECT_EQ(mask_mac(L"AA-BB-CC-11-22-33"), L"AA-BB-CC-xx-xx-xx");
    EXPECT_EQ(mask_mac(L"aa:bb:cc:11:22:33"), L"aa:bb:cc:xx:xx:xx");

    EXPECT_EQ(mask_mac(L""), L"");
    EXPECT_EQ(mask_mac(L"{"), L"{");
}

static void test_guid() {
    EXPECT_EQ(mask_guid(L"{4C1E9A02-1111-2222-3333-444455556666}"),
              L"{4C1E9A02-xxxx-xxxx-xxxx-xxxxxxxxxxxx}");

    EXPECT_EQ(mask_guid(L""), L"");
    EXPECT_EQ(mask_guid(L"{"), L"{");
}

static void test_user_paths() {
    EXPECT_EQ(mask_user_paths(L"C:\\Users\\Arda\\AppData\\x.txt"),
              L"C:\\Users\\<user>\\AppData\\x.txt");
    EXPECT_EQ(mask_user_paths(L"D:\\Users\\Bob\\Desktop"),
              L"D:\\Users\\<user>\\Desktop");

    // Odd input: no user segment name, must not crash
    EXPECT_EQ(mask_user_paths(L"C:\\Users\\"), L"C:\\Users\\");
    EXPECT_EQ(mask_user_paths(L""), L"");
}

static void test_sanitize() {
    std::wstring out = sanitize_text(L"fail at 192.168.0.4 for C:\\Users\\Ayse\\a");
    EXPECT(out.find(L"192.168.0.4") == std::wstring::npos);
    EXPECT(out.find(L"Ayse") == std::wstring::npos);

    EXPECT_EQ(sanitize_text(L""), L"");
    EXPECT_EQ(sanitize_text(L"::"), L"::");
    EXPECT_EQ(sanitize_text(L"1.2.3"), L"1.2.3");
    EXPECT_EQ(sanitize_text(L"{"), L"{");
    EXPECT_EQ(sanitize_text(L"C:\\Users\\"), L"C:\\Users\\");
}

int main() {
    wprintf(L"Running selftest mask tests...\n");

    test_ipv4();
    test_ipv6();
    test_mac();
    test_guid();
    test_user_paths();
    test_sanitize();

    if (g_failed == 0) {
        wprintf(L"All selftest mask tests passed.\n");
        return EXIT_SUCCESS;
    } else {
        wprintf(L"%d selftest mask test(s) FAILED.\n", g_failed);
        return EXIT_FAILURE;
    }
}

// test_selftest_runner.cpp — drives selftest::run with an in-memory fake Env.
// Never touches the registry.

#include "selftest.h"
#include <cstdio>
#include <map>

using namespace netshutup;
using namespace netshutup::selftest;

static int g_failed = 0;
#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            wprintf(L"FAIL  %hs:%d  %hs\n", __FILE__, __LINE__, #cond);       \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

struct Fake {
    std::map<std::wstring, std::optional<DWORD>> state{
        {L"a", std::nullopt}, {L"b", DWORD(5)}, {L"c", DWORD(0)}};
    std::map<std::wstring, std::vector<std::pair<std::wstring, DWORD>>> effects{
        {L"p1", {{L"a", 1}, {L"b", 6}}},
        {L"p2", {{L"c", 1}}},
        {L"p3", {{L"b", 9}}}};
    std::map<std::wstring, Snapshot> backups;
    std::wstring fail_apply, fail_revert;
    bool notset_becomes_zero = false;
    unsigned long clock = 0;
};

static Fake* F(void* c) { return static_cast<Fake*>(c); }

static Env make_env(Fake& f) {
    Env e{};
    e.ctx = &f;
    e.now_ms = [](void* c) -> unsigned long { return F(c)->clock += 10; };
    e.snapshot = [](void* c) { return Snapshot(F(c)->state.begin(), F(c)->state.end()); };
    e.apply = [](void* c, const wchar_t* p) {
        Fake* fk = F(c);
        apply::ApplyResult r{};
        r.profile_name = p;
        if (fk->fail_apply == p) { r.success = false; r.error_message = L"boom"; return r; }
        std::wstring path = L"bk_" + std::wstring(p);
        fk->backups[path] = Snapshot(fk->state.begin(), fk->state.end());
        for (auto& [id, v] : fk->effects[p]) fk->state[id] = v;
        r.success = true;
        r.backup_path = path;
        r.applied_count = fk->effects[p].size();
        return r;
    };
    e.revert = [](void* c, const std::wstring& path) {
        Fake* fk = F(c);
        apply::RevertResult r{};
        r.backup_path = path;
        if (!fk->fail_revert.empty() && path == L"bk_" + fk->fail_revert) {
            r.success = false;
            r.error_message = L"cannot restore";
            return r;
        }
        for (auto& [id, v] : fk->backups[path]) {
            fk->state[id] = (!v.has_value() && fk->notset_becomes_zero)
                                ? std::optional<DWORD>(0) : v;
        }
        r.success = true;
        r.restored_count = fk->backups[path].size();
        return r;
    };
    return e;
}

static bool section_has_fail(const Section& s) {
    for (auto& c : s.checks) if (c.status == Status::Fail) return true;
    return false;
}
static bool has_fail(const Report& r) {
    for (auto& s : r.sections) if (section_has_fail(s)) return true;
    return false;
}
static const Section* sec(const Report& r, const std::wstring& n) {
    for (auto& s : r.sections) if (s.name == n) return &s;
    return nullptr;
}
static const std::vector<std::wstring> kProfiles{L"p1", L"p2", L"p3"};

static void all_green() {
    Fake f; Env e = make_env(f);
    Report r = run(e, kProfiles);
    EXPECT(r.all_restored);
    EXPECT(!has_fail(r));
    EXPECT(sec(r, L"p3") != nullptr);
    EXPECT(r.total_millis > 0);
}
static void apply_fails_continues() {
    Fake f; f.fail_apply = L"p1"; Env e = make_env(f);
    Report r = run(e, kProfiles);
    EXPECT(sec(r, L"p1") != nullptr && section_has_fail(*sec(r, L"p1")));
    EXPECT(sec(r, L"p2") != nullptr && sec(r, L"p3") != nullptr);
    EXPECT(r.all_restored);   // nothing was written, state equals start
}
static void revert_fails_stops() {
    Fake f; f.fail_revert = L"p2"; Env e = make_env(f);
    Report r = run(e, kProfiles);
    EXPECT(sec(r, L"p3") == nullptr);
    EXPECT(!r.all_restored);
    EXPECT(r.manual_revert_cmd.find(L"bk_p2") != std::wstring::npos);
}
static void diff_after_revert() {
    Fake f; f.notset_becomes_zero = true; Env e = make_env(f);
    Report r = run(e, {L"p1"});
    EXPECT(!r.all_restored);
    bool named = false;
    for (auto& c : sec(r, L"p1")->checks)
        if (c.status == Status::Fail && c.detail.find(L"a") != std::wstring::npos) named = true;
    EXPECT(named);
}
static void not_set_stays_not_set() {
    Fake f; Env e = make_env(f);
    Report r = run(e, {L"p1"});
    EXPECT(r.all_restored && !has_fail(r));
    EXPECT(!f.state[L"a"].has_value());
}
static void diff_fn() {
    Snapshot a{{L"x", std::nullopt}}, b{{L"x", DWORD(0)}};
    EXPECT(diff(a, a).empty());
    EXPECT(diff(a, b).size() == 1);   // not set != 0
}

int main() {
    all_green();
    apply_fails_continues();
    revert_fails_stops();
    diff_after_revert();
    not_set_stays_not_set();
    diff_fn();
    if (g_failed) { wprintf(L"%d failure(s)\n", g_failed); return 1; }
    wprintf(L"selftest_runner: all passed\n");
    return 0;
}

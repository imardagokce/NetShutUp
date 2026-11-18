// selftest_runner.cpp — pure apply/verify/revert/compare loop (no side effects
// except through Env).

#include "selftest.h"
#include <map>

namespace netshutup::selftest {

namespace {

std::wstring join_ids(const std::vector<std::wstring>& ids) {
    std::wstring out;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i) out += L", ";
        out += ids[i];
    }
    return out;
}

Check make(const wchar_t* id, const wchar_t* title, Status st,
           std::wstring detail = L"", unsigned long ms = 0) {
    Check c;
    c.id = id;
    c.title = title;
    c.status = st;
    c.detail = std::move(detail);
    c.millis = ms;
    return c;
}

} // namespace

std::vector<std::wstring> diff(const Snapshot& a, const Snapshot& b) {
    std::map<std::wstring, std::optional<DWORD>> am(a.begin(), a.end());
    std::map<std::wstring, std::optional<DWORD>> bm(b.begin(), b.end());
    std::vector<std::wstring> out;
    for (const auto& [id, va] : am) {
        auto it = bm.find(id);
        if (it == bm.end() || it->second != va) out.push_back(id);
    }
    for (const auto& [id, vb] : bm) {
        (void)vb;
        if (am.find(id) == am.end()) out.push_back(id);
    }
    return out;
}

Report run(const Env& env, const std::vector<std::wstring>& profiles) {
    Report rep;
    rep.all_restored = true;
    const unsigned long t_start = env.now_ms(env.ctx);
    const Snapshot initial = env.snapshot(env.ctx);

    for (const auto& profile : profiles) {
        Section sec;
        sec.name = profile;
        const Snapshot before = env.snapshot(env.ctx);

        unsigned long t = env.now_ms(env.ctx);
        apply::ApplyResult ar = env.apply(env.ctx, profile.c_str());
        unsigned long dt = env.now_ms(env.ctx) - t;

        const std::wstring backup = ar.backup_path;
        bool need_revert = false;

        if (!ar.success) {
            sec.checks.push_back(make(L"apply", L"Apply profile", Status::Fail,
                                      ar.error_message, dt));
            need_revert = !backup.empty();   // engine may have written a backup
        } else {
            sec.checks.push_back(make(L"apply", L"Apply profile", Status::Pass,
                                      L"applied " + std::to_wstring(ar.applied_count) +
                                      L", skipped " + std::to_wstring(ar.skipped_count), dt));
            auto changed = diff(before, env.snapshot(env.ctx));
            if (changed.empty()) {
                sec.checks.push_back(make(L"verify", L"Values changed", Status::Info,
                                          L"nothing changed (already at target values)"));
            } else {
                sec.checks.push_back(make(L"verify", L"Values changed", Status::Pass,
                                          std::to_wstring(changed.size()) + L" value(s): " +
                                          join_ids(changed)));
            }
            need_revert = true;
        }

        if (need_revert) {
            t = env.now_ms(env.ctx);
            apply::RevertResult rr = env.revert(env.ctx, backup);
            dt = env.now_ms(env.ctx) - t;
            if (!rr.success) {
                sec.checks.push_back(make(L"revert", L"Revert from backup", Status::Fail,
                                          rr.error_message, dt));
                rep.all_restored = false;
                rep.manual_revert_cmd = L"netshutup-legacy revert \"" + backup + L"\"";
                rep.sections.push_back(std::move(sec));
                break;   // stop: do not stack more changes on a broken state
            }
            sec.checks.push_back(make(L"revert", L"Revert from backup", Status::Pass,
                                      L"restored " + std::to_wstring(rr.restored_count), dt));
        }

        auto leftovers = diff(before, env.snapshot(env.ctx));
        if (leftovers.empty()) {
            sec.checks.push_back(make(L"compare", L"State equals pre-apply state", Status::Pass));
        } else {
            sec.checks.push_back(make(L"compare", L"State equals pre-apply state", Status::Fail,
                                      L"differs: " + join_ids(leftovers)));
        }
        rep.sections.push_back(std::move(sec));
    }

    // Closing comparison against the very first snapshot (also after an abort,
    // so the report says exactly what is left over).
    Section fin;
    fin.name = L"Final";
    auto left = diff(initial, env.snapshot(env.ctx));
    if (left.empty()) {
        fin.checks.push_back(make(L"final", L"All settings equal the start state", Status::Pass));
    } else {
        fin.checks.push_back(make(L"final", L"All settings equal the start state", Status::Fail,
                                  L"differs: " + join_ids(left)));
        rep.all_restored = false;
        if (rep.manual_revert_cmd.empty()) rep.manual_revert_cmd = L"netshutup-legacy revert";
    }
    rep.sections.push_back(std::move(fin));
    rep.total_millis = env.now_ms(env.ctx) - t_start;
    return rep;
}

} // namespace netshutup::selftest

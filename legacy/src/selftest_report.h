#pragma once

// selftest_report.h — selftest result model and renderers (pure, no I/O).

#include <string>
#include <vector>

namespace netshutup::selftest {

enum class Status { Pass, Fail, Skip, Info };

struct Check {
    std::wstring  id;
    std::wstring  title;
    std::wstring  detail;
    Status        status = Status::Info;
    unsigned long millis = 0;
};

struct Section {
    std::wstring       name;   // "Environment", "privacy", ...
    std::vector<Check> checks;
};

struct Report {
    int                  schema = 1;
    std::wstring         started_utc;
    unsigned long        total_millis = 0;
    bool                 all_restored = false;
    std::wstring         manual_revert_cmd;   // shown when all_restored == false
    std::vector<Section> sections;
};

// Every free-text field goes through mask::sanitize_text inside both renderers.
std::wstring render_markdown(const Report& r);
std::string  render_json(const Report& r);   // UTF-8

} // namespace netshutup::selftest

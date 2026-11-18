// selftest_report.cpp — Markdown and JSON renderers for the selftest report (pure, no I/O).

#include "selftest_report.h"
#include "selftest_mask.h"

#include <windows.h>

#include <string>

namespace netshutup::selftest {

namespace {

const std::wstring kCheckMark = L"✅";  // check mark
const std::wstring kCrossMark = L"❌";  // cross mark
const std::wstring kSkipMark  = L"⏭";  // skip
const wchar_t kHex[] = L"0123456789abcdef";

// Sanitizes free text, then makes it safe for one markdown table cell.
std::wstring md_text(const std::wstring& raw) {
    std::wstring s = mask::sanitize_text(raw);
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'|') {
            out += L"\\|";
        } else if (c == L'\r' || c == L'\n') {
            out += L' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::wstring status_md(Status s) {
    switch (s) {
        case Status::Pass: return kCheckMark + L" Pass";
        case Status::Fail: return kCrossMark + L" Fail";
        case Status::Skip: return kSkipMark + L" Skip";
        case Status::Info: return L"Info";
    }
    return L"Info";
}

const wchar_t* status_json(Status s) {
    switch (s) {
        case Status::Pass: return L"pass";
        case Status::Fail: return L"fail";
        case Status::Skip: return L"skip";
        case Status::Info: return L"info";
    }
    return L"info";
}

// Sanitizes free text and returns it as a quoted, JSON-escaped string (still wide).
std::wstring json_str(const std::wstring& raw) {
    std::wstring s = mask::sanitize_text(raw);
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\b': out += L"\\b";  break;
            case L'\f': out += L"\\f";  break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if (static_cast<unsigned int>(c) < 0x20u) {
                    out += L"\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += c;
                }
                break;
        }
    }
    out += L"\"";
    return out;
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int in_len = static_cast<int>(w.size());
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), in_len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), in_len, &out[0], n, nullptr, nullptr);
    return out;
}

} // namespace

std::wstring render_markdown(const Report& r) {
    std::wstring out;

    if (!r.all_restored) {
        out += L"**NOT everything was restored.** Revert manually: `";
        out += md_text(r.manual_revert_cmd);
        out += L"`\n\n";
    }

    out += L"# NetShutUp self-test report\n\n";
    out += L"- Started (UTC): " + md_text(r.started_utc) + L"\n";
    out += L"- Total time: " + std::to_wstring(r.total_millis) + L" ms\n";
    out += std::wstring(L"- All settings restored: ") + (r.all_restored ? L"yes" : L"no") + L"\n\n";

    std::size_t pass = 0;
    std::size_t fail = 0;
    std::size_t skip = 0;
    for (const auto& sec : r.sections) {
        for (const auto& c : sec.checks) {
            if (c.status == Status::Pass) ++pass;
            else if (c.status == Status::Fail) ++fail;
            else if (c.status == Status::Skip) ++skip;
        }
    }
    out += L"Summary: " + kCheckMark + L" " + std::to_wstring(pass)
         + L"  " + kCrossMark + L" " + std::to_wstring(fail)
         + L"  " + kSkipMark + L" " + std::to_wstring(skip) + L"\n\n";

    for (const auto& sec : r.sections) {
        out += L"## " + md_text(sec.name) + L"\n\n";
        out += L"| Step | Result | ms | Note |\n";
        out += L"|---|---|---|---|\n";
        for (const auto& c : sec.checks) {
            const std::wstring& step = c.title.empty() ? c.id : c.title;
            out += L"| " + md_text(step)
                 + L" | " + status_md(c.status)
                 + L" | " + std::to_wstring(c.millis)
                 + L" | " + md_text(c.detail) + L" |\n";
        }
        out += L"\n";
    }

    return out;
}

std::string render_json(const Report& r) {
    std::wstring out = L"{\"schema\":" + std::to_wstring(r.schema);
    out += L",\"started_utc\":" + json_str(r.started_utc);
    out += L",\"total_millis\":" + std::to_wstring(r.total_millis);
    out += std::wstring(L",\"all_restored\":") + (r.all_restored ? L"true" : L"false");
    out += L",\"manual_revert_cmd\":" + json_str(r.manual_revert_cmd);
    out += L",\"sections\":[";

    for (std::size_t i = 0; i < r.sections.size(); ++i) {
        const Section& sec = r.sections[i];
        if (i > 0) out += L",";
        out += L"{\"name\":" + json_str(sec.name) + L",\"checks\":[";
        for (std::size_t j = 0; j < sec.checks.size(); ++j) {
            const Check& c = sec.checks[j];
            if (j > 0) out += L",";
            out += L"{\"id\":" + json_str(c.id);
            out += L",\"title\":" + json_str(c.title);
            out += L",\"detail\":" + json_str(c.detail);
            out += std::wstring(L",\"status\":\"") + status_json(c.status) + L"\"";
            out += L",\"millis\":" + std::to_wstring(c.millis);
            out += L"}";
        }
        out += L"]}";
    }

    out += L"]}";
    return to_utf8(out);
}

} // namespace netshutup::selftest

// firewall.cpp — INetFwPolicy2 access and third-party firewall detection.

#include "firewall.h"

#include <netfw.h>
#include <wbemidl.h>
#include <cwchar>

namespace netshutup::firewall {

namespace {

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    // S_FALSE = already initialised on this thread (fine); changed-mode = also usable.
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

template <class T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ~ComPtr() { if (p) p->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

bool profile_type(const wchar_t* profile, NET_FW_PROFILE_TYPE2& out) {
    if (!profile) return false;
    if (wcscmp(profile, L"domain") == 0)  { out = NET_FW_PROFILE2_DOMAIN;  return true; }
    if (wcscmp(profile, L"private") == 0) { out = NET_FW_PROFILE2_PRIVATE; return true; }
    if (wcscmp(profile, L"public") == 0)  { out = NET_FW_PROFILE2_PUBLIC;  return true; }
    return false;
}

bool open_policy(ComPtr<INetFwPolicy2>& policy) {
    HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(INetFwPolicy2),
                                  reinterpret_cast<void**>(policy.put()));
    return SUCCEEDED(hr) && policy;
}

} // namespace

std::optional<DWORD> read(const wchar_t* profile, const wchar_t* what) {
    NET_FW_PROFILE_TYPE2 pt;
    if (!profile_type(profile, pt) || !what) return std::nullopt;

    ComScope com;
    if (!com.ok()) return std::nullopt;
    ComPtr<INetFwPolicy2> policy;
    if (!open_policy(policy)) return std::nullopt;

    if (wcscmp(what, L"enabled") == 0) {
        VARIANT_BOOL b = VARIANT_FALSE;
        if (FAILED(policy->get_FirewallEnabled(pt, &b))) return std::nullopt;
        return b != VARIANT_FALSE ? 1u : 0u;
    }
    if (wcscmp(what, L"inbound") == 0) {
        NET_FW_ACTION a = NET_FW_ACTION_BLOCK;
        if (FAILED(policy->get_DefaultInboundAction(pt, &a))) return std::nullopt;
        return a == NET_FW_ACTION_ALLOW ? 1u : 0u;
    }
    return std::nullopt;
}

bool write(const wchar_t* profile, const wchar_t* what, DWORD value) {
    NET_FW_PROFILE_TYPE2 pt;
    if (!profile_type(profile, pt) || !what || value > 1) return false;

    {
        ComScope com;
        if (!com.ok()) return false;
        ComPtr<INetFwPolicy2> policy;
        if (!open_policy(policy)) return false;

        HRESULT hr = E_INVALIDARG;
        if (wcscmp(what, L"enabled") == 0) {
            hr = policy->put_FirewallEnabled(pt, value ? VARIANT_TRUE : VARIANT_FALSE);
        } else if (wcscmp(what, L"inbound") == 0) {
            hr = policy->put_DefaultInboundAction(pt, value ? NET_FW_ACTION_ALLOW : NET_FW_ACTION_BLOCK);
        }
        if (FAILED(hr)) return false;
    }

    // Read back: Group Policy may override the effective value.
    auto now = read(profile, what);
    return now.has_value() && *now == value;
}

bool is_third_party_name(const std::wstring& display_name) {
    if (display_name.empty()) return false;
    static const wchar_t kPrefix[] = L"Windows";
    return _wcsnicmp(display_name.c_str(), kPrefix, 7) != 0;
}

std::vector<std::wstring> registered_products() {
    std::vector<std::wstring> names;

    ComScope com;
    if (!com.ok()) return names;

    ComPtr<IWbemLocator> loc;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, reinterpret_cast<void**>(loc.put())))) return names;

    BSTR ns = SysAllocString(L"ROOT\\SecurityCenter2");
    ComPtr<IWbemServices> svc;
    HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, svc.put());
    SysFreeString(ns);
    if (FAILED(hr) || !svc) return names;

    CoSetProxyBlanket(svc.p, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    BSTR lang  = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT displayName FROM FirewallProduct");
    ComPtr<IEnumWbemClassObject> en;
    hr = svc->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                        nullptr, en.put());
    SysFreeString(lang);
    SysFreeString(query);
    if (FAILED(hr) || !en) return names;

    for (;;) {
        IWbemClassObject* obj = nullptr;
        ULONG returned = 0;
        hr = en->Next(WBEM_INFINITE, 1, &obj, &returned);
        if (FAILED(hr) || returned == 0 || !obj) break;
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"displayName", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal) {
            names.emplace_back(v.bstrVal);
        }
        VariantClear(&v);
        obj->Release();
    }
    return names;
}

std::wstring skip_reason() {
    for (const auto& n : registered_products()) {
        if (is_third_party_name(n)) {
            return L"Another firewall (" + n + L") is registered, so Windows Firewall settings were left alone.";
        }
    }
    return L"";
}

} // namespace netshutup::firewall

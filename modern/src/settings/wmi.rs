//! Minimal read-only WMI access for settings that Windows exposes only
//! through its management interface (TCP global settings).

use windows::Win32::System::Com::{
    CLSCTX_INPROC_SERVER, COINIT_MULTITHREADED, CoCreateInstance, CoInitializeEx,
    CoSetProxyBlanket, EOAC_NONE, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
};
use windows::Win32::System::Wmi::{
    IWbemClassObject, IWbemLocator, WBEM_FLAG_FORWARD_ONLY, WBEM_FLAG_RETURN_IMMEDIATELY,
    WBEM_INFINITE, WbemLocator,
};
use windows::Win32::System::Variant::{VARIANT, VT_I4, VT_UI1, VT_UI4, VariantClear};
use windows::core::{BSTR, HSTRING, PCWSTR};

const RPC_C_AUTHN_WINNT: u32 = 10;
const RPC_C_AUTHZ_NONE: u32 = 0;

const NAMESPACE: &str = r"ROOT\StandardCimv2";

/// Reads one integer property from the first object matching the query.
/// `Ok(None)` means no object matched or the property is empty.
pub fn query_u32(class: &str, filter: Option<&str>, property: &str) -> Result<Option<u32>, String> {
    let query = match filter {
        Some(filter) => format!("SELECT {property} FROM {class} WHERE {filter}"),
        None => format!("SELECT {property} FROM {class}"),
    };

    let describe = |error: windows::core::Error| format!("WMI error {:#010X}", error.code().0);

    unsafe {
        // Already-initialized results are fine; real failures surface below.
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);

        let locator: IWbemLocator =
            CoCreateInstance(&WbemLocator, None, CLSCTX_INPROC_SERVER).map_err(describe)?;
        let services = locator
            .ConnectServer(
                &BSTR::from(NAMESPACE),
                &BSTR::new(),
                &BSTR::new(),
                &BSTR::new(),
                0,
                &BSTR::new(),
                None,
            )
            .map_err(describe)?;
        // WMI refuses queries unless it may act with the caller's identity.
        CoSetProxyBlanket(
            &services,
            RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE,
            PCWSTR::null(),
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            None,
            EOAC_NONE,
        )
        .map_err(describe)?;

        let rows = services
            .ExecQuery(
                &BSTR::from("WQL"),
                &BSTR::from(query.as_str()),
                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                None,
            )
            .map_err(describe)?;

        let mut row: [Option<IWbemClassObject>; 1] = [None];
        let mut returned = 0u32;
        let next = rows.Next(WBEM_INFINITE, &mut row, &mut returned);
        if next.is_err() {
            return Err(format!("WMI error {:#010X}", next.0));
        }
        let Some(object) = row[0].take().filter(|_| returned == 1) else {
            return Ok(None);
        };

        let mut value = VARIANT::default();
        object
            .Get(&HSTRING::from(property), 0, &mut value, None, None)
            .map_err(describe)?;

        let inner = &value.Anonymous.Anonymous;
        let number = match inner.vt {
            VT_UI1 => Some(u32::from(inner.Anonymous.bVal)),
            VT_I4 => u32::try_from(inner.Anonymous.lVal).ok(),
            VT_UI4 => Some(inner.Anonymous.ulVal),
            _ => None,
        };
        let _ = VariantClear(&mut value);

        Ok(number)
    }
}

//! Low-level registry access. Every function takes the hive explicitly so
//! tests can run against a sandbox key under `HKEY_CURRENT_USER` instead of
//! the real system settings.
//!
//! Errors are raw Win32 error codes.

use std::ffi::c_void;

use windows::Win32::System::Registry::{
    HKEY, KEY_ENUMERATE_SUB_KEYS, KEY_QUERY_VALUE, KEY_SET_VALUE, REG_BINARY, REG_DWORD, REG_EXPAND_SZ,
    REG_MULTI_SZ, REG_OPTION_NON_VOLATILE, REG_QWORD, REG_SZ, REG_VALUE_TYPE, RRF_RT_REG_DWORD, RRF_RT_REG_SZ, RegCloseKey, RegCreateKeyExW,
    RegDeleteKeyValueW, RegDeleteKeyW, RegEnumKeyExW, RegEnumValueW, RegGetValueW, RegOpenKeyExW, RegQueryInfoKeyW,
    RegSetValueExW,
};
use windows::core::{HSTRING, PCWSTR, PWSTR};

const ERROR_SUCCESS: u32 = 0;
const ERROR_FILE_NOT_FOUND: u32 = 2;
const ERROR_NO_MORE_ITEMS: u32 = 259;

pub type RegResult<T> = Result<T, u32>;

/// `Ok(None)` when the key or the value does not exist.
pub fn read_dword(hive: HKEY, key: &str, value: &str) -> RegResult<Option<u32>> {
    let mut data: u32 = 0;
    let mut size = std::mem::size_of::<u32>() as u32;

    let result = unsafe {
        RegGetValueW(
            hive,
            &HSTRING::from(key),
            &HSTRING::from(value),
            RRF_RT_REG_DWORD,
            None,
            Some((&raw mut data).cast::<c_void>()),
            Some(&mut size),
        )
    };

    match result.0 {
        ERROR_SUCCESS => Ok(Some(data)),
        ERROR_FILE_NOT_FOUND => Ok(None),
        code => Err(code),
    }
}

/// `Ok(None)` when the key or the value does not exist. The text is returned
/// exactly as stored so that it can be written back unchanged.
pub fn read_string(hive: HKEY, key: &str, value: &str) -> RegResult<Option<String>> {
    let mut buffer = [0u16; 1024];
    let mut size = std::mem::size_of_val(&buffer) as u32;

    let result = unsafe {
        RegGetValueW(
            hive,
            &HSTRING::from(key),
            &HSTRING::from(value),
            RRF_RT_REG_SZ,
            None,
            Some(buffer.as_mut_ptr().cast::<c_void>()),
            Some(&mut size),
        )
    };

    match result.0 {
        ERROR_SUCCESS => {
            let len = buffer.iter().position(|&unit| unit == 0).unwrap_or(buffer.len());
            Ok(Some(String::from_utf16_lossy(&buffer[..len])))
        }
        ERROR_FILE_NOT_FOUND => Ok(None),
        code => Err(code),
    }
}

/// Writes a DWORD value, creating the key if needed.
pub fn write_dword(hive: HKEY, key: &str, value: &str, data: u32) -> RegResult<()> {
    set_value(hive, key, value, REG_DWORD, &data.to_le_bytes())
}

/// Writes a string value, creating the key if needed.
pub fn write_string(hive: HKEY, key: &str, value: &str, data: &str) -> RegResult<()> {
    let bytes: Vec<u8> = data
        .encode_utf16()
        .chain(std::iter::once(0))
        .flat_map(u16::to_le_bytes)
        .collect();
    set_value(hive, key, value, REG_SZ, &bytes)
}

fn set_value(
    hive: HKEY,
    key: &str,
    value: &str,
    kind: REG_VALUE_TYPE,
    bytes: &[u8],
) -> RegResult<()> {
    unsafe {
        let mut handle = HKEY::default();
        let created = RegCreateKeyExW(
            hive,
            &HSTRING::from(key),
            None,
            PCWSTR::null(),
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            None,
            &mut handle,
            None,
        );
        check(created.0)?;

        let result = RegSetValueExW(handle, &HSTRING::from(value), None, kind, Some(bytes));
        let _ = RegCloseKey(handle);
        check(result.0)
    }
}

/// Deletes a value. A value or key that is already missing counts as success.
pub fn delete_value(hive: HKEY, key: &str, value: &str) -> RegResult<()> {
    let result = unsafe { RegDeleteKeyValueW(hive, &HSTRING::from(key), &HSTRING::from(value)) };
    match result.0 {
        ERROR_SUCCESS | ERROR_FILE_NOT_FOUND => Ok(()),
        code => Err(code),
    }
}

/// The names of a key's subkeys, sorted. A key that does not exist has none.
pub fn subkeys(hive: HKEY, key: &str) -> RegResult<Vec<String>> {
    unsafe {
        let mut handle = HKEY::default();
        let opened =
            RegOpenKeyExW(hive, &HSTRING::from(key), None, KEY_ENUMERATE_SUB_KEYS, &mut handle);
        match opened.0 {
            ERROR_SUCCESS => {}
            ERROR_FILE_NOT_FOUND => return Ok(Vec::new()),
            code => return Err(code),
        }

        let mut names = Vec::new();
        let mut index = 0u32;
        let outcome = loop {
            let mut buffer = [0u16; 256];
            let mut length = buffer.len() as u32;
            let result = RegEnumKeyExW(
                handle,
                index,
                Some(PWSTR(buffer.as_mut_ptr())),
                &mut length,
                None,
                None,
                None,
                None,
            );
            match result.0 {
                ERROR_SUCCESS => names.push(String::from_utf16_lossy(&buffer[..length as usize])),
                ERROR_NO_MORE_ITEMS => break Ok(()),
                code => break Err(code),
            }
            index += 1;
        };
        let _ = RegCloseKey(handle);
        outcome?;
        names.sort();
        Ok(names)
    }
}

/// Every value of a key as `(name, "<type>:<data>")`, sorted by name. Binary
/// data is hex. A key that does not exist has none.
pub fn values(hive: HKEY, key: &str) -> RegResult<Vec<(String, String)>> {
    unsafe {
        let mut handle = HKEY::default();
        let opened = RegOpenKeyExW(hive, &HSTRING::from(key), None, KEY_QUERY_VALUE, &mut handle);
        match opened.0 {
            ERROR_SUCCESS => {}
            ERROR_FILE_NOT_FOUND => return Ok(Vec::new()),
            code => return Err(code),
        }

        let mut found = Vec::new();
        let mut index = 0u32;
        let outcome = loop {
            let mut name = vec![0u16; 16384];
            let mut name_len = name.len() as u32;
            let mut data = vec![0u8; 65536];
            let mut data_len = data.len() as u32;
            let mut kind = 0u32;
            let result = RegEnumValueW(
                handle,
                index,
                Some(PWSTR(name.as_mut_ptr())),
                &mut name_len,
                None,
                Some(&mut kind),
                Some(data.as_mut_ptr()),
                Some(&mut data_len),
            );
            match result.0 {
                ERROR_SUCCESS => found.push((
                    String::from_utf16_lossy(&name[..name_len as usize]),
                    describe_value(kind, &data[..data_len as usize]),
                )),
                ERROR_NO_MORE_ITEMS => break Ok(()),
                code => break Err(code),
            }
            index += 1;
        };
        let _ = RegCloseKey(handle);
        outcome?;
        found.sort();
        Ok(found)
    }
}

fn describe_value(kind: u32, data: &[u8]) -> String {
    let text = |data: &[u8]| {
        let units: Vec<u16> = data.chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])).collect();
        String::from_utf16_lossy(&units)
    };
    match REG_VALUE_TYPE(kind) {
        REG_SZ => format!("REG_SZ:{}", text(data).trim_end_matches('\0')),
        REG_EXPAND_SZ => format!("REG_EXPAND_SZ:{}", text(data).trim_end_matches('\0')),
        REG_MULTI_SZ => {
            let joined = text(data).trim_end_matches('\0').split('\0').collect::<Vec<_>>().join("|");
            format!("REG_MULTI_SZ:{joined}")
        }
        REG_DWORD if data.len() == 4 => {
            format!("REG_DWORD:{}", u32::from_le_bytes([data[0], data[1], data[2], data[3]]))
        }
        REG_QWORD if data.len() == 8 => {
            let mut bytes = [0u8; 8];
            bytes.copy_from_slice(data);
            format!("REG_QWORD:{}", u64::from_le_bytes(bytes))
        }
        other => {
            let name = if other == REG_BINARY { "REG_BINARY" } else { "OTHER" };
            let hex: String = data.iter().map(|b| format!("{b:02x}")).collect();
            format!("{name}:{hex}")
        }
    }
}

/// Whether a key exists. A key that exists but cannot be opened counts as
/// existing, so callers never treat it as something they created.
pub fn key_exists(hive: HKEY, key: &str) -> bool {
    unsafe {
        let mut handle = HKEY::default();
        let result = RegOpenKeyExW(hive, &HSTRING::from(key), None, KEY_QUERY_VALUE, &mut handle);
        if result.0 == ERROR_SUCCESS {
            let _ = RegCloseKey(handle);
        }
        result.0 != ERROR_FILE_NOT_FOUND
    }
}

/// Deletes a key only if it has no values and no subkeys. Returns whether it
/// was deleted.
pub fn delete_key_if_empty(hive: HKEY, key: &str) -> RegResult<bool> {
    unsafe {
        let mut handle = HKEY::default();
        check(RegOpenKeyExW(hive, &HSTRING::from(key), None, KEY_QUERY_VALUE, &mut handle).0)?;

        let mut subkeys = 0u32;
        let mut values = 0u32;
        let result = RegQueryInfoKeyW(
            handle,
            None,
            None,
            None,
            Some(&mut subkeys),
            None,
            None,
            Some(&mut values),
            None,
            None,
            None,
            None,
        );
        let _ = RegCloseKey(handle);
        check(result.0)?;

        if subkeys != 0 || values != 0 {
            return Ok(false);
        }
        check(RegDeleteKeyW(hive, &HSTRING::from(key)).0)?;
        Ok(true)
    }
}

fn check(code: u32) -> RegResult<()> {
    if code == ERROR_SUCCESS { Ok(()) } else { Err(code) }
}

#[cfg(test)]
pub mod sandbox {
    //! A throwaway key under `HKEY_CURRENT_USER` for tests. Nothing outside
    //! `HKCU\Software\NetShutUp-Test` is ever written by a test.

    use windows::Win32::System::Registry::{HKEY_CURRENT_USER, RegDeleteTreeW};
    use windows::core::HSTRING;

    pub const ROOT: &str = r"Software\NetShutUp-Test";

    pub fn path(name: &str) -> String {
        format!(r"{ROOT}\{name}")
    }

    /// Removes the named sandbox and everything in it.
    pub fn wipe(name: &str) {
        unsafe {
            let _ = RegDeleteTreeW(HKEY_CURRENT_USER, &HSTRING::from(path(name)));
        }
        let _ = super::delete_key_if_empty(HKEY_CURRENT_USER, &path(name));
        let _ = super::delete_key_if_empty(HKEY_CURRENT_USER, ROOT);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows::Win32::System::Registry::{HKEY_CURRENT_USER as HKCU, HKEY_LOCAL_MACHINE};

    #[test]
    fn missing_value_reads_as_none() {
        let read = read_dword(HKEY_LOCAL_MACHINE, r"SOFTWARE\NetShutUp\DoesNotExist", "Missing");
        assert_eq!(read, Ok(None));
    }

    #[test]
    fn values_round_trip_and_delete() {
        sandbox::wipe("registry-roundtrip");
        let key = sandbox::path(r"registry-roundtrip\Deep\Key");

        assert!(!key_exists(HKCU, &key));
        write_dword(HKCU, &key, "Number", 0xFFFF_FFFF).unwrap();
        write_string(HKCU, &key, "Text", "9.9.9.9,149.112.112.112").unwrap();
        write_string(HKCU, &key, "Empty", "").unwrap();
        assert!(key_exists(HKCU, &key));

        assert_eq!(read_dword(HKCU, &key, "Number"), Ok(Some(0xFFFF_FFFF)));
        assert_eq!(
            read_string(HKCU, &key, "Text"),
            Ok(Some("9.9.9.9,149.112.112.112".to_string()))
        );
        assert_eq!(read_string(HKCU, &key, "Empty"), Ok(Some(String::new())));
        // A value of the wrong type is an error, never silently reinterpreted.
        assert!(read_dword(HKCU, &key, "Text").is_err());

        assert_eq!(delete_key_if_empty(HKCU, &key), Ok(false));
        for value in ["Number", "Text", "Empty", "NeverExisted"] {
            delete_value(HKCU, &key, value).unwrap();
        }
        assert_eq!(read_dword(HKCU, &key, "Number"), Ok(None));
        assert_eq!(delete_key_if_empty(HKCU, &key), Ok(true));
        assert!(!key_exists(HKCU, &key));

        sandbox::wipe("registry-roundtrip");
        assert!(!key_exists(HKCU, &sandbox::path("registry-roundtrip")));
    }

    #[test]
    fn subkeys_lists_children_of_a_sandbox_key() {
        sandbox::wipe("registry-subkeys");
        let base = sandbox::path("registry-subkeys");
        write_string(HKCU, &format!(r"{base}\b"), "v", "1").unwrap();
        write_string(HKCU, &format!(r"{base}\a"), "v", "1").unwrap();

        assert_eq!(subkeys(HKCU, &base), Ok(vec!["a".to_string(), "b".to_string()]));
        // A key that does not exist has no subkeys, like a missing value reads as none.
        assert_eq!(subkeys(HKCU, &sandbox::path("registry-subkeys-missing")), Ok(vec![]));

        sandbox::wipe("registry-subkeys");
    }

    #[test]
    fn registry_values_lists_a_sandbox_key() {
        sandbox::wipe("registry-values");
        let key = sandbox::path("registry-values");
        write_string(HKCU, &key, "Template", "https://x/dns-query").unwrap();
        write_dword(HKCU, &key, "Flags", 2).unwrap();

        assert_eq!(
            values(HKCU, &key),
            Ok(vec![
                ("Flags".to_string(), "REG_DWORD:2".to_string()),
                ("Template".to_string(), "REG_SZ:https://x/dns-query".to_string()),
            ])
        );
        assert_eq!(values(HKCU, &sandbox::path("registry-values-missing")), Ok(vec![]));

        sandbox::wipe("registry-values");
    }
}

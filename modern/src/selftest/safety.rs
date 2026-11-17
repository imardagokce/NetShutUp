//! The self-test's safety net: which registry keys are exported before anything
//! is written, and how the files are named.

use std::path::Path;

/// The keys the program's settings live under (or next to). All are `HKLM`.
pub const REGISTRY_KEYS: &[&str] = &[
    r"HKLM\SYSTEM\CurrentControlSet\Services\Dnscache",
    r"HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters",
    r"HKLM\SYSTEM\CurrentControlSet\Services\DiagTrack",
    r"HKLM\SOFTWARE\Policies\Microsoft\Windows NT\DNSClient",
    r"HKLM\SOFTWARE\Policies\Microsoft\Windows\DataCollection",
    r"HKLM\SOFTWARE\Policies\Microsoft\Windows\DeliveryOptimization",
    r"HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile",
];

/// The arguments for `reg.exe`: export the key to the file, overwriting it.
pub fn export_args(key: &str, file: &Path) -> Vec<String> {
    vec![
        "export".to_string(),
        key.to_string(),
        file.to_string_lossy().to_string(),
        "/y".to_string(),
    ]
}

/// A file name for the key's export: its last segment, with the parent in front
/// when that segment alone would be ambiguous (`Parameters`).
pub fn export_file_name(key: &str) -> String {
    let mut segments = key.rsplit('\\');
    let last = segments.next().unwrap_or(key);
    let stem = match (last, segments.next()) {
        ("Parameters", Some(parent)) => format!("{parent}_{last}"),
        _ => last.to_string(),
    };
    let safe: String = stem
        .chars()
        .map(|c| if c.is_alphanumeric() || matches!(c, ' ' | '.' | '-' | '_') { c } else { '_' })
        .collect();
    format!("{safe}.reg")
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    #[test]
    fn export_args_are_reg_export_with_overwrite() {
        let args = export_args(r"HKLM\SOFTWARE\X", Path::new(r"C:\b\x.reg"));
        assert_eq!(args, vec!["export", r"HKLM\SOFTWARE\X", r"C:\b\x.reg", "/y"]);
    }

    #[test]
    fn export_file_names_are_unique_and_safe() {
        let names: Vec<String> = REGISTRY_KEYS.iter().map(|key| export_file_name(key)).collect();
        assert_eq!(names.len(), 7);
        let mut sorted = names.clone();
        sorted.sort();
        sorted.dedup();
        assert_eq!(sorted.len(), 7, "{names:?}");
        for name in &names {
            assert!(name.ends_with(".reg"), "{name}");
            assert!(!name.contains('\\') && !name.contains(':'), "{name}");
        }
        assert!(names.contains(&"Tcpip_Parameters.reg".to_string()), "{names:?}");
    }
}

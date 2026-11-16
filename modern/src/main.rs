mod adapters;
mod settings;
mod version;

use adapters::NetworkAdapter;
use settings::catalog::{CATALOG, Scope};
use settings::reader::{self, Origin};
use settings::store::{Store, SystemStore};

fn main() {
    let version = version::get_windows_version();
    println!("Windows Version: {}.{}.{}", version.0, version.1, version.2);

    let adapters = match adapters::get_network_adapters() {
        Ok(adapters) => adapters,
        Err(code) => {
            eprintln!("GetAdaptersAddresses failed with error code {code}");
            Vec::new()
        }
    };

    print_adapters(&adapters);

    for adapter in adapters.iter().filter(|adapter| !adapter.is_loopback) {
        print_settings(&adapter.friendly_name, Scope::Adapter, Some(&adapter.guid));
    }
    print_settings("System", Scope::System, None);
}

fn print_adapters(adapters: &[NetworkAdapter]) {
    println!("\nNetwork Adapters ({}):", adapters.len());
    for (i, adapter) in adapters.iter().enumerate() {
        println!("\n[{}] {}", i + 1, adapter.friendly_name);
        println!("    Description: {}", adapter.description);
        println!("    GUID:        {}", adapter.guid);
        println!("    Status:      {}", if adapter.is_up { "Up" } else { "Down" });
        println!("    IP Address:  {}", or_dash(adapter.ip_addresses.join(", ")));
        println!("    MAC Address: {}", or_dash(adapter.mac_address.clone()));
        println!("    Type:        {}", adapter.adapter_type);
    }
}

fn print_settings(title: &str, scope: Scope, adapter_guid: Option<&str>) {
    let store = SystemStore::system();
    println!("\nSettings — {title}");
    for setting in CATALOG.iter().filter(|setting| setting.scope == scope) {
        let state = store.read(setting, adapter_guid);
        let origin = match reader::classify(setting, &state) {
            Origin::Default => "(Windows default)",
            Origin::Changed => "(changed from default)",
            Origin::Unknown => "",
        };
        println!(
            "  {:<26} {:<28} {origin}",
            setting.name,
            reader::format(setting, &state)
        );
    }
}

fn or_dash(value: String) -> String {
    if value.is_empty() { "-".to_string() } else { value }
}

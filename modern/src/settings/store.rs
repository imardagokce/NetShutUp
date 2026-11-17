//! Where setting values live. `SystemStore` is the real Windows system; the
//! `Store` trait lets the apply/revert engine be tested against a fake.

use windows::Win32::System::Registry::{HKEY, HKEY_LOCAL_MACHINE};

use super::catalog::{ADAPTER_GUID, Setting, Source};
use super::reader::{State, Value};
use super::{interface_dns, registry, service, wmi};

pub trait Store {
    fn read(&self, setting: &Setting, adapter: Option<&str>) -> State;

    /// Sets a setting to `state`; `State::NotSet` removes the value.
    fn write(&self, setting: &Setting, adapter: Option<&str>, state: &State) -> Result<(), String>;

    /// How many trailing levels of the setting's container (its registry
    /// key) do not exist yet and would be created by a write.
    fn missing_container_levels(&self, _setting: &Setting, _adapter: Option<&str>) -> u32 {
        0
    }

    /// Removes up to `levels` trailing container levels, each only if empty.
    fn remove_empty_containers(&self, _setting: &Setting, _adapter: Option<&str>, _levels: u32) {}
}

pub struct SystemStore {
    hive: HKEY,
    /// Prepended to every registry key; empty for the real system.
    prefix: String,
}

impl SystemStore {
    pub fn system() -> Self {
        Self { hive: HKEY_LOCAL_MACHINE, prefix: String::new() }
    }

    /// A store whose registry settings live in a test sandbox key.
    #[cfg(test)]
    pub fn sandbox(name: &str) -> Self {
        Self {
            hive: windows::Win32::System::Registry::HKEY_CURRENT_USER,
            prefix: format!(r"{}\", registry::sandbox::path(name)),
        }
    }

    fn key(&self, template: &str, adapter: Option<&str>) -> String {
        let key = template.replace(ADAPTER_GUID, adapter.unwrap_or_default());
        format!("{}{key}", self.prefix)
    }

    fn registry_key(&self, setting: &Setting, adapter: Option<&str>) -> Option<String> {
        match setting.source {
            Source::RegDword { key, .. } | Source::RegString { key, .. } => {
                Some(self.key(key, adapter))
            }
            Source::Service { .. } | Source::Wmi { .. } | Source::InterfaceDns => None,
        }
    }
}

fn registry_error(code: u32) -> String {
    match code {
        5 => "access denied (administrator rights required)".to_string(),
        code => format!("registry error {code}"),
    }
}

fn parent(key: &str) -> Option<&str> {
    key.rsplit_once('\\').map(|(parent, _)| parent)
}

impl Store for SystemStore {
    fn read(&self, setting: &Setting, adapter: Option<&str>) -> State {
        let from_registry = |read: registry::RegResult<Option<Value>>| match read {
            Ok(Some(value)) => State::Value(value),
            Ok(None) => State::NotSet,
            Err(code) => State::Unavailable(registry_error(code)),
        };

        match setting.source {
            Source::RegDword { key, value } => from_registry(
                registry::read_dword(self.hive, &self.key(key, adapter), value)
                    .map(|found| found.map(Value::Dword)),
            ),
            Source::RegString { key, value } => from_registry(
                registry::read_string(self.hive, &self.key(key, adapter), value)
                    .map(|found| found.map(Value::Text)),
            ),
            Source::Service { name } => service::read(name),
            Source::InterfaceDns => interface_dns::read(adapter.unwrap_or_default())
                .unwrap_or_else(State::Unavailable),
            Source::Wmi { class, filter, property } => {
                match wmi::query_u32(class, filter, property) {
                    Ok(Some(number)) => State::Value(Value::Dword(number)),
                    Ok(None) => State::Unavailable("no value reported".to_string()),
                    Err(reason) => State::Unavailable(reason),
                }
            }
        }
    }

    fn write(&self, setting: &Setting, adapter: Option<&str>, state: &State) -> Result<(), String> {
        let result = match (&setting.source, state) {
            (Source::RegDword { key, value }, State::Value(Value::Dword(number))) => {
                registry::write_dword(self.hive, &self.key(key, adapter), value, *number)
            }
            (Source::RegString { key, value }, State::Value(Value::Text(text))) => {
                registry::write_string(self.hive, &self.key(key, adapter), value, text)
            }
            (
                Source::RegDword { key, value } | Source::RegString { key, value },
                State::NotSet,
            ) => registry::delete_value(self.hive, &self.key(key, adapter), value),
            (Source::Service { name }, State::Value(Value::Service { state, start_type })) => {
                return service::write(name, *start_type, *state);
            }
            (Source::InterfaceDns, state) => {
                return interface_dns::write(adapter.unwrap_or_default(), state);
            }
            (Source::Wmi { .. }, _) => return Err("this setting is read-only".to_string()),
            _ => return Err("value type does not match the setting".to_string()),
        };

        result.map_err(registry_error)
    }

    fn missing_container_levels(&self, setting: &Setting, adapter: Option<&str>) -> u32 {
        let Some(key) = self.registry_key(setting, adapter) else {
            return 0;
        };

        let mut missing = 0;
        let mut current = Some(key.as_str());
        while let Some(key) = current.filter(|key| !registry::key_exists(self.hive, key)) {
            missing += 1;
            current = parent(key);
        }
        missing
    }

    fn remove_empty_containers(&self, setting: &Setting, adapter: Option<&str>, levels: u32) {
        let Some(key) = self.registry_key(setting, adapter) else {
            return;
        };

        let mut current = Some(key.as_str());
        for _ in 0..levels {
            let Some(key) = current else { break };
            if registry::delete_key_if_empty(self.hive, key) != Ok(true) {
                break;
            }
            current = parent(key);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::catalog::{CATALOG, Scope};

    #[test]
    fn every_system_setting_can_be_read() {
        let store = SystemStore::system();
        for setting in CATALOG.iter().filter(|s| s.scope == Scope::System) {
            let state = store.read(setting, None);
            assert!(!matches!(state, State::Unavailable(_)), "{}: {state:?}", setting.id);
        }
    }
}

/// An in-memory store for tests, with injectable failures.
#[cfg(test)]
pub mod testing {
    use std::cell::{Cell, RefCell};
    use std::collections::HashMap;

    use super::Store;
    use crate::settings::catalog::Setting;
    use crate::settings::reader::State;

    pub type Key = (String, Option<String>);

    #[derive(Default)]
    pub struct FakeStore {
        pub values: RefCell<HashMap<Key, State>>,
        /// Fail this many upcoming writes to `fail_id` (to `fail_adapter` only, if set).
        pub fail_id: Option<&'static str>,
        pub fail_adapter: Option<&'static str>,
        pub failures_left: Cell<u32>,
        pub unreadable_id: Option<&'static str>,
        pub writes: Cell<u32>,
        /// Changes what is stored, as Windows might normalise a written value.
        pub mangle: Option<fn(&State) -> State>,
        /// Every successful write, in order: (adapter, state written).
        pub log: RefCell<Vec<(Option<String>, State)>>,
    }

    impl FakeStore {
        fn key(setting: &Setting, adapter: Option<&str>) -> Key {
            (setting.id.to_string(), adapter.map(str::to_string))
        }

        pub fn seed(&self, id: &str, adapter: Option<&str>, state: State) {
            self.values
                .borrow_mut()
                .insert((id.to_string(), adapter.map(str::to_string)), state);
        }

        pub fn snapshot(&self) -> HashMap<Key, State> {
            self.values.borrow().clone()
        }
    }

    impl Store for FakeStore {
        fn read(&self, setting: &Setting, adapter: Option<&str>) -> State {
            if self.unreadable_id == Some(setting.id) {
                return State::Unavailable("simulated".to_string());
            }
            let key = Self::key(setting, adapter);
            self.values.borrow().get(&key).cloned().unwrap_or(State::NotSet)
        }

        fn write(
            &self,
            setting: &Setting,
            adapter: Option<&str>,
            state: &State,
        ) -> Result<(), String> {
            let adapter_matches = self.fail_adapter.is_none() || self.fail_adapter == adapter;
            if self.fail_id == Some(setting.id) && adapter_matches && self.failures_left.get() > 0 {
                self.failures_left.set(self.failures_left.get() - 1);
                return Err("simulated failure".to_string());
            }
            self.writes.set(self.writes.get() + 1);
            self.log.borrow_mut().push((adapter.map(str::to_string), state.clone()));
            let key = Self::key(setting, adapter);
            let stored = match (self.mangle, state) {
                (Some(mangle), state) => mangle(state),
                (None, state) => state.clone(),
            };
            match stored {
                State::NotSet => self.values.borrow_mut().remove(&key),
                stored => self.values.borrow_mut().insert(key, stored),
            };
            Ok(())
        }
    }
}

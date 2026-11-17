//! The real resolver: asks the Windows DNS client, so the answer reflects the
//! encrypted DNS settings that were just applied.

use std::process::{Command, Stdio};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

use windows::Win32::NetworkManagement::Dns::{
    DNS_QUERY_BYPASS_CACHE, DNS_QUERY_NO_HOSTS_FILE, DNS_RECORDA, DNS_TYPE_A, DnsFree,
    DnsFreeRecordList, DnsQuery_W,
};
use windows::core::HSTRING;

use super::Resolver;

/// One lookup gives up after this long; a blocked network must not make the
/// user wait for minutes.
const LOOKUP_DEADLINE: Duration = Duration::from_secs(8);

pub struct SystemResolver;

impl Resolver for SystemResolver {
    fn lookup(&self, name: &str) -> Result<Duration, String> {
        lookup_with(LOOKUP_DEADLINE, name, query)
    }

    fn flush_cache(&self) {
        // Best effort: the lookups bypass the cache anyway.
        let _ = Command::new("ipconfig")
            .arg("/flushdns")
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    }
}

/// Times `query(name)`, giving up after `limit`.
fn lookup_with(
    limit: Duration,
    name: &str,
    query: fn(&str) -> Result<(), String>,
) -> Result<Duration, String> {
    let owned = name.to_string();
    let started = Instant::now();
    match with_deadline(limit, move || query(&owned)) {
        Some(Ok(())) => Ok(started.elapsed()),
        Some(Err(reason)) => Err(format!("{name}: {reason}")),
        None => Err(format!("{name}: timed out")),
    }
}

/// Runs `work` on its own thread and waits at most `limit` for its result. A
/// call that overruns is left to finish on its own; its result is dropped.
fn with_deadline<T: Send + 'static>(
    limit: Duration,
    work: impl FnOnce() -> T + Send + 'static,
) -> Option<T> {
    let (sender, receiver) = mpsc::channel();
    thread::spawn(move || {
        let _ = sender.send(work());
    });
    receiver.recv_timeout(limit).ok()
}

/// One IPv4 lookup through the Windows DNS client, without the cache or the
/// hosts file.
fn query(name: &str) -> Result<(), String> {
    let host = HSTRING::from(name);
    let mut records: *mut DNS_RECORDA = std::ptr::null_mut();
    let code = unsafe {
        DnsQuery_W(
            &host,
            DNS_TYPE_A,
            DNS_QUERY_BYPASS_CACHE | DNS_QUERY_NO_HOSTS_FILE,
            None,
            &mut records,
            None,
        )
    };
    if !records.is_null() {
        unsafe { DnsFree(Some(records.cast_const().cast()), DnsFreeRecordList) };
    }
    if code.0 == 0 { Ok(()) } else { Err(format!("DNS error {}", code.0)) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::thread;

    fn fast_ok(_: &str) -> Result<(), String> {
        Ok(())
    }

    fn slow(_: &str) -> Result<(), String> {
        thread::sleep(Duration::from_millis(300));
        Ok(())
    }

    fn refused(_: &str) -> Result<(), String> {
        Err("DNS error 9003".to_string())
    }

    #[test]
    fn with_deadline_returns_the_result_when_fast() {
        assert_eq!(with_deadline(Duration::from_millis(500), || 7), Some(7));
    }

    #[test]
    fn with_deadline_gives_up_on_a_slow_call() {
        let started = Instant::now();
        let result = with_deadline(Duration::from_millis(50), || {
            thread::sleep(Duration::from_millis(300));
            7
        });
        assert_eq!(result, None);
        assert!(started.elapsed() < Duration::from_millis(250), "waited {:?}", started.elapsed());
    }

    #[test]
    fn lookup_reports_a_timeout_as_an_error_message() {
        let error = lookup_with(Duration::from_millis(50), "slow.test", slow).unwrap_err();
        assert_eq!(error, "slow.test: timed out");
    }

    #[test]
    fn lookup_returns_the_elapsed_time_and_names_failed_names() {
        assert!(lookup_with(Duration::from_millis(500), "fast.test", fast_ok).is_ok());
        let error = lookup_with(Duration::from_millis(500), "nope.test", refused).unwrap_err();
        assert_eq!(error, "nope.test: DNS error 9003");
    }

    /// Needs internet; run by hand with `cargo test -- --ignored`.
    #[test]
    #[ignore]
    fn real_lookup_of_a_public_name() {
        let time = SystemResolver.lookup("example.com").unwrap();
        println!("example.com resolved in {time:?}");
    }
}

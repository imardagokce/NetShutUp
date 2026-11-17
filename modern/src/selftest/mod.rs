//! The tester mode: runs every profile through a real apply and revert,
//! collects an allow-listed set of diagnostic facts, and writes a masked
//! report. Nothing is sent anywhere.

use serde::Serialize;

pub mod diagnostics;
pub mod files;
pub mod mask;
pub mod render;
pub mod runner;
pub mod safety;
pub mod system_env;

use diagnostics::Diagnostics;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Status {
    Pass,
    Fail,
    /// Not run; the detail says why.
    Skip,
    /// Worth knowing, but neither good nor bad.
    Info,
}

#[derive(Debug, Clone, Serialize)]
pub struct Check {
    pub id: String,
    pub title: String,
    pub status: Status,
    pub detail: String,
    pub millis: u64,
}

impl Check {
    /// `detail` is scrubbed of addresses on the way in, so no check can leak one.
    pub fn new(id: &str, title: &str, status: Status, detail: &str, millis: u64) -> Self {
        Self {
            id: id.to_string(),
            title: title.to_string(),
            status,
            detail: mask::scrub(detail),
            millis,
        }
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct Cycle {
    pub profile: String,
    pub checks: Vec<Check>,
}

#[derive(Debug, Clone, Serialize)]
pub struct Report {
    pub schema: u32,
    pub program_version: String,
    pub created: String,
    pub total_millis: u64,
    /// Whether the system ended exactly as it started.
    pub all_restored: bool,
    pub diagnostics: Diagnostics,
    pub environment: Vec<Check>,
    pub safety: Vec<Check>,
    pub reading: Vec<Check>,
    pub cycles: Vec<Cycle>,
    pub closing: Vec<Check>,
}

impl Report {
    pub const SCHEMA: u32 = 1;

    fn all_checks(&self) -> impl Iterator<Item = &Check> {
        self.environment
            .iter()
            .chain(&self.safety)
            .chain(&self.reading)
            .chain(self.cycles.iter().flat_map(|cycle| cycle.checks.iter()))
            .chain(&self.closing)
    }

    /// (pass, fail, skip, info)
    pub fn counts(&self) -> (usize, usize, usize, usize) {
        let count = |wanted: Status| self.all_checks().filter(|c| c.status == wanted).count();
        (count(Status::Pass), count(Status::Fail), count(Status::Skip), count(Status::Info))
    }
}

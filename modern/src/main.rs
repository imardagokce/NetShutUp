mod adapters;
mod commands;
mod elevation;
mod menu;
mod settings;
mod style;
mod version;

use std::process::ExitCode;

use commands::ELEVATED_FLAG;

const DRY_RUN_FLAG: &str = "--dry-run";

const USAGE: &str = "\
Usage:
  netshutup-modern                    Show the current network settings
  netshutup-modern preview <profile>  Show what a profile would change
  netshutup-modern apply <profile>    Apply a profile (asks first, saves a backup)
  netshutup-modern apply <profile> --dry-run
                                      Go through apply without changing anything
  netshutup-modern revert             Undo the most recent apply

Profiles: privacy, performance, recommended";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (flags, words): (Vec<&str>, Vec<&str>) =
        args.iter().map(String::as_str).partition(|arg| arg.starts_with("--"));

    if flags.iter().any(|flag| ![DRY_RUN_FLAG, ELEVATED_FLAG].contains(flag)) {
        eprintln!("{USAGE}");
        return ExitCode::from(2);
    }
    let dry_run = flags.contains(&DRY_RUN_FLAG);
    let elevated_copy = flags.contains(&ELEVATED_FLAG);

    let exit_code = match words.as_slice() {
        [] => commands::scan(),
        ["preview", profile] => commands::preview(profile),
        ["apply", profile] => commands::apply(profile, dry_run, elevated_copy),
        ["revert"] => commands::revert(elevated_copy),
        _ => {
            eprintln!("{USAGE}");
            ExitCode::from(2)
        }
    };

    if elevated_copy {
        commands::wait_for_enter();
    }
    exit_code
}

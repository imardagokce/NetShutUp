mod adapters;
mod commands;
mod elevation;
mod menu;
mod settings;
mod style;
mod terminal;
mod version;

use std::process::ExitCode;

use commands::ELEVATED_FLAG;
use menu::Resume;

const DRY_RUN_FLAG: &str = "--dry-run";

const USAGE: &str = "\
Usage:
  netshutup-modern                    Open the menu
  netshutup-modern scan               Show the current network settings
  netshutup-modern preview <profile>  Show what a profile would change
  netshutup-modern apply <profile>    Apply a profile (asks first, saves a backup)
  netshutup-modern apply <profile> --dry-run
                                      Go through apply without changing anything
  netshutup-modern revert             Undo the most recent apply

Profiles: privacy, performance, recommended";

#[derive(Debug, PartialEq, Eq)]
enum Command {
    /// The menu; `Some` in the administrator copy that continues a choice.
    Menu(Option<Resume>),
    Scan,
    Preview(String),
    Apply(String),
    Revert,
}

/// Turns the words on the command line into a command. Without any words the
/// menu opens, but only where someone can answer it.
fn parse(words: &[&str], interactive: bool) -> Option<Command> {
    match words {
        [] if interactive => Some(Command::Menu(None)),
        ["scan"] => Some(Command::Scan),
        ["preview", profile] => Some(Command::Preview((*profile).to_string())),
        ["apply", profile] => Some(Command::Apply((*profile).to_string())),
        ["revert"] => Some(Command::Revert),
        // How the menu continues in the administrator copy; not in USAGE.
        ["menu", "apply", profile] if interactive => {
            Some(Command::Menu(Some(Resume::Apply((*profile).to_string()))))
        }
        ["menu", "revert"] if interactive => Some(Command::Menu(Some(Resume::Revert))),
        _ => None,
    }
}

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

    let Some(command) = parse(&words, terminal::is_interactive()) else {
        eprintln!("{USAGE}");
        return ExitCode::from(2);
    };

    let exit_code = match &command {
        Command::Menu(resume) => commands::menu(resume.clone()),
        Command::Scan => commands::scan(),
        Command::Preview(profile) => commands::preview(profile),
        Command::Apply(profile) => commands::apply(profile, dry_run, elevated_copy),
        Command::Revert => commands::revert(elevated_copy),
    };

    // The menu keeps its own window usable; the commands need a moment to be read.
    if elevated_copy && !matches!(command, Command::Menu(_)) {
        commands::wait_for_enter();
    }
    exit_code
}

#[cfg(test)]
mod tests {
    use super::*;
    use menu::Resume;

    #[test]
    fn no_arguments_open_the_menu_only_on_a_terminal() {
        assert_eq!(parse(&[], true), Some(Command::Menu(None)));
        assert_eq!(parse(&[], false), None);
    }

    #[test]
    fn scan_is_an_explicit_command() {
        assert_eq!(parse(&["scan"], false), Some(Command::Scan));
    }

    #[test]
    fn existing_commands_still_parse() {
        assert_eq!(parse(&["preview", "privacy"], false), Some(Command::Preview("privacy".into())));
        assert_eq!(parse(&["apply", "privacy"], false), Some(Command::Apply("privacy".into())));
        assert_eq!(parse(&["revert"], false), Some(Command::Revert));
    }

    #[test]
    fn resume_commands_parse() {
        assert_eq!(
            parse(&["menu", "apply", "privacy"], true),
            Some(Command::Menu(Some(Resume::Apply("privacy".into()))))
        );
        assert_eq!(parse(&["menu", "revert"], true), Some(Command::Menu(Some(Resume::Revert))));
    }

    #[test]
    fn unknown_words_are_rejected() {
        assert_eq!(parse(&["bogus"], true), None);
        assert_eq!(parse(&["apply"], true), None);
    }
}

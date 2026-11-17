//! Colour for terminal output. Plain text whenever the output is not a
//! terminal, `NO_COLOR` is set, or the console cannot show colour.

use std::io::{IsTerminal, stdout};
use std::sync::OnceLock;

use crossterm::style::Stylize;

#[derive(Clone, Copy)]
enum Paint {
    Changed,
    Heading,
}

/// A value that differs from the Windows default or is about to change.
pub fn changed(text: &str) -> String {
    paint(text, Paint::Changed, enabled())
}

/// A title or table header.
pub fn heading(text: &str) -> String {
    paint(text, Paint::Heading, enabled())
}

fn colour_allowed(is_terminal: bool, no_color: bool, ansi: bool) -> bool {
    is_terminal && !no_color && ansi
}

fn paint(text: &str, paint: Paint, enabled: bool) -> String {
    if !enabled {
        return text.to_string();
    }
    match paint {
        Paint::Changed => text.yellow().to_string(),
        Paint::Heading => text.bold().to_string(),
    }
}

fn enabled() -> bool {
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| {
        let no_color = std::env::var_os("NO_COLOR").is_some_and(|value| !value.is_empty());
        // Also switches the Windows console into ANSI mode when it can.
        let ansi = crossterm::ansi_support::supports_ansi();
        colour_allowed(stdout().is_terminal(), no_color, ansi)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn colour_needs_a_terminal_ansi_support_and_no_no_color() {
        assert!(colour_allowed(true, false, true));
        assert!(!colour_allowed(false, false, true)); // redirected
        assert!(!colour_allowed(true, true, true)); // NO_COLOR set
        assert!(!colour_allowed(true, false, false)); // console without ANSI
    }

    #[test]
    fn paint_leaves_text_alone_when_colour_is_off() {
        assert_eq!(paint("on", Paint::Changed, false), "on");
    }

    #[test]
    fn paint_wraps_and_resets_when_colour_is_on() {
        let painted = paint("on", Paint::Changed, true);
        assert!(painted.starts_with("\u{1b}["));
        assert!(painted.contains("on"));
        // crossterm resets just the foreground colour it set.
        assert!(painted.ends_with("\u{1b}[39m"), "got {painted:?}");
    }
}

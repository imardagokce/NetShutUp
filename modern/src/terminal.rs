//! The real terminal behind the menu: arrow-key questions through `inquire`.
//! Kept thin; the decisions live in `menu.rs`.

use std::io::{IsTerminal, stdin, stdout};

use inquire::{Confirm, InquireError, Select};

use crate::menu::{Answer, Ui};

const HELP: &str = "↑↓ move · Enter select · Esc back · Ctrl+C quit";

/// Menus need a person at a keyboard: both input and output must be a terminal.
pub fn is_interactive() -> bool {
    stdin().is_terminal() && stdout().is_terminal()
}

pub struct Terminal;

impl Ui for Terminal {
    fn select(&mut self, question: &str, options: &[String]) -> Answer {
        let result = Select::new(question, options.to_vec())
            .without_filtering()
            .with_help_message(HELP)
            .raw_prompt()
            .map(|option| option.index);
        report_unexpected(&result);
        select_answer(result)
    }

    fn say(&mut self, message: &str) {
        println!("\n{message}");
    }
}

/// A yes/no question; the answer defaults to No. Esc and Ctrl+C mean No.
pub fn confirm(question: &str) -> bool {
    let result = Confirm::new(question).with_default(false).prompt();
    report_unexpected(&result);
    confirm_answer(result)
}

fn select_answer(result: Result<usize, InquireError>) -> Answer {
    match result {
        Ok(index) => Answer::Picked(index),
        Err(InquireError::OperationCanceled) => Answer::Back,
        // Ctrl+C, or a terminal that stopped working: leave rather than loop.
        Err(_) => Answer::Quit,
    }
}

fn confirm_answer(result: Result<bool, InquireError>) -> bool {
    result.unwrap_or(false)
}

/// Esc and Ctrl+C are answers; anything else is worth telling the user.
fn report_unexpected<T>(result: &Result<T, InquireError>) {
    if let Err(error) = result {
        if !matches!(error, InquireError::OperationCanceled | InquireError::OperationInterrupted) {
            eprintln!("The terminal could not be read: {error}");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use inquire::InquireError;

    #[test]
    fn escape_and_ctrl_c_at_a_yes_no_question_mean_no() {
        assert!(!confirm_answer(Err(InquireError::OperationCanceled)));
        assert!(!confirm_answer(Err(InquireError::OperationInterrupted)));
        assert!(confirm_answer(Ok(true)));
        assert!(!confirm_answer(Ok(false)));
    }

    #[test]
    fn select_maps_escape_to_back_and_ctrl_c_to_quit() {
        assert_eq!(select_answer(Err(InquireError::OperationCanceled)), Answer::Back);
        assert_eq!(select_answer(Err(InquireError::OperationInterrupted)), Answer::Quit);
        assert_eq!(select_answer(Ok(2)), Answer::Picked(2));
    }
}

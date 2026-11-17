//! The interactive menu's flow. It knows nothing about the terminal or about
//! settings: questions go through `Ui`, work goes through `Actions`.

use crate::commands::Flow;
use crate::elevation::Start;

/// What the user did at a question.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Answer {
    Picked(usize),
    /// Esc: one step back.
    Back,
    /// Ctrl+C: leave the program.
    Quit,
}

pub trait Ui {
    fn select(&mut self, question: &str, options: &[String]) -> Answer;
    fn say(&mut self, message: &str);
}

/// A choice to carry out first in the administrator copy.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Resume {
    Apply(String),
    Revert,
}

impl Resume {
    /// The command line that starts the menu at this choice.
    pub fn args(&self) -> Vec<String> {
        match self {
            Resume::Apply(id) => vec!["menu".to_string(), "apply".to_string(), id.clone()],
            Resume::Revert => vec!["menu".to_string(), "revert".to_string()],
        }
    }
}

pub struct ProfileChoice {
    pub id: String,
    pub label: String,
}

pub trait Actions {
    fn profiles(&self) -> Vec<ProfileChoice>;
    fn scan(&mut self);
    fn apply(&mut self, profile_id: &str) -> Flow;
    fn revert(&mut self) -> Flow;
    /// Starts the administrator copy at `resume`.
    fn hand_over(&mut self, resume: &Resume) -> Start;
}

/// Runs the menu until the user leaves. `resume` is set in the administrator
/// copy: it carries out that choice first, then shows the menu.
pub fn run(ui: &mut dyn Ui, actions: &mut dyn Actions, resume: Option<Resume>) {
    // The copy that was started as administrator must never start another.
    let is_administrator_copy = resume.is_some();

    if let Some(resume) = resume {
        let flow = match &resume {
            Resume::Apply(id) => actions.apply(id),
            Resume::Revert => actions.revert(),
        };
        if flow == Flow::NeedsAdmin {
            ui.say(NO_ADMIN_RIGHTS);
        }
    }

    let main_options: Vec<String> = ["Scan current settings", "Apply a profile", "Revert last change", "Exit"]
        .map(String::from)
        .to_vec();

    loop {
        let (flow, resume) = match ui.select("What would you like to do?", &main_options) {
            Answer::Picked(0) => {
                actions.scan();
                continue;
            }
            Answer::Picked(1) => match choose_profile(ui, actions) {
                ProfileAnswer::Chosen(id) => (actions.apply(&id), Resume::Apply(id)),
                ProfileAnswer::Back => continue,
                ProfileAnswer::Quit => return,
            },
            Answer::Picked(2) => (actions.revert(), Resume::Revert),
            Answer::Picked(_) | Answer::Back | Answer::Quit => return,
        };

        if flow == Flow::NeedsAdmin && !hand_over(&resume, is_administrator_copy, ui, actions) {
            return;
        }
    }
}

const NO_ADMIN_RIGHTS: &str = "Administrator rights could not be obtained. Nothing was changed.";

enum ProfileAnswer {
    Chosen(String),
    Back,
    Quit,
}

fn choose_profile(ui: &mut dyn Ui, actions: &dyn Actions) -> ProfileAnswer {
    let profiles = actions.profiles();
    if profiles.is_empty() {
        ui.say("No profiles are available.");
        return ProfileAnswer::Back;
    }

    let labels: Vec<String> = profiles.iter().map(|profile| profile.label.clone()).collect();
    match ui.select("Choose a profile", &labels) {
        Answer::Picked(index) => match profiles.into_iter().nth(index) {
            Some(profile) => ProfileAnswer::Chosen(profile.id),
            None => ProfileAnswer::Back,
        },
        Answer::Back => ProfileAnswer::Back,
        Answer::Quit => ProfileAnswer::Quit,
    }
}

/// The choice needs administrator rights: continue in an administrator copy.
/// Returns whether this copy should go on showing the menu.
fn hand_over(
    resume: &Resume,
    is_administrator_copy: bool,
    ui: &mut dyn Ui,
    actions: &mut dyn Actions,
) -> bool {
    if is_administrator_copy {
        // Asking again would loop.
        ui.say(NO_ADMIN_RIGHTS);
        return true;
    }

    ui.say(
        "Changing settings needs administrator rights.
         Windows will ask for permission; NetShutUp continues in a new window.",
    );
    match actions.hand_over(resume) {
        Start::Started => false,
        Start::Declined => {
            ui.say("Permission was not given. Nothing was changed.");
            true
        }
        Start::Failed(reason) => {
            ui.say(&format!("{reason}. Nothing was changed."));
            true
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;

    struct FakeUi {
        answers: VecDeque<Answer>,
        questions: Vec<String>,
        said: Vec<String>,
    }

    impl FakeUi {
        fn new(answers: Vec<Answer>) -> Self {
            Self { answers: answers.into(), questions: Vec::new(), said: Vec::new() }
        }

        fn asked(&self, question: &str) -> usize {
            self.questions.iter().filter(|asked| asked.as_str() == question).count()
        }

        fn said_anything_with(&self, text: &str) -> bool {
            self.said.iter().any(|message| message.contains(text))
        }
    }

    impl Ui for FakeUi {
        fn select(&mut self, question: &str, _options: &[String]) -> Answer {
            self.questions.push(question.to_string());
            self.answers.pop_front().expect("the menu asked more questions than the test expected")
        }

        fn say(&mut self, message: &str) {
            self.said.push(message.to_string());
        }
    }

    struct FakeActions {
        calls: Vec<String>,
        flow: Flow,
        start: fn() -> Start,
        profiles: Vec<ProfileChoice>,
    }

    impl FakeActions {
        fn new() -> Self {
            Self {
                calls: Vec::new(),
                flow: Flow::Finished,
                start: || Start::Started,
                profiles: vec![
                    ProfileChoice { id: "privacy".into(), label: "Privacy".into() },
                    ProfileChoice { id: "recommended".into(), label: "Recommended".into() },
                ],
            }
        }

        fn needing_admin(start: fn() -> Start) -> Self {
            Self { flow: Flow::NeedsAdmin, start, ..Self::new() }
        }
    }

    impl Actions for FakeActions {
        fn profiles(&self) -> Vec<ProfileChoice> {
            self.profiles
                .iter()
                .map(|p| ProfileChoice { id: p.id.clone(), label: p.label.clone() })
                .collect()
        }

        fn scan(&mut self) {
            self.calls.push("scan".to_string());
        }

        fn apply(&mut self, profile_id: &str) -> Flow {
            self.calls.push(format!("apply {profile_id}"));
            self.flow
        }

        fn revert(&mut self) -> Flow {
            self.calls.push("revert".to_string());
            self.flow
        }

        fn hand_over(&mut self, resume: &Resume) -> Start {
            self.calls.push(format!("hand_over {}", resume.args().join(" ")));
            (self.start)()
        }
    }

    const MAIN: &str = "What would you like to do?";
    const SCAN: usize = 0;
    const APPLY: usize = 1;
    const REVERT: usize = 2;
    const EXIT: usize = 3;

    #[test]
    fn exit_choice_ends_without_doing_anything() {
        let mut ui = FakeUi::new(vec![Answer::Picked(EXIT)]);
        let mut actions = FakeActions::new();
        run(&mut ui, &mut actions, None);
        assert!(actions.calls.is_empty());
    }

    #[test]
    fn escape_at_the_main_menu_exits() {
        let mut ui = FakeUi::new(vec![Answer::Back]);
        let mut actions = FakeActions::new();
        run(&mut ui, &mut actions, None);
        assert!(actions.calls.is_empty());
    }

    #[test]
    fn ctrl_c_exits_from_the_profile_list() {
        let mut ui = FakeUi::new(vec![Answer::Picked(APPLY), Answer::Quit]);
        let mut actions = FakeActions::new();
        run(&mut ui, &mut actions, None);
        assert!(actions.calls.is_empty());
        assert_eq!(ui.asked(MAIN), 1);
    }

    #[test]
    fn scan_runs_and_returns_to_the_menu() {
        let mut ui = FakeUi::new(vec![Answer::Picked(SCAN), Answer::Picked(EXIT)]);
        let mut actions = FakeActions::new();
        run(&mut ui, &mut actions, None);
        assert_eq!(actions.calls, ["scan"]);
        assert_eq!(ui.asked(MAIN), 2);
    }

    #[test]
    fn escape_at_the_profile_list_goes_back() {
        let mut ui = FakeUi::new(vec![Answer::Picked(APPLY), Answer::Back, Answer::Picked(EXIT)]);
        let mut actions = FakeActions::new();
        run(&mut ui, &mut actions, None);
        assert!(actions.calls.is_empty());
        assert_eq!(ui.asked(MAIN), 2);
    }

    #[test]
    fn a_chosen_profile_is_applied_and_the_menu_returns() {
        let mut ui =
            FakeUi::new(vec![Answer::Picked(APPLY), Answer::Picked(1), Answer::Picked(EXIT)]);
        let mut actions = FakeActions::new();
        run(&mut ui, &mut actions, None);
        assert_eq!(actions.calls, ["apply recommended"]);
        assert_eq!(ui.asked(MAIN), 2);
    }

    #[test]
    fn needing_admin_hands_over_and_ends() {
        let mut ui = FakeUi::new(vec![Answer::Picked(APPLY), Answer::Picked(0)]);
        let mut actions = FakeActions::needing_admin(|| Start::Started);
        run(&mut ui, &mut actions, None);
        assert_eq!(actions.calls, ["apply privacy", "hand_over menu apply privacy"]);
        assert_eq!(ui.asked(MAIN), 1);
        assert!(ui.said_anything_with("Changing settings needs administrator rights."));
    }

    #[test]
    fn declined_permission_returns_to_the_menu() {
        let mut ui = FakeUi::new(vec![Answer::Picked(REVERT), Answer::Picked(EXIT)]);
        let mut actions = FakeActions::needing_admin(|| Start::Declined);
        run(&mut ui, &mut actions, None);
        assert_eq!(actions.calls, ["revert", "hand_over menu revert"]);
        assert_eq!(ui.asked(MAIN), 2);
        assert!(ui.said_anything_with("Permission was not given. Nothing was changed."));
    }

    #[test]
    fn a_failed_start_is_reported_and_the_menu_returns() {
        let mut ui =
            FakeUi::new(vec![Answer::Picked(APPLY), Answer::Picked(0), Answer::Picked(EXIT)]);
        let mut actions = FakeActions::needing_admin(|| Start::Failed("boom".to_string()));
        run(&mut ui, &mut actions, None);
        assert_eq!(ui.asked(MAIN), 2);
        assert!(ui.said_anything_with("boom. Nothing was changed."));
    }

    #[test]
    fn resume_performs_the_choice_first() {
        let mut ui = FakeUi::new(vec![Answer::Picked(EXIT)]);
        let mut actions = FakeActions::new();
        run(&mut ui, &mut actions, Some(Resume::Apply("recommended".to_string())));
        assert_eq!(actions.calls, ["apply recommended"]);
    }

    #[test]
    fn resumed_copy_never_hands_over_again() {
        let mut ui = FakeUi::new(vec![Answer::Picked(EXIT)]);
        let mut actions = FakeActions::needing_admin(|| Start::Started);
        run(&mut ui, &mut actions, Some(Resume::Revert));
        assert_eq!(actions.calls, ["revert"]);
        assert!(ui.said_anything_with("could not be obtained"));
    }

    #[test]
    fn resumed_copy_does_not_hand_over_from_its_own_menu_either() {
        let mut ui = FakeUi::new(vec![Answer::Picked(REVERT), Answer::Picked(EXIT)]);
        let mut actions = FakeActions::needing_admin(|| Start::Started);
        run(&mut ui, &mut actions, Some(Resume::Revert));
        assert_eq!(actions.calls, ["revert", "revert"]);
    }

    #[test]
    fn running_out_of_profiles_is_survivable() {
        let mut ui = FakeUi::new(vec![Answer::Picked(APPLY), Answer::Picked(EXIT)]);
        let mut actions = FakeActions::new();
        actions.profiles.clear();
        run(&mut ui, &mut actions, None);
        assert!(actions.calls.is_empty());
        assert!(ui.said_anything_with("No profiles are available."));
    }
}

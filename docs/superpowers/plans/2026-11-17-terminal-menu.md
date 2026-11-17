# Terminal Menu (Alpha Stage 4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Running `netshutup-modern.exe` with no arguments (a double-click) opens an arrow-key menu for scanning, applying a profile and reverting.

**Architecture:** The menu loop is pure logic in `menu.rs`, driven through two traits: `Ui` (asking and telling) and `Actions` (what the choices do). Tests drive it with scripted fakes. The real `Ui` is a thin `inquire` wrapper in `terminal.rs`; the real `Actions` lives in `commands.rs` and reuses the existing scan/apply/revert code. When a choice needs administrator rights, the menu starts an elevated copy of itself that resumes at that choice, and the first window exits.

**Tech Stack:** Rust 2024, `inquire` 0.9.5 (prompts), `crossterm` 0.29 (colour; already a dependency of `inquire`), existing `windows` 0.61.3.

**Spec:** `docs/design/2026-11-16-alpha-release.md`, section "Terminal menüsü (aşama 4)".

## Global Constraints

- All program text is English.
- No test writes to real settings or triggers a UAC prompt.
- Nothing is changed without a preview and an explicit confirmation; confirmation defaults to "No".
- The command-line `preview`, `apply`, `apply --dry-run` and `revert` keep their current behaviour and output (colour aside).
- No colour codes when output is not a terminal or `NO_COLOR` is set.
- Single `.exe`: new crates are compiled in; no extra files.
- Commits: conventional prefixes, no Co-authored-by line.
- Run commands from `modern/`. Full suite: `cargo test`.

## Review Focus

1. No arguments while input or output is not a terminal (redirect, script): prints usage, exits 2, never waits for a key. → Task 4, `parse` tests.
2. UAC declined from the menu: the same window returns to the main menu and says nothing was changed. → Task 3.
3. The resumed copy is not actually elevated: it must not start another copy (no loop). → Task 3.
4. Nothing to write (profile already matches, profile blocked, no backup): the user is told so without a UAC prompt. → Task 2, `route` tests.
5. Esc or Ctrl+C at a yes/no question counts as "No". → Task 4, `confirm_answer` test.
6. A console without ANSI support (old Windows 10 console): plain text, no stray escape codes. → Task 1, `colour_allowed` test.

---

### Task 1: Colour helper and coloured output

**Files:**
- Create: `modern/src/style.rs`
- Modify: `modern/Cargo.toml`, `modern/src/main.rs` (`mod style;`), `modern/src/commands.rs` (`print_settings`, `print_table`)

**Interfaces:**
- Produces: `style::changed(text: &str) -> String` (yellow), `style::heading(text: &str) -> String` (bold). Both return the text unchanged when colour is off.

- [ ] **Step 1:** Add `crossterm = "0.29.0"` to `[dependencies]`; add `mod style;` to `main.rs`.
- [ ] **Step 2: Write the failing tests** in `style.rs`:

```rust
#[test]
fn colour_needs_a_terminal_ansi_support_and_no_no_color() {
    assert!(colour_allowed(true, false, true));
    assert!(!colour_allowed(false, false, true)); // redirected
    assert!(!colour_allowed(true, true, true));   // NO_COLOR set
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
    assert!(painted.ends_with("\u{1b}[0m"));
}
```

- [ ] **Step 3:** Run `cargo test style` — fails to compile (`colour_allowed`, `paint`, `Paint` missing).
- [ ] **Step 4:** Implement `enum Paint { Changed, Heading }`, `fn colour_allowed(is_terminal: bool, no_color: bool, ansi: bool) -> bool`, `fn paint(text: &str, paint: Paint, enabled: bool) -> String` (crossterm `Stylize`), and a `OnceLock`-cached `fn enabled() -> bool` that feeds `colour_allowed` from `stdout().is_terminal()`, `NO_COLOR` being set and non-empty, and `crossterm::ansi_support::supports_ansi()` (which also switches the Windows console to ANSI mode). `changed` / `heading` call `paint(.., enabled())`.
- [ ] **Step 5:** In `commands.rs`: `print_settings` paints the `(changed from default)` label with `style::changed` and its `Settings — …` title with `style::heading`; `print_table` paints the header row with `style::heading` and the last cell of each data row with `style::changed`. Pad first, paint after, so column widths stay right.
- [ ] **Step 6:** `cargo test` — all pass. `cargo run -q -- preview performance | cat` shows no escape codes.
- [ ] **Step 7:** Commit `feat: colour changed values in terminal output`.

### Task 2: Let callers learn that admin rights are needed

**Files:**
- Modify: `modern/src/commands.rs`, `modern/src/elevation.rs`

**Interfaces:**
- Produces, in `commands.rs`:
  - `pub enum Flow { Finished, NeedsAdmin }`
  - `pub fn menu_apply(profile_id: &str, ask: &mut dyn FnMut(&str) -> bool) -> Flow`
  - `pub fn menu_revert(ask: &mut dyn FnMut(&str) -> bool) -> Flow`
- Produces, in `elevation.rs`:
  - `pub enum Start { Started, Declined, Failed(String) }`
  - `pub fn start_elevated(args: &[&str]) -> Start` — like `relaunch_elevated` but returns as soon as the copy has started.

- [ ] **Step 1: Write the failing tests** in `commands.rs`:

```rust
#[test]
fn nothing_to_write_is_reported_without_asking_for_admin() {
    assert_eq!(route(true, false), Route::Report);
    assert_eq!(route(true, true), Route::Report);
}

#[test]
fn a_real_change_needs_admin_first() {
    assert_eq!(route(false, false), Route::NeedsAdmin);
    assert_eq!(route(false, true), Route::Proceed);
}
```

- [ ] **Step 2:** `cargo test route` — fails to compile.
- [ ] **Step 3:** Implement `enum Route { Report, NeedsAdmin, Proceed }` and `fn route(nothing_to_write: bool, elevated: bool) -> Route`.
- [ ] **Step 4:** Split `apply` into private pieces shared by both entry points: preparing (profile, adapters, plan, needed count), the "nothing to write" verdict (already matches / `apply::check` failed) with its message, and the write step (ask, apply, report). `apply` keeps its exact order and text. `menu_apply` uses `route`: `Report` → preview + verdict message; `NeedsAdmin` → print nothing, return `Flow::NeedsAdmin`; `Proceed` → preview, then the write step.
- [ ] **Step 5:** Same split for `revert`. "Nothing to write" = no backup or unusable backup. `menu_revert` prints the restore table only on `Proceed`.
- [ ] **Step 6:** The write steps take `ask: &mut dyn FnMut(&str) -> bool`; the command-line entry points pass the existing `confirm`. The "To undo" hint after a menu apply reads `To undo: choose "Revert last change"`; the command-line text is unchanged.
- [ ] **Step 7:** In `elevation.rs`, move the `ShellExecuteExW` call into a private function shared by `relaunch_elevated` and the new `start_elevated`.
- [ ] **Step 8:** `cargo test` passes; `cargo run -q -- apply recommended --dry-run` prints the same text as before this task.
- [ ] **Step 9:** Commit `refactor: separate the admin-rights decision from apply and revert`.

### Task 3: The menu loop

**Files:**
- Create: `modern/src/menu.rs`
- Modify: `modern/src/main.rs` (`mod menu;`)

**Interfaces:**
- Consumes: `commands::Flow`, `elevation::Start`.
- Produces:

```rust
pub enum Answer { Picked(usize), Back, Quit }   // Back = Esc, Quit = Ctrl+C

pub trait Ui {
    fn select(&mut self, question: &str, options: &[String]) -> Answer;
    fn say(&mut self, message: &str);
}

pub enum Resume { Apply(String), Revert }
impl Resume { pub fn args(&self) -> Vec<String> }  // ["menu","apply",id] / ["menu","revert"]

pub struct ProfileChoice { pub id: String, pub label: String }

pub trait Actions {
    fn profiles(&self) -> Vec<ProfileChoice>;
    fn scan(&mut self);
    fn apply(&mut self, profile_id: &str) -> Flow;
    fn revert(&mut self) -> Flow;
    fn hand_over(&mut self, resume: &Resume) -> Start;
}

/// `resume`: set in the elevated copy; it performs that choice first.
pub fn run(ui: &mut dyn Ui, actions: &mut dyn Actions, resume: Option<Resume>)
```

Behaviour: main question `What would you like to do?` with options `Scan current settings`, `Apply a profile`, `Revert last change`, `Exit`; profile question `Choose a profile`. After every finished action the main question is asked again. On `Flow::NeedsAdmin`: say `Changing settings needs administrator rights.\nWindows will ask for permission; NetShutUp continues in a new window.`, then `hand_over`. `Started` → `run` returns. `Declined` → say `Permission was not given. Nothing was changed.` and continue. `Failed(reason)` → say `{reason}. Nothing was changed.` and continue. If `resume` was given, `NeedsAdmin` never hands over again: say `Administrator rights could not be obtained. Nothing was changed.`

- [ ] **Step 1: Write the failing tests** with a `FakeUi` (queue of answers; records questions and messages) and `FakeActions` (records calls as strings; configurable `Flow` and `Start`; two profiles `privacy`, `recommended`):

```rust
exit_choice_ends_without_doing_anything        // [Picked(3)] → calls == []
escape_at_the_main_menu_exits                  // [Back] → calls == []
ctrl_c_exits_from_the_profile_list             // [Picked(1), Quit] → calls == [], main asked once
scan_runs_and_returns_to_the_menu              // [Picked(0), Picked(3)] → ["scan"], main asked twice
escape_at_the_profile_list_goes_back           // [Picked(1), Back, Picked(3)] → [], main asked twice
elevated_apply_runs_the_chosen_profile         // Flow::Finished, [Picked(1), Picked(1), Picked(3)] → ["apply recommended"]
needing_admin_hands_over_and_ends              // NeedsAdmin + Started, [Picked(1), Picked(0)] → ["apply privacy", "hand_over menu apply privacy"], no further question
declined_permission_returns_to_the_menu        // NeedsAdmin + Declined, [Picked(2), Picked(3)] → ["revert", "hand_over menu revert"]; message contains "Permission was not given"
failed_start_is_reported                       // NeedsAdmin + Failed("boom") → message contains "boom"
resume_performs_the_choice_first               // resume Apply("recommended"), [Picked(3)] → ["apply recommended"]
resumed_copy_never_hands_over_again            // resume Revert + NeedsAdmin, [Picked(3)] → ["revert"]; message contains "could not be obtained"
running_out_of_profiles_is_survivable          // profiles() empty, [Picked(1), Picked(3)] → [], says "No profiles are available."
```

- [ ] **Step 2:** `cargo test menu` — fails to compile.
- [ ] **Step 3:** Implement `run` and the types above.
- [ ] **Step 4:** `cargo test` passes with no warnings other than unused-code ones for items Task 4 wires up.
- [ ] **Step 5:** Commit `feat: add the menu loop`.

### Task 4: Real terminal, wiring and docs

**Files:**
- Create: `modern/src/terminal.rs`
- Modify: `modern/Cargo.toml`, `modern/src/main.rs`, `modern/src/commands.rs`, `README.md` (no change expected), `modern/README.md`, `docs/design/2026-11-16-alpha-release.md`

**Interfaces:**
- Consumes: everything from Tasks 1–3.
- Produces: `terminal::Terminal` (implements `menu::Ui`), `terminal::confirm(question: &str) -> bool`, `terminal::is_interactive() -> bool`, `commands::menu(resume: Option<Resume>, elevated_copy: bool) -> ExitCode`, and in `main.rs` `enum Command` + `fn parse(words: &[&str], interactive: bool) -> Option<Command>`.

- [ ] **Step 1:** Add `inquire = "0.9.5"` with `default-features = false, features = ["crossterm"]`.
- [ ] **Step 2: Write the failing tests.** In `main.rs`:

```rust
no_arguments_open_the_menu_only_on_a_terminal   // parse(&[], true) == Some(Menu(None)); parse(&[], false) == None
scan_is_an_explicit_command                     // parse(&["scan"], false) == Some(Scan)
existing_commands_still_parse                   // preview/apply/revert as today
resume_commands_parse                           // ["menu","apply","privacy"] → Menu(Some(Apply("privacy"))); ["menu","revert"] → Menu(Some(Revert))
unknown_words_are_rejected                      // parse(&["bogus"], true) == None
```

In `terminal.rs`:

```rust
escape_and_ctrl_c_at_a_yes_no_question_mean_no  // confirm_answer(Err(OperationCanceled)) / Err(OperationInterrupted) == false; Ok(true) == true
select_maps_escape_to_back_and_ctrl_c_to_quit   // select_answer(Err(OperationCanceled)) == Back; Err(OperationInterrupted) == Quit; Ok(2) == Picked(2)
```

In `commands.rs`:

```rust
header_names_the_windows_release_and_rights
// header("0.1.0", (10, 0, 26200), false) == "NetShutUp 0.1.0 · Windows 11 (build 26200) · not elevated"
// header("0.1.0", (10, 0, 19045), true)  == "NetShutUp 0.1.0 · Windows 10 (build 19045) · administrator"
```

(Windows 11 is build 22000 and later.)

- [ ] **Step 3:** `cargo test` — fails to compile.
- [ ] **Step 4:** Implement `terminal.rs`: `Select` without filtering, answered by index; `Confirm` with default `false`; any other prompt error is printed and treated as `Quit` / `false`. `is_interactive` = stdin and stdout are both terminals.
- [ ] **Step 5:** Implement `commands::menu`: print the header (`style::heading`), build the real `Actions` (profile labels `{name:<12} {description}`; `apply`/`revert` call `menu_apply`/`menu_revert` with `terminal::confirm`; `hand_over` calls `elevation::start_elevated` with `resume.args()` plus `ELEVATED_FLAG`), call `menu::run`.
- [ ] **Step 6:** Rewire `main`: use `parse`; `None` → usage on stderr, exit 2. The elevated copy waits for Enter only for the non-menu commands. Update `USAGE`: first line becomes `netshutup-modern    Open the menu`, add `netshutup-modern scan    Show the current network settings`; the `menu …` resume form is not listed.
- [ ] **Step 7:** `cargo test` passes, `cargo build --release` has no warnings. Check by hand: `cargo run -q < /dev/null` prints usage and exits 2; `cargo run -q -- scan | cat` prints the scan without escape codes.
- [ ] **Step 8:** Drive the real menu through a pseudo-console (no UAC paths): Scan → back at menu; Apply → profile list → Esc → menu; Revert with no backup → "There is no change to revert."; Exit.
- [ ] **Step 9:** Docs: `modern/README.md` usage block and "Not there yet" list; design doc stage 4 marked done with what still needs a person at the keyboard (the UAC hand-over).
- [ ] **Step 10:** Commit `feat: open an interactive menu when run without arguments` and `docs: describe the terminal menu`.

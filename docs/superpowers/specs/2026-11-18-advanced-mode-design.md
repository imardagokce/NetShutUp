# Advanced mode (Milestone 6): design

Status: draft for approval. Date: 2026-11-18. Line: Modern (`main`).

## Intent and why

A user picks a profile but disagrees with one item (for example, they want the telemetry service to stay as it is, or they do not want DNS touched). Today the only choices are the whole profile or nothing. The owner's stated need: **turn off a single item of a profile**, and keep that choice for next time. Advanced mode gives a per-item view of a profile, lets the user uncheck items, and lets them save the result as a custom profile.

## Goals

- A new main-menu entry **Advanced** that shows every item of a base profile with its current and target values, each with a checkbox.
- Unchecked items are not changed and not backed up. Checked items go through the existing plan, backup, apply, verify and revert path unchanged.
- Show settings that are read-only or outside the profile (for example the TCP global settings read through WMI) with their current values, clearly marked as not changeable.
- Save the checked items as a **custom profile** that is listed with the built-in profiles in "Apply a profile" and works with `apply <id>` and `preview <id>` on the command line.

## Non-goals

- No new settings and no free-form values. A custom profile can only contain items that a built-in profile already assigns, with the value that profile assigns.
- No editing of a saved custom profile in place (re-open Advanced with it as the base and save again under the same name).
- No GUI, no import/export UI, no sharing, no Enterprise features.
- No change to the backup format, the `revert` command or the self-test.

## Anti-goals (a failure even if it works)

- Writing a value that no one verified against Microsoft documentation.
- A custom profile that applies something the user unchecked, or skips the preview and confirmation.
- A malformed or hand-edited custom profile file crashing the program or being applied unvalidated.

## Constraints

- Profiles stay data: custom profiles use the same JSON schema as `modern/profiles/*.json` and pass the same `profile::parse` validation (which rejects unknown ids and values of the wrong kind; the schema denies unknown fields).
- Setting ids never change. Custom profiles reference catalog ids only.
- Nothing is written without a preview and confirmation. Administrator rights are requested only when applying, as today.
- The menu logic must stay free of terminal and settings code (`menu.rs` talks to `Ui` and `Actions` only) so it stays unit-testable with fakes.
- Tests write only under `HKCU\Software\NetShutUp-Test`; the owner's real settings are never touched without explicit approval.
- Commits: conventional commits, no Co-authored-by line.

## Approaches considered

1. **A. A "customise" step inside "Apply a profile".** Smallest change; no way to see items outside the profile and no saving. Rejected as too narrow once the owner asked for saved profiles.
2. **B. A separate Advanced entry with per-item toggles.** Gives the full view and the checkbox list.
3. **C. Saved custom profiles.** Persistence of the user's choices.
4. **Chosen (owner's decision): B and C together.** The Advanced screen is the editor; saving writes a custom profile file that the rest of the program already knows how to load because it is the ordinary profile format.

## The design

### Flow

1. Main menu gets **Advanced** (between "Apply a profile" and "Revert last change").
2. Choose a base profile (built-in or custom).
3. Item list (multi-select). Rows, top to bottom:
   - Each `settings` entry of the base profile, **checked** by default. One row per setting, except that all firewall base-tier settings form **one row** ("Windows Firewall base: on, inbound blocked") and DNS is **one row** ("DNS: <providers> with DoH"). A DNS row unchecked means no DNS step runs at all.
   - Each `optional` entry of the base profile, **unchecked** by default (the existing default of "No"); checking it counts as the approval the normal flow would ask for. The row shows the profile's question text.
   - A separator, then read-only rows (catalog settings that are not writable, or not in the profile) with the current value and no checkbox.
   - Each changeable row shows `current -> target` and uses the existing value labels and the "changed" colouring.
4. After confirming the list, the user chooses **Apply now**, **Save as custom profile**, or **Back**.
   - **Apply now** runs the normal apply flow for the checked items: preview of what changes, confirmation, backup, write, verify; administrator hand-over as today (`Resume` gains a way to carry the chosen item set, see Resume below).
   - **Save as custom profile** asks for a name (Ui text input), writes the file, and returns to the menu. It does not apply.
5. If nothing is checked, say "Nothing to apply" and do not offer Apply.

### Custom profile files

- Folder: a `profiles` subfolder next to the backups (same base folder the backups use, see `backup::default_dir`). Created on first save.
- One file per profile, `<id>.json`, in exactly the built-in profile schema: `id`, `name`, `description`, `settings`, `optional`, `dns`. The saved file contains only the checked items; `description` records the base profile (for example "Custom, based on Privacy").
- `id` is derived from the name: lower-case ASCII letters, digits and `-`, at most 40 characters, prefixed `custom-` so it can never equal a built-in id. `name` is what the user typed (printable, at most 40 characters, trimmed). Saving under an existing custom name asks to overwrite (default No). Overwriting is the only way to "edit".
- Loading: the menu and the CLI list built-in profiles followed by valid custom ones. A file that fails `profile::parse` (or has an id that does not match its file name, or collides with a built-in id) is skipped and mentioned in one line ("Skipped custom profile X: <reason>"); it never aborts the program.
- Loading also checks the non-goal: every `settings` and `optional` entry must match, by id and value, an entry of some built-in profile. The catalog contains settings that no profile uses because they are unverified (for example `tcp.nagle.no_delay`), so catalog validation alone is not enough. The check runs at load time against the built-ins, so it follows built-in changes; a custom profile that no longer matches is skipped with the reason. *(Builder-check call.)*

### Command line

- `apply <id>` and `preview <id>` accept custom ids (profile lookup reads built-ins plus the custom folder). The "unknown profile" message that lists the available ids must list custom ids too. *(Builder-check call.)*
- No new command. Advanced mode is interactive only.

### Code shape (guidance, builder decides details)

- `Ui` gains a multi-select question and a text-input question; the terminal implementation uses the `inquire` crate already in use (`MultiSelect`, `Text`). `FakeUi` in tests gets matching scripted answers.
- A new module, for example `settings/custom.rs`, holds: building the item rows from a `Profile` plus the catalog and current state, reducing a `Profile` to the checked rows (the pure core, unit-tested), the name-to-id rule, and saving/loading the folder through the `Store`-style seam so tests use a temp directory.
- `Actions` gains the operations the menu needs (list rows for a base, apply a reduced profile, save a custom profile). The existing `apply`, `plan`, `backup` and `revert` functions are reused as they are.
- `Resume` for the administrator hand-over: applying a reduced profile in the elevated copy must reproduce the same item set. Simplest: write the reduced profile to a temporary custom-style file or pass the unchecked ids on the command line; the builder picks, as long as the elevated copy applies exactly the chosen items and nothing else.

### Edge cases

- Base is a custom profile: rows come from its saved `settings`/`optional`; unchecking more and saving works the same.
- Third-party firewall registered: the firewall row is shown as skipped with the existing explanatory note and is not checkable (same rule as `preview`).
- Windows 10 (no encrypted DNS): the DNS row is shown as skipped, as the preview already does, and is not checkable.
- A row whose setting is unreadable shows "unknown" and is checkable only if the existing plan logic would apply it.
- Esc goes one step back at every question; Ctrl+C leaves, as in the current menu.
- A saved profile whose base later gains new items does not receive them; it stays exactly what the user saved.

## Testing

- Unit tests for the pure core (rows from a profile, reducing to checked rows, group rows, id derivation and rejection of bad names, collision with built-in ids).
- Menu tests with `FakeUi`/`FakeActions` for the Advanced flow (apply, save, back, nothing checked, hand-over carrying the item set).
- Save/load round trip in a temp directory, including a corrupt file, a file with an unknown setting id and an id/file-name mismatch.
- One registry-sandbox test (under `HKCU\Software\NetShutUp-Test`) that applies a reduced profile and reverts it, asserting unchecked settings were not written or backed up.
- The existing 222 tests must keep passing; no new compiler warnings.

## Open for the builder

- Exact row wording, colours and the order inside each group.
- The mechanism that carries the chosen item set into the administrator copy.
- Whether the read-only section shows every non-writable catalog entry or only those relevant to the base profile (default: all non-writable entries).
- Naming of modules and functions.

## Documentation to update when built

`README.md` (features, roadmap tick), `CHANGELOG.md` (Unreleased), `modern/README.md` (menu), `docs/TESTING.md` and `docs/TESTING.tr.md` (a short Advanced-mode check), `CLAUDE.md` (Milestone 6).

## Calls settled after the builder check

These were not the owner's to decide; they follow from the intent above. Marked as the author's calls so they can be reviewed.

1. **Approval in the administrator copy.** The Advanced screen is where the user chooses items; it does not ask a second confirmation. The administrator copy shows the normal preview and the single "Apply N changes?" confirmation, as `write_profile` does today. A checked optional row counts as approved, so the per-item optional questions are **not** asked again for Advanced applies (they are still asked in the normal "Apply a profile" flow).
2. **How the item set reaches the administrator copy.** A command-line argument, not a temporary file, so nothing extra shows up as a profile. Shape suggested: `menu apply <base-id> --items <id,id,...>` where the ids are setting ids plus the two group names (`firewall.base`, `dns`). `main.rs` gets a parser arm for it. The elevated copy applies only the listed items.
3. **Optional rows that would be a no-op or are blocked** (already at the target value, or refused by `apply::check`) are hidden, as in the normal flow. "Nothing to apply" is decided after planning, so it can also be reached when the only checked rows are no-ops.
4. **Optional firewall rows are independent** of the firewall base row, as they are asked independently today. Unchecking the base row while keeping an optional row is allowed; the preview shows exactly what will change.
5. **"Exactly the chosen items"** means no other setting is written. The existing safety steps still run and may drop an item: the DNS pre-check and trials can skip or roll back DNS, and `plan_for_precheck` can remove items the system cannot take. This is reported in the output as today.

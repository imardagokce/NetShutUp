# NetShutUp Modern

This folder contains the planned Windows 10/11 product line for NetShutUp.

## Goal

Provide a lightweight Windows network profile configurator focused on:

- read-only diagnostics first
- clear user confirmation before applying changes
- profile-specific configuration for Privacy, Performance, and Balanced modes
- rollback support and user-readable change previews

## Current milestone

The first milestone is not changing settings. It is only:

1. detect Windows version
2. detect administrator privileges
3. enumerate network adapters and current relevant settings
4. summarize findings for the user
5. show proposed changes before any action is taken

## Proposed architecture

- UI layer: modern Windows desktop UI
- service layer: system inspection and profile logic
- rollback layer: preserve previous settings and offer restore actions
- guard layer: explicit warnings before risky operations

## Initial guardrails

- no profile change without user confirmation
- no silent command execution
- no unsupported profile application on untested OS/build combinations
- restore point creation is optional and should be treated as recovery support, not a guarantee

## Design principle

This product line is intended to be faster, clearer, and less legacy-dependent than the older branch. It should focus on current Windows behavior and modern compatibility while maintaining a disciplined safety-first flow.

# Tester Guide — NetShutUp Legacy (Self-test)

Thanks for testing. The self-test checks every profile on your own computer,
undoes every change, and writes a report for us. Nothing is sent anywhere
automatically.

## Before you start

- Windows 7 SP1, 8 or 8.1.
- Run it as administrator. Open Command Prompt as administrator, then:

```text
cd <folder with netshutup-legacy.exe>
netshutup-legacy.exe selftest
```

If you start it from a normal window, the program asks for UAC and opens
an elevated window itself. Answer `y` at the question first (default is No).

## What it does

1. Creates a safety net: a `.reg` export of every registry key it will touch
   (in `%ProgramData%\NetShutUp\safety\<timestamp>\`) and a System Restore
   point when Windows allows it.
2. Runs read-only checks (preview and dry-run for each profile).
3. For each profile (`privacy`, `performance`, `recommended`): applies it,
   verifies the result, reverts it, and compares the values with the state
   before apply. A setting that was "not set" must be "not set" again.
4. Compares the final state with the very first snapshot.
5. Writes the report.

## If a revert fails

The program stops and prints a command like this:

```text
netshutup-legacy revert "<backup file>"
```

Run it in an administrator Command Prompt. If that also fails, double-click
the matching `.reg` file from the safety folder above (accept the prompt).
Then report it (see below) — a failure is exactly what we need to see.

## Where the reports are

- `%ProgramData%\NetShutUp\reports\NetShutUp-selftest-<YYYYMMDD-HHMMSS>.md`
- `%ProgramData%\NetShutUp\reports\NetShutUp-selftest-<YYYYMMDD-HHMMSS>.json`
- A copy of both files on your Desktop.

Open the `.md` file to read the results. Exit code `0` means no check
failed and everything was restored; any other code means something needs a look.

## How to send the report

1. Open a **test report issue** at https://github.com/imardagokce/NetShutUp/issues/new
   (choose the test report template).
2. Attach the `.md` and the `.json` file from your Desktop.
3. Add your Windows version and anything unusual you saw.

## Manual revert

If you want to undo the latest apply by hand, run an administrator Command
Prompt:

```text
netshutup-legacy.exe revert
```

To undo a specific backup, add its path in quotes:

```text
netshutup-legacy.exe revert "<path to .nsub backup>"
```

## What is NOT collected

- Nothing is sent over the network. The program has no telemetry and no
  upload code.
- IP addresses, MAC addresses, adapter GUIDs and user profile paths are
  masked in the reports.
- DNS-over-HTTPS and firewall settings are not part of the Legacy line yet,
  so the self-test does not change them.
- Your files, browser data, passwords and personal documents are not read.

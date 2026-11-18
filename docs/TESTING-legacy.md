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

## Encrypted DNS in the self-test

Applying a profile also sets up encrypted DNS. A small local service named
`NetShutUpDns` listens on `127.0.0.1` (port 53), and the IPv4 DNS of your
network adapters is pointed at it. It forwards queries over DNS-over-HTTPS,
then DNS-over-TLS. If that fails, the original DNS is used. The self-test
does the same thing for each profile and undoes it afterwards.

What you will see in the `.md` report:

- Environment: "TLS 1.2 for encrypted DNS" (should say `available`; on
  Windows 7 it needs update KB3140245), "Local DNS port 53" (should be
  `free`) and "DNS service" (should be `not installed` before the test).
- For each profile: "DNS stage (apply)" and "DNS stage (revert)". Each one
  is Pass, Skip or Fail.
- The before/after comparison includes the adapter DNS servers and the DNS
  service.

Skip messages. A skip is not an error. DNS is left as it was and the rest of
the profile is still applied:

- `no internet connection` - the test ran without internet.
- `no physical network adapter found` - no Ethernet or Wi-Fi adapter.
- `TLS 1.2 is not available` - install KB3140245 (Windows 7).
- `port 53 on 127.0.0.1 is in use` - another DNS program (ICS, a VPN, a
  local DNS tool) is using port 53.
- `no encrypted DNS method works on this network` - the network or a
  firewall blocks DoH and DoT.
- `DNS service is already installed; revert first` - an earlier apply was
  not reverted. Run `netshutup-legacy.exe revert` first.

Fail means the DNS stage itself failed. The program tries to roll the DNS
change back by itself. A FAIL on "DNS stage (revert)" means the original DNS
could not be restored automatically. Use the manual recovery below.

### Manual recovery of DNS

1. Run `netshutup-legacy.exe revert` in an administrator Command Prompt.
   To undo a specific backup, add its path in quotes.
2. If your internet still does not work, set the DNS of each network adapter
   back by hand: Network Connections > adapter > Properties > Internet
   Protocol Version 4 > either "Obtain DNS server address automatically", or
   the servers listed in `dns-before.txt`.
3. Check the state with `netshutup-legacy.exe dns-status` (read-only). It
   should report that the service is not installed.

`dns-before.txt` is written into the safety folder
(`%ProgramData%\NetShutUp\safety\<timestamp>\`). It lists each adapter's
interface index, GUID and DNS servers before the test.

### What to send

- The `.md` and `.json` reports, as described above. They are the main
  thing we need.
- Do not attach `dns-before.txt` or the safety folder unless we ask. They are
  not masked: they contain adapter GUIDs and your DNS servers.
- Mention any DNS skip or fail message word for word, and whether you had to
  run manual recovery.

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
- The self-test changes DNS (see "Encrypted DNS in the self-test") and the
  Windows Firewall settings of each profile. Both are put back at the end.
- Your files, browser data, passwords and personal documents are not read.

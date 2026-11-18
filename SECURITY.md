# Security Policy

NetShutUp Legacy changes Windows network settings and asks for administrator rights
to do so. Security reports are taken seriously.

The project is **in active development** — no public release binary has been tagged yet.
Once releases begin, only the latest commit on the `legacy` branch will be supported.

## Reporting a vulnerability

Please do not open a public issue for a security problem.

Use GitHub's private reporting instead: open the **Security** tab of this
repository and choose **Report a vulnerability**. Include:

- what you found and why it is a problem,
- the Windows version and build (e.g. Windows 7 SP1 x64), and the NetShutUp commit or version,
- steps to reproduce it, if you have them.

You can expect an answer within a few days. This is a small open-source
project, so fixes are made on a best-effort basis.

## What is in scope

- Anything that lets a program or user without administrator rights make
  NetShutUp write something it should not (settings outside its catalog,
  files outside its backup folder).
- Backups that can be tampered with to make `revert` write unexpected values.
- Changes that are applied without the preview and confirmation step.
- A revert that leaves the system in a different state from before the apply.
- Registry paths or values not verified against Microsoft documentation that
  entered a profile and could cause harm.

Plain bugs (wrong text, a crash, a setting shown incorrectly) are not security
issues — please report them as normal GitHub issues.

## Scope note — Legacy vs. Modern

This policy covers the **Legacy** branch (`legacy`), targeting Windows 7 SP1, 8 and 8.1.
For the Modern line (Windows 10/11, `main` branch), see the security policy there.
The two product lines are independent and do not share code.

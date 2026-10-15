# Privacy

This repository contains a source-only, generalized utility. It deliberately
excludes personal paths, computer/user names, device IDs, private test reports,
runtime logs, screenshots of desktops, credentials, compiled/debug artifacts, and
machine-specific symbol configuration. Test fixtures are synthetic.

The application performs no telemetry or unattended network requests. It reads
local Windows display and taskbar state and writes local recovery settings and
diagnostic logs. Those logs can include paths and screen geometry; review/redact
them before posting an issue.

The optional symbol-preparation command downloads public Windows debug symbols
from Microsoft's symbol server. Its request identifies the installed public
Windows module version. It does not send monitor identifiers or application logs.
When a Windows update replaces the taskbar, pressing Enable asks before making
the same request from the app; background startup never downloads anything.

Git history for this public source starts clean. It does not import a development
workspace's history or local verification evidence. Contributions should likewise
avoid committing `dist`, symbols, logs, local settings, or real personal email
addresses in Git author metadata when privacy is desired.

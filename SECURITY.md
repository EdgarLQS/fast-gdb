# Security policy

## Supported versions

The latest development state on `main` is the actively maintained version.
Older releases may not receive security fixes unless the release notes say
otherwise.

## Reporting a vulnerability

Please do not open a public issue for an exploitable vulnerability. Use the
[private GitHub Security Advisory form](https://github.com/EdgarLQS/fast-gdb/security/advisories/new)
when that feature is available. If it is unavailable, contact the repository
maintainer through the private contact mechanism on the `EdgarLQS/fast-gdb`
GitHub repository and include `[SECURITY]` in the subject.

Please include:

- the affected commit, release, platform, and GDAL version if applicable;
- a minimal reproducer or FileGDB fixture that can be legally shared;
- expected and observed behavior;
- any suggested mitigation.

Redact credentials, access tokens, private datasets, and proprietary ArcGIS
or customer data before sending a report.

The project is especially interested in memory-safety issues, malformed input
handling, path traversal, denial-of-service behavior, and unsafe interaction
between FileGDB readers and GDAL-backed components.

## Scope notes

fast-gdb is a Reader-only library. Same-directory GDAL update operations while
fast-gdb readers remain open are unsupported; reports involving that workflow
should state the exact lifecycle and whether the documented close/edit/reopen
sequence was followed.

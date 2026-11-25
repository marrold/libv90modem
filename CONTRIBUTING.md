# Contributing

**Every fix comes with a test, seen failing first.** Write the test, run it
against the code as it is and see it fail, then make the change and see it
pass. Say in the commit what the test showed before and after.

**A quirk of a real modem comes with its recording.** When a fault was found
on a call with a real modem, add the recording of that call as a fixture
(`test/fixtures/`, see `tools/README.md` for the format and `fixtures.txt`
there for the settings each one was recorded with), and a test that replays
it and fails without the fix. Name the modem, its firmware and the date.

**Originality.** Write from the ITU-T recommendations. Do not copy code from
other modem or compression implementations (spandsp, D-Modem, vendor
sources); spandsp may be used as a peer in the interop tests, never copied.

**Code.** Portable C99. Fixed-point only in the signal path (`sim/` and the
tests may use double). No allocation after `v90m_create`; no global or
static mutable state: two modems in one process must stay independent.
Nothing outside `include/v90modem.h` is part of the API.

**Checks.** A change is ready when `make check` passes, and `make check-arm`
for anything in the signal path. A change that should not alter what the
modem sends shows it: the same transmit hash from `make bench-v90`, and
`make golden` with no golden file changed. A golden file is re-recorded
only when the change explains it, and the commit says which and why
(`docs/testing.md`; `make verify` runs all of this at once).

By contributing you agree that your contribution is licensed under
GPL-2.0-only, like the rest of the library.

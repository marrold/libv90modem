# ARM CI on a throwaway Mythic Beasts Pi 4 — setup

The idea: GitHub's own cloud runner does the x86 build + tests (`host`); for
the ARM suite it provisions a Mythic Beasts Pi 4 on demand, runs
`make check-arm` on it natively (no qemu — the A72 runs the 32-bit ARMv5
binaries at EL0), then deletes it. Nothing is left running; idle cost is
zero. None of this lives on the branches you send upstream.

This supplements `.github/workflows/ci.yml` on `main`, which already runs
`make check-arm` under qemu on every push/PR (and stays within GitHub's
360-minute job limit only by compiling with `TEST_SHORT`, per its comment).
The Pi here runs the same target several times faster, so it's both a
cheaper/faster on-demand check and, if you want it, room to drop
`TEST_SHORT` (`EXTRA_CPPFLAGS=-DTEST_FULL` in `tests.yml`'s "arm" job) for
full coverage — not measured yet on real hardware, so time a run before
relying on it as a merge gate.

## 1. Repo layout (keeps `main` a clean mirror of upstream)

| branch      | role                         | default? |
|-------------|------------------------------|----------|
| `arm-ci`    | holds `.github/` + this file | yes      |
| `main`      | tracks upstream `main` (`georgica/libv90modem`) | no |
| `feature/*` | work, **cut from `main`**    | no       |

Not named `ci`: upstream (`georgica/libv90modem`) already has its own,
unrelated, long-dormant `ci` branch (pre-`ci.yml` CI history); reusing that
name on the fork's default branch would be confusing every time you look at
a branch list that has both.

- Set `arm-ci` as the **default branch** (Settings → Branches).
  `workflow_dispatch` and `issue_comment` triggers only register on the
  default branch — that is the one requirement.
- Always branch features off `main` (`git checkout -b feature/x main`) so their
  diff vs upstream carries no CI files.
- `.github/workflows/tests.yml` (this folder) lives only on `arm-ci`; `main`
  keeps its own `ci.yml` (the qemu-based suite, synced from upstream).

Fork vs standalone mirror: a GitHub *fork* works here — `tests.yml` has no
`on: schedule`, so GitHub's restriction on scheduled workflows on forks
doesn't apply; the stale-Pi reaper runs as a step inside the workflow instead
of a cron job for exactly that reason.

## 2. Mythic Beasts API key (NOT your account login)

Reuse the one already set up for `v90d-fork` if you have it (scoped to the
Pi API only), or make a fresh one:

1. Log into the control panel with your normal account.
2. Go to **API users** (`/customer/api-users`) → create a key.
3. **Scope it to the Raspberry Pi API only** — if it leaks via CI it can then
   only create/delete Pis, not touch billing/DNS/account.
4. Copy the **Key ID** and the **secret** (the secret is shown once).

## 3. Dedicated CI SSH key

```sh
ssh-keygen -t ed25519 -f ci_pi -N ''     # ci_pi (private), ci_pi.pub (public)
```
The workflow injects the public half at provision time; you only store the
private half as a secret. Reusing the `v90d-fork` keypair is fine too — it's
only ever used to reach a throwaway Pi.

## 4. GitHub secrets & variables (Settings → Secrets and variables → Actions)

Secrets (per-repo — GitHub doesn't share these across repos, even forks of
the same account, so these need setting here even if `v90d-fork` already has
them):
- `MB_KEY_ID`  — the API Key ID
- `MB_SECRET`  — the API secret
- `MB_SSH_KEY` — contents of the **private** `ci_pi` file

Optional variable:
- `MB_OS_IMAGE` — override the OS image. Unset uses the API's own default.

Because a run needs either write access (`workflow_dispatch`) or an
OWNER/MEMBER/COLLABORATOR's `/test` comment, these secrets are never exposed
to an untrusted fork-PR run.

## 5. Running it

```sh
gh workflow run tests.yml -f test_ref=feature/x -f scope=all   # from the arm-ci branch
```
Or comment on a PR in this repo: `/test`, `/test host`, or `/test arm`.

Flow: `host` runs immediately on GitHub's cloud; `arm` provisions a Pi
(~2-3 min), SSHes in, runs `make check-arm QEMU= ARM_CC=gcc` natively, and
deletes the Pi (even on failure/cancel). The first step of `arm` also reaps
any orphaned `ci-*` Pi older than 2 h.

## 6. Gate merges (optional, in your own repo)

Open an internal PR `feature/x → main`, comment-trigger or dispatch the run,
and it posts a `tests` commit status on the PR's head SHA. Add a
branch-protection rule on `main` requiring that status, and merges block
until it's green. (Upstream has its own merge rules; this is just your gate.)

## 7. Send it upstream

```sh
gh pr create --repo georgica/libv90modem --base main --head marrold:feature/x
```
Because `feature/x` was cut from the clean `main` mirror, the diff is only
your code — no `.github`, no CI.

## Known limitation

`make check-arm` already loops the submodules (`v34 v90 v92 v42 v32`, via
`SUBS` in the Makefile) on its own, so the Pi step above is genuinely just
one command — there's no separate per-submodule step to add here, unlike
`v90d`'s `fw/` (which this project has no equivalent of).

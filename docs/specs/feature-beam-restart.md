# BEAM restart and recovery Specification v0.1.0

> **Spec type:** Feature
> **Path:** `docs/specs/feature-beam-restart.md`

## Overview

The native host respawns a packaged BEAM child after an unexpected exit, with
shared backoff and attempt limits. After a run of startup crashes it may run a
configured Elixir recovery script through Mix release `eval` (OTP and Elixir
load; the application does not start). `--edw-recover` runs that same `eval`
path as a one-shot CLI.

**Integration context:** Host-first packaged spawn in
`native/{macos,windows,linux}/` `HostController` plus process-shell CLI.
Replaces `heart` for desktop bundles.

## Design Principles

1. **One contract on every OS.** Same ini keys, flags, backoff formula, and
   counters.
2. **Reset on `initialize` only.** Do not reset attempt counters on spawn.
3. **Recovery is Mix `eval`, not `start` or `rpc`.** The broken application
   must not boot.
4. **`--edw-recover` is the same helper** as automatic recovery, for tests and
   manual use.
5. **`--edw-no-beam` does not spawn, restart, or recover.**

---

## Output Structure

**Do generate:** host respawn + recovery, `--edw-recover`, packaging docs,
Elixir E2E.

**Do not generate:** JSON-RPC methods, native unit-test frameworks.

---

## Type Conventions

| Spec type | Meaning | Examples |
|-----------|---------|----------|
| `milliseconds` | Integer delay | `500`, `5000` |
| `count` | Non-negative integer | `0` = no cap for max attempts |
| `recovery_script` | Path to `.exs` | `recovery.exs` |

### Normalization

- Relative `recovery_script` resolves like `beam.path`.
- CLI `--edw-recovery-script=` and ini `[lifetime] recovery_script` use the
  existing ini-over-CLI merge for overlapping keys. `--edw-recover` is CLI only.
- `--edw-recover` honors `--edw-config`, `--edw-beam-path`, `--edw-beam-app`.

---

## Error Handling

| Condition | Result |
|-----------|--------|
| `--edw-rpc` and `--edw-recover` together | non-zero, no eval |
| `--edw-recover` and no script / missing file | non-zero, no `start` |
| Automatic recovery `eval` fails | log stderr, still respawn `start` |
| Restart cap reached | host process exits |

---

## Restart policy

Defaults:

- `restart_beam` = true
- `restart_max_attempts` = 0 (no cap)
- `restart_backoff_ms` = 500
- `recovery_after` = 3
- `recovery_script` unset (automatic recovery off)

Backoff after consecutive unexpected exit *n* (1-based):

`min(restart_backoff_ms * 2^min(n-1, 4), 5000)`

So 500, 1000, 2000, 4000, then 5000 ms.

**Startup crash:** child exits and `initialize` has not succeeded for that
child. Capture this **before** session reset (reset clears `initialized`).

**Runtime crash:** child exits after a successful `initialize`.

**Clean exit:** `system.prepare_quit` window, or host-initiated quit. Do not
respawn.

On successful `initialize`: set `restart_attempts = 0` and
`startup_failures = 0`.

On unexpected exit, if `restart_beam`:

1. If startup crash: `startup_failures += 1`. If `recovery_script` is set and
   `recovery_after > 0` and `startup_failures >= recovery_after`, run recovery
   `eval`, then set `startup_failures = 0`.
2. `restart_attempts += 1`. If `restart_max_attempts > 0` and
   `restart_attempts >= restart_max_attempts`, exit the host (no further
   spawn).
3. Else wait backoff and spawn `start` again.

Do not run recovery on runtime crashes (`initialize` already reset
`startup_failures`).

`recovery_after = 0` disables automatic recovery. `--edw-recover` still works.

---

## Recovery `eval`

Command (Unix):

```text
{beam}/bin/{app} eval "Code.eval_file(\"ABS_PATH\")"
```

Windows: `{app}.bat eval ...` through `cmd.exe /c` as for `start`.

Working directory: beam working_dir or beam dir. Extra `[env]` from ini.
Do not require `EDW_PORT`.

This is Mix **eval**: OTP + Elixir, application **not** started.

---

## API Surface (Behaviors)

### `--edw-recover` → eval exit_code

One-shot. No UI, no `listening`, no `start`.

**Behavior:**

| Condition | Output |
|-----------|--------|
| Script present | run recovery `eval`, forward stdio, exit with eval status |
| Script missing | non-zero |
| Combined with `--edw-rpc` | non-zero |

Automatic crash-loop recovery MUST call this same helper.

### Ini `[lifetime]`

| Key | Default | Role |
|-----|---------|------|
| `restart_beam` | true | Enable respawn |
| `restart_max_attempts` | 0 | Cap consecutive unexpected exits |
| `restart_backoff_ms` | 500 | Initial backoff |
| `recovery_script` | unset | Path to `.exs` |
| `recovery_after` | 3 | Startup crashes before automatic eval |

CLI: `--edw-restart-beam=`, `--edw-max-restart-attempts=`,
`--edw-restart-backoff-ms=`, `--edw-recovery-script=`,
`--edw-recovery-after=`, `--edw-recover`.

---

## Testing

Cases live in [tests-beam-restart.yaml](tests-beam-restart.yaml). Shared Elixir
E2E is the source of truth.

## Generated Documentation

Packaging lifetime section, porting checklist, status matrix rows.

## Implementation Checklist

- [ ] Counters reset only on `initialize`
- [ ] Windows parses the same restart CLI flags as macOS/Linux
- [ ] Recovery helper shared with `--edw-recover`
- [ ] E2E for recover CLI, crash loop, and max attempts
- [ ] Status `done` only when E2E is green

## Version History

- **v0.1.0** - Initial specification

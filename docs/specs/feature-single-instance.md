# Native single-instance Specification v0.1.0

> **Spec type:** Feature
> **Path:** `docs/specs/feature-single-instance.md`

## Overview

When `instances = single`, the native host owns process uniqueness. The first
host binds a per-user control socket for its process lifetime. A second launch
does **not** connect to the Elixir EDW TCP socket. It sends `instance.activate`
on the control socket, the running host raises existing windows and forwards
open-url / open-file / reopen to BEAM, and the second process exits 0.

**Integration context:** Host process shell in `native/{macos,windows,linux}/`.
The EDW TCP session stays one Elixir client ([protocol.md](../protocol.md)
“Single client”). `--edw-rpc` is a client of this same control socket
([feature-edw-rpc.md](feature-edw-rpc.md)).

## Design Principles

1. **Host owns the lock.** Apps do not need `epmd` or a named BEAM node to
   detect a second launch.
2. **Do not replace the Elixir TCP client.** Second launch and `--edw-rpc`
   use the control socket only.
3. **Default is multi.** Current E2E and `--edw-no-beam` keep working.
   Packaged apps set `instances = single` in their ini.
4. **Same contract on every OS.** Same ini keys, flags, activate argv rules,
   and exit codes.
5. **One-shot exclusive modes do not bind as server.** `--edw-rpc` is a
   control-socket client. `--edw-recover` stays Mix `eval` and does not use
   the socket. A normal UI host, including `--edw-no-beam`, binds when
   `instances = single`.

---

## Output Structure

**Do generate:** control-socket server/client per platform, ini/CLI, Elixir E2E.

**Do not generate:** a second Elixir TCP client, shared Swift/C++/GTK UI,
native unit-test frameworks, changes to `--edw-recover`.

---

## Type Conventions

| Spec type | Meaning | Examples |
|-----------|---------|----------|
| `instances` | `multi` (default) or `single` | `single` |
| `instance_id` | Lock name; default is the host exe basename | `ddrive`, `DesktopWebView` |
| `argv` | Forwarded argv after `--edw-*` strip | `["ddrive://invite/x"]` |
| `exit_code` | Process status | `0` success, non-zero failure |

### Normalization

- CLI `--edw-instances=` and `--edw-instance-id=` use the same ini-over-CLI
  merge as other overlapping keys.
- Relative paths in activate argv resolve from the current working directory
  of the second process.
- `instance_id` is sanitized for the socket / pipe name (keep `[A-Za-z0-9._-]`).

---

## Error Handling

| Condition | Result |
|-----------|--------|
| `instances = multi` | No lock. A second host starts normally. |
| Bind fails and `instance.activate` succeeds | Second process exits 0 |
| Bind fails and activate cannot reach a host | Second process exits non-zero |
| `--edw-rpc` when `instances = multi` | Non-zero (no running single-instance host) |
| `--edw-rpc` when no host holds the lock | Non-zero |

---

## Ini and CLI

```ini
[lifetime]
# multi (default) | single
instances = single
# lock name; default is the host exe basename
# instance_id = ddrive
```

| Flag | Meaning |
|------|---------|
| `--edw-instances=multi\|single` | Instance mode |
| `--edw-instance-id=NAME` | Lock name |

On BEAM spawn, the host sets `RELEASE_DISTRIBUTION=none` when that env key is
unset. Apps can still set a name if they want distribution.

---

## Control socket

When `instances = single`, the first host binds a per-user local endpoint and
holds it for the process lifetime:

- macOS/Linux: Unix socket `$TMPDIR/edw-{uid}-{instance_id}.sock`
  (`TMPDIR` falls back to `/tmp`)
- Windows: named mutex `Local\edw-{instance_id}` plus named pipe
  `\\.\pipe\edw-{uid}-{instance_id}`

Framing: same **4-byte big-endian length + JSON-RPC 2.0** as EDW.

| Method | Params | Result |
|--------|--------|--------|
| `instance.activate` | `{argv: [string]}` | `true` |
| `instance.eval` | `{expr: string}` | `{inspect: string}` |

`instance.eval` forwards host→client `rpc.eval` on the existing EDW session
([protocol.md](../protocol.md)). No initialized Elixir client → JSON-RPC
error; the `--edw-rpc` process exits non-zero.

A stale Unix socket file (nothing listens) is removed and the first host
binds again.

---

## Activate argv

The second process strips `--edw-*` first (same as BEAM forward). Then, for
each remaining argument (or once with an empty list):

| Argv | Host action |
|------|-------------|
| empty | `event.system.reopen` and raise existing native windows |
| `scheme:` (including `file:` and `ddrive:`) | `event.system.open_url` |
| else if the path exists | `event.system.open_file` |
| else | `event.system.open_url` with the raw string |

A Windows drive path (`C:\...`) is not a URL scheme. Raise / show existing
native windows (same role as `Desktop.Window.show` in `do_focus`).

---

## Testing

Cases live in [tests-single-instance.yaml](tests-single-instance.yaml). Elixir
E2E under `test/e2e/` MUST implement them. Hosts MUST NOT add XCTest / gtest
as the source of truth.

## Generated Documentation

Packaging `[lifetime] instances` / `instance_id`, CLI flags, porting checklist
row, AGENTS.md hard rule, status rows.

## Implementation Checklist

- [x] macOS / Windows / Linux control socket
- [x] Activate argv classification + raise windows
- [x] `RELEASE_DISTRIBUTION=none` when unset
- [x] E2E cases from tests-single-instance.yaml
- [x] Status row `done` only when E2E is green

## Version History

- **v0.1.0** - Initial specification

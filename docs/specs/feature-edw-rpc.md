# `--edw-rpc` Specification v0.2.0

> **Spec type:** Feature
> **Path:** `docs/specs/feature-edw-rpc.md`

## Overview

The native `DesktopWebView` binary exposes a one-shot `--edw-rpc <expr>` CLI.
It connects to the **control socket** of a running single-instance host, asks
that host to evaluate an Elixir expression on the existing EDW session
(`rpc.eval`), prints the inspected return value, and exits.

This path does **not** use `erl_call`, a distribution cookie, or `epmd`.

**Integration context:** Host process shell in `native/{macos,windows,linux}/`.
Config discovery follows [docs/packaging.md](../packaging.md). The control
socket is specified in [feature-single-instance.md](feature-single-instance.md).
The host→client method is specified in [docs/protocol.md](../protocol.md).

## Design Principles

1. **One-shot, no UI.** `--edw-rpc` does not listen, print `listening`, spawn
   `start`, create a window, or bind the control socket as a server.
2. **Elixir in, inspect out.** The public expression is Elixir. Stdout is
   `Kernel.inspect/1` of the value plus a newline.
3. **Ask the running host.** The CLI is a control-socket client
   (`instance.eval`). The host forwards `rpc.eval` to the initialized Elixir
   client.
4. **Single-instance only.** If `instances = multi`, or no host holds the
   lock, exit non-zero.
5. **Same contract on every OS.** macOS, Windows, and Linux use the same
   flags, discovery order, and exit codes.
6. **Mutually exclusive with `--edw-recover`.** `--edw-recover` stays Mix
   `eval` and does not use the control socket.

---

## Output Structure

**Do generate:** native CLI client, packaging docs, Elixir E2E.

**Do not generate:** `erl_call` cookie/node discovery, a second Elixir TCP
client, native unit-test frameworks.

---

## Type Conventions

| Spec type | Meaning | Examples |
|-----------|---------|----------|
| `elixir_expr` | UTF-8 Elixir source | `1+1`, `DesktopWebview.Binary.available?()` |
| `instance_id` | Control-socket lock name | `edw-rpc-42` |
| `exit_code` | Process status | `0` success, non-zero failure |

### Normalization

- `--edw-rpc <expr>` (next argv) and `--edw-rpc=<expr>` are the same.
- The client uses the same ini-over-CLI merge as other overlapping keys
  (`--edw-config`, `--edw-instances`, `--edw-instance-id`).

---

## Error Handling

| Platform host | Error style |
|---------------|-------------|
| Native CLI | Message on stderr, non-zero process exit |

| Condition | Exit | Stderr |
|-----------|------|--------|
| `--edw-rpc` and `--edw-recover` together | non-zero | mutually exclusive |
| Missing expression | non-zero | usage |
| `instances = multi` | non-zero | no running single-instance host |
| No host / connect failed | non-zero | no running single-instance host |
| No initialized Elixir client | non-zero | control `instance.eval` error |
| Eval error | non-zero | JSON-RPC `-32000` message |

---

## API Surface (Behaviors)

### `--edw-rpc <expr>` → stdout + exit_code

Evaluate `expr` on the Elixir client of the running single-instance host.

**Arguments:**

- `expr`: Elixir source. Required.

**Behavior:**

| Condition | Output |
|-----------|--------|
| Success | `inspect(value)` and a newline on stdout (and stderr), exit 0 |
| No host / multi / not initialized | non-zero |
| Combined with `--edw-recover` | non-zero, no eval |

**Eval method:** Connect to the control socket. Send JSON-RPC
`instance.eval` `{expr}`. The host sends EDW request `rpc.eval` to the
Elixir client. `DesktopWebview.Transport` runs `Code.eval_string/1` and
returns `{inspect}`.

Do not `halt` the remote VM.

**Examples:**

- `--edw-rpc '1+1'` → stdout `2`
- `--edw-rpc 'DesktopWebview.Binary.available?()'` → `true` when that
  module is loaded in the connected client

**Edge cases:**

- Empty expression → error
- Quotes and newlines in `expr` → JSON string, no shell interpolation of
  the remote source

---

## Testing

Cases live in [tests-edw-rpc.yaml](tests-edw-rpc.yaml). Elixir E2E under
`test/e2e/` MUST implement them. Hosts MUST NOT add XCTest / gtest as the
source of truth.

## Generated Documentation

Packaging CLI table. Porting checklist row for `--edw-rpc`. Protocol
`rpc.eval`.

## Implementation Checklist

- [x] macOS / Windows / Linux one-shot CLI via `instance.eval`
- [x] Mutual exclusion with `--edw-recover`
- [x] E2E cases from tests-edw-rpc.yaml
- [x] Status row `done` only when E2E is green

## Version History

- **v0.2.0** - Control socket + `rpc.eval`; drop `erl_call`
- **v0.1.0** - Initial specification (`erl_call`)

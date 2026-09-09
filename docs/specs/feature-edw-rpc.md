# `--edw-rpc` Specification v0.1.0

> **Spec type:** Feature
> **Path:** `docs/specs/feature-edw-rpc.md`

## Overview

The native `DesktopWebView` binary exposes a one-shot `--edw-rpc <expr>` CLI.
It evaluates an Elixir expression on a **running** packaged BEAM node through
erts `erl_call`, prints the inspected return value, and exits.

**Integration context:** Host process shell in `native/{macos,windows,linux}/`.
Config discovery follows [docs/packaging.md](../packaging.md). This is not
JSON-RPC (`docs/protocol.md`).

## Design Principles

1. **One-shot, no UI.** `--edw-rpc` does not listen, print `listening`, spawn
   `start`, or create a window.
2. **Elixir in, inspect out.** The public expression is Elixir. The host wraps
   it for `erl_call`. Stdout is `Kernel.inspect/1` of the value plus a newline.
3. **Release files supply cookie and node.** Ini may override. The host does
   not invent a cookie.
4. **Do not start BEAM.** If the node is down, exit non-zero.
5. **Same contract on every OS.** macOS, Windows, and Linux use the same flags,
   discovery order, and exit codes.
6. **Mutually exclusive with `--edw-recover`.**

---

## Output Structure

**Do generate:** native CLI handling, packaging docs, Elixir E2E.

**Do not generate:** JSON-RPC methods, native unit-test frameworks, a second
RPC protocol.

---

## Type Conventions

| Spec type | Meaning | Examples |
|-----------|---------|----------|
| `elixir_expr` | UTF-8 Elixir source | `1+1`, `node()` |
| `node_name` | Erlang node | `my_app@127.0.0.1`, short `my_app` |
| `cookie` | Distribution cookie string | contents of `releases/COOKIE` |
| `exit_code` | Process status | `0` success, non-zero failure |

### Normalization

- `--edw-rpc <expr>` (next argv) and `--edw-rpc=<expr>` are the same.
- Relative `beam.path` resolves from the resources / executable directory as
  in packaging.md.
- A node name with `@` from `-name` is a long name. `-sname` is a short name.

---

## Error Handling

| Platform host | Error style |
|---------------|-------------|
| Native CLI | Message on stderr, non-zero process exit |

| Condition | Exit | Stderr |
|-----------|------|--------|
| `--edw-rpc` and `--edw-recover` together | non-zero | mutually exclusive |
| Missing expression | non-zero | usage |
| `erl_call` not found | non-zero | path search failed |
| Cookie or node not found | non-zero | discovery failed |
| Node down / `erl_call` fails / eval error | `erl_call` status | `erl_call` stderr |

---

## Discovery

Search order is the same on every OS.

**Cookie**

1. Ini `[beam] cookie`
2. Ini `[beam] cookie_file` (file contents, trim newline)
3. `{beam}/releases/COOKIE`
4. `-setcookie` in `vm.args`

**Node**

1. Ini `[beam] node`
2. `-name` or `-sname` in `{beam}/releases/<vsn>/vm.args` (`start_erl.data` or
   first `releases/*/vm.args`)

**`erl_call` binary** (`.exe` on Windows)

1. `{beam}/erts-*/bin/erl_call`
2. `{beam}/lib/erl_interface-*/bin/erl_call`
3. `PATH`

---

## API Surface (Behaviors)

### `--edw-rpc <expr>` → stdout + exit_code

Evaluate `expr` on the running node.

**Arguments:**

- `expr`: Elixir source. Required.

**Behavior:**

| Condition | Output |
|-----------|--------|
| Success | `inspect(value)` and a newline on stdout and stderr, exit 0 |
| Node down | non-zero |
| Combined with `--edw-recover` | non-zero, no eval |

**Eval method:** Base64-encode `expr`. Pipe Erlang to `erl_call -c <cookie>`
with `-name <node>` (long) or `-sname <node>` (short). Pass `-r` and
`-no_result_term`. Do **not** pass `-s` (that starts a node). The host writes
`Kernel.inspect/1` of the value to stdout (a temp file is allowed; `io:format`
does not reach a pipe).

```erlang
Bin = base64:decode(<<"...">>),
{Val, _} = 'Elixir.Code':eval_string(Bin),
io:format("~ts~n", ['Elixir.Kernel':inspect(Val)]).
```

Do not `halt` the remote node.

**Examples:**

- `--edw-rpc '1+1'` → stdout `2`
- `--edw-rpc 'node()'` → the remote node name

**Edge cases:**

- Empty expression → error
- Quotes and newlines in `expr` → Base64 wrap, no shell interpolation of the
  remote source

---

## Testing

Cases live in [tests-edw-rpc.yaml](tests-edw-rpc.yaml). Elixir E2E under
`test/e2e/` MUST implement them. Hosts MUST NOT add XCTest / gtest as the
source of truth.

## Generated Documentation

Packaging CLI table and ini `[beam] node` / `cookie` keys. Porting checklist
row for `--edw-rpc`.

## Implementation Checklist

- [ ] macOS / Windows / Linux one-shot CLI
- [ ] Discovery order implemented
- [ ] Mutual exclusion with `--edw-recover`
- [ ] E2E cases from tests-edw-rpc.yaml
- [ ] Status row `done` only when E2E is green

## Version History

- **v0.1.0** - Initial specification

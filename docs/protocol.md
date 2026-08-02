# Protocol

Elixir Desktop Webview (EDW) wire protocol between the native host (server) and
the BEAM client.

## Transport

- TCP on `127.0.0.1` (default). Port from `--edw-port` / ini / ephemeral `0`.
- Framing: **4-byte big-endian unsigned length** + UTF-8 JSON body.
- Body: [JSON-RPC 2.0](https://www.jsonrpc.org/specification) objects.
- Both peers may send **requests** (with `id`) and **notifications** (no `id`).
- After accept, the client MUST call `initialize` before other production methods.

Environment for the BEAM child (packaged mode): `EDW_PORT`, `EDW_HOST` (default `127.0.0.1`).

Stdout discovery line (always, once listening):

```text
listening <port>
```

## Message shapes

Request:

```json
{"jsonrpc":"2.0","id":1,"method":"window.open","params":{"title":"App","width":800,"height":600}}
```

Success response:

```json
{"jsonrpc":"2.0","id":1,"result":{"window_id":"w1","webview_id":"v1"}}
```

Error response:

```json
{"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"Method not found"}}
```

Notification (no `id`):

```json
{"jsonrpc":"2.0","method":"event.window.close_requested","params":{"window_id":"w1"}}
```

### Error codes

| Code | Meaning |
|------|---------|
| -32700 | Parse error |
| -32600 | Invalid request |
| -32601 | Method not found |
| -32602 | Invalid params |
| -32603 | Internal error |
| -32000 | Host / application error |
| -32001 | Not initialized |
| -32002 | Unknown resource id |
| -32003 | Test RPC disabled |

## Lifecycle

1. Host listens and prints `listening <port>`.
2. Optionally spawns BEAM (unless `--edw-no-beam`).
3. Client connects and calls `initialize`.
4. Client drives windows/menus/… ; host emits `event.*` notifications and may send
   requests (e.g. `permission.request`) that the client must answer.
5. Default lifetime: host keeps listening after disconnect (`reconnect`).
   `--edw-lifetime=coupled` exits the host when the client disconnects (and kills
   BEAM when the host exits in packaged mode).

## Production methods

### `initialize`

Params: `{ "client": "desktop_webview", "version": "0.1.0" }` (informational).

Result:

```json
{
  "protocol_version": 1,
  "platform": "macos",
  "capabilities": {
    "window": true,
    "webview": true,
    "menu": true,
    "tray": true,
    "notification": true,
    "permission": true,
    "media": true,
    "test_rpc": false
  }
}
```

### Window

| Method | Params | Result |
|--------|--------|--------|
| `window.open` | `title`, `width`, `height`, `min_width?`, `min_height?`, `icon_id?` | `{window_id, webview_id}` |
| `window.close` | `window_id` | `true` |
| `window.show` | `window_id`, `show?` | `true` |
| `window.hide` | `window_id` | `true` |
| `window.set_title` | `window_id`, `title` | `true` |
| `window.set_min_size` | `window_id`, `width`, `height` | `true` |
| `window.set_icon` | `window_id`, `icon_id` | `true` |
| `window.set_menubar` | `window_id`, `menu_id` | `true` |
| `window.iconize` | `window_id`, `iconize` | `true` |
| `window.shown` | `window_id` | `boolean` |
| `window.active` | `window_id` | `boolean` |
| `window.raise` | `window_id` | `true` |
| `window.destroy` | `window_id` | `true` |
| `window.close_veto` | `window_id` | `true` |

Events: `event.window.close_requested`, `event.window.focus`, `event.window.blur`.

### Webview

| Method | Params | Result |
|--------|--------|--------|
| `webview.load_url` | `webview_id`, `url` | `true` |
| `webview.reload` | `webview_id` | `true` |
| `webview.current_url` | `webview_id` | `string \| null` |
| `webview.rebuild` | `window_id` | `{webview_id}` |
| `webview.set_context_menu` | `webview_id`, `enabled` | `true` |

Events: `event.webview.new_window` (`url`), `event.webview.error`, `event.webview.finished`.

### Menu / tray

| Method | Params | Result |
|--------|--------|--------|
| `menu.create` | `kind`: `"menubar"` \| `"popup"`, `dom` | `{menu_id}` |
| `menu.update` | `menu_id`, `dom` | `true` |
| `menu.destroy` | `menu_id` | `true` |
| `tray.create` | `icon_id?`, `menu_id?` | `{tray_id}` |
| `tray.set_icon` | `tray_id`, `icon_id` | `true` |
| `tray.set_menu` | `tray_id`, `menu_id` | `true` |
| `tray.destroy` | `tray_id` | `true` |
| `menu.set_apple` | `app_name`, `window_id?` | `true` |

`dom` is a JSON encoding of the `Desktop.Menu` tree:

```json
{"tag":"menubar","attrs":{},"children":[
  {"tag":"menu","attrs":{"label":"File"},"children":[
    {"tag":"item","attrs":{"onclick":"quit"},"children":["Quit"]},
    {"tag":"hr","attrs":{},"children":[]}
  ]}
]}
```

Events: `event.menu.click` (`menu_id`, `onclick`), `event.tray.click`.

### Notification / media / system

| Method | Params | Result |
|--------|--------|--------|
| `notification.show` | `id?`, `title`, `message`, `timeout?`, `type?` | `{notification_id}` |
| `notification.close` | `notification_id` | `true` |
| `icon.create` | `path` or `png_base64` | `{icon_id}` |
| `icon.destroy` | `icon_id` | `true` |
| `system.open_url` | `url` | `true` |
| `system.locale` | — | `string \| null` |
| `system.os_description` | — | `string` |
| `system.set_permission_policy` | `origin`, `camera`/`microphone`: `"allow"|"deny"|"ask"` | `true` |

Events: `event.notification.click`, `event.notification.dismiss`,
`event.system.open_url`, `event.system.open_file`, `event.system.reopen`.

### Permissions (hybrid)

Host → client **request**:

```json
{"jsonrpc":"2.0","id":42,"method":"permission.request","params":{
  "origin":"http://127.0.0.1:4000","type":"microphone","webview_id":"v1"
}}
```

Client response `result`: `{ "decision": "allow" | "deny" | "ask" }`.

- `allow` / `deny`: host answers WebKit without further UI (TCC may still apply).
- `ask`: host uses the OS / WebKit prompt.

Events: `event.permission.changed`.

## Test-only methods (`test.*`)

Enabled only when the host was started with **`--edw-test-rpc`**.
Release binaries used by apps must leave this off. If called while disabled → `-32003`.

| Method | Params | Result / effect |
|--------|--------|-----------------|
| `test.ping` | — | `"pong"` |
| `test.echo` | any | same params |
| `test.capabilities` | — | capability map |
| `test.window.list` | — | `[{window_id, webview_id, title, url}]` |
| `test.webview.eval` | `webview_id`, `script` | eval result (JSON-compatible) |
| `test.permission.simulate` | `origin`, `type` | triggers `permission.request` |
| `test.disconnect` | — | host closes the TCP connection |
| `test.crash` | — | host process exits non-zero (E2E only) |

Production code paths must not call `test.*`.

# Bridge contract of the trimmed Filament Manager page

Extracted from the source of this folder (Bambu Studio `v02.09.01.70`). Files are relative to `src/`.

## Transport

Page to host (`hooks/Bridge.tsx` `rawSend`, :67-96). The packet is serialized with `JSON.stringify` and sent as a string, first match wins:

1. Windows WebView2: `window.chrome.webview.postMessage(str)`.
2. macOS WKWebView: `window.webkit.messageHandlers.wx.postMessage(str)` (handler name `wx`).
3. Fallback: a hidden `iframe` whose `src` is `app://` + `encodeURIComponent(str)`, removed after 200 ms.

Host to page: the host dispatches a DOM `CustomEvent` named `cpp:device` on `document`; the packet is `event.detail`. (`hooks/Bridge.tsx:222`, `useFilamentManagerBridge.ts:326`.) Packets whose `head.version` is not `"1.0"` are ignored.

## Envelope

```ts
type Packet = { head: { version: '1.0'; type: 'request' | 'response' | 'report'; seq: number; ts: number /* ms */ }; body: any }
```

- Request: `body = { module, submod, action, payload }`. `seq` starts at 0 and increments per request (separate counters for the page's request hook and for the `device_host` health messages).
- Response: `head.type = 'response'`, same `head.seq` as the request, `body = { module, submod, action, error_code: number, message: string, payload }`. The page treats `error_code === 0` as success; any other value is a failure and `message` is shown. A response whose `seq` is unknown is dropped.
- Report: `head.type = 'report'` (any `seq`), `body = { module, submod, action, payload }`. Page handlers only act on `body.module === 'filament'`, and only while the route is `/filament_manager` and `document.visibilityState !== 'hidden'` (`useFilamentManagerBridge.ts:23-27, 152`).
- Timeout: 5000 ms per request (`hooks/Bridge.tsx:152`, no caller overrides it); on expiry the call resolves as `Request Timeout` and a later response is dropped.

## Requests the page sends (`module: "filament"`)

Types are in `features/filament-manager/types.ts`. `R` = response `payload`. Calls with a line in `useFilamentManagerBridge.ts` (call it HB) are made through its `makeBody(...)` helper.

| submod | action | request payload | R | source |
|---|---|---|---|---|
| init | init | `{}` | `InitData { theme: 'dark'\|'light'; spools: Spool[]; presets: PresetOptions; cloud_sync?: CloudSyncState; debug_enabled?: boolean }` | HB:393 |
| spool | list | `{}` | `Spool[]` | HB:414 |
| spool | add | `Partial<Spool>` | `Spool[]` (the full list after the change) | HB:461 |
| spool | batch_add | `{ spool: Partial<Spool>; quantity: number }` | `Spool[]` | HB:473 |
| spool | batch_create | `{ creates: Partial<Spool>[]; updates: Partial<Spool>[] }` (updates carry `spool_id`) | `Spool[]` | HB:501 |
| spool | update | `Partial<Spool>` (with `spool_id`) | `Spool[]` | HB:517 |
| spool | remove | `{ spool_id: string }` | `Spool[]` | HB:529 |
| spool | batch_remove | `{ spool_ids: string[] }` | `Spool[]` | HB:541 |
| spool | mark_empty | `{ spool_id: string }` | `Spool[]` | HB:553 |
| spool | toggle_favorite | `{ spool_id: string }` | `Spool[]` | HB:563 |
| spool | archive | `{ spool_id: string }` | `Spool[]` | HB:573 |
| preset | list | `{}` | `PresetOptions { vendors: PresetVendor[] }` | HB:584 |
| preset | create_custom | `{ vendor: string; client_request_id: string }` | not read (only `error_code`); the result arrives as report `preset/create_custom_done` | `features/filament-manager/AddEditDialog.tsx:714` |
| machine | list | `{}` | `{ machines: MachineItem[]; selected_dev_id?: string }` | HB:593 |
| machine | request_pushall | `{ dev_id?: string }` | not read (only `error_code`) | HB:622 |
| ams | list | `{ dev_id?: string; switch_selected?: true }` | `AmsData { selected_dev_id; ams_units: AmsUnit[]; ext_slots: AmsTray[]; is_rack?: boolean }` | HB:642 |
| colors | query_for_id | `{ fila_id: string }` | `FilamentColorCodesResponse { fila_id; fila_type; candidates: CandidateColor[] }` | HB:359 and `AddEditDialog.tsx:761` |
| sync | status | `{}` | `CloudSyncState { logged_in; is_syncing; is_pulling; last_synced_at: string; last_error: { code: number; message: string } }` | HB:661 |
| sync | pull | `{}` | `CloudSyncState` | HB:670 |
| sync | push_all_now | `{}` | `CloudSyncState` (the hook still exports it; the toolbar button that called it is removed in this copy (see `NOTICE.md`), so the page never sends it) | HB:685 |
| config | fetch | `{ force?: true }` | when `message === 'cached'`: `CloudFilamentConfig` (otherwise ignored; data comes by report `config/fetched`) | HB:703 |

Failed `spool` writes show an error toast. Spool writes that succeed replace the whole list from `R`.

## Reports the page handles (all `module: "filament"`, handler `useFilamentManagerBridge.ts:146-325`)

| submod | action | payload | handler |
|---|---|---|---|
| spool | any action | `Spool[]` (replaces the list) | :154-158 |
| preset | create_custom_done | `{ ok?: boolean; client_request_id?: string; created?: { vendor; type; name; filament_id } }` | :161-171 |
| preset | list | `PresetOptions` | :172-174 |
| debug | log | `Omit<DebugLogEntry, 'id'>` | :178-182 |
| sync | state | `CloudSyncState` | :186-187 |
| sync | pull_done | `{ spools?: Spool[]; state?: CloudSyncState; added?: number; updated?: number }` | :188-212 |
| sync | push_done | `{ op?: 'create'\|'update'\|'delete'; spool_id?: string; state?: CloudSyncState }` | :213-240 |
| sync | push_failed | `{ op?: string; message?: string; spool_id?: string }` | :241-257 |
| sync | auto_push_summary | `{ trigger?: 'auto'\|'manual'; device_state?: 'busy'\|'idle'\|'manual'; pushed?; skipped_cooldown?; skipped_no_diff?; skipped_no_rfid?; skipped_no_total_nw? }` (numbers) | :258-296 |
| config | fetched | `CloudFilamentConfig` (`filamentSettings[]` of `{ filamentVendor, filamentType, filamentName, filamentId, isSupport }`; vendors/types derived when absent) | :304-307 |
| machine | selected_changed | `{ selected_dev_id: string; machines?: { machines?: MachineItem[] }; ams?: AmsData }` | :313-324 |

## `device_host` messages the page sends on its own

`hooks/Bridge.tsx` `reportDeviceHealthReady` (:102-125): after the first successful (`error_code === 0`) response to a request whose `action === 'init'` or `submod/action === 'state/get'` (and `module !== 'device_host'`; here that is `init/init`), if the host has injected `window.__bambuWebViewHealth` with a `pageInstanceId`, the page sends two requests, once per page instance:

- `{ module: 'device_host', submod: 'health', action: 'boot', payload: { page_instance_id, document_url } }`
- the same with `action: 'ready'`.

`document_url` is `location.href` cut to 2048 characters. The page does not wait for or read a response to these. Without `__bambuWebViewHealth` nothing is sent. The page sends no `layout` message (see changes).

## Changed versus the research notes (v02.08.04.57)

- The notes list a report `init/theme_changed` `{theme}`. This page has no handler for it. After `init`, the only way the theme changes is the `theme` field of the `init` response; a host that wants a live theme switch needs a new message or a page reload with `?theme=`.
- No `device_host/layout` message exists in this page; only `device_host/health` boot/ready.
- Initial theme: no longer read from the user-agent string (see `NOTICE.md`).

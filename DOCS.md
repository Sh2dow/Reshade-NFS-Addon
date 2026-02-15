# NFSTweakBridge RT/DS Notes

This document captures current Render Target / Depth Stencil behavior and a design baseline for future per-RT/DS effect routing.

## 1) Current Runtime Context

- Renderer path: Vulkan (DXVK) via ReShade add-on.
- Pre-HUD manual rendering is used.
- Regular Vulkan ReShade pass is forced off in code (`set_effects_state(false)`), and manual techniques are rendered in pre-HUD path.

## 2) Log Fields (PreHUD)

`Rendered effects at pre-HUD (rc=... frame=... bp=... rtv=... dsv=... score=...)`

- `rc`: render counter (manual pre-HUD render count).
- `frame`: internal frame index.
- `bp`: begin-pass counter.
- `rtv`: selected render target resource handle.
- `dsv`: selected depth-stencil resource handle.
- `score`: pass score (600 means main-candidate pass currently selected).

`PreHUD skip (... req=... reqAge=... reqWin=... lock=... lockMatch=... bbOk=... early=... score=... rt=... ds=... back=...)`

- `req`: pre-HUD request active.
- `reqAge`: request age in frames.
- `reqWin`: request timing gate result.
- `lock`: lock signature active.
- `lockMatch`: current pass matches locked RT/DS signature.
- `bbOk`: backbuffer gating condition result.
- `early`: early-frame phase gate result.
- `score`: candidate score (often `600` for scene, `0` for non-scene).
- `rt`, `ds`, `back`: RT handle, DS handle, current backbuffer handle.

## 3) Latest Observed RT/DS (from `spam.log`)

From the latest log snapshot:

- Main manual render pair:
  - `RTV=175917272`
  - `DSV=176235904`
  - `score=600`
- Backbuffer handles observed while the same scene pair is active:
  - `177897472`
  - `177898704`
  - `177899936`
- Non-scene skips frequently show:
  - `rt=0`, `ds=176235904`, `score=0`

Interpretation:
- The chosen scene RT/DS is stable.
- Backbuffer handle can rotate while scene pair remains valid.
- Skip lines with `rt=0` are expected pass candidates that must be ignored.

## 4) Current Selection Behavior (Code-Level)

Current pre-HUD render path in `NFS_addon/dllmain.cpp`:

1. Bridge request sets `g_request_pre_hud_effects`.
2. `on_begin_render_pass` scores pass and validates timing/lock gates.
3. On valid pass, manual rendering executes (enabled techniques only).
4. Locked scene signature (`g_prehud_locked_rt_resource`, `g_prehud_locked_ds_resource`) is used to stabilize selection.

Key gate dimensions:

- Request age/window (`reqAge`, `reqWin`)
- Early frame phase
- Backbuffer requirement (`g_require_vulkan_backbuffer_rt`)
- Lock match (`lockMatch`)
- Cooldown / same-frame prevention

## 5) Goal: Per-RT/DS Effect Assignment

Target capability:

- Route different ReShade techniques to different RT/DS signatures.
- Example:
  - Scene pair A -> full preset
  - Pair B (post stack) -> subset
  - HUD-related pair(s) -> none

### Proposed Data Model

Use signature key:

- `signature = (rtv_handle, dsv_handle)`

Routing table:

- `signature -> list<technique_name>` (or `mode=all/none/allowlist/denylist`)

Suggested structure:

- `std::unordered_map<uint64_t, PassProfile>`
- Packed key: `((uint64_t)rtv << 32) ^ dsv` (or another stable packing strategy for 32-bit handles).

`PassProfile` example fields:

- `mode`: `all`, `none`, `allowlist`, `denylist`
- `techniques`: set of technique names
- `priority`: optional tie-break
- `enabled`: runtime toggle

## 6) Implementation Plan (Next)

1. Add signature profiler:
   - collect observed `(rtv,dsv)` with counters and score histogram.
2. Add profile config:
   - load/save JSON/INI mapping for signatures.
3. Replace “render all enabled techniques” with “render techniques for active profile”.
4. Add overlay UI:
   - show current signature
   - assign profile to current signature
   - quick actions: `All`, `None`, `Copy from Scene`.
5. Add fallback behavior:
   - unknown signature -> `none` (safe) or `all` (compat mode), configurable.

## 7) Practical Notes

- Handle IDs are process-session specific; do not hardcode across runs.
- Persist by signature behavior patterns, not raw IDs alone (or re-learn each launch).
- Keep lock first, then route: unstable pass selection will invalidate any routing layer.


//   Next can be implemented step 1 from that doc immediately: a runtime signature profiler + overlay list of discovered RT/DS pairs.

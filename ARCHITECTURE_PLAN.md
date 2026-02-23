# Deterministic Pre-HUD Strategy

## Goals
- Never render on wrong RT/DS.
- Avoid heuristic pass guessing loops.
- Keep HUD unaffected.
- Recover cleanly across rain/tunnel/overlay transitions.

## Rules
1. Bridge is timing/event producer only.
2. Add-on is render decision owner only.
3. No per-pass FE polling in add-on decision path.
4. No score-based lock migration after lock is established.
5. One render max per scene token.

## Event Contract (Bridge -> Add-on)
- `BEGIN_SCENE_WINDOW(token, epoch)`
- `END_SCENE_WINDOW(token, epoch)`
- `PHASE_INVALIDATE(reason, epoch++)`

`reason` includes:
- weather_on
- weather_off
- overlay_enter
- overlay_exit
- scene_transition

## Add-on State Machine
- `Disabled`
- `Stabilizing`
- `Armed`
- `Locked`

Transitions:
- `PHASE_INVALIDATE` -> `Stabilizing` (clear lock)
- settle complete -> `Armed`
- valid lock candidate -> `Locked`

## Lock Policy
- Lock candidate key: `(rt, ds, width, height, samples, format)`
- Lock acquisition only from:
  - exact backbuffer pass, or
  - repeated same candidate across consecutive scene windows in same epoch.
- No opportunistic migration while locked.
- Migration only via explicit invalidate or exact backbuffer confirmation.

## Render Policy
- Render only if:
  - current scene token open,
  - token not rendered yet,
  - pass matches locked candidate (or lock-acquire pass),
  - state is `Armed`/`Locked`.
- If not matched, skip frame (skip is acceptable, wrong pass is not).

## Logging Contract
Only log transitions/decisions:
- `STATE_CHANGE old->new`
- `LOCK_SET key`
- `LOCK_CLEAR reason`
- `RENDER_OK token key`
- `RENDER_SKIP token reason`

No per-pass spam.

## Refactor Status
- `NFS_addon` monolith split into:
  - `NFS_addon/src/addon_core.inl`
  - `NFS_addon/src/addon_exports.inl`
  - `NFS_addon/src/addon_runtime.inl`
  - `NFS_addon/src/addon_dllmain.inl`
- `NFS_addon/dllmain.cpp` is now a thin entry include file.

## Next Implementation Steps
1. Add explicit overlay enter/exit signal in bridge and remove FE polling from add-on runtime decisions.
2. Introduce `epoch` and enforce epoch match in token/render path.
3. Replace remaining fallback locks with state-machine lock acquisition only.
4. Reduce logs to transition-only diagnostics.

# ESM First Person adoption scaffold

Status: offline ABI boundary only, not a playable port. `EsmModule_GetApi`
negotiates fixed ABI v1; `create` clears its output and returns
`UNSUPPORTED_ADAPTER`. No instance, callback, hook, config reader, worker or
resource lease is created. Other lifecycle callbacks also reject use. Do not
package or install this DLL. There is intentionally no shipping manifest.

## Name, credits and provenance

The adapted feature's user-facing name is **ESM First Person**. Future UI,
package display names and adaptation documentation should use that name.
This local work adds the ESM ABI scaffold and its boundary tests; it does not
claim authorship of the original First Person camera or stamina HUD work.

The source reference is Shroudtopia's `example-mods/first_person_view`.
Its mod metadata credits **s0T7x**. The repository's [MIT license](../../../LICENSE)
retains **Copyright (c) 2024 Miguel Oppermann**. Preserve that license, original
author and contributor credits, required attribution and upstream provenance
in any future distribution. Existing upstream documentation is retained.

Branding does not migrate compatibility identifiers. Keep `FirstPersonView`,
`camera_distance`, `active`, `shroudtopia.json`, ABI exports and existing
technical artifact names unchanged unless a migration is explicitly authorized.
No protocol or save-format change is part of this adaptation.

## Ownership and compatibility

All changes are confined to this First Person directory. The legacy source now
consumes the extracted distance rules; its hook behavior and compatibility IDs
are preserved. Loader/runtime, Minimap and installer belong to other work.
No shared provider is implemented here.

Legacy source references in the parent directory:

- `camera_patch.h`: tested on client revision 1076226; verifies the named system
  and instruction signatures without requiring an exact PE timestamp or image
  size. Patches interior call 0x2663D3 to transform 0xCB9C10. Distance
  at LocalPlayerCamera +0xEF0 defaults to -0.15 m (valid -0.5 through 0.5),
  shoulder +0xED4 becomes zero.
- Minimap `src/dllmain.cpp`: entry hook at 0x2662A0 for the same named
  `player_camera_transform` system. Distinct patch sites do not prove safe
  independent ESM provider ownership, callback ordering or unload behavior.
- `stamina_patch.h`: parent stamina layout at 0xAA6244, native widget scope
  0x9437E0 and UI context 0x273F588. The anchor is 45 percent width / 50 percent
  height. Coordinate with shared HUD/render ownership before porting this hook.

On 2026-10-14 the live `shroudtopia.json` had FirstPersonView.active=false and
camera_distance=-0.15. It was read only. Installed reference DLL SHA-256:
`3d678da2892554d954913c5fb36a3c182a5a1c52f99594b41e8eeff0b549b7ae`.
The installed DLL's source/build provenance was not established from this hash.

## Required contracts before functional adaptation

1. Accepted client camera provider: validated game layout, local-player identity,
   serialized transform adjustment, ordering with Minimap observations, and
   exclusive ownership/conflict reporting for the underlying hook. Consume it
   rather than implementing a second camera provider.
2. Accepted native callback lifetime: stop admission, drain callbacks accessing
   module state, restore owned patches and release state only after quiescence.
   Fixed-v1 PENDING retention alone does not drain native camera/HUD callbacks.
3. Accepted settings binding: disabled-by-default activation, finite/ranged
   distance, effective-state reporting, and game-thread application. Preserve
   the existing disabled preference in any later installer migration.
4. Accepted optional HUD layout service and ordering: missing HUD support should
   retain native stamina placement, as the legacy mod does. No independent
   widget/render hook is added by this scaffold.

The loader's current `module_services_v1.h` is outside the accepted fixed-v1
checkpoint. A declaration is not proof of a working provider. The checkpoint
contains only module_v1.h and runtime_v1.h; CMake verifies both byte hashes.

## Shared-service assessment, 2026-10-15

The canonical [SDK adoption status](../../../../esm-loader/docs/sdk/adoption-status.md)
now records accepted bounded offline module/runtime integration (02C4), isolated
driver work (05E7D-R1), settings persistence (08E6-R1), and host-fixture
settings/lifecycle/persistence (08E4). These extend the earlier 5F fixtures;
they do not establish native camera identity, game-thread affinity or hook
draining. This scaffold still refuses creation.

The inspected public runtime headers in `E:\Worktrees\esm-p0-runtime-20261013`
expose dispatch, routing, settings and native UI services, but no shared camera
service. Host-only `begin_revoke`, `poll_callbacks` and `release_context` account
for scoped service callbacks; they do not prove that inline camera/HUD detours
have drained. Native UI page/window operations are not a stamina layout service.
Do not wire legacy detours into ESM lifecycle callbacks on this evidence.

Readiness recheck: SDK adoption-status SHA-256
`7b52a93ec19bda763d221d03947dfa3567270f3201715f594fc310c43423ceaf`.
Both pinned ABI header hashes and all 11 recorded First Person source hashes
matched the prior verified candidate before this documentation-only update.
The 102 prior checks remain historical local evidence; they were not rerun for
this readiness check. NIC-498/NIC-499 must supply an accepted camera provider
and native callback-draining contract before functional integration can proceed.

| Mode | Planned component placement | Current evidence and gate |
| --- | --- | --- |
| Singleplayer | Local client camera/HUD consumer | No ESM gameplay acceptance; require local-player binding, restoration and drain |
| Listen host | Host's local client consumer only | No acceptance; distinguish host camera from authority/world state |
| Listen guest | Guest's local client consumer | No acceptance; prove effects stay local and disconnect restores state |
| Self-hosted dedicated | Camera/HUD on each participating client; no renderer on server | No acceptance; validate server policy/negotiation requirements before claiming vanilla-server support |
| Provider-hosted dedicated | Same intended client placement | No acceptance; provider hosting does not establish compatibility |

Local camera placement suggests no gameplay server handler is needed, but this
is a source-based design inference, not verified client-only support. Activation
must consume accepted session/capability and applicable policy contracts without
inventing a separate SteamID list or granting permissions from a capability bit.
Camera/HUD state must never mutate shared world time, saves or other players.
Missing required camera/lifetime services keep the feature unavailable; missing
optional HUD service retains native stamina placement. Service version matching
and required paired components remain to be resolved with NIC-498/NIC-499.

Owner remains this First Person port in Shroudtopia. Minimap owns its consumer;
Loader/runtime owners supply shared contracts. No EMBER feature or Stack Size
implementation is added here. Next functional step: consume an accepted camera
provider and teardown contract, then verify distance changes, mode/session exit,
focus loss, resolution changes and Minimap coexistence against exact artifacts.

Progress is linked to the existing [NIC-530 integration record](https://linear.app/aerox912/issue/NIC-530/m20-integrate-camera-and-photo-tools)
and its [Notion work package](https://app.notion.com/p/3f86f2829cab81f49f46f290b19bdadb).
This reusable First Person checkpoint does not complete the separate free/orbit
camera or photography feature checklist. The shared
[branding guide](../../../../esm-loader/docs/branding/ESM-BRANDING.md) remains
canonical; there is no UI or logo package in this offline scaffold.

## Hook-free settings extraction, 2026-10-15

### Dependency ownership and next evidence

Loader response, 2026-10-15: the shared owner's
[consumer capability assessment](../../../../esm-loader/docs/sdk/consumer-capability-status-2026-10-15.md#first-person-request)
confirms the next step is a written camera-provider contract covering local
player/world identity, application-thread affinity, Minimap ordering, exclusive
hook ownership and conflicts, followed by per-profile native quiescence proof.
It establishes no assigned camera implementation package or accepted camera/HUD
API. The consumer requirements below are input to that shared contract, not its
replacement. No new consumer implementation is unblocked by this response.

NIC-498 and NIC-499 remain In Progress. Their current comments and SDK status
distinguish accepted offline slices from the following open shared work:

| Open item | Existing owner / First Person consequence |
| --- | --- |
| 05E7B budget backend, final composition and delayed ACK across settings-consumer replacement | Loader/runtime owner; do not duplicate or repair from this port |
| Store02-R1 exact-object policy publication/cleanup repair, metadata/policy backends and authenticated endpoint | Loader/runtime owner; no First Person policy implementation here |
| Native world/mode/player identity, window/thread affinity and inline-hook drain | NIC-498/NIC-499; direct activation blockers |
| Published camera service, single hook owner and ordering with Minimap | Shared owner contract still absent; First Person remains a consumer |
| Typed resource/composition provider (offline Task 9) | NIC-502 and EMBER owners; this is not a camera service or a new First Person dependency |
| Actual native profiles (Task 10) and accepted package identities | Separate native/packaging gates; offline success cannot claim gameplay support |

The ESM Loader chat was active during this check. Its latest progress reports
hold the budget component for a possible close-before-first-snapshot leak and
missing accounting checks. This is owner-reported work in progress, not a new
verified defect or an invitation to edit its files. The older issue-description
text saying no implementation/tests exist is superseded by later bounded
acceptance comments; neither issue is complete.

Safe Light work here is the consumer acceptance specification below, alongside
the completed hook-free settings extraction. These are required observations,
not invented function signatures or a fake provider implementation:

- Missing or incompatible camera/lifetime provider: refuse activation without
  acquiring a hook or changing camera/HUD state.
- Valid local-player generation: apply the validated distance only on the
  provider's documented thread; stale or replaced generations cannot write.
- Minimap coexistence: one underlying hook owner, explicit transform/observation
  order, and deterministic refusal of an incompatible owner.
- Retirement during an in-flight callback: close admission, retain callback code
  and state, and report pending until the real callback exits. Restore only owned
  state and release it after confirmed quiescence. Include reentrant retirement.
- Partial activation failure and provider replacement: clean up only this
  consumer, preserve siblings, and reject stale work without replaying it.
- Missing optional HUD provider: retain native stamina placement. With support,
  verify restoration on focus/resolution/session changes for each claimed mode.

Run these against exact accepted provider/header/artifact pins once published.
Do not implement a parallel lifecycle state machine or use a mocked drain as
native acceptance. No additional independent production-code slice was
identified by this dependency check; test scaffolding that assumes unpublished
API signatures would create avoidable rework.

`../camera_settings.h` contains the default, finite/range validation and reader
error handling. It depends only on `<cmath>` and is consumed by the legacy
adapter and the offline settings test. It does not read files, register settings,
retain callbacks or apply values to a camera. The ESM DLL remains inactive and
does not yet consume a native settings service.

The reader receives -0.15 as the missing-setting fallback. A finite value in
[-0.5, 0.5] replaces the previous distance; invalid values or reader exceptions
retain it. The caller must supply its last valid distance. The legacy adapter
still checks for absent context/provider before calling the helper and preserves
the `FirstPersonView` / `camera_distance` keys. Missing setting and unavailable
provider therefore remain distinct. No new parsing or configuration schema is
introduced.

Local Release x64 evidence: the new test first failed with 11 of 24 assertions
against the stub, then all 24 passed after implementation. The actual-DLL ESM
boundary test passed all 21 checks, and the existing synthetic camera/HUD suite
passed all 57 checks. Both the ESM scaffold and legacy DLL built successfully.
These are local/synthetic checks, not live game or multiplayer acceptance.

Logs and verified Anti-Churn receipts are under
`E:\Build\first-person-esm-settings-20261015`. The accepted red receipt records
the expected failing-test observation, not a passing product. The green receipt
records the changed candidate after a typed candidate invalidation.

[ChatGPT review](https://chatgpt.com/g/g-p-6ac81a29422881919957c85c01cc79ed-enshrouded/c/6ad01079-ca64-83ed-b52b-8b8b0a76d151)
through Surface Control (GPT-6, Pro) found no concrete correctness or legacy-parity
blockers in the supplied code. This was a source review, not independent test
execution. It confirmed that the previous valid distance is a caller invariant
and that the Windows-only CMake harness does not change the header's portability.
Review summary and candidate hashes are `review.md` and `candidate.json` in the
build evidence directory above.

Source reference: [s0t7x/shroudtopia](https://github.com/s0t7x/shroudtopia), local
base commit `195e6189e7e695a06f5cb4ca6830c0b08cc3ac8f`; this is the inspected
checkout identity, not proof that this commit exists on upstream. The new shared
header extracts rules from `camera_patch.h` and `dllmain.cpp`; no upstream
authorship or license notices are replaced.

## Build and focused verification

From a shell with CMake and Visual Studio 2022 C++ tools:

```powershell
cmake -S . -B E:/Build/first-person-esm-settings-20261015/native -G 'Visual Studio 17 2022' -A x64 -DESM_FIXED_SDK=E:/Build/esm-production-host-impl-20261014/sdk-fixed-v1
cmake --build E:/Build/first-person-esm-settings-20261015/native --config Release
ctest --test-dir E:/Build/first-person-esm-settings-20261015/native -C Release -V
```

Observed 2026-10-14: MSVC 19.44.35228, Windows SDK 10.0.26100.0, Release x64
build passed; one DLL-loading CTest passed with 21 checks. Coverage includes
export separation, invalid output/size/ABI requests, cleared failed-create
output, and refusal across absent/client/combined capabilities. Tests use the
public exported table, not source inclusion. Loading in a test process is not
loader package integration or in-game acceptance. Legacy tests were not rerun
at that initial scaffold checkpoint; the extraction checkpoint above adds fresh
legacy verification.

No game installation, live setting, server, save, installer or shared provider
was modified. No deployment, commit, push or publication was performed.

## Task5 portable ViewMode source reuse, 2026-10-17

[Approved Task5 plan](https://app.notion.com/p/3fc6f2829cab81b9a7d3ca8c62e2df0f). The bounded source-reuse slice is complete and independently accepted: `esm/view_mode.h` is byte-identical to the r3 helper (SHA-256 `31ca7dcb1dedbea12da2b05def3365a96007a78b53bc1fc0ef4df601f8c92fad`); `view_mode_test.cpp` carries the 12 r3 checks with only its include adjusted; CMake adds three target-registration lines. The r3 client `25988036` identifies provenance only.

Evidence is retained at `E:\Build\first-person-esm-view-mode-20261017`: `candidate-manifest.json`, `red-build.log`, Debug/Release CTest logs, `verification\retire-probe-run.log`, and `.agent-churn-control\receipts.ndjson`. The expected RED compile failed with C1083 on the missing header (exit 1). Debug and Release each passed all 3 CTest targets within the 60-second timeout (ViewMode 12 checks, settings 24, actual-DLL boundary 21); the focused Release retire-before-activation probe also passed. The independent Tester accepted the slice and recorded `E:\Build\first-person-esm-view-mode-20261017\verification\review.md` (SHA-256 `2d047f1f92ea8e2a5a748dc36ca59cd6407e636ef38a66be9b0df35d05e35540`) and `E:\Build\first-person-esm-view-mode-20261017\verification\source-closure.json` (SHA-256 `95931c72d8469b2b4e84190380fed798d7e75a3bd02a04a404c92b5af48c909c`). Source closure confirms the ten unchanged prior-candidate pins; the existing CMake prefix matches its pin with only the three intended target lines added. Unchanged suites were not replayed.

ViewMode consumes counted toggles under serialized lifecycle dispatch with synchronous, nonthrowing Apply. No input-edge owner, provider/service API, native integration or live behavior was added; `module.cpp` remains fail-closed (`UNSUPPORTED_ADAPTER`). Loader Tasks 2/3 and accepted provider artifacts remain gates for functional integration; camera and native callback-drain acceptance are absent. M20 free/orbit/photo scope remains separate.

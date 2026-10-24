# Generic server adapter contract r6

Task ID: generic-adapter-production  
Deployment: generic-adapter-r6-20261024  
Status: the C ABI and ownership contract are frozen for consumer adaptation; runtime wiring, CMake extraction and acceptance tests are still pending.

## Consumer entry and compatibility

The existing module loader remains the only adapter entry. A module exports
ShroudweaveServerModuleStartV1 and receives ShroudweaveServerHostV1. The
existing host prefix through audit is unchanged. The optional services pointer
is appended after that prefix. Older modules may read the prefix fields and
ignore the tail; consumers must use minimum-size checks rather than exact
struct-size equality. A consumer that needs services checks the host ABI major,
requires SHROUDWEAVE_SERVER_HOST_V1_SERVICES_SIZE, then checks the services
table ABI major and its required prefix. The optional observer is exposed only
when the table size includes that tail field and its pointer is non-null.

The service table is per module. Its opaque context binds calls to the module
that received the table, so registration, settings reads and action pumping
cannot claim another module's ownership. The module may cache the table and
context for the process lifetime. Descriptor pointers and strings are borrowed
only for a registration call; the host copies them before returning. Callback
pointers and contexts remain module-owned, and the host keeps an accepted
module resident for process life. The generic service header has no
Windows-only types and uses explicit-length UTF-8/JSON buffers and C calling
conventions for C or Rust consumers. Callbacks must not reenter the services
table while validation or migration is in progress.

ABI status values are 0 success, 1 unavailable, 2 invalid or capacity, and 3
failure. IDs are lowercase ASCII [a-z][a-z0-9_-]{0,63}. JSON object inputs are
UTF-8 and at most 49,152 bytes. Descriptors, IDs and defaults are copied during
registration. Callback pointers and callback contexts remain owned by the
module; an accepted module remains resident for process life. No exception may
cross the C ABI.

## Startup and registration

Startup order is:

1. Prepare the generic host runtime and authority/transport.
2. Load each manifest-declared module through the existing module loader and
   call its Start entry on the startup thread.
3. During that module's Start call on the startup thread, registration functions
   may register its settings pages and action providers. Each successful
   settings-page registration synchronously validates and initializes that
   store, including any declared legacy transform, before returning. The
   module may read its initialized page during Start. Registration calls
   outside the active Start call, from another thread, after freeze, or through
   a different module's service context fail. Action pumping remains
   unavailable until routing is ready.
4. After every required module starts successfully, freeze registrations and
   start transport routing.

A module that returns failure from Start contributes no registered or
published pages or providers; the host rolls back its in-memory registrations
before freeze. Settings already persisted during a successful registration
remain in storage, including a canonical result created by migration. A failure
anywhere before freeze prevents routing from starting. The host copies
descriptors and strings, but retains callback pointers and their module-owned
context. Duplicate IDs/routes, invalid descriptors, invalid defaults, failed
validators, and late registration fail closed. Startup errors must not make a
declared required module optional.

## Per-module authorization policy

Each settings page and action provider includes an explicit
ShroudweaveServerAuthorizationPolicyV1. Registration rejects an absent,
malformed or unknown policy; the host does not select a default policy for a
module. The module must choose one of:

- EXACT_GROUP: require the current peer's verified group to equal the copied
  selected_group string.
- EQUIVALENT_PERMISSIONS: require a verified group and all bits in
  selected_group_permissions.
- OPEN_CONNECTED: allow the current connected peer without group selection.

The policy supplies a nonzero revision used by the existing authority
revalidation contract. The host binds its current configuration epoch into
the private authority policy during registration; modules cannot select or
impersonate that epoch. Exact-group mode requires a
nonempty selected_group and zero permission mask. Equivalent-permissions mode
requires a nonzero permission mask and no selected group. Open-connected mode
requires neither selector. The SDK assigns no group names or permission
meanings; a mod chooses them in its own registration code. In particular, the
SDK does not choose Admin or Friend.

## Settings pages and migration

ShroudweaveServerSettingsPageV1 binds one global route ID to module-owned
defaults, schema version, validator and explicit authorization policy. The host
validates defaults and all existing, migrated and edited values through that
callback. A registered module can read only its own pages through
read_settings_page. A successful read returns only the JSON values object and
its persisted revision; the caller provides a sufficiently large buffer, and a
capacity error reports the required length without returning partial JSON.

Page storage remains in the existing shared settings store, keyed by the
registered page ID. Page IDs are globally unique across the companion. This
preserves the shared settings store contract while the mod owns page names and
schemas.

Legacy migration is declared by both legacy_page_id and migrate. If the
canonical page key exists, it has priority: a valid current schema is returned;
corrupt, unsupported, wrong-schema or validator-rejected canonical state fails
closed without reading the legacy key. Only a true not-found result permits
legacy lookup. If no legacy key exists, the module defaults initialize the
canonical page at revision 1. If a legacy key exists, the host passes its stored
schema version, revision and values JSON to the module's bounded transform
callback. The callback returns transformed values and an explicit positive
output revision. It may preserve the source revision for an identity transform
or advance it for a schema change; the SDK does not assign revision semantics.
The callback must reject revision overflow, and the host rejects zero or
otherwise invalid revisions. The host validates the result against the current schema and
atomically creates the canonical key. Any malformed source, transform failure,
invalid output or persistence failure stops page initialization; it never
silently falls back to defaults. The old key is never overwritten or deleted.
A migration never replays queued actions.

Concurrent initialization uses the store's compare-and-exchange behavior. If a
canonical value appears during migration, the host rereads and validates that
canonical value; it does not replace it with legacy data. A valid canonical
value wins.

## Action providers and native calls

ShroudweaveServerActionProviderV1 gives one module-owned route, an explicit
authorization policy, a module-defined capability mask, a pure action
validator/capability mapper and a synchronous effect callback. The SDK defines
no capability names or bit meanings. The authorize callback parses and
validates the action name and values and returns the required bit mask. The
host rejects bits outside the registered provider's mask, checks the current
available mask, and calls the provider validator again immediately before
execution.

The generic action wire envelope uses the provider route ID as page:
- read: v, request, page, op=read
- act: v, request, page, op=act, session, sequence, action, values
- result: v, request, page, op=result, session, sequence

The request ID and all numeric/session fields are validated by the host. The
host owns connection authorization, session binding, monotonic sequence checks,
duplicate receipts, bounded pending work, expiry, replay suppression and result
receipts. The module owns action names, payload validation, capability mapping
and the effect. A pending action executes at most once, synchronously, and only
when its provider is pumped with the required current capability bits. A module
must return success only after the effect is applied; after a partial failure,
the host records failure and never replays that action. The native callback
receives current peer Steam ID, connection ID, world ID, action and values as
borrowed data.

The service return from pump_actions reports service/provider availability.
Per-action completion is reported through the action route's receipt protocol.
The host may execute at most one action per pump call. Capability masks are
module-defined bitsets; the generic SDK does not bind them to gameplay features.

## Peer and creation observation

is_peer_current returns success only for the exact current Steam ID, connection
and world association; stale or absent associations return unavailable. It
does not grant a capability or effect authorization.

observe_creation_context is an optional observer hook for a module that already
owns a verified native call context. It accepts opaque manager and caller
addresses. Status 0 means a qualified observation was recorded, not that the
caller has a capability or authorization. Status 1 may still mean an event was
observed. The hook supplies no action capability, authentication decision,
lifetime proof or thread proof. The calling module must obey its own
native-context contract. The current implementation and profile composition
must be verified before claiming availability; no code in this ABI authorizes
new hooks or wider native effects.

## Extraction and owner map

Only these API/header and map files changed in this pass:

- include/server_services_v1.h: generic settings, migration, policy, action,
  peer and optional observer types and service table.
- include/server_module_v1.h: appended optional services pointer; old prefix
  through audit remains byte-layout compatible.
- include/shared_services_v1.h: retains the existing client transport exports
  and removes feature-specific server exports from the shared public header.
- docs/porting/generic-adapter-r6-contract.md: this contract and extraction map.

The mod-owner transfer receipt records 71 byte-identical files from the frozen
source artifact, with tree digest
4084a9dfaa943be3330a8d397a9bd672284bab04a11dff3eb4cab339aae7cc05. The source
artifact is
E:\Build\shroudtopia-native-providers-20261024-r5\ARTIFACT-HANDOFF.json,
SHA-256
9bbe84dea69300fcbfdeb1b743ccf3d2ef9f3b6ea71b72b0f2e76df299108f54. It is
immutable extraction reference only, not evidence of a generic ABI binding or
runtime acceptance. Preserve its credits, licenses, notices and source hashes.

| Current seam | Ownership needed for a generic SDK |
| --- | --- |
| m182/; src/m182_schema.cpp; include/m182_actions.hpp; src/m182_actions.cpp | Mod-owned activation, defaults/schema, action names and policies. The received source transfer covers the feature implementation; generic settings and action mechanics should be extracted separately. |
| entry/shared_runtime.cpp; include/settings_router.hpp | Split the reusable connected transport, generic settings router and action broker from feature configuration, defaults, page routing and migration. |
| entry/server_modules.cpp; src/inventory.cpp; entry/shroudweave_host.cpp; CMakeLists.txt | Remove reserved feature branches and require every declared required module to load successfully. Preserve the failure result when a required module is absent or fails. |
| whitelist/; host bootstrap; inventory special cases; whitelist CMake targets | Whitelist policy/runtime belongs to its mod package. The owner receipt at E:\Repos\enshrouded-server-whitelist\integrations\shroudweave-sdk\TRANSFER-RECEIPT.md records a byte-matched source transfer with tree digest a5814cab8332a6f54a48a77ac3bdbc6a7a5848ce76b4bb0e7e3e7fdd24db3dfd. Remove only receipt-matched feature copies/build links/rules; keep generic required-module failure behavior. |
| src/connected_authority.cpp and its policy types | Keep generic connected-peer/session revalidation. Move Admin/Friend group selection and feature permission policy behind module-owned registration. |
| native/context_capture*, native/context_hooks*, native/context_activation* | Treat exact native layouts, signatures, call sites, lifetime assumptions and feature providers as profile-owned until each source has an accepted extraction/composition receipt. The observer function table entry alone does not make these hooks generic. |
| entry/dedicated_entry_gate.cpp, entry/dedicated_entry_thunk.asm, entry/dedicated_dbghelp_proxy.cpp/.def, entry/shroudweave_host.cpp | Existing proxy and host boundary. This pass does not edit the gate, bridge, call-site validation, installation, or hook bytes. |

## Target and hook boundary

The current CMake production target is shroudweave_server_companion, with
OUTPUT_NAME dbghelp. It contains the existing ESM dedicated entry/proxy gate,
Shroudweave host, inventory, Whitelist runtime and shared server runtime. The
separate shroudweave_services_client target builds transport/client.cpp. The
shroudweave_gate_fixture target is a test fixture. This pass changes no CMake
target, DLL, client pin, server install or hook.

Generic adapter entry remains ShroudweaveServerModuleStartV1, loaded for a
manifest module named shroudweave_<id>.dll after path and hash verification.
The current Whitelist dbghelp.dll special case and internal M182 activation
branch are feature-specific loader behavior to remove only with exact owner
receipts. SDKCreateModInstance, ShroudweaveServerModuleStartV1 and
ESMEsmModule_GetApi are distinct module ABIs and are not interchangeable.

Known hook/owner boundaries in this source tree:
- The dedicated proxy gate and native bridge use the existing ESM entry contract;
  entry/shroudweave_host.cpp verifies EsmDedicatedGate_GetApiV1 and call site
  offset 0x9cf205.
- whitelist/runtime.cpp and whitelist/policy.cpp implement the current
  mandatory Whitelist host policy; CMake describes native signatures pinned to
  26083076. This is not generic SDK policy.
- m182/activation.cpp and its supervisor own a separate seven-site commit
  profile; it remains feature-source in the transfer receipt.
- Native diagnostic context capture is behind
  SHROUDWEAVE_CONTEXT_CAPTURE_DIAGNOSTIC, which defaults off. Its current
  status says observation-only; it does not establish authentication or action
  capability.

Do not treat this companion's dbghelp.dll as a drop-in replacement for the
separate K2 observer research dbghelp74ed7324... or native Vein hooks. Existing
client pin strings Shroudtopia73a85e3b..., winmmb44f42e0... and
SDK30aa6921... are incomplete contextual values, not full hashes or
composition proof. A conflicting hook needs an explicit composed delta and
fresh evidence. The exact full composition is an open dependency.

## Validation boundary

No runtime source, CMake, fixture, test, client, proxy, live game, server,
install, or frozen r5 artifact was changed or exercised for this header/map
handoff. The next production step is to wire this ABI through the generic
module loader and shared runtime, keep feature schemas/actions in module
packages, then run the Tester-owned API and routing checks. No generic runtime
acceptance is claimed by the presence of these declarations.

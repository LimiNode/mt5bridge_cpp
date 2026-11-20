# Typed managed OPEN owner factory: bounded implementation plan

## Status and scope

Draft follow-up to merged PR #84: an implementation plan plus the first
Python-free numeric/policy validation slice. The owner factory, runtime source
authority, request serialization and durable binding are not implemented.
This does not grant dispatch permission or establish live-terminal readiness;
no existing production path or durable format changes.

The next implementation should replace caller-authored OPEN JSON at the private
manager boundary with a typed intent and a factory-owned, immutable request.
It must reuse the existing owner, journal, admission barrier and one-shot
backend. No second journal, scheduler, request framework or observation store
is needed. Public asynchronous TradeManager, production CLOSE/INOUT and B3E
ownership activation remain separate.

`SYMBOL_TRADE_EXECUTION_MARKET` is a temporary restriction of the first slice,
not a permanent MT5Bridge execution-mode restriction. Do not broaden PR #85:
complete its native validation/tests and independent review first. The approved
follow-up order is collector, execution-mode expansion, serialization/binding,
then manager integration; each remains independently reviewed.

## Existing seams and gaps

- `src/dispatch/private_trade_manager.hpp` composes one account, TradeId and
  legacy OPEN slice. It does not establish symbol, side or quantity authority.
- `Mt5AccountInfo` and `Mt5SymbolCapabilities` expose observations with
  `known_fields`; their public POD shape does not authenticate caller values.
- `ManagedOpeningQuantityDomain`, `derive_managed_opening_quantity()` and
  `matches_managed_opening_quantity()` in
  `src/dispatch/managed_opening_quantity_contract.hpp` already validate unique
  binary64-to-canonical-step quantities. They are quantity validators, not
  source-authority producers.
- `map_managed_logical_units_to_broker_volume()` implements the distinct
  [ADR-0033](adr/0033-exact-managed-broker-volume-mapping.md) scale-zero,
  whole-lot contract. It must not be repurposed to mean one logical unit per
  broker step without a reviewed quantity-domain change.
- The Python shape gate currently runs after the durable dispatch barrier
  ([ADR-0014](adr/0014-private-python-dispatch-transport.md)). The factory must
  reject locally invalid typed inputs before preparation/admission, while
  retaining that final defensive gate and the no-blind-resend rule.

## Authority and immutable binding

The caller supplies a desired AccountKey, symbol, explicit BUY/SELL side and
positive bounded logical quantity, not raw JSON or a claimed capability proof.
The owner factory must join those inputs to runtime-owned observations in the
existing trade-critical lane. Reading public PODs from the caller cannot grant
this authority.

The runtime collection seam must bracket symbol/account observations with live
account checks, require exact server/login agreement with the account lease,
and bind the returned canonical symbol name to the requested UTF-8 identity.
Missing required fields, truncated/aliased symbol names, an account switch or
a changed lease generation fail closed. A snapshot proves only the collected
state; normal fresh environment admission and the backend's final account and
fencing checks remain mandatory.

The factory output should retain one immutable tuple: operation identity,
AccountKey, canonical symbol, side, logical quantity and quantity-domain
version, canonical broker quantity/step, execution/filling policy, source
capability identity, and canonical request bytes. Only the factory constructs
the admitted private value; a public constructor, `valid()` method or matching
digest alone is not source authority. The descriptor's requested volume and
the logical intent must agree. `position` and `position_by` are forbidden;
the action is DEAL and the order type is only BUY or SELL.

Request serialization consumes that tuple once, with no mutable JSON overlay.
Changing any tuple member requires a new pre-dispatch validation, not editing
an already prepared request. A checksum may detect changes, but it cannot
replace the tuple or its authenticated collection path.

## Exact quantity and capability invariants

Reuse the B2 canonical quantity contract for observation conversion and
round-trip validation. Do not introduce epsilon rounding, `round(volume / step)`,
or a second binary64/decimal conversion algorithm in the factory.

The runtime-owned symbol normalization must establish a reviewed canonical
scale (at most nine), positive step numerator and explicit logical mapping.
Selection of that domain from symbol metadata is an implementation gate, not
an existing authority claim. Equivalent decimal encodings must canonicalize
identically; ambiguous or unsupported encodings must be rejected.

The first wired factory retains the legacy whole-lot logical domain and uses
`map_managed_logical_units_to_broker_volume()` with the admitted symbol
normalization. Thus one logical unit remains one lot even when the broker step
is 0.01 lots. A B2 domain with one unit per step may validate broker quantities,
but its step count must not become the legacy owner's logical volume. A future
non-legacy mapping must separately prove divisibility by logical-units-per-step
and checked numerator arithmetic before it can be wired.

Validate min/max and any applicable directional volume limit in the same exact
broker domain, using fresh positions/orders for an aggregate limit. Unknown
required limits or incomplete coverage cannot mean unlimited capacity. Never
clamp or silently round the requested quantity.

Serialize a deterministic decimal broker volume and validate the value decoded
by the actual Python request path against the same canonical quantity, using
the existing B2 helpers. Values whose round trip loses or ambiguously identifies
a broker step fail before admission. Include overflow and binary64 precision
boundary regressions.

Account trade permissions, symbol trade mode, market-order support, execution
mode and filling masks must be present and compatible with the requested side.
Do not infer hedge permission from margin mode, session-open state from trade
mode, or FOK/IOC/RETURN support from a different enum. Market OPEN must reject
pending/BOC policies; RETURN must not be selected for market execution. Unknown
combinations fail closed rather than receiving a default filling mode. Price
and deviation requirements must be explicit for the supported execution mode;
unsupported modes remain rejected until their policy is tested.

The current managed owner settles whole-lot quantities. A fractional quantity
may be exactly representable by B2 yet unsupported by this owner's settlement
and restart semantics. The first wired implementation must retain that
whole-lot boundary and reject other mappings. Fractional managed execution
requires a separate reviewed change covering descriptor, settlement and replay;
it cannot be enabled by the request serializer alone.

On a netting account, an opposite existing position can turn a BUY/SELL DEAL
into CLOSE/INOUT even without a position field. Until that ownership path is
supported, fresh complete symbol-position evidence must reject that case and
any ambiguous/external exposure. Do not infer pure opening from request shape
or claim the bridge lease excludes manual/other-terminal trading. Unexpected
exit/reversal evidence stays fail-closed in reconciliation.

## Pre-barrier and restart behavior

1. Validate the typed intent and collect account/symbol authority in the
   existing runtime lane. Resolve exact quantity and execution policy without
   sending or changing managed exposure. An `order_check` is advisory only.
2. Within the existing account capability, rescan blockers and durably prepare
   the immutable request/binding. A validation failure creates no prepared
   record, permit or transport call. Hold no journal file lock during Python
   collection/transport; do not add another locking protocol.
3. Immediately before the normal barrier, revalidate the retained binding
   against fresh account/symbol policy and the current lease. On a changed
   quantity domain or incompatible capability, block; do not rewrite the
   prepared operation or automatically generate another attempt.
4. After `dispatching`, all existing at-most-once and observation-only recovery
   rules apply, including local defensive shape failures. Neither a factory
   value nor a fresh `order_check` may manufacture a replacement permit.

The durable binding format is a prerequisite to factory wiring. Existing
opaque request bytes do not independently encode a verified typed contract;
the journal must retain enough versioned information to replay and compare the
complete tuple. Specify the encoding, immutable CAS comparisons, size limits,
unknown-version rejection and legacy decoding before implementing it. Reuse
the operation journal, not a parallel store. Legacy records remain legacy;
restart must not upgrade caller-authored JSON into factory authority.

Prepared restart must recover the same request and binding, then revalidate
fresh capabilities before its first dispatch. Post-barrier restart remains
reconcile-only and uses the retained operation identity and result-derived
bindings; it never regenerates request bytes or resends an old operation.

## Reviewable implementation slices

1. **Quantity and policy validation, Python-free:** a private typed intent and
   pure numeric/policy helper using the existing exact quantity primitives.
   Supplied metadata remains explicitly unauthenticated; no manager wiring,
   request serialization or broker transport is enabled at this stage.
2. **Runtime-owned capability collector:** reliably collect account/symbol
   identity, permissions, reported execution mode, filling capabilities and
   volume limits with required-field completeness, account/lease continuity
   and freshness checks. Missing, changed or stale values fail closed. Capture
   the reported execution mode without interpreting Market as the only valid
   MT5 mode; collection is not request-policy selection or dispatch admission.
3. **Execution-mode expansion:** after the collector, implement and review
   INSTANT, then REQUEST, then EXCHANGE policy slices, including correct
   mode-specific price, deviation and filling rules and RETURN where allowed.
   Each newly supported mode needs native positive/negative policy tests and
   authenticated quote inputs where required; other modes remain rejected.
   Do this before serializer/binding and manager integration, not by enabling
   another branch in an already wired trading pipeline.
4. **Canonical request serialization and immutable journal binding:** encode
   all supported mode-specific policies into the same deterministic request
   and versioned operation-journal tuple. Specify immutable CAS comparisons,
   bounded/unknown-version decoding and legacy/prepared/post-barrier replay.
   Test actual Python decoding and tampering with account, symbol, quantity,
   execution mode, price, deviation and filling; no legacy authority upgrade.
5. **Private manager integration:** consume only the factory-admitted value
   on the typed path, revalidate before the existing barrier and exercise the
   existing dispatch/reconciliation pipeline with fake-Python end-to-end and
   file-store restart tests. Keep the old raw seam internal; it must not be an
   alternate entry point into typed authority.

Each slice needs independent review before the next expands its trust boundary.
This Draft implements only slice 1 as described below; the source producer,
canonical request serializer and versioned binding do not yet exist.

## Extensible execution-policy model (design only)

Keep these responsibilities separate; none requires another journal, broker
backend, class hierarchy or general policy framework:

- **Collected facts:** runtime-owned account/symbol capabilities retain the
  reported execution mode and filling flags, exact quantity domain, limits,
  source identity and freshness/lease scope. An unknown numeric mode must
  never become Market by default. A capability snapshot alone grants neither
  ownership nor permission to send.
- **Mode-specific policy:** the future private typed policy is a tagged value
  with distinct Market, Instant, Request and Exchange alternatives, not a
  Market flag plus unrelated optional request fields. Bind its tag to the
  collector's reported mode, not a caller attempt to change the symbol's
  execution mode. Keep filling explicitly requested and validated; reject
  irrelevant/missing fields rather than silently ignoring or inventing them.
  Share account/symbol and exact volume checks across alternatives.
- **Price/deviation contract:** Instant and Request must establish the correct
  side-specific price from authenticated, sufficiently fresh quote evidence,
  its supported price/tick domain, and explicit bounded deviation in points.
  Explicit zero deviation is not a missing policy. For Request, a last-tick
  snapshot is not evidence of a dealer-accepted quote; requote/price-change
  outcomes do not authorize resending an operation past the durable barrier.
  Specify Exchange's price/deviation requirements independently for the
  supported DEAL policy; do not borrow either Market's omission rules or
  Instant/Request assumptions. Quote collection uses the same runtime-owned
  source/freshness boundary, never a caller-authored price proof. No silent price
  rounding, stale-quote fallback or silent execution-mode substitution.
- **Filling compatibility:** evaluate execution mode and requested filling
  using the MT5 per-mode compatibility rules, not a universal Market mask
  test. In particular, ORDER_FILLING enum values are not SYMBOL_FILLING bits,
  RETURN has no corresponding symbol bit, and RETURN remains forbidden for
  Market execution. Instant/Request do not inherit Market's symbol-flag gate;
  test their FOK/IOC/RETURN rules separately against the upstream table.
  Exchange FOK/IOC require their symbol flags; RETURN also needs reviewed
  residual-order/partial-fill lifecycle handling, not just enum admission.
  BOC and pending orders remain outside typed
  managed market-order OPEN even after execution-mode expansion.
- **Immutable hand-off:** after policy expansion, the serializer and durable
  binding retain the execution tag and all meaningful mode-specific fields
  together with source identity, account/symbol, side, exact quantity and
  canonical bytes. Unknown/inconsistent tags fail closed; a capability change
  blocks before the barrier, never rewrites or downgrades a prepared policy.
  The manager consumes this one binding through the existing admission and
  one-shot path; it does not implement a separate pipeline per execution mode.

The current `ManagedOpenFilling` and Market-only `supported_policy()` are
slice-local private validation details, not an exhaustive public policy model
or a durable encoding. Their extension belongs to slice 3, before any typed
request is serialized, journaled or wired. PR #85 adds no new execution tag,
price/deviation fields, RETURN handling or mode implementation.

Mode expansion must use the upstream
[execution/filling compatibility rules](https://www.mql5.com/en/docs/constants/tradingconstants/orderproperties#enum_order_type_filling)
and [trade-request field contract](https://www.mql5.com/en/docs/constants/structures/mqltraderequest)
as implementation references and pin the chosen interpretation in tests.
Across every slice retain fail-closed admission, exact volume mapping,
pre-barrier revalidation and post-barrier no-blind-resend semantics. Production
CLOSE/INOUT and ownership activation still require separate authorization.

## Implemented first validation slice

`src/dispatch/typed_managed_open_validation.{hpp,cpp}` and the private native
`mt5bridge_typed_managed_open_validation` target have no manager, journal,
transport or Python dependency. The result is a freely copyable/editable
`ManagedOpenCandidate`, intentionally not a sealed proof. No production caller
consumes it. Passing these checks cannot prepare an operation, establish pure
opening semantics, change exposure or manufacture a dispatch permit.

- Intent contains only AccountKey, bounded UTF-8 symbol, BUY/SELL, positive
  integer whole lots and explicit FOK/IOC. No pending order type, position,
  position_by, mutable JSON or fractional logical quantity is representable.
- Supplied account server/login, symbol account scope and exact symbol name
  must match. Required known-field bits and both account trading permissions
  are checked. This is consistency checking, not authenticated collection.
- In this first slice only `SYMBOL_TRADE_EXECUTION_MARKET` is supported. This is
  temporary, not the final bridge policy. FOK/IOC use their distinct
  `SYMBOL_FILLING_*` bits, not `ORDER_FILLING_*` enum values. Missing/unknown
  policy, disabled/close-only or incompatible directional trade mode, absent
  market-order support, RETURN/BOC and all other execution modes are rejected.
  No price/deviation or session-open policy is inferred.
- An explicit supplied scale/step is validated against the raw symbol step by
  B2. Redundant trailing decimal zeros are removed for a canonical domain.
  The legacy whole-lot mapper determines requested broker units; B2 checks
  step-aligned min/max, quantity uniqueness and binary64 round trip. B2's
  temporary one-unit-per-step map never becomes the owner's logical volume.
  Non-grid bounds/limits are conservatively rejected as unsupported metadata;
  this does not claim support for every broker's sizing policy.
- A known finite zero `volume_limit` means no directional numeric cap. A
  positive limit requires a supplied complete same-direction positions/orders
  aggregate (including explicit zero), exact step conversion and checked
  remaining capacity. The supplied aggregate is an assertion, not evidence
  of complete coverage, ownership or absence of opposite netting exposure.

Native Release tests cover this pure boundary. They do not replace the planned
runtime freshness/lease tests, authenticated source collection, complete netting
guard, immutable file-store restart, actual Python request decoding or no-resend
integration tests. Those remain prerequisites for wiring, not claims of slice 1.

Reproduce the isolated native Release check without finding/linking Python:

```powershell
cmake -S . -B build_typed_open_native -G "Visual Studio 17 2022" -A x64 -DMT5BRIDGE_BUILD_RUNTIME=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=ON "-DCMAKE_CXX_FLAGS=/W4 /WX /EHsc"
cmake --build build_typed_open_native --config Release --target mt5bridge_typed_managed_open_validation_test --parallel 4
ctest --test-dir build_typed_open_native -C Release -R "^mt5bridge_typed_managed_open_validation$" --output-on-failure
```

## Acceptance matrix

| Case | Required result |
| --- | --- |
| Valid BUY and SELL under supported capabilities | Exact canonical request; no implicit dispatch permission |
| Missing/foreign account, changed lease or mismatched symbol | Rejected before preparation/barrier; zero sends |
| Forged caller capability POD or edited admitted payload | No typed authority; no durable mutation |
| Zero/negative/non-finite, non-step or out-of-range volume | No clamp/rounding, no prepare and zero sends |
| Overflow, ambiguous binary64 step, changed quantity domain | Fail closed using the shared exact quantity contract |
| Missing permissions, unknown execution/filling or incompatible side | Explicit unsupported/rejected outcome; zero sends |
| Pending/position-targeted request or opposite netting exposure | No OPEN admission; production CLOSE/INOUT remains disabled |
| Capability changes between prepare and execute | Block before barrier; retain immutable intent, no auto-replacement |
| Changed durable binding field or unsupported version | CAS/load/recovery fails closed; no permit |
| Prepared restart with matching binding and fresh policy | Only the original operation may undergo normal admission |
| Post-barrier restart, timeout or malformed result | Reconcile-only; durable call count never increases |

Verification must include Windows Release full CTest, focused `/W4 /WX`, exact
head Push/PR CI, real file-store restart and fake-Python decoding. Live/demo
orders, new public ABI and B3E ownership mutations are not acceptance tools.

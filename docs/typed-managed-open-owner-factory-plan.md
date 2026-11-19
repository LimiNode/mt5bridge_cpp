# Typed managed OPEN owner factory: bounded implementation plan

## Status and scope

Draft follow-up to merged PR #84. This document is a plan, not an implemented
factory, a new dispatch permission, or evidence of live-terminal readiness.
It changes no production code or durable format.

The next implementation should replace caller-authored OPEN JSON at the private
manager boundary with a typed intent and a factory-owned, immutable request.
It must reuse the existing owner, journal, admission barrier and one-shot
backend. No second journal, scheduler, request framework or observation store
is needed. Public asynchronous TradeManager, production CLOSE/INOUT and B3E
ownership activation remain separate.

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

Given an admitted mapping, the requested logical quantity must be divisible
by logical-units-per-step. Compute the integer step count and broker numerator
with checked arithmetic. Validate min/max and any applicable directional
volume limit in the same exact domain, using fresh positions/orders for an
aggregate limit. Unknown required limits or incomplete coverage cannot mean
unlimited capacity. Never clamp or silently round the requested quantity.

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

1. **Quantity and policy admission, Python-free:** a private typed intent and
   pure validation/serialization helper using the existing exact quantity
   primitives. Test source metadata remains explicitly unauthenticated; no
   manager wiring or broker transport is enabled at this stage.
2. **Runtime source and durable binding:** add the narrow authenticated
   account/symbol collector and versioned journal binding/CAS rules. Test
   changed symbol/account/quantity fields and legacy/unknown-version restart
   rejection. No live broker is used to establish these contracts.
3. **Private manager integration:** consume only the factory-admitted value
   on the typed path, revalidate before the existing barrier and exercise the
   real Python decoder plus fake terminal. Keep the old raw seam internal;
   it must not be an alternate entry point into typed authority.

Each slice needs independent review before the next expands its trust boundary.
This Draft proposes the sequence; it does not claim those producers or the
versioned binding already exist.

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

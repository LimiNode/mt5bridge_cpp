# ADR-0023: Finite formal model for managed trade lifecycle

## Status

Accepted as a verification aid for the managed lifecycle design. It is not a
public `TradeManager` API and has no terminal side effect.

## Decision

Keep a small finite model in
`formal/managed_trade/managed_trade_lifecycle.tla`. The model sits above the
already verified one-shot dispatch and observation/reconciliation models. It
deliberately models logical volume and lifecycle decisions rather than MT5
tickets:

- a running execution plan may create bounded open slices, while stopping the
  plan prevents new slices without erasing exposure or an active remainder;
- a partial open records filled volume and a separate pending remainder, which
  can later be observed or explicitly cancelled;
- a `CloseObligation` is durable desired state, not another operation ID;
- closing is satisfied only after reconciled close evidence reduces exposure to
  zero; a partial close leaves the obligation active;
- an unknown broker outcome becomes `ambiguous`, and a new decision requires a
  fresh authoritative snapshot before the ambiguous attempt can be retired;
  and
- each logical operation has at most one broker send.

The model does not represent broker ticket identity, Python result payloads,
lease fencing, or the C++ journal; those contracts remain covered by the
dispatch and reconciliation models and native tests. `TradeManager`, timed
schedules, risk policies, and execution-planner heuristics are later slices.

TLC 1.8.0 is run by CI with the same pinned artifact and Java 17 as the other
formal models. A model counterexample should become a focused C++ lifecycle
test before any public asynchronous API is added.

## Consequences

- The distinction between desired close state and individual side effects is
  executable and reviewable.
- Partial fills and pending remainders cannot silently overshoot the target or
  be treated as a completed close.
- Ambiguous outcomes are explicitly non-resendable until a fresh observation
  supplies a new decision point.
- The model remains finite and independent from broker-specific transport
  details, so it can evolve without changing the C ABI.

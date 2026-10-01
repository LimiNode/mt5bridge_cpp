# ADR-0024: Private managed-trade logical state

## Status

Accepted as the first implementation slice above durable dispatch and
reconciliation. It is private C++ infrastructure, not a public `TradeManager`
API and not a broker transport.

## Decision

Keep one durable logical state per `TradeId` in `src/trade/managed_trade.*`.
The state deliberately contains no MT5 tickets or Python objects. It owns:

- bounded target/max-slice execution-plan state;
- confirmed open volume and an independently tracked pending entry remainder;
- a durable `CloseObligation` that represents desired zero exposure rather than
  another operation id;
- one current operation slice with at-most-one-send evidence;
- observation epochs needed to retire an ambiguous attempt only after a fresh
  authoritative snapshot.

The state machine enforces the following edges:

```text
open slice -> submitting -> broker evidence -> reconciliation
partial open -> pending remainder or explicit cancel
close request before first fill -> obligation remains durable
unknown outcome -> ambiguous -> fresh snapshot -> acknowledge
```

An unsatisfied zero-exposure close obligation cannot become idle. An ambiguous
operation is never resendable directly; recovery must request a fresh snapshot.
Stopping a plan prevents new open slices but preserves exposure, remainders,
and close management.

The implementation uses logical integer volume units. Broker volume scaling,
ticket identity, result payloads, leases, dispatch permits, and observation
graph attribution remain in their existing layers. A future owner loop will
adapt this state to those contracts and persist it through the journal.

## Consequences

- Close-before-fill and late-fill races are represented without inventing a
  broker ticket before one exists.
- Partial fills cannot silently discard a remainder or overshoot the target.
- Recovery decisions are explicit and testable before any side-effecting
  execution planner is introduced.
- The public SDK and C ABI remain unchanged; no public `TradeManager` exists
  yet.

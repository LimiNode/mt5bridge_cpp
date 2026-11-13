# ADR-0039: Polling-first trade event observation

## Status

Proposed as the next product contour after managed-trade stabilization. This
decision defines an observation stream for account changes; it does not enable
automatic copy-trading, change the public C ABI, or replace the managed owner.

## Context

MetaTrader 5 exposes orders, deals, and positions as different entities. A
pending order may be modified or cancelled, one request may produce multiple
partial deals, and a netting position may be changed by another EA, a manual
trade, or the server. Python `MetaTrader5` exposes synchronous snapshots but
does not expose the MQL5 `OnTradeTransaction()` callback.

MQL5 transaction notifications are useful hints when an optional adapter is
available, but their order is not a durable event log and the terminal queue is
bounded. The bridge therefore needs a polling/reconciliation source that can
recover from restart, delayed history, reconnect, and notification loss.

## Decision

The first Trade Event Observation implementation is polling-first. It reads
active orders, positions, history orders, and history deals through the
existing typed observation APIs, then emits a host-driven native event stream
after reconciliation. A future MQL5 adapter may add low-latency hints to the
same reconciliation engine; hints never bypass a snapshot/history proof.

The stream is an observation of what the terminal history makes observable. It
does not promise broker-level exactly-once delivery or a globally ordered
transaction log.

## Event model

The model keeps the three MT5 identities separate:

- `Order` events describe creation, modification, cancellation, expiration,
  and lifecycle changes of an order;
- `Deal` events describe one observed execution, including partial execution
  and repeated deals for one order; and
- `Position` events describe an observed exposure change keyed by account,
  symbol, direction, and broker position identity.

An event carries an account-scoped identity, observation provenance, a source
cursor or revision, and a confidence/state classification. The initial
baseline is a snapshot, not a stream of newly created orders. A consumer may
receive:

```text
confirmed change | unresolved evidence | GAP | RESYNC_REQUIRED | STATUS
```

An unchanged snapshot produces no change event. Missing history, conflicting
identities, external mutation, and incomplete synchronization remain explicit
diagnostics rather than inferred absence.

The initial semantic vocabulary should cover `ORDER_PLACED`,
`ORDER_MODIFIED`, `ORDER_CANCELLED`, `ORDER_EXPIRED`, `DEAL_EXECUTED`,
`POSITION_CHANGED`, `SL_TP_CHANGED`, and `HISTORY_CORRECTED`, plus the
`GAP`/`RESYNC_REQUIRED`/`STATUS` control states. These names are a design
contract for the future versioned DTO, not an ABI addition in this PR.

## Polling and reconciliation

Each account stream maintains durable cursors for active and historical
domains. Reads use bounded overlap and inclusive boundaries; reconciliation
deduplicates by full order/deal/position identity and preserves multiplicity
where timestamps collide. History revisions and graph instance changes reset
or widen the affected overlap and emit `RESYNC_REQUIRED` when prior evidence
cannot be trusted.

A poll cycle may advance a cursor only after the corresponding result is
complete enough for the declared domain. Empty, partial, stale, and
unavailable results are not equivalent to “no trade”. A timeout after a
side-effecting command is observed through the same reconciliation path and
never triggers a blind resend.

## Optional transaction adapter

An MQL5 `OnTradeTransaction()` adapter is optional and out of the first
polling implementation. It copies notifications quickly into a bounded hint
buffer and reports overflow or reorder as `GAP`/`RESYNC_REQUIRED`. The adapter
does not own durable cursors, does not mutate managed exposure, and cannot
authorize an allocation or an external copy command by itself.

## Delivery and ABI boundary

Delivery is host-driven and runs outside the CPython GIL, runtime mutex, and
internal source locks. Queue capacity and consumer lag are bounded; overflow
is explicit. Callback memory is borrowed for the callback duration unless a
future versioned owned-buffer contract says otherwise. A separate trade-event
namespace/version is required; the existing tick subscription ABI is not
silently repurposed.

The bridge library observes and reports. It does not automatically copy trades.
Source/destination mapping, scaling, risk limits, and command scheduling belong
to an application that consumes the stream and uses separately reviewed safe
command APIs.

## Reliability invariants

- Account identity is immutable for one stream and checked on every source
  read.
- Order, deal, position ticket, and `POSITION_IDENTIFIER` are never conflated
  with a managed `TradeId`.
- Replay after restart is idempotent for the consumer cursor and event key.
- A lost notification, queue overflow, history rewrite, or account mismatch
  produces a visible GAP/RESYNC state.
- The initial baseline is not replayed as new activity.
- External/manual activity is never silently attributed to a managed owner.
- Delivery guarantees are stated explicitly; exactly-once broker events are
  not promised.

## Scope and dependencies

This ADR depends on the typed read APIs, observation graph, reconciliation
contracts, runtime callback rules, and the completion of the current managed
trade contour. It does not depend on production C3 `INOUT` authority and must
not be wired into `ManagedTradeOwner` internal state.

The implementation is split into bounded PRs:

1. event identity/status DTOs and a pure snapshot-diff contract;
2. durable polling cursors and overlap/restart recovery;
3. host-driven trade-event delivery with bounded queues and GAP semantics;
4. optional MQL5 hint adapter and live overflow/reorder acceptance; and
5. application-level copy-trading examples and command completeness checks.

Each slice must preserve the public ABI versioning rules and keep unsupported
cases fail-closed.

## Verification scenarios

Before the observation gate closes, a real demo account must cover:

- another EA placing and modifying a BUY LIMIT or STOP;
- partial execution followed by completion, cancellation, or expiration;
- SL/TP and position changes, including external/manual activity;
- several orders and deals in the same millisecond;
- terminal disconnect/reconnect and restart; and
- observer recovery without duplicate confirmed execution events.

Fake-runtime tests remain necessary for deterministic cursor and diff logic,
but they do not replace cold-cache and live-terminal acceptance runs.

## Research basis

This decision is based on the repository's `mt5-quirks.md`, the existing
observation/reconciliation contracts, and the MetaQuotes references for
[`OnTradeTransaction`](https://www.mql5.com/en/docs/event_handlers/ontradetransaction),
[`order_send`](https://www.mql5.com/en/docs/python_metatrader5/mt5ordersend_py),
and [Python Integration](https://www.mql5.com/en/docs/python_metatrader5),
reviewed on 2026-10-25. Terminal and Python-package behavior remains version
dependent; a release gate requires current live validation rather than relying
on this research snapshot.

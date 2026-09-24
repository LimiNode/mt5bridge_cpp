# ADR-0018: Execution-lane fairness

## Status

Accepted for the private runtime admission layer.

## Decision

Serialized calls into the embedded MetaTrader Python interpreter use two
private admission lanes:

- `trade_critical` for account, symbol-capability, order-check, and trade
  observation calls;
- `market_data` for tick/rate reads, realtime polling, and other bulk data
  work.

The lane admits one call at a time. A waiting trade-critical call is admitted
ahead of queued market-data work after the current call finishes. A bounded
trade burst then yields one slot to a waiting market-data call, so neither lane
can be starved by an unbounded queue.

The scheduler is deliberately non-preemptive: a market-data call already
inside a Python/MT5 operation is allowed to finish. Individual pagination and
bootstrap probes therefore remain the unit of admission and must stay bounded.
Realtime catch-up follows the same rule for every physical
`copy_ticks_from()` page: the page is copied into native POD storage, then the
GIL, interpreter mutex, and market-data lane are released before C++ performs
boundary and multiplicity processing or requests the next page. A single
`now_msc` snapshot still bounds both forward and overlap passes in one poll
epoch.
The lane is private implementation state and does not change the C ABI or the
public C++ client surface.

## Consequences

Trade-critical reads no longer sit behind an arbitrarily long queue of pending
market-data calls. Market-data progress remains observable under sustained
trade traffic because the scheduler enforces a bounded trade burst. This is a
scheduling guarantee, not a claim that MT5 itself is preemptible or that an
in-flight Python call can be cancelled safely.

The next runtime hardening slice should keep long bulk operations split into
bounded admission units and should use the same lane for the future internal
dispatch adapter immediately around its terminal call.

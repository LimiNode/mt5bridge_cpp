# ADR-0006: Managed trade lifecycle and execution boundaries

## Status

Accepted. This ADR defines the domain boundaries for the managed trading
runtime; implementation is staged after typed observation snapshots.

## Context

The old trading helpers mixed strategy intent, slicing, risk checks, retries,
timed expiry, and terminal side effects in one manager. Repeating
`order_send` or `order_close` after a timeout can duplicate an action whose
result is merely not yet observable. A high-level API must preserve the useful
intent (manage exposure until the obligation is satisfied) while making every
side effect and uncertainty explicit.

The Python MetaTrader5 backend is synchronous and calls are serialized by the
runtime interpreter mutex. An API may therefore be asynchronous to its C++
caller without promising concurrent terminal-native sends. True concurrent
execution requires a future MQL5 or process-isolated backend.

## Decision

### Identity hierarchy

Managed execution uses three distinct logical identities:

```text
TradeGroupId / ExecutionPlanId
    ├── TradeId
    │   ├── OperationId OPEN
    │   ├── OperationId CLOSE ...
    │   └── CloseObligation
    └── TradeId ...
```

- `TradeGroupId` identifies one strategy signal or desired exposure plan.
- `TradeId` identifies one independently managed logical allocation. On a
  hedging account it may map to a position; on a netting account it is a
  virtual allocation in an attribution ledger.
- `OperationId` identifies one concrete side effect attempt. It is never
  reused for a retry after reconciliation.
- `CloseObligation` is the durable desired end state (for example, exposure
  zero), not another send attempt.

MT5 `ORDER_TICKET`, `DEAL_TICKET`, `POSITION_TICKET`,
`POSITION_IDENTIFIER`, and `request_id` remain separate observed evidence.
None is interchangeable with a managed ID.

### Close and schedule semantics

Closing “until satisfied” means reconciling after every attempt and sending
only the remaining exposure. A timeout or connection failure is ambiguous and
must not trigger a blind resend. `POSITION_CLOSED` may satisfy an obligation
after reconciliation; `DONE_PARTIAL` creates a new `OperationId` only for the
remaining volume. An invalid close volume is reconciled against the current
position before recalculating the remainder.

Timed trades use a persisted `CloseSchedule`:

```text
none
after(duration)  -> anchored to the first confirmed fill by default
at(absolute_utc)
```

When a process restarts, an expired schedule creates or resumes its
`CloseObligation`; timing is not reconstructed from process uptime or request
submission time.

### Exit and execution boundaries

`ExitPolicy` may combine a virtual strategy exit with a wider broker-side
disaster stop. Virtual exits are driven by the reconciled realtime tick stream,
not by a single `symbol_info_tick()` hint. The runtime must surface the loss of
the virtual-exit process as a risk condition; a virtual stop is not a broker
guarantee.

Sliced entry/exit is an `ExecutionPlanner` concern, separate from the manager
for one logical trade. A planner may use fixed volume, maximum volume, risk,
margin, spread, slippage, interval, or signal-validity policies. It must
respect `SYMBOL_VOLUME_MIN`, `SYMBOL_VOLUME_MAX`, `SYMBOL_VOLUME_STEP`, and
`SYMBOL_VOLUME_LIMIT`. Stopping a plan prevents new slices while preserving
management of already created `TradeId` values.

### Risk boundary

Sizing proposes a volume; `RiskGuard` caps or rejects it; `order_check` projects
the broker response; only then can an execution plan be formed. Future risk
policies include fixed risk, fractional Kelly, margin, drawdown, position,
spread, and portfolio/currency-exposure guards. `OrderCalcMargin` is an input
to a projection, not a complete account-margin truth because it does not model
already open positions and pending orders.

### Stage-2 dispatch guardrail

Durable dispatch semantics and runtime admission form one atomic logical
boundary. The implementation must explicitly decide when an operation becomes
`may_have_been_sent`/non-resendable relative to admission and the
`g_python_mutex` queue. It must not wrap `order_send` in
`RuntimeCallAdmission` by analogy with advisory `order_check` without first
defining that ordering. A shutdown that begins after a side-effecting call is
admitted may let that call finish; the journal and recovery rules must make
that outcome safe.

## Required staging

1. Typed snapshots for active orders, positions, history orders, and history
   deals are implemented as the read-only Stage 1 boundary, with all
   graph-relevant fields and fail-closed conversion rules.
2. An observation-only graph and reconciliation tests; no send side effect.
3. Durable journal, account/lease fencing, and one internal at-most-once send.
4. Public asynchronous `TradeManager` and bounded host-thread events.
5. Timed close obligations, execution planner, hybrid exits, and risk guards.

Trading examples are dry-run/advisory by default. A future live example must
require an explicit `--live` flag and an additional real-account acknowledgement
before enabling side effects.

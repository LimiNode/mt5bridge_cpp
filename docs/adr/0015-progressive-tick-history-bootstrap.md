# ADR-0015: Progressive tick-history bootstrap

## Status

Accepted. This slice hardens deep tick reads before the controlled trade
smoke. It does not change the C ABI, synthesize bars, or call any trading API.

## Context

`CopyTicks` can start synchronization of the terminal's local tick database
from the requested `from` timestamp. During that work, Python may return only a
currently available suffix, an empty result, or a non-empty result carrying a
transient history/IPC status. Repeating one large `from -> now` request does
not necessarily move the earliest locally observable frontier backward. A
deep query can therefore fail even when narrower, progressively older probes
would eventually make the requested history available.

The existing lossless paginator remains the authority for exact result
delivery. A bootstrap must only initiate synchronization and detect a stalled
frontier; it must never publish probe rows or treat one non-empty probe as
proof of complete history.

## Decision

For tick ranges spanning at least thirty days, `visit_ticks_range()` performs
one-row synchronization probes before its normal pagination:

```text
frontier = min(request.to, current_time)
step = 366 days

while frontier > request.from:
    candidate = max(request.from, frontier - step)
    copy_ticks_from(candidate, count=1)
    if oldest returned tick moved before frontier:
        frontier = oldest returned tick
        reset stalled counter
        expand step toward 366 days
    else:
        halve step down to 30 days
        increment stalled counter
```

The bootstrap is bounded by sixteen probes. Three consecutive stalled probes
at the minimum step return a retry-exhausted failure with an explicit
`history bootstrap made no progress` diagnostic. A successful probe at the
requested anchor ends the bootstrap only; the normal inclusive paginator still
filters the requested millisecond range, reconciles same-timestamp payload
multiplicity, and confirms completion.

Transient/partial probe results are never used to advance the frontier. The
existing retry and IPC reconnect rules remain in force through the shared page
reader. Probe rows are discarded, so the public result contains no duplicates
introduced by the bootstrap.

This mechanism is intentionally limited to ticks. Bar history has a separate
MT5 cache and `Max bars in chart` contract; the bridge does not replace native
M1 bars with midpoint or tick-derived OHLC data.

## Consequences

- A cold multi-year tick request can progressively warm the terminal history
  instead of replaying one oversized anchor forever.
- No-progress behavior is bounded and fail-closed; a stalled terminal cannot
  spin indefinitely.
- The public ABI remains unchanged, including the fixed-size
  `Mt5FetchDiagnostics` record.
- Existing pagination, boundary-multiplicity, and callback semantics remain
  authoritative for delivered ticks.
- Native rates remain independent of tick bootstrap and are never silently
  synthesized.

## Verification

`tests/test_fake_mt5_runtime.py` verifies that deep probes move through
successively older years before normal pagination, that every probe requests
one row, and that a repeated oldest tick exhausts a bounded adaptive budget
with a retry-exhausted diagnostic. Existing pagination, transient recovery,
realtime, and dispatch tests remain part of the full runtime suite.

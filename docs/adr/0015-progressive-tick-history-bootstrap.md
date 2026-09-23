# ADR-0015: Progressive tick-history bootstrap

## Status

Accepted. This slice hardens deep tick reads before the controlled trade
smoke. It preserves the existing ABI-8 records, adds no side-effecting
trading API, and does not synthesize bars.

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
search_cursor = min(request.to, current_time)
confirmed_frontier = empty
step = 366 days

while probes remain bounded:
    candidate = max(request.from, search_cursor - step)
    copy_ticks_from(candidate, count=1)
    discard rows with time_msc < candidate
    if a non-empty response moves before search_cursor:
        search_cursor = oldest returned tick
        confirmed_frontier = oldest returned tick
        reset stalled counter
        expand step toward 366 days
    if the response is clean empty:
        advance only search_cursor; do not establish confirmed_frontier
    else:
        halve step down to 30 days
        increment stalled counter
```

Reaching the requested anchor is not inferred from a single suffix or from a
clean empty response. Empty pages may occur while terminal history is still
warming, so they only advance the bounded search cursor. A successful
bootstrap requires positive tick evidence and a target probe at
`request.from` that observes the same oldest tick again. Repeatedly returning
the old confirmed frontier without that target progress therefore remains
stalled and cannot declare bootstrap success. Weekend/holiday gaps are valid
once this clean confirmation is present.
`CopyTicksFrom` can return rows older than its requested anchor; a probe whose
rows are all pre-anchor is treated exactly like a clean empty response and
cannot move `confirmed_frontier`.
To avoid treating a distant stale suffix as a valid gap, the first available
tick must be no more than the adaptive 30-day minimum step after the requested
anchor; larger gaps remain retry-exhausted unless a probe reaches the anchor.

The bootstrap is bounded by sixteen probes and a five-second wall-clock budget.
Backoff starts at 100 ms and grows to one second; all sleeps occur after the
single probe's runtime admission and GIL scopes have been released. Three
consecutive stalled probes at the smallest step return a retry-exhausted
failure with an explicit `history bootstrap made no progress` diagnostic. Probe
warm-up diagnostics are set only after transient/partial, empty, stalled, or
frontier-moving synchronization evidence is observed. A successful probe at
the requested anchor ends the bootstrap only; the normal inclusive paginator
still filters the requested millisecond range, reconciles same-timestamp
payload multiplicity, and confirms completion.

Transient/partial probe results are never used to advance either frontier. The
existing retry and IPC reconnect rules remain in force through the shared page
reader. Probe rows are discarded, so the public result contains no duplicates
introduced by the bootstrap. If all bounded probes are empty, the operation
returns retry-exhausted/coverage-unproven rather than claiming a deep range is
empty or complete.

This mechanism is intentionally limited to ticks. Bar history has a separate
MT5 cache and `Max bars in chart` contract; the bridge does not replace native
M1 bars with midpoint or tick-derived OHLC data.

## Consequences

- A cold multi-year tick request can progressively warm the terminal history
  instead of replaying one oversized anchor forever.
- No-progress behavior is bounded and fail-closed; a stalled terminal cannot
  spin indefinitely.
- The existing ABI-8 records remain unchanged, including the fixed-size
  `Mt5FetchDiagnostics` record; rates expose immutable V1 coverage through an
  additive V1 accessor.
- Existing pagination, boundary-multiplicity, and callback semantics remain
  authoritative for delivered ticks.
- Native rates remain independent of tick bootstrap and are never silently
  synthesized.

## Verification

`tests/test_fake_mt5_runtime.py` verifies that deep probes move through
successively older years before normal pagination, that every probe requests
one row, that an unreached target suffix cannot terminate bootstrap, that a
valid calendar gap receives a clean confirmation, that empty probes cannot
complete a deep request before positive evidence arrives, and that an all-empty
deep request remains retry-exhausted. A malformed result after a transient
probe remains a fatal error. A repeated oldest tick exhausts a bounded adaptive
budget with a retry-exhausted diagnostic. Existing
pagination, transient recovery, realtime, and dispatch tests remain part of
the full runtime suite. A pre-anchor probe regression confirms that stale rows
cannot terminate bootstrap as positive evidence.

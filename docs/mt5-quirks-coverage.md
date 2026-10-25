# MT5 compatibility and reliability coverage matrix

This matrix turns the searchable registry in [`mt5-quirks.md`](mt5-quirks.md)
into release evidence. “Code” means a deterministic implementation rule exists;
“test” means a repeatable automated regression exists; “live” requires a real
terminal run. A row is not release-complete while either required evidence
column is unknown.

| Area / registry tags | Source evidence | Current implementation | Regression evidence | Live validation | Residual risk / status |
| --- | --- | --- | --- | --- | --- |
| Tick history warm-up, timeout, overlap (`history-warmup`, `history-timeout`, `tick-timestamp-ties`, `same-ms-order-instability`) | Official CopyTicks behavior and existing registry references | Bounded retries, inclusive cursor, boundary multiplicity, diagnostics, and progressive bootstrap | Reconciliation graph and market-data tests cover rotated/equal timestamp pages and non-progress | Cold terminal history and reconnect run required | History may be rewritten by MT5; best-effort synchronized-history guarantee only / partial |
| Rates completeness (`rates-calendar-boundaries`) | CopyRates range and broker calendar semantics | Strict fixed-intraday coverage; D1/W1/MN1 remain unproven | Rate coverage and partial-result tests | Cold history across session/calendar boundaries required | Interior completeness and broker calendars are not universally provable / partial |
| IPC and runtime reconnect (`ipc-transport`, `access-point-switch`, `cpython-restart`) | Python `last_error`, reconnect and CPython lifecycle references | Lifecycle admission, bounded retry and diagnostics exist | Runtime and fake-runtime tests | Reconnect, shutdown/startup and repeated packaged-runtime run required | Terminal/plugin state can outlive one Python call / partial |
| Side-effect timeout and dispatch recovery (`side-effect-timeout`, `trade-reconciliation`, `trade-reconciliation-lifetime`) | `order_send` and reconciliation references | Durable dispatch barrier, journal recovery, fencing, and no blind retry | Journal, worker, and owner restart tests | Accepted-after-timeout and delayed-visibility demo run required | Broker-side ambiguity remains unresolved until evidence arrives / code-backed, live pending |
| Order/deal history ordering (`trade-history-ordering`, `trade-partial-fill`) | History arrays are not append-only and one request may create many deals | Ticket-keyed reconciliation and per-deal settlement contracts | Reconciliation and managed settlement tests | Multiple partial-fill and cancel/remainder run required | History delay and corrections require overlap / partial |
| Position identity and account mode (`position-identity`, `trade-account-mode`) | Ticket versus `POSITION_IDENTIFIER`, netting/hedging references | Separate identities and fail-closed ambiguous managed attribution | Reversal, C2 candidate, and C3 contract tests | Netting and hedging demo runs with external activity required | Mixed netting ownership remains unsupported / partial |
| Filling and close-by semantics (`trade-execution-mode`, `trade-filling-mode`, `trade-closeby-capability`) | Symbol execution/filling capability references | Read-side typed capability validation; side-effect policy remains bounded | Existing order-check and dispatch tests | Broker symbols covering market/request/exchange and close-by required | Capability combinations vary by broker / planned |
| Account switching (`trade-account-switch`) | Account identity and terminal account properties | Immutable `AccountKey`, lease and mismatch diagnostics | Journal/lease and runtime tests | Switch/reconnect acceptance run required | Terminal account change during observation remains a live risk / partial |
| Native transaction hints (`trade-event-overflow`, `trade-request-id-scope`) | `OnTradeTransaction` ordering and 1024-event queue limit | Registry records hints only; Python polling remains source of truth | Future Trade Event Observation tests | Optional EA adapter overflow/reorder run required | Python API has no native transaction callback / planned |
| Pending order lifecycle | Orders, history orders, deals, and expiration semantics | Read APIs exist; unified event stream is not implemented | Existing snapshots do not prove event semantics | BUY LIMIT/STOP modify, partial fill, cancel, expiry demo required | Pending-copy behavior is not yet a supported product contract / planned |
| Callback delivery and consumer overflow | Existing host-driven tick subscription contract | Callbacks outside GIL/runtime mutex, bounded ring, explicit GAP | Realtime subscription tests | Long-running consumer/backpressure run required | Trade events need a separate namespace and cursor contract / planned |

## Acceptance rule

For each row, the release report links the exact source citation, code path,
regression, and live run. A missing or rewritten history response is not
interpreted as “no trade”; it remains partial, unresolved, or a GAP. Unknown
terminal behavior is diagnosed or rejected rather than guessed.

## Research maintenance

When a new symptom is reproduced, add it to `mt5-quirks.md` with terminal
build, Python package version, reproduction, bounded rule, and source link;
then update this matrix. The matrix is reviewed before a release gate closes,
not only after a production incident.

The matrix reflects the repository and upstream references reviewed on
2026-10-25. When a terminal build, Python package, or broker changes behavior,
record the reproduction and version before changing a status from partial or
planned.

# ADR-0045: Restart reconciliation of unresolved managed OPEN

## Status

Proposed as the bounded follow-up to the private owner-loop MVP.

## Context

The durable dispatch barrier and `PrivateOwnerLoop` already classify an
operation that survived a process crash as non-resendable. The existing
settled-state replay reconstructs confirmed exposure, but an OPEN whose broker
result was persisted before the history deal became visible still needs a
managed state at the observation-only edge. Recreating that state ad hoc in a
caller would risk either losing the pending operation or manufacturing
exposure.

## Decision

`ManagedTradeOwner::recover_pending_open()` replays the ordered durable OPEN
frontier for one account/trade and reconstructs exactly one unresolved final
slice at `submitting`. Earlier settled OPEN records are replayed using their
durable settled volumes; the pending record must be at or beyond the durable
dispatch barrier, have zero settled volume, and be `submitting`, `accepted`, or
`reconciling`. Any gap, additional record after the pending slice, malformed
descriptor, gated settlement mode, or inconsistent volume fails closed.

The helper applies no fill volume, creates no `DispatchPermit`, and does not
call a transport. After restart, the private owner loop acquires the fresh
lease and classifies the record as `reconcile_only`. A new
`OperationReconciliationWorker` re-anchors its in-memory baseline to the new
observation graph. Only a fresh, account-matched history observation carrying
the durable result binding may then let `ManagedTradeOwner` commit executed
volume. The same durable operation id is retained and the broker transport is
never called a second time.

The integration regression uses two independent child processes and the real
file journal, lease, Python transport seam, and observation graph. Process one
performs one `order_send` and exits before the matching deal is visible.
Process two starts a new owner loop, observes the deal, settles the OPEN, and
verifies that the durable call counter remains one.

## Consequences

- Unresolved OPEN exposure remains zero across restart until authoritative deal
  evidence is observed.
- A matching post-restart deal settles the original operation without a new
  operation id or resend capability.
- Account mismatch, missing/foreign evidence, event gaps, incomplete history
  coverage, and contradictory observations remain unresolved and fail closed.
- CLOSE, INOUT, automatic scheduling, and the public ABI remain outside this
  slice.

## Verification

`managed_trade_owner_test` covers reconstruction at the submitting edge with
zero exposure. The Python integration regression runs separate first and
second processes against the durable Windows journal, verifies
`PrivateOwnerLoop::reconcile_only`, settles one matching history deal through
`OperationReconciliationWorker`, and proves exactly one `order_send` call.

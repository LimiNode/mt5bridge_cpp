# ADR-0036: Allocation-gated managed close settlement and recovery

## Status

Proposed as the bounded follow-up to the durable managed allocation ledger.
This decision defines the owner/reconciliation admission boundary; it does not
select an allocation policy or change the public SDK.

## Context

ADR-0029 lets `ManagedTradeOwner::settle_close_reconciliation()` derive a
fresh, provenance-bearing exit volume and then advance both the durable journal
and the in-memory managed state. ADR-0035 now provides a durable close-leg
allocation entry, but it is not yet part of that path.

Using the two paths independently would leave two unsafe crash windows:

```text
history evidence -> managed exposure
```

could apply one broker deal twice after a restart, and:

```text
allocation entry -> managed exposure
```

could leave a committed ledger claim that the owner does not know how to
replay. A read-only ADR-0034 admission result is not a reservation and cannot
close either window.

The next implementation must therefore make the allocation entry the durable
claim that gates close exposure. It must also define what a restart does when
the process stopped between allocation publication, journal publication, and
the in-memory state update.

## Decision

The first owner integration supports only an explicitly allocation-gated
`OperationKind::close` whose source mapping consumes `broker_close_leg`.
`broker_reverse_open_leg`, `INOUT` opening ownership, FIFO/LIFO/pro-rata
selection, and cross-operation policy remain outside this slice.

### Close settlement publication order

For one fresh `OperationReconciliationWorker` cycle, the owner must perform
the following steps in order. No managed state mutation is allowed before step
4 succeeds.

1. Validate the cycle, operation identity, result-derived deal identity, and
   observation provenance using the existing ADR-0029 rules.
2. Produce or exactly replay the immutable per-deal settlement fact/frontier
   for `(OperationKey, DEAL_TICKET)`. The frontier must contain the complete
   durable fact set for the operation; cumulative `settled_volume` is never
   treated as this deal's individual contribution.
3. Reload the settlement fact, broker reversal, ownership basis, operation, and
   broker-only envelope. Derive the canonical integer-domain
   `ManagedBrokerVolumeMapping` and reject any mismatch or unsupported
   reverse-open claim.
4. Commit the `ManagedAllocationEntry` under the account-scoped fencing
   capability and allocation-store lock. Only `committed` and exact-payload
   `already_committed` are admissible outcomes. Missing or pending settlement,
   source drift, capacity exhaustion, conflicting replay, and lease loss leave
   managed state unchanged.
5. Advance the operation journal to the cumulative settled frontier with its
   normal CAS transition. The durable journal transition is the publication
   point for the owner-visible cumulative close result.
6. Apply the corresponding delta to `ManagedTradeState` only after the journal
   transition succeeds. The in-memory state is replaced only by a validated
   candidate, as in ADR-0026 and ADR-0029.

The allocation commit is intentionally before the journal transition. If the
process stops after step 4, the committed allocation is the durable recovery
receipt and no exposure has yet been applied. If it stops after step 5, the
journal is authoritative and restart reconstruction supplies the in-memory
state.

### Exact replay and duplicate prevention

The allocation key remains:

```text
(BrokerReversalKey, ManagedDealSettlementKey, broker_leg)
```

The owner must distinguish these cases while holding the same account-scoped
writer ownership:

```text
exact key absent
    -> fresh source admission, capacity check, allocation publication

exact key present with identical payload
    -> already_committed; never apply the deal delta a second time

exact key present with different payload
    -> conflict; leave journal and managed state unchanged
```

`already_committed` is not by itself evidence that the journal transition was
completed. The owner compares the committed entry's cumulative operation
frontier with the durable operation record:

- if the journal already includes the entry, replay is a no-op for managed
  exposure;
- if the journal is still behind the committed entry, recovery may perform the
  exact journal CAS transition from the durable allocation evidence, without a
  new observation or a second allocation claim;
- if the journal is ahead of the allocation frontier, or contains a different
  cumulative decomposition, recovery fails closed as an invalid durable
  state.

The same rule applies to an in-process retry and to a retry after restart. A
history snapshot or a caller-supplied volume can never re-apply an entry whose
allocation key is already committed.

### Restart recovery

Recovery acquires the account-scoped lease before reading or repairing the
owner/allocation pair. It performs an all-or-nothing historical allocation
scan, then groups committed close entries by `OperationKey` in deterministic
key order.

For every allocation-backed close operation, recovery verifies:

- every entry passes ADR-0035 historical source validation;
- each managed settlement key occurs at most once;
- the integer sum of committed logical units is overflow-safe;
- the journal's `settled_volume` is either equal to the committed frontier or
  is exactly behind it because publication stopped before the journal CAS;
- an operation cannot be reconstructed from a broker mapping that is absent,
  conflicting, or attached to another account/trade.

When the journal is exactly behind, recovery performs the same durable CAS
transition that step 5 would have performed and then reconstructs
`ManagedTradeState` from the resulting journal. It does not call the broker,
refresh history, or run current-revision admission for an already committed
historical entry. If any comparison is ambiguous, recovery returns a failure
without mutating exposure.

Legacy close records that have no valid allocation evidence are not silently
treated as allocated. They remain observation-only and must not increase
managed exposure through this allocation-gated recovery path until a separate
migration or fresh authoritative close proof is defined.

### Crash matrix

| Durable state after restart | Recovery result |
| --- | --- |
| settlement fact/frontier only | pending proof; no allocation and no exposure |
| allocation entry, journal before its frontier | CAS journal from the exact durable entry, then reconstruct |
| allocation entry and matching journal frontier | idempotent replay; no exposure delta |
| journal frontier without matching allocation entry | invalid durable state; fail closed |
| same key with different payload or conflicting source | invalid/conflict; fail closed |

The allocation store remains the only consumer of broker close capacity. The
owner does not mutate the broker envelope, settlement fact, ownership basis,
or allocation entry during recovery.

## Consequences

- A close deal becomes eligible for managed exposure only after a durable,
  source-coherent allocation claim exists.
- A process crash cannot turn one committed broker deal into two managed
  deltas; exact allocation replay is idempotent and journal replay is CAS
  guarded.
- The existing durable journal remains the owner-visible cumulative frontier,
  while the allocation ledger is the immutable per-deal claim behind it.
- The first implementation may reject legacy close records and pending facts;
  conservative failure is preferable to reconstructing exposure from an
  unproven volume.
- No public ABI, broker-envelope bytes, allocation policy, reverse-open
  semantics, or owner-loop scheduling policy changes.

## Verification boundary

The implementation slice must add focused regressions for:

- allocation publication before journal CAS followed by restart recovery;
- journal CAS before in-memory state replacement followed by reconstruction;
- exact replay after journal advancement with no second state delta;
- same allocation key with a different payload rejected as conflict;
- duplicate managed settlement/deal identity rejected before exposure;
- pending-only settlement and journal-without-allocation rejected;
- source, capacity, account, and fencing failures leaving managed state
  unchanged; and
- successful close settlement proving that the committed allocation and the
  journal frontier agree before exposure mutation.

The tests must use durable source fakes/stores and must destroy/recreate owner
and journal objects for restart cases. Reusing an in-memory proof object is not
a restart test.

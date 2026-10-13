# ADR-0036: Allocation-gated managed close settlement and recovery

## Status

Proposed as the bounded follow-up to the durable managed allocation ledger.
This decision defines the owner/reconciliation admission boundary; it does not
select an allocation policy or change the public SDK. The implementation is
staged: durable settlement preparation and admission are separate from owner
wiring that applies the exposure-effective journal frontier.

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

There is also a publication cycle in the existing contracts. The settlement
store currently accepts a committed frontier only when the journal already
contains that same settled frontier, while allocation admission reads only
committed settlement and the owner must not publish the exposure-effective
journal frontier before the allocation claim.

The next implementation must therefore make the allocation entry the durable
claim that gates close exposure, while retaining an exact durable receipt for a
settlement that is prepared but not yet exposure-effective. It must also define
what a restart does when the process stopped between preparation, allocation
publication, journal publication, settlement finalization, and the in-memory
state update.

## Decision

The first owner integration supports only an explicitly allocation-gated
`OperationKind::close` whose source mapping consumes `broker_close_leg`.
`broker_reverse_open_leg`, `INOUT` opening ownership, FIFO/LIFO/pro-rata
selection, and cross-operation policy remain outside this slice.

### Close settlement publication protocol

For one fresh reconciliation cycle, the account writer first loads the durable
operation at base revision `R` while holding the account-scoped fencing
capability. The owner then builds a validated target operation at `R + 1` using
the normal journal state machine; it does not manufacture an arbitrary record by
changing `settled_volume` in memory.

The settlement store exposes a settlement-specific preparation/finalization
seam. A prepared manifest contains the exact fact, complete frontier, base
revision, and target frontier revision. It is durable recovery evidence, but it
is not visible through the ordinary committed-only `load()` API.

For one fresh cycle, the owner must perform the following steps in order. No
managed state mutation is allowed before the final step succeeds.

1. Validate the cycle, operation identity, result-derived deal identity, and
   observation provenance using the applicable close proof. The existing
   ADR-0029 aggregator remains the path for `OUT` and `OUT_BY`; it is not
   silently broadened to accept `INOUT`.
2. Build a target operation at `R + 1` and produce the exact per-deal fact and
   complete cumulative frontier for `(OperationKey, DEAL_TICKET)`. The fact's
   source revision may be `<=` the target frontier revision. Cumulative
   `settled_volume` is never treated as this deal's individual contribution.
3. Durably prepare the exact fact/frontier pair. Preparation validates the
   current base operation, target volume/state, complete fact set, provenance,
   and overflow-safe sum, then publishes a pending manifest. A pending-only
   fact is not a committed settlement and ordinary ADR-0034 admission must
   continue to report it as unavailable.
4. Under the same account fencing capability and allocation-store lock, run a
   staged admission for the prepared target. It reloads and compares the
   prepared fact/frontier, broker reversal, ownership basis, operation base,
   and broker-only envelope, and checks capacity. This is a separate owner
   protocol; the ordinary committed-only ADR-0034 admission is unchanged.
5. Commit the `ManagedAllocationEntry`. Only `committed` and exact-payload
   `already_committed` are admissible outcomes. Missing evidence, source drift,
   capacity exhaustion, conflicting replay, and lease loss leave managed state
   unchanged.
6. Advance the operation journal from base revision `R` to the target frontier
   with its normal CAS transition. This is the exposure-effective durable
   publication point.
7. Finalize the prepared settlement against the now committed target journal
   record. Finalization is idempotent and publishes the fact/frontier as
   committed; a crash before this step leaves a recoverable prepared receipt.
8. Apply the corresponding delta to `ManagedTradeState` only after the journal
   transition and settlement finalization succeed. The in-memory state is
   replaced only by a validated candidate, as in ADR-0026 and ADR-0029.

The allocation commit is intentionally before the journal transition. The
prepared manifest is the recovery receipt until settlement finalization; the
allocation entry becomes the durable claim that authorizes the CAS. Neither
prepared settlement nor `already_committed` by itself mutates exposure.

### Staged admission and exact replay

The ordinary ADR-0034 admission remains committed-only. A pending fact must not
be returned by `load()` or accepted as a reservation token. The owner path has a
separate staged admission that accepts only the exact prepared manifest while
holding the account fencing capability and allocation-store lock. It performs
the same source-coherence checks as ADR-0034, but compares the target frontier
with the base operation at `R` rather than requiring the exposure-effective
journal record at `R + 1` to exist already.

The allocation key remains:

```text
(BrokerReversalKey, ManagedDealSettlementKey, broker_leg)
```

The owner must distinguish these cases while holding the same account-scoped
writer ownership:

```text
exact key absent
    -> staged source admission, capacity check, allocation publication

exact key present with identical payload
    -> already_committed; never apply the deal delta a second time

exact key present with different payload
    -> conflict; leave journal and managed state unchanged
```

`already_committed` is not by itself evidence that the journal transition was
completed. The owner compares the exact committed or prepared settlement
frontier with the durable operation record:

- if the journal already includes the frontier, replay is a no-op for managed
  exposure;
- if the journal is still at the prepared base revision, recovery may perform
  the exact journal CAS transition and then finalize the prepared settlement;
- if the journal is at the frontier but settlement finalization is pending,
  recovery finalizes the exact manifest and performs no second exposure delta;
- if the journal is ahead with a different cumulative decomposition, or the
  matching frontier/allocation evidence is absent, recovery fails closed.

An allocation entry records an immutable reference to the exact settlement
frontier it consumed: the frontier revision and an identity/provenance digest.
Recovery must load and validate that complete frontier; a revision number or
the sum of allocation entries alone is not a completeness proof. The durable
allocation format therefore has a new version for this reference. Older
allocation records remain capacity-consuming historical claims, but they are
not silently upgraded to the new frontier-backed recovery proof.

### INOUT close-leg proof

The allocation-gated path uses a separate bounded proof for
`DEAL_ENTRY_INOUT`. It must bind the operation's single history-deal identity
to the durable `BrokerReversalRecord`, the durable ownership basis, and the
managed close intent. The proof derives managed logical units from an
authoritative managed reconciliation source; it never copies broker volume into
managed units. The broker mapping may consume only `broker_close_leg`.

`broker_reverse_open_leg` remains unallocated, and ambiguous position/order
binding, stale observation, incomplete deal fields, or missing managed-delta
evidence fail closed. The existing `history_deal_volume(..., exit)` path is not
reused for this proof because it deliberately rejects `INOUT`.

### Restart recovery

Recovery acquires the account-scoped lease before reading or repairing the
owner/allocation pair. It first classifies each close operation as `legacy` or
`allocation_gated` using an explicit durable operation marker. A new gated close
must never fall back to the legacy journal-only path when its allocation proof
is absent.

For gated operations, recovery performs an all-or-nothing historical allocation
scan, accepting either a committed settlement or the exact prepared manifest
belonging to an interrupted publication. It then groups committed close entries
by `OperationKey` in deterministic key order.

For every allocation-backed close operation, recovery verifies:

- every entry passes ADR-0035 historical source validation;
- every entry has a matching committed or exact prepared settlement frontier;
- every frontier entry has exactly one matching allocation claim for the gated
  close being advanced;
- each managed settlement key occurs at most once;
- the integer sum of committed logical units is overflow-safe;
- the journal's `settled_volume` is either equal to the committed frontier or is
  exactly behind it because publication stopped before the journal CAS; and
- an operation cannot be reconstructed from a broker mapping that is absent,
  conflicting, or attached to another account/trade.

When the journal is exactly behind a prepared or committed frontier and the
matching allocation claim exists, recovery performs the same durable CAS
transition that step 6 would have performed, finalizes a prepared settlement if
needed, and then reconstructs `ManagedTradeState` from the resulting journal.
It does not call the broker, refresh history, or run current-revision admission
for an already committed historical entry. If any comparison is ambiguous,
recovery returns a failure without mutating exposure.

Legacy close records that have no valid allocation evidence retain their
existing ADR-0029 journal-only recovery behavior under the explicit `legacy`
marker. A close explicitly marked `allocation_gated` without a valid
prepared/committed allocation proof is invalid and must not increase managed
exposure until a separate migration or fresh authoritative close proof is
defined.

### Crash matrix

| Durable state after restart | Recovery result |
| --- | --- |
| prepared settlement only | pending proof; no allocation and no exposure |
| prepared settlement + allocation, journal before its frontier | CAS journal from the exact prepared frontier, finalize, then reconstruct |
| allocation + committed settlement and matching journal frontier | idempotent replay; no exposure delta |
| allocation + prepared settlement and matching journal frontier | finalize, then idempotent replay; no exposure delta |
| journal frontier without matching allocation entry | invalid durable state; fail closed |
| same key with different payload or conflicting source | invalid/conflict; fail closed |

The allocation store remains the only consumer of broker close capacity. The
owner does not mutate the broker envelope, settlement fact, ownership basis, or
allocation entry during recovery.

## Consequences

- A close deal becomes eligible for managed exposure only after a durable,
  source-coherent allocation claim exists.
- A prepared settlement is durable recovery evidence, but is not an
  exposure-effective journal frontier and is not ordinary committed admission.
- A process crash cannot turn one committed broker deal into two managed deltas;
  exact allocation replay is idempotent and journal replay is CAS guarded.
- The existing durable journal remains the owner-visible cumulative frontier,
  while the allocation ledger is the immutable per-deal claim behind it.
- Legacy allocation records continue to consume broker capacity, but cannot be
  treated as frontier-backed gated recovery evidence without an explicit
  compatibility decision.
- No public ABI, broker-envelope bytes, allocation policy, reverse-open
  semantics, or owner-loop scheduling policy changes.

## Verification boundary

The implementation slice must add focused regressions for:

- prepared settlement without allocation remains pending and does not pass
  ordinary admission;
- staged admission accepts the exact prepared target without treating it as a
  committed exposure frontier;
- allocation publication before journal CAS followed by restart recovery;
- allocation committed with prepared settlement, followed by restart before
  journal CAS and before settlement finalization;
- journal CAS completed before settlement finalization, followed by restart;
- exact replay after journal advancement with no second state delta;
- same allocation key with a different payload rejected as conflict;
- duplicate managed settlement/deal identity rejected before exposure;
- pending-only settlement and journal-without-allocation rejected;
- source, capacity, account, and fencing failures leaving managed state
  unchanged; and
- successful close settlement proving that the committed allocation and the
  journal frontier agree before exposure mutation;
- INOUT close-leg proof accepts only an authoritative managed delta and rejects
  reverse-open or ambiguous ownership;
- legacy close recovery remains separate from explicitly allocation-gated close
  recovery; and
- version-1 allocation records continue to consume broker capacity but cannot
  be treated as frontier-backed gated recovery evidence without an explicit
  compatibility decision.

The tests must use durable source fakes/stores and must destroy/recreate owner
and journal objects for restart cases. Reusing an in-memory proof object is not
a restart test.

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

### Implementation staging

Stage B implements the durable preparation/finalization protocol and its
restart validation. Stage C1 adds the immutable `ManagedSettlementMode`
discriminator to the operation journal and `MT5PRP01` preparation receipt:
`legacy` preserves the existing journal-only recovery contract, while
`allocation_gated` is legal only for a CLOSE operation and requires the
allocation-backed evidence described below. Existing journal versions 1--4
and preparation version 1 decode as `legacy`; unknown or malformed mode values
fail closed, and compare-and-commit cannot change the mode. The legacy owner
recovery path never interprets a gated CLOSE as legacy. Stage C2 now provides a
private structural `ManagedInoutCloseDeltaCandidateProducer`: it checks a
positive delta candidate against pre/post exposure frontiers, one fresh,
complete INOUT history observation, a staged ownership association, and the
exact close intent. The candidate is not an independent managed-attribution
source and therefore is not authorization for allocation or exposure
mutation. It accepts neither a caller scalar nor broker volume, does not
attribute the reverse-open leg, and does not publish settlement, allocation,
journal state, or exposure. Stage C3 remains blocked until an independent
managed-attribution source is defined and connected to the owner loop.

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

The manifest also contains a sealed staged ownership association. This is not
the ordinary `ManagedOwnershipBasis`: that record intentionally requires an
already-settled journal operation. The staged association instead binds the
account, `OperationKey`, `BrokerReversalKey`, broker provenance, base revision,
target revision, and the authoritative close-intent/deal-binding digest. It
contains no volume cap and cannot authorize exposure by itself. After the
journal CAS, the staged association is finalized into the ordinary immutable
ownership basis and is then eligible for historical validation.

For one fresh cycle, the owner must perform the following steps in order. No
managed state mutation is allowed before the final step succeeds.

1. Validate the cycle, operation identity, result-derived deal identity, and
   observation provenance using the applicable close proof. The existing
   ADR-0029 aggregator remains the path for `OUT` and `OUT_BY`; it is not
   silently broadened to accept `INOUT`. For an `INOUT` close, the owner must
   first obtain the structural `ManagedInoutCloseDeltaCandidate` described
   below and an independent managed-attribution authorization. That source is
   not implemented in this slice.
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
   prepared fact/frontier, staged ownership association, broker reversal,
   operation base, the structural managed-delta candidate plus its independent
   authorization, and broker-only
   envelope, and checks capacity. This is a separate owner protocol; the
   ordinary committed-only ADR-0034 admission and
   `validate_managed_ownership_basis()` contract are unchanged.
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

The frontier digest is calculated from a canonical encoding containing the
domain/version tag, account and operation identity, target frontier revision,
cumulative settled volume, frontier provenance, and the entry count followed by
entries sorted by `(deal_ticket, source_operation_revision,
managed_logical_units)`. The digest covers the complete frontier, not only the
new deal's observation. A digest mismatch, duplicate entry, missing entry, or
different canonical composition is a durable conflict.

### INOUT close-leg candidate and authority boundary

The allocation-gated path will eventually need a separate bounded authorization
for `DEAL_ENTRY_INOUT`. C2 currently provides only the structural
`ManagedInoutCloseDeltaCandidate`: it binds the operation's result-derived
history-deal identity to the staged ownership association, durable
`BrokerReversalRecord`, managed close intent, and fresh observation provenance.
It derives a positive delta from the supplied pre/post exposure frontiers, but
those frontiers are not an independent attribution source. The candidate is
therefore not a caller-supplied scalar, not broker volume, and not authorization
for allocation or exposure mutation.

The missing authoritative source must independently establish that the managed
delta belongs to the observed INOUT close. Until that source exists, C2 remains
a fail-closed structural validator. Its candidate checks still require complete
history coverage, a zero-ticket predicate with exactly one non-zero correlation
binding, a staged association with the exact close-intent digest, and durable
broker reversal identity. Broker `broker_close_leg` remains reserved for later
integer-domain mapping; broker volume is never copied into managed logical units.

`broker_reverse_open_leg` remains unallocated, and ambiguous position/order
binding, stale observation, incomplete deal fields, competing same-position
history, or missing managed-delta authority fail closed. An `INOUT` close
remains unsupported in production and produces no prepared settlement,
allocation, journal advance, or exposure mutation. The existing
`history_deal_volume(..., exit)` path is not reused because it deliberately
rejects `INOUT`.

### Restart recovery

Recovery acquires the account-scoped lease before reading or repairing the
owner/allocation pair. It first classifies each close operation as `legacy` or
`allocation_gated` using an explicit durable operation marker in the operation
record. New gated records must persist that marker before dispatch/reconciliation
and must never fall back to the legacy journal-only path when their allocation
proof is absent.

The journal compatibility contract is explicit: records written by the old
format and lacking the marker are classified as `legacy`; they are never
reinterpreted as gated. A new-format gated record must carry the marker and
must carry the prepared/committed evidence required below. A format migration
may preserve old records as legacy, but it must not manufacture a gated marker
or a frontier digest for them.

For gated operations, recovery performs an all-or-nothing historical allocation
scan, accepting either a committed settlement or the exact prepared manifest
belonging to an interrupted publication. It then groups committed close entries
by `OperationKey` in deterministic key order.

For every allocation-backed close operation, recovery verifies:

- every entry passes ADR-0035 historical source validation;
- every entry has a matching committed or exact prepared settlement frontier;
- every prepared frontier has a matching staged ownership association and, once
  C3 exists, an independent authoritative managed-delta authorization;
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
needed, finalizes the staged ownership association into the ordinary basis, and
then reconstructs `ManagedTradeState` from the resulting journal.
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
| prepared settlement + staged ownership + allocation, journal before its frontier | CAS journal from the exact prepared frontier, finalize ownership/settlement, then reconstruct |
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
- C2 structural INOUT candidate validation rejects reverse-open attribution,
  caller-selected tickets, competing same-position history, or ambiguous
  ownership; authoritative managed-delta authorization remains a C3 prerequisite;
- staged ownership cannot be admitted through the ordinary settled-basis
  validator and is finalized only after the target journal CAS;
- legacy close recovery remains separate from explicitly allocation-gated close
  recovery; and
- version-1 allocation records continue to consume broker capacity but cannot
  be treated as frontier-backed gated recovery evidence without an explicit
  compatibility decision.

The tests must use durable source fakes/stores and must destroy/recreate owner
and journal objects for restart cases. Reusing an in-memory proof object is not
a restart test.

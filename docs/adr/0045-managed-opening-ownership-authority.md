# ADR-0045: Managed opening ownership authority gate

## Status

Proposed as C3A B3E. This ADR defines the trust boundary between a verified
OPEN allocation and managed-position ownership. It is an architecture-only
slice: it adds no runtime wiring, exposure mutation, broker send, public ABI,
or `INOUT` support.

## Context

ADR-0044 and the B3D producer establish a sealed
`VerifiedManagedOpeningAllocation`. The proof is authoritative for one
committed opening allocation: its account-scoped operation, immutable source
revision, broker `POSITION_IDENTIFIER`, managed logical units, evidence digest,
and historical/current frontier scope are independently reloaded under the
account fence.

That proof is deliberately narrower than ownership. It does not prove that the
position is currently exclusively owned by one managed trade, that earlier
allocations were consumed exactly once, or that no external participant changed
the net position. A current broker position volume, a non-zero digest, or a
successful B3D lookup must therefore never be copied into managed exposure.

The existing contracts have separate authority boundaries:

| Existing contract | Independently establishes | Does not establish |
| --- | --- | --- |
| B3D `ManagedOpeningProvenanceProducer` | One committed OPEN allocation and its source frontier | Exclusive current ownership or permission to mutate exposure |
| `ManagedTradeOwner` / `ManagedTradeState` | Logical trade lifecycle, intent, and current in-memory exposure | Broker ownership, opening allocation, or external-position absence |
| `ManagedOwnershipBasis` | A close-operation/reversal association | OPEN ownership, per-deal attribution, or position continuity |
| Managed close allocation ledger | Consumption of close-leg capacity | Opening ownership or a current position partition |
| `ObservationGraph` | Account-scoped position/history observations and provenance | Durable ownership, complete consumption history, or semantic attribution |
| Account lease and journal CAS | Writer serialization and publication fencing | Truth of caller-selected identity, volume, or ownership |

Consequently B3E must add a separate authority gate before any future owner
integration. The gate must join independently durable ownership evidence with a
fresh position observation; it must not turn B3D into an exposure API.

## Decision

Managed ownership is authorized only by a complete, durable ownership frontier
for one exact broker position. The frontier is a partition of the observed
position, not a scalar copied from the broker or from a B3D proof.

A future frontier contains, at minimum:

```text
account
POSITION_IDENTIFIER
position symbol and direction
complete normalized broker volume
current observation provenance (graph identity and required revisions)
canonical frontier digest and schema version
managed ownership entries
explicit unmanaged/external entries
source and consumption references for every entry
```

A managed entry identifies one `TradeId`, one or more committed OPEN
operations, the exact logical units, the normalized broker contribution, and
the immutable B3D source identity. An unmanaged entry carries positive broker
volume and an independent evidence identity but no managed trade or logical
units. Entries must form an overflow-safe exact partition of the complete
position. Absence of an unmanaged entry is not evidence that no external
volume exists.

### Additional authority evidence

Before a verified OPEN can contribute to managed ownership, a later
implementation must independently reload and validate all of the following:

1. The OPEN journal record is committed, account-consistent, and still bound
   to the operation key carried by the B3D proof. `OperationKey.trade_id`
   identifies the managed trade; a caller cannot substitute another trade.
2. The B3D proof is current for live ownership mutation. A historical proof may
   be used to replay a durable frontier after restart, but it is not by itself
   permission to mutate live exposure.
3. A complete position/history observation covers the exact
   `POSITION_IDENTIFIER`, account, symbol, direction, and normalized volume.
   Missing coverage, stale domains, or an ambiguous graph return
   `unavailable`, not a guessed frontier.
4. Every managed entry is backed by a committed B3D allocation and every
   consumed deal/allocation appears exactly once in durable consumption
   history. Cumulative frontiers are compared by their complete source set;
   revision numbers and digests do not replace that set.
5. Explicit unmanaged/external residue is present when the position contains
   volume not explained by managed entries. An unexplained position change is
   a contradiction, not an implicit unmanaged entry.
6. The candidate transition is overflow-safe, step-normalized, account-scoped,
   and compatible with the durable owner frontier. No logical units are
   reverse-derived from broker volume.

The existing close-only ownership basis and allocation ledger cannot satisfy
these requirements alone. The implementation must introduce one reviewed
durable ownership-frontier/consumption source, or extend an existing durable
store without creating a competing source of truth. Until that source exists,
the correct result is `unavailable` and managed exposure remains unchanged.

### Historical and current semantics

The two B3D scopes remain distinct:

| B3D result | Allowed B3E use | Not allowed |
| --- | --- | --- |
| `historical` frontier A | Exact restart/replay of an already committed ownership frontier that references A | New live ownership mutation or inference of current position state |
| `current` frontier B | Input to a live ownership check, together with complete current observation and durable ownership history | Automatic authorization without an ownership frontier |
| compatible but unpublished successor | No ownership decision | Treating B3B freshness as publication or ownership |
| missing/ambiguous/contradictory current evidence | `unavailable` or `invalid_evidence` | Patching from current broker volume or digest |

For partial fills `A -> B`, A remains an immutable historical frontier. B is a
new cumulative source set and may replace A as current only after its allocation
and ownership frontier are durably published. The B frontier must not add A's
cumulative units a second time. A replay of the exact B source is idempotent;
a different owner, position, source set, or digest is a conflict.

### Ownership identity and external changes

One managed trade may have multiple OPEN operations. Each operation remains a
distinct source identity, while `OperationKey.trade_id` joins it to the trade.
The ownership frontier must retain every contributing operation/allocation
reference; it must not collapse them into an untraceable total.

External or non-managed position changes require a new complete frontier. B3E
does not infer ownership from the latest position volume, FIFO/LIFO policy, or
the fact that a position identifier is unchanged. Mixed managed ownership,
unexplained residue, and unsupported legacy positions are not resolved by
choosing one managed entry. If an independently durable external observation is
not available, the gate fails closed.

### Durable consumption and double-attribution prevention

Consumption history is part of the ownership authority, not an optional audit
trail. A durable claim identifies the account, position, `TradeId`, source
operation/allocation identity, complete source frontier identity, and consumed
logical units. The canonical set of source deal/allocation references is unique
within the account/position namespace.

The following replay rules are mandatory:

```text
exact claim and exact frontier payload already committed
    -> already_committed; apply no second exposure delta
same identity with a different payload or source set
    -> conflict; leave durable and in-memory state unchanged
overlapping source references claimed by another owner
    -> conflict; never split or silently reassign the claim
missing or contradictory consumption evidence
    -> unavailable/invalid_evidence; do not publish ownership
```

### Private integration seam

The later implementation should expose one private, status-bearing ownership
gate, conceptually:

```text
verify_managed_opening_ownership(
    VerifiedManagedOpeningAllocation,
    durable operation/journal evidence,
    complete current observation,
    durable ownership frontier and consumption history,
    account-scoped fencing capability)
```

Its result is a sealed, scope-limited ownership authorization or a fail-closed
status. It must not return a caller-selected volume or a mutable pointer to
`ManagedTradeState`.

Required statuses are:

| Status | Meaning |
| --- | --- |
| `authorized` | Complete independent evidence and exact frontier transition are valid |
| `already_committed` | Exact ownership claim was durably committed; no second delta |
| `unavailable` | Required source is absent, incomplete, unpublished, or not current |
| `invalid_evidence` | Durable records or observations contradict or fail validation |
| `conflict` | Competing claim or same identity with different payload |
| `io_error` | A required durable source could not be read or written |
| `lease_not_held` | Fencing capability was absent or changed during verification/publication |

The gate is read-only until a later, separately reviewed publication protocol.
For the eventual owner integration, validation and publication order is:

1. Validate the request and account/trade/operation identity.
2. Acquire the account lease and retain the fencing capability through every
   source read, claim check, and publication decision.
3. Reload the journal record and B3D proof; reject historical proof for a new
   live mutation.
4. Reload complete current position/history evidence and validate graph identity,
   coverage, position identity, direction, symbol, and normalized volume.
5. Reload the ownership frontier and consumption history; validate canonical
   digests, exact partition, source references, and overflow-safe quantities.
6. Build a candidate frontier/claim without mutating `ManagedTradeState`.
7. Publish an exact prepared manifest, then commit the ownership claim/frontier
   with an account-fenced compare-and-swap boundary.
8. Recheck the fencing token and return the sealed authorization only after the
   durable commit is complete. The owner may apply a validated logical-state
   candidate only after this point.

The owner, not the gate, owns the final `ManagedTradeState` replacement. No
broker call is made by this seam. A crash before the durable commit leaves only
a recoverable prepared manifest; a crash after commit is replayed by exact
identity without applying exposure twice.

### Restart and fencing

Recovery acquires the same account-scoped lease before loading or repairing the
ownership frontier. A new observation graph may re-anchor historical evidence,
but it cannot manufacture the old baseline or replace missing durable source
facts. Historical replay may reconstruct an already committed frontier from
durable records; live mutation still requires a fresh current proof.

If the fencing token changes during nested verification, publication, or the
post-commit check, the operation returns `lease_not_held`, with no authorization
and no caller-visible exposure change. Prepared records are either completed
from an exact payload under a fresh lease or abandoned as unavailable; they are
never treated as ownership by themselves.

## Evidence and ownership matrix

Every positive decision must identify the independent evidence that justifies
it. The following matrix is the minimum B3E regression contract:

| Scenario | Independent evidence required | Expected gate result |
| --- | --- | --- |
| Complete valid OPEN ownership | committed OPEN journal; current B3D proof; complete current position/history observation; exact durable frontier; complete consumption set; no unexplained residue | `authorized` |
| Same allocation replayed twice | exact committed claim/frontier payload and same source set | `already_committed`; zero second delta |
| Partial fill A -> B | immutable A claim; later B B3D source set; B frontier contains A plus new facts exactly once; current observation complete | A remains historical; B may become current |
| Historical A while B is current | exact durable A frontier and historical B3D proof; no current-mutation request | replay only; live mutation `unavailable` |
| External position change | complete current observation plus explicit durable unmanaged entry, or a contradiction proving unexplained change | explicit residue remains unmanaged; otherwise `unavailable`/`invalid_evidence` |
| Ambiguous ownership | competing managed entries, overlapping claims, or incomplete partition | `conflict` or `unavailable`; no mutation |
| Missing/contradictory consumption | absent, duplicated, rewritten, or source-inconsistent claim history | `unavailable`/`invalid_evidence` |
| Stale or transferred lease | token absent or changed during nested verification/publication | `lease_not_held`; no proof/authorization |
| Restart with a new graph | durable committed frontier and historical B3D proof; fresh graph for live mutation | historical replay may succeed; live mutation waits for current evidence |
| Concurrent ownership claims | same account/position fence and atomic claim CAS | one exact commit; competing claim `conflict` |
| Unsupported legacy position | no complete B3D chain or no durable ownership frontier | `unavailable`; no migration by inference |
| Incomplete publication/recovery | prepared manifest without exact committed frontier/claim | pending/unavailable; no exposure delta |

## Non-goals and strict boundaries

This ADR does not:

- enable production `INOUT` or implement CLOSE attribution;
- change `ManagedTradeOwner` or `ManagedTradeState` behavior;
- mutate managed exposure, broker state, or journal records;
- add broker sends, live migrations, or legacy-position repair;
- change the public ABI or consumer-facing headers; or
- restore or include unrelated `private_owner_loop` work.

The B3D producer remains a read-only source. A sealed B3D proof alone is never
an exposure permission.

## Implementation slices and dependency order

1. Add durable schema and restart fixtures for the ownership frontier and
   consumption claims. Reuse existing fencing and journal primitives; do not
   create a parallel source of truth.
2. Add a read-only ownership validator that joins B3D, journal, complete
   observations, and durable consumption. Cover all matrix failures before any
   publication API exists.
3. Add the prepared-manifest and fenced commit/recovery protocol with exact
   replay and competing-claim tests.
4. Add a private owner integration seam in a separate PR. It may replace logical
   state only after the durable authorization/commit and must remain separate
   from `INOUT`/CLOSE work.
5. Only after independent review of those slices may any exposure mutation be
   enabled. If the required durable facts cannot be produced, retain
   `unavailable` rather than manufacture ownership.

## Verification boundary

The B3E implementation must destroy and recreate owner, journal, frontier, and
observation objects for restart tests. Reusing an in-memory proof is not a
restart test. Positive tests must name the independent evidence they use, and
every negative test must assert that the caller-visible managed state and
durable source set remain unchanged.

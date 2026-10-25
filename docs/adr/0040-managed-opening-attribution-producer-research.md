# ADR-0040: Research boundary for a committed managed-opening producer

## Status

Proposed as C3A B1. This ADR records the result of tracing the existing
dispatch, journal, and observation paths. It does not implement an opening
allocation store, create a producer, enable production `INOUT`, or change the
public ABI.

## Problem

ADR-0038 deliberately leaves `VerifiedManagedOpeningAllocation` unavailable.
The next question is not how to serialize that value, but whether the current
broker and durable paths can prove that a committed managed `OPEN` created the
position identified by a later observation.

The answer must account for the three different MT5 entities:

- an **order** is an instruction and may remain pending or be modified;
- a **deal** is an execution fact and one order may produce several partial
  deals; and
- a **position** is the resulting exposure and may be changed by another EA,
  a manual action, or the server.

A position snapshot alone is therefore not an opening-allocation source. A
transaction hint alone is not one either: MQL5 transaction notifications are
not a durable ordered log, and the Python MetaTrader5 API does not expose
`OnTradeTransaction()`.

## Existing source inventory

The following inventory is based on the current `main` implementation.

| Existing source | What can be reloaded | Missing authority fact |
| --- | --- | --- |
| `OperationRecord` in `OperationJournal` | Account-scoped operation identity, immutable request, lifecycle state, revision, fencing token, result bytes, and settlement volume | No opening allocation, no persisted position identifier, and no proof that a position remained attributable to this operation |
| `python_dispatch_transport::extract_bindings()` | Result-derived order or deal ticket for a declared reconciliation predicate | `validate_result()` persists only `order` and `deal`; a position predicate is rejected, so the broker result does not bind `POSITION_IDENTIFIER` |
| `ManagedTradeOwner::history_deal_attribution_context()` | A fresh observation can join the bound order/deal history to one `position_id` | The value is computed in memory during reconciliation and is not an immutable opening source; it does not establish pre/post position ownership |
| `ObservationGraph` history and position snapshots | Deal fields, position identifier, observation revisions, and source provenance | Snapshots can be delayed, rewritten, incomplete, or affected by external activity; they do not create managed ownership |
| `ManagedTradeState` and `settled_volume` | The owner's logical lifecycle and a cumulative integer volume | The current `Volume` is not a durable broker-volume normalization and is derived from observed deal volume; it cannot by itself prove an opening allocation |
| `ManagedAllocationEntry` and `ManagedOwnershipBasis` | Immutable close-leg consumption and close-operation/reversal association | The allocation ledger is explicitly close-leg-only; the ownership basis is for settled CLOSE evidence and carries no opening source |
| Account lease and journal CAS | Serialized writers and monotonic durable revisions | Fencing orders writers but does not make caller-selected identity or volume authoritative |

This leaves a concrete gap: a fresh history-deal observation can tell us which
position identifier a deal reports, but there is no durable record that joins
that identifier to a committed managed opening allocation and preserves the
join through later consumption or external mutation.

## Decision

No trusted C3A B1 producer is added until the missing source chain exists. The
current `UnavailableManagedOpeningProvenanceSource` remains the only valid
production result, and `INOUT` remains fail-closed.

The first future producer must be a source-derived operation, not a store that
persists fields selected by its caller and later treats them as authority. It
may return `found` only after independently reloading and comparing all of the
following under one account-scoped fencing domain:

```text
committed managed OPEN journal record
  + result-derived order/deal binding
  + complete opening deal history for that operation
  + exact broker POSITION_IDENTIFIER from the deal observation
  + an immutable opening-allocation record with normalized broker volume
  + any prior-position / external-residue evidence required by the supported case
  + canonical digest and exact replay identity
```

The producer must compare the reloaded operation key, operation revision, and
position identifier with the lookup request. A non-zero identifier, digest,
broker volume, or caller flag is never sufficient.

### First bounded case

The initial implementation should support only a new managed opening whose
source chain is unambiguous. It must distinguish three different durable
facts rather than treating the first successful deal as a completed OPEN:

- **Per-deal execution evidence** is one immutable, independently validated
  deal fact. A complete history observation may stage or publish a new fact
  when its identity, fields, and source revision are complete. One fact does
  not by itself prove that the operation is terminal or that the whole opening
  frontier is complete.
- **The cumulative opening frontier** is the canonical, deterministically
  ordered set of all validated execution facts for one OPEN operation. A new
  fill creates a new frontier revision; earlier frontiers remain immutable and
  are never rewritten from a later position snapshot.
- **Terminal OPEN state** is a separate durable lifecycle conclusion. It is
  allowed only when the journal state and authoritative order/history evidence
  prove that no pending remainder can execute (for example, a fully filled
  order or an explicitly rejected, cancelled, or expired remainder). A first
  deal, a covered history window, or a non-zero position is not terminality.

For each newly observed fill, the bounded source chain is:

1. the OPEN operation is committed and has a result-derived order/deal binding;
2. the authoritative history window contains every execution deal for that
   operation, including partial fills, with complete ticket, order, position,
   entry, volume, and timestamp fields;
3. every such deal agrees on account, order identity, and one non-zero
   `POSITION_IDENTIFIER`;
4. the producer has an explicit, overflow-safe broker-volume normalization and
   a separately defined conversion to managed logical units;
5. the exact pre-operation position state is either empty or is already backed
   by a supported durable ownership frontier; and
6. no external residue, conflicting deal, history gap, or rewritten evidence is
   present.

History coverage is sufficient for this per-deal publication only when the
declared history window is completely covered at the sampled history-domain
revision, the observation reports no partial page or GAP, every matching deal
in the window has the required identity/entry/volume/time fields, and the new
deal's evidence revision equals that sampled revision. This proves the fact
as-of that observation revision; it does not claim that the order can no
longer fill.

Terminality requires a separate complete observation at or after the last
frontier: the durable OPEN lifecycle has no pending remainder, the active and
history-order views agree on a terminal outcome, the relevant history window
is complete through the terminal observation revision, and no later-fill or
history-rewrite indicator is present. A pending order with a complete current
window may publish a new per-deal fact, but it cannot publish a terminal OPEN
frontier.

Any existing netting position with unknown or mixed ownership, missing history,
fractional/unrepresentable volume, or an observation gap is `unavailable` or an
explicit invalid-evidence result. It is not repaired from the latest position
volume. A later fill after a non-terminal frontier is a new per-deal fact and
must extend the cumulative frontier. A fill that appears after the operation
was declared terminal is a contradiction requiring resynchronization or
invalid evidence; it must not silently mutate the old proof.

### Observation boundary

Polling is an acquisition mechanism, not an authority source. The producer
must keep these layers separate:

1. broker observations acquired from active orders, positions, history orders,
   and history deals;
2. completeness and provenance checks over bounded-overlap snapshots and
   history windows;
3. committed journal and opening-allocation evidence; and
4. the sealed verified ownership result returned only after all prior layers
   agree.

The source of observations is polling with durable cursors and restart
recovery. An optional MQL5 transaction adapter may provide low-latency hints,
but hints only trigger a fresh poll and never authorize publication,
allocation, or exposure mutation. Orders, deals, and positions remain
separate identities throughout the join.

### Publication and recovery boundary

When implementation begins, the writer must hold the same account-scoped lease
through source validation and publication of the opening allocation. The
bounded protocol is deliberately one-way and has no producer/allocation cycle:

1. **Source evidence:** reload the committed OPEN journal record and complete
   broker history, then publish immutable per-deal source facts under the
   account lease. Each fact retains the execution identity and fields,
   normalized quantity, observation provenance, coverage revision, format
   version, and exact journal source revision. Exact replay is idempotent and a
   different payload for the same key conflicts. These facts are not yet an
   opening allocation and cannot authorize ownership.
2. **Preparation:** persist an immutable opening-allocation preparation
   manifest containing the exact journal revision, position identifier,
   normalized quantity, exact durable keys and versions of every per-deal
   source fact, source revisions, and the canonical cumulative frontier
   digest. The digest verifies the referenced source set; it never replaces
   those reproducible facts. A prepared manifest is recovery evidence only.
3. **Publication:** while the lease is held, revalidate the manifest and the
   journal and referenced durable per-deal sources, then commit the immutable
   opening-allocation record with exact replay/conflict handling. The record is
   keyed by the operation, execution identity, and frontier revision; it is not
   constructed from caller-selected fields.
4. **Independent verification:** reload the committed allocation and all of
   its sources. Only this final comparison may construct the sealed verified
   proof.

The journal OPEN is already committed before this bounded publication starts;
the allocation record references its exact revision rather than recursively
creating the journal source it is meant to prove. A crash before preparation
leaves no allocation. A crash after preparation leaves a retryable manifest.
A crash after allocation publication is resolved by exact idempotent replay and
source revalidation. Lease loss at any publication step rejects the writer.

Recovery is all-or-nothing. A missing referenced source fact, unknown source
version, malformed or checksum-invalid manifest, or digest/source-set mismatch
is invalid evidence. Restart between preparation and allocation commit reloads
the exact manifest and referenced durable facts before retrying publication. A
committed allocation whose referenced facts are unavailable cannot produce a
sealed proof. If MT5 later rewrites its history, the immutable historical facts
remain the evidence that was admitted at their recorded coverage revision; a
conflicting fresh observation emits contradiction/resynchronization and cannot
rewrite them. A new live broker snapshot is never used to reconstruct a lost
source fact or repair a corrupt manifest.

The durable opening record must retain the exact source revisions, position
identifier, normalized broker quantity, managed logical quantity, and canonical
evidence digest. Historical replay validates those immutable sources and exact
identity; it does not rerun current admission against a newer journal revision.

### Quantity-domain prerequisite

The current owner uses `managed_trade::Volume = std::uint64_t`, and
`ManagedTradeOwner::history_deal_volume()` accepts an observed total only when
it is a positive whole number (`std::floor(total) == total`). This is a real
whole-lot/integer semantic, not a safe representation for ordinary fractional
Forex lots such as `0.01`, `0.05`, or `0.10`; those observations are currently
unrepresentable and must remain fail-closed.

The future producer therefore needs a separate exact quantity-domain contract:

- a fractional lot value is valid when it represents an integer number of the
  exact minimum broker volume step: `0.01` lot at step `0.01` is one step and
  `0.05` lot is five steps; a fractional number of steps such as `0.015` lot at
  step `0.01` is invalid;
- the broker step is first represented canonically as positive integer
  `step_units` plus an explicit decimal `scale`; the scale is supplied by a
  reviewed symbol/account quantity contract, not guessed independently for
  each observed volume;
- an MT5 binary `double` is accepted only when it is finite and positive and
  its IEEE-754 rounding interval contains exactly one in-range integer multiple
  of the canonical broker step. No matching integer, more than one possible
  integer, or a value beyond the bounded capacity is ambiguous and fails
  closed. This bounded uniqueness check tolerates the representation error of
  ordinary values such as `0.01` without applying an arbitrary epsilon or blind
  decimal rounding;
- the unique step count is converted to the overflow-safe `scale`/
  `step_units`/`units` representation before comparison or persistence;
- the mapping from broker step units to managed logical units is explicit,
  deterministic, and independently validated for every symbol/account mode;
- zero, negative, NaN, infinity, fractional-step, overflowed,
  incompatible-step, ambiguous-conversion, and scale-mismatch inputs are
  rejected; and
- the normalized broker quantity and the managed logical quantity are both
  retained in the immutable opening record and included in its digest.

Required examples are:

```text
0.01 lot / step 0.01  -> ACCEPT, 1 step
0.05 lot / step 0.01  -> ACCEPT, 5 steps
0.015 lot / step 0.01 -> REJECT, fractional step count
negative / NaN / inf  -> REJECT
overflow / scale mismatch / incompatible step -> REJECT
```

Closing this quantity-domain blocker is a prerequisite for release readiness
of a real fractional-lot manager. It is not solved by changing the current
whole-number conversion or by silently rounding broker volume.

## Required implementation seams

The next code slice may introduce only the seams needed to prove the first
bounded case:

1. a result/history join that exposes the complete opening execution identity;
2. a durable opening-allocation record and store with exact replay/conflict
   behavior;
3. a source-derived normalization contract for broker and managed units;
4. an account-fenced publication protocol linked to the committed OPEN; and
5. restart and negative regressions for gaps, multiple positions, external
   residue, partial history, conflicting replay, and stale writers.

It must not add mixed-owner policy, FIFO/LIFO/pro-rata attribution, legacy
position migration, automatic copy-trading, owner exposure mutation, or a
caller-supplied `is_authoritative`/digest/revision escape hatch.

## Verification plan

Before a producer can replace the unavailable source, the implementation must
demonstrate with real durable stores:

- each partial deal produces one immutable per-deal fact, while the cumulative
  frontier contains every validated deal and remains distinct from terminal
  OPEN state;
- a single OPEN with one or more partial deals produces one exact immutable
  position identity and survives restart;
- a first deal does not terminate a pending OPEN, and a later fill extends the
  frontier without rewriting an earlier revision;
- a terminal OPEN requires durable no-more-fills evidence, and a late fill
  after terminality produces contradiction/resync rather than silent replay;
- a result containing only order/deal tickets cannot create a proof without
  the later authoritative history join;
- a missing or rewritten history page, duplicate/conflicting deal, position
  mismatch, and unrepresentable volume fail closed;
- valid fractional lot values normalize to one unique integer step count,
  while fractional-step values, non-finite values, overflow, incompatible
  broker steps, ambiguous conversions, and scale mismatches fail closed;
- an existing mixed or external netting position remains unavailable;
- a crash after preparation but before allocation publication is retryable, and
  a crash after allocation publication is resolved by exact replay without
  creating a second record;
- missing source facts, rewritten history, manifest corruption, and a committed
  allocation with unavailable source facts fail closed without reconstruction
  from a live broker snapshot;
- replay after journal advancement returns the same proof or an exact conflict,
  without a second allocation; and
- writer A cannot publish after its account lease is transferred to writer B.

Until these checks pass, `found` is not a legal result and no allocation or
managed exposure mutation may consume the contract.

## Non-goals

This research slice does not change `OperationRecord`, `ManagedAllocationEntry`,
the public C ABI, `ManagedTradeOwner`, exposure accounting, or the C2
candidate. It also does not claim that the current repository can recover
legacy positions. Unsupported cases remain explicitly fail-closed.

## Research basis

This inventory was checked against `main` on 2026-10-25. The primary source
paths are `src/dispatch/managed_trade_owner.cpp`,
`src/trade/python_dispatch_transport.cpp`,
`include/mt5bridge/dispatch/journal.hpp`,
`include/mt5bridge/dispatch/journal_types.hpp`,
`include/mt5bridge/dispatch/managed_allocation.hpp`, and the
`ObservationGraph`/reconciliation headers. The actionable rule is to keep the
opening source `unavailable` until a producer can reload the complete chain
and pass the durable, fenced, restart-aware checks above; a new broker or
terminal observation must update this research before that status changes.

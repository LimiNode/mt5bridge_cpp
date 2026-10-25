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
source chain is unambiguous:

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

Any existing netting position with unknown or mixed ownership, missing history,
fractional/unrepresentable volume, or an observation gap is `unavailable` or an
explicit invalid-evidence result. It is not repaired from the latest position
volume.

### Observation boundary

The producer consumes a provenance-bearing reconciliation sample, not a
callback payload. The source of truth is polling of active orders, positions,
history orders, and history deals with bounded overlap, durable cursors, and
restart recovery. An optional MQL5 transaction adapter may provide low-latency
hints, but hints only trigger a fresh poll and never authorize publication,
allocation, or exposure mutation. Orders, deals, and positions remain separate
identities throughout the join.

### Publication and recovery boundary

When implementation begins, the writer must hold the same account-scoped lease
through source validation and publication of the opening allocation. The
publication boundary must reject a stale writer after ownership transfer and
must make the operation/allocation pair idempotent. A crash before or after
publication must be recoverable from durable manifests without obtaining a new
broker observation merely to manufacture missing evidence.

The durable opening record must retain the exact source revisions, position
identifier, normalized broker quantity, managed logical quantity, and canonical
evidence digest. Historical replay validates those immutable sources and exact
identity; it does not rerun current admission against a newer journal revision.

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

- a single OPEN with one or more partial deals produces one exact immutable
  position identity and survives restart;
- a result containing only order/deal tickets cannot create a proof without
  the later authoritative history join;
- a missing or rewritten history page, duplicate/conflicting deal, position
  mismatch, and unrepresentable volume fail closed;
- an existing mixed or external netting position remains unavailable;
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

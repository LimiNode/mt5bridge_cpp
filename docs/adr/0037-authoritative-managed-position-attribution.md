# ADR-0037: Authoritative managed position attribution prerequisite

## Status

Proposed as the research prerequisite for Stage C3. This decision defines the
independent evidence that would be required before the structural C2
`ManagedInoutCloseDeltaCandidate` could authorize allocation or managed
exposure mutation. It does not add a production ownership store, enable
`DEAL_ENTRY_INOUT`, or connect the allocation protocol to `ManagedTradeOwner`.

## Context

The C2 candidate binds one allocation-gated CLOSE operation to a result-derived
INOUT deal, durable reversal evidence, the staged ownership association, and a
coherent pre/post managed-state transition. That is necessary but not
authoritative: both managed frontiers supplied to the candidate originate from
the same owner transition being proposed. The same broker observation can
therefore support more than one structurally coherent managed delta.

The missing fact is the ownership of the broker pre-position before the INOUT
deal. On a netting account, that position may combine several managed
`TradeId`s and external volume. An INOUT deal consumes the complete old
position and creates a new position in the opposite direction. Applying the
deal only to one selected managed trade while leaving another managed or
external share unaccounted would manufacture exposure that no longer exists at
the broker.

The close operation, its request payload, result binding, staged association,
and the broker close volume cannot independently solve this problem:

- the operation describes intent, not the provenance of existing position
  ownership;
- the staged association is deliberately volume-free;
- the C2 pre/post frontiers are the state transition under evaluation;
- broker volume establishes capacity but does not name a managed `TradeId` or
  create managed logical units; and
- selecting FIFO, LIFO, or pro-rata after observing the deal would introduce a
  new allocation policy and would still require durable pre-deal ownership.

## Decision

The authoritative source for a future INOUT close is a durable, complete
managed-position ownership frontier captured for the exact broker pre-position
observation. It is independent of the close candidate and may be produced only
from previously committed managed opening allocations, their subsequent
committed consumption history, and explicitly recorded unmanaged volume.

The exact source key is:

```text
(AccountKey,
 POSITION_IDENTIFIER,
 graph_instance_id,
 pre_graph_revision,
 pre_positions_revision)
```

The frontier records:

```text
account
position identifier
graph and positions revisions
symbol and pre-position direction
complete step-normalized broker position volume
source-set evidence digest
complete ownership entries
```

A managed ownership entry contains the managed `TradeId`, exact logical units,
the corresponding step-normalized broker volume, and the immutable OPEN
operation/allocation source identity. An unmanaged entry contains no managed
identity or logical units but retains positive broker volume and a durable
evidence identity. Entries must form an overflow-safe, exact partition of the
complete broker pre-position.

The frontier digest uses a versioned, canonical encoding of the complete key,
position identity, source evidence, and deterministically sorted entries. It is
a structural replay identity, not a cryptographic anti-forgery mechanism.

### First bounded authorization

The first eligible case is intentionally narrower than general netting
allocation. Authorization succeeds only when:

1. the C2 candidate and durable `BrokerReversalRecord` agree on account,
   operation, deal, staged ownership, and broker provenance;
2. the ownership source reloads the exact frontier keyed by the reversal's
   pre-position provenance;
3. symbol, position identifier, direction, normalization, and complete volume
   exactly match the reversal pre-position and `broker_close_leg`;
4. every frontier entry belongs to the candidate's `TradeId` and derives from
   an earlier managed operation;
5. no other managed owner and no unmanaged/external residue is present;
6. the source logical-unit sum equals the candidate's pre-close open exposure,
   close intent, and positive delta;
7. the candidate is a complete close: post-close open exposure is zero, the
   close obligation is satisfied, and the close slice is filled; and
8. mapping the source logical-unit sum into the frontier's broker normalization
   exactly equals the complete broker close leg.

This establishes exclusive ownership before validating the resulting managed
delta. Managed units originate from committed ownership entries; they are not
copied or reverse-derived from broker volume.

### Fail-closed cases

The authorization is unavailable when any of the following is true:

- the durable frontier is missing, malformed, incomplete, or cannot be read;
- its observation revisions or broker position identity differ from the
  reversal's exact pre-state;
- the position contains another managed `TradeId`;
- any explicitly unmanaged or external residue remains;
- the candidate closes only part of the managed owner;
- an ownership entry is not backed by an earlier OPEN source identity;
- the source arithmetic, broker normalization, or canonical digest is invalid;
  or
- the candidate, deal, and durable reversal identities disagree.

Mixed managed ownership is not resolved by choosing one entry. Because INOUT
consumes the complete old position, supporting that case requires one atomic
multi-owner transition and an explicit allocation policy. External mutation
requires a new complete ownership frontier; stale ownership must never be
patched from the close observation.

### Durability and fencing requirements

A future production source must:

- publish ownership entries only from committed opening allocations and update
  them only through committed position-consumption facts;
- serialize frontier publication and consumption in the same account-scoped
  fencing domain as journal and allocation writes;
- preserve immutable historical frontiers for exact restart validation;
- reject stale writers through the publication boundary, not through a token
  snapshot alone;
- advance the frontier when any committed managed allocation or an observed
  external mutation changes the broker position; and
- make an exact historical lookup possible without refreshing broker state
  during restart.

The allocation ledger remains the only consumer of a reversal close leg. A
future authorization may be retained as source evidence for staged admission,
but does not itself reserve capacity, publish a settlement, advance the
journal, or mutate exposure.

## Current implementation boundary

This prerequisite adds a private executable contract only:

- data shapes for a complete position-ownership frontier;
- an abstract durable source lookup;
- canonical frontier identity;
- an exact-exclusive INOUT attribution assessment; and
- positive and negative fixtures for the bounded rules above.

There is deliberately no production implementation of the durable source and
no production caller of the resulting authorization. Consequently the
allocation store and `ManagedTradeOwner` still reject INOUT publication. A
memory source in the contract test demonstrates validation behavior only; it is
not evidence that authoritative ownership exists in production.

## Deferred work

- durable opening-allocation and ownership-frontier publication;
- crash-safe frontier advancement and historical recovery;
- partial INOUT close attribution;
- atomic multi-`TradeId` settlement for mixed managed positions;
- policy for FIFO, LIFO, pro-rata, or explicitly reserved virtual lots;
- attribution and ownership of `broker_reverse_open_leg`;
- production staged-admission and owner-loop integration; and
- migration of positions that predate the ownership ledger.

## Verification boundary

The prerequisite contract must verify:

- one complete pre-position owned exclusively by the target `TradeId` can
  authorize an exact full-close candidate;
- missing, invalid, incomplete, stale, and I/O-failed frontiers fail closed;
- competing managed owners and unmanaged residue fail closed;
- a partial managed close cannot use exclusive INOUT authorization;
- a foreign broker deal cannot reuse another candidate; and
- canonical frontier identity is independent of entry order.

Later production slices must add real durable-store restart fixtures before
any authorization can be consumed by allocation or exposure code.

# ADR-0030: Durable reversal attribution model

## Status

Accepted as the design boundary for `DEAL_ENTRY_INOUT`. The broker-only durable
record and immutable file-store contract are implemented; managed allocation
and runtime settlement remain deferred.

## Context

Managed settlement currently rejects `DEAL_ENTRY_INOUT` deliberately. An INOUT
deal reports one broker execution volume even though a netting reversal may
contain two logical legs:

```text
broker_close_leg        = broker volume that removes the prior opposite exposure
broker_reverse_open_leg = broker volume that creates the new exposure
```

Counting the broker volume as a pure OPEN or CLOSE fill would either invent
exposure or erase exposure without proving which logical trade owned it. The
current fail-closed behavior is therefore retained until the decomposition and
its durable evidence are defined. Broker decomposition and managed ownership
allocation are separate facts: a broker position may contain exposure owned by
several logical trades or by an external actor.

## Decision

A broker INOUT decomposition is a derived durable fact, not a property inferred
from the deal row alone. A future reconciliation result must carry a complete
broker reversal record with at least:

```text
account key and account margin mode
broker deal ticket and order ticket
DEAL_ENTRY = INOUT
DEAL_POSITION_ID
pre-deal POSITION_IDENTIFIER, direction, and volume
post-deal POSITION_IDENTIFIER, direction, and volume
DEAL_VOLUME
broker_close_leg volume
broker_reverse_open_leg volume
observation graph instance and revisions
```

The record is accepted only after the before/after observations and deal
identity are covered by one provenance chain. This record proves only the
broker-level decomposition. It does not decide which `TradeId` owns either
leg, and it must not require the two broker legs to equal one managed trade's
allocation. A later managed allocation layer may correlate the record with an
operation, but ownership is accepted only with its own ledger proof.

The broker decomposition record and any later managed allocation are committed
as separate durable facts with an explicit link. A later snapshot or restart
must replay the broker record and the allocation link, not derive a second pair
of legs from the same deal ticket.

The first implementation slice provides `BrokerReversalObservation`,
`BrokerReversalRecord`, and `DurableBrokerReversalStore`. The observation
factory accepts already step-normalized exact volumes, derives the two broker
legs once, and rejects contradictory evidence before persistence. The file
store uses an immutable account/deal key, a checksummed versioned envelope,
atomic replacement, duplicate-idempotent commit, conflict detection, and
all-or-nothing scan for restart recovery.

The runtime integration accepts two sequential `ObservationSample` values from
one `ObservationGraph`. The post sample must carry the explicit post-action
history query window containing the target deal; both samples must carry authoritative
position domains, and the reversal collection records explicit domain order:
positions before the complete pre history query, then the complete post history
query before post positions. The adapter requires the target deal to be absent
from the complete pre window and present exactly once in the complete post
window, one matching pre/post position, continuous identity, complete deal
fields, and an exact symbol-volume normalization proof. Only after
`derive_broker_reversal_record()` succeeds does it call
`DurableBrokerReversalStore::commit()`. This path does not infer a managed
`TradeId`, mutate managed exposure, or choose an ownership allocation policy.

The model is account-mode aware:

- On a netting account, `DEAL_POSITION_ID` together with the pre/post
  `POSITION_IDENTIFIER`, position direction, and volume are authoritative for
  detecting one broker reversal. A position mutation by another logical trade
  or an external actor can make managed ownership ambiguous even when the
  broker decomposition itself is provable.
- On a hedging account, independent position identities remain separate. An
  INOUT row that cannot be bound to one managed position is ambiguous; it is
  never merged into a logical trade merely because symbol or volume matches.

No FIFO, LIFO, or pro-rata allocation policy is selected by this ADR. Those
policies are a later managed allocation decision after the broker-level
reversal proof is executable.

## Invariants

For an accepted broker decomposition, the following must hold:

1. `broker_close_leg + broker_reverse_open_leg == DEAL_VOLUME` in the broker's
   volume domain. Conversion to the private integer `Volume` is allowed only
   when the configured symbol step proves an exact representable value.
2. `broker_close_leg` matches the reduction of the pre-deal broker position and
   does not exceed that broker position's volume.
3. `broker_reverse_open_leg` matches the post-reversal position delta after the
   proven close leg is accounted for; an unexplained residual is ambiguous.
4. The position direction flips and the pre/post position evidence is
   consistent with the selected account mode.
5. The pre- and post-deal `POSITION_IDENTIFIER` are the same continuous
   identity, and `DEAL_POSITION_ID` equals that identifier. A generic symbol,
   ticket, or order match is insufficient; identity replacement is outside
   this bounded slice.
6. The decomposition is bound to one account, deal ticket, and graph
   provenance chain. Applying the same deal ticket or durable broker record
   twice is a no-op or a fail-closed conflict; it cannot increase either leg
   twice.
7. Recovery replays the durable broker record and never re-sends the side
   effect or recomputes a different decomposition from a later snapshot.

Managed ownership is a separate future allocation contract. It may establish
`managed_close_leg <= broker_close_leg` and
`managed_reverse_open_leg <= broker_reverse_open_leg`, but it need not consume
the full broker volume. For example, a broker BUY position of 5 may contain
only 2 units owned by one managed trade; a SELL INOUT deal of 6 can have a
broker close leg of 5 and reverse-open leg of 1 while that trade's proven
managed close allocation is at most 2. Without an explicit ledger proof, the
remaining broker legs stay unallocated and the managed operation remains
unresolved.

If any invariant or provenance requirement cannot be proven, the operation
remains `reconciling`/ambiguous and managed exposure is unchanged. In
particular, the implementation must not fall back to treating INOUT as `IN`
or `OUT`.

## Consequences

- The existing managed owner behavior is correct and remains fail-closed until
  this model has an executable implementation.
- A durable reversal record needs stronger before/after position evidence than
  the current entry/exit aggregate helpers provide.
- Netting allocation policy is isolated from reversal decomposition; it can be
  chosen after the proof boundary is tested.
- The public `TradeManager`, generic observation graph, and C ABI remain
  unchanged by this design slice.

## Verification boundary

The broker-only implementation now has focused regressions for:

- a proven netting reversal where the two broker legs exactly reconstruct
  `DEAL_VOLUME` and match pre/post position evidence;
- an external or concurrent position mutation between the two snapshots;
- duplicate refresh and restart replay of one deal ticket;
- hedging observations with independent position identities; and
- incomplete or contradictory provenance, which must remain unresolved; and
- duplicate commit, conflicting decomposition, and restart load of one durable
  broker record.

The runtime adapter now connects fresh `ObservationGraph` evidence to this
record without changing managed exposure. Its regressions cover:

- same-graph, revision-ordered pre/post samples with complete history windows,
  explicit positions/history ordering, and target absence-before/presence-after;
- foreign-graph samples, duplicate position identities, non-INOUT deals, and
  non-step-normalized volumes remaining unresolved;
- target history evidence already present in the pre window remaining unresolved;
- a deal observed between post-position and post-history collection remaining
  unresolved because the post order is invalid.

The later managed allocation slice must still add regressions for:

- a broker reversal whose ownership is split across managed and external
  exposure, leaving managed allocation unresolved;
- an external or concurrent position mutation between the two snapshots; and
- hedging observations with independent position identities.

Until the later managed allocation proof exists, managed settlement remains
fail-closed for `DEAL_ENTRY_INOUT` as specified by
[ADR-0026](0026-managed-trade-reconciliation-settlement.md) and
[ADR-0029](0029-durable-exit-settlement.md).

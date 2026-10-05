# ADR-0030: Durable reversal attribution model

## Status

Accepted as the design boundary for a future `DEAL_ENTRY_INOUT` implementation;
runtime and ledger implementation remain deferred.

## Context

Managed settlement currently rejects `DEAL_ENTRY_INOUT` deliberately. An INOUT
deal reports one broker execution volume even though a netting reversal may
contain two logical legs:

```text
close_leg        = volume that removes the prior opposite exposure
reverse_open_leg = volume that creates the new exposure
```

Counting the broker volume as a pure OPEN or CLOSE fill would either invent
exposure or erase exposure without proving which logical trade owned it. The
current fail-closed behavior is therefore retained until the decomposition and
its durable evidence are defined.

## Decision

An INOUT attribution is a derived durable fact, not a property inferred from
the deal row alone. A future reconciliation result must carry a complete
reversal attribution record with at least:

```text
account key and account margin mode
managed TradeId / OperationId
broker deal, order, and position identity
pre-deal position identity, direction, and volume
post-deal position identity, direction, and volume
broker INOUT volume
close_leg volume
reverse_open_leg volume
observation graph instance and revisions
```

The record is accepted only after the before/after observations and deal
identity are covered by one provenance chain. The managed state transition and
the durable attribution record are committed together. A later snapshot or
restart must replay that record, not derive a second pair of legs from the same
deal ticket.

The model is account-mode aware:

- On a netting account, the pre/post position identity and direction are
  authoritative for detecting a reversal. A position mutation by another
  logical trade or an external actor makes ownership ambiguous unless the
  ledger has an explicit allocation proof.
- On a hedging account, independent position identities remain separate. An
  INOUT row that cannot be bound to one managed position is ambiguous; it is
  never merged into a logical trade merely because symbol or volume matches.

No FIFO, LIFO, or pro-rata allocation policy is selected by this ADR. Those
  policies are a later ledger decision after the single-reversal proof is
  executable.

## Invariants

For an accepted reversal attribution, the following must hold:

1. `close_leg + reverse_open_leg == broker INOUT volume` in the broker's
   volume domain. Conversion to the private integer `Volume` is allowed only
   when the configured symbol step proves an exact representable value.
2. `close_leg <= previously attributed opposite exposure`.
3. `reverse_open_leg` is never created from an un-attributed remainder. Any
   remainder requires an explicit observation/proof or remains unresolved.
4. The pre/post position delta, deal volume, and two legs agree for the
   selected account mode; contradictory snapshots are ambiguous.
5. The attribution is bound to one account, deal ticket, operation identity,
   and position identity. A matching symbol or order alone is insufficient.
6. Applying the same deal ticket or durable attribution record twice is a
   no-op or a fail-closed conflict; it cannot increase either leg twice.
7. Recovery replays the durable attribution record and never re-sends the
   side effect or recomputes a different decomposition from a later snapshot.

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

The implementation slice following this ADR must add focused regressions for:

- a proven netting reversal where the two legs exactly reconstruct broker
  volume;
- a reversal whose close leg exceeds attributed prior exposure;
- an external or concurrent position mutation between the two snapshots;
- duplicate refresh and restart replay of one deal ticket;
- hedging observations with independent position identities; and
- incomplete or contradictory provenance, which must remain unresolved.

Until those tests and a durable record schema exist, `DEAL_ENTRY_INOUT` stays
rejected by the managed settlement boundary as specified by
[ADR-0026](0026-managed-trade-reconciliation-settlement.md) and
[ADR-0029](0029-durable-exit-settlement.md).

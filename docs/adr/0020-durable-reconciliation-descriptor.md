# ADR-0020: Durable reconciliation descriptor before dispatch

## Status

Accepted for the native durable-dispatch and recovery layer.

## Decision

Every operation must persist an immutable reconciliation descriptor before the
`dispatching` barrier can be opened. The descriptor binds the operation to:

- its `AccountKey`;
- the observation graph instance and all domain revisions in the pre-dispatch
  baseline;
- the explicit presence/absence predicates used to attribute a later broker
  effect, including whether the identity was present before dispatch and the
  expected causal transition; and
- the lifecycle state that confirmed evidence is allowed to settle.

Broker-assigned identities are not required to exist before `order_send`. A
predicate may instead carry a non-zero client correlation id with a zero broker
ticket. Such a predicate is durable and valid, but remains pending until a
trusted transport result binds the broker ticket; an unknown ticket can never
confirm either presence or absence by itself. Known-ticket predicates retain
the same explicit baseline transition contract, preventing a pre-existing
ticket from being mistaken for a post-dispatch effect.

Ticket enrichment is not caller-authoritative. The journal stores a separate
single-assignment `correlation_id -> broker_ticket` binding. Only the private
one-shot backend can create it, and only as part of the same durable compare-
and-commit that stores the complete broker result. Repeating the same binding
is therefore a recovery property of one record, not a second write after the
result. A different ticket for an already-bound correlation is rejected before
the commit. Recovery restores the result and bindings together before a worker
reconstructs its request.

The descriptor is stored in the same compare-and-committed operation record as
the dispatch intent. A record at or beyond `dispatching` without a valid
descriptor is malformed and is rejected during recovery. A reconciliation
worker must either use the exact durable descriptor or provide a request that
matches it byte-for-byte in semantic fields; caller-supplied predicates cannot
silently broaden or replace the durable contract. The one controlled enrichment
is replacing a zero broker ticket with a non-zero ticket while retaining the
same non-zero client correlation id. This resolves a pre-send unknown identity
from a trusted broker result without changing the causal contract.

Before opening `dispatching`, `DispatchAdmissionBarrier` requires the
descriptor baseline to match the admission graph account, graph instance, and
every domain revision exactly. Every known-ticket predicate is then checked
against the actual graph contents at that baseline; a fabricated
`baseline_present` value or a stale baseline rejects admission. A baseline
absence is accepted only when its domain was actually observed. For a history
absence (including a known-ticket presence expected to appear later), the
descriptor must carry a valid history window and the graph must prove complete
coverage of that window no later than the captured baseline revision. A
history cache that has not queried the relevant window cannot be used to infer
absence.

The descriptor is evidence provenance, not a consistency proof. It does not
make a recovered graph baseline current, and it does not authorize a resend.
After restart, the owner must rebuild current observations and reconcile only
against the durable predicates. Because graph instance ids and domain
revisions are process-local, a worker reconstructed from a durable descriptor
always re-anchors its in-memory baseline to the current graph before its first
refresh. This forces fresh post-restart evidence even if an instance id is
reused, without silently changing the durable provenance contract. Deadline
and event-gap hints stay ephemeral owner-loop inputs and are never persisted as
evidence.

## Consequences

- A crash after the dispatch barrier cannot lose the information needed to
  attribute a later order, deal, position, or active-order observation.
- New OPEN/LIMIT/STOP operations can cross the durable barrier before the
  broker assigns an order/deal/position ticket, without weakening fail-closed
  reconciliation.
- Recovery fails closed for legacy or corrupted post-dispatch records that do
  not carry the descriptor.
- The descriptor is persisted before admission, so the admission barrier never
  creates a non-resendable operation with an unknown reconciliation contract.
- A direct recovery worker can reconstruct its request from durable data, while
  a caller-provided request with different predicates or baseline revisions is
  rejected.
- Result bytes and broker-identity bindings are one atomic journal mutation;
  recovery cannot observe a persisted result without the bindings extracted
  from that same validated result.
- Unobserved active/position domains and uncovered history windows fail closed
  at admission instead of allowing a later cache refresh to masquerade as a
  post-dispatch effect.

## Verification

The dispatch, file-store, one-shot backend, and operation-worker tests cover
descriptor persistence, reopen/recovery, missing-descriptor rejection, direct
worker reconstruction, mismatch rejection, atomic result-plus-binding
recovery, and fail-closed admission for unobserved domains or uncovered
history windows. The file-store format retains legacy pre-dispatch records for
compatibility; such records cannot advance to `dispatching` until a descriptor
is durably attached.

# ADR-0007: Account-scoped observation graph

## Status

Accepted. This slice implements the read-only graph foundation after typed
trade snapshots and before the durable dispatch journal.

## Decision

`mt5bridge::ObservationGraph` is a lightweight C++ consumer-side graph. It
accepts an `ObservationBatch` containing one `AccountKey`, explicit observed
domain bits, and any combination of active orders, positions, history orders,
and history deals. It never calls MetaTrader, owns a runtime, or submits
`order_send`.

The account key is `(server, login)` with both fields required; `login == 0`
is invalid. `make_account_key()` also requires the account server/login
`known_fields` bits. An unbound graph adopts the first valid batch; a bound
graph rejects a different account atomically with `account_mismatch`. A batch
with an empty server, zero login, zero primary ticket, duplicate primary
ticket, missing graph-identity `known_fields`, or missing/invalid history
coverage is rejected without changing the graph or its revision.

For graph identity, active/history orders require `ORDER_TICKET` and
`ORDER_POSITION_ID`, positions require `POSITION_TICKET` and
`POSITION_IDENTIFIER`, and deals require `DEAL_TICKET`, `DEAL_ORDER`, and
`DEAL_POSITION_ID`. A history record also requires its native time field when
the batch supplies a coverage window. A zero value remains valid when its
corresponding known bit is set; the graph never treats zero as proof that a
field was absent.

The `active_orders` and `positions` domains are authoritative full snapshots:
when their domain bit is set, the corresponding namespace is replaced and an
empty vector means authoritative empty. An omitted domain is unchanged. The
history domains are positive evidence: records are upserted by primary MT5
ticket. A history window is optional for non-empty, possibly filtered evidence;
when supplied it must describe an account-wide unfiltered query and is retained
as revision-tagged coverage. Entries from one revision may be coalesced, but
coverage from different revisions is never merged without provenance. An empty
history result must include a valid window, or it is rejected because it proves
nothing.
Active orders and history orders remain separate namespaces. Position `ticket`
and `identifier` remain distinct. Read methods return deterministic
ticket-sorted copies and expose provenance-preserving links:

```text
ORDER_POSITION_ID / POSITION_IDENTIFIER
DEAL_ORDER         -> active or history order ticket
DEAL_POSITION_ID   -> position identifier
```

Active and history order links are separate methods; no link API combines two
namespaces into an untagged vector.

Every accepted batch advances a monotonic global revision. Each domain also
records the revision of its last accepted observation; an omitted domain keeps
its previous domain revision. History coverage is stored as
`ObservationCoverage{window, revision}`. Coverage from different revisions is
never merged into an untagged range. `history_*_covered(window, since_revision)`
counts only coverage with `revision > since_revision`, so it can be used for a
baseline-aware negative-evidence proof. Positive history evidence without a
window updates the history domain revision but does not prove absence over any
time range.

`clear_evidence()` removes records and coverage, retains the account scope, and
resets all domain freshness revisions to zero. Staging uses temporary maps,
coverage vectors, and metadata; if staging allocation fails, no graph state or
revision is committed. The graph is single-owner state and is not internally
thread-safe.

The graph deliberately does not invent `TradeGroupId`, `TradeId`,
`OperationId`, `request_id`, certainty, or lifecycle state. Those are managed
domain/journal concepts and may only be associated by a later reconciliation
layer with explicit evidence and account/session scope.

## Consequences

- Reordered MT5 arrays cannot change graph identity or link results.
- Account switches cannot mix evidence from two terminals.
- A later journal/reconciliation worker can consume deterministic snapshots,
  domain freshness, and baseline-aware coverage without adding another Python
  or C ABI implementation.
- Persistence, deadlines, transaction hints, and side effects remain outside
  this header-only foundation and are intentionally deferred to the next
  stage.

## Verification

`tests/reconciliation_graph_test.cpp` covers first-account binding and
known-field validation, authoritative empty snapshots, omitted domains,
domain revision freshness, sorted provenance-preserving links, same-ticket
history upsert, revision-tagged coverage and baseline proofs, account mismatch
isolation, duplicate/zero/invalid evidence, and revision/clear semantics.

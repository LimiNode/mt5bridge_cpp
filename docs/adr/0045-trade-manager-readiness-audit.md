# ADR-0045: Bounded TradeManager readiness audit

Status: Proposed audit and implementation boundary  
Date: 2026-11-06

## Context

The dispatch, reconciliation, and managed-owner mechanisms are implemented as
private C++ seams. They deliberately stop short of a public asynchronous
`TradeManager`: no caller can currently create a managed trade through the
stable DLL ABI, and no component is allowed to turn an uncertain broker call
into an implicit retry. This audit records what is already usable and the
smallest missing layer for one constrained managed trade.

The audit is based on the current source and focused Release tests, including
the durable journal, one-shot backend, operation reconciliation worker, and
managed owner tests. It does not authorize live or demo broker activity.

## Existing path

The private owner loop already has this bounded path:

1. `ManagedTradeOwner::prepare_open`, `prepare_close`, or `prepare_cancel`
   validates the private logical state and persists one durable operation.
2. `DispatchAdmissionBarrier::admit` checks account identity, environment
   coverage, graph revision, and the continuously-held account lease before
   entering durable `dispatching`.
3. `OneShotDispatchBackend::execute` rechecks the account and fencing token,
   transitions to `submitting`, invokes the transport once, and persists the
   raw result plus reconciliation bindings atomically.
4. `ManagedTradeOwner::execute_pending` converts transport uncertainty into a
   durable non-resendable reconciliation state.
5. `OperationReconciliationWorker` refreshes authoritative observations. The
   owner applies OPEN, CLOSE, and CANCEL state transitions only after the
   worker returns trusted provenance and the corresponding durable journal CAS
   succeeds.
6. The existing recovery functions reconstruct private state from a complete
   journal scan; unresolved or contradictory records remain fail-closed.

This path is private and caller-driven. It is not a public manager, scheduler,
or callback API.

## Readiness matrix

| Concern | Current state | Boundary for the first MVP |
| --- | --- | --- |
| Durable operation identity and lifecycle | Implemented and tested | Reuse unchanged |
| Single-send admission and fencing | Implemented and tested | Reuse unchanged |
| Python `order_send` transport | Implemented as private adapter | Bind only through an internal owner factory |
| OPEN/CLOSE/CANCEL settlement | Implemented in private owner seams | Expose one constrained orchestration path |
| Restart reconstruction | Implemented for durable journal/owner state | Add startup scan and unresolved-state reporting |
| Account and symbol capability policy | Available as observations, not a manager policy | Add an explicit whitelist and volume-step validator |
| Scheduling and deadlines | Caller-driven only | Add one owner-loop scheduler; no implicit resend |
| State notifications | Result structs only | Add bounded callback/event delivery on the owner loop |
| B3D opening proof | Separate private producer; production exposure remains off | Keep out of the first MVP until independently approved |
| Public asynchronous `TradeManager` | Missing | Add only after the internal path has an integration test |

## First usable MVP boundary

The first production-facing slice may support exactly one managed `TradeId` per
account and one isolated symbol at a time. It may support:

- an explicit OPEN with a bounded volume and a validated volume step;
- an explicit CLOSE of that same managed trade after authoritative ownership is
  proven;
- CANCEL of a still-pending OPEN; and
- restart recovery that reports `awaiting_reconciliation` or `ambiguous` rather
  than sending again.

The MVP must reject mixed netting ownership, external residue, partial or
unattributed position changes, `INOUT` reversal, FIFO/LIFO/pro-rata allocation,
and unsupported filling/account modes. These are policy decisions, not reasons
to weaken the existing durable contracts.

The B3D opening proof is only evidence that one committed OPEN allocation is
attributable to a managed trade at a selected frontier. It does not prove that
the current broker position is still exclusively owned by that trade, authorize
a CLOSE, or authorize managed-exposure mutation. Those decisions require a
separate current-position ownership gate and remain fail-closed in this MVP.

## Acceptance criteria for the next implementation batch

The next implementation PR should remain separate from B3D and should provide
the following testable seams:

1. A private owner factory that binds a real `Mt5PythonDispatchTransport` and
   the existing durable stores without changing the C ABI.
2. Capability admission that checks account mode, symbol trade permissions,
   filling policy, and the exact volume step before journal preparation.
3. A startup recovery coordinator that scans the journal before accepting a new
   intent and exposes every unresolved operation to the owner loop.
4. One integration test using a fake MT5 provider and real durable stores for
   `prepare -> admit -> send once -> reconcile -> settle`.
5. Restart regressions proving that an uncertain send is reconciled and never
   submitted a second time, and that CLOSE/CANCEL remain bound to the original
   `TradeId` and operation identity.
6. A test-only callback seam that reports state transitions without making
   callbacks from the Python/runtime thread.

No acceptance criterion permits a public blind retry, caller-supplied broker
identifiers, production `INOUT`, or managed exposure mutation without a
separately approved authoritative proof.

## Explicit non-goals

This decision does not add mixed-netting attribution, a universal broker
adapter, a risk engine, strategy execution, virtual stops, self-extracting
packaging, or live/demo trading automation. It also does not merge or alter
the pending B3D implementation PR.

## Evidence and anti-neuroslop check

Every claimed ready item above maps to an existing private implementation and
focused test target. The matrix intentionally marks the public manager,
scheduler, capability policy, startup coordinator, and callback delivery as
missing rather than inferring them from test-only seams. A passing fake
transport test is not evidence of a live terminal integration; that integration
requires a separately authorized bounded smoke test.

## Verification record

The local Windows Release build used for this audit ran 33/33 configured CTest
targets. The exact-head GitHub Actions run for the documentation commit ran
40/40 tests because CI also enables the runtime and Python-backed targets. Both
results are recorded separately; the local count is not presented as the CI
count.

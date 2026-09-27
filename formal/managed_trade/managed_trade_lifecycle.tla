--------------------------- MODULE managed_trade_lifecycle ---------------------------
EXTENDS Naturals, FiniteSets

(******************************************************************************)
(* A finite model of the managed lifecycle above one-shot dispatch.  The      *)
(* model deliberately keeps broker identifiers out of the state: dispatch    *)
(* and reconciliation own those raw identities, while this layer owns the    *)
(* logical trade, its close obligation, and the execution-plan decisions.     *)
(******************************************************************************)

CONSTANTS TargetVolume, MaxSliceVolume, MaxOperations, MaxObservationEpoch

PlanStates == {"running", "stopped"}
OperationKinds == {"none", "open", "close", "cancel"}
OperationStates == {"idle", "dispatching", "submitting", "reconciling",
                    "partially_filled", "filled", "cancelled", "rejected",
                    "ambiguous"}
BrokerOutcomes == {"none", "partial", "full", "rejected", "unknown"}
TerminalOperationStates == {"idle", "partially_filled", "filled", "cancelled",
                            "rejected"}
TradeStates == {"pending", "partially_open", "open", "reducing", "closing",
                "closed"}

VARIABLES planState, sliceCount, openVolume, pendingRemainderVolume,
          closeObligation, obligationEver, obligationSatisfied, observationEpoch,
          lastAttemptEpoch, operationId, operationKind, operationState,
          operationVolume, resultVolume, brokerOutcome, sendCount

vars == <<planState, sliceCount, openVolume, pendingRemainderVolume,
           closeObligation, obligationEver, obligationSatisfied, observationEpoch,
           lastAttemptEpoch, operationId, operationKind, operationState,
           operationVolume, resultVolume, brokerOutcome, sendCount>>

Init ==
    /\ planState = "running"
    /\ sliceCount = 0
    /\ openVolume = 0
    /\ pendingRemainderVolume = 0
    /\ closeObligation = FALSE
    /\ obligationEver = FALSE
    /\ obligationSatisfied = FALSE
    /\ observationEpoch = 0
    /\ lastAttemptEpoch = 0
    /\ operationId = 0
    /\ operationKind = "none"
    /\ operationState = "idle"
    /\ operationVolume = 0
    /\ resultVolume = 0
    /\ brokerOutcome = "none"
    /\ sendCount = 0

TradeState ==
    IF obligationSatisfied
    THEN "closed"
    ELSE IF (closeObligation /\ operationKind = "close" /\
             operationState \in {"dispatching", "submitting", "reconciling"})
         THEN "closing"
         ELSE IF closeObligation
              THEN "reducing"
              ELSE IF openVolume = 0
                   THEN "pending"
                   ELSE IF openVolume < TargetVolume
                        THEN "partially_open"
                        ELSE "open"

CanStartOperation == operationState \in TerminalOperationStates

StartOpenSlice(volume) ==
    /\ planState = "running"
    /\ CanStartOperation
    /\ operationKind = "none" \/ operationState # "ambiguous"
    /\ closeObligation = FALSE
    /\ openVolume < TargetVolume
    /\ pendingRemainderVolume = 0
    /\ operationId < MaxOperations
    /\ sliceCount < MaxOperations
    /\ volume \in 1..MaxSliceVolume
    /\ volume <= TargetVolume - openVolume
    /\ operationId' = operationId + 1
    /\ operationKind' = "open"
    /\ operationState' = "dispatching"
    /\ operationVolume' = volume
    /\ resultVolume' = 0
    /\ brokerOutcome' = "none"
    /\ sendCount' = 0
    /\ lastAttemptEpoch' = observationEpoch
    /\ sliceCount' = sliceCount + 1
    /\ UNCHANGED <<planState, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch>>

StartClose(volume) ==
    /\ closeObligation
    /\ ~obligationSatisfied
    /\ CanStartOperation
    /\ operationKind = "none" \/ operationState # "ambiguous"
    /\ openVolume > 0
    /\ pendingRemainderVolume = 0
    /\ operationId < MaxOperations
    /\ volume \in 1..openVolume
    /\ operationId' = operationId + 1
    /\ operationKind' = "close"
    /\ operationState' = "dispatching"
    /\ operationVolume' = volume
    /\ resultVolume' = 0
    /\ brokerOutcome' = "none"
    /\ sendCount' = 0
    /\ lastAttemptEpoch' = observationEpoch
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch>>

StartCancel ==
    /\ pendingRemainderVolume > 0
    /\ CanStartOperation
    /\ operationKind = "none" \/ operationState # "ambiguous"
    /\ operationId < MaxOperations
    /\ operationId' = operationId + 1
    /\ operationKind' = "cancel"
    /\ operationState' = "dispatching"
    /\ operationVolume' = pendingRemainderVolume
    /\ resultVolume' = 0
    /\ brokerOutcome' = "none"
    /\ sendCount' = 0
    /\ lastAttemptEpoch' = observationEpoch
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch>>

EnterSubmitting ==
    /\ operationState = "dispatching"
    /\ operationKind \in {"open", "close", "cancel"}
    /\ operationState' = "submitting"
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume, resultVolume, brokerOutcome, sendCount>>

BrokerFill(volume) ==
    /\ operationState = "submitting"
    /\ operationKind \in {"open", "close"}
    /\ volume \in 1..operationVolume
    /\ operationState' = "reconciling"
    /\ resultVolume' = volume
    /\ brokerOutcome' = IF volume = operationVolume THEN "full" ELSE "partial"
    /\ sendCount' = sendCount + 1
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume>>

BrokerCancelAccepted ==
    /\ operationState = "submitting"
    /\ operationKind = "cancel"
    /\ operationState' = "reconciling"
    /\ resultVolume' = 0
    /\ brokerOutcome' = "full"
    /\ sendCount' = sendCount + 1
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume>>

BrokerReject ==
    /\ operationState = "submitting"
    /\ operationKind \in {"open", "close", "cancel"}
    /\ operationState' = "reconciling"
    /\ resultVolume' = 0
    /\ brokerOutcome' = "rejected"
    /\ sendCount' = sendCount + 1
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume>>

BrokerUnknown ==
    /\ operationState = "submitting"
    /\ operationKind \in {"open", "close", "cancel"}
    /\ operationState' = "reconciling"
    /\ resultVolume' = 0
    /\ brokerOutcome' = "unknown"
    /\ sendCount' = sendCount + 1
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume>>

ReconcileOpenFull ==
    /\ operationKind = "open"
    /\ operationState = "reconciling"
    /\ brokerOutcome = "full"
    /\ resultVolume = operationVolume
    /\ openVolume + operationVolume <= TargetVolume
    /\ operationState' = "filled"
    /\ openVolume' = openVolume + operationVolume
    /\ pendingRemainderVolume' = 0
    /\ UNCHANGED <<planState, sliceCount, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume, resultVolume, brokerOutcome, sendCount>>

ReconcileOpenPartial ==
    /\ operationKind = "open"
    /\ operationState = "reconciling"
    /\ brokerOutcome = "partial"
    /\ resultVolume < operationVolume
    /\ openVolume + resultVolume <= TargetVolume
    /\ operationState' = "partially_filled"
    /\ openVolume' = openVolume + resultVolume
    /\ pendingRemainderVolume' = operationVolume - resultVolume
    /\ UNCHANGED <<planState, sliceCount, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume, resultVolume, brokerOutcome, sendCount>>

ReconcileOpenRejected ==
    /\ operationKind = "open"
    /\ operationState = "reconciling"
    /\ brokerOutcome = "rejected"
    /\ operationState' = "rejected"
    /\ obligationSatisfied' =
        (obligationSatisfied \/
         (closeObligation /\ openVolume = 0 /\ pendingRemainderVolume = 0))
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume, resultVolume, brokerOutcome, sendCount>>

ReconcileOpenUnknown ==
    /\ operationKind = "open"
    /\ operationState = "reconciling"
    /\ brokerOutcome = "unknown"
    /\ operationState' = "ambiguous"
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume, resultVolume, brokerOutcome, sendCount>>

ReconcileCloseFull ==
    /\ operationKind = "close"
    /\ operationState = "reconciling"
    /\ brokerOutcome = "full"
    /\ resultVolume = operationVolume
    /\ operationVolume <= openVolume
    /\ operationState' = "filled"
    /\ openVolume' = openVolume - operationVolume
    /\ obligationSatisfied' = (openVolume = operationVolume)
    /\ UNCHANGED <<planState, sliceCount, pendingRemainderVolume,
                    closeObligation, observationEpoch, lastAttemptEpoch,
                    operationId, operationKind, operationVolume,
                    resultVolume, brokerOutcome, sendCount>>

ReconcileClosePartial ==
    /\ operationKind = "close"
    /\ operationState = "reconciling"
    /\ brokerOutcome = "partial"
    /\ resultVolume < operationVolume
    /\ resultVolume > 0
    /\ operationVolume <= openVolume
    /\ operationState' = "partially_filled"
    /\ openVolume' = openVolume - resultVolume
    /\ obligationSatisfied' = FALSE
    /\ UNCHANGED <<planState, sliceCount, pendingRemainderVolume,
                    closeObligation, observationEpoch, lastAttemptEpoch,
                    operationId, operationKind, operationVolume,
                    resultVolume, brokerOutcome, sendCount>>

ReconcileCloseRejected ==
    /\ operationKind = "close"
    /\ operationState = "reconciling"
    /\ brokerOutcome = "rejected"
    /\ operationState' = "rejected"
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume, resultVolume, brokerOutcome, sendCount>>

ReconcileCloseUnknown ==
    /\ operationKind = "close"
    /\ operationState = "reconciling"
    /\ brokerOutcome = "unknown"
    /\ operationState' = "ambiguous"
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume, resultVolume, brokerOutcome, sendCount>>

ReconcileCancelFull ==
    /\ operationKind = "cancel"
    /\ operationState = "reconciling"
    /\ brokerOutcome = "full"
    /\ operationState' = "cancelled"
    /\ pendingRemainderVolume' = 0
    /\ obligationSatisfied' =
        (obligationSatisfied \/
         (closeObligation /\ openVolume = 0))
    /\ UNCHANGED <<planState, sliceCount, openVolume, closeObligation,
                    observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume, resultVolume, brokerOutcome, sendCount>>

ReconcileCancelRejected ==
    /\ operationKind = "cancel"
    /\ operationState = "reconciling"
    /\ brokerOutcome = "rejected"
    /\ operationState' = "rejected"
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume, resultVolume, brokerOutcome, sendCount>>

ReconcileCancelUnknown ==
    /\ operationKind = "cancel"
    /\ operationState = "reconciling"
    /\ brokerOutcome = "unknown"
    /\ operationState' = "ambiguous"
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationVolume, resultVolume, brokerOutcome, sendCount>>

ObservePendingRemainder(volume) ==
    /\ pendingRemainderVolume > 0
    /\ volume \in 1..pendingRemainderVolume
    /\ openVolume + volume <= TargetVolume
    /\ openVolume' = openVolume + volume
    /\ pendingRemainderVolume' = pendingRemainderVolume - volume
    /\ UNCHANGED <<planState, sliceCount, closeObligation,
                    obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationState, operationVolume, resultVolume,
                    brokerOutcome, sendCount>>

CreateCloseObligation ==
    /\ ~closeObligation
    /\ ~obligationSatisfied
    /\ (openVolume > 0 \/ pendingRemainderVolume > 0)
    /\ closeObligation' = TRUE
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, obligationSatisfied,
                    observationEpoch, lastAttemptEpoch, operationId,
                    operationKind, operationState, operationVolume,
                    resultVolume, brokerOutcome, sendCount>>

StopPlan ==
    /\ planState = "running"
    /\ planState' = "stopped"
    /\ UNCHANGED <<sliceCount, openVolume, pendingRemainderVolume,
                    closeObligation, obligationSatisfied, observationEpoch,
                    lastAttemptEpoch, operationId, operationKind,
                    operationState, operationVolume, resultVolume,
                    brokerOutcome, sendCount>>

FreshSnapshot(open, pending) ==
    /\ operationState = "ambiguous"
    /\ observationEpoch < MaxObservationEpoch
    /\ open \in 0..TargetVolume
    /\ pending \in 0..TargetVolume
    /\ open + pending <= TargetVolume
    /\ openVolume' = open
    /\ pendingRemainderVolume' = pending
    /\ observationEpoch' = observationEpoch + 1
    /\ UNCHANGED <<planState, sliceCount, closeObligation,
                    obligationSatisfied, lastAttemptEpoch, operationId,
                    operationKind, operationState, operationVolume,
                    resultVolume, brokerOutcome, sendCount>>

AcknowledgeAmbiguous ==
    /\ operationState = "ambiguous"
    /\ observationEpoch > lastAttemptEpoch
    /\ operationState' = "idle"
    /\ operationKind' = "none"
    /\ operationVolume' = 0
    /\ resultVolume' = 0
    /\ brokerOutcome' = "none"
    /\ sendCount' = 0
    /\ obligationSatisfied' =
        (obligationSatisfied \/
         (closeObligation /\ openVolume = 0 /\ pendingRemainderVolume = 0))
    /\ UNCHANGED <<planState, sliceCount, openVolume,
                    pendingRemainderVolume, closeObligation,
                    observationEpoch,
                    lastAttemptEpoch, operationId>>

LifecycleNext ==
    \/ \E volume \in 1..MaxSliceVolume : StartOpenSlice(volume)
    \/ \E volume \in 1..TargetVolume : StartClose(volume)
    \/ StartCancel
    \/ EnterSubmitting
    \/ \E volume \in 1..TargetVolume : BrokerFill(volume)
    \/ BrokerCancelAccepted
    \/ BrokerReject
    \/ BrokerUnknown
    \/ ReconcileOpenFull
    \/ ReconcileOpenPartial
    \/ ReconcileOpenRejected
    \/ ReconcileOpenUnknown
    \/ ReconcileCloseFull
    \/ ReconcileClosePartial
    \/ ReconcileCloseRejected
    \/ ReconcileCloseUnknown
    \/ ReconcileCancelFull
    \/ ReconcileCancelRejected
    \/ ReconcileCancelUnknown
    \/ \E volume \in 1..TargetVolume : ObservePendingRemainder(volume)
    \/ CreateCloseObligation
    \/ StopPlan
    \/ \E open \in 0..TargetVolume, pending \in 0..TargetVolume :
           FreshSnapshot(open, pending)
    \/ AcknowledgeAmbiguous

Next ==
    /\ LifecycleNext
    /\ obligationEver' = (obligationEver \/ closeObligation')

TypeOK ==
    /\ planState \in PlanStates
    /\ sliceCount \in 0..MaxOperations
    /\ openVolume \in 0..TargetVolume
    /\ pendingRemainderVolume \in 0..TargetVolume
    /\ closeObligation \in BOOLEAN
    /\ obligationEver \in BOOLEAN
    /\ obligationSatisfied \in BOOLEAN
    /\ observationEpoch \in 0..MaxObservationEpoch
    /\ lastAttemptEpoch \in 0..MaxObservationEpoch
    /\ operationId \in 0..MaxOperations
    /\ operationKind \in OperationKinds
    /\ operationState \in OperationStates
    /\ operationVolume \in 0..TargetVolume
    /\ resultVolume \in 0..TargetVolume
    /\ brokerOutcome \in BrokerOutcomes
    /\ sendCount \in 0..2
    /\ (operationState = "idle" => operationKind = "none")
    /\ (operationKind = "none" => operationState = "idle")
    /\ (operationState = "submitting" => operationVolume > 0)
    /\ (operationState = "reconciling" => sendCount = 1)
    /\ (brokerOutcome = "partial" => resultVolume > 0 /\ resultVolume < operationVolume)

NoOvershoot ==
    openVolume + pendingRemainderVolume <= TargetVolume

AtMostOneSendPerOperation ==
    sendCount <= 1

ObligationOnlyAfterExposureGone ==
    obligationSatisfied => openVolume = 0 /\ pendingRemainderVolume = 0

SatisfiedObligationIsDurable ==
    obligationSatisfied => closeObligation

CloseObligationIsDurable ==
    obligationEver => closeObligation

AmbiguousAttemptIsNonResendable ==
    operationState = "ambiguous" => sendCount = 1

SliceBound ==
    sliceCount <= MaxOperations

ClosedTradeHasNoExposure ==
    TradeState = "closed" =>
        obligationSatisfied /\ openVolume = 0 /\ pendingRemainderVolume = 0

CloseObligationHasReducingState ==
    closeObligation /\ ~obligationSatisfied /\ openVolume > 0 =>
        TradeState \in {"reducing", "closing"}

ZeroExposureObligationIsSatisfied ==
    closeObligation /\ openVolume = 0 /\ pendingRemainderVolume = 0 /\
        operationState \in TerminalOperationStates => obligationSatisfied

Spec == Init /\ [][Next]_vars

=============================================================================

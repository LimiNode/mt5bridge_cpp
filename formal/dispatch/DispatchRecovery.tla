------------------------------ MODULE DispatchRecovery ------------------------------
EXTENDS Naturals, FiniteSets

CONSTANTS Writers, Operations, MaxEpoch

States == {"created", "intent", "dispatching", "submitting",
           "result_persisted", "reconciling", "terminal"}
PreDispatch == {"created", "intent"}
BarrierStates == {"dispatching", "submitting", "result_persisted", "reconciling",
                  "terminal"}
BrokerEffects == {"none", "executed", "rejected"}

VARIABLES state, barrierEver, sendCount, brokerEffect, resultDurable,
          bindingsDurable, observationEvidence, leaseOwner, leaseEpoch,
          heldToken, crashed, dispatchWriter, dispatchEpoch, permitAvailable,
          volatileResult, sendWriter, sendEpoch

vars == <<state, barrierEver, sendCount, brokerEffect, resultDurable,
           bindingsDurable, observationEvidence, leaseOwner, leaseEpoch,
           heldToken, crashed, dispatchWriter, dispatchEpoch, permitAvailable,
           volatileResult, sendWriter, sendEpoch>>

Init ==
    /\ state = [o \in Operations |-> "created"]
    /\ barrierEver = [o \in Operations |-> FALSE]
    /\ sendCount = [o \in Operations |-> 0]
    /\ brokerEffect = [o \in Operations |-> "none"]
    /\ resultDurable = [o \in Operations |-> FALSE]
    /\ bindingsDurable = [o \in Operations |-> FALSE]
    /\ observationEvidence = [o \in Operations |-> FALSE]
    /\ leaseOwner = [o \in Operations |-> "none"]
    /\ leaseEpoch = [o \in Operations |-> 0]
    /\ heldToken = [w \in Writers |-> [o \in Operations |-> 0]]
    /\ crashed = [w \in Writers |-> FALSE]
    /\ dispatchWriter = [o \in Operations |-> "none"]
    /\ dispatchEpoch = [o \in Operations |-> 0]
    /\ permitAvailable = [o \in Operations |-> FALSE]
    /\ volatileResult = [w \in Writers |-> [o \in Operations |-> FALSE]]
    /\ sendWriter = [o \in Operations |-> "none"]
    /\ sendEpoch = [o \in Operations |-> 0]

AcquireLease(w, o) ==
    /\ w \in Writers
    /\ o \in Operations
    /\ crashed[w] = FALSE
    /\ leaseOwner[o] = "none"
    /\ leaseEpoch[o] < MaxEpoch
    /\ leaseOwner' = [leaseOwner EXCEPT ![o] = w]
    /\ leaseEpoch' = [leaseEpoch EXCEPT ![o] = @ + 1]
    /\ heldToken' = [heldToken EXCEPT ![w][o] = leaseEpoch[o] + 1]
    /\ UNCHANGED <<state, barrierEver, sendCount, brokerEffect, resultDurable,
                    bindingsDurable, observationEvidence, crashed,
                    dispatchWriter, dispatchEpoch, permitAvailable,
                    volatileResult, sendWriter, sendEpoch>>

Prepare(w, o) ==
    /\ w \in Writers
    /\ o \in Operations
    /\ crashed[w] = FALSE
    /\ state[o] = "created"
    /\ leaseOwner[o] = w
    /\ heldToken[w][o] = leaseEpoch[o]
    /\ state' = [state EXCEPT ![o] = "intent"]
    /\ UNCHANGED <<barrierEver, sendCount, brokerEffect, resultDurable,
                    bindingsDurable, observationEvidence, leaseOwner,
                    leaseEpoch, heldToken, crashed, dispatchWriter,
                    dispatchEpoch, permitAvailable, volatileResult,
                    sendWriter, sendEpoch>>

EnterDispatching(w, o) ==
    /\ w \in Writers
    /\ o \in Operations
    /\ crashed[w] = FALSE
    /\ state[o] = "intent"
    /\ leaseOwner[o] = w
    /\ heldToken[w][o] = leaseEpoch[o]
    /\ leaseEpoch[o] > 0
    /\ state' = [state EXCEPT ![o] = "dispatching"]
    /\ barrierEver' = [barrierEver EXCEPT ![o] = TRUE]
    /\ dispatchWriter' = [dispatchWriter EXCEPT ![o] = w]
    /\ dispatchEpoch' = [dispatchEpoch EXCEPT ![o] = leaseEpoch[o]]
    /\ permitAvailable' = [permitAvailable EXCEPT ![o] = TRUE]
    /\ UNCHANGED <<sendCount, brokerEffect, resultDurable, bindingsDurable,
                    observationEvidence, leaseOwner, leaseEpoch, heldToken,
                    crashed, volatileResult, sendWriter, sendEpoch>>

EnterSubmitting(w, o) ==
    /\ w \in Writers
    /\ o \in Operations
    /\ crashed[w] = FALSE
    /\ state[o] = "dispatching"
    /\ leaseOwner[o] = w
    /\ heldToken[w][o] = leaseEpoch[o]
    /\ permitAvailable[o] = TRUE
    /\ state' = [state EXCEPT ![o] = "submitting"]
    /\ UNCHANGED <<barrierEver, sendCount, brokerEffect, resultDurable,
                    bindingsDurable, observationEvidence, leaseOwner,
                    leaseEpoch, heldToken, crashed, dispatchWriter,
                    dispatchEpoch, permitAvailable, volatileResult,
                    sendWriter, sendEpoch>>

OrderSendKnown(w, o, effect) ==
    /\ w \in Writers
    /\ o \in Operations
    /\ effect \in BrokerEffects
    /\ crashed[w] = FALSE
    /\ state[o] = "submitting"
    /\ leaseOwner[o] = w
    /\ heldToken[w][o] = dispatchEpoch[o]
    /\ dispatchWriter[o] = w
    /\ dispatchEpoch[o] = leaseEpoch[o]
    /\ permitAvailable[o] = TRUE
    /\ sendCount[o] = 0
    /\ sendCount' = [sendCount EXCEPT ![o] = @ + 1]
    /\ brokerEffect' = [brokerEffect EXCEPT ![o] = effect]
    /\ permitAvailable' = [permitAvailable EXCEPT ![o] = FALSE]
    /\ volatileResult' = [volatileResult EXCEPT ![w][o] = TRUE]
    /\ sendWriter' = [sendWriter EXCEPT ![o] = w]
    /\ sendEpoch' = [sendEpoch EXCEPT ![o] = dispatchEpoch[o]]
    /\ UNCHANGED <<state, barrierEver, resultDurable, bindingsDurable,
                    observationEvidence, leaseOwner, leaseEpoch, heldToken,
                    crashed, dispatchWriter, dispatchEpoch>>

OrderSendUnknown(w, o, effect) ==
    /\ w \in Writers
    /\ o \in Operations
    /\ effect \in BrokerEffects
    /\ crashed[w] = FALSE
    /\ state[o] = "submitting"
    /\ leaseOwner[o] = w
    /\ heldToken[w][o] = dispatchEpoch[o]
    /\ dispatchWriter[o] = w
    /\ dispatchEpoch[o] = leaseEpoch[o]
    /\ permitAvailable[o] = TRUE
    /\ sendCount[o] = 0
    /\ sendCount' = [sendCount EXCEPT ![o] = @ + 1]
    /\ brokerEffect' = [brokerEffect EXCEPT ![o] = effect]
    /\ permitAvailable' = [permitAvailable EXCEPT ![o] = FALSE]
    /\ sendWriter' = [sendWriter EXCEPT ![o] = w]
    /\ sendEpoch' = [sendEpoch EXCEPT ![o] = dispatchEpoch[o]]
    /\ UNCHANGED <<state, barrierEver, resultDurable, bindingsDurable,
                    observationEvidence, leaseOwner, leaseEpoch, heldToken,
                    crashed, dispatchWriter, dispatchEpoch, volatileResult>>

PersistResult(w, o) ==
    /\ w \in Writers
    /\ o \in Operations
    /\ crashed[w] = FALSE
    /\ state[o] = "submitting"
    /\ sendCount[o] = 1
    /\ volatileResult[w][o] = TRUE
    /\ resultDurable[o] = FALSE
    /\ state' = [state EXCEPT ![o] = "result_persisted"]
    /\ resultDurable' = [resultDurable EXCEPT ![o] = TRUE]
    /\ bindingsDurable' = [bindingsDurable EXCEPT ![o] = TRUE]
    /\ volatileResult' = [volatileResult EXCEPT ![w][o] = FALSE]
    /\ UNCHANGED <<barrierEver, sendCount, brokerEffect, observationEvidence,
                    leaseOwner, leaseEpoch, heldToken, crashed, dispatchWriter,
                    dispatchEpoch, permitAvailable, sendWriter, sendEpoch>>

BeginReconciliation(o) ==
    /\ o \in Operations
    /\ state[o] = "result_persisted"
    /\ state' = [state EXCEPT ![o] = "reconciling"]
    /\ UNCHANGED <<barrierEver, sendCount, brokerEffect, resultDurable,
                    bindingsDurable, observationEvidence, leaseOwner,
                    leaseEpoch, heldToken, crashed, dispatchWriter,
                    dispatchEpoch, permitAvailable, volatileResult,
                    sendWriter, sendEpoch>>

Recover(o) ==
    /\ o \in Operations
    /\ state[o] \in {"dispatching", "submitting"}
    /\ state' = [state EXCEPT ![o] = "reconciling"]
    /\ UNCHANGED <<barrierEver, sendCount, brokerEffect, resultDurable,
                    bindingsDurable, observationEvidence, leaseOwner,
                    leaseEpoch, heldToken, crashed, dispatchWriter,
                    dispatchEpoch, permitAvailable, volatileResult,
                    sendWriter, sendEpoch>>

ObserveBroker(o) ==
    /\ o \in Operations
    /\ state[o] = "reconciling"
    /\ brokerEffect[o] = "executed"
    /\ observationEvidence' = [observationEvidence EXCEPT ![o] = TRUE]
    /\ UNCHANGED <<state, barrierEver, sendCount, brokerEffect, resultDurable,
                    bindingsDurable, leaseOwner, leaseEpoch, heldToken,
                    crashed, dispatchWriter, dispatchEpoch, permitAvailable,
                    volatileResult, sendWriter, sendEpoch>>

Resolve(o) ==
    /\ o \in Operations
    /\ state[o] = "reconciling"
    /\ resultDurable[o] \/ observationEvidence[o]
    /\ state' = [state EXCEPT ![o] = "terminal"]
    /\ UNCHANGED <<barrierEver, sendCount, brokerEffect, resultDurable,
                    bindingsDurable, observationEvidence, leaseOwner,
                    leaseEpoch, heldToken, crashed, dispatchWriter,
                    dispatchEpoch, permitAvailable, volatileResult,
                    sendWriter, sendEpoch>>

LoseLease(o) ==
    /\ o \in Operations
    /\ leaseOwner[o] # "none"
    /\ leaseOwner' = [leaseOwner EXCEPT ![o] = "none"]
    /\ UNCHANGED <<state, barrierEver, sendCount, brokerEffect, resultDurable,
                    bindingsDurable, observationEvidence, leaseEpoch, heldToken,
                    crashed, dispatchWriter, dispatchEpoch, permitAvailable,
                    volatileResult, sendWriter, sendEpoch>>

Crash(w) ==
    /\ w \in Writers
    /\ crashed[w] = FALSE
    /\ crashed' = [crashed EXCEPT ![w] = TRUE]
    /\ leaseOwner' = [o \in Operations |->
                         IF leaseOwner[o] = w THEN "none" ELSE leaseOwner[o]]
    /\ volatileResult' = [volatileResult EXCEPT ![w] = [o \in Operations |-> FALSE]]
    /\ UNCHANGED <<state, barrierEver, sendCount, brokerEffect, resultDurable,
                    bindingsDurable, observationEvidence, leaseEpoch, heldToken,
                    dispatchWriter, dispatchEpoch, permitAvailable,
                    sendWriter, sendEpoch>>

Restart(w) ==
    /\ w \in Writers
    /\ crashed[w] = TRUE
    /\ crashed' = [crashed EXCEPT ![w] = FALSE]
    /\ UNCHANGED <<state, barrierEver, sendCount, brokerEffect, resultDurable,
                    bindingsDurable, observationEvidence, leaseOwner,
                    leaseEpoch, heldToken, dispatchWriter, dispatchEpoch,
                    permitAvailable, volatileResult, sendWriter, sendEpoch>>

Next ==
    \/ \E w \in Writers, o \in Operations : AcquireLease(w, o)
    \/ \E w \in Writers, o \in Operations : Prepare(w, o)
    \/ \E w \in Writers, o \in Operations : EnterDispatching(w, o)
    \/ \E w \in Writers, o \in Operations : EnterSubmitting(w, o)
    \/ \E w \in Writers, o \in Operations, e \in BrokerEffects :
        OrderSendKnown(w, o, e)
    \/ \E w \in Writers, o \in Operations, e \in BrokerEffects :
        OrderSendUnknown(w, o, e)
    \/ \E w \in Writers, o \in Operations : PersistResult(w, o)
    \/ \E o \in Operations : BeginReconciliation(o)
    \/ \E o \in Operations : Recover(o)
    \/ \E o \in Operations : ObserveBroker(o)
    \/ \E o \in Operations : Resolve(o)
    \/ \E o \in Operations : LoseLease(o)
    \/ \E w \in Writers : Crash(w)
    \/ \E w \in Writers : Restart(w)

TypeOK ==
    /\ state \in [Operations -> States]
    /\ barrierEver \in [Operations -> BOOLEAN]
    /\ sendCount \in [Operations -> 0..1]
    /\ brokerEffect \in [Operations -> BrokerEffects]
    /\ resultDurable \in [Operations -> BOOLEAN]
    /\ bindingsDurable \in [Operations -> BOOLEAN]
    /\ observationEvidence \in [Operations -> BOOLEAN]
    /\ leaseOwner \in [Operations -> (Writers \cup {"none"})]
    /\ leaseEpoch \in [Operations -> 0..MaxEpoch]
    /\ heldToken \in [Writers -> [Operations -> 0..MaxEpoch]]
    /\ crashed \in [Writers -> BOOLEAN]
    /\ dispatchWriter \in [Operations -> (Writers \cup {"none"})]
    /\ dispatchEpoch \in [Operations -> 0..MaxEpoch]
    /\ permitAvailable \in [Operations -> BOOLEAN]
    /\ volatileResult \in [Writers -> [Operations -> BOOLEAN]]
    /\ sendWriter \in [Operations -> (Writers \cup {"none"})]
    /\ sendEpoch \in [Operations -> 0..MaxEpoch]

AtMostOneBrokerSend ==
    \A o \in Operations : sendCount[o] <= 1

NoResendAfterBarrier ==
    \A o \in Operations : barrierEver[o] => state[o] \notin PreDispatch

NoStaleWriterSend ==
    \A o \in Operations : sendCount[o] = 0 \/
        (sendWriter[o] = dispatchWriter[o] /\ sendEpoch[o] = dispatchEpoch[o])

FencingMonotonic ==
    /\ \A o \in Operations : leaseEpoch[o] \in 0..MaxEpoch
    /\ \A w \in Writers, o \in Operations : heldToken[w][o] <= leaseEpoch[o]
    /\ \A o \in Operations : dispatchEpoch[o] <= leaseEpoch[o]

ExecutedNeverReturnsToPreDispatch ==
    \A o \in Operations : brokerEffect[o] = "executed" =>
        state[o] \notin PreDispatch

DurableResultImpliesBindings ==
    \A o \in Operations : resultDurable[o] => bindingsDurable[o]

TerminalRequiresEvidence ==
    \A o \in Operations : state[o] = "terminal" =>
        resultDurable[o] \/ observationEvidence[o]

NoSendWithoutBarrier ==
    \A o \in Operations : sendCount[o] = 0 \/ barrierEver[o]

Spec == Init /\ [][Next]_vars

=============================================================================

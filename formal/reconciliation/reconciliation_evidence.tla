------------------------- MODULE reconciliation_evidence -------------------------
EXTENDS Naturals, FiniteSets

(***************************************************************************)
(* A deliberately small model of observation-only reconciliation.  Each   *)
(* domain is refreshed independently: a deal may be visible before its    *)
(* history order, while an unrefreshed domain remains stale relative to    *)
(* the durable baseline.                                                   *)
(***************************************************************************)

CONSTANTS Domains, Accounts, OperationAccount, RequestModes, MaxRevision,
          MaxGraphInstance, MaxDomainRevision

EvidenceKinds == {"unseen", "match", "absence", "ambiguous"}
RefreshKinds == EvidenceKinds \ {"unseen"}

VARIABLES requestMode, currentAccount, baselineAccount,
          graphInstance, graphRevision, baselineGraphInstance,
          baselineGraphRevision, domainRevision, baselineDomainRevision,
          evidence, authoritative, eventGap, deadlineExpired

vars == <<requestMode, currentAccount, baselineAccount,
           graphInstance, graphRevision, baselineGraphInstance,
           baselineGraphRevision, domainRevision, baselineDomainRevision,
           evidence, authoritative, eventGap, deadlineExpired>>

Init ==
    /\ requestMode \in RequestModes
    /\ OperationAccount \in Accounts
    /\ currentAccount = OperationAccount
    /\ baselineAccount = OperationAccount
    /\ graphInstance = 1
    /\ graphRevision = 0
    /\ baselineGraphInstance = 1
    /\ baselineGraphRevision = 0
    /\ domainRevision = [d \in Domains |-> 0]
    /\ baselineDomainRevision = [d \in Domains |-> 0]
    /\ evidence = [d \in Domains |-> "unseen"]
    /\ authoritative = [d \in Domains |-> FALSE]
    /\ eventGap = FALSE
    /\ deadlineExpired = FALSE

RequiredDomains(mode) ==
    IF mode = "all_present"
    THEN Domains
    ELSE IF mode = "history_order_absent"
    THEN {"history_orders"}
    ELSE IF mode = "active_order_present"
    THEN {"active_orders"}
    ELSE IF mode = "history_deal_present"
    THEN {"history_deals"}
    ELSE {}

ExpectedPresent(d) ==
    IF requestMode = "history_order_absent" /\ d = "history_orders"
    THEN FALSE
    ELSE TRUE

Fresh(d) == domainRevision[d] > baselineDomainRevision[d]

PredicateSatisfied(d) ==
    /\ Fresh(d)
    /\ IF ExpectedPresent(d)
          THEN evidence[d] = "match"
          ELSE /\ evidence[d] = "absence"
               /\ authoritative[d]

AllFresh == \A d \in RequiredDomains(requestMode) : Fresh(d)
AllSatisfied == \A d \in RequiredDomains(requestMode) : PredicateSatisfied(d)
HasFreshAmbiguity ==
    \E d \in RequiredDomains(requestMode) : Fresh(d) /\ evidence[d] = "ambiguous"

Outcome ==
    IF currentAccount # OperationAccount \/ baselineAccount # OperationAccount
    THEN "account_mismatch"
    ELSE IF graphInstance # baselineGraphInstance
    THEN "ambiguous"
    ELSE IF HasFreshAmbiguity
    THEN "ambiguous"
    ELSE IF eventGap /\ (~AllFresh \/ ~AllSatisfied)
    THEN "trade_event_gap"
    ELSE IF ~AllFresh
    THEN "pending"
    ELSE IF AllSatisfied
    THEN "confirmed"
    ELSE IF deadlineExpired
    THEN "not_observed"
    ELSE "pending"

Refresh(d, kind, isAuthoritative) ==
    /\ d \in Domains
    /\ kind \in RefreshKinds
    /\ isAuthoritative \in BOOLEAN
    /\ d \in RequiredDomains(requestMode)
    /\ graphRevision < MaxRevision
    /\ domainRevision[d] < MaxDomainRevision
    /\ graphRevision' = graphRevision + 1
    /\ domainRevision' = [domainRevision EXCEPT ![d] = @ + 1]
    /\ evidence' = [evidence EXCEPT ![d] = kind]
    /\ authoritative' = [authoritative EXCEPT ![d] = isAuthoritative]
    /\ UNCHANGED <<requestMode, currentAccount, baselineAccount,
                    graphInstance, baselineGraphInstance,
                    baselineGraphRevision, baselineDomainRevision,
                    eventGap, deadlineExpired>>

OpenEventGap ==
    /\ ~eventGap
    /\ eventGap' = TRUE
    /\ UNCHANGED <<requestMode, currentAccount, baselineAccount,
                    graphInstance, graphRevision, baselineGraphInstance,
                    baselineGraphRevision, domainRevision,
                    baselineDomainRevision, evidence, authoritative,
                    deadlineExpired>>

SwitchAccount(account) ==
    /\ account \in Accounts
    /\ account # currentAccount
    /\ currentAccount' = account
    /\ UNCHANGED <<requestMode, baselineAccount, graphInstance,
                    graphRevision, baselineGraphInstance,
                    baselineGraphRevision, domainRevision,
                    baselineDomainRevision, evidence, authoritative,
                    eventGap, deadlineExpired>>

GraphReplacement ==
    /\ graphRevision < MaxRevision
    /\ graphInstance < MaxGraphInstance
    /\ graphInstance' = graphInstance + 1
    /\ graphRevision' = graphRevision + 1
    /\ UNCHANGED <<requestMode, currentAccount, baselineAccount,
                    baselineGraphInstance, baselineGraphRevision,
                    domainRevision, baselineDomainRevision, evidence,
                    authoritative, eventGap, deadlineExpired>>

RestartReanchor ==
    /\ graphRevision < MaxRevision
    /\ graphInstance < MaxGraphInstance
    /\ graphInstance' = graphInstance + 1
    /\ graphRevision' = graphRevision + 1
    /\ baselineGraphInstance' = graphInstance + 1
    /\ baselineGraphRevision' = graphRevision + 1
    /\ baselineAccount' = currentAccount
    /\ baselineDomainRevision' = domainRevision
    /\ eventGap' = FALSE
    /\ deadlineExpired' = FALSE
    /\ UNCHANGED <<requestMode, currentAccount, domainRevision, evidence,
                    authoritative>>

ExpireDeadline ==
    /\ deadlineExpired' = TRUE
    /\ UNCHANGED <<requestMode, currentAccount, baselineAccount,
                    graphInstance, graphRevision, baselineGraphInstance,
                    baselineGraphRevision, domainRevision,
                    baselineDomainRevision, evidence, authoritative,
                    eventGap>>

Next ==
    \/ \E d \in RequiredDomains(requestMode), kind \in RefreshKinds,
          isAuthoritative \in BOOLEAN : Refresh(d, kind, isAuthoritative)
    \/ OpenEventGap
    \/ \E account \in Accounts : SwitchAccount(account)
    \/ GraphReplacement
    \/ RestartReanchor
    \/ ExpireDeadline

TypeOK ==
    /\ requestMode \in RequestModes
    /\ currentAccount \in Accounts
    /\ baselineAccount \in Accounts
    /\ graphInstance \in 1..MaxGraphInstance
    /\ graphRevision \in 0..MaxRevision
    /\ baselineGraphInstance \in 1..MaxGraphInstance
    /\ baselineGraphRevision \in 0..MaxRevision
    /\ domainRevision \in [Domains -> 0..MaxDomainRevision]
    /\ baselineDomainRevision \in [Domains -> 0..MaxDomainRevision]
    /\ evidence \in [Domains -> EvidenceKinds]
    /\ authoritative \in [Domains -> BOOLEAN]
    /\ eventGap \in BOOLEAN
    /\ deadlineExpired \in BOOLEAN
    /\ baselineGraphRevision <= graphRevision
    /\ \A d \in Domains :
           domainRevision[d] <= graphRevision /\
           baselineDomainRevision[d] <= graphRevision

SafeToConfirm ==
    /\ currentAccount = OperationAccount
    /\ baselineAccount = OperationAccount
    /\ graphInstance = baselineGraphInstance
    /\ AllFresh
    /\ AllSatisfied
    /\ ~HasFreshAmbiguity

NoFalseConfirmed ==
    Outcome = "confirmed" => SafeToConfirm

NoCrossAccountConfirmation ==
    Outcome = "confirmed" => currentAccount = OperationAccount

GapDoesNotMaskUnsettledEvidence ==
    (eventGap /\ currentAccount = OperationAccount /\
        baselineAccount = OperationAccount /\
        graphInstance = baselineGraphInstance /\
        ~HasFreshAmbiguity /\ (~AllFresh \/ ~AllSatisfied))
        => (Outcome = "trade_event_gap")

RestartRequiresFreshEvidence ==
    (graphInstance = baselineGraphInstance /\
        baselineGraphRevision = graphRevision /\
        ~AllFresh)
        => (Outcome # "confirmed")

AbsenceNeedsAuthoritativeCoverage ==
    (Outcome = "confirmed" /\ requestMode = "history_order_absent")
        => (authoritative["history_orders"] /\
            evidence["history_orders"] = "absence")

Spec == Init /\ [][Next]_vars

=============================================================================

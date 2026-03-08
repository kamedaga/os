------------------------------ MODULE CapabilityIO ------------------------------
EXTENDS FiniteSets, TLC

\* Minimal state machine for exclusive DMA pages.
\* This mirrors the current kernel semantics closely enough to expose
\* rights-escalation bugs before adding a richer DMA mapping token model.

RightBits == {"cpu_read", "cpu_write", "dma"}
NoRights == {}
FullRights == RightBits
DmaOnly == {"dma"}

Principals == {"Process0", "Process1", "Device0"}
PAddrs == {"Page0"}

ViolationKinds ==
    {"MoveEscalatesRights",
     "DeviceGetsCpuRights",
     "CompleteDmaRestoresDifferentRights"}

VARIABLES caps, dmaSaved, violations

vars == <<caps, dmaSaved, violations>>

HolderCount(p) ==
    Cardinality({ principal \in Principals : caps[principal][p] # NoRights })

TypeInv ==
    /\ caps \in [Principals -> [PAddrs -> SUBSET RightBits]]
    /\ dmaSaved \in [PAddrs -> SUBSET RightBits]
    /\ violations \subseteq ViolationKinds

ExclusiveHolder ==
    \A p \in PAddrs : (HolderCount(p) = 0) \/ (HolderCount(p) = 1)

NoViolations == violations = {}

Init ==
    /\ caps =
        [principal \in Principals |->
            [p \in PAddrs |->
                IF principal = "Process0" THEN FullRights ELSE NoRights]]
    /\ dmaSaved = [p \in PAddrs |-> NoRights]
    /\ violations = {}

ExclusiveMove(from, to, p, newRights) ==
    LET oldRights == caps[from][p]
    IN
    /\ from /= to
    /\ caps[from][p] # NoRights
    /\ caps[to][p] = NoRights
    /\ newRights # NoRights
    /\ newRights \subseteq oldRights
    /\ (to # "Device0") \/ (newRights = DmaOnly)
    /\ caps' = [caps EXCEPT ![from][p] = NoRights, ![to][p] = newRights]
    /\ dmaSaved' = dmaSaved
    /\ violations' = violations

StartDma(p) ==
    LET savedRights == caps["Process0"][p]
    IN
    /\ savedRights # NoRights
    /\ "dma" \in savedRights
    /\ caps["Device0"][p] = NoRights
    /\ dmaSaved[p] = NoRights
    /\ caps' =
        [caps EXCEPT
            !["Process0"][p] = NoRights,
            !["Device0"][p] = DmaOnly]
    /\ dmaSaved' = [dmaSaved EXCEPT ![p] = savedRights]
    /\ violations' = violations

CompleteDma(p) ==
    /\ caps["Process0"][p] = NoRights
    /\ caps["Device0"][p] # NoRights
    /\ "dma" \in caps["Device0"][p]
    /\ dmaSaved[p] # NoRights
    /\ caps' =
        [caps EXCEPT
            !["Device0"][p] = NoRights,
            !["Process0"][p] = dmaSaved[p]]
    /\ dmaSaved' = [dmaSaved EXCEPT ![p] = NoRights]
    /\ violations' = violations

Next ==
    \E p \in PAddrs :
        \/ StartDma(p)
        \/ CompleteDma(p)
        \/ \E from \in Principals, to \in Principals, rs \in SUBSET RightBits :
            ExclusiveMove(from, to, p, rs)

Spec == Init /\ [][Next]_vars

=============================================================================

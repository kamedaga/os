------------------------------ MODULE SakuraDMA ------------------------------

EXTENDS Naturals, Sequences

(***************************************************************************)
(* Sets *)
(***************************************************************************)

CONSTANT Proc
CONSTANT Dev
CONSTANT MemRegion
CONSTANT Rights
CONSTANT ReadRight
CONSTANT WriteRight
CONSTANT DMARight

Principal == Proc \cup Dev
Cap == [obj : MemRegion, rights : SUBSET Rights]

(***************************************************************************)
(* State Variables *)
(***************************************************************************)

VARIABLES owner, cap_store

(***************************************************************************)
(* Init *)
(***************************************************************************)

Init ==
  /\ owner \in [MemRegion -> Proc]
  /\ cap_store =
       [p \in Principal |->
          { [obj |-> r, rights |-> {ReadRight, DMARight}] :
              r \in { rr \in MemRegion : owner[rr] = p } }]
  /\ ReadRight \in Rights
  /\ WriteRight \in Rights
  /\ DMARight \in Rights
  /\ ReadRight /= DMARight
  /\ WriteRight /= DMARight

(***************************************************************************)
(* Access Definition *)
(***************************************************************************)

Access(p, r, right) ==
  /\ \E c \in cap_store[p] :
       /\ c.obj = r
       /\ right \in c.rights
  /\ owner[r] = p

(***************************************************************************)
(* DMA Start *)
(***************************************************************************)

StartDMA(d, r) ==
  /\ d \in Dev
  /\ r \in MemRegion
  /\ \E p \in Proc :
        /\ owner[r] = p
        /\ \E cp \in cap_store[p] :
             /\ cp.obj = r
             /\ DMARight \in cp.rights
             /\ ~(\E cd \in cap_store[d] : cd.obj = r)
             /\ owner' = [owner EXCEPT ![r] = d]
             /\ cap_store' =
                  [cap_store EXCEPT
                      ![p] = (cap_store[p] \ {cp})
                             \cup {[cp EXCEPT !.rights = cp.rights \ {DMARight}]},
                      ![d] = cap_store[d] \cup {[obj |-> r, rights |-> {DMARight}]}]

(***************************************************************************)
(* DMA End *)
(***************************************************************************)

EndDMA(d, r, p) ==
  /\ d \in Dev
  /\ p \in Proc
  /\ owner[r] = d
  /\ \E cd \in cap_store[d] :
       /\ cd.obj = r
       /\ DMARight \in cd.rights
       /\ \E cp \in cap_store[p] :
            /\ cp.obj = r
            /\ owner' = [owner EXCEPT ![r] = p]
            /\ cap_store' =
                 [cap_store EXCEPT
                     ![d] = cap_store[d] \ {cd},
                     ![p] = (cap_store[p] \ {cp})
                            \cup {[cp EXCEPT !.rights = cp.rights \cup {DMARight}]}]

(***************************************************************************)
(* Next Relation *)
(***************************************************************************)

Next ==
  \/ \E d \in Dev, r \in MemRegion :
        StartDMA(d, r)
  \/ \E d \in Dev, r \in MemRegion, p \in Proc :
        EndDMA(d, r, p)

Spec ==
  Init /\ [][Next]_<<owner, cap_store>>

(***************************************************************************)
(* Invariants *)
(***************************************************************************)

ExclusiveOwnership ==
  \A r \in MemRegion :
    owner[r] \in Principal

AccessSafety ==
  \A p \in Proc :
    \A r \in MemRegion :
      \A right \in Rights :
        Access(p, r, right) => owner[r] = p

DMAIsolation ==
  \A d \in Dev :
    \A r \in MemRegion :
      owner[r] = d =>
        \A p \in Proc :
          \A right \in Rights :
            ~Access(p, r, right)

CapNotImpliesOwnership ==
  \A p \in Proc :
    \A c \in cap_store[p] :
      c.obj \in MemRegion

CapImpliesPotentialOwnership ==
  \A p \in Principal :
    \A c \in cap_store[p] :
      owner[c.obj] \in Principal

DMAEnabled ==
  \A r \in MemRegion :
    owner[r] \in Proc =>
      \E c \in cap_store[owner[r]] :
        c.obj = r

OwnershipHasCap ==
  \A r \in MemRegion :
    \E c \in cap_store[owner[r]] :
      c.obj = r

=============================================================================

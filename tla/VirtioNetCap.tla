------------------------------ MODULE VirtioNetCap ------------------------------

EXTENDS Naturals, Sequences

(***************************************************************************)
(* Sets *)
(***************************************************************************)

CONSTANT Proc
CONSTANT Dev
CONSTANT MemRegion
CONSTANT Rights
CONSTANT ReadRight
CONSTANT DMARight

Principal == Proc \cup Dev
Cap == [obj : MemRegion, rights : SUBSET Rights]

(***************************************************************************)
(* State Variables *)
(***************************************************************************)

VARIABLES owner, cap_store, tx_queue

(***************************************************************************)
(* Init *)
(***************************************************************************)

Init ==
  /\ owner \in [MemRegion -> Proc]
  /\ cap_store =
       [p \in Principal |->
          { [obj |-> r, rights |-> {ReadRight, DMARight}] :
              r \in { rr \in MemRegion : owner[rr] = p } }]
  /\ tx_queue = << >>
  /\ ReadRight \in Rights
  /\ DMARight \in Rights
  /\ ReadRight /= DMARight

(***************************************************************************)
(* Access *)
(***************************************************************************)

Access(p, r, right) ==
  /\ \E c \in cap_store[p] :
        /\ c.obj = r
        /\ right \in c.rights
  /\ owner[r] = p

(***************************************************************************)
(* Submit: Process sends packet *)
(***************************************************************************)

Submit(p, r) ==
  /\ p \in Proc
  /\ r \in MemRegion
  /\ owner[r] = p
  /\ \E cp \in cap_store[p] :
        /\ cp.obj = r
        /\ DMARight \in cp.rights
        /\ LET d == CHOOSE dv \in Dev : TRUE IN
             /\ owner' = [owner EXCEPT ![r] = d]
             /\ cap_store' =
                  [cap_store EXCEPT
                      ![p] = (cap_store[p] \ {cp})
                             \cup {[cp EXCEPT !.rights = cp.rights \ {DMARight}]},
                      ![d] = cap_store[d]
                             \cup {[obj |-> r, rights |-> {DMARight}]}]
             /\ tx_queue' = << r >>

(***************************************************************************)
(* Complete: Device finishes packet *)
(***************************************************************************)

Complete(d, r, p) ==
  /\ d \in Dev
  /\ p \in Proc
  /\ owner[r] = d
  /\ tx_queue = << r >>
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
              /\ tx_queue' = << >>

(***************************************************************************)
(* Next *)
(***************************************************************************)

Next ==
  \/ \E p \in Proc, r \in MemRegion :
        Submit(p, r)
  \/ \E d \in Dev, r \in MemRegion, p \in Proc :
        Complete(d, r, p)

Spec ==
  Init /\ [][Next]_<<owner, cap_store, tx_queue>>

SeqToSet(s) ==
  { s[i] : i \in 1..Len(s) }

(***************************************************************************)
(* Invariants *)
(***************************************************************************)

ExclusiveOwnership ==
  \A r \in MemRegion :
    owner[r] \in Principal

DMAIsolation ==
  \A d \in Dev :
    \A r \in MemRegion :
      owner[r] = d =>
        \A p \in Proc :
          ~Access(p, r, ReadRight)

QueueOwner ==
  \A r \in SeqToSet(tx_queue) :
    owner[r] \in Dev

OwnershipHasCap ==
  \A r \in MemRegion :
    \E c \in cap_store[owner[r]] :
      c.obj = r

=============================================================================

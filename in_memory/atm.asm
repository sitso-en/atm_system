; Group 7 (FF17) - ATM system, in-memory build.
; Accounts live in RAM and reset each run. All the code is shared: see
; ../common/atm_core.inc. This build just includes it without PERSISTENT.
%include "atm_core.inc"

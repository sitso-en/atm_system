; Group 7 (FF17) - ATM system, persistent build.
; Account state survives between runs in atm_data.dat. All the code is shared:
; see ../common/atm_core.inc. Defining PERSISTENT turns on load/save.
%define PERSISTENT
%include "atm_core.inc"

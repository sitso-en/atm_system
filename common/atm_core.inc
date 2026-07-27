; Group 7 (FF17) - ATM system, shared core
; x86-64 NASM, Linux, terminal only. No libc, just syscalls.
;
; This one file is the whole program. The two builds include it:
;   in_memory/atm.asm   - just includes this; state lives in RAM, resets each run
;   persistent/atm.asm  - defines PERSISTENT first; state is saved to a file
; Everything guarded by %ifdef PERSISTENT is the only difference between them,
; so the two builds can't drift apart.
;
; First time you pick an account you set its own 4-digit PIN. PINs are never
; stored in the clear - we keep a salted hash and compare hashes.
;
; Register notes:
;   syscalls  - number in rax, args rdi/rsi/rdx/r10/r8/r9, result in rax.
;               the kernel clobbers rax, rcx and r11 on every syscall, so we
;               never keep a live value in rcx/r11 across one.
;   we keep long-lived values (account index, counters) in the callee-saved
;   regs r12-r15 so calls and syscalls don't wipe them.

; --- settings ---
N_ACCOUNTS         equ 6            ; demo accounts created on a fresh start
MAX_ACCOUNTS       equ 12           ; capacity of the account table (room for more)
HIST_MAX           equ 10           ; transactions remembered per account
MINI_COUNT         equ 5            ; lines shown in a mini statement
; money is held in pesewas (1 GHS = 100 pesewas) so there's no floating point
WITHDRAW_LIMIT     equ 1000 * 100   ; GHS 1,000 cap per single withdrawal
DAILY_LIMIT        equ 2000 * 100   ; GHS 2,000 cap on total withdrawn per day
WD_MULTIPLE        equ 10 * 100     ; withdrawals must be a whole number of GHS 10
MAX_INVALID        equ 3            ; bad menu entries in a row before logout
ADMIN_PIN          equ 9999
TIMEOUT_MS         equ 60000        ; idle time before auto logout
PIN_MIN            equ 1000         ; PIN must be exactly 4 digits
PIN_MAX            equ 9999
MAX_DIGITS         equ 18           ; refuse longer numbers (avoids 64-bit wrap)
PIN_SALT           equ 0x5A17C0DE   ; mixed into the PIN hash

; terminal ioctls used to hide the PIN while typing
TCGETS             equ 0x5401
TCSETS             equ 0x5402
ECHO_BIT           equ 8            ; ECHO flag inside c_lflag
SA_RESTORER        equ 0x04000000

; syscalls we use by number
SYS_read           equ 0
SYS_write          equ 1
SYS_open           equ 2
SYS_close          equ 3
SYS_poll           equ 7
SYS_rt_sigaction   equ 13
SYS_rt_sigreturn   equ 15
SYS_ioctl          equ 16
SYS_time           equ 201
SYS_exit           equ 60

; file access (persistent build)
O_RDONLY           equ 0
O_WRCREATTRUNC     equ 0x241        ; O_WRONLY|O_CREAT|O_TRUNC
FILE_MODE          equ 0o644

; on the persistent build, mutating helpers call save_state; on the in-memory
; build SAVE expands to nothing.
%ifdef PERSISTENT
  %macro SAVE 0
    call save_state
  %endmacro
%else
  %macro SAVE 0
  %endmacro
%endif


section .data

; the sigaction struct, so a Ctrl-C mid-PIN can't leave the terminal with echo
; turned off. handler restores echo then exits.
sigact:
    dq sig_handler
    dq SA_RESTORER
    dq sig_restorer
    dq 0

note_vals: dq 20000, 10000, 5000, 2000, 1000   ; note values in pesewas (GHS 200..10)
NOTE_COUNT equ ($ - note_vals) / 8

%ifdef PERSISTENT
filename:  db "atm_data.dat", 0      ; state file, in the current directory
banner:
    db 10
    db "  Group 7 ATM (saved)", 10
    db "  -------------------", 10
%else
banner:
    db 10
    db "  Group 7 ATM", 10
    db "  -----------", 10
%endif
banner_len equ $ - banner

welcome_menu:
    db 10
    db "Main menu", 10
    db "1. Login", 10
    db "2. Admin", 10
    db "3. Quit", 10
    db "> "
welcome_menu_len equ $ - welcome_menu

account_menu:
    db 10
    db "Account menu", 10
    db "1. Balance", 10
    db "2. Deposit", 10
    db "3. Withdraw", 10
    db "4. Transfer", 10
    db "5. History", 10
    db "6. Mini statement", 10
    db "7. Change PIN", 10
    db "8. Log out", 10
    db "> "
account_menu_len equ $ - account_menu

admin_menu:
    db 10
    db "Admin menu", 10
    db "1. View accounts", 10
    db "2. Unlock account", 10
    db "3. Reset a PIN", 10
    db "4. View account history", 10
    db "5. Create account", 10
    db "6. Back", 10
    db "> "
admin_menu_len equ $ - admin_menu

; prompts
ask_acct:    db "Account number: "
ask_acct_len equ $ - ask_acct
ask_pin:     db "PIN: "
ask_pin_len  equ $ - ask_pin
ask_admin:   db "Admin PIN: "
ask_admin_len equ $ - ask_admin
ask_amount:  db "Amount: GHS "
ask_amount_len equ $ - ask_amount
ask_cur_pin: db "Current PIN: "
ask_cur_pin_len equ $ - ask_cur_pin
ask_new_pin: db "New PIN: "
ask_new_pin_len equ $ - ask_new_pin
ask_confirm: db "Confirm PIN: "
ask_confirm_len equ $ - ask_confirm
ask_unlock:  db "Account to unlock: "
ask_unlock_len equ $ - ask_unlock
ask_target:  db "Transfer to account: "
ask_target_len equ $ - ask_target
ask_newacct: db "New account number: "
ask_newacct_len equ $ - ask_newacct

; messages
m_setpin:    db "No PIN set for this account. Pick one now.", 10
m_setpin_len equ $ - m_setpin
m_pin_len:   db "PIN must be a 4-digit number (1000-9999).", 10
m_pin_len_len equ $ - m_pin_len
m_login_ok:  db "Login successful.", 10
m_login_ok_len equ $ - m_login_ok
m_wrong:     db "Wrong PIN, try again.", 10
m_wrong_len  equ $ - m_wrong
m_locked:    db "Too many wrong tries. Account locked.", 10
m_locked_len equ $ - m_locked
m_is_locked: db "Account is locked. Please see the admin.", 10
m_is_locked_len equ $ - m_is_locked
m_no_acct:   db "No account with that number.", 10
m_no_acct_len equ $ - m_no_acct
m_balance:   db "Balance: GHS "
m_balance_len equ $ - m_balance
m_deposited: db "Deposit done.", 10
m_deposited_len equ $ - m_deposited
m_withdrawn: db "Withdrawal done.", 10
m_withdrawn_len equ $ - m_withdrawn
m_nofunds:   db "Not enough funds.", 10
m_nofunds_len equ $ - m_nofunds
m_overlimit: db "Over the GHS 1,000 per-withdrawal limit.", 10
m_overlimit_len equ $ - m_overlimit
m_daily:     db "That would pass the GHS 2,000 daily withdrawal limit.", 10
m_daily_len  equ $ - m_daily
m_multiple:  db "Withdrawals must be a multiple of GHS 10.", 10
m_multiple_len equ $ - m_multiple
m_bad_num:   db "Please enter a positive whole number.", 10
m_bad_num_len equ $ - m_bad_num
m_bad_amt:   db "Please enter a valid amount, e.g. 50 or 12.50.", 10
m_bad_amt_len equ $ - m_bad_amt
m_bad_opt:   db "Invalid option.", 10
m_bad_opt_len equ $ - m_bad_opt
m_cancel:    db "Cancelled.", 10
m_cancel_len equ $ - m_cancel
m_too_many:  db "Too many invalid entries. Logging out.", 10
m_too_many_len equ $ - m_too_many
m_timeout:   db 10, "Timed out. Logging out.", 10
m_timeout_len equ $ - m_timeout
m_logout:    db "Logged out.", 10
m_logout_len equ $ - m_logout
m_cur_wrong: db "Current PIN is wrong.", 10
m_cur_wrong_len equ $ - m_cur_wrong
m_nomatch:   db "PINs did not match. PIN unchanged.", 10
m_nomatch_len equ $ - m_nomatch
m_pin_set:   db "PIN changed.", 10
m_pin_set_len equ $ - m_pin_set
m_admin_no:  db "Wrong admin PIN.", 10
m_admin_no_len equ $ - m_admin_no
m_unlocked:  db "Account unlocked.", 10
m_unlocked_len equ $ - m_unlocked
m_not_locked: db "That account is not locked.", 10
m_not_locked_len equ $ - m_not_locked
m_transfer:  db "Transfer done.", 10
m_transfer_len equ $ - m_transfer
m_self:      db "Cannot transfer to the same account.", 10
m_self_len   equ $ - m_self
m_tlocked:   db "That account is locked.", 10
m_tlocked_len equ $ - m_tlocked
m_pin_reset: db "PIN reset. The holder will set a new one at next login.", 10
m_pin_reset_len equ $ - m_pin_reset
m_created:   db "Account created.", 10
m_created_len equ $ - m_created
m_exists:    db "That account number already exists.", 10
m_exists_len equ $ - m_exists
m_full:      db "Account table is full.", 10
m_full_len   equ $ - m_full
m_no_txn:    db "  no transactions yet", 10
m_no_txn_len equ $ - m_no_txn
h_history:   db 10, "History:", 10
h_history_len equ $ - h_history
h_mini:      db 10, "Last transactions:", 10
h_mini_len   equ $ - h_mini
h_dispense:  db "Dispensing:", 10
h_dispense_len equ $ - h_dispense
m_bye:       db 10, "Thanks for using Group 7's ATM.", 10
m_bye_len    equ $ - m_bye
m_summary:   db "Transactions this session: "
m_summary_len equ $ - m_summary

; small pieces
p_dot:     db ". "
p_dot_len  equ $ - p_dot
p_dep:     db "deposit    +GHS "
p_dep_len  equ $ - p_dep
p_wd:      db "withdraw   -GHS "
p_wd_len   equ $ - p_wd
p_tout:    db "transfer   -GHS "
p_tout_len equ $ - p_tout
p_tin:     db "transfer   +GHS "
p_tin_len  equ $ - p_tin
p_two:     db "  "
p_two_len  equ $ - p_two
p_bal:     db "  GHS "
p_bal_len  equ $ - p_bal
p_note_x:  db " x GHS "
p_note_x_len equ $ - p_note_x
p_active:  db "  active", 10
p_active_len equ $ - p_active
p_locked:  db "  LOCKED", 10
p_locked_len equ $ - p_locked
p_dash:    db "-"
p_colon:   db ":"
p_point:   db "."
newline:   db 10


section .bss

; --- persistent state block ---
; keep these labels adjacent and in this order; load/save copy the whole
; range [state_begin, state_end) in one syscall.
state_begin:
n_accounts  resq 1                          ; accounts currently in use
acct_no     resq MAX_ACCOUNTS               ; account numbers
acct_pin    resq MAX_ACCOUNTS               ; salted PIN hash, 0 = no PIN yet
acct_bal    resq MAX_ACCOUNTS
acct_lock   resq MAX_ACCOUNTS               ; 1 = locked
wd_day      resq MAX_ACCOUNTS               ; day-number of last withdrawal
wd_today    resq MAX_ACCOUNTS               ; total withdrawn on wd_day
hist_n      resq MAX_ACCOUNTS               ; transactions stored per account
hist_type   resq MAX_ACCOUNTS * HIST_MAX     ; 1 dep, 2 wd, 3 transfer-out, 4 transfer-in
hist_amt    resq MAX_ACCOUNTS * HIST_MAX
hist_time   resq MAX_ACCOUNTS * HIST_MAX     ; epoch seconds when it happened
state_end:
STATE_SIZE  equ state_end - state_begin

; --- non-persistent scratch / session state ---
cur_acct    resq 1                          ; index of logged-in account
session_txn resq 1                          ; deposits+withdrawals+transfers this run

rdbuf       resb 256                         ; buffered stdin
rd_len      resq 1
rd_pos      resq 1
in_status   resb 1                           ; on read fail: 0 = eof, 1 = timeout
pfd         resq 1                           ; pollfd for the idle timeout
termbuf     resb 64                          ; saved terminal settings
saved_lflag resd 1
numbuf      resb 32

; scratch used by the date formatter
dt_secs resq 1
dt_era  resq 1
dt_doe  resq 1
dt_yoe  resq 1
dt_doy  resq 1
dt_mp   resq 1
dt_y    resq 1
dt_m    resq 1
dt_d    resq 1
dt_hh   resq 1
dt_mm   resq 1
dt_ss   resq 1
dt_t1   resq 1
dt_t2   resq 1
dt_t3   resq 1


section .text
    global _start

_start:
    call install_sigs
%ifdef PERSISTENT
    call load_state
%else
    call init_defaults
%endif
    mov rsi, banner
    mov rdx, banner_len
    call print

; top menu: login / admin / quit. Reached again after every logout.
welcome:
    mov rsi, welcome_menu
    mov rdx, welcome_menu_len
    call print
    call read_number
    jc .failed
    cmp rax, 1
    je do_login
    cmp rax, 2
    je do_admin
    cmp rax, 3
    je quit
    jmp .bad
.failed:
    cmp rdx, 1                  ; eof means stdin closed, so quit
    je quit
.bad:
    mov rsi, m_bad_opt
    mov rdx, m_bad_opt_len
    call print
    jmp welcome


; --- login ---
; r14 = account index, r12 = PIN attempts left
do_login:
    mov rsi, ask_acct
    mov rdx, ask_acct_len
    call print
    call read_number
    jc .read_failed

    call find_acct              ; rax number -> r14 index, CF=1 if none
    jc .no_acct

.got_acct:
    cmp qword [acct_lock + r14*8], 0
    jne .locked_out
    cmp qword [acct_pin + r14*8], 0    ; no PIN yet -> first-use setup
    je .first_use

    mov r12, 3
.pin_loop:
    mov rsi, ask_pin
    mov rdx, ask_pin_len
    call print
    call read_pin
    jc .pin_read_failed
    call hash_pin
    cmp rax, [acct_pin + r14*8]
    je .ok
.wrong:
    dec r12
    jz .lock_now
    mov rsi, m_wrong
    mov rdx, m_wrong_len
    call print
    jmp .pin_loop
.pin_read_failed:
    cmp rdx, 2
    je timeout_logout
    cmp rdx, 1
    je quit
    jmp .wrong                  ; garbage counts as a wrong try
.lock_now:
    mov qword [acct_lock + r14*8], 1
    SAVE
    mov rsi, m_locked
    mov rdx, m_locked_len
    call print
    jmp welcome
.locked_out:
    mov rsi, m_is_locked
    mov rdx, m_is_locked_len
    call print
    jmp welcome
.no_acct:
    mov rsi, m_no_acct
    mov rdx, m_no_acct_len
    call print
    jmp welcome

.first_use:
    mov rsi, m_setpin
    mov rdx, m_setpin_len
    call print
.set_new:
    mov rsi, ask_new_pin
    mov rdx, ask_new_pin_len
    call print
    call read_pin
    jc .enroll_failed
    call check_pin_range        ; CF=1 if not 4 digits
    jc .pin_bad
    mov r13, rax
    mov rsi, ask_confirm
    mov rdx, ask_confirm_len
    call print
    call read_pin
    jc .enroll_failed
    cmp rax, r13
    jne .enroll_mismatch
    mov rax, r13
    call hash_pin
    mov [acct_pin + r14*8], rax
    SAVE
    jmp .ok
.pin_bad:
    mov rsi, m_pin_len
    mov rdx, m_pin_len_len
    call print
    jmp .set_new
.enroll_mismatch:
    mov rsi, m_nomatch
    mov rdx, m_nomatch_len
    call print
    jmp welcome
.enroll_failed:
    cmp rdx, 2
    je timeout_logout
    cmp rdx, 1
    je quit
    mov rsi, m_bad_num
    mov rdx, m_bad_num_len
    call print
    jmp welcome

.ok:
    mov [cur_acct], r14
    mov rsi, m_login_ok
    mov rdx, m_login_ok_len
    call print
    jmp account_loop

.read_failed:
    cmp rdx, 2
    je timeout_logout
    cmp rdx, 1
    je quit
    mov rsi, m_bad_num
    mov rdx, m_bad_num_len
    call print
    jmp welcome


; --- customer menu ---
; r15 = bad entries in a row (too many -> logout)
account_loop:
    xor r15, r15
.top:
    mov rsi, account_menu
    mov rdx, account_menu_len
    call print
    call read_number
    jc .failed
    cmp rax, 1
    je .balance
    cmp rax, 2
    je .deposit
    cmp rax, 3
    je .withdraw
    cmp rax, 4
    je .transfer
    cmp rax, 5
    je .history
    cmp rax, 6
    je .mini
    cmp rax, 7
    je .change_pin
    cmp rax, 8
    je .logout
    jmp .bad
.failed:
    cmp rdx, 2
    je timeout_logout
    cmp rdx, 1
    je quit
.bad:
    inc r15
    cmp r15, MAX_INVALID
    jge .too_many
    mov rsi, m_bad_opt
    mov rdx, m_bad_opt_len
    call print
    jmp .top
.too_many:
    mov rsi, m_too_many
    mov rdx, m_too_many_len
    call print
    jmp welcome

.balance:
    xor r15, r15
    call show_balance
    jmp .top
.deposit:
    xor r15, r15
    call do_deposit
    jmp .top
.withdraw:
    xor r15, r15
    call do_withdraw
    jmp .top
.transfer:
    xor r15, r15
    call do_transfer
    jmp .top
.history:
    xor r15, r15
    call show_history
    jmp .top
.mini:
    xor r15, r15
    call show_mini
    jmp .top
.change_pin:
    xor r15, r15
    call do_change_pin
    jmp .top
.logout:
    mov rsi, m_logout
    mov rdx, m_logout_len
    call print
    jmp welcome


; --- deposit ---
do_deposit:
    mov rsi, ask_amount
    mov rdx, ask_amount_len
    call print
    call read_money
    jc .failed
    test rax, rax
    jz .bad_num
    mov r14, [cur_acct]
    add [acct_bal + r14*8], rax
    mov rdi, 1
    mov rsi, rax
    mov rcx, r14
    call add_txn
    inc qword [session_txn]
    SAVE
    mov rsi, m_deposited
    mov rdx, m_deposited_len
    call print
    call show_balance
    ret
.failed:
    test rdx, rdx               ; eof/timeout -> just cancel
    jnz .cancel
.bad_num:
    mov rsi, m_bad_amt
    mov rdx, m_bad_amt_len
    call print
    ret
.cancel:
    mov rsi, m_cancel
    mov rdx, m_cancel_len
    call print
    ret


; --- withdraw ---
; r12 = amount, r14 = account index. single exit at .ret restores r12.
do_withdraw:
    push r12
    mov rsi, ask_amount
    mov rdx, ask_amount_len
    call print
    call read_money
    jc .failed
    test rax, rax
    jz .bad_num
    mov r12, rax
    ; must be a whole number of notes
    xor rdx, rdx
    mov rax, r12
    mov rcx, WD_MULTIPLE
    div rcx
    test rdx, rdx
    jnz .bad_mult
    cmp r12, WITHDRAW_LIMIT
    ja .over
    mov r14, [cur_acct]
    cmp r12, [acct_bal + r14*8]
    ja .nofunds
    ; roll the daily tally over if the day changed
    call today_number           ; rax = current day number
    cmp [wd_day + r14*8], rax
    je .sameday
    mov [wd_day + r14*8], rax
    mov qword [wd_today + r14*8], 0
.sameday:
    mov rax, [wd_today + r14*8]
    add rax, r12
    cmp rax, DAILY_LIMIT
    ja .over_daily
    ; commit
    sub [acct_bal + r14*8], r12
    add [wd_today + r14*8], r12
    mov rdi, 2
    mov rsi, r12
    mov rcx, r14
    call add_txn
    inc qword [session_txn]
    SAVE
    mov rsi, m_withdrawn
    mov rdx, m_withdrawn_len
    call print
    call show_balance
    mov rax, r12
    call dispense
    jmp .ret
.over:
    mov rsi, m_overlimit
    mov rdx, m_overlimit_len
    call print
    jmp .ret
.over_daily:
    mov rsi, m_daily
    mov rdx, m_daily_len
    call print
    jmp .ret
.bad_mult:
    mov rsi, m_multiple
    mov rdx, m_multiple_len
    call print
    jmp .ret
.nofunds:
    mov rsi, m_nofunds
    mov rdx, m_nofunds_len
    call print
    jmp .ret
.failed:
    test rdx, rdx
    jnz .cancel
.bad_num:
    mov rsi, m_bad_amt
    mov rdx, m_bad_amt_len
    call print
    jmp .ret
.cancel:
    mov rsi, m_cancel
    mov rdx, m_cancel_len
    call print
.ret:
    pop r12
    ret


; --- transfer ---
; r14 = sender index, r13 = target index, r12 = amount.
do_transfer:
    push r12
    push r13
    push r14
    mov r14, [cur_acct]
    mov rsi, ask_target
    mov rdx, ask_target_len
    call print
    call read_number
    jc .cancel
    call find_acct              ; rax number -> r13 via helper? helper uses r14; adapt
    jc .no_acct
    mov r13, r14                ; find_acct left index in r14
    mov r14, [cur_acct]         ; restore sender index
    cmp r13, r14
    je .self
    cmp qword [acct_lock + r13*8], 0
    jne .tlocked
    mov rsi, ask_amount
    mov rdx, ask_amount_len
    call print
    call read_money
    jc .cancel
    test rax, rax
    jz .bad_num
    mov r12, rax
    cmp r12, [acct_bal + r14*8]
    ja .nofunds
    sub [acct_bal + r14*8], r12
    add [acct_bal + r13*8], r12
    mov rdi, 3                  ; transfer out on sender
    mov rsi, r12
    mov rcx, r14
    call add_txn
    mov rdi, 4                  ; transfer in on target
    mov rsi, r12
    mov rcx, r13
    call add_txn
    inc qword [session_txn]
    SAVE
    mov rsi, m_transfer
    mov rdx, m_transfer_len
    call print
    call show_balance
    jmp .ret
.self:
    mov rsi, m_self
    mov rdx, m_self_len
    call print
    jmp .ret
.tlocked:
    mov rsi, m_tlocked
    mov rdx, m_tlocked_len
    call print
    jmp .ret
.no_acct:
    mov rsi, m_no_acct
    mov rdx, m_no_acct_len
    call print
    jmp .ret
.nofunds:
    mov rsi, m_nofunds
    mov rdx, m_nofunds_len
    call print
    jmp .ret
.bad_num:
    mov rsi, m_bad_amt
    mov rdx, m_bad_amt_len
    call print
    jmp .ret
.cancel:
    mov rsi, m_cancel
    mov rdx, m_cancel_len
    call print
.ret:
    pop r14
    pop r13
    pop r12
    ret


; --- change PIN ---
; r13 holds the new PIN while we ask for confirmation
do_change_pin:
    mov rsi, ask_cur_pin
    mov rdx, ask_cur_pin_len
    call print
    call read_pin
    jc .cancel
    call hash_pin
    mov r14, [cur_acct]
    cmp rax, [acct_pin + r14*8]
    jne .cur_wrong
    mov rsi, ask_new_pin
    mov rdx, ask_new_pin_len
    call print
    call read_pin
    jc .cancel
    call check_pin_range
    jc .bad_new
    mov r13, rax
    mov rsi, ask_confirm
    mov rdx, ask_confirm_len
    call print
    call read_pin
    jc .cancel
    cmp rax, r13
    jne .nomatch
    mov rax, r13
    call hash_pin
    mov r14, [cur_acct]
    mov [acct_pin + r14*8], rax
    SAVE
    mov rsi, m_pin_set
    mov rdx, m_pin_set_len
    call print
    ret
.cur_wrong:
    mov rsi, m_cur_wrong
    mov rdx, m_cur_wrong_len
    call print
    ret
.bad_new:
    mov rsi, m_pin_len
    mov rdx, m_pin_len_len
    call print
    ret
.nomatch:
    mov rsi, m_nomatch
    mov rdx, m_nomatch_len
    call print
    ret
.cancel:
    mov rsi, m_cancel
    mov rdx, m_cancel_len
    call print
    ret


; --- admin ---
do_admin:
    mov r12, 3
.pin_loop:
    mov rsi, ask_admin
    mov rdx, ask_admin_len
    call print
    call read_pin
    jc .failed
    cmp rax, ADMIN_PIN
    je .ok
.wrong:
    dec r12
    jz .denied
    mov rsi, m_wrong
    mov rdx, m_wrong_len
    call print
    jmp .pin_loop
.failed:
    cmp rdx, 2
    je timeout_logout
    cmp rdx, 1
    je quit
    jmp .wrong
.denied:
    mov rsi, m_admin_no
    mov rdx, m_admin_no_len
    call print
    jmp welcome

.ok:
.menu:
    mov rsi, admin_menu
    mov rdx, admin_menu_len
    call print
    call read_number
    jc .menu_failed
    cmp rax, 1
    je .view
    cmp rax, 2
    je .unlock
    cmp rax, 3
    je .reset
    cmp rax, 4
    je .hist
    cmp rax, 5
    je .create
    cmp rax, 6
    je welcome
    jmp .menu_bad
.menu_failed:
    cmp rdx, 2
    je timeout_logout
    cmp rdx, 1
    je quit
.menu_bad:
    mov rsi, m_bad_opt
    mov rdx, m_bad_opt_len
    call print
    jmp .menu
.view:
    call admin_view
    jmp .menu
.unlock:
    call admin_unlock
    jmp .menu
.reset:
    call admin_reset_pin
    jmp .menu
.hist:
    call admin_history
    jmp .menu
.create:
    call admin_create
    jmp .menu


; list every account with balance and lock state
admin_view:
    push rbx
    push r12
    xor r12, r12
.loop:
    cmp r12, [n_accounts]
    jge .done
    mov rsi, p_two
    mov rdx, p_two_len
    call print
    mov rax, [acct_no + r12*8]
    call print_uint
    mov rsi, p_bal
    mov rdx, p_bal_len
    call print
    mov rax, [acct_bal + r12*8]
    call print_money
    cmp qword [acct_lock + r12*8], 0
    jne .locked
    mov rsi, p_active
    mov rdx, p_active_len
    call print
    jmp .next
.locked:
    mov rsi, p_locked
    mov rdx, p_locked_len
    call print
.next:
    inc r12
    jmp .loop
.done:
    pop r12
    pop rbx
    ret


admin_unlock:
    mov rsi, ask_unlock
    mov rdx, ask_unlock_len
    call print
    call read_number
    jc .cancel
    call find_acct
    jc .no_acct
    cmp qword [acct_lock + r14*8], 0    ; only act if it's actually locked
    je .not_locked
    mov qword [acct_lock + r14*8], 0
    SAVE
    mov rsi, m_unlocked
    mov rdx, m_unlocked_len
    call print
    ret
.not_locked:
    mov rsi, m_not_locked
    mov rdx, m_not_locked_len
    call print
    ret
.no_acct:
    mov rsi, m_no_acct
    mov rdx, m_no_acct_len
    call print
    ret
.cancel:
    mov rsi, m_cancel
    mov rdx, m_cancel_len
    call print
    ret


; admin: wipe an account's PIN so the holder re-enrolls at next login
admin_reset_pin:
    mov rsi, ask_acct
    mov rdx, ask_acct_len
    call print
    call read_number
    jc .cancel
    call find_acct
    jc .no_acct
    mov qword [acct_pin + r14*8], 0
    SAVE
    mov rsi, m_pin_reset
    mov rdx, m_pin_reset_len
    call print
    ret
.no_acct:
    mov rsi, m_no_acct
    mov rdx, m_no_acct_len
    call print
    ret
.cancel:
    mov rsi, m_cancel
    mov rdx, m_cancel_len
    call print
    ret


; admin: show any account's history (borrows cur_acct, then restores it)
admin_history:
    push r12
    mov rsi, ask_acct
    mov rdx, ask_acct_len
    call print
    call read_number
    jc .cancel
    call find_acct
    jc .no_acct
    mov r12, [cur_acct]         ; save whatever was there
    mov [cur_acct], r14
    call show_history
    mov [cur_acct], r12         ; restore
    pop r12
    ret
.no_acct:
    mov rsi, m_no_acct
    mov rdx, m_no_acct_len
    call print
    pop r12
    ret
.cancel:
    mov rsi, m_cancel
    mov rdx, m_cancel_len
    call print
    pop r12
    ret


; admin: add a new account (PIN unset, zero balance) up to capacity
admin_create:
    mov rax, [n_accounts]
    cmp rax, MAX_ACCOUNTS
    jge .full
    mov rsi, ask_newacct
    mov rdx, ask_newacct_len
    call print
    call read_number
    jc .cancel
    call find_acct              ; already taken?
    jnc .exists
    mov r14, [n_accounts]
    mov [acct_no + r14*8], rax
    mov qword [acct_pin + r14*8], 0
    mov qword [acct_bal + r14*8], 0
    mov qword [acct_lock + r14*8], 0
    mov qword [wd_day + r14*8], 0
    mov qword [wd_today + r14*8], 0
    mov qword [hist_n + r14*8], 0
    inc qword [n_accounts]
    SAVE
    mov rsi, m_created
    mov rdx, m_created_len
    call print
    ret
.exists:
    mov rsi, m_exists
    mov rdx, m_exists_len
    call print
    ret
.full:
    mov rsi, m_full
    mov rdx, m_full_len
    call print
    ret
.cancel:
    mov rsi, m_cancel
    mov rdx, m_cancel_len
    call print
    ret


show_balance:
    mov rsi, m_balance
    mov rdx, m_balance_len
    call print
    mov r14, [cur_acct]
    mov rax, [acct_bal + r14*8]
    call print_money
    mov rsi, newline
    mov rdx, 1
    call print
    ret


; print every stored transaction for the current account
show_history:
    push rbx
    push r12
    push r13
    push r14
    mov rsi, h_history
    mov rdx, h_history_len
    call print
    mov r14, [cur_acct]
    imul rbx, r14, HIST_MAX
    mov r12, [hist_n + r14*8]
    test r12, r12
    jz .empty
    xor r13, r13
.loop:
    cmp r13, r12
    jge .done
    lea rax, [rbx + r13]
    mov rdi, r13
    inc rdi
    mov rsi, [hist_type + rax*8]
    mov rdx, [hist_amt + rax*8]
    mov rcx, [hist_time + rax*8]
    call print_txn
    inc r13
    jmp .loop
.empty:
    mov rsi, m_no_txn
    mov rdx, m_no_txn_len
    call print
.done:
    pop r14
    pop r13
    pop r12
    pop rbx
    ret


; same as history but only the last MINI_COUNT lines
show_mini:
    push rbx
    push r12
    push r13
    push r14
    mov rsi, h_mini
    mov rdx, h_mini_len
    call print
    mov r14, [cur_acct]
    imul rbx, r14, HIST_MAX
    mov r12, [hist_n + r14*8]
    test r12, r12
    jz .empty
    xor r13, r13
    cmp r12, MINI_COUNT
    jle .go
    mov r13, r12
    sub r13, MINI_COUNT
.go:
.loop:
    cmp r13, r12
    jge .done
    lea rax, [rbx + r13]
    mov rdi, r13
    inc rdi
    mov rsi, [hist_type + rax*8]
    mov rdx, [hist_amt + rax*8]
    mov rcx, [hist_time + rax*8]
    call print_txn
    inc r13
    jmp .loop
.empty:
    mov rsi, m_no_txn
    mov rdx, m_no_txn_len
    call print
.done:
    pop r14
    pop r13
    pop r12
    pop rbx
    ret


; record a transaction. rdi = type, rsi = amount, rcx = account index.
; stamps it with the current time and keeps the newest HIST_MAX entries.
add_txn:
    push rbx
    push r12
    push r13
    push r14
    mov r14, rcx                ; account index
    mov r12, rdi                ; type
    mov r13, rsi                ; amount
    call now_epoch              ; rax = epoch seconds
    mov rbx, rax                ; timestamp
    imul rcx, r14, HIST_MAX     ; base slot for this account
    mov rax, [hist_n + r14*8]
    cmp rax, HIST_MAX
    jl .append
    ; full: shift everything down one slot
    xor r8, r8
.shift:
    cmp r8, HIST_MAX - 1
    jge .after_shift
    lea r9, [rcx + r8]
    lea r10, [rcx + r8 + 1]
    mov rax, [hist_type + r10*8]
    mov [hist_type + r9*8], rax
    mov rax, [hist_amt + r10*8]
    mov [hist_amt + r9*8], rax
    mov rax, [hist_time + r10*8]
    mov [hist_time + r9*8], rax
    inc r8
    jmp .shift
.after_shift:
    mov rax, HIST_MAX - 1
    jmp .store
.append:
    lea rdx, [rax + 1]
    mov [hist_n + r14*8], rdx
.store:
    add rax, rcx                ; global slot = base + slot
    mov [hist_type + rax*8], r12
    mov [hist_amt + rax*8], r13
    mov [hist_time + rax*8], rbx
    pop r14
    pop r13
    pop r12
    pop rbx
    ret


; one history line. rdi = number, rsi = type, rdx = amount, rcx = epoch.
print_txn:
    push rbx
    push r12
    mov rbx, rdi                ; line number
    mov r8, rsi                 ; type
    mov r9, rdx                 ; amount
    mov r12, rcx                ; timestamp
    mov rsi, p_two
    mov rdx, p_two_len
    call print
    mov rax, rbx
    call print_uint
    mov rsi, p_dot
    mov rdx, p_dot_len
    call print
    cmp r8, 1
    je .dep
    cmp r8, 2
    je .wd
    cmp r8, 3
    je .tout
    mov rsi, p_tin
    mov rdx, p_tin_len
    call print
    jmp .amt
.dep:
    mov rsi, p_dep
    mov rdx, p_dep_len
    call print
    jmp .amt
.wd:
    mov rsi, p_wd
    mov rdx, p_wd_len
    call print
    jmp .amt
.tout:
    mov rsi, p_tout
    mov rdx, p_tout_len
    call print
.amt:
    mov rax, r9
    call print_money
    mov rsi, p_two
    mov rdx, p_two_len
    call print
    mov rax, r12
    call fmt_datetime
    mov rsi, newline
    mov rdx, 1
    call print
    pop r12
    pop rbx
    ret


; print the note breakdown for a withdrawal. rax = amount (a multiple of a note).
dispense:
    push rbx
    push r12
    push r13
    mov r12, rax                ; remaining amount
    mov rsi, h_dispense
    mov rdx, h_dispense_len
    call print
    xor rbx, rbx                ; note index
.loop:
    cmp rbx, NOTE_COUNT
    jge .done
    mov rcx, [note_vals + rbx*8]
    mov rax, r12
    xor rdx, rdx
    div rcx                     ; rax = count, rdx = leftover
    mov r12, rdx
    test rax, rax
    jz .next
    mov r13, rax                ; how many of this note
    mov rsi, p_two
    mov rdx, p_two_len
    call print
    mov rax, r13
    call print_uint
    mov rsi, p_note_x
    mov rdx, p_note_x_len
    call print
    mov rax, [note_vals + rbx*8]
    xor rdx, rdx
    mov rcx, 100
    div rcx                     ; note value back to whole GHS for display
    call print_uint
    mov rsi, newline
    mov rdx, 1
    call print
.next:
    inc rbx
    jmp .loop
.done:
    pop r13
    pop r12
    pop rbx
    ret


; ---- helpers ----

; look up an account number. in: rax = number. out: r14 = index, CF=0 on hit;
; CF=1 and r14 = n_accounts if not found.
find_acct:
    xor r14, r14
.loop:
    cmp r14, [n_accounts]
    jge .miss
    cmp rax, [acct_no + r14*8]
    je .hit
    inc r14
    jmp .loop
.hit:
    clc
    ret
.miss:
    stc
    ret


; salted splitmix64 of a PIN. in: rax = PIN. out: rax = hash (never 0).
hash_pin:
    push rbx
    add rax, PIN_SALT
    mov rbx, 0x9E3779B97F4A7C15
    add rax, rbx
    mov rcx, rax
    shr rcx, 30
    xor rax, rcx
    mov rbx, 0xBF58476D1CE4E5B9
    imul rax, rbx
    mov rcx, rax
    shr rcx, 27
    xor rax, rcx
    mov rbx, 0x94D049BB133111EB
    imul rax, rbx
    mov rcx, rax
    shr rcx, 31
    xor rax, rcx
    test rax, rax
    jnz .ok
    inc rax                     ; 0 is the "no PIN" marker, so avoid it
.ok:
    pop rbx
    ret


; check a PIN is exactly 4 digits. in: rax = PIN. CF=1 if out of range.
check_pin_range:
    cmp rax, PIN_MIN
    jb .bad
    cmp rax, PIN_MAX
    ja .bad
    clc
    ret
.bad:
    stc
    ret


; seconds since the Unix epoch, in rax
now_epoch:
    xor rdi, rdi
    mov rax, SYS_time
    syscall
    ret


; current day number (epoch / 86400), in rax
today_number:
    call now_epoch
    xor rdx, rdx
    mov rcx, 86400
    div rcx
    ret


; ---- low level I/O ----

; write rsi[0..rdx) to stdout
print:
    mov rax, SYS_write
    mov rdi, 1
    syscall
    ret


; install SIGINT/SIGTERM handlers so the terminal never gets stuck with echo off
install_sigs:
    mov rax, SYS_rt_sigaction
    mov rdi, 2                  ; SIGINT
    mov rsi, sigact
    xor rdx, rdx
    mov r10, 8
    syscall
    mov rax, SYS_rt_sigaction
    mov rdi, 15                 ; SIGTERM
    mov rsi, sigact
    xor rdx, rdx
    mov r10, 8
    syscall
    ret

; on a signal: turn echo back on, then exit. runs on the signal stack, but all
; it does is two ioctls and exit, so we don't need our own stack frame.
sig_handler:
    mov rax, SYS_ioctl
    xor rdi, rdi
    mov rsi, TCGETS
    mov rdx, termbuf
    syscall
    or dword [termbuf + 12], ECHO_BIT
    mov rax, SYS_ioctl
    xor rdi, rdi
    mov rsi, TCSETS
    mov rdx, termbuf
    syscall
    mov rsi, newline
    mov rdx, 1
    call print
    mov rax, SYS_exit
    mov rdi, 130
    syscall

; the kernel needs a restorer to return through even though we never return
sig_restorer:
    mov rax, SYS_rt_sigreturn
    syscall


; read a PIN with the typed digits hidden.
; turns off terminal echo, calls read_number, then restores echo.
; if stdin isn't a terminal (piped input) it just reads normally.
; returns exactly what read_number returns (rax, CF, rdx).
read_pin:
    mov rax, SYS_ioctl
    mov rdi, 0
    mov rsi, TCGETS
    mov rdx, termbuf
    syscall
    test rax, rax
    js .plain                   ; not a tty, don't bother masking

    mov eax, [termbuf + 12]     ; c_lflag
    mov [saved_lflag], eax
    and dword [termbuf + 12], ~ECHO_BIT & 0xFFFFFFFF   ; clear ECHO bit
    mov rax, SYS_ioctl
    mov rdi, 0
    mov rsi, TCSETS
    mov rdx, termbuf
    syscall

    call read_number

    pushfq                      ; hold onto read_number's result
    push rax
    push rdx
    mov eax, [saved_lflag]      ; put echo back
    mov [termbuf + 12], eax
    mov rax, SYS_ioctl
    mov rdi, 0
    mov rsi, TCSETS
    mov rdx, termbuf
    syscall
    mov rsi, newline            ; the Enter wasn't echoed, so add a newline
    mov rdx, 1
    call print
    pop rdx
    pop rax
    popfq
    ret
.plain:
    jmp read_number


; next byte from stdin, buffered, with an idle timeout.
; out: al = byte, CF = 0. on failure CF = 1 and in_status = 0 (eof) / 1 (timeout).
; poll() before each refill so an idle terminal eventually times out. Piped
; input is always ready, so the timeout never fires during automated runs.
get_char:
    mov rcx, [rd_pos]
    cmp rcx, [rd_len]
    jb .have
    mov dword [pfd], 0
    mov word [pfd + 4], 1       ; POLLIN
    mov word [pfd + 6], 0
    mov rax, SYS_poll
    mov rdi, pfd
    mov rsi, 1
    mov rdx, TIMEOUT_MS
    syscall
    test rax, rax
    jz .timeout
    js .eof
    xor rax, rax                ; read
    xor rdi, rdi
    mov rsi, rdbuf
    mov rdx, 256
    syscall
    test rax, rax
    jle .eof
    mov [rd_len], rax
    xor rcx, rcx
    mov [rd_pos], rcx
.have:
    movzx rax, byte [rdbuf + rcx]
    inc rcx
    mov [rd_pos], rcx
    clc
    ret
.timeout:
    mov byte [in_status], 1
    stc
    ret
.eof:
    mov byte [in_status], 0
    stc
    ret


; read one line and parse it as an unsigned integer.
; out: rax = value (CF=0). CF=1 with rdx = 0 invalid, 1 eof, 2 timeout.
; a line with any non-digit (or one too long to be safe) is swallowed to the
; newline so we stay in sync. value builds in rbx, digit count in r13 (saved).
read_number:
    push rbx
    push r13
    xor rbx, rbx
    xor r13, r13
.loop:
    call get_char
    jc .nochar
    cmp al, 10
    je .line_end
    cmp al, 13
    je .loop
    cmp al, '0'
    jb .bad_char
    cmp al, '9'
    ja .bad_char
    cmp r13, MAX_DIGITS         ; too many digits -> would overflow, reject
    jae .bad_char
    movzx rcx, al
    sub rcx, '0'
    imul rbx, rbx, 10
    add rbx, rcx
    inc r13
    jmp .loop
.line_end:
    test r13, r13
    jz .invalid
    mov rax, rbx
    clc
    jmp .ret
.nochar:
    cmp byte [in_status], 1
    je .timeout
    test r13, r13               ; digits with no trailing newline are still valid
    jz .eof
    mov rax, rbx
    clc
    jmp .ret
.bad_char:
.flush:
    call get_char
    jc .flush_end
    cmp al, 10
    jne .flush
.invalid:
    xor rdx, rdx
    stc
    jmp .ret
.flush_end:
    cmp byte [in_status], 1
    je .timeout
.eof:
    mov rdx, 1
    stc
    jmp .ret
.timeout:
    mov rdx, 2
    stc
.ret:
    pop r13
    pop rbx
    ret


; read a line and parse it as a GHS amount, e.g. "12.50", "12", ".5".
; returns the value in pesewas. same result convention as read_number:
; rax = value (CF=0); CF=1 with rdx = 0 invalid, 1 eof, 2 timeout.
; rbx accumulates every digit, r13 counts them, r14 tracks the decimal point
; (-1 = none seen yet, otherwise the number of fractional digits so far).
read_money:
    push rbx
    push r13
    push r14
    xor rbx, rbx
    xor r13, r13
    mov r14, -1
.loop:
    call get_char
    jc .nochar
    cmp al, 10
    je .line_end
    cmp al, 13
    je .loop
    cmp al, '.'
    je .dot
    cmp al, '0'
    jb .bad_char
    cmp al, '9'
    ja .bad_char
    cmp r13, MAX_DIGITS
    jae .bad_char
    movzx rcx, al
    sub rcx, '0'
    imul rbx, rbx, 10
    add rbx, rcx
    inc r13
    cmp r14, 0
    jl .loop                    ; still in the whole-cedi part
    inc r14                     ; count a digit after the point
    cmp r14, 2
    jg .bad_char                ; more than 2 decimals isn't a valid amount
    jmp .loop
.dot:
    cmp r14, -1
    jne .bad_char               ; a second '.' is invalid
    xor r14, r14                ; point seen, no fractional digits yet
    jmp .loop
.line_end:
    test r13, r13
    jz .invalid
    jmp .scale
.nochar:
    cmp byte [in_status], 1
    je .timeout
    test r13, r13
    jz .eof
.scale:
    ; pesewas = digits * 10^(2 - fractional_digits)
    cmp r14, 1
    je .mul10
    jg .done                    ; r14 == 2, already pesewas
    imul rbx, rbx, 100          ; r14 <= 0, whole cedis
    jmp .done
.mul10:
    imul rbx, rbx, 10
.done:
    mov rax, rbx
    clc
    jmp .ret
.bad_char:
.flush:
    call get_char
    jc .flush_end
    cmp al, 10
    jne .flush
.invalid:
    xor rdx, rdx
    stc
    jmp .ret
.flush_end:
    cmp byte [in_status], 1
    je .timeout
.eof:
    mov rdx, 1
    stc
    jmp .ret
.timeout:
    mov rdx, 2
    stc
.ret:
    pop r14
    pop r13
    pop rbx
    ret


; print rax as plain decimal, no newline
print_uint:
    mov rcx, 10
    lea rsi, [numbuf + 32]
.div:
    xor rdx, rdx
    div rcx
    add dl, '0'
    dec rsi
    mov [rsi], dl
    test rax, rax
    jnz .div
    lea rdx, [numbuf + 32]
    sub rdx, rsi
    mov rax, SYS_write
    mov rdi, 1
    syscall
    ret


; print rax as a zero-padded two-digit number (rax < 100)
print_pad2:
    xor rdx, rdx
    mov rcx, 10
    div rcx                     ; rax = tens, rdx = units
    add al, '0'
    mov [numbuf], al
    add dl, '0'
    mov [numbuf + 1], dl
    mov rax, SYS_write
    mov rdi, 1
    mov rsi, numbuf
    mov rdx, 2
    syscall
    ret


; print an amount in pesewas as GHS with two decimals, e.g. 1,234.56
print_money:
    push rbx
    xor rdx, rdx
    mov rcx, 100
    div rcx                     ; rax = whole GHS, rdx = pesewas
    mov rbx, rdx
    call print_grouped
    mov rsi, p_point
    mov rdx, 1
    call print
    mov rax, rbx
    call print_pad2
    pop rbx
    ret

; print rax as a plain integer with thousands separators, e.g. 1,234
print_grouped:
    mov rcx, 10
    lea rsi, [numbuf + 32]
    xor r8, r8                  ; digits since the last comma
.div:
    xor rdx, rdx
    div rcx
    add dl, '0'
    dec rsi
    mov [rsi], dl
    test rax, rax
    jz .done
    inc r8
    cmp r8, 3
    jne .div
    dec rsi
    mov byte [rsi], ','
    xor r8, r8
    jmp .div
.done:
    lea rdx, [numbuf + 32]
    sub rdx, rsi
    mov rax, SYS_write
    mov rdi, 1
    syscall
    ret


; print an epoch time as "YYYY-MM-DD HH:MM:SS" (UTC).
; date part uses the standard civil-from-days calculation; every intermediate
; is stashed in memory so the internal prints don't clobber it.
fmt_datetime:
    mov rcx, 86400
    xor rdx, rdx
    div rcx                     ; rax = days, rdx = seconds into day
    mov [dt_secs], rdx
    add rax, 719468             ; shift epoch to the 0000-03-01 era
    mov rcx, 146097
    xor rdx, rdx
    div rcx
    mov [dt_era], rax
    mov [dt_doe], rdx
    ; yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365
    mov rax, [dt_doe]
    mov rcx, 1460
    xor rdx, rdx
    div rcx
    mov [dt_t1], rax
    mov rax, [dt_doe]
    mov rcx, 36524
    xor rdx, rdx
    div rcx
    mov [dt_t2], rax
    mov rax, [dt_doe]
    mov rcx, 146096
    xor rdx, rdx
    div rcx
    mov [dt_t3], rax
    mov rax, [dt_doe]
    sub rax, [dt_t1]
    add rax, [dt_t2]
    sub rax, [dt_t3]
    mov rcx, 365
    xor rdx, rdx
    div rcx
    mov [dt_yoe], rax
    ; y = yoe + era*400
    mov rax, [dt_era]
    imul rax, rax, 400
    add rax, [dt_yoe]
    mov [dt_y], rax
    ; doy = doe - (365*yoe + yoe/4 - yoe/100)
    mov rax, [dt_yoe]
    imul rax, rax, 365
    mov [dt_t1], rax
    mov rax, [dt_yoe]
    mov rcx, 4
    xor rdx, rdx
    div rcx
    mov [dt_t2], rax
    mov rax, [dt_yoe]
    mov rcx, 100
    xor rdx, rdx
    div rcx
    mov [dt_t3], rax
    mov rax, [dt_doe]
    sub rax, [dt_t1]
    sub rax, [dt_t2]
    add rax, [dt_t3]
    mov [dt_doy], rax
    ; mp = (5*doy + 2) / 153
    mov rax, [dt_doy]
    imul rax, rax, 5
    add rax, 2
    mov rcx, 153
    xor rdx, rdx
    div rcx
    mov [dt_mp], rax
    ; d = doy - (153*mp + 2)/5 + 1
    mov rax, [dt_mp]
    imul rax, rax, 153
    add rax, 2
    mov rcx, 5
    xor rdx, rdx
    div rcx
    mov rcx, [dt_doy]
    sub rcx, rax
    add rcx, 1
    mov [dt_d], rcx
    ; m = mp < 10 ? mp + 3 : mp - 9
    mov rax, [dt_mp]
    cmp rax, 10
    jb .m_low
    sub rax, 9
    jmp .m_set
.m_low:
    add rax, 3
.m_set:
    mov [dt_m], rax
    cmp qword [dt_m], 2         ; Jan/Feb belong to the next year
    ja .no_bump
    inc qword [dt_y]
.no_bump:
    ; split seconds into hh:mm:ss
    mov rax, [dt_secs]
    mov rcx, 3600
    xor rdx, rdx
    div rcx
    mov [dt_hh], rax
    mov rax, rdx
    mov rcx, 60
    xor rdx, rdx
    div rcx
    mov [dt_mm], rax
    mov [dt_ss], rdx
    ; print it
    mov rax, [dt_y]
    call print_uint
    call .p_dash
    mov rax, [dt_m]
    call print_pad2
    call .p_dash
    mov rax, [dt_d]
    call print_pad2
    mov rsi, p_two
    mov rdx, 1                  ; single space
    call print
    mov rax, [dt_hh]
    call print_pad2
    call .p_colon
    mov rax, [dt_mm]
    call print_pad2
    call .p_colon
    mov rax, [dt_ss]
    call print_pad2
    ret
.p_dash:
    mov rsi, p_dash
    mov rdx, 1
    call print
    ret
.p_colon:
    mov rsi, p_colon
    mov rdx, 1
    call print
    ret


; populate the state block with the demo accounts (fresh start / reset).
init_defaults:
    mov qword [n_accounts], N_ACCOUNTS
    ; wipe every slot first, then fill in the demo accounts. PINs stay 0
    ; (unset), so each holder picks their own PIN on first login.
    xor rcx, rcx
.clear:
    cmp rcx, MAX_ACCOUNTS
    jge .accts
    mov qword [acct_no + rcx*8], 0
    mov qword [acct_pin + rcx*8], 0
    mov qword [acct_bal + rcx*8], 0
    mov qword [acct_lock + rcx*8], 0
    mov qword [wd_day + rcx*8], 0
    mov qword [wd_today + rcx*8], 0
    mov qword [hist_n + rcx*8], 0
    inc rcx
    jmp .clear
.accts:
    mov qword [acct_no + 0*8], 1001
    mov qword [acct_no + 1*8], 1002
    mov qword [acct_no + 2*8], 1003
    mov qword [acct_no + 3*8], 1004
    mov qword [acct_no + 4*8], 1005
    mov qword [acct_no + 5*8], 1006
    mov qword [acct_bal + 0*8], 1000 * 100          ; balances in pesewas
    mov qword [acct_bal + 1*8], 5000 * 100
    mov qword [acct_bal + 2*8], 250 * 100
    mov qword [acct_bal + 3*8], 750 * 100
    mov qword [acct_bal + 4*8], 3200 * 100 + 75     ; GHS 3,200.75
    mov qword [acct_bal + 5*8], 100 * 100
    ret


%ifdef PERSISTENT
; ---- persistence ----

; load the state file into the state block. if it's missing or not exactly
; STATE_SIZE bytes, fall back to defaults and write a fresh file.
load_state:
    push r12
    push r13
    mov rax, SYS_open
    mov rdi, filename
    mov rsi, O_RDONLY
    xor rdx, rdx
    syscall
    test rax, rax
    js .fresh
    mov r12, rax                ; fd
    mov rax, SYS_read
    mov rdi, r12
    mov rsi, state_begin
    mov rdx, STATE_SIZE
    syscall
    mov r13, rax                ; bytes read
    mov rax, SYS_close
    mov rdi, r12
    syscall
    cmp r13, STATE_SIZE
    jne .fresh
    pop r13
    pop r12
    ret
.fresh:
    call init_defaults
    call save_state
    pop r13
    pop r12
    ret


; write the whole state block to the file (create/truncate). best-effort.
save_state:
    push r12
    mov rax, SYS_open
    mov rdi, filename
    mov rsi, O_WRCREATTRUNC
    mov rdx, FILE_MODE
    syscall
    test rax, rax
    js .done
    mov r12, rax                ; fd
    mov rax, SYS_write
    mov rdi, r12
    mov rsi, state_begin
    mov rdx, STATE_SIZE
    syscall
    mov rax, SYS_close
    mov rdi, r12
    syscall
.done:
    pop r12
    ret
%endif


; idle-timeout path back to the main menu
timeout_logout:
    mov rsi, m_timeout
    mov rdx, m_timeout_len
    call print
    jmp welcome


; goodbye, session summary, exit
quit:
    mov rsi, m_bye
    mov rdx, m_bye_len
    call print
    mov rsi, m_summary
    mov rdx, m_summary_len
    call print
    mov rax, [session_txn]
    call print_uint
    mov rsi, newline
    mov rdx, 1
    call print
    xor rdi, rdi
    mov rax, SYS_exit
    syscall

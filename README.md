# atm_system
An ATM System built using Assembly Language
Group 7(FF17)

Scope:
Here's the scope, based on what we discussed:
ATM System (x86-64 NASM, Linux, terminal-based)

Core Features (required)

    PIN entry and verification (max 3 attempts, then lock/exit)
    Check balance
    Deposit money
    Withdraw money (must check for insufficient funds)
    Exit system

Extended Features (for extra marks)
6. Multiple accounts (user selects account number, then enters PIN)
7. Transaction history (log deposits/withdrawals, print on request)
8. Change PIN
9. Withdrawal limit per transaction (e.g. max amount per single withdrawal)

Extra features
10. Mini statement (last N transactions only, not full history)
11. Account balance printed with proper formatting (not just raw number)
12. Input validation (reject letters when a number is expected, reject negative amounts)
13. "Session timeout" simulation (after inactivity or wrong menu choice, log out)
14. Admin mode (separate PIN, can view all accounts, unlock locked accounts)

Technical/presentation extras
15. ASCII art banner on startup (small effort, looks polished)
16. Clear exit message with basic session summary (e.g. "You made 2 transactions today")
17. Code comments explaining each function, well organized into labeled sections (this matters for markers reading assembly)

Technical constraints

    Language: x86-64 Assembly (NASM syntax)
    Assembler: NASM 2.16.03
    Linker: GNU ld
    Platform: Linux (Debian), uses Linux syscalls directly (no libc, no DOS interrupts)
    Interface: terminal/console only, text menu driven

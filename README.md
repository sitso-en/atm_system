# ATM System — Group 7 (FF17)

An ATM simulation written in **x86-64 assembly (NASM)** for Linux. It talks to
the kernel directly through syscalls — no libc, no DOS interrupts — and runs
entirely in the terminal.

## Layout

```
common/atm_core.inc   the whole program: one shared source of truth
in_memory/atm.asm     thin wrapper -> in-memory build (state resets each run)
persistent/atm.asm    thin wrapper -> persistent build (state saved to a file)
tests/                golden-output test suite
Makefile              top-level: build both, run tests
```

Both builds are the **same code**. The persistent build just defines
`PERSISTENT` before including the core, which switches on load/save. Because
there is only one source file, the two builds can never drift apart.

## Build and run

```sh
make            # build both variants
make test       # build, then run the test suite
make clean      # remove all build artifacts

# or build one variant on its own:
cd in_memory && make && ./atm      # or: cd persistent && make run
```

Requires `nasm` (2.16+) and GNU `ld`.

## Accounts

Six demo accounts are created on a fresh start (admin can add more, up to the
table capacity):

| Account | Opening balance |
|---------|-----------------|
| 1001    | GHS 1,000.00    |
| 1002    | GHS 5,000.00    |
| 1003    | GHS 250.00      |
| 1004    | GHS 750.00      |
| 1005    | GHS 3,200.75    |
| 1006    | GHS 100.00      |

**PINs are not preset.** The first time an account logs in, the holder chooses
their own 4-digit PIN. PINs are never stored in the clear — only a salted hash
is kept, including on disk in the persistent build.

Admin PIN: `9999`. In the persistent build, delete `atm_data.dat` to reset to
the defaults above.

## Features

Customer:

- First-use PIN enrolment (pick + confirm a 4-digit PIN) and change PIN
- Balance, deposit, withdraw
- **Fund transfer** to another account
- Amounts accept pesewas, e.g. `12.50` (money is stored as integer minor units —
  pesewas — so there's no floating point; balances print to two decimals)
- Withdrawals dispensed as a note breakdown (GHS 200/100/50/20/10), so a
  withdrawal must be a whole number of GHS 10 notes
- Per-withdrawal limit (GHS 1,000) and a **cumulative daily limit** (GHS 2,000)
- Full history and mini statement, each line **timestamped** (`YYYY-MM-DD HH:MM:SS`)

Admin (separate PIN):

- View all accounts, unlock a locked account
- Reset a customer's PIN (forces re-enrolment at next login)
- View any account's history
- Create a new account (up to the table capacity)

Safety / robustness:

- PIN entry is **masked** (terminal echo off) on a real terminal; falls back to
  plain reads when input is piped
- A `SIGINT`/`SIGTERM` handler restores terminal echo before exiting, so
  Ctrl-C during PIN entry can't leave your shell with typing invisible
- Input is validated; over-long numbers are rejected to avoid 64-bit overflow
- 3 wrong PINs locks the account; idle sessions time out; too many bad menu
  choices log out
- Session summary on exit

## Persistent build

All mutable state (account numbers, PIN hashes, balances, lock flags, daily
withdrawal tallies, and transaction history) lives in one contiguous block that
is saved to `atm_data.dat` as a single blob. It is loaded at startup and
rewritten after every committed change, so a crash never loses a saved change.
If the file is missing or the wrong size, the program falls back to the demo
accounts and writes a fresh file.

## Tests

`tests/run_tests.sh` feeds scripted input to both builds and compares the output
against saved golden files (timestamps are normalised so runs are comparable).
Persistent cases run in a throwaway directory and one case runs the binary twice
to prove state survives across runs.

```sh
make test                 # run the suite
UPDATE=1 ./tests/run_tests.sh   # regenerate goldens after an intended change
```

CI (`.github/workflows/ci.yml`) builds both variants and runs the suite on every
push.

## Known limitations

- PIN hashing is a simple salted hash for the exercise, not a real
  password-hashing scheme (no per-user salt, not memory-hard).
- Accounts can be created but not deleted; the account table is a fixed-capacity
  array.

## Technical constraints

- Language: x86-64 assembly (NASM syntax); Assembler: NASM 2.16.03; Linker: GNU ld
- Platform: Linux, direct syscalls, no libc
- Interface: terminal only, text-menu driven

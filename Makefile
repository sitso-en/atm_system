# Top-level Makefile for the Group 7 ATM.
# Builds both variants from the single shared core in common/atm_core.inc.
#
#   make            build both the in-memory and persistent binaries
#   make test       build, then run the golden-output test suite
#   make clean      remove all build artifacts

.PHONY: all inmem persist test clean

all: inmem persist

inmem:
	$(MAKE) -C in_memory

persist:
	$(MAKE) -C persistent

test: all
	./tests/run_tests.sh

clean:
	$(MAKE) -C in_memory clean
	$(MAKE) -C persistent clean

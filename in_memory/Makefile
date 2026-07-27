# Makefile - ATM system (in-memory build)
# Shared source lives in ../common; -I lets NASM find atm_core.inc.

NASM   = nasm
LD     = ld
TARGET = atm
OBJ    = atm.o
SRC    = atm.asm
COMMON = ../common/atm_core.inc

NASMFLAGS = -f elf64 -g -F dwarf -I../common/

all: $(TARGET)

$(TARGET): $(OBJ)
	$(LD) -o $(TARGET) $(OBJ)

$(OBJ): $(SRC) $(COMMON)
	$(NASM) $(NASMFLAGS) -o $(OBJ) $(SRC)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all run clean

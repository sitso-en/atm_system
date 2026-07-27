# Makefile - ATM system (persistent build)
# Shared source lives in ../common; -I lets NASM find atm_core.inc.
# State is saved to atm_data.dat in the working directory.

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
	rm -f $(OBJ) $(TARGET) atm_data.dat

.PHONY: all run clean

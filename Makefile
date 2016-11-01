CC = gcc
INCFLAGS = -I./
DEFS = -D NOOMR_ALIGN_HEADERS -D COLLECT_STATS -D NOOMR_SYSTEM_ALLOC
OPT_FLAGS = -O0 -fno-inline
DEBUG = -pg -ggdb3 -ggdb -g
CFLAGS = $(OPT_FLAGS) $(DEBUG_FLAGS) -Wall -Wno-unused-function -Wno-deprecated-declarations -march=native $(INCFLAGS) $(DEFS)
TEST_BINARIES = $(basename $(wildcard tests/*.c))
OBJECTS = noomr.o memmap.o
LDFLAGS = -Wl,--no-as-needed -lm -ldl

default: $(OBJECTS)

memmap.o: memmap.c memmap.h
noomr.o: noomr.c noomr.h stack.h memmap.h

# stack tests doesn't depend on the whole system -- special case
tests/stack_%: tests/stack_%.c stack.h
	$(CC) $(CFLAGS) $? -o $@ $(LDFLAGS)

tests/%: tests/%.c $(OBJECTS)
	$(CC) $(CFLAGS) $? -o $@ $(LDFLAGS)

tests: $(TEST_BINARIES)

test: $(TEST_BINARIES)
	@$(foreach tb, $?, echo ./$(tb) && ./$(tb) )

clean:
	rm -f tests/*.o $(TEST_BINARIES) $(OBJECTS)

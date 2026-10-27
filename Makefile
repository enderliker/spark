CC      ?= cc
CFLAGS  ?= -std=c11 -O3 -g -Wall -Wextra -Wpedantic
CPPFLAGS += -Isrc -D_POSIX_C_SOURCE=200809L -MMD -MP
LDLIBS  += -lm

BIN := spark
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEPS := $(OBJ:.o=.d)

.PHONY: all clean test coverage

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(DEPS) $(BIN) src/*.gcda src/*.gcno *.gcov coverage.info

test: $(BIN)
	./tests/run_tests.sh

coverage:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=c11 -O0 -g --coverage -Wall -Wextra -Wpedantic" LDFLAGS="--coverage"
	python3 tests/run_tests.py
	gcov src/*.c

-include $(DEPS)

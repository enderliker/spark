CC      ?= cc
CFLAGS  ?= -std=c11 -O3 -g -Wall -Wextra -Wpedantic
CPPFLAGS += -Isrc -D_POSIX_C_SOURCE=200809L -MMD -MP
LDLIBS  += -lm

BIN := chispa
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEPS := $(OBJ:.o=.d)

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(DEPS) $(BIN)

test: $(BIN)
	./tests/run_tests.sh

-include $(DEPS)

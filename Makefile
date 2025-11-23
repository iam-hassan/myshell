CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -std=c17 -g
LDFLAGS ?=

SRC := src/myshell.c
TARGET := myshell

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)


# Intel macOS CPU-frequency reader.
# Build: make
# Sample: make run INTERVAL_MS=1000 SAMPLES=5

CC = clang
CPPFLAGS ?=
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
LDFLAGS ?=
LDLIBS ?=

TARGET := MacMhz
SOURCE := MacMhz.c
INTERVAL_MS ?= 1000
SAMPLES ?= 5

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SOURCE) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(SOURCE) $(LDLIBS) -o $@

run: $(TARGET)
	./$(TARGET) $(INTERVAL_MS) $(SAMPLES)

clean:
	$(RM) $(TARGET)

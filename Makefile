CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude
LDFLAGS ?=

# X11 flags
X11_CFLAGS := $(shell pkg-config --cflags x11 2>/dev/null)
X11_LIBS   := $(shell pkg-config --libs x11 2>/dev/null)

ifeq ($(strip $(X11_LIBS)),)
    X11_LIBS := -lX11
endif

CFLAGS  += $(X11_CFLAGS)
LDFLAGS += $(X11_LIBS)

SRC_DIR := src
OBJ_DIR := obj
BIN := terminator

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

test: $(BIN)
	@echo "Running Terminator Test Suite..."
	@bash tests/test_commands.sh
	@bash tests/test_redirection.sh
	@bash tests/test_pipelines.sh
	@bash tests/test_multiline.sh
	@echo "All automated tests completed successfully!"

clean:
	rm -rf $(OBJ_DIR) $(BIN) *.o
	rm -f tests/*.tmp tests/out*.txt tests/res*.txt tests/sort*.txt

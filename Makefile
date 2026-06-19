# pgforge — schema-driven backend (Postgres + C)
# Single static-ish binary. Builds vendored libs (wslib, opcode_dispatcher, logger)
# then the pgforge sources, and links them together.

CC       := gcc
CSTD     := -std=gnu11
WARN     := -Wall -Wextra -Wno-unused-parameter
OPT      := -O2 -g
DEFS     := -D_GNU_SOURCE
CFLAGS   := $(CSTD) $(WARN) $(OPT) $(DEFS)

BUILD    := build
OBJ      := $(BUILD)/obj
BIN      := $(BUILD)/pgforge

# ---- vendored libraries -----------------------------------------------------
LIB_WS   := lib/wslib
LIB_DISP := lib/opcode_dispatcher
LIB_LOG  := lib/logger

LIB_INCLUDES := -I$(LIB_WS)/include -I$(LIB_WS)/src -I$(LIB_WS)/src/internal \
                -I$(LIB_DISP)/include -I$(LIB_LOG)/include

INCLUDES := $(LIB_INCLUDES) -Isrc -Isrc/core -I/usr/include/postgresql

# System libraries. (pq/sodium/crypto/cjson/uuid pulled in as the engine grows.)
LDLIBS   := -pthread -lpq -lsodium -lcrypto -lcjson -luuid

# ---- sources ----------------------------------------------------------------
LIB_SRCS := $(wildcard $(LIB_WS)/src/*.c) \
            $(wildcard $(LIB_DISP)/src/*.c) \
            $(wildcard $(LIB_LOG)/src/*.c)
APP_SRCS := $(wildcard src/*.c) $(wildcard src/core/*.c) \
            $(wildcard src/engine/*.c) $(wildcard src/handlers/*.c)

LIB_OBJS := $(patsubst %.c,$(OBJ)/%.o,$(LIB_SRCS))
APP_OBJS := $(patsubst %.c,$(OBJ)/%.o,$(APP_SRCS))
ALL_OBJS := $(LIB_OBJS) $(APP_OBJS)

# ---- targets ----------------------------------------------------------------
.PHONY: all clean run
all: $(BIN)

$(BIN): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ALL_OBJS) -o $@ $(LDLIBS)
	@echo "==> built $@"

# Pattern rule: any .c -> build/obj/<path>.o, creating dirs as needed.
$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD)

run: $(BIN)
	$(BIN)

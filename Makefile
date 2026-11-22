# Caro Online — Group 13. Everything is built into build/.
#
#   make              all binaries (default)
#   make server       build/caro_server
#   make client       build/caro_client                 (needs GTK 3 through pkg-config)
#   make tools        build/cli build/bot build/test_frame
#   make test         build and run test_frame, test_game, test_elo
#   make debug        all binaries with -O0 -fsanitize=address,undefined
#   make DEBUG=1 test the tests with the sanitizers
#   make clean
#
# GNU make on Ubuntu (gcc) and macOS (Apple clang, Homebrew GTK 3).

CFLAGS   := -std=c11 -Wall -Wextra -g -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Icommon
LDFLAGS  :=
# -MMD -MP write a .d file next to each object, so editing a header rebuilds its users.
DEPFLAGS := -MMD -MP

ifeq ($(DEBUG),1)
CFLAGS   += -O0 -fsanitize=address,undefined
LDFLAGS  += -fsanitize=address,undefined
endif

# "=" (not ":=") so pkg-config only runs when the client is built: `make server` works on a
# machine without GTK.
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   = $(shell pkg-config --libs gtk+-3.0)
SERVER_LIBS := -lsqlite3 -lm

BUILD := build
OBJ   := $(BUILD)/obj

# Every .c directly in server/ or client/ belongs to that program, so a new file there needs
# no Makefile change. server/tests/ is not matched by server/*.c.
COMMON_SRC := common/frame.c
SERVER_SRC := $(wildcard server/*.c)
CLIENT_SRC := $(wildcard client/*.c)
TOOLS      := cli bot test_frame

COMMON_OBJ := $(COMMON_SRC:%.c=$(OBJ)/%.o)
SERVER_OBJ := $(SERVER_SRC:%.c=$(OBJ)/%.o)
CLIENT_OBJ := $(CLIENT_SRC:%.c=$(OBJ)/%.o)
TOOLS_OBJ  := $(TOOLS:%=$(OBJ)/tools/%.o)
TESTS_OBJ  := $(OBJ)/server/tests/test_game.o $(OBJ)/server/tests/test_elo.o

SERVER_BIN := $(BUILD)/caro_server
CLIENT_BIN := $(BUILD)/caro_client
TOOL_BINS  := $(TOOLS:%=$(BUILD)/%)
# tools/bot is the only multithreaded program (one pthread per bot, Lec06): -pthread is given to
# it alone, when compiling bot.c and when linking build/bot. A plain variable, not a
# target-specific one: those are inherited by prerequisites (frame.o, build/.flags).
BOT_BIN       := $(BUILD)/bot
PTHREAD_FLAGS := -pthread
TEST_BINS  := $(BUILD)/test_frame $(BUILD)/test_game $(BUILD)/test_elo

.PHONY: all server client tools test debug clean

all: server client tools $(TEST_BINS)

server: $(SERVER_BIN)
client: $(CLIENT_BIN)
tools:  $(TOOL_BINS)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done
	@echo "== all tests passed"

debug:
	@$(MAKE) --no-print-directory DEBUG=1 all

clean:
	rm -rf $(BUILD)

# ---- link ---------------------------------------------------------------------------------
# Libraries go after the objects: the GNU linker on Ubuntu resolves symbols left to right.

$(SERVER_BIN): $(SERVER_OBJ) $(COMMON_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@ $(SERVER_LIBS)

$(CLIENT_BIN): $(CLIENT_OBJ) $(COMMON_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@ $(GTK_LIBS)

$(filter-out $(BOT_BIN),$(TOOL_BINS)): $(BUILD)/%: $(OBJ)/tools/%.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(BOT_BIN): $(OBJ)/tools/bot.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@ $(PTHREAD_FLAGS)

# Server unit tests link only the pure module they test (no sockets, no DB).
$(BUILD)/test_game: $(OBJ)/server/tests/test_game.o $(OBJ)/server/game.o
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/test_elo: $(OBJ)/server/tests/test_elo.o $(OBJ)/server/elo.o
	$(CC) $(LDFLAGS) $^ -o $@ -lm

# ---- compile ------------------------------------------------------------------------------
# Specific pattern rules first: GNU make 3.81 (macOS) takes the first matching pattern rule.

$(OBJ)/client/%.o: client/%.c $(BUILD)/.flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJ)/server/tests/%.o: server/tests/%.c $(BUILD)/.flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Iserver $(DEPFLAGS) -c $< -o $@

# An explicit rule, so it wins over the generic pattern rule below.
$(OBJ)/tools/bot.o: tools/bot.c $(BUILD)/.flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(PTHREAD_FLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJ)/%.o: %.c $(BUILD)/.flags
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# build/.flags holds the current compiler flags and is rewritten only when they change, so
# switching between `make debug` and `make` rebuilds every object instead of mixing them.
$(BUILD)/.flags: FORCE
	@mkdir -p $(@D)
	@echo '$(CC) $(CFLAGS) $(LDFLAGS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS) $(LDFLAGS)' > $@

FORCE:

-include $(COMMON_OBJ:.o=.d) $(SERVER_OBJ:.o=.d) $(CLIENT_OBJ:.o=.d) \
         $(TOOLS_OBJ:.o=.d) $(TESTS_OBJ:.o=.d)

include config.mk

VERSION = 0.1.0
UNAME_S := $(shell uname -s)

SRC_C = src/core/main.c src/core/util.c src/core/db.c src/core/dispatch.c \
        src/llm/inference.c src/llm/model.c src/llm/registry.c \
        src/ui/tui.c src/ui/input.c src/ui/event.c src/ui/status.c \
        src/agent/lua_bridge.c src/agent/run.c src/agent/parser.c \
        src/agent/loop.c src/agent/guards.c

SRC_CXX = src/llm/chat_native.cpp

OBJ = $(SRC_C:.c=.o) $(SRC_CXX:.cpp=.o)
BIN = kora

LLAMA_BUILD = vendor/llama.cpp/build
LLAMA_LIB = $(LLAMA_BUILD)/src/libllama.a
COMMON_LIB = $(LLAMA_BUILD)/common/libcommon.a
LLAMA_SERVER = $(LLAMA_BUILD)/bin/llama-server
GGML_LIBS = $(LLAMA_BUILD)/ggml/src/libggml.a \
            $(LLAMA_BUILD)/ggml/src/libggml-base.a \
            $(LLAMA_BUILD)/ggml/src/libggml-cpu.a \
            $(wildcard $(LLAMA_BUILD)/ggml/src/ggml-metal/libggml-metal.a) \
            $(wildcard $(LLAMA_BUILD)/ggml/src/ggml-blas/libggml-blas.a)

LUA_LIB = vendor/lua/src/liblua.a

CFLAGS += -DVERSION=\"$(VERSION)\" -DLUADIR=\"$(LUADIR)\"
CFLAGS += -Ivendor/llama.cpp/include -Ivendor/llama.cpp/ggml/include
CFLAGS += -Ivendor/llama.cpp -Ivendor/llama.cpp/common
CFLAGS += -Ivendor/llama.cpp/vendor
CFLAGS += -Ivendor/lua/src
CFLAGS += -Isrc/core -Isrc/llm -Isrc/ui -Isrc/agent

CXXFLAGS += $(CFLAGS) -std=c++17

ifeq ($(UNAME_S),Darwin)
  NPROC := $(shell sysctl -n hw.ncpu)
  LUA_PLATFORM = macosx
  # macOS system ncurses is stuck on mouse v1 (no BUTTON5_PRESSED, so wheel-down
  # doesn't work). Prefer homebrew ncurses if installed — it's mouse v2.
  HOMEBREW_NCURSES := $(shell brew --prefix ncurses 2>/dev/null)
  ifneq ($(HOMEBREW_NCURSES),)
    CFLAGS += -I$(HOMEBREW_NCURSES)/include
    NCURSES_LIB = -L$(HOMEBREW_NCURSES)/lib -lncursesw
  else
    NCURSES_LIB = -lncurses
  endif
  PLATFORM_LIBS = -lm -lpthread $(NCURSES_LIB) -lsqlite3 -framework Accelerate -framework Metal -framework Foundation
else
  NPROC := $(shell nproc)
  LUA_PLATFORM = linux
  PLATFORM_LIBS = -lm -ldl -lpthread -lstdc++ -lgomp -lncursesw -lsqlite3
endif

# default target
all: $(BIN) $(LLAMA_SERVER)

# --- schema embedding ---
SCHEMA_HDR = src/core/schema_sql.h

$(SCHEMA_HDR): src/sql/schema.sql
	@echo "  GEN     $@"
	@printf 'static const char schema_sql_data[] = {\n' > $@
	@xxd -i < $< >> $@
	@printf ', 0x00\n};\n' >> $@

src/core/db.o: $(SCHEMA_HDR)

# --- vendor libs ---
$(LLAMA_LIB) $(LLAMA_SERVER):
	cmake -S vendor/llama.cpp -B $(LLAMA_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		-DLLAMA_BUILD_TESTS=OFF \
		-DLLAMA_BUILD_EXAMPLES=OFF \
		-DLLAMA_BUILD_COMMON=ON \
		-DLLAMA_BUILD_TOOLS=ON \
		-DLLAMA_BUILD_SERVER=ON \
		-DBUILD_SHARED_LIBS=OFF \
		$(LLAMA_CMAKE_FLAGS)
	cmake --build $(LLAMA_BUILD) --config Release -j$(NPROC)

$(LUA_LIB):
	$(MAKE) -C vendor/lua $(LUA_PLATFORM)

# --- kora binary ---
$(BIN): $(OBJ) $(LLAMA_LIB) $(LUA_LIB)
	$(CXX) -o $@ $(OBJ) $(COMMON_LIB) $(LLAMA_LIB) $(GGML_LIBS) $(LUA_LIB) $(LDFLAGS) $(PLATFORM_LIBS)
	cp -f $(LLAMA_SERVER) ./llama-server

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(BIN) llama-server $(OBJ) $(SCHEMA_HDR) tests/c/test_parser

clean-all: clean
	rm -rf $(LLAMA_BUILD)
	$(MAKE) -C vendor/lua clean

install: $(BIN) $(LLAMA_SERVER)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	mkdir -p $(DESTDIR)$(LUADIR)/core
	mkdir -p $(DESTDIR)$(LUADIR)/plugins
	mkdir -p $(DESTDIR)$(LUADIR)/tools
	mkdir -p $(DESTDIR)$(LUADIR)/agents/sub
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN)
	cp -f $(LLAMA_SERVER) $(DESTDIR)$(PREFIX)/bin/llama-server
	chmod 755 $(DESTDIR)$(PREFIX)/bin/llama-server
	cp -f lua/core/*.lua $(DESTDIR)$(LUADIR)/core/ 2>/dev/null || true
	cp -f lua/plugins/*.lua $(DESTDIR)$(LUADIR)/plugins/ 2>/dev/null || true
	cp -f lua/tools/*.lua $(DESTDIR)$(LUADIR)/tools/ 2>/dev/null || true
	cp -f lua/agents/*.lua $(DESTDIR)$(LUADIR)/agents/ 2>/dev/null || true
	cp -f lua/agents/sub/*.lua $(DESTDIR)$(LUADIR)/agents/sub/ 2>/dev/null || true

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(PREFIX)/bin/llama-server
	rm -rf $(DESTDIR)$(LUADIR)

.PHONY: all clean clean-all install uninstall test test-lua test-c

# --- tests ---

LUA_BIN = vendor/lua/src/lua

test: test-lua test-c
	@echo ""
	@printf '  \033[1mall test suites passed\033[0m\n'

test-lua: $(LUA_LIB)
	@printf '\n\033[1mlua\033[0m\n'
	@$(LUA_BIN) tests/lua/run.lua

test-c: tests/c/test_parser
	@printf '\n'
	@./tests/c/test_parser

tests/c/test_parser: tests/c/test_parser.c src/agent/parser.c src/agent/parser.h
	@$(CC) -Wall -Wextra -O2 -Isrc/agent -o $@ tests/c/test_parser.c src/agent/parser.c

include config.mk

VERSION = 0.1.0
UNAME_S := $(shell uname -s)

SRC_C = src/core/main.c src/core/util.c src/core/db.c src/core/dispatch.c \
        src/core/config.c \
        src/llm/client.c src/llm/model.c src/llm/registry.c \
        src/server/server.c src/server/pool.c \
        src/ui/tui.c src/ui/input.c src/ui/event.c src/ui/status.c \
        src/ui/session.c

SRC_CXX = src/server/proxy.cc

HTTPLIB_SRC = vendor/llama.cpp/vendor/cpp-httplib/httplib.cpp
HTTPLIB_OBJ = $(HTTPLIB_SRC:.cpp=.o)

OBJ = $(SRC_C:.c=.o) $(SRC_CXX:.cc=.o) $(HTTPLIB_OBJ)
BIN = kora

LLAMA_BUILD = vendor/llama.cpp/build
LLAMA_SERVER = $(LLAMA_BUILD)/bin/llama-server

LUA_LIB = vendor/lua/src/liblua.a

CFLAGS += -DVERSION=\"$(VERSION)\" -DLUADIR=\"$(LUADIR)\"
CFLAGS += -Ivendor/lua/src
CFLAGS += -Isrc/core -Isrc/llm -Isrc/server -Isrc/ui
CFLAGS += -Ivendor/llama.cpp/vendor/cpp-httplib

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
  PLATFORM_LIBS = -lm -lpthread $(NCURSES_LIB) -lsqlite3
else
  NPROC := $(shell nproc)
  LUA_PLATFORM = linux
  PLATFORM_LIBS = -lm -lpthread -lstdc++ -lncursesw -lsqlite3
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
# kora links against liblua (for config) but NOT libllama. llama-server is
# a separate binary that the supervisor execs.
$(LLAMA_SERVER):
	cmake -S vendor/llama.cpp -B $(LLAMA_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		-DLLAMA_BUILD_TESTS=OFF \
		-DLLAMA_BUILD_EXAMPLES=OFF \
		-DLLAMA_BUILD_COMMON=ON \
		-DLLAMA_BUILD_TOOLS=ON \
		-DLLAMA_BUILD_SERVER=ON \
		-DBUILD_SHARED_LIBS=OFF \
		$(LLAMA_CMAKE_FLAGS)
	cmake --build $(LLAMA_BUILD) --config Release -j$(NPROC) --target llama-server

$(LUA_LIB):
	$(MAKE) -C vendor/lua $(LUA_PLATFORM)

# --- kora binary ---
# kora needs llama-server at ./llama-server (next to it) for the supervisor to
# exec, so force-build the dep and fail loud if the copy fails.
$(BIN): $(OBJ) $(LUA_LIB) $(LLAMA_SERVER)
	$(CXX) -o $@ $(OBJ) $(LUA_LIB) $(LDFLAGS) $(PLATFORM_LIBS)
	cp -f $(LLAMA_SERVER) ./llama-server

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(HTTPLIB_OBJ): $(HTTPLIB_SRC)
	$(CXX) $(CXXFLAGS) -w -c $< -o $@

clean:
	rm -f $(BIN) llama-server $(OBJ) $(SCHEMA_HDR)

clean-all: clean
	rm -rf $(LLAMA_BUILD)
	$(MAKE) -C vendor/lua clean

install: $(BIN) $(LLAMA_SERVER)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	mkdir -p $(DESTDIR)$(LUADIR)/core
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN)
	cp -f $(LLAMA_SERVER) $(DESTDIR)$(PREFIX)/bin/llama-server
	chmod 755 $(DESTDIR)$(PREFIX)/bin/llama-server
	cp -f lua/core/*.lua $(DESTDIR)$(LUADIR)/core/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(PREFIX)/bin/llama-server
	rm -rf $(DESTDIR)$(LUADIR)

.PHONY: all clean clean-all install uninstall

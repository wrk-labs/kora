include config.mk

VERSION = 0.1.0
UNAME_S := $(shell uname -s)

SRC = src/main.c src/util.c src/model.c src/registry.c src/inference.c src/lua_bridge.c src/input.c
OBJ = $(SRC:.c=.o)
BIN = kora

LLAMA_BUILD = vendor/llama.cpp/build
LLAMA_LIB = $(LLAMA_BUILD)/src/libllama.a
GGML_LIBS = $(LLAMA_BUILD)/ggml/src/libggml.a \
            $(LLAMA_BUILD)/ggml/src/libggml-base.a \
            $(LLAMA_BUILD)/ggml/src/libggml-cpu.a \
            $(wildcard $(LLAMA_BUILD)/ggml/src/ggml-metal/libggml-metal.a) \
            $(wildcard $(LLAMA_BUILD)/ggml/src/ggml-blas/libggml-blas.a)

LUA_LIB = vendor/lua/src/liblua.a

CFLAGS += -DVERSION=\"$(VERSION)\" -DLUADIR=\"$(LUADIR)\"
CFLAGS += -Ivendor/llama.cpp/include -Ivendor/llama.cpp/ggml/include
CFLAGS += -Ivendor/lua/src

ifeq ($(UNAME_S),Darwin)
  NPROC := $(shell sysctl -n hw.ncpu)
  LUA_PLATFORM = macosx
  PLATFORM_LIBS = -lm -lpthread -framework Accelerate -framework Metal -framework Foundation
else
  NPROC := $(shell nproc)
  LUA_PLATFORM = linux
  PLATFORM_LIBS = -lm -ldl -lpthread -lstdc++ -lgomp
endif

all: $(BIN)

$(LLAMA_LIB):
	cmake -S vendor/llama.cpp -B $(LLAMA_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		-DLLAMA_BUILD_TESTS=OFF \
		-DLLAMA_BUILD_EXAMPLES=OFF \
		-DLLAMA_BUILD_SERVER=OFF \
		-DBUILD_SHARED_LIBS=OFF \
		$(LLAMA_CMAKE_FLAGS)
	cmake --build $(LLAMA_BUILD) --config Release -j$(NPROC)

$(LUA_LIB):
	$(MAKE) -C vendor/lua $(LUA_PLATFORM)

$(BIN): $(OBJ) $(LLAMA_LIB) $(LUA_LIB)
	$(CXX) -o $@ $(OBJ) $(LLAMA_LIB) $(GGML_LIBS) $(LUA_LIB) $(LDFLAGS) $(PLATFORM_LIBS)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(BIN) $(OBJ)

clean-all: clean
	rm -rf $(LLAMA_BUILD)
	$(MAKE) -C vendor/lua clean

install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	mkdir -p $(DESTDIR)$(LUADIR)/core
	mkdir -p $(DESTDIR)$(LUADIR)/plugins
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN)
	cp -f lua/core/*.lua $(DESTDIR)$(LUADIR)/core/ 2>/dev/null || true
	cp -f lua/plugins/*.lua $(DESTDIR)$(LUADIR)/plugins/ 2>/dev/null || true

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -rf $(DESTDIR)$(LUADIR)

.PHONY: all clean clean-all install uninstall

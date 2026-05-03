include config.mk

# Version lives in the VERSION file (single source of truth).
# To release: bump VERSION, commit, tag the same value with `v` prefix, push tag.
VERSION := $(shell cat VERSION)
UNAME_S := $(shell uname -s)

SRC_C = src/core/main.c src/core/util.c src/core/db.c src/core/dispatch.c \
        src/core/config.c src/core/prompt.c \
        src/llm/client.c src/llm/model.c src/llm/registry.c \
        src/server/server.c src/server/pool.c \
        src/ui/tui.c src/ui/input.c src/ui/event.c src/ui/status.c \
        src/ui/session.c src/ui/markdown.c

SRC_CXX = src/server/proxy.cc

HTTPLIB_SRC = vendor/llama.cpp/vendor/cpp-httplib/httplib.cpp
HTTPLIB_OBJ = $(HTTPLIB_SRC:.cpp=.o)

MD4C_SRC = vendor/md4c/src/md4c.c
MD4C_OBJ = $(MD4C_SRC:.c=.o)

OBJ = $(SRC_C:.c=.o) $(SRC_CXX:.cc=.o) $(HTTPLIB_OBJ) $(MD4C_OBJ)
BIN = kora

LLAMA_BUILD = vendor/llama.cpp/build
LLAMA_SERVER = $(LLAMA_BUILD)/bin/llama-server

LUA_LIB = vendor/lua/src/liblua.a

CFLAGS += -DVERSION=\"$(VERSION)\" -DLUADIR=\"$(LUADIR)\"
CFLAGS += -Ivendor/lua/src
CFLAGS += -Isrc/core -Isrc/llm -Isrc/server -Isrc/ui
CFLAGS += -Ivendor/llama.cpp/vendor/cpp-httplib
CFLAGS += -Ivendor/md4c/src

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
  # Metal is the only realistic GPU on macOS. Compile it in and embed the
  # shader library so the bottle is a single self-contained binary — no
  # plugin .so files, no ggml-metal.metal sidecar, no GGML_BACKEND_PATH.
  LLAMA_GPU_FLAGS = -DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON
else
  NPROC := $(shell nproc)
  LUA_PLATFORM = linux
  # -ldl is required for lua's loadlib (dlsym) on glibc < 2.34, where libdl
  # is still its own DSO. on glibc 2.34+ libdl was merged into libc and -ldl
  # is a harmless no-op alias for -lc.
  PLATFORM_LIBS = -lm -lpthread -lstdc++ -lncursesw -lsqlite3 -ldl
  # Linux ships with dynamic backend loading: the kora .deb bundles the CPU
  # and Vulkan backends as .so files under /usr/lib/kora/backends/; the
  # optional kora-cuda .deb drops libggml-cuda.so into the same directory
  # and llama-server picks the best backend at runtime.
  LLAMA_GPU_FLAGS = -DGGML_BACKEND_DL=ON -DGGML_CPU=ON -DGGML_VULKAN=ON
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
#
# LLAMA_NATIVE controls whether llama.cpp uses -march=native (CPU-specific
# optimizations). Default OFF for portable distribution builds — set ON for
# local dev to squeeze out perf on your own machine.
LLAMA_NATIVE ?= OFF

# macOS keeps llama.cpp statically linked (Metal compiles in, single binary).
# Linux needs BUILD_SHARED_LIBS=ON because GGML_BACKEND_DL requires it (cmake
# enforces this with a FATAL_ERROR otherwise) — the trade-off is that we ship
# libllama.so / libggml.so / libggml-base.so alongside llama-server in the
# .deb, plus the per-backend plugin .so files.
ifeq ($(UNAME_S),Darwin)
  LLAMA_SHARED = OFF
else
  LLAMA_SHARED = ON
endif

$(LLAMA_SERVER):
	cmake -S vendor/llama.cpp -B $(LLAMA_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		-DLLAMA_BUILD_TESTS=OFF \
		-DLLAMA_BUILD_EXAMPLES=OFF \
		-DLLAMA_BUILD_COMMON=ON \
		-DLLAMA_BUILD_TOOLS=ON \
		-DLLAMA_BUILD_SERVER=ON \
		-DBUILD_SHARED_LIBS=$(LLAMA_SHARED) \
		-DGGML_NATIVE=$(LLAMA_NATIVE) \
		$(LLAMA_GPU_FLAGS) \
		$(LLAMA_CMAKE_FLAGS)
	cmake --build $(LLAMA_BUILD) --config Release -j$(NPROC) --target llama-server

$(LUA_LIB):
	$(MAKE) -C vendor/lua $(LUA_PLATFORM)

# --- kora binary ---
# kora needs llama-server at ./llama-server (next to it) for the supervisor to
# exec, so force-build the dep and fail loud if the copy fails.
#
# On Linux, llama-server is dynamically linked against libllama.so /
# libggml*.so. For the dev tree to be runnable without `make install`, copy
# the core .so files next to llama-server and rpath them so $ORIGIN finds
# everything. (On Mac, llama-server is statically linked — nothing else to
# copy.)
$(BIN): $(OBJ) $(LUA_LIB) $(LLAMA_SERVER)
	$(CXX) -o $@ $(OBJ) $(LUA_LIB) $(LDFLAGS) $(PLATFORM_LIBS)
	cp -f $(LLAMA_SERVER) ./llama-server
ifneq ($(UNAME_S),Darwin)
	# llama-server's NEEDED entries are SONAME-versioned (libllama.so.0,
	# libggml.so.0, libggml-base.so.0, libmtmd.so.0), so we need to preserve
	# the symlink chain (libfoo.so -> libfoo.so.0 -> libfoo.so.0.X.Y). cp -a
	# preserves symlinks; the glob captures all three names per lib.
	cp -af $(LLAMA_BUILD)/bin/libllama.so* ./
	cp -af $(LLAMA_BUILD)/bin/libggml.so* ./
	cp -af $(LLAMA_BUILD)/bin/libggml-base.so* ./
	cp -af $(LLAMA_BUILD)/bin/libmtmd.so* ./
	mkdir -p ./backends
	cp -af $(LLAMA_BUILD)/bin/libggml-cpu.so* ./backends/
	cp -af $(LLAMA_BUILD)/bin/libggml-vulkan.so* ./backends/
	patchelf --set-rpath '$$ORIGIN' ./llama-server
	patchelf --set-rpath '$$ORIGIN/..' ./backends/libggml-cpu.so
	patchelf --set-rpath '$$ORIGIN/..' ./backends/libggml-vulkan.so
endif

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(HTTPLIB_OBJ): $(HTTPLIB_SRC)
	$(CXX) $(CXXFLAGS) -w -c $< -o $@

$(MD4C_OBJ): $(MD4C_SRC)
	$(CC) $(CFLAGS) -w -c $< -o $@

clean:
	rm -f $(BIN) llama-server $(OBJ) $(SCHEMA_HDR)
	rm -f $(TEST_BINS) $(TEST_OBJ)
	# Linux dev tree: drop the copied core libs and plugin dir. Glob covers
	# the SONAME chain (libfoo.so, libfoo.so.0, libfoo.so.0.X.Y).
	rm -f libllama.so* libggml.so* libggml-base.so* libmtmd.so*
	rm -rf backends

clean-all: clean
	rm -rf $(LLAMA_BUILD) $(LLAMA_CUDA_BUILD)
	$(MAKE) -C vendor/lua clean

# --- tests ---
# each tests/test_*.c produces its own binary linked against the pieces of
# kora it exercises. `make test` builds and runs all of them; any non-zero
# exit status aborts the run. no external test framework — tests/test.h is
# a 50-line harness.
TEST_SRC = tests/test_registry.c tests/test_session.c tests/test_client.c \
           tests/test_db.c tests/test_config.c tests/test_prompt.c \
           tests/test_markdown.c
TEST_BINS = $(TEST_SRC:.c=)
TEST_OBJ = $(TEST_SRC:.c=.o)

TEST_CFLAGS = $(CFLAGS) -Itests

tests/test_registry: tests/test_registry.o src/llm/registry.o
	$(CC) -o $@ $^ $(LDFLAGS) $(PLATFORM_LIBS)

tests/test_session: tests/test_session.o src/ui/session.o
	$(CC) -o $@ $^ $(LDFLAGS) $(PLATFORM_LIBS)

tests/test_client: tests/test_client.o src/llm/client.o
	$(CC) -o $@ $^ $(LDFLAGS) $(PLATFORM_LIBS)

tests/test_db: tests/test_db.o src/core/db.o src/core/util.o \
               src/llm/model.o src/llm/registry.o
	$(CC) -o $@ $^ $(LDFLAGS) $(PLATFORM_LIBS)

tests/test_config: tests/test_config.o src/core/config.o src/core/util.o $(LUA_LIB)
	$(CC) -o $@ $^ $(LDFLAGS) $(PLATFORM_LIBS)

tests/test_prompt: tests/test_prompt.o src/core/prompt.o
	$(CC) -o $@ $^ $(LDFLAGS) $(PLATFORM_LIBS)

tests/test_markdown: tests/test_markdown.o src/ui/markdown.o $(MD4C_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(PLATFORM_LIBS)

tests/%.o: tests/%.c tests/test.h
	$(CC) $(TEST_CFLAGS) -c $< -o $@

test: $(TEST_BINS)
	@fail=0; total_checks=0; total_groups=0; \
	for t in $(TEST_BINS); do \
		name=$$(basename $$t); \
		out=$$(./$$t 2>&1); rc=$$?; \
		summary=$$(printf '%s\n' "$$out" | tail -n 1); \
		if [ $$rc -eq 0 ]; then \
			printf "  %-22s %s\n" "$$name" "$$summary"; \
			c=$$(printf '%s' "$$summary" | awk '{print $$1}'); \
			g=$$(printf '%s' "$$summary" | awk '{print $$4}'); \
			total_checks=$$((total_checks + c)); \
			total_groups=$$((total_groups + g)); \
		else \
			printf "  %-22s FAIL\n" "$$name"; \
			printf '%s\n' "$$out" | sed 's/^/    /'; \
			fail=1; \
		fi; \
	done; \
	if [ $$fail -ne 0 ]; then \
		echo ""; echo "TESTS FAILED"; exit 1; \
	else \
		echo ""; \
		echo "  all tests passed ($$total_checks checks across $$total_groups tests in $(words $(TEST_BINS)) binaries)"; \
	fi

KORA_LIB_DIR = $(PREFIX)/lib/kora
BACKENDS_DIR = $(KORA_LIB_DIR)/backends

install: $(BIN) $(LLAMA_SERVER)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	mkdir -p $(DESTDIR)$(LUADIR)/core
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN)
	cp -f lua/core/*.lua $(DESTDIR)$(LUADIR)/core/
ifeq ($(UNAME_S),Darwin)
	# macOS: single statically-linked llama-server (Metal embedded).
	cp -f $(LLAMA_SERVER) $(DESTDIR)$(PREFIX)/bin/llama-server
	chmod 755 $(DESTDIR)$(PREFIX)/bin/llama-server
else
	# Linux: shared-lib layout. llama-server + core .so files live under
	# /usr/lib/kora/, plugins under /usr/lib/kora/backends/. A symlink in
	# /usr/bin keeps kora's llama_server_path() lookup (sibling of kora)
	# working — kora resolves the symlink with realpath() at runtime to
	# derive the backends dir.
	mkdir -p $(DESTDIR)$(KORA_LIB_DIR)
	mkdir -p $(DESTDIR)$(BACKENDS_DIR)
	cp -f $(LLAMA_SERVER)                        $(DESTDIR)$(KORA_LIB_DIR)/llama-server
	chmod 755 $(DESTDIR)$(KORA_LIB_DIR)/llama-server
	# Preserve SONAME symlink chains; llama-server's NEEDED uses libfoo.so.0
	# names, so just copying libfoo.so (the unversioned symlink target) leaves
	# the loader unable to resolve the deps. -a keeps the symlinks intact.
	cp -af $(LLAMA_BUILD)/bin/libllama.so*       $(DESTDIR)$(KORA_LIB_DIR)/
	cp -af $(LLAMA_BUILD)/bin/libggml.so*        $(DESTDIR)$(KORA_LIB_DIR)/
	cp -af $(LLAMA_BUILD)/bin/libggml-base.so*   $(DESTDIR)$(KORA_LIB_DIR)/
	cp -af $(LLAMA_BUILD)/bin/libmtmd.so*        $(DESTDIR)$(KORA_LIB_DIR)/
	cp -af $(LLAMA_BUILD)/bin/libggml-cpu.so*    $(DESTDIR)$(BACKENDS_DIR)/
	cp -af $(LLAMA_BUILD)/bin/libggml-vulkan.so* $(DESTDIR)$(BACKENDS_DIR)/
	# rpath = $$ORIGIN means the loader looks in the file's own dir.
	# llama-server finds its sibling core libs; plugins find core libs
	# one level up via $$ORIGIN/...
	patchelf --set-rpath '$$ORIGIN'    $(DESTDIR)$(KORA_LIB_DIR)/llama-server
	patchelf --set-rpath '$$ORIGIN/..' $(DESTDIR)$(BACKENDS_DIR)/libggml-cpu.so
	patchelf --set-rpath '$$ORIGIN/..' $(DESTDIR)$(BACKENDS_DIR)/libggml-vulkan.so
	# Symlink so /usr/bin/llama-server works (PATH lookup, scripts).
	ln -sf ../lib/kora/llama-server $(DESTDIR)$(PREFIX)/bin/llama-server
endif

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(PREFIX)/bin/llama-server
	rm -rf $(DESTDIR)$(KORA_LIB_DIR)
	rm -rf $(DESTDIR)$(LUADIR)

# --- debian package ---
# `make deb` produces dist/kora_<version>_<arch>.deb. CI overrides DEB_VERSION
# from the git tag (refs/tags/vX.Y.Z -> X.Y.Z); local builds default to the
# Makefile VERSION.
DEB_VERSION ?= $(VERSION)
DEB_ARCH    := $(shell dpkg --print-architecture 2>/dev/null)
DEB_STAGE   := dist/deb-stage
DEB_FILE    := dist/kora_$(DEB_VERSION)_$(DEB_ARCH).deb

deb: debian/control.in
	$(MAKE) clean-all
	$(MAKE) PREFIX=/usr LUADIR=/usr/share/kora/lua
	rm -rf $(DEB_STAGE)
	$(MAKE) install DESTDIR=$(DEB_STAGE) PREFIX=/usr LUADIR=/usr/share/kora/lua
	mkdir -p $(DEB_STAGE)/DEBIAN
	# Installed-Size is in KiB (debian policy), excluding DEBIAN/ metadata.
	# dpkg-deb does NOT auto-compute it; without it apt can't show how much
	# space the package will use or free.
	INSTALLED_SIZE=$$(du -sk --exclude=DEBIAN $(DEB_STAGE) | cut -f1) && \
	sed -e 's|__VERSION__|$(DEB_VERSION)|g' \
	    -e 's|__ARCH__|$(DEB_ARCH)|g' \
	    -e "s|__INSTALLED_SIZE__|$$INSTALLED_SIZE|g" \
	    debian/control.in > $(DEB_STAGE)/DEBIAN/control
	mkdir -p dist
	fakeroot dpkg-deb --build --root-owner-group $(DEB_STAGE) $(DEB_FILE)
	@echo
	@echo "  built $(DEB_FILE)"

# --- cuda plugin ---
# `make cuda-plugin` builds JUST the CUDA backend as a shared library plugin
# (no llama-server, no other backends). Must run inside an nvidia/cuda devel
# container — see Dockerfile.cuda-build. Output: build-cuda/bin/libggml-cuda.so.
LLAMA_CUDA_BUILD = vendor/llama.cpp/build-cuda

cuda-plugin:
	cmake -S vendor/llama.cpp -B $(LLAMA_CUDA_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		-DLLAMA_BUILD_TESTS=OFF \
		-DLLAMA_BUILD_EXAMPLES=OFF \
		-DLLAMA_BUILD_TOOLS=OFF \
		-DLLAMA_BUILD_SERVER=OFF \
		-DLLAMA_BUILD_COMMON=OFF \
		-DBUILD_SHARED_LIBS=ON \
		-DGGML_BACKEND_DL=ON \
		-DGGML_CPU=OFF \
		-DGGML_CUDA=ON
	cmake --build $(LLAMA_CUDA_BUILD) --config Release -j$(NPROC) --target ggml-cuda

# `make cuda-deb` produces dist/kora-cuda_<version>_<arch>.deb — a thin addon
# package that just drops libggml-cuda.so + bundled cuBLAS into
# /usr/lib/kora/backends/. Depends on the matching kora package; installing
# it pulls kora in if not present.
DEB_CUDA_STAGE := dist/deb-cuda-stage
DEB_CUDA_FILE  := dist/kora-cuda_$(DEB_VERSION)_$(DEB_ARCH).deb

# Backend install path inside the .deb. Hardcoded to /usr/lib/... rather
# than $(BACKENDS_DIR) because $(PREFIX) defaults to /usr/local and the
# Debian package always wants /usr; matches the path kora's supervisor
# exports as GGML_BACKEND_PATH on Linux.
DEB_CUDA_BACKENDS = /usr/lib/kora/backends

# CUDA runtime libraries to bundle inside the .deb so users don't need to add
# NVIDIA's apt repo. Resolved from CUDA_HOME (set in the build container).
CUDA_HOME ?= /usr/local/cuda
CUDA_BUNDLED_LIBS = libcudart.so.12 libcublas.so.12 libcublasLt.so.12

cuda-deb: cuda-plugin debian/control-cuda.in
	rm -rf $(DEB_CUDA_STAGE)
	mkdir -p $(DEB_CUDA_STAGE)$(DEB_CUDA_BACKENDS)
	cp -f $(LLAMA_CUDA_BUILD)/bin/libggml-cuda.so $(DEB_CUDA_STAGE)$(DEB_CUDA_BACKENDS)/
	# Bundle cuBLAS / cudaRt next to the plugin. dereference symlinks (-L)
	# so the .deb carries the real shared object, not a dangling link.
	for lib in $(CUDA_BUNDLED_LIBS); do \
		cp -fL $(CUDA_HOME)/lib64/$$lib $(DEB_CUDA_STAGE)$(DEB_CUDA_BACKENDS)/; \
	done
	# rpath so libggml-cuda.so finds (a) its sibling cuBLAS libs in the
	# same dir, and (b) libggml-base.so in the parent /usr/lib/kora/ dir
	# (shipped by the kora package, not duplicated here). $$ORIGIN is
	# the .so's own dir at load time.
	patchelf --set-rpath '$$ORIGIN:$$ORIGIN/..' \
	    $(DEB_CUDA_STAGE)$(DEB_CUDA_BACKENDS)/libggml-cuda.so
	mkdir -p $(DEB_CUDA_STAGE)/DEBIAN
	# Installed-Size in KiB; see deb target above for why this is manual.
	INSTALLED_SIZE=$$(du -sk --exclude=DEBIAN $(DEB_CUDA_STAGE) | cut -f1) && \
	sed -e 's|__VERSION__|$(DEB_VERSION)|g' \
	    -e 's|__ARCH__|$(DEB_ARCH)|g' \
	    -e "s|__INSTALLED_SIZE__|$$INSTALLED_SIZE|g" \
	    debian/control-cuda.in > $(DEB_CUDA_STAGE)/DEBIAN/control
	mkdir -p dist
	fakeroot dpkg-deb --build --root-owner-group $(DEB_CUDA_STAGE) $(DEB_CUDA_FILE)
	@echo
	@echo "  built $(DEB_CUDA_FILE)"

.PHONY: all clean clean-all install uninstall test deb cuda-plugin cuda-deb

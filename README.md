# Kora

A minimal, local LLM tool for the terminal. Built with C and Lua.

Kora runs large language models entirely on your machine — no cloud, no API keys, no telemetry. It uses llama.cpp for inference and supports any GGUF model.

## Features

- **Chat** — interactive conversations with local LLMs
- **Three-pane TUI** — `SESSIONS` on the left, `CHAT` in the middle, `MODELS` on the right; Tab cycles focus
- **Model management** — download, list, switch, cancel-in-progress, and remove GGUF models; URL pulls show up as a selectable row with the same cancel gesture
- **Session management** — create, open, rename, delete, auto-titled in the background once a conversation has some substance; persisted in SQLite and resumable across runs
- **Automatic context compaction** — when the running context hits ~75% of the model's window, Kora summarises older turns in the background and continues without interruption
- **Non-blocking UI** — generation, compaction, and downloads all run on background threads; the pane stays responsive
- **Scrollable chat** — mouse-wheel scroll in the chat pane
- **GPU acceleration** — automatic VRAM detection and layer offloading via the vendored llama.cpp

## Install

```
make && sudo make install
```

For GPU support, pass the backend flag:

```
make GGML_CUDA=1      # NVIDIA
make GGML_VULKAN=1    # NVIDIA, AMD, Intel
make GGML_HIP=1       # AMD (ROCm)
make GGML_METAL=1     # Apple Silicon
```

## Usage

```
kora                  # start chatting (default)
kora chat [model]     # start chatting with a specific model
kora pull <model|url> # download a GGUF (registry alias or direct URL)
kora list             # list downloaded models
kora rm <model>       # remove a model from disk
kora serve [model]    # run the inference daemon (normally managed for you)
kora help             # print usage information
kora version          # print version
```

Kora speaks the OpenAI chat-completions protocol internally: the TUI is a thin
client that talks to a local `kora serve` daemon. In normal use you don't need
to start the daemon yourself — if it isn't running, Kora tells you how. If you
want it always-on, run it under systemd --user or equivalent.

### Keyboard shortcuts

The TUI has three panes — `SESSIONS`, `CHAT`, `MODELS` — and the shortcut hint
at the bottom always reflects the focused pane.

**Global**

```
Tab / Shift+Tab   Cycle focus across panes
Ctrl-C            Quit (prompts if a download is in progress)
```

**CHAT pane**

```
type + Enter      Send message
Alt+Enter         Insert newline (multiline input)
Esc               Cancel the current generation / compaction
Ctrl-L            Clear the chat view (history is kept on disk)
mouse wheel       Scroll chat
```

**SESSIONS pane**

```
j / k             Highlight next / previous session
Enter             Open the highlighted session
n                 New session
d                 Delete the highlighted session
r                 Rename the highlighted session
```

**MODELS pane**

```
j / k             Highlight next / previous model
Enter             Switch to the highlighted model
p                 Pull the highlighted model
u                 Pull an arbitrary URL (prompts for the URL)
d                 Remove the highlighted model — or cancel its download
                  when that row is currently being pulled (hint updates)
```

Downloads show a `⟳` glyph on their row. URL pulls appear as a synthetic
row at the top of the `MODELS` pane so the same `d`-to-cancel gesture works.
Invalid targets (HTML pages, JSON, truncated files) are detected by a GGUF
magic-byte check and rejected instead of being saved as fake models.

## Configuration

All data lives under `~/.kora/`:

```
~/.kora/
├── models/         # downloaded GGUF files
├── config.lua      # user settings (copied from the bundled template on first run)
└── kora.db         # sessions, messages, model catalog, settings (SQLite)
```

### System prompt

The system prompt lives in `lua/core/system.lua` as a template. Kora
renders it at session start and substitutes a small set of placeholders
so the model has current environmental context:

```
{date}      e.g. "Friday, April 24, 2026"
{time}      local time + UTC offset, e.g. "10:01 -03"
{platform}  lowercase uname, e.g. "darwin" / "linux"
{model}     current model alias, e.g. "llama-3.2-3b"
{ctx}       configured context-window size in tokens
```

Unknown placeholders are left verbatim so typos are visible. Mid-session
model swaps (via the `MODELS` pane) append a short `[Model changed from X
to Y.]` system-role event to the transcript rather than rewriting the
top-level prompt — this lets the model see the swap as a concrete turn
in its history.

## Architecture

```
kora/
├── src/
│   ├── core/       # main loop, config, database, dispatch, util
│   ├── llm/        # model download/registry + OpenAI-compat HTTP client
│   ├── server/     # inference supervisor: HTTP proxy + llama-server pool
│   ├── ui/         # TUI (ncurses), event queue, status handlers, input
│   └── sql/        # schema (embedded into the binary at build time)
├── lua/            # Lua configuration (system prompts, user config template)
│   └── core/
├── tests/          # unit tests — one binary per module, `make test` runs all
├── vendor/         # llama.cpp, Lua 5.5 (vendored)
├── Makefile
├── config.mk
└── README.md
```

### Key design choices

- **Client/daemon split** — `kora serve` runs a supervisor that pools `llama-server` children and exposes an OpenAI-compatible chat-completions endpoint; the TUI is a thin HTTP client. This keeps model loads hot across TUI restarts and lets multiple clients share one model.
- **Event-driven TUI** — background threads never touch ncurses directly; they push events onto a thread-safe ring buffer, drained by the main thread at ~60fps.
- **Serialized DB access** — every SQLite call is guarded by a recursive mutex, so concurrent writes from generation / compaction / download / session-naming workers are safe regardless of the system `libsqlite3`'s threading mode.
- **Priority-based right statusbar** — handler system where higher-priority handlers (download progress) override lower ones (context info) automatically.
- **Pad-based chat scrolling** — ncurses pad with virtual height, mouse-wheel scroll, reflow-on-resize from a source-of-truth log.
- **Pane-driven commands** — user actions come from focused-pane shortcuts rather than typed slash commands; the status bar hint always reflects what the current keystroke will do.

## Development

```
make test             # build and run the unit-test suite
make clean            # remove build artifacts (keeps vendored deps)
make clean-all        # also wipe vendored llama.cpp and lua builds
```

Tests live under `tests/` — one binary per module (`test_registry`,
`test_session`, `test_client`, `test_db`, `test_config`) — and use a tiny
harness in `tests/test.h`. DB / config tests sandbox themselves under
`/tmp` via `mkdtemp` and never touch your real `~/.kora/`.

## Known issues

- **URL pulls are unreliable.** Arbitrary `https://` GGUF URLs don't always
  complete — some targets hang mid-download, some fail the GGUF magic-byte
  check even though the file would normally be valid, and HuggingFace's
  `?download=true` redirect chain isn't consistently handled. Registry
  pulls (by alias) work. If you need a model that isn't in
  `src/llm/registry.c`, the reliable workaround for now is to `curl` it
  manually into `~/.kora/models/` as `<name>.gguf` and let
  `db_models_sync` pick it up on next start; or add it to the registry
  and rebuild.

## License

GPL-2.0

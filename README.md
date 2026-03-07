# Kora

A minimal, local LLM tool for the terminal. Built with C and Lua.

Kora runs large language models entirely on your machine — no cloud, no API keys, no telemetry. It uses llama.cpp for inference and supports any GGUF model.

## Features

- **Chat** — interactive conversations with local LLMs
- **Code** — agentic coding assistant with file editing, search, and shell access
- **Model management** — download, list, switch, and remove GGUF models
- **Context compression** — automatic and manual conversation compaction
- **Non-blocking UI** — model loading, generation, compaction all run in background threads
- **Generation control** — cancel any running task with ESC
- **Scrollable chat** — PgUp/PgDn and mouse wheel support
- **Plugins** — extend functionality with Lua scripts
- **GPU acceleration** — automatic VRAM detection and layer offloading

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
kora chat [model]     # start chatting
kora code [model]     # coding assistant (wip)
kora version          # print version
```

### Chat commands

```
/help           Show available commands
/model <name>   Switch to a different model
/pull <name>    Download a model
/compact        Summarize conversation to free context
/clear          Clear conversation history
/exit           Quit chat
```

Press **ESC** during generation or compaction to cancel.

## Configuration

All data lives under `~/.kora/`:

```
~/.kora/
├── models/         # downloaded GGUF files
├── sessions/       # conversation history
├── config.lua      # settings
└── plugins/        # user plugins
```

## Architecture

```
kora/
├── src/
│   ├── core/       # main loop, config, database, dispatch
│   ├── llm/        # inference, model management, registry
│   ├── ui/         # TUI (ncurses), event queue, status handlers, input
│   └── agent/      # Lua bridge
├── lua/            # Lua agent and plugins
│   ├── core/
│   └── plugins/
├── vendor/         # llama.cpp, Lua 5.5 (vendored)
├── Makefile
├── config.mk
└── README.md
```

### Key design choices

- **Event-driven TUI** — background threads never touch ncurses directly; they push events onto a thread-safe ring buffer, drained by the main thread at ~60fps
- **Priority-based statusbar** — right statusbar managed by a handler system; higher-priority handlers (download progress) override lower ones (context info) automatically
- **Non-blocking operations** — model loading, generation, compaction, and downloads all run in background threads with the UI remaining responsive
- **Pad-based scrolling** — chat uses an ncurses pad with virtual height, supporting PgUp/PgDn and mouse wheel scrolling

## License

GPL-2.0

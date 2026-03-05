# Kora

A minimal, local LLM tool for the terminal. Built with C and Lua.

Kora runs large language models entirely on your machine — no cloud, no API keys, no telemetry. It uses llama.cpp for inference and supports any GGUF model.

## Features

- **Chat** — interactive conversations with local LLMs
- **Code** — agentic coding assistant with file editing, search, and shell access
- **Model management** — download, list, and remove GGUF models
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
kora pull qwen-coder-7b        # download a model
kora chat                       # start chatting
kora code                       # coding assistant
kora list                       # list downloaded models
kora rm <model>                 # remove a model
kora version                    # print version
```

## Configuration

All data lives under `~/.kora/`:

```
~/.kora/
├── models/         # downloaded GGUF files
├── sessions/       # conversation history
├── config.lua      # settings
└── plugins/        # user plugins
```

## Project Structure

```
kora/
├── src/            # C source
├── lua/            # Lua agent and plugins
│   ├── core/
│   └── plugins/
├── vendor/         # llama.cpp, Lua 5.5 (vendored)
├── Makefile
├── config.mk
└── README.md
```

## License

GPL-2.0

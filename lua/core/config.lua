-- default kora configuration
-- copy to ~/.kora/config.lua to customize
return {
    default_model = "llama-3.2-3b",
    -- chat_model = "llama-3.2-3b",
    -- code_model = "qwen-coder-7b",

    ctx_size = 4096,        -- set to 0 for auto-detect based on available memory
    -- threads = 4,         -- auto-detect if omitted
    gpu_layers = -2,        -- -2 = auto, -1 = all on GPU, 0 = CPU only, N = fixed
}

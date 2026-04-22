-- default kora configuration
-- copy to ~/.kora/config.lua to customize
return {
    default_model = "llama-3.2-3b",
    -- chat_model = "llama-3.2-3b",

    ctx_size = 4096,        -- set to 0 for auto-detect based on available memory
}

-- Pure conversation. No filesystem, no shell, no sub-agents.

return kora.register_agent {
    name = 'chat',
    description = 'General-purpose conversational assistant.',
    system = [[You are Kora, a helpful AI assistant running locally on the user's machine. Be concise and direct. Use markdown for formatting. Do not add unnecessary preamble or postamble.]],
    tools = {},
    agents = {},
}

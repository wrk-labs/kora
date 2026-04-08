-- General-purpose sub-agent: full toolset, used for catch-all delegation.

return kora.register_agent {
    name = 'general',
    description = 'Catch-all sub-agent for multi-step delegated work. Has the full toolset.',
    system = [[You are a general-purpose sub-agent. You were given a focused task by the main agent. Complete it and return a written result.

Rules:
- You have the full toolset including write/edit/bash. Use them only as needed.
- Stay focused on the task you were given. Do not expand scope.
- When done, write a brief summary of what you did and stop.]],
    tools = {
        'read', 'write', 'edit', 'bash',
        'glob', 'grep', 'todo',
    },
    agents = {},
}

-- Spawn a sub-agent in an isolated context. Returns the sub-agent's
-- final assistant message as a string.
--
-- The C side validates `agent` against the parent agent's declared
-- `agents` whitelist before spawning, and enforces depth/step limits.

return kora.register_tool {
    name = 'task',
    description = 'Delegate a focused subtask to a specialized sub-agent. The sub-agent runs in an isolated context and returns a written result.',
    schema = {
        agent = 'string',
        prompt = 'string',
    },
    run = function(args)
        if type(args.agent) ~= 'string' or args.agent == '' then
            return { ok = false, error = 'agent name required' }
        end
        if type(args.prompt) ~= 'string' or args.prompt == '' then
            return { ok = false, error = 'prompt required' }
        end
        -- kora.loop_run is a C binding: it creates a child run frame,
        -- enforces the parent's `agents` whitelist, runs to completion,
        -- and returns the final assistant string (or nil + error).
        local result, err = kora.loop_run(args.agent, args.prompt)
        if not result then
            return { ok = false, error = err or 'sub-agent failed' }
        end
        return { ok = true, content = result }
    end,
}

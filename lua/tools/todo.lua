-- TodoWrite-equivalent: writes the agent's plan back into context.
-- The list is held in C (kora.todos_set/kora.todos_get) so it survives
-- across loop iterations and can be rendered by the TUI.

return kora.register_tool {
    name = 'todo',
    description = 'Write or update the current task list. Pass an array of {content, status} objects (status: pending|in_progress|completed). Use this to plan multi-step work and track progress.',
    schema = {
        todos = 'array',
    },
    run = function(args)
        local todos = args.todos
        -- small models often pass the array as a JSON-encoded *string* instead
        -- of an actual array. accept either form.
        if type(todos) == 'string' then
            local json = require('core.json')
            local decoded = json.decode(todos)
            if type(decoded) == 'table' then todos = decoded end
        end
        if type(todos) ~= 'table' then
            return { ok = false, error = 'todos must be an array of {content, status} objects' }
        end
        -- normalize and validate. accept entries that are plain strings
        -- (model shorthand) by promoting them to {content=..., status="pending"}.
        local cleaned = {}
        for i, t in ipairs(todos) do
            if type(t) == 'string' then
                cleaned[i] = { content = t, status = 'pending' }
            elseif type(t) == 'table' and type(t.content) == 'string' then
                local status = t.status or 'pending'
                if status ~= 'pending' and status ~= 'in_progress' and status ~= 'completed' then
                    return { ok = false, error = 'todo[' .. i .. '].status must be pending|in_progress|completed' }
                end
                cleaned[i] = { content = t.content, status = status }
            else
                return { ok = false, error = 'todo[' .. i .. '] must be a string or {content, status}' }
            end
        end
        kora.todos_set(cleaned)
        return { ok = true, content = 'updated todo list (' .. #cleaned .. ' items)' }
    end,
}

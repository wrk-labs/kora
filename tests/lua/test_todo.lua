-- Tests for the todo tool. Stubs kora.todos_set so we can verify what
-- the tool actually persists.

_G.kora = _G.kora or {}
local _last_todos = nil
function kora.todos_set(t) _last_todos = t end
function kora.todos_get() return _last_todos or {} end
function kora.shell_exec(_, _) return '', 0 end
function kora.event_push(_, _) end
function kora.loop_run(_, _) return nil, 'stub' end
function kora.confirm(_) return 'y' end

require('core.loader')
require('tools.todo')
T.truthy(kora.tools.todo)
local todo = kora.tools.todo.run

T.case('accepts an array of {content, status}', function()
    _last_todos = nil
    local r = todo {
        todos = {
            { content = 'do thing', status = 'pending' },
            { content = 'do another', status = 'in_progress' },
        },
    }
    T.eq(r.ok, true)
    T.truthy(_last_todos)
    T.eq(#_last_todos, 2)
    T.eq(_last_todos[1].content, 'do thing')
    T.eq(_last_todos[1].status, 'pending')
    T.eq(_last_todos[2].status, 'in_progress')
end)

T.case('defaults missing status to pending', function()
    _last_todos = nil
    todo { todos = { { content = 'no status' } } }
    T.eq(_last_todos[1].status, 'pending')
end)

T.case('promotes plain string entries to {content, status=pending}', function()
    _last_todos = nil
    local r = todo { todos = { 'first', 'second', 'third' } }
    T.eq(r.ok, true)
    T.eq(#_last_todos, 3)
    T.eq(_last_todos[1].content, 'first')
    T.eq(_last_todos[1].status, 'pending')
    T.eq(_last_todos[3].content, 'third')
end)

T.case('decodes JSON-encoded string-of-array (small model fallback)', function()
    _last_todos = nil
    -- the small model bug: passing the array as a JSON string
    local r = todo {
        todos = '[{"content":"a","status":"pending"},{"content":"b","status":"completed"}]',
    }
    T.eq(r.ok, true)
    T.eq(#_last_todos, 2)
    T.eq(_last_todos[1].content, 'a')
    T.eq(_last_todos[2].status, 'completed')
end)

T.case('decodes JSON string-of-strings shorthand', function()
    _last_todos = nil
    local r = todo { todos = '["x","y","z"]' }
    T.eq(r.ok, true)
    T.eq(#_last_todos, 3)
    T.eq(_last_todos[1].content, 'x')
end)

T.case('rejects non-array todos', function()
    _last_todos = nil
    local r = todo { todos = 42 }
    T.eq(r.ok, false)
    T.matches(r.error, 'array')
end)

T.case('rejects bad status value', function()
    local r = todo { todos = { { content = 'x', status = 'wrong' } } }
    T.eq(r.ok, false)
    T.matches(r.error, 'status must be')
end)

T.case('rejects entry with no content', function()
    local r = todo { todos = { { status = 'pending' } } }
    T.eq(r.ok, false)
    T.matches(r.error, 'must be a string or')
end)

T.case('returns count in confirmation', function()
    local r = todo { todos = { 'a', 'b', 'c' } }
    T.matches(r.content, '3 items')
end)

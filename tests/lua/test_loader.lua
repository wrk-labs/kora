-- Test the loader's tool/agent registration and the dispatch whitelist gate.
-- The C bindings (kora.shell_exec etc) are stubbed out so we can run in a
-- plain lua interpreter.

_G.kora = {}
function kora.shell_exec(_, _) return '', 0 end
function kora.todos_set(_) end
function kora.todos_get() return {} end
function kora.event_push(_, _) end
function kora.loop_run(_, _) return nil, 'stub' end
function kora.confirm(_) return 'y' end

local json = require('core.json')
require('core.loader')

T.case('register_tool', function()
    kora.register_tool {
        name = 'noop',
        description = 'does nothing',
        run = function(args) return { ok = true, content = 'ran with ' .. (args.x or '?') } end,
    }
    T.truthy(kora.tools.noop)
    T.eq(kora.tools.noop.name, 'noop')
end)

T.case('register_agent normalizes whitelists', function()
    kora.register_agent {
        name = 'tester',
        system = 'test system prompt',
        tools = { 'noop', 'read' },
        agents = { 'helper' },
    }
    local a = kora.agents.tester
    T.truthy(a._tool_set.noop)
    T.truthy(a._tool_set.read)
    T.falsy(a._tool_set.bash)
    T.truthy(a._agent_set.helper)
end)

T.case('dispatch_tool returns ok JSON', function()
    local result = kora.dispatch_tool('noop', '{"x":"hi"}')
    local v = json.decode(result)
    T.eq(v.ok, true)
    T.matches(v.content, 'hi')
end)

T.case('dispatch_tool unknown tool', function()
    local v = json.decode(kora.dispatch_tool('does_not_exist', '{}'))
    T.eq(v.ok, false)
    T.matches(v.error, 'unknown tool')
end)

T.case('dispatch_tool bad json', function()
    local v = json.decode(kora.dispatch_tool('noop', '{not json'))
    T.eq(v.ok, false)
    T.matches(v.error, 'json')
end)

T.case('whitelist gate blocks tool not in current set', function()
    kora._current_tools = { read = true }  -- noop is NOT in the set
    local v = json.decode(kora.dispatch_tool('noop', '{}'))
    T.eq(v.ok, false)
    T.matches(v.error, 'not available')
    kora._current_tools = nil
end)

T.case('whitelist gate allows tool in current set', function()
    kora._current_tools = { noop = true }
    local v = json.decode(kora.dispatch_tool('noop', '{"x":"yes"}'))
    T.eq(v.ok, true)
    kora._current_tools = nil
end)

T.case('resolve_agent rejects unknown', function()
    local a, err = kora.resolve_agent('nope', nil)
    T.eq(a, nil)
    T.matches(err, 'unknown')
end)

T.case('resolve_agent enforces parent whitelist', function()
    -- tester declares { 'helper' }; we register helper but check we cannot
    -- reach an agent NOT declared by tester
    kora.register_agent {
        name = 'helper',
        system = 'helper prompt',
        tools = {},
        agents = {},
    }
    kora.register_agent {
        name = 'stranger',
        system = 'stranger prompt',
        tools = {},
        agents = {},
    }
    local a = kora.resolve_agent('helper', 'tester')
    T.truthy(a)
    local b, err = kora.resolve_agent('stranger', 'tester')
    T.eq(b, nil)
    T.matches(err, 'not declared')
end)

T.case('build_system_prompt includes tool section in harness mode', function()
    local p = kora.build_system_prompt('tester', 'harness')
    T.matches(p, 'test system prompt')
    T.matches(p, '## Tools')
    T.matches(p, 'noop')
end)

T.case('build_system_prompt omits tool section in native mode', function()
    local p = kora.build_system_prompt('tester', 'native')
    T.matches(p, 'test system prompt')
    T.eq(p:find('## Tools'), nil)  -- chat template injects tools natively
end)

T.case('build_system_prompt omits sections for empty agent', function()
    kora.register_agent {
        name = 'silent',
        system = 'silent prompt',
        tools = {},
        agents = {},
    }
    local p = kora.build_system_prompt('silent')
    T.eq(p:find('## Tools'), nil)
    T.eq(p:find('## Sub%-agents'), nil)
end)

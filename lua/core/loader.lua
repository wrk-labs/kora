-- Loader: discovers and registers tools and agents.
-- Called by C at startup, after the kora.* C bindings are installed.
--
-- Layout:
--   lua/tools/<name>.lua       -> tool definitions
--   lua/agents/<name>.lua      -> top-level agents (chat, code, ...)
--   lua/agents/sub/<name>.lua  -> sub-agents (explore, plan, ...)
--
-- Each tool/agent file returns a table; this loader hands it to
-- kora.register_tool / kora.register_agent (set up by lua_bridge in C).

local M = {}

local json = require('core.json')

-- ----- registries -----

kora = kora or {}
kora.tools = {}
kora.agents = {}

function kora.register_tool(t)
    assert(type(t) == 'table', 'tool must be a table')
    assert(type(t.name) == 'string', 'tool.name required')
    assert(type(t.run) == 'function', 'tool.run required')
    t.description = t.description or ''
    t.schema = t.schema or {}
    t.dangerous = t.dangerous or false
    kora.tools[t.name] = t
    return t
end

-- Convert a tool's lightweight schema table {key='string', key2='number?'}
-- to a JSON Schema string suitable for llama.cpp's common_chat_tool.
-- Optional fields end with '?'. Other type tokens recognized: string,
-- number, boolean, array, object.
function kora.tool_json_schema(tool)
    local props = {}
    local required = {}
    local first = true
    for key, type_token in pairs(tool.schema or {}) do
        local optional = false
        local t = type_token
        if type(t) == 'string' and t:sub(-1) == '?' then
            optional = true
            t = t:sub(1, -2)
        end
        local jt
        if t == 'string' then jt = 'string'
        elseif t == 'number' then jt = 'number'
        elseif t == 'boolean' then jt = 'boolean'
        elseif t == 'array' then jt = 'array'
        elseif t == 'object' then jt = 'object'
        else jt = 'string' end
        if not first then props[#props + 1] = ',' end
        first = false
        props[#props + 1] = string.format('%q:{"type":%q}', key, jt)
        if not optional then required[#required + 1] = string.format('%q', key) end
    end
    return string.format('{"type":"object","properties":{%s},"required":[%s]}',
        table.concat(props), table.concat(required, ','))
end

function kora.register_agent(a)
    assert(type(a) == 'table', 'agent must be a table')
    assert(type(a.name) == 'string', 'agent.name required')
    assert(type(a.system) == 'string', 'agent.system required')
    a.tools = a.tools or {}
    a.agents = a.agents or {}
    a.description = a.description or ''
    a.allow_inline_agents = a.allow_inline_agents or false
    a.safety = a.safety or 'paranoid'  -- default to safest mode
    assert(a.safety == 'paranoid' or a.safety == 'safe' or a.safety == 'unsafe',
        'agent.safety must be paranoid|safe|unsafe')
    -- normalize whitelists into sets for O(1) membership checks
    local tset, aset = {}, {}
    for _, n in ipairs(a.tools) do tset[n] = true end
    for _, n in ipairs(a.agents) do aset[n] = true end
    a._tool_set = tset
    a._agent_set = aset
    kora.agents[a.name] = a
    return a
end

-- ----- file discovery -----

-- LFS-free directory walk: rely on `ls` via io.popen.
-- this runs once at startup so the cost is irrelevant.
local function list_dir(path)
    local out = {}
    local p = io.popen('ls "' .. path .. '" 2>/dev/null')
    if not p then return out end
    for line in p:lines() do
        if line:match('%.lua$') then out[#out + 1] = line end
    end
    p:close()
    return out
end

local function load_module(modname)
    local ok, mod = pcall(require, modname)
    if not ok then
        io.stderr:write('kora: failed to load ' .. modname .. ': ' .. tostring(mod) .. '\n')
        return nil
    end
    return mod
end

-- luadir is the install path passed in from C (LUADIR macro)
function M.bootstrap(luadir)
    -- ensure require can find our modules
    package.path = luadir .. '/?.lua;' .. luadir .. '/?/init.lua;' .. package.path

    -- load tools
    local tools_dir = luadir .. '/tools'
    for _, fname in ipairs(list_dir(tools_dir)) do
        local name = fname:gsub('%.lua$', '')
        load_module('tools.' .. name)
    end

    -- load top-level agents
    local agents_dir = luadir .. '/agents'
    for _, fname in ipairs(list_dir(agents_dir)) do
        local name = fname:gsub('%.lua$', '')
        load_module('agents.' .. name)
    end

    -- load sub-agents
    local sub_dir = luadir .. '/agents/sub'
    for _, fname in ipairs(list_dir(sub_dir)) do
        local name = fname:gsub('%.lua$', '')
        load_module('agents.sub.' .. name)
    end
end

-- ----- dispatch (called from C) -----
-- C invokes this when the harness parser extracts a <tool_call> body.
-- args_json is the JSON object string from inside the tool_call.
-- returns a JSON-encoded result string.

-- Lenient salvage: when extract_tool_name in C fails (the body wasn't a
-- valid {"name":..., "arguments":...} object), C calls dispatch_tool with
-- name="" and args_json containing the raw <tool_call> body. We try to
-- match common malformed shapes:
--   <bash>...</bash>
--   <bash>command="ls"</bash>
--   {bash: ...}
-- and rewrite into the canonical form.
local function salvage(body)
    if not body then return nil end
    -- shape 1: <toolname>...</toolname>
    local tname, inner = body:match('<(%w+)>%s*(.-)%s*</%w+>')
    if tname and kora.tools[tname] then
        local t = kora.tools[tname]
        -- pick the first non-optional schema key as the implicit argument
        local first_key
        for k, _ in pairs(t.schema) do first_key = k; break end
        if not first_key then return tname, {} end
        -- key=value pattern inside?
        local v = inner:match('%w+%s*=%s*"(.-)"') or
                  inner:match('%w+%s*=%s*([^,%s]+)')
        if v then return tname, { [first_key] = v } end
        -- otherwise the entire inner is the value
        if inner and inner ~= '' then
            return tname, { [first_key] = inner }
        end
    end
    return nil
end

-- common placeholder strings the model emits when it doesn't actually
-- know what to pass. each one returns a directive error so the model is
-- told *what to do next* instead of just "missing argument".
local PLACEHOLDER_PATTERNS = {
    '^path/to/',
    '^/home/user/',
    '^/path/to/',
    '^example%.',
    '^your_',
    '^<.*>$',  -- <file_path>, <name>, etc
}

local function looks_like_placeholder(s)
    if type(s) ~= 'string' then return false end
    if s == '' then return true end
    for _, p in ipairs(PLACEHOLDER_PATTERNS) do
        if s:match(p) then return true end
    end
    return false
end

local function check_placeholder_args(tool, args)
    if type(args) ~= 'table' then return nil end
    for k, v in pairs(args) do
        if looks_like_placeholder(v) then
            return k, v
        end
    end
    return nil
end

function kora.dispatch_tool(name, args_json)
    -- salvage path: empty name means C's parser couldn't extract one
    if (not name or name == '') and args_json then
        local tname, targs = salvage(args_json)
        if tname then
            name = tname
            args_json = json.encode(targs)
        end
    end

    local tool = kora.tools[name]
    if not tool then
        return json.encode({ ok = false, error = 'unknown tool: ' .. (name or '?') })
    end

    -- per-run whitelist gate (set by C before calling)
    if kora._current_tools and not kora._current_tools[name] then
        return json.encode({ ok = false, error = 'tool not available to current agent: ' .. name })
    end

    local args, perr = json.decode(args_json or '{}')
    if not args then
        return json.encode({ ok = false, error = 'bad json arguments: ' .. tostring(perr) })
    end

    -- placeholder/empty argument check: catch the model passing fantasy
    -- values like "" or "/home/user/projects/main.c" before they hit the tool
    local pkey, pval = check_placeholder_args(tool, args)
    if pkey then
        return json.encode({
            ok = false,
            error = string.format(
                "argument '%s'=%q looks like a placeholder, not a real value. "
                .. "Use the `glob` tool to find real files first, or ask the user "
                .. "for the actual path. Do NOT invent paths.",
                pkey, tostring(pval)),
        })
    end

    local ok, result = pcall(tool.run, args)
    if not ok then
        return json.encode({ ok = false, error = tostring(result) })
    end

    -- truncate huge tool results so a single read of a big file doesn't
    -- bloat the conversation history and break subsequent turns. 8 KB
    -- (~2000 tokens) is enough for any sensible read, plus a clear note
    -- so the model knows the file was truncated.
    local function truncate(s)
        if type(s) ~= 'string' then return s end
        local cap = 8192
        if #s <= cap then return s end
        return s:sub(1, cap)
            .. '\n\n[... truncated by kora: '
            .. (#s - cap) .. ' more bytes. read with offset/limit for the rest ...]'
    end

    if type(result) == 'string' then
        return json.encode({ ok = true, content = truncate(result) })
    elseif type(result) == 'table' then
        if result.ok == nil then result.ok = true end
        if type(result.content) == 'string' then
            result.content = truncate(result.content)
        end
        return json.encode(result)
    else
        return json.encode({ ok = true, content = tostring(result) })
    end
end

-- agent resolver, used by the task tool and by C.
-- 'parent' is the parent agent name (or nil for top-level).
-- enforces the parent's declared `agents` whitelist.
function kora.resolve_agent(name, parent_name)
    local a = kora.agents[name]
    if not a then return nil, 'unknown agent: ' .. name end
    if parent_name then
        local parent = kora.agents[parent_name]
        if not parent then return nil, 'unknown parent agent' end
        if not parent._agent_set[name] then
            return nil, 'agent ' .. name .. ' not declared by ' .. parent_name
        end
    end
    return a
end

-- build the system prompt for an agent.
-- mode: "native" (default) or "harness". in native mode the chat template
-- injects the tools section per-model, so we don't include the format
-- teaching here. in harness mode we tell the model to emit <tool_call>
-- markers because there's no template-level support.
function kora.build_system_prompt(agent_name, mode)
    local a = kora.agents[agent_name]
    if not a then return nil, 'unknown agent: ' .. agent_name end
    mode = mode or 'native'

    local parts = { a.system }

    -- inject the project block (root walk + file index + symbols + README).
    -- the index module caches on first scan and reuses for subsequent calls
    -- as long as the cwd hasn't changed.
    local ok, index = pcall(require, 'core.index')
    if ok and index then
        local cwd_out = kora.shell_exec and kora.shell_exec('pwd', 1000) or '.'
        local cwd = (cwd_out or '.'):gsub('\n$', '')
        local proj = index.scan(cwd)
        local block = index.format_block(proj)
        if block ~= '' then parts[#parts + 1] = block end
    end

    if #a.tools > 0 and mode == 'harness' then
        parts[#parts + 1] = '\n\n## Tools\n\n'
        parts[#parts + 1] = 'To call a tool, emit a <tool_call> block containing a SINGLE JSON object with two keys: "name" (string) and "arguments" (object). Nothing else. No XML tags inside. No prose.\n\n'
        parts[#parts + 1] = 'EXAMPLE — to read /etc/hosts, output exactly:\n\n'
        parts[#parts + 1] = '<tool_call>\n{"name": "read", "arguments": {"file_path": "/etc/hosts"}}\n</tool_call>\n\n'
        parts[#parts + 1] = 'EXAMPLE — to run a shell command:\n\n'
        parts[#parts + 1] = '<tool_call>\n{"name": "bash", "arguments": {"command": "ls -la"}}\n</tool_call>\n\n'
        parts[#parts + 1] = 'After the </tool_call> tag, STOP generating. The system will execute the tool and respond with a <tool_result> block. Then you continue.\n\n'
        parts[#parts + 1] = 'Available tools:\n'
        for _, tname in ipairs(a.tools) do
            local t = kora.tools[tname]
            if t then
                parts[#parts + 1] = '- ' .. tname .. ': ' .. (t.description or '') .. '\n'
                if next(t.schema) then
                    parts[#parts + 1] = '  arguments: '
                    local sparts = {}
                    for k, v in pairs(t.schema) do
                        sparts[#sparts + 1] = k .. ' (' .. v .. ')'
                    end
                    parts[#parts + 1] = table.concat(sparts, ', ') .. '\n'
                end
            end
        end
    end
    -- in native mode, the chat template handles the format and the
    -- tool list is passed to common_chat_templates_apply structurally —
    -- so the system prompt stays clean.

    if #a.agents > 0 then
        parts[#parts + 1] = '\n## Sub-agents\n'
        parts[#parts + 1] = 'Use the `task` tool to delegate focused subtasks to these specialized agents:\n'
        for _, aname in ipairs(a.agents) do
            local sub = kora.agents[aname]
            if sub then
                parts[#parts + 1] = '- ' .. aname .. ': ' .. (sub.description or '') .. '\n'
            end
        end
    end

    return table.concat(parts)
end

return M

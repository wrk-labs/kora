-- Tests for the grep tool. shell_exec is mocked.

_G.kora = _G.kora or {}
local _calls = {}
local _mock_queue = {}  -- queue of (out, code) for sequential calls
function kora.shell_exec(cmd, timeout)
    _calls[#_calls + 1] = { cmd = cmd, timeout = timeout }
    if #_mock_queue > 0 then
        local r = table.remove(_mock_queue, 1)
        return r.out, r.code
    end
    return '', 0
end
local function queue(out, code)
    _mock_queue[#_mock_queue + 1] = { out = out, code = code }
end
local function reset()
    _calls = {}
    _mock_queue = {}
end
function kora.todos_set(_) end
function kora.todos_get() return {} end
function kora.event_push(_, _) end
function kora.loop_run(_, _) return nil, 'stub' end
function kora.confirm(_) return 'y' end

require('core.loader')
require('tools.grep')
T.truthy(kora.tools.grep)
local grep = kora.tools.grep.run

T.case('returns matches with rg available', function()
    reset()
    queue('yes\n', 0)            -- rg detection
    queue('src/main.c:42:hello\n', 0)
    local r = grep { pattern = 'hello' }
    T.eq(r.ok, true)
    T.matches(r.content, 'src/main%.c:42:hello')
    -- second call should be rg, not grep
    T.matches(_calls[2].cmd, '^rg ')
end)

T.case('falls back to grep when rg missing', function()
    reset()
    queue('', 0)                  -- rg detection: no
    queue('main.c:1:foo\n', 0)
    local r = grep { pattern = 'foo' }
    T.eq(r.ok, true)
    T.matches(_calls[2].cmd, '^grep ')
end)

T.case('strips perl-style /pattern/ delimiters', function()
    reset()
    queue('yes\n', 0)
    queue('', 0)
    grep { pattern = '/foo/' }
    -- the rg call should have "foo" not "/foo/"
    T.truthy(_calls[2].cmd:find('"foo"', 1, true))
    T.eq(_calls[2].cmd:find('"/foo/"', 1, true), nil)
end)

T.case('no matches returns helpful message', function()
    reset()
    queue('yes\n', 0)
    queue('', 1)  -- grep/rg "no matches" exit code
    local r = grep { pattern = 'xyz' }
    T.eq(r.ok, true)
    T.matches(r.content, 'no matches')
end)

T.case('passes glob filter to rg', function()
    reset()
    queue('yes\n', 0)
    queue('', 0)
    grep { pattern = 'foo', glob = '*.c' }
    T.truthy(_calls[2].cmd:find('-g ', 1, true))
end)

T.case('passes glob to grep --include', function()
    reset()
    queue('', 0)  -- no rg
    queue('', 0)
    grep { pattern = 'foo', glob = '*.c' }
    T.truthy(_calls[2].cmd:find('--include=', 1, true))
end)

T.case('missing pattern errors', function()
    reset()
    local r = grep {}
    T.eq(r.ok, false)
    T.matches(r.error, 'pattern required')
end)

T.case('empty pattern after stripping delimiters errors', function()
    reset()
    local r = grep { pattern = '//' }
    T.eq(r.ok, false)
    T.matches(r.error, 'empty after stripping')
end)

T.case('shell error returns directive', function()
    reset()
    queue('yes\n', 0)
    queue('', 2)  -- arbitrary error code
    local r = grep { pattern = 'foo' }
    T.eq(r.ok, false)
    T.matches(r.error, 'grep failed')
end)

T.case('long output gets truncation note', function()
    reset()
    queue('yes\n', 0)
    -- 200 lines of fake matches (the cap is 200, so this hits the cap)
    local lines = {}
    for i = 1, 200 do lines[i] = 'f.c:' .. i .. ':match' end
    queue(table.concat(lines, '\n') .. '\n', 0)
    local r = grep { pattern = 'match' }
    T.eq(r.ok, true)
    T.matches(r.content, 'truncated at 200')
end)

T.case('default path is cwd', function()
    reset()
    queue('yes\n', 0)
    queue('', 0)
    grep { pattern = 'foo' }
    T.truthy(_calls[2].cmd:find('"."', 1, true))
end)

T.case('non-string pattern errors', function()
    reset()
    local r = grep { pattern = 42 }
    T.eq(r.ok, false)
end)

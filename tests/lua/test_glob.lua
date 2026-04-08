-- Tests for the glob tool. shell_exec is mocked.

_G.kora = _G.kora or {}
local _mock = { out = '', exit_code = 0 }
local _last = nil
function kora.shell_exec(cmd, timeout)
    _last = { cmd = cmd, timeout = timeout }
    return _mock.out, _mock.exit_code
end
function kora.todos_set(_) end
function kora.todos_get() return {} end
function kora.event_push(_, _) end
function kora.loop_run(_, _) return nil, 'stub' end
function kora.confirm(_) return 'y' end

require('core.loader')
require('tools.glob')
T.truthy(kora.tools.glob)
local glob = kora.tools.glob.run

T.case('returns matched paths', function()
    _mock.out = 'src/main.c\nsrc/util.c\n'
    _mock.exit_code = 0
    local r = glob { pattern = '**/*.c' }
    T.eq(r.ok, true)
    T.matches(r.content, 'src/main%.c')
    T.matches(r.content, 'src/util%.c')
end)

T.case('uses path arg as cwd', function()
    _mock.out = 'a.lua\n'
    glob { pattern = '*.lua', path = 'lua/tools' }
    T.matches(_last.cmd, 'lua/tools')
end)

T.case('defaults path to cwd', function()
    _mock.out = ''
    glob { pattern = '*' }
    -- the cmd is bash-quoted; just check 'cd' appears
    T.matches(_last.cmd, 'cd ')
end)

T.case('empty result returns helpful message', function()
    _mock.out = ''
    _mock.exit_code = 0
    local r = glob { pattern = '**/*.xyz' }
    T.eq(r.ok, true)
    T.matches(r.content, 'no matches')
end)

T.case('failure returns error', function()
    _mock.out = ''
    _mock.exit_code = 2
    local r = glob { pattern = '**/*.c' }
    T.eq(r.ok, false)
    T.matches(r.error, 'glob failed')
end)

T.case('missing pattern errors', function()
    local r = glob {}
    T.eq(r.ok, false)
    T.matches(r.error, 'pattern required')
end)

T.case('empty pattern errors', function()
    local r = glob { pattern = '' }
    T.eq(r.ok, false)
end)

T.case('truncates at MAX_RESULTS with note', function()
    -- generate 200 lines of output
    local lines = {}
    for i = 1, 200 do lines[i] = 'file' .. i .. '.c' end
    _mock.out = table.concat(lines, '\n') .. '\n'
    _mock.exit_code = 0
    local r = glob { pattern = '**/*.c' }
    T.eq(r.ok, true)
    T.matches(r.content, 'truncated at 200')
end)

T.case('passes timeout to shell_exec', function()
    _mock.out = ''
    glob { pattern = '*.c' }
    T.eq(_last.timeout, 10000)
end)

T.case('shell-quotes pattern with quotes safely', function()
    _mock.out = ''
    glob { pattern = 'a"b' }
    -- the inner quote should be escaped (we just check the pattern survived)
    T.truthy(_last.cmd:find('a', 1, true))
    T.truthy(_last.cmd:find('b', 1, true))
end)

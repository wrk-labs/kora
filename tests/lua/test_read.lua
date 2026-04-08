-- Tests for the read tool: slice-by-default, line numbers, pagination,
-- hard cap, edge cases.

_G.kora = _G.kora or {}
function kora.shell_exec(_, _) return '', 0 end
function kora.todos_set(_) end
function kora.todos_get() return {} end
function kora.event_push(_, _) end
function kora.loop_run(_, _) return nil, 'stub' end
function kora.confirm(_) return 'y' end

require('core.loader')
require('tools.read')
T.truthy(kora.tools.read)
local read = kora.tools.read.run

local function tmpfile(content)
    local path = os.tmpname()
    local f = io.open(path, 'w')
    f:write(content)
    f:close()
    return path
end

local function nlines(n)
    local t = {}
    for i = 1, n do t[i] = 'line ' .. i end
    return table.concat(t, '\n') .. '\n'
end

T.case('reads small file with line numbers', function()
    local p = tmpfile('hello\nworld\n')
    local r = read { file_path = p }
    T.eq(r.ok, true)
    T.matches(r.content, '1  hello')
    T.matches(r.content, '2  world')
    os.remove(p)
end)

T.case('default limit is 200 lines', function()
    local p = tmpfile(nlines(500))
    local r = read { file_path = p }
    T.eq(r.ok, true)
    -- should contain line 200 but NOT line 201
    T.matches(r.content, '200  line 200')
    T.eq(r.content:find('201  line 201'), nil)
    os.remove(p)
end)

T.case('shows total + paginate hint when truncated', function()
    local p = tmpfile(nlines(500))
    local r = read { file_path = p }
    T.matches(r.content, 'showing lines 1%-200 of 500')
    T.matches(r.content, '300 more lines')
    T.matches(r.content, 'offset=201')
    os.remove(p)
end)

T.case('offset paginates from a given line', function()
    local p = tmpfile(nlines(500))
    local r = read { file_path = p, offset = 201 }
    T.eq(r.ok, true)
    T.matches(r.content, '201  line 201')
    T.matches(r.content, '400  line 400')
    T.eq(r.content:find('200  line 200'), nil)
    os.remove(p)
end)

T.case('limit caps the slice', function()
    local p = tmpfile(nlines(500))
    local r = read { file_path = p, limit = 5 }
    T.eq(r.ok, true)
    T.matches(r.content, '1  line 1')
    T.matches(r.content, '5  line 5')
    T.eq(r.content:find('6  line 6'), nil)
    T.matches(r.content, 'showing lines 1%-5 of 500')
    os.remove(p)
end)

T.case('limit is hard-capped at 500', function()
    local p = tmpfile(nlines(2000))
    local r = read { file_path = p, limit = 9999 }
    T.eq(r.ok, true)
    T.matches(r.content, '500  line 500')
    T.eq(r.content:find('501  line 501'), nil)
    T.matches(r.content, 'clamped to 500')
    os.remove(p)
end)

T.case('offset past end of file errors', function()
    local p = tmpfile(nlines(10))
    local r = read { file_path = p, offset = 999 }
    T.eq(r.ok, false)
    T.matches(r.error, 'past end of file')
    T.matches(r.error, '10 lines')
    os.remove(p)
end)

T.case('empty file is handled', function()
    local p = tmpfile('')
    local r = read { file_path = p }
    T.eq(r.ok, true)
    T.matches(r.content, '%[empty file%]')
    os.remove(p)
end)

T.case('missing file_path errors clearly', function()
    local r = read {}
    T.eq(r.ok, false)
    T.matches(r.error, 'file_path required')
end)

T.case('non-existent file errors', function()
    local r = read { file_path = '/nonexistent/path/that/does/not/exist' }
    T.eq(r.ok, false)
end)

T.case('line number padding aligns when crossing magnitudes', function()
    -- 99 lines: width=2 → "99  line 99"
    -- 100 lines: width=3 → " 99  line 99" and "100 line 100"
    local p = tmpfile(nlines(100))
    local r = read { file_path = p }
    T.matches(r.content, '100  line 100')
    -- earlier lines should be padded to width 3
    T.matches(r.content, ' 99  line 99')
    os.remove(p)
end)

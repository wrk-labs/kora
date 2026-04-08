-- Test the edit tool against real tmp files. Stubs the C bindings so the
-- tool file can be required directly.

_G.kora = _G.kora or {}
function kora.shell_exec(_, _) return '', 0 end
function kora.todos_set(_) end
function kora.todos_get() return {} end
function kora.event_push(_, _) end
function kora.loop_run(_, _) return nil, 'stub' end
function kora.confirm(_) return 'y' end

require('core.loader')

-- loading the file registers the tool
local edit = require('tools.edit')
T.truthy(kora.tools.edit)

local function tmpfile(content)
    local path = os.tmpname()
    local f = io.open(path, 'w')
    f:write(content)
    f:close()
    return path
end

local function readall(path)
    local f = io.open(path, 'r')
    local s = f:read('*a')
    f:close()
    return s
end

T.case('edit replaces unique substring', function()
    local p = tmpfile('hello world\nfoo bar\n')
    local r = kora.tools.edit.run {
        file_path = p,
        old_string = 'world',
        new_string = 'lua',
    }
    T.eq(r.ok, true)
    T.eq(readall(p), 'hello lua\nfoo bar\n')
    os.remove(p)
end)

T.case('edit fails when old_string not found', function()
    local p = tmpfile('abc\n')
    local r = kora.tools.edit.run {
        file_path = p,
        old_string = 'xyz',
        new_string = 'qqq',
    }
    T.eq(r.ok, false)
    T.matches(r.error, 'not found')
    os.remove(p)
end)

T.case('edit fails when old_string not unique', function()
    local p = tmpfile('foo foo foo\n')
    local r = kora.tools.edit.run {
        file_path = p,
        old_string = 'foo',
        new_string = 'bar',
    }
    T.eq(r.ok, false)
    T.matches(r.error, 'not unique')
    os.remove(p)
end)

T.case('edit replace_all replaces every occurrence', function()
    local p = tmpfile('foo foo foo\n')
    local r = kora.tools.edit.run {
        file_path = p,
        old_string = 'foo',
        new_string = 'bar',
        replace_all = true,
    }
    T.eq(r.ok, true)
    T.eq(readall(p), 'bar bar bar\n')
    os.remove(p)
end)

T.case('edit handles lua pattern metacharacters in old_string', function()
    -- '.' '%' '(' ')' '+' '*' '?' '[' ']' '^' '$' all special in lua patterns
    local p = tmpfile('a.b(c)+[d]^e$f%g\n')
    local r = kora.tools.edit.run {
        file_path = p,
        old_string = 'a.b(c)+[d]^e$f%g',
        new_string = 'REPLACED',
    }
    T.eq(r.ok, true)
    T.eq(readall(p), 'REPLACED\n')
    os.remove(p)
end)

T.case('edit handles % in new_string', function()
    local p = tmpfile('placeholder\n')
    local r = kora.tools.edit.run {
        file_path = p,
        old_string = 'placeholder',
        new_string = '100% sure',
    }
    T.eq(r.ok, true)
    T.eq(readall(p), '100% sure\n')
    os.remove(p)
end)

T.case('edit fails on missing file with directive error', function()
    local r = kora.tools.edit.run {
        file_path = '/nonexistent/path/that/does/not/exist',
        old_string = 'a',
        new_string = 'b',
    }
    T.eq(r.ok, false)
    T.matches(r.error, 'does not exist')
    -- directive: tells the model what to do next
    T.matches(r.error, 'glob')
    T.matches(r.error, 'write')
end)

T.case('edit refuses empty old_string', function()
    local p = tmpfile('something\n')
    local r = kora.tools.edit.run {
        file_path = p,
        old_string = '',
        new_string = 'whatever',
    }
    T.eq(r.ok, false)
    T.matches(r.error, 'empty')
    -- directive: points to the right tool
    T.matches(r.error, 'write')
    os.remove(p)
end)

T.case('edit refuses missing file_path', function()
    local r = kora.tools.edit.run {
        old_string = 'a',
        new_string = 'b',
    }
    T.eq(r.ok, false)
    T.matches(r.error, 'file_path required')
end)

T.case('edit refuses missing old_string', function()
    local p = tmpfile('x\n')
    local r = kora.tools.edit.run {
        file_path = p,
        new_string = 'y',
    }
    T.eq(r.ok, false)
    T.matches(r.error, 'old_string and new_string required')
    os.remove(p)
end)

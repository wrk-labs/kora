-- Tests for the write tool: happy path, validation, parent dir creation,
-- size cap, error handling.

_G.kora = _G.kora or {}
function kora.shell_exec(_, _) return '', 0 end
function kora.todos_set(_) end
function kora.todos_get() return {} end
function kora.event_push(_, _) end
function kora.loop_run(_, _) return nil, 'stub' end
function kora.confirm(_) return 'y' end

require('core.loader')
require('tools.write')
T.truthy(kora.tools.write)
local write = kora.tools.write.run

local function readall(path)
    local f = io.open(path, 'r')
    if not f then return nil end
    local s = f:read('*a')
    f:close()
    return s
end

T.case('writes a file with content', function()
    local p = os.tmpname()
    local r = write { file_path = p, content = 'hello world' }
    T.eq(r.ok, true)
    T.matches(r.content, 'wrote 11 bytes')
    T.eq(readall(p), 'hello world')
    os.remove(p)
end)

T.case('overwrites existing file', function()
    local p = os.tmpname()
    write { file_path = p, content = 'first' }
    local r = write { file_path = p, content = 'second' }
    T.eq(r.ok, true)
    T.eq(readall(p), 'second')
    os.remove(p)
end)

T.case('creates parent directories', function()
    local base = os.tmpname()
    os.remove(base)  -- delete the file, use the path as a dir
    local p = base .. '/sub/nested/file.txt'
    local r = write { file_path = p, content = 'deep' }
    T.eq(r.ok, true)
    T.eq(readall(p), 'deep')
    os.remove(p)
    -- cleanup
    os.execute('rm -rf "' .. base .. '"')
end)

T.case('empty content is allowed', function()
    local p = os.tmpname()
    local r = write { file_path = p, content = '' }
    T.eq(r.ok, true)
    T.eq(readall(p), '')
    os.remove(p)
end)

T.case('missing file_path errors', function()
    local r = write { content = 'x' }
    T.eq(r.ok, false)
    T.matches(r.error, 'file_path required')
end)

T.case('empty file_path errors', function()
    local r = write { file_path = '', content = 'x' }
    T.eq(r.ok, false)
    T.matches(r.error, 'file_path required')
end)

T.case('nil content errors', function()
    local r = write { file_path = '/tmp/whatever' }
    T.eq(r.ok, false)
    T.matches(r.error, 'content required')
end)

T.case('non-string content errors', function()
    local r = write { file_path = '/tmp/whatever', content = 42 }
    T.eq(r.ok, false)
    T.matches(r.error, 'content required')
end)

T.case('size cap at 1 MB', function()
    local big = string.rep('x', 1024 * 1024 + 1)
    local r = write { file_path = '/tmp/kora-test-big', content = big }
    T.eq(r.ok, false)
    T.matches(r.error, 'too large')
end)

T.case('write to read-only path errors gracefully', function()
    local r = write { file_path = '/proc/this/should/never/exist/file', content = 'x' }
    T.eq(r.ok, false)
    T.truthy(r.error)
end)

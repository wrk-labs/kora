-- Minimal Lua test runner. Quiet by default — one line per file.
-- Set V=1 in the environment for verbose (per-case) output.
--
-- A test file calls T.case('name', function() T.eq(...) end)

local T = {}
_G.T = T

local C_GREEN  = '\27[32m'
local C_RED    = '\27[31m'
local C_DIM    = '\27[2m'
local C_BOLD   = '\27[1m'
local C_RESET  = '\27[0m'

local verbose = (os.getenv('V') == '1')

-- per-file state, reset for each loaded file
local file_pass, file_fail, file_failures
local total_pass, total_fail = 0, 0

function T.case(name, fn)
    local ok, err = pcall(fn)
    if ok then
        file_pass = file_pass + 1
        if verbose then
            io.write('    ' .. C_GREEN .. 'ok  ' .. C_RESET .. name .. '\n')
        end
    else
        file_fail = file_fail + 1
        file_failures[#file_failures + 1] = { name = name, err = tostring(err) }
    end
end

function T.eq(a, b, msg)
    if a ~= b then
        error((msg or 'eq') .. ': expected ' .. tostring(b) .. ', got ' .. tostring(a), 2)
    end
end

function T.deep_eq(a, b, msg)
    local function eq(x, y)
        if type(x) ~= type(y) then return false end
        if type(x) ~= 'table' then return x == y end
        for k, v in pairs(x) do if not eq(v, y[k]) then return false end end
        for k in pairs(y) do if x[k] == nil then return false end end
        return true
    end
    if not eq(a, b) then
        error((msg or 'deep_eq') .. ': tables differ', 2)
    end
end

function T.truthy(v, msg)
    if not v then error((msg or 'truthy') .. ': value was falsy', 2) end
end

function T.falsy(v, msg)
    if v then error((msg or 'falsy') .. ': expected falsy, got ' .. tostring(v), 2) end
end

function T.matches(s, pat, msg)
    if not (type(s) == 'string' and s:find(pat)) then
        error((msg or 'matches') .. ': ' .. tostring(s) .. ' does not match ' .. pat, 2)
    end
end

-- locate kora root from this file
local script = arg[0] or 'tests/lua/run.lua'
local root = script:match('(.*)/tests/lua/run%.lua$') or '.'
package.path = root .. '/lua/?.lua;' .. root .. '/lua/?/init.lua;' ..
               root .. '/tests/lua/?.lua;' .. package.path

local files
if #arg > 0 then
    files = arg
else
    files = {
        root .. '/tests/lua/test_json.lua',
        root .. '/tests/lua/test_loader.lua',
        root .. '/tests/lua/test_edit.lua',
        root .. '/tests/lua/test_index.lua',
        root .. '/tests/lua/test_read.lua',
        root .. '/tests/lua/test_write.lua',
        root .. '/tests/lua/test_bash.lua',
        root .. '/tests/lua/test_glob.lua',
        root .. '/tests/lua/test_grep.lua',
        root .. '/tests/lua/test_todo.lua',
    }
end

-- short label for each file
local function short(path)
    return (path:match('tests/lua/(.+)%.lua$')) or path
end

-- pad helper
local function pad(s, n)
    if #s >= n then return s end
    return s .. string.rep(' ', n - #s)
end

local all_failures = {}

for _, f in ipairs(files) do
    file_pass, file_fail = 0, 0
    file_failures = {}

    local label = short(f)
    if verbose then io.write(C_BOLD .. label .. C_RESET .. '\n') end

    local chunk, err = loadfile(f)
    if not chunk then
        io.write('  ' .. C_RED .. 'LOAD' .. C_RESET .. ' ' .. label
            .. ': ' .. err .. '\n')
        total_fail = total_fail + 1
    else
        local ok, perr = pcall(chunk)
        if not ok then
            io.write('  ' .. C_RED .. 'CRASH' .. C_RESET .. ' ' .. label
                .. ': ' .. tostring(perr) .. '\n')
            file_fail = file_fail + 1
        end

        local total = file_pass + file_fail
        local mark = (file_fail == 0)
            and (C_GREEN .. 'ok  ' .. C_RESET)
            or  (C_RED   .. 'FAIL' .. C_RESET)
        io.write(string.format('  %s %s %s%d/%d%s\n',
            mark, pad(label, 28),
            C_DIM, file_pass, total, C_RESET))
    end

    total_pass = total_pass + file_pass
    total_fail = total_fail + file_fail

    for _, fl in ipairs(file_failures) do
        all_failures[#all_failures + 1] = { file = f, name = fl.name, err = fl.err }
    end
end

-- detail block for failures
if #all_failures > 0 then
    io.write('\n' .. C_BOLD .. 'failures:' .. C_RESET .. '\n')
    for _, fl in ipairs(all_failures) do
        io.write('  ' .. C_RED .. '✗' .. C_RESET .. ' '
            .. short(fl.file) .. ' :: ' .. fl.name .. '\n')
        io.write('    ' .. C_DIM .. fl.err .. C_RESET .. '\n')
    end
end

local total = total_pass + total_fail
io.write('\n')
if total_fail == 0 then
    io.write(string.format('%s%d passed%s in %d files\n',
        C_GREEN, total_pass, C_RESET, #files))
else
    io.write(string.format('%s%d passed, %d failed%s of %d\n',
        C_RED, total_pass, total_fail, C_RESET, total))
end

os.exit(total_fail == 0 and 0 or 1)

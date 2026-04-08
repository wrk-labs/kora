-- Tests for the bash tool. The actual shell execution is mocked via the
-- kora.shell_exec stub so we can verify the contract without spawning
-- subprocesses or depending on the C side.

_G.kora = _G.kora or {}

-- mock shell_exec — stores last call so tests can inspect it, returns
-- whatever the test sets via _mock
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
require('tools.bash')
T.truthy(kora.tools.bash)
local bash = kora.tools.bash.run

T.case('runs a command, returns stdout + ok=true', function()
    _mock.out = 'hello\n'
    _mock.exit_code = 0
    local r = bash { command = 'echo hello' }
    T.eq(r.ok, true)
    T.eq(r.content, 'hello\n')
    T.eq(r.exit_code, 0)
    T.eq(_last.cmd, 'echo hello')
end)

T.case('non-zero exit makes ok=false', function()
    _mock.out = ''
    _mock.exit_code = 1
    local r = bash { command = 'false' }
    T.eq(r.ok, false)
    T.eq(r.exit_code, 1)
end)

T.case('timeout/kill returns ok=false with error', function()
    _mock.out = 'partial output'
    _mock.exit_code = -1
    local r = bash { command = 'sleep 999' }
    T.eq(r.ok, false)
    T.eq(r.exit_code, -1)
    T.matches(r.error, 'timed out')
end)

T.case('default timeout is 30000ms', function()
    _mock.out = ''
    _mock.exit_code = 0
    bash { command = 'true' }
    T.eq(_last.timeout, 30000)
end)

T.case('explicit timeout is passed through', function()
    bash { command = 'true', timeout_ms = 5000 }
    T.eq(_last.timeout, 5000)
end)

T.case('timeout is clamped to min 100ms', function()
    bash { command = 'true', timeout_ms = 1 }
    T.eq(_last.timeout, 100)
end)

T.case('timeout is clamped to max 60s', function()
    bash { command = 'true', timeout_ms = 999999 }
    T.eq(_last.timeout, 60000)
end)

T.case('missing command errors', function()
    local r = bash {}
    T.eq(r.ok, false)
    T.matches(r.error, 'command required')
end)

T.case('empty command errors', function()
    local r = bash { command = '' }
    T.eq(r.ok, false)
    T.matches(r.error, 'command required')
end)

T.case('non-string command errors', function()
    local r = bash { command = 42 }
    T.eq(r.ok, false)
    T.matches(r.error, 'command required')
end)

T.case('blocks rm -rf /', function()
    local r = bash { command = 'rm -rf /' }
    T.eq(r.ok, false)
    T.matches(r.error, 'blocked pattern')
end)

T.case('blocks rm -rf /*', function()
    local r = bash { command = 'rm -rf /*' }
    T.eq(r.ok, false)
end)

T.case('blocks mkfs', function()
    local r = bash { command = 'mkfs.ext4 /dev/sda1' }
    T.eq(r.ok, false)
end)

T.case('blocks shutdown / reboot', function()
    T.eq(bash { command = 'shutdown -h now' }.ok, false)
    T.eq(bash { command = 'reboot' }.ok, false)
end)

T.case('blocks dd to disk', function()
    local r = bash { command = 'dd if=/dev/zero of=/dev/sda' }
    T.eq(r.ok, false)
end)

T.case('blocks fork bomb', function()
    local r = bash { command = ':() { :|: & }; :' }
    T.eq(r.ok, false)
end)

T.case('safe rm -rf in subdir is allowed', function()
    _mock.out = ''
    _mock.exit_code = 0
    local r = bash { command = 'rm -rf build/tmp' }
    T.eq(r.ok, true)
end)

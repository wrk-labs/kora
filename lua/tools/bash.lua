-- Execute a shell command via the C-side kora.shell_exec.
--
-- The C side enforces a hard timeout (kills the process group on expiry),
-- a 256 KB output cap, and runs everything via /bin/sh -c. Lua can't do
-- any of this safely on its own (io.popen can't kill runaway processes).
--
-- Defensive contract:
--   - command must be a non-empty string
--   - timeout_ms is clamped to [100ms, 60s] — anything outside is suspicious
--   - exit_code -1 means killed by timeout or output cap
--   - blocked patterns: a small denylist of obviously destructive commands
--     that should never be run from an agent without an explicit user ask

local MIN_TIMEOUT = 100
local MAX_TIMEOUT = 60000
local DEFAULT_TIMEOUT = 30000

-- substring patterns we hard-refuse. catches the most catastrophic
-- mistakes a small model can make. not security — that's the permission
-- gate's job — this is just an extra net for the worst case.
local BLOCKED = {
    'rm %-rf /',          -- recursive root delete
    'rm %-rf /%*',        -- same with glob
    ':%(%)%s*{%s*:|',     -- fork bomb
    'mkfs',               -- filesystem format
    'dd if=.+ of=/dev/',  -- raw disk write
    '> /dev/sd',          -- raw disk redirect
    'shutdown',           -- power off
    'reboot',             -- reboot
    'halt',               -- halt
}

return kora.register_tool {
    name = 'bash',
    description = 'Execute a shell command and return its stdout/stderr. Use for system operations; prefer dedicated tools (read/write/edit/glob/grep) when applicable. Has a hard timeout (default 30s, max 60s) and 256 KB output cap.',
    schema = {
        command = 'string',
        timeout_ms = 'number?',
        description = 'string?',
    },
    dangerous = true,
    run = function(args)
        if type(args.command) ~= 'string' or args.command == '' then
            return { ok = false, error = 'command required (non-empty string)' }
        end

        -- denylist check
        for _, pat in ipairs(BLOCKED) do
            if args.command:find(pat) then
                return {
                    ok = false,
                    error = 'command matches blocked pattern (' .. pat .. '). '
                        .. 'kora refuses obviously destructive commands. If '
                        .. 'this is intentional, run it directly in the shell.',
                }
            end
        end

        local timeout = tonumber(args.timeout_ms) or DEFAULT_TIMEOUT
        if timeout < MIN_TIMEOUT then timeout = MIN_TIMEOUT end
        if timeout > MAX_TIMEOUT then timeout = MAX_TIMEOUT end

        local out, exit_code = kora.shell_exec(args.command, timeout)
        out = out or ''

        local result = {
            ok = (exit_code == 0),
            content = out,
            exit_code = exit_code,
        }
        if exit_code == -1 then
            result.error = 'command timed out or exceeded output cap (was killed)'
        end
        return result
    end,
}

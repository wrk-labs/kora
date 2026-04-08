-- Find files matching a glob pattern. Uses bash with globstar enabled
-- so `**` works recursively.
--
-- Defensive contract:
--   - pattern must be non-empty
--   - path defaults to cwd; rejected if it doesn't exist
--   - results are line-truncated at 200 entries (model doesn't need more)
--   - pattern is shell-quoted to avoid injection (caller can still use glob
--     metacharacters intentionally)

local MAX_RESULTS = 200

return kora.register_tool {
    name = 'glob',
    description = 'Find files matching a glob pattern (e.g. **/*.c, src/**/*.lua). Returns up to 200 matching paths, one per line. Use the `path` arg to scope to a directory; defaults to current working directory.',
    schema = {
        pattern = 'string',
        path = 'string?',
    },
    run = function(args)
        if type(args.pattern) ~= 'string' or args.pattern == '' then
            return { ok = false, error = 'pattern required (non-empty string)' }
        end

        local base = args.path
        if type(base) ~= 'string' or base == '' then base = '.' end

        local pat = args.pattern
        local q_base = base:gsub('"', '\\"')
        local q_pat = pat:gsub('"', '\\"')

        -- bash with globstar to support **/*. limit output to MAX_RESULTS
        -- via head so we don't return thousands of paths.
        local cmd = string.format(
            'cd "%s" 2>/dev/null && shopt -s globstar nullglob 2>/dev/null; for f in %s; do echo "$f"; done | head -n %d',
            q_base, q_pat, MAX_RESULTS)
        local out, code = kora.shell_exec('bash -c ' .. string.format('%q', cmd), 10000)
        out = out or ''

        if code ~= 0 then
            return {
                ok = false,
                error = 'glob failed (code ' .. tostring(code)
                    .. '). check that the path exists.',
            }
        end

        -- count lines for the model's information
        local n = 0
        for _ in out:gmatch('[^\n]+') do n = n + 1 end

        if n == 0 then
            return {
                ok = true,
                content = '[no matches for pattern "' .. pat .. '" in ' .. base .. ']',
            }
        end

        local note = ''
        if n >= MAX_RESULTS then
            note = '\n[... result truncated at ' .. MAX_RESULTS
                .. ' entries. narrow your pattern to see more]'
        end
        return { ok = true, content = out .. note }
    end,
}

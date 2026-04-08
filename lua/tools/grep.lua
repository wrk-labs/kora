-- Search file contents. Prefers ripgrep if available, falls back to grep -rn.
--
-- Defensive contract:
--   - pattern is required and non-empty
--   - perl-style /pattern/ delimiters are silently stripped (small models
--     emit them by habit; this fixes the call before it runs)
--   - results are line-truncated at 200 lines so a broad search can't
--     dump the entire codebase into the conversation
--   - exit code 1 from grep/rg means "no matches" and is treated as success

local MAX_LINES = 200

return kora.register_tool {
    name = 'grep',
    description = 'Search for a regex pattern across files. Returns matching lines as file:line:text. Use `path` to scope to a directory and `glob` to filter file types (e.g. "*.c"). Capped at 200 lines per call — narrow your pattern if you hit the cap.',
    schema = {
        pattern = 'string',
        path = 'string?',
        glob = 'string?',
    },
    run = function(args)
        local pat = args.pattern
        if type(pat) ~= 'string' or pat == '' then
            return { ok = false, error = 'pattern required (non-empty string)' }
        end

        -- strip perl-style /pattern/ delimiters that small models emit
        if #pat >= 2 and pat:sub(1, 1) == '/' and pat:sub(-1) == '/' then
            pat = pat:sub(2, -2)
        end
        if pat == '' then
            return { ok = false, error = 'pattern is empty after stripping delimiters' }
        end

        local path = args.path
        if type(path) ~= 'string' or path == '' then path = '.' end
        local glob = args.glob

        -- detect rg
        local has_rg = kora.shell_exec('command -v rg >/dev/null 2>&1 && echo yes', 2000)
        local cmd
        if has_rg and has_rg:match('yes') then
            cmd = string.format('rg -n --color=never %s %s %s | head -n %d',
                glob and ('-g ' .. string.format('%q', glob)) or '',
                string.format('%q', pat),
                string.format('%q', path),
                MAX_LINES)
        else
            cmd = string.format('grep -rn %s %s %s 2>/dev/null | head -n %d',
                glob and ('--include=' .. string.format('%q', glob)) or '',
                string.format('%q', pat),
                string.format('%q', path),
                MAX_LINES)
        end

        local out, code = kora.shell_exec(cmd, 15000)
        out = out or ''

        -- code 1 from grep/rg means "no matches" — not an error.
        -- with `| head` the exit code may be 0 even if grep itself returned 1.
        if code ~= 0 and code ~= 1 then
            return {
                ok = false,
                error = 'grep failed (code ' .. tostring(code)
                    .. '). check that the path exists and the pattern is valid.',
            }
        end

        if out == '' then
            return {
                ok = true,
                content = '[no matches for "' .. pat .. '" in ' .. path .. ']',
            }
        end

        -- count lines, add truncation note if at the cap
        local n = 0
        for _ in out:gmatch('[^\n]+') do n = n + 1 end
        if n >= MAX_LINES then
            out = out .. '\n[... result truncated at ' .. MAX_LINES
                .. ' lines. narrow your pattern, scope with `path`, or filter with `glob`]'
        end
        return { ok = true, content = out }
    end,
}

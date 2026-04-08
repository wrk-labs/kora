-- Write content to a file, creating parent directories as needed.
--
-- Defensive contract:
--   - file_path must be non-empty (no placeholders, dispatcher catches those too)
--   - content must be a string (not nil, not a number, not a table)
--   - parent directory is created via mkdir -p (path is shell-quoted)
--   - hard size cap of 1 MB per write to prevent runaway disk fill
--   - returns a confirmation with byte count + absolute path
--
-- Always rejected:
--   - empty file_path
--   - nil content (passing "" is fine — that's an explicit empty file)
--   - content larger than the cap

local MAX_BYTES = 1024 * 1024  -- 1 MB

return kora.register_tool {
    name = 'write',
    description = 'Write content to a file at an absolute path, overwriting any existing file. Creates parent directories. Max 1 MB per write. Use `edit` for in-place changes to existing files.',
    schema = { file_path = 'string', content = 'string' },
    dangerous = true,
    run = function(args)
        local path = args.file_path
        if type(path) ~= 'string' or path == '' then
            return { ok = false, error = 'file_path required (non-empty string)' }
        end
        if type(args.content) ~= 'string' then
            return {
                ok = false,
                error = 'content required (must be a string; pass "" for an empty file)',
            }
        end
        if #args.content > MAX_BYTES then
            return {
                ok = false,
                error = string.format(
                    'content too large: %d bytes > %d byte cap. Split into smaller writes.',
                    #args.content, MAX_BYTES),
            }
        end

        -- ensure parent directory exists. shell-quote the path so spaces /
        -- punctuation don't break the command.
        local dir = path:match('^(.*)/[^/]+$')
        if dir and dir ~= '' then
            local quoted = dir:gsub('"', '\\"')
            os.execute('mkdir -p "' .. quoted .. '" 2>/dev/null')
        end

        local f, err = io.open(path, 'w')
        if not f then
            return { ok = false, error = err }
        end
        local ok_w, werr = pcall(function() f:write(args.content) end)
        f:close()
        if not ok_w then
            return { ok = false, error = 'write failed: ' .. tostring(werr) }
        end

        return {
            ok = true,
            content = string.format('wrote %d bytes to %s', #args.content, path),
        }
    end,
}

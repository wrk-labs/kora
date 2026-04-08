-- Read a slice of a file from disk, with line numbers and pagination.
--
-- Default: returns the first 200 lines, prefixed with 1-based line numbers
-- (`cat -n` style). Use `offset` to paginate through long files. Hard
-- limit per call: 500 lines, regardless of what the model asks for.
--
-- The defaults exist because dumping a whole file into the conversation
-- bloats context fast on small local models — once a 30 KB tool result
-- is in history, every subsequent turn pays for it forever. Slicing keeps
-- prompts manageable and teaches the model to navigate via line numbers.

local DEFAULT_LIMIT = 200
local MAX_LIMIT     = 500

return kora.register_tool {
    name = 'read',
    description = 'Read a slice of a file at an absolute path. Returns numbered lines (cat -n style). Default: first 200 lines, max 500 per call. For files longer than 200 lines, use `offset` (1-based) to paginate — read the start first, then continue with offset=201, etc. Always start with offset=1 unless you already know what part of the file you need.',
    schema = {
        file_path = 'string',
        offset = 'number?',
        limit = 'number?',
    },
    run = function(args)
        local path = args.file_path
        if not path or path == '' then
            return { ok = false, error = 'file_path required' }
        end

        local f, err = io.open(path, 'r')
        if not f then
            return { ok = false, error = err }
        end

        local offset = tonumber(args.offset) or 1
        if offset < 1 then offset = 1 end

        local requested_limit = tonumber(args.limit) or DEFAULT_LIMIT
        if requested_limit < 1 then requested_limit = DEFAULT_LIMIT end
        local limit = requested_limit
        if limit > MAX_LIMIT then limit = MAX_LIMIT end

        -- collect the requested slice + count total lines
        local lines = {}
        local total = 0
        for line in f:lines() do
            total = total + 1
            if total >= offset and #lines < limit then
                lines[#lines + 1] = line
            end
        end
        f:close()

        if total == 0 then
            return { ok = true, content = '[empty file]' }
        end

        if offset > total then
            return {
                ok = false,
                error = string.format(
                    'offset %d is past end of file (file has %d lines)',
                    offset, total),
            }
        end

        -- prefix each line with a 1-based number, padded to align
        local end_line = offset + #lines - 1
        local width = #tostring(end_line)
        local out = {}
        for i, line in ipairs(lines) do
            out[i] = string.format('%' .. width .. 'd  %s', offset + i - 1, line)
        end

        local body = table.concat(out, '\n')
        local note
        if #lines < total then
            local remaining = total - end_line
            if remaining > 0 then
                note = string.format(
                    '\n\n[showing lines %d-%d of %d. %d more lines — read with offset=%d to continue]',
                    offset, end_line, total, remaining, end_line + 1)
            else
                note = string.format(
                    '\n\n[showing lines %d-%d of %d]',
                    offset, end_line, total)
            end
        end
        if requested_limit > MAX_LIMIT then
            note = (note or '') .. string.format(
                '\n[note: limit %d clamped to %d (max per call)]',
                requested_limit, MAX_LIMIT)
        end

        return { ok = true, content = body .. (note or '') }
    end,
}

-- Exact-string find-and-replace in a file.
-- Errors if old_string is not unique (unless replace_all=true).

return kora.register_tool {
    name = 'edit',
    description = 'Replace an exact string in a file with a new string. old_string must match exactly and be unique unless replace_all is true.',
    schema = {
        file_path = 'string',
        old_string = 'string',
        new_string = 'string',
        replace_all = 'boolean?',
    },
    dangerous = true,
    run = function(args)
        local path = args.file_path
        if not path or path == '' then
            return { ok = false, error = 'file_path required' }
        end
        if args.old_string == nil or args.new_string == nil then
            return { ok = false, error = 'old_string and new_string required' }
        end
        -- empty old_string with replace_all is "create from nothing" — that's
        -- not what edit is for. Use the `write` tool to create files.
        if args.old_string == '' then
            return {
                ok = false,
                error = 'old_string is empty. The `edit` tool cannot create '
                    .. 'content from nothing — use the `write` tool to create '
                    .. 'or overwrite a file. For an existing file, pass the '
                    .. 'exact text you want to replace.',
            }
        end

        local f, err = io.open(path, 'r')
        if not f then
            return {
                ok = false,
                error = path .. ' does not exist. Use `glob` to find the '
                    .. 'correct path, or `write` to create a new file. Do NOT '
                    .. 'retry this same path.',
            }
        end
        local content = f:read('*a')
        f:close()

        -- literal find/replace (no patterns)
        local function literal_find(s, pat, init)
            return string.find(s, pat, init, true)
        end

        -- count occurrences
        local count = 0
        local pos = 1
        while true do
            local a, b = literal_find(content, args.old_string, pos)
            if not a then break end
            count = count + 1
            pos = b + 1
        end

        if count == 0 then
            return { ok = false, error = 'old_string not found in ' .. path }
        end
        if count > 1 and not args.replace_all then
            return { ok = false, error = 'old_string is not unique in ' .. path .. ' (' .. count .. ' matches); pass replace_all=true or provide more context' }
        end

        -- do the replacement (literal, no pattern escapes needed thanks to gsub being escape-aware? no — must escape)
        local function escape(s) return (s:gsub('[%(%)%.%%%+%-%*%?%[%]%^%$]', '%%%0')) end
        -- parens around the inner gsub discard its second return value
        -- (the replacement count); otherwise it leaks into the outer gsub
        -- as the n-limit argument and silently zeroes out the replacement.
        local repl = (args.new_string:gsub('%%', '%%%%'))
        local new_content
        if args.replace_all then
            new_content = content:gsub(escape(args.old_string), repl)
        else
            new_content = content:gsub(escape(args.old_string), repl, 1)
        end

        local wf, werr = io.open(path, 'w')
        if not wf then return { ok = false, error = werr } end
        wf:write(new_content)
        wf:close()

        return { ok = true, content = 'edited ' .. path .. ' (' .. count .. ' replacement' .. (count == 1 and '' or 's') .. ')' }
    end,
}

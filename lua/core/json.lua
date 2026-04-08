-- minimal JSON encoder/decoder for kora agent tool calls.
-- handles: object, array, string, number, bool, null.
-- not a general-purpose lib; tuned for what tool calls need.

local json = {}

-- ----- encoder -----

local encode

local escape_map = {
    ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b',
    ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
}

local function escape_char(c)
    return escape_map[c] or string.format('\\u%04x', c:byte())
end

local function encode_string(s)
    return '"' .. s:gsub('[%z\1-\31\\"]', escape_char) .. '"'
end

local function is_array(t)
    local n = 0
    for k in pairs(t) do
        if type(k) ~= 'number' then return false end
        n = n + 1
    end
    for i = 1, n do
        if t[i] == nil then return false end
    end
    return true, n
end

local function encode_table(t)
    local arr, n = is_array(t)
    if arr then
        local parts = {}
        for i = 1, n do parts[i] = encode(t[i]) end
        return '[' .. table.concat(parts, ',') .. ']'
    else
        local parts = {}
        for k, v in pairs(t) do
            parts[#parts + 1] = encode_string(tostring(k)) .. ':' .. encode(v)
        end
        return '{' .. table.concat(parts, ',') .. '}'
    end
end

encode = function(v)
    local t = type(v)
    if v == nil or v == json.null then return 'null'
    elseif t == 'string' then return encode_string(v)
    elseif t == 'number' then
        if v ~= v or v == math.huge or v == -math.huge then return 'null' end
        return tostring(v)
    elseif t == 'boolean' then return v and 'true' or 'false'
    elseif t == 'table' then return encode_table(v)
    else return 'null' end
end

json.encode = encode
json.null = setmetatable({}, { __tostring = function() return 'null' end })

-- ----- decoder -----

local decode_value

local function skip_ws(s, i)
    while i <= #s do
        local c = s:sub(i, i)
        if c ~= ' ' and c ~= '\t' and c ~= '\n' and c ~= '\r' then break end
        i = i + 1
    end
    return i
end

local function decode_error(s, i, msg)
    error('json: ' .. msg .. ' at position ' .. i)
end

local function decode_string(s, i)
    if s:sub(i, i) ~= '"' then decode_error(s, i, 'expected string') end
    i = i + 1
    local out = {}
    while i <= #s do
        local c = s:sub(i, i)
        if c == '"' then return table.concat(out), i + 1 end
        if c == '\\' then
            local n = s:sub(i + 1, i + 1)
            if n == '"' then out[#out+1] = '"'
            elseif n == '\\' then out[#out+1] = '\\'
            elseif n == '/' then out[#out+1] = '/'
            elseif n == 'b' then out[#out+1] = '\b'
            elseif n == 'f' then out[#out+1] = '\f'
            elseif n == 'n' then out[#out+1] = '\n'
            elseif n == 'r' then out[#out+1] = '\r'
            elseif n == 't' then out[#out+1] = '\t'
            elseif n == 'u' then
                local hex = s:sub(i + 2, i + 5)
                local cp = tonumber(hex, 16)
                if cp and cp < 0x80 then
                    out[#out+1] = string.char(cp)
                elseif cp and cp < 0x800 then
                    out[#out+1] = string.char(0xc0 + math.floor(cp / 0x40), 0x80 + (cp % 0x40))
                elseif cp then
                    out[#out+1] = string.char(
                        0xe0 + math.floor(cp / 0x1000),
                        0x80 + math.floor(cp / 0x40) % 0x40,
                        0x80 + (cp % 0x40))
                end
                i = i + 4
            else decode_error(s, i, 'bad escape') end
            i = i + 2
        else
            out[#out+1] = c
            i = i + 1
        end
    end
    decode_error(s, i, 'unterminated string')
end

local function decode_number(s, i)
    local j = i
    if s:sub(j, j) == '-' then j = j + 1 end
    while j <= #s do
        local c = s:sub(j, j)
        if not c:match('[%d%.eE%+%-]') then break end
        j = j + 1
    end
    local n = tonumber(s:sub(i, j - 1))
    if not n then decode_error(s, i, 'bad number') end
    return n, j
end

local function decode_array(s, i)
    i = i + 1
    local out = {}
    i = skip_ws(s, i)
    if s:sub(i, i) == ']' then return out, i + 1 end
    while true do
        local v
        v, i = decode_value(s, i)
        out[#out + 1] = v
        i = skip_ws(s, i)
        local c = s:sub(i, i)
        if c == ',' then i = skip_ws(s, i + 1)
        elseif c == ']' then return out, i + 1
        else decode_error(s, i, 'expected , or ]') end
    end
end

local function decode_object(s, i)
    i = i + 1
    local out = {}
    i = skip_ws(s, i)
    if s:sub(i, i) == '}' then return out, i + 1 end
    while true do
        i = skip_ws(s, i)
        local k
        k, i = decode_string(s, i)
        i = skip_ws(s, i)
        if s:sub(i, i) ~= ':' then decode_error(s, i, 'expected :') end
        i = skip_ws(s, i + 1)
        local v
        v, i = decode_value(s, i)
        out[k] = v
        i = skip_ws(s, i)
        local c = s:sub(i, i)
        if c == ',' then i = i + 1
        elseif c == '}' then return out, i + 1
        else decode_error(s, i, 'expected , or }') end
    end
end

decode_value = function(s, i)
    i = skip_ws(s, i)
    local c = s:sub(i, i)
    if c == '{' then return decode_object(s, i)
    elseif c == '[' then return decode_array(s, i)
    elseif c == '"' then return decode_string(s, i)
    elseif c == 't' and s:sub(i, i + 3) == 'true' then return true, i + 4
    elseif c == 'f' and s:sub(i, i + 4) == 'false' then return false, i + 5
    elseif c == 'n' and s:sub(i, i + 3) == 'null' then return json.null, i + 4
    elseif c == '-' or c:match('%d') then return decode_number(s, i)
    else decode_error(s, i, 'unexpected character ' .. c) end
end

function json.decode(s)
    local ok, result = pcall(function()
        local v, _ = decode_value(s, 1)
        return v
    end)
    if not ok then return nil, result end
    return result
end

return json

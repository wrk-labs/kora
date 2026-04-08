-- Project indexer — ported from kai/config/nvim/lua/kai/ai.lua.
--
-- Walks up to find a real project root, enumerates files via `rg --files`
-- (or `find` as a fallback), detects the dominant language by file extension,
-- grabs a README excerpt, and extracts top-level symbols by per-language
-- regex. Result is cached on the module table and only re-scanned when the
-- working directory changes (or `M.invalidate()` is called).
--
-- This module replaces the build-the-env-block-from-scratch-every-prompt
-- approach with: scan once at startup, reuse for the rest of the session.

local M = {}

-- ----- static config -----

local ROOT_MARKERS = {
    '.git', 'Makefile', 'CMakeLists.txt', 'package.json',
    'Cargo.toml', 'go.mod', 'pyproject.toml', 'setup.py',
}

local README_NAMES = { 'README.md', 'README', 'README.txt', 'readme.md' }

local EXT_LANG = {
    c = 'c', h = 'c', cpp = 'cpp', hpp = 'cpp', cc = 'cpp',
    py = 'python', lua = 'lua', sh = 'bash', bash = 'bash', zsh = 'bash',
    js = 'javascript', ts = 'typescript', jsx = 'javascript', tsx = 'typescript',
    go = 'go', rs = 'rust', rb = 'ruby', java = 'java',
    json = 'json', yaml = 'yaml', yml = 'yaml', toml = 'toml',
    md = 'markdown', html = 'html', css = 'css', sql = 'sql',
}

local SYMBOL_PATTERNS = {
    c          = {
        '^%s*[%w_*]+%s+([%w_]+)%s*%(',          -- function definition
        '^#define%s+([%w_]+)',                    -- macro
        '^typedef%s+.-%s+([%w_]+)%s*;',           -- typedef
    },
    cpp        = {
        '^%s*[%w_:*]+%s+([%w_:]+)%s*%(',
        '^class%s+([%w_]+)',
        '^struct%s+([%w_]+)',
    },
    python     = {
        '^def%s+([%w_]+)%s*%(',
        '^class%s+([%w_]+)',
    },
    lua        = {
        '^function%s+([%w_.]+)%s*%(',
        '^local%s+function%s+([%w_]+)%s*%(',
        '^local%s+([%w_]+)%s*=%s*function',
    },
    bash       = {
        '^([%w_]+)%s*%(%)%s*{',
        '^function%s+([%w_]+)',
    },
    javascript = {
        '^function%s+([%w_]+)',
        '^const%s+([%w_]+)%s*=',
        '^class%s+([%w_]+)',
        '^export%s+.-%s+([%w_]+)',
    },
    typescript = {
        '^function%s+([%w_]+)',
        '^const%s+([%w_]+)%s*=',
        '^class%s+([%w_]+)',
        '^interface%s+([%w_]+)',
        '^type%s+([%w_]+)',
    },
    go         = {
        '^func%s+([%w_]+)%(',
        '^func%s+%(.-%)%s+([%w_]+)%(',           -- methods
        '^type%s+([%w_]+)%s+struct',
        '^type%s+([%w_]+)%s+interface',
    },
    rust       = {
        '^pub%s+fn%s+([%w_]+)',
        '^fn%s+([%w_]+)',
        '^pub%s+struct%s+([%w_]+)',
        '^struct%s+([%w_]+)',
    },
}

-- ----- cache -----

M.cache = nil  -- last scan result; nil means "not scanned yet"

-- ----- helpers -----

local function shell(cmd, timeout_ms)
    -- routes through the C-side shell_exec (which enforces a timeout)
    if kora and kora.shell_exec then
        return kora.shell_exec(cmd, timeout_ms or 5000)
    end
    -- fallback for tests: io.popen (no timeout)
    local p = io.popen(cmd)
    if not p then return '', -1 end
    local out = p:read('*a') or ''
    p:close()
    return out, 0
end

local function file_exists(path)
    local f = io.open(path, 'r')
    if not f then return false end
    f:close()
    return true
end

local function dir_exists(path)
    -- shell out; lfs would be cleaner but we don't depend on it
    local out = shell(string.format('test -d %q && echo yes', path), 1000)
    return out:match('yes') ~= nil
end

-- read up to `max_lines` lines from a file. returns array.
local function read_lines(path, max_lines)
    local f = io.open(path, 'r')
    if not f then return {} end
    local lines, n = {}, 0
    for line in f:lines() do
        n = n + 1
        if n > (max_lines or math.huge) then break end
        lines[n] = line
    end
    f:close()
    return lines
end

-- ----- project root walk -----

local function find_project_root(start)
    local dir = start
    for _ = 1, 20 do
        for _, marker in ipairs(ROOT_MARKERS) do
            local path = dir .. '/' .. marker
            if file_exists(path) or dir_exists(path) then
                return dir
            end
        end
        local parent = dir:match('(.+)/[^/]+$')
        if not parent or parent == dir then break end
        dir = parent
    end
    return start
end

-- ----- file enumeration -----

local function enumerate_files(root)
    -- prefer rg if installed (respects .gitignore + handles excludes)
    local has_rg = shell('command -v rg >/dev/null 2>&1 && echo yes', 1000):match('yes')
    local cmd
    if has_rg then
        cmd = string.format(
            'cd %q && rg --files --no-ignore-vcs '
            .. '-g "!.git" -g "!node_modules" -g "!vendor" '
            .. '-g "!build" -g "!*.o" -g "!*.so" -g "!*.a" '
            .. '-g "!*.gguf" 2>/dev/null',
            root)
    else
        cmd = string.format(
            'cd %q && find . -type f '
            .. '-not -path "*/.git/*" '
            .. '-not -path "*/node_modules/*" '
            .. '-not -path "*/vendor/*" '
            .. '-not -path "*/build/*" '
            .. '-not -name "*.o" '
            .. '-not -name "*.so" '
            .. '-not -name "*.a" '
            .. '-not -name "*.gguf" 2>/dev/null',
            root)
    end

    local out = shell(cmd, 10000)
    local files = {}
    for raw in out:gmatch('[^\n]+') do
        local path = raw
        if path ~= '' then
            -- normalize: strip leading "./"
            if path:sub(1, 2) == './' then path = path:sub(3) end
            local ext = path:match('%.([%w]+)$')
            local lang = ext and EXT_LANG[ext:lower()]
            if lang then
                files[#files + 1] = { path = path, lang = lang }
            end
        end
    end
    return files
end

-- ----- dominant language -----

local function dominant_language(files)
    local count = {}
    for _, f in ipairs(files) do
        count[f.lang] = (count[f.lang] or 0) + 1
    end
    local best, best_count = nil, 0
    for lang, n in pairs(count) do
        if n > best_count then best, best_count = lang, n end
    end
    return best, count
end

-- ----- README excerpt -----

local function read_readme(root)
    for _, name in ipairs(README_NAMES) do
        local path = root .. '/' .. name
        local lines = read_lines(path, 30)
        if #lines > 0 then
            return table.concat(lines, '\n'), name
        end
    end
    return nil, nil
end

-- ----- symbol extraction -----

local function extract_symbols(root, files, max_files, max_lines_per_file)
    max_files = max_files or 40
    max_lines_per_file = max_lines_per_file or 100
    local symbols = {}
    local sampled = 0
    for _, f in ipairs(files) do
        if sampled >= max_files then break end
        local patterns = SYMBOL_PATTERNS[f.lang]
        if patterns then
            local lines = read_lines(root .. '/' .. f.path, max_lines_per_file)
            for _, line in ipairs(lines) do
                for _, pat in ipairs(patterns) do
                    local sym = line:match(pat)
                    if sym and #sym > 1 and #sym < 80 then
                        symbols[#symbols + 1] = {
                            name = sym,
                            file = f.path,
                            lang = f.lang,
                            line = line:gsub('^%s+', ''):sub(1, 120),
                        }
                    end
                end
            end
            sampled = sampled + 1
        end
    end
    return symbols
end

-- ----- top-level scan entry point -----

function M.scan(cwd, force)
    if not force and M.cache and M.cache.cwd == cwd then
        return M.cache
    end

    local root = find_project_root(cwd)
    local files = enumerate_files(root)
    local lang, lang_count = dominant_language(files)
    local readme, readme_name = read_readme(root)
    local symbols = extract_symbols(root, files)

    M.cache = {
        cwd = cwd,
        root = root,
        files = files,
        lang = lang,
        lang_count = lang_count,
        readme = readme,
        readme_name = readme_name,
        symbols = symbols,
    }
    return M.cache
end

function M.invalidate()
    M.cache = nil
end

-- ----- formatting -----

-- produce the markdown block injected into the agent system prompt.
function M.format_block(p)
    if not p then return '' end
    local parts = { '\n\n## Project\n\n' }

    parts[#parts + 1] = string.format('Root: %s\n', p.root)
    if p.lang then
        parts[#parts + 1] = string.format('Language: %s (%d files)\n',
            p.lang, p.lang_count[p.lang] or 0)
    end

    if p.readme then
        local excerpt = p.readme:sub(1, 600)
        parts[#parts + 1] = '\n### ' .. (p.readme_name or 'README') .. ' (excerpt)\n'
        parts[#parts + 1] = excerpt
        if #p.readme > 600 then parts[#parts + 1] = '\n...' end
        parts[#parts + 1] = '\n'
    end

    if #p.symbols > 0 then
        parts[#parts + 1] = '\n### Top-level definitions (sample)\n'
        local n = math.min(30, #p.symbols)
        for i = 1, n do
            local s = p.symbols[i]
            parts[#parts + 1] = string.format('- %s — %s\n', s.file, s.line)
        end
        if #p.symbols > n then
            parts[#parts + 1] = string.format('... (%d more)\n', #p.symbols - n)
        end
    end

    if #p.files > 0 then
        parts[#parts + 1] = '\n### Files (' .. #p.files .. ' total)\n'
        local shown = math.min(40, #p.files)
        for i = 1, shown do
            parts[#parts + 1] = '- ' .. p.files[i].path .. '\n'
        end
        if #p.files > shown then
            parts[#parts + 1] = string.format('... (%d more files)\n', #p.files - shown)
        end
    end

    parts[#parts + 1] = '\nUse the paths above. Do NOT invent paths that are not listed here. If a file is not in this list, it does NOT exist in this project.\n'

    return table.concat(parts)
end

return M

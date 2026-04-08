-- Smoke test for the project indexer. Runs against the kora repo itself.
-- We can't fully exercise the cache invalidation here without spinning up
-- multiple cwds, but we cover: scan succeeds, root walk works, language
-- detection finds C, README is captured, symbols are extracted, format
-- block has the expected sections.

_G.kora = _G.kora or {}
function kora.shell_exec(cmd, _)
    -- bypass to real popen for the index test
    local p = io.popen(cmd)
    if not p then return '', -1 end
    local out = p:read('*a') or ''
    p:close()
    return out, 0
end

local index = require('core.index')

-- locate the kora repo root from this test file
local script = arg[0] or 'tests/lua/run.lua'
local root = script:match('(.*)/tests/lua/[^/]+%.lua$') or '.'

T.case('scan returns a populated project table', function()
    index.invalidate()
    local p = index.scan(root)
    T.truthy(p, 'project table should not be nil')
    T.truthy(p.root, 'root should be set')
    T.truthy(#p.files > 10, 'expected to find more than 10 files, got ' .. #p.files)
end)

T.case('finds the project root via .git or Makefile', function()
    local p = index.scan(root)
    -- root should be the kora repo (which has .git, Makefile, README.md)
    T.truthy(p.root:match('kora$') or p.root == root,
        'root should end in kora, got ' .. tostring(p.root))
end)

T.case('detects C as the dominant language', function()
    local p = index.scan(root)
    T.eq(p.lang, 'c', 'expected dominant language to be c')
end)

T.case('captures README excerpt', function()
    local p = index.scan(root)
    T.truthy(p.readme, 'README excerpt should be set')
    T.matches(p.readme, 'Kora', 'README should mention Kora')
end)

T.case('extracts symbols from C source', function()
    local p = index.scan(root)
    T.truthy(#p.symbols > 0, 'should extract at least one symbol')
    -- look for a known kora function
    local found = false
    for _, s in ipairs(p.symbols) do
        if s.name == 'kora_loop_run' or s.name == 'kora_parser_init' then
            found = true
            break
        end
    end
    T.truthy(found, 'should find kora_loop_run or kora_parser_init in symbols')
end)

T.case('cache reuses on second call with same cwd', function()
    local p1 = index.scan(root)
    local p2 = index.scan(root)
    T.eq(p1, p2, 'cached scan should return the same table reference')
end)

T.case('invalidate forces re-scan', function()
    local p1 = index.scan(root)
    index.invalidate()
    local p2 = index.scan(root)
    T.truthy(p1 ~= p2, 'after invalidate, scan should produce a new table')
end)

T.case('format_block has expected sections', function()
    local p = index.scan(root)
    local block = index.format_block(p)
    T.matches(block, '## Project')
    T.matches(block, 'Root:')
    T.matches(block, 'Language: c')
    T.matches(block, '### README')
    T.matches(block, '### Top%-level definitions')
    T.matches(block, '### Files')
end)

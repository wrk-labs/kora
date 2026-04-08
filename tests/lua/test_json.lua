local json = require('core.json')

T.case('encode primitives', function()
    T.eq(json.encode(nil), 'null')
    T.eq(json.encode(true), 'true')
    T.eq(json.encode(false), 'false')
    T.eq(json.encode(0), '0')
    T.eq(json.encode(42), '42')
    T.eq(json.encode(-1.5), '-1.5')
    T.eq(json.encode(''), '""')
    T.eq(json.encode('hi'), '"hi"')
end)

T.case('encode escapes', function()
    T.eq(json.encode('a"b'), '"a\\"b"')
    T.eq(json.encode('a\\b'), '"a\\\\b"')
    T.eq(json.encode('a\nb'), '"a\\nb"')
    T.eq(json.encode('a\tb'), '"a\\tb"')
end)

T.case('encode array', function()
    T.eq(json.encode({1, 2, 3}), '[1,2,3]')
    T.eq(json.encode({}), '[]')
    T.eq(json.encode({'a', 'b'}), '["a","b"]')
end)

T.case('encode object', function()
    local s = json.encode({name = 'read'})
    T.truthy(s == '{"name":"read"}', 'got: ' .. s)
end)

T.case('decode primitives', function()
    T.eq(json.decode('true'), true)
    T.eq(json.decode('false'), false)
    T.eq(json.decode('42'), 42)
    T.eq(json.decode('-1.5'), -1.5)
    T.eq(json.decode('"hello"'), 'hello')
end)

T.case('decode escapes', function()
    T.eq(json.decode('"a\\"b"'), 'a"b')
    T.eq(json.decode('"a\\nb"'), 'a\nb')
    T.eq(json.decode('"a\\\\b"'), 'a\\b')
    T.eq(json.decode('"\\t"'), '\t')
end)

T.case('decode object', function()
    local v = json.decode('{"name":"read","args":{"file_path":"/tmp/x"}}')
    T.eq(v.name, 'read')
    T.eq(v.args.file_path, '/tmp/x')
end)

T.case('decode array', function()
    local v = json.decode('[1, 2, "three", true, null]')
    T.eq(v[1], 1)
    T.eq(v[2], 2)
    T.eq(v[3], 'three')
    T.eq(v[4], true)
    T.eq(v[5], json.null)
end)

T.case('decode nested', function()
    local v = json.decode('{"a":[{"b":1},{"b":2}]}')
    T.eq(v.a[1].b, 1)
    T.eq(v.a[2].b, 2)
end)

T.case('decode handles whitespace', function()
    local v = json.decode('  {  "k"  :  42  }  ')
    T.eq(v.k, 42)
end)

T.case('decode bad json returns nil', function()
    local v, err = json.decode('{not valid')
    T.eq(v, nil)
    T.truthy(err)
end)

T.case('round trip a tool call', function()
    local original = {
        name = 'edit',
        arguments = {
            file_path = '/tmp/foo.txt',
            old_string = 'hello',
            new_string = 'world',
            replace_all = true,
        },
    }
    local v = json.decode(json.encode(original))
    T.eq(v.name, 'edit')
    T.eq(v.arguments.file_path, '/tmp/foo.txt')
    T.eq(v.arguments.replace_all, true)
end)

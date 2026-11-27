# release builds only: lifts every `static const char *X = GLSL(...);` out of the given sources into one lzma blob.
# comments go, whitespace collapses, and the pointers are filled by a constructor. usage: shaders.py outdir src.c...
import lzma, os, re, sys

out, files = sys.argv[1], sys.argv[2:]
texts = []
for path in files:
    s = open(path).read()
    names = []
    while True:
        m = re.search(r'static const char \*(\w+) = GLSL\(', s)
        if not m:
            break
        i, depth = m.end(), 1
        while depth:
            depth += {'(': 1, ')': -1}.get(s[i], 0)
            i += 1
        body = s[m.end():i - 1]
        body = re.sub(r'/\*.*?\*/', ' ', re.sub(r'//.*', ' ', body), flags=re.S)
        assert '#' not in body, (path, m.group(1))
        names.append((m.group(1), len(texts)))
        texts.append(' '.join(body.split()))
        s = s[:m.start()] + f'static const char *{m.group(1)};' + s[i + 1:]
    ctor = ''.join(f'    {n} = shader_get({k});\n' for n, k in names)
    s += f'\nconst char *shader_get(int i);\n__attribute__((constructor)) static void shaders_init_{os.path.basename(path)[:-2]}(void)\n{{\n{ctor}}}\n'
    open(f'{out}/{os.path.basename(path)}', 'w').write(f'#line 1 "{path}"\n' + s)

raw = b'\0'.join(t.encode() for t in texts) + b'\0'
f = [{'id': lzma.FILTER_LZMA1, 'preset': 9 | lzma.PRESET_EXTREME, 'lc': 3, 'lp': 0, 'pb': 0, 'dict_size': 1 << 20}]
z = bytes([3]) + lzma.compress(raw, format=lzma.FORMAT_RAW, filters=f)
body = ',\n'.join(','.join(str(b) for b in z[i:i + 32]) for i in range(0, len(z), 32))
open(f'{out}/shaders.c', 'w').write(f'''#include <stdlib.h>
void lzma_unpack(const unsigned char *in, unsigned char *out, int n);
static const unsigned char z[] = {{
{body}
}};

const char *shader_get(int i)
{{
    static char *text;
    if (!text) {{
        text = malloc({len(raw)});
        lzma_unpack(z, (unsigned char *)text, {len(raw)});
    }}
    char *p = text;
    while (i--) while (*p++) ;
    return p;
}}
''')
print(f'shaders: {len(texts)} programs, {sum(len(t) for t in texts)} chars -> {len(raw)} -> {len(z)} bytes')

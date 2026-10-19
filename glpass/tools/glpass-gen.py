#!/usr/bin/env python3
"""Generates the GL pass-through glue from glpass/guest/gl_api.txt.

Outputs (committed; re-run after changing this script or the API list):
  glpass/guest/glpass_fn.h        command numbers shared by guest and host
  glpass/guest/glpass_gen.c       guest entry points (encoders and stubs)
  glpass/host/GlDispatchGen.inc   host replay of the generated commands

Each IRIX entry point is one of:
  auto    plain-value arguments, or constant arrays whose length follows from
          the function (glVertex3fv: 3, glLoadMatrixf: 16, glLightfv: by pname);
          encoded and replayed by generated code
  manual  hand-written in glpass.c (results, images, vertex arrays, ...) and,
          when it reaches the host, in GlBridgeMac.cpp
  guest   hand-written in glpass.c, which tracks state with it, and sent as
          an auto command: replayed by generated host code
  stub    not available on the host: logs its first use, does nothing
Only functions the host OpenGL declares (macOS SDK headers) are forwarded.

  glpass/tools/glpass-gen.py [path to the host OpenGL headers]
"""
import os
import re
import subprocess
import sys

GLPASS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # glpass/
API = os.path.join(GLPASS, 'guest', 'gl_api.txt')

# Hand-written on the guest side (glpass.c).
MANUAL = {
    # results
    'glGetError', 'glGetString', 'glGetBooleanv', 'glGetIntegerv', 'glGetFloatv', 'glGetDoublev',
    'glIsEnabled', 'glIsList', 'glIsTexture', 'glGenLists', 'glGenTextures', 'glRenderMode',
    'glReadPixels', 'glGetLightfv', 'glGetLightiv', 'glGetMaterialfv', 'glGetMaterialiv',
    'glGetTexParameterfv', 'glGetTexParameteriv', 'glGetTexLevelParameterfv', 'glGetTexLevelParameteriv',
    'glGetTexEnvfv', 'glGetTexEnviv', 'glGetClipPlane', 'glGetPointerv', 'glAreTexturesResident',
    'glFinish', 'glFlush', 'glSelectBuffer', 'glFeedbackBuffer',
    # data
    'glTexImage1D', 'glTexImage2D', 'glTexSubImage1D', 'glTexSubImage2D', 'glDrawPixels', 'glBitmap',
    'glPolygonStipple', 'glCallLists', 'glDeleteTextures', 'glPrioritizeTextures', 'glPixelStorei',
    'glPixelStoref',
    # client-side vertex arrays
    'glVertexPointer', 'glNormalPointer', 'glColorPointer', 'glTexCoordPointer', 'glIndexPointer',
    'glEdgeFlagPointer', 'glInterleavedArrays', 'glEnableClientState', 'glDisableClientState',
    'glDrawArrays', 'glDrawElements', 'glArrayElement', 'glPushClientAttrib', 'glPopClientAttrib',
    'glVertexPointerEXT', 'glNormalPointerEXT', 'glColorPointerEXT', 'glTexCoordPointerEXT',
    'glDrawArraysEXT', 'glArrayElementEXT', 'glIndexPointerEXT', 'glEdgeFlagPointerEXT', 'glGetPointervEXT',
    # state the guest tracks, or the host translates
    'glDrawBuffer', 'glReadBuffer', 'glPolygonOffsetEXT', 'glGetTexGenfv', 'glGetTexGeniv', 'glGetTexGendv',
    'glGetPolygonStipple', 'glGetTexImage', 'glIsTextureEXT', 'glGenTexturesEXT', 'glDeleteTexturesEXT',
    'glAreTexturesResidentEXT', 'glPrioritizeTexturesEXT', 'glTexSubImage1DEXT', 'glTexSubImage2DEXT',
    'glMap1f', 'glMap1d', 'glMap2f', 'glMap2d', 'glPixelMapfv', 'glPixelMapuiv', 'glPixelMapusv',
    'glDrawRangeElements',  # (glpass.c also exports glDrawRangeElementsEXT, which IRIX does not declare)
    # 3D textures: images sized with the 3D pixel store state
    'glTexImage3D', 'glTexImage3DEXT', 'glTexSubImage3D', 'glTexSubImage3DEXT',
    # colour index mode, emulated with the window's colormap
    'glIndexd', 'glIndexdv', 'glIndexf', 'glIndexfv', 'glIndexi', 'glIndexiv', 'glIndexs', 'glIndexsv',
    'glIndexub', 'glIndexubv', 'glClearIndex',
    # glArrayElement runs between them are batched into one indexed draw
    'glBegin', 'glEnd',
    # colour index state on an RGBA host buffer
    'glEnable', 'glDisable', 'glIndexMask',
}

# Hand-written on the guest side (they track state there) but sent as plain
# commands: the host replays them with generated code.
GUEST_MANUAL = {
    # colour index writes
    'glLogicOp', 'glClear', 'glPushAttrib', 'glPopAttrib',
    # display lists: their colour index state changes are tracked
    'glNewList', 'glEndList', 'glCallList', 'glListBase', 'glDeleteLists',
    # replays of a colour index draw pass the depth test again (see index passes)
    'glDepthFunc',
    # texture coordinate arrays are per texture unit
    'glClientActiveTexture', 'glClientActiveTextureARB',
}

# Vendor suffixes: glFooEXT is replayed as the host's glFoo when the
# signatures match (glBindTextureEXT -> glBindTexture).
SUFFIX = re.compile(r'^(gl\w+?)(EXT|ARB|SGI|SGIS|SGIX)$')

INT_TYPES = {
    'GLenum': 'u', 'GLboolean': 'u', 'GLbitfield': 'u', 'GLuint': 'u', 'GLubyte': 'u', 'GLushort': 'u',
    'GLbyte': 'i', 'GLshort': 'i', 'GLint': 'i', 'GLsizei': 'i',
}
FLOAT_TYPES = {'GLfloat', 'GLclampf'}
DOUBLE_TYPES = {'GLdouble', 'GLclampd'}
SCALAR_TYPES = set(INT_TYPES) | FLOAT_TYPES | DOUBLE_TYPES

PNAME_GROUPS = {
    'glLightfv': 'LIGHT', 'glLightiv': 'LIGHT', 'glLightModelfv': 'LIGHTMODEL', 'glLightModeliv': 'LIGHTMODEL',
    'glMaterialfv': 'MATERIAL', 'glMaterialiv': 'MATERIAL', 'glFogfv': 'FOG', 'glFogiv': 'FOG',
    'glTexParameterfv': 'TEXPARAM', 'glTexParameteriv': 'TEXPARAM', 'glTexEnvfv': 'TEXENV',
    'glTexEnviv': 'TEXENV', 'glTexGenfv': 'TEXGEN', 'glTexGeniv': 'TEXGEN', 'glTexGendv': 'TEXGEN',
    'glPointParameterfv': 'POINTPARAM', 'glPointParameteriv': 'POINTPARAM',
    'glPointParameterfvARB': 'POINTPARAM', 'glPointParameterfvEXT': 'POINTPARAM',
    'glPointParameterfvSGIS': 'POINTPARAM',
}
VECTOR = re.compile(r'^gl(Vertex|Normal|Color|TexCoord|RasterPos|EvalCoord|SecondaryColor|WindowPos|MultiTexCoord)'
                    r'([1-4])(b|s|i|f|d|ub|us|ui)v(EXT|ARB)?$')


def parse_params(text):
    if text.strip() in ('', 'void'):
        return []
    params = []
    for part in text.split(','):
        part = part.strip()
        pointer = '*' in part
        words = part.replace('*', ' * ').split()
        name = words[-1]
        base = [w for w in words[:-1] if w not in ('const', '*')]
        params.append({'type': ' '.join(base), 'name': name, 'pointer': pointer, 'const': 'const' in words,
                       'decl': part})
    return params


def array_counts(name, params):
    """Element count of each pointer parameter, or None if not generated."""
    pointers = [p for p in params if p['pointer']]
    if not pointers:
        return []
    if not all(p['const'] and p['type'] in SCALAR_TYPES for p in pointers):
        return None
    if name in PNAME_GROUPS:
        return ['pname']
    m = VECTOR.match(name)
    if m and len(pointers) == 1:
        return [int(m.group(2))]
    if re.match(r'^glIndex(s|i|f|d|ub)v$', name) or name == 'glEdgeFlagv':
        return [1]
    if re.match(r'^gl(Load|Mult)(Transpose)?Matrix[fd](ARB)?$', name):
        return [16]
    if re.match(r'^glFogCoord[fd]v(EXT)?$', name):
        return [1]
    if name == 'glClipPlane':
        return [4]
    if re.match(r'^glRect[sifd]v$', name):
        return [2, 2]
    return None


def host_functions(header_dir, candidates):
    """The candidates the host OpenGL headers declare, as the compiler sees them."""
    import tempfile
    framework = os.path.dirname(os.path.dirname(header_dir))  # .../Frameworks
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'probe.c')
        with open(src, 'w') as f:
            f.write('#define GL_SILENCE_DEPRECATION 1\n#include <OpenGL/gl.h>\n#include <OpenGL/glext.h>\n')
            for i, name in enumerate(sorted(candidates)):
                f.write(f'void *probe{i} = (void *)&{name};\n')
        out = subprocess.run(['cc', '-fsyntax-only', '-ferror-limit=0', '-F', framework, src],
                             capture_output=True, text=True).stderr
    missing = set(re.findall(r"undeclared identifier '(gl\w+)'", out))
    return set(candidates) - missing


def words_of(t):
    return 2 if t in DOUBLE_TYPES else 1


def guest_store(t, expr, at):
    if t in FLOAT_TYPES:
        return f'p[{at}] = glp_fbits({expr});'
    if t in DOUBLE_TYPES:
        return f'glp_dbits(&p[{at}], {expr});'
    if INT_TYPES[t] == 'i':
        return f'p[{at}] = (unsigned int)(int)({expr});'
    return f'p[{at}] = (unsigned int)({expr});'


def host_load(t, at):
    if t in FLOAT_TYPES:
        return f'static_cast<{t}>(w.f({at}))'
    if t in DOUBLE_TYPES:
        return f'static_cast<{t}>(w.d({at}))'
    return f'static_cast<{t}>(w.{INT_TYPES[t]}({at}))'


# Texture images: the host stores low-precision sized formats at 8 bits
# (hostTextureFormat, host/TextureFormat.hpp).
TEXTURE_IMAGE = re.compile(r'^gl(Copy)?TexImage[123]D')


def host_param(name, p, at):
    value = host_load(p['type'], at)
    if TEXTURE_IMAGE.match(name) and p['name'] in ('internalformat', 'internalFormat'):
        return f'static_cast<{p["type"]}>(hostTextureFormat(static_cast<GLint>({value})))'
    return value


def validate(api, kinds, counts_of):
    """Stops the generation when an encoded function's arrays are not sized
    exactly: the guest would read past the program's array, or the host past
    the record."""
    errors = []
    names = [name for _, name, _ in api]
    for name in sorted({n for n in names if names.count(n) > 1}):
        errors.append(f'{name}: listed twice in gl_api.txt')
    for name in sorted((MANUAL | GUEST_MANUAL) - set(names)):
        errors.append(f'{name}: hand-written but not in gl_api.txt')
    internal = open(os.path.join(GLPASS, 'guest', 'glpass_internal.h')).read()
    groups = set(re.findall(r'GLP_GROUP_(\w+)', internal))
    for name, group in PNAME_GROUPS.items():
        if group not in groups:
            errors.append(f'{name}: pname group {group} has no GLP_GROUP_{group} in glpass_internal.h')
    for ret, name, params in api:
        if kinds[name] not in ('auto', 'guest'):
            continue
        pointers = [p for p in params if p['pointer']]
        counts = counts_of(name, params)
        for p in pointers:
            if not p['const'] or p['type'] not in SCALAR_TYPES:
                errors.append(f'{name}: {p["name"]} is not a constant array of a scalar type')
        if counts == ['pname']:
            if len(pointers) != 1 or not any(p['name'] == 'pname' and not p['pointer'] for p in params):
                errors.append(f'{name}: sized by pname, but has no pname or more than one array')
        elif len(counts) != len(pointers) or not all(isinstance(c, int) and 1 <= c <= 16 for c in counts):
            errors.append(f'{name}: array sizes {counts} for {len(pointers)} arrays')
    if errors:
        sys.exit('glpass-gen: ' + '\n  '.join(['the API list does not generate safely:'] + errors))


def main():
    header_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip(),
        'System/Library/Frameworks/OpenGL.framework/Headers')
    api = []
    for line in open(API):
        if line.startswith('#') or not line.strip():
            continue
        ret, name, params = line.rstrip('\n').split('|')
        api.append((ret, name, parse_params(params)))

    names = {name for _, name, _ in api}
    stripped = {m.group(1) for m in (SUFFIX.match(n) for n in names) if m}
    host = host_functions(header_dir, names | stripped)
    signature = {name: [(p['type'], p['pointer']) for p in params] for _, name, params in api}
    kinds = {}
    host_name = {}
    for ret, name, params in api:
        if name in MANUAL:
            kinds[name] = 'manual'
            continue
        # Prefer the core command when a vendor form has the same signature.
        target = name
        m = SUFFIX.match(name)
        if m and m.group(1) in host and signature.get(m.group(1), signature[name]) == signature[name]:
            target = m.group(1)
        counts = array_counts(name, params)
        if counts is None and target != name:
            counts = array_counts(target, params)
        scalars_ok = all(p['pointer'] or p['type'] in SCALAR_TYPES for p in params)
        if ret == 'void' and scalars_ok and counts is not None and target in host:
            kinds[name] = 'auto'
            host_name[name] = target
        else:
            kinds[name] = 'stub'
        if name in GUEST_MANUAL:
            if kinds[name] != 'auto':
                sys.exit(f'glpass-gen: {name} is in GUEST_MANUAL but cannot be generated for the host')
            kinds[name] = 'guest'

    def counts_of(name, params):
        c = array_counts(name, params)
        return c if c is not None else array_counts(host_name[name], params)

    validate(api, kinds, counts_of)
    encoded = sorted(n for n, k in kinds.items() if k in ('auto', 'manual', 'guest'))
    ids = {n: 0x100 + i for i, n in enumerate(encoded)}

    # ---- glpass_fn.h -----------------------------------------------------------
    with open(os.path.join(GLPASS, 'guest', 'glpass_fn.h'), 'w') as f:
        f.write('/* Generated by glpass/tools/glpass-gen.py: command numbers (record word 0, high half). */\n')
        import zlib
        table_id = zlib.crc32('\n'.join(f'{n}={ids[n]}' for n in encoded).encode()) or 1
        f.write('#ifndef GLPASS_FN_H\n#define GLPASS_FN_H\n\n')
        f.write('/* Identifies this command table: guest and emulator must have the same (GLP_OP_HELLO). */\n')
        f.write(f'#define GLP_FN_TABLE_ID {table_id:#010x}u\n\nenum {{\n')
        for n in encoded:
            f.write(f'    GLP_FN_{n} = {ids[n]:#x},\n')
        f.write('    GLP_FN_END\n};\n\n#endif\n')

    # ---- glpass_gen.c ----------------------------------------------------------
    with open(os.path.join(GLPASS, 'guest', 'glpass_gen.c'), 'w') as f:
        f.write('/* Generated by glpass/tools/glpass-gen.py: guest OpenGL entry points. */\n')
        f.write('#include "glpass_internal.h"\n\n')
        for ret, name, params in api:
            kind = kinds[name]
            if kind in ('manual', 'guest'):
                continue
            decl = ', '.join(p['decl'] for p in params) or 'void'
            f.write(f'{ret} {name}({decl})\n{{\n')
            if kind == 'stub':
                # Its own flag: logged once (GLPASS_TRACE), whatever the number of stubs called.
                f.write('    static int logged;\n')
                for p in params:
                    f.write(f'    (void){p["name"]};\n')
                f.write(f'    glp_unimplemented("{name}", &logged);\n')
                if ret != 'void':
                    f.write(f'    return ({ret})0;\n')
                f.write('}\n\n')
                continue
            counts = counts_of(name, params)
            scalars = [p for p in params if not p['pointer']]
            pointers = [p for p in params if p['pointer']]
            fixed = sum(words_of(p['type']) for p in scalars)
            f.write('    unsigned int *p;\n')
            if counts == ['pname']:
                arr = pointers[0]
                pname = next(p['name'] for p in scalars if p['name'] == 'pname')
                group = PNAME_GROUPS[name]
                f.write(f'    int i, n = glp_pname_count(GLP_GROUP_{group}, {pname});\n')
                f.write(f'    GLP_TRACE(GLP_FN_{name}, "{name}");\n')
                f.write(f'    if (!{arr["name"]}) return; /* GL leaves it undefined: nothing is read */\n')
                f.write(f'    p = glp_put(GLP_FN_{name}, {fixed + 1} + n * {words_of(arr["type"])});\n')
                at = 0
                for p in scalars:
                    f.write(f'    {guest_store(p["type"], p["name"], at)}\n')
                    at += words_of(p['type'])
                f.write(f'    p[{at}] = (unsigned int)n;\n')
                f.write(f'    for (i = 0; i < n; ++i) {{ {guest_store(arr["type"], arr["name"] + "[i]", f"{at + 1} + i * {words_of(arr["type"])}")} }}\n')
            else:
                total = fixed + sum(c * words_of(p['type']) for p, c in zip(pointers, counts))
                if pointers:
                    f.write('    int i;\n')
                f.write(f'    GLP_TRACE(GLP_FN_{name}, "{name}");\n')
                if pointers:
                    test = ' || '.join(f'!{p["name"]}' for p in pointers)
                    f.write(f'    if ({test}) return; /* GL leaves it undefined: nothing is read */\n')
                f.write(f'    p = glp_put(GLP_FN_{name}, {total});\n')
                at = 0
                ptr_index = 0
                for p in params:
                    if p['pointer']:
                        c = counts[ptr_index]
                        ptr_index += 1
                        ww = words_of(p['type'])
                        f.write(f'    for (i = 0; i < {c}; ++i) {{ {guest_store(p["type"], p["name"] + "[i]", f"{at} + i * {ww}")} }}\n')
                        at += c * ww
                    else:
                        f.write(f'    {guest_store(p["type"], p["name"], at)}\n')
                        at += words_of(p['type'])
                if not pointers and at == 0:
                    f.write('    (void)p;\n')
            f.write('}\n\n')

    # ---- GlDispatchGen.inc -------------------------------------------------------
    with open(os.path.join(GLPASS, 'host', 'GlDispatchGen.inc'), 'w') as f:
        f.write('// Generated by glpass/tools/glpass-gen.py: host replay of generated commands.\n')
        f.write('// Included inside a switch over the command number; `w` is the batch, `p` the payload start,\n')
        f.write('// `n` its length in words. A record shorter than its command is skipped, never read past.\n')
        for ret, name, params in api:
            if kinds[name] not in ('auto', 'guest'):
                continue
            counts = counts_of(name, params)
            pointers = [p for p in params if p['pointer']]
            target = host_name[name]
            f.write(f'case GLP_FN_{name}: {{\n')
            if counts == ['pname']:
                arr = pointers[0]
                ww = words_of(arr['type'])
                at = 0
                args = []
                for p in params:
                    if p['pointer']:
                        args.append('a')
                    else:
                        args.append(host_param(name, p, f'p + {at}'))
                        at += words_of(p['type'])
                f.write(f'    if (n < {at + 1}) break;\n')
                f.write(f'    {arr["type"]} a[16] = {{}}; // the most any pname takes is 4\n')
                f.write(f'    const size_t count = std::min<size_t>({{16, w.u(p + {at}), (n - {at + 1}) / {ww}}});\n')
                f.write(f'    for (size_t k = 0; k < count; ++k) a[k] = {host_load(arr["type"], f"p + {at + 1} + k * {ww}")};\n')
                f.write(f'    {target}({", ".join(args)});\n')
            else:
                words = sum(c * words_of(p['type']) for p, c in zip(pointers, counts)) + \
                    sum(words_of(p['type']) for p in params if not p['pointer'])
                if words:
                    f.write(f'    if (n < {words}) break;\n')
                at = 0
                args = []
                ptr_index = 0
                for p in params:
                    if p['pointer']:
                        c = counts[ptr_index]
                        var = f'a{ptr_index}'
                        ptr_index += 1
                        ww = words_of(p['type'])
                        f.write(f'    {p["type"]} {var}[{c}];\n')
                        f.write(f'    for (size_t k = 0; k < {c}; ++k) {var}[k] = {host_load(p["type"], f"p + {at} + k * {ww}")};\n')
                        args.append(var)
                        at += c * ww
                    else:
                        args.append(host_param(name, p, f'p + {at}'))
                        at += words_of(p['type'])
                f.write(f'    {target}({", ".join(args)});\n')
            f.write('    break;\n}\n')

    counts = {k: sum(1 for v in kinds.values() if v == k) for k in ('auto', 'manual', 'guest', 'stub')}
    print(f'{len(api)} entry points: {counts["auto"]} generated, {counts["manual"]} hand-written, '
          f'{counts["guest"]} hand-written in the guest only, {counts["stub"]} stubs')


if __name__ == '__main__':
    main()

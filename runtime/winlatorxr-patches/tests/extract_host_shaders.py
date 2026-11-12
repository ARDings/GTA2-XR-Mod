#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract actual generated host shader strings for an independent GLES pixel test."""
import argparse
import json
import os
from pathlib import Path
import re


def under_work_root(path):
    """Test outputs stay outside the source tree: under GTA2CEILING_WORK_ROOT when set, else on D:."""
    root = os.environ.get('GTA2CEILING_WORK_ROOT')
    if root:
        root = Path(root).resolve()
        return path == root or root in path.parents
    return path.drive.lower() == 'd:'


def strings(source):
    return ''.join(json.loads(value) for value in re.findall(r'"(?:\\.|[^"\\])*"', source))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fork', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if not under_work_root(args.output.resolve()):
        raise ValueError('Test outputs must stay under the work root (GTA2CEILING_WORK_ROOT or D:)')
    java = (args.fork/'app/src/main/java/com/winlator/cmod/renderer/material/WindowMaterial.java').read_text(encoding='utf-8')
    fragment = strings(java.split('protected String getFragmentShader()',1)[1])
    direct = (args.fork/'app/src/main/cpp/xr/direct.c').read_text(encoding='utf-8')
    alpha = strings(direct.split('#define KEY_ALPHA_GLSL',1)[1].split('static const char* KEY_VS',1)[0])
    vertex = strings(direct.split('static const char* KEY_VS =',1)[1].split('static const char* KEY_FS',1)[0])
    fs = direct.split('static const char* KEY_FS =',1)[1].split('static GLuint g_key_program',1)[0]
    before,after = fs.split('    KEY_ALPHA_GLSL',1)
    fragment_direct = strings(before) + alpha + strings(after)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text('// Actual host shaders, generated for tests only.\n' + '\n'.join(
        f'static const char {name}[] = {json.dumps(value)};'
        for name,value in [('gccWindowFragment',fragment),('gccDirectVertex',vertex),('gccDirectFragment',fragment_direct)])+'\n',encoding='utf-8')

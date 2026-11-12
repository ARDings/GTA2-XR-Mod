#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and exercise the actual native projection-pose selection expression.

Synthetic saved poses are deliberately different for both eyes and times, so a
left-eye-only or current-slot substitution fails. This is not device timewarp QA.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess


def under_work_root(path):
    """Test outputs stay outside the source tree: under GTA2CEILING_WORK_ROOT when set, else on D:."""
    root = os.environ.get('GTA2CEILING_WORK_ROOT')
    if root:
        root = Path(root).resolve()
        return path == root or root in path.parents
    return path.drive.lower() == 'd:'

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fork',type=Path)
    parser.add_argument('work',type=Path)
    parser.add_argument('--vcvars',type=Path,default=Path(r'C:\VS\VC\Auxiliary\Build\vcvars64.bat'))
    args=parser.parse_args()
    work=args.work.resolve()
    if not under_work_root(work) or any(p.is_symlink() for p in [work,*work.parents]):
        raise ValueError('Test outputs must stay on a non-symlink path under the work root (GTA2CEILING_WORK_ROOT or D:)')
    source=(args.fork/'app/src/main/cpp/xr/renderer.c').read_text(encoding='utf-8')
    statements=re.findall(r'XrPosef pose = renderer->InvertedViewPose\[[^;]+;',source)
    if len(statements)!=1:
        raise ValueError('Expected one reviewed native projection-pose selection')
    work.mkdir(parents=True,exist_ok=True)
    cpp=work/'projection_pose.cpp'
    cpp.write_text(r'''
#include <cassert>
#include <cstdio>
#include <initializer_list>
struct XrPosef { float qx,qy,qz,qw,x,y,z; };
struct Renderer { XrPosef InvertedViewPose[2][257]; };
XrPosef Select(Renderer* renderer,int eye,int frame) {
    '''+statements[0]+r'''
    return pose;
}
int main() {
 Renderer saved{};
 // A yawed head: the IPD baseline has both X and Z components.
 saved.InvertedViewPose[0][12]={0,.3826834f,0,.9238795f,.9773726f,1.6f,2.0226274f};
 saved.InvertedViewPose[1][12]={0,.3826834f,0,.9238795f,1.0226274f,1.6f,1.9773726f};
 saved.InvertedViewPose[0][24]={.1f,0,0,.9949874f,-.032f,1.7f,3};
 saved.InvertedViewPose[1][24]={.1f,0,0,.9949874f,.032f,1.7f,3};
 for(int slot:{12,24}) for(int eye:{0,1}) {
  const auto got=Select(&saved,eye,slot), expected=saved.InvertedViewPose[eye][slot];
  assert(got.x==expected.x&&got.y==expected.y&&got.z==expected.z);
  assert(got.qx==expected.qx&&got.qy==expected.qy&&got.qz==expected.qz&&got.qw==expected.qw);
 }
 assert(Select(&saved,0,12).z!=Select(&saved,1,12).z);
 std::puts("PASS: actual host projection expression selects physical eye and original sync slot");
}
''',encoding='utf-8')
    batch=work/'run.cmd'
    batch.write_text('@echo off\nset TEMP='+str(work)+'\nset TMP='+str(work)+'\ncall "'+str(args.vcvars)+'" >nul\ncd /d "'+str(work)+'"\ncl /nologo /std:c++17 /EHsc /W4 /WX /O2 /UNDEBUG projection_pose.cpp /Fe:projection_pose.exe >build.log 2>&1\nif errorlevel 1 exit /b 1\nprojection_pose.exe\nexit /b %errorlevel%\n',encoding='utf-8')
    subprocess.run(['cmd.exe','/d','/c',str(batch)],check=True)

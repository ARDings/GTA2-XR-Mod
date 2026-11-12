#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the actual generated WindowMaterial Java class against recording GL calls.

This checks state lifetime and mode isolation; GLSL pixel output is checked by
the separate offscreen GLES test on the real GPU.
"""
import argparse
import os
from pathlib import Path
import subprocess


def under_work_root(path):
    """Test outputs stay outside the source tree: under GTA2CEILING_WORK_ROOT when set, else on D:."""
    root = os.environ.get('GTA2CEILING_WORK_ROOT')
    if root:
        root = Path(root).resolve()
        return path == root or root in path.parents
    return path.drive.lower() == 'd:'


GLES = r'''package android.opengl;
public class GLES20 {
 public static final int GL_TEXTURE_2D=3553,GL_TEXTURE_MIN_FILTER=10241,GL_TEXTURE_MAG_FILTER=10240,
 GL_BLEND_SRC_RGB=32969,GL_BLEND_DST_RGB=32968,GL_BLEND_SRC_ALPHA=32971,GL_BLEND_DST_ALPHA=32970,
 GL_NEAREST=9728,GL_BLEND=3042,GL_ONE=1,GL_ONE_MINUS_SRC_ALPHA=771;
 public static int[] state={9987,9729,770,771,1,0};
 public static boolean enabled=false;
 public static int calls=0;
 public static float width,height;
 public static void glUniform1i(int a,int b) {}
 public static void glUniform1f(int a,float b) {}
 public static void glUniform2f(int a,float b,float c) {width=b;height=c;}
 public static void glGetTexParameteriv(int target,int name,int[] out,int offset) {out[offset]=state[name==GL_TEXTURE_MIN_FILTER?0:1];}
 public static void glGetIntegerv(int name,int[] out,int offset) {
  out[offset]=state[name==GL_BLEND_SRC_RGB?2:name==GL_BLEND_DST_RGB?3:name==GL_BLEND_SRC_ALPHA?4:5];
 }
 public static boolean glIsEnabled(int c) {return enabled;}
 public static void glTexParameteri(int target,int name,int value) {calls++;state[name==GL_TEXTURE_MIN_FILTER?0:1]=value;}
 public static void glBlendFuncSeparate(int a,int b,int c,int d) {calls++;state[2]=a;state[3]=b;state[4]=c;state[5]=d;}
 public static void glEnable(int c) {calls++;enabled=true;}
 public static void glDisable(int c) {calls++;enabled=false;}
}'''
BASE = r'''package com.winlator.cmod.renderer.material;
public class ShaderMaterial {
 public void setUniformNames(String... n) {}
 public void use() {}
 public int getUniformLocation(String n) {return 1;}
 protected String getVertexShader(){return "";}
 protected String getFragmentShader(){return "";}
}'''
TEST = r'''import android.opengl.GLES20;
import com.winlator.cmod.renderer.material.WindowMaterial;
import java.util.Arrays;
public class TestWindowMaterialState {
 static void check(boolean condition,String message) {if(!condition)throw new AssertionError(message);}
 public static void main(String[] args) {
  WindowMaterial m=new WindowMaterial();
  for(int mode=0;mode<5;mode++) {
   m.setColourKey(mode,.03f); int[] saved=GLES20.state.clone();GLES20.calls=0;
   m.beginExactKeySampling();m.endExactKeySampling();
   check(GLES20.calls==0&&Arrays.equals(saved,GLES20.state),"Legacy mode state changed: "+mode);
  }
  for(boolean originalEnable:new boolean[]{false,true}) {
   GLES20.enabled=originalEnable;GLES20.state=new int[]{9987,9729,770,771,1,0};
   int[] saved=GLES20.state.clone();m.setColourKey(5,.03f);
   m.setSourceSize(3072,1536);check(GLES20.width==3072&&GLES20.height==1536,"Actual drawable dimensions lost");
   m.beginExactKeySampling();
   check(GLES20.enabled&&Arrays.equals(GLES20.state,new int[]{9728,9728,1,771,1,771}),"Exact mode must use raw texels and premultiplied blending");
   m.endExactKeySampling();
   check(GLES20.enabled==originalEnable&&Arrays.equals(saved,GLES20.state),"Caller GL state was not restored");
   GLES20.calls=0;m.endExactKeySampling();check(GLES20.calls==0,"Second end must be harmless");
  }
  System.out.println("PASS: actual WindowMaterial restores filters, RGB/alpha factors, enable state; other modes untouched");
 }
}'''


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fork',type=Path)
    parser.add_argument('work',type=Path)
    parser.add_argument('--jdk',type=Path,default=Path(r'C:\Program Files\Unity\Hub\Editor\6000.3.10f1\Editor\Data\PlaybackEngines\AndroidPlayer\OpenJDK'))
    args=parser.parse_args()
    work=args.work.resolve()
    if not under_work_root(work) or any(p.is_symlink() for p in [work,*work.parents]):
        raise ValueError('Test build files must stay on a non-symlink path under the work root (GTA2CEILING_WORK_ROOT or D:)')
    inputs=[]
    for name,contents in [('android/opengl/GLES20.java',GLES),('com/winlator/cmod/renderer/material/ShaderMaterial.java',BASE),('TestWindowMaterialState.java',TEST)]:
        target=work/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_text(contents,encoding='utf-8');inputs.append(str(target))
    source=args.fork/'app/src/main/java/com/winlator/cmod/renderer/material/WindowMaterial.java'
    subprocess.run([str(args.jdk/'bin/javac.exe'),'-encoding','UTF-8','-d',str(work),*inputs,str(source)],check=True)
    subprocess.run([str(args.jdk/'bin/java.exe'),f'-Djava.io.tmpdir={work}','-cp',str(work),'TestWindowMaterialState'],check=True)

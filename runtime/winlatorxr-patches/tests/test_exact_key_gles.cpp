// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen GLES pixel test using the exact generated host shader strings.
// No OpenXR session, game process or visible surface is opened.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "gcc_host_shaders.h"

static void Require(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s (GL %x EGL %x)\n", what, glGetError(), eglGetError()); std::exit(1); }
}
static GLuint Shader(GLenum kind, const char* text) {
    GLuint result = glCreateShader(kind); glShaderSource(result, 1, &text, nullptr); glCompileShader(result);
    GLint ok = 0; glGetShaderiv(result, GL_COMPILE_STATUS, &ok);
    if (!ok) { char error[4096]{}; glGetShaderInfoLog(result, sizeof(error), nullptr, error); std::fprintf(stderr,"%s\n",error); }
    Require(ok, "actual host shader compiles"); return result;
}
static GLuint Program(const char* vertex, const char* fragment) {
    GLuint p = glCreateProgram(), v = Shader(GL_VERTEX_SHADER, vertex), f = Shader(GL_FRAGMENT_SHADER, fragment);
    glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok); Require(ok,"actual host shader links");
    glDeleteShader(v); glDeleteShader(f); return p;
}
static void Pixel(const char* label, std::array<int,4> expected) {
    std::array<unsigned char,4> pixel{}; glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
    for (int i=0;i<4;++i) if (std::abs(int(pixel[i])-expected[i])>2) {
        std::fprintf(stderr,"%s: got %u,%u,%u,%u; expected %d,%d,%d,%d\n",label,pixel[0],pixel[1],pixel[2],pixel[3],expected[0],expected[1],expected[2],expected[3]);
        std::exit(1);
    }
    Require(glGetError()==GL_NO_ERROR,label);
}
int main() {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY); Require(display != EGL_NO_DISPLAY,"EGL display");
    Require(eglInitialize(display,nullptr,nullptr),"EGL initialize");
    const EGLint configArgs[]{EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config{}; EGLint count=0; Require(eglChooseConfig(display,configArgs,&config,1,&count)&&count==1,"RGBA8 pbuffer config");
    const EGLint surfaceArgs[]{EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE}, contextArgs[]{EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    EGLSurface surface=eglCreatePbufferSurface(display,config,surfaceArgs); Require(surface!=EGL_NO_SURFACE,"Pbuffer");
    EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,contextArgs); Require(context!=EGL_NO_CONTEXT,"ES3 context");
    Require(eglMakeCurrent(display,surface,surface,context),"Make offscreen context current");
    std::printf("GPU %s; GLES %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
    glViewport(0,0,1,1); glDisable(GL_DITHER); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    GLuint texture; glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    const unsigned char mixed[]{255,0,255,255, 0,255,0,255, 0,0,255,255, 255,0,255,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,mixed);
    const char* vertex="attribute vec2 position; uniform highp vec2 sampleUV; varying highp vec2 vUV; void main(){vUV=sampleUV; gl_Position=vec4(position,0.,1.);}";
    GLuint window=Program(vertex,gccWindowFragment); glUseProgram(window);
    glUniform1i(glGetUniformLocation(window,"texture"),0); glUniform2f(glGetUniformLocation(window,"gccTextureSize"),2,2);
    glUniform1i(glGetUniformLocation(window,"keyMode"),5); glUniform1f(glGetUniformLocation(window,"keyThreshold"),0.0333333f);
    GLuint vbo; glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER,vbo);
    const float triangle[]{-1,-1,3,-1,-1,3}; glBufferData(GL_ARRAY_BUFFER,sizeof(triangle),triangle,GL_STATIC_DRAW);
    GLuint attr=GLuint(glGetAttribLocation(window,"position")); glVertexAttribPointer(attr,2,GL_FLOAT,GL_FALSE,0,nullptr); glEnableVertexAttribArray(attr);
    const auto draw=[&](float x,float y){glUniform2f(glGetUniformLocation(window,"sampleUV"),x,y);glDrawArrays(GL_TRIANGLES,0,3);};
    draw(.25f,.25f); Pixel("exact magenta contributes no RGB or alpha",{0,0,0,0});
    draw(.5f,.25f); Pixel("half green edge is premultiplied without magenta",{0,128,0,128});
    draw(.5f,.5f); Pixel("four texels interpolate keyed coverage",{0,64,64,128});
    draw(-.5f,-.5f); Pixel("outside UV clamps to original key texel",{0,0,0,0});
    glEnable(GL_BLEND); glBlendFuncSeparate(GL_ONE,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(.2f,.4f,.6f,1); glClear(GL_COLOR_BUFFER_BIT); draw(.5f,.25f);
    Pixel("premultiplied edge over room retains coverage",{26,179,77,255}); glDisable(GL_BLEND);
    const unsigned char purple[]{153,0,204,255,153,0,204,255,153,0,204,255,153,0,204,255};
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,purple); draw(.5f,.5f);
    Pixel("game purple stays opaque and unchanged",{153,0,204,255});
    const unsigned char nearKey[]{255,0,254,255,255,0,254,255,255,0,254,255,255,0,254,255};
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,nearKey); draw(.5f,.5f);
    Pixel("one-byte off key stays opaque",{255,0,254,255});
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,mixed);
    glUniform1i(glGetUniformLocation(window,"keyMode"),0);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    draw(.5f,.25f); Pixel("mode zero retains original linear sampling",{128,128,128,255});
    GLuint direct=Program(gccDirectVertex,gccDirectFragment); glUseProgram(direct);
    glUniform1i(glGetUniformLocation(direct,"src"),0); glUniform4i(glGetUniformLocation(direct,"rect"),0,0,1,1);
    glUniform2f(glGetUniformLocation(direct,"size"),1,1); glUniform1i(glGetUniformLocation(direct,"keyMode"),5);
    glDrawArrays(GL_TRIANGLES,0,3); Pixel("direct mode exact key stores transparent black",{0,0,0,0});
    glUniform1i(glGetUniformLocation(direct,"keyMode"),0); glDrawArrays(GL_TRIANGLES,0,3);
    Pixel("direct mode zero preserves magenta",{255,0,255,255});
    glDeleteProgram(window);glDeleteProgram(direct);glDeleteBuffers(1,&vbo);glDeleteTextures(1,&texture);
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);eglDestroySurface(display,surface);eglTerminate(display);
    std::puts("PASS: actual host shaders, exact key before filtering, premultiplied blend and non-key colours");return 0;
}

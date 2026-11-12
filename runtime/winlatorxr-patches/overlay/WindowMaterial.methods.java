    // SPDX-License-Identifier: GPL-3.0-or-later
    private boolean gccExactKey;
    private boolean gccSamplingActive;
    private boolean gccBlendWasEnabled;
    private final int[] gccSavedState = new int[6];

    public void setSourceSize(int width, int height) {
        GLES20.glUniform2f(getUniformLocation("gccTextureSize"), Math.max(width, 1), Math.max(height, 1));
    }

    // Called only while the window's texture is bound. Other colour-key modes
    // keep their upstream sampling and blend behaviour unchanged.
    public void beginExactKeySampling() {
        if (!gccExactKey) return;
        GLES20.glGetTexParameteriv(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, gccSavedState, 0);
        GLES20.glGetTexParameteriv(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, gccSavedState, 1);
        GLES20.glGetIntegerv(GLES20.GL_BLEND_SRC_RGB, gccSavedState, 2);
        GLES20.glGetIntegerv(GLES20.GL_BLEND_DST_RGB, gccSavedState, 3);
        GLES20.glGetIntegerv(GLES20.GL_BLEND_SRC_ALPHA, gccSavedState, 4);
        GLES20.glGetIntegerv(GLES20.GL_BLEND_DST_ALPHA, gccSavedState, 5);
        gccBlendWasEnabled = GLES20.glIsEnabled(GLES20.GL_BLEND);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_NEAREST);
        // OpenXR layers use BLEND_TEXTURE_SOURCE_ALPHA without UNPREMULTIPLIED.
        // The shader already returns premultiplied RGB and coverage alpha.
        GLES20.glEnable(GLES20.GL_BLEND);
        GLES20.glBlendFuncSeparate(GLES20.GL_ONE, GLES20.GL_ONE_MINUS_SRC_ALPHA, GLES20.GL_ONE, GLES20.GL_ONE_MINUS_SRC_ALPHA);
        gccSamplingActive = true;
    }

    public void endExactKeySampling() {
        if (!gccSamplingActive) return;
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, gccSavedState[0]);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, gccSavedState[1]);
        GLES20.glBlendFuncSeparate(gccSavedState[2], gccSavedState[3], gccSavedState[4], gccSavedState[5]);
        if (!gccBlendWasEnabled) GLES20.glDisable(GLES20.GL_BLEND);
        gccSamplingActive = false;
    }

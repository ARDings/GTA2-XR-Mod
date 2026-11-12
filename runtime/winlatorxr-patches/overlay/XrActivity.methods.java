    // SPDX-License-Identifier: GPL-3.0-or-later
    // GTA2Ceiling same-session scene access. Called from the native XR thread.
    private static final int GCC_SCENE_REQUEST = 4816;
    private volatile boolean gccSceneDenied = false;
    private volatile boolean gccSceneRequestPending = false;

    public int gccScenePermissionState() {
        if (checkSelfPermission("com.oculus.permission.USE_SCENE") == android.content.pm.PackageManager.PERMISSION_GRANTED) return 1;
        return gccSceneDenied ? -1 : 0;
    }

    public void gccRequestScenePermission() {
        runOnUiThread(() -> {
            if (gccSceneRequestPending || gccScenePermissionState() != 0 || isFinishing()) return;
            gccSceneRequestPending = true;
            requestPermissions(new String[]{"com.oculus.permission.USE_SCENE"}, GCC_SCENE_REQUEST);
        });
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == GCC_SCENE_REQUEST) {
            gccSceneRequestPending = false;
            gccSceneDenied = grantResults.length == 0 || grantResults[0] != android.content.pm.PackageManager.PERMISSION_GRANTED;
        }
    }

    public String gccRoomTokenPath() {
        try {
            java.io.File privateRoot = getFilesDir().getCanonicalFile();
            java.io.File imageRoot = new java.io.File(privateRoot, "imagefs").getCanonicalFile();
            java.io.File parent = new java.io.File(imageRoot, "tmp/xr").getCanonicalFile();
            // imagefs/tmp may be its normal usr/tmp symlink; canonical target stays private.
            if (!imageRoot.getPath().startsWith(privateRoot.getPath() + "/") ||
                !parent.getPath().startsWith(imageRoot.getPath() + "/")) return "";
            if (!parent.isDirectory() && !parent.mkdirs()) return "";
            return new java.io.File(parent, "gta2ceiling_room.token").getPath();
        } catch (java.io.IOException | SecurityException error) {
            android.util.Log.e("GCCROOM1", "Private room-token directory unavailable");
            return "";
        }
    }

    // GTA2Ceiling: the Quest Touch controllers are always a pad as well, in the
    // Xbox layout (WinlatorXR's gamepad emulation); a connected Xbox controller
    // works at the same time - WinHandler merges both into player 1. Mapped keys
    // and mouse emulation stay off so nothing is pressed twice. Called from the
    // input thread (XrInput.updateXServer); only the first call does anything.
    private Boolean gccMode = null;
    private boolean gccTouchPadOn = false;

    public boolean gccMode() {
        if (gccMode == null) gccMode = SessionSettings.getBoolean(this, "gcc_exact_colour_key", false);
        return gccMode;
    }

    public void gccUpdateTouchPad() {
        if (gccTouchPadOn || !gccMode()) return;
        gccTouchPadOn = true;
        gamepadEmulation = true; keysEmulation = false; mouseEmulation = false;
        android.util.Log.i("GTA2Ceiling", "Touch controllers and game controller both drive player 1");
    }

    // GTA2Ceiling: the right stick press belongs to the game (held 4 s it freezes
    // the city, with the left one it opens the mod's menu), so WinlatorXR's own
    // menu opens with the LEFT stick held alone for a second.
    public static boolean gccMenuHold(boolean[] buttons, com.winlator.xr.api.XrInterface.ControllerButton primary) {
        XrActivity activity = getInstance();
        if (activity == null || !activity.gccMode()) return buttons[primary.ordinal()];
        return buttons[com.winlator.xr.api.XrInterface.ControllerButton.L_THUMBSTICK_PRESS.ordinal()]
                && !buttons[com.winlator.xr.api.XrInterface.ControllerButton.R_THUMBSTICK_PRESS.ordinal()];
    }

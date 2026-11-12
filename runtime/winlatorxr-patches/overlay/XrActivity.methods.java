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

    // GTA2Ceiling: without a game controller the Quest Touch controllers are the
    // pad. They become a virtual Xbox pad on P1 (WinlatorXR's gamepad emulation,
    // same button layout); a connected physical controller always wins. Checked
    // about once a second from the input thread (XrInput.updateXServer).
    private Boolean gccMode = null;
    private Boolean gccTouchPad = null;
    private boolean gccSavedKeys = true, gccSavedMouse = true;
    private long gccPadCheckAt = 0;

    public boolean gccMode() {
        if (gccMode == null) gccMode = SessionSettings.getBoolean(this, "gcc_exact_colour_key", false);
        return gccMode;
    }

    // A real game controller (Bluetooth or USB), never the Quest's own controllers.
    private static String gccPhysicalPad() {
        for (int id : android.view.InputDevice.getDeviceIds()) {
            android.view.InputDevice device = android.view.InputDevice.getDevice(id);
            if (device == null || device.isVirtual()) continue;
            int sources = device.getSources();
            if ((sources & android.view.InputDevice.SOURCE_GAMEPAD) != android.view.InputDevice.SOURCE_GAMEPAD) continue;
            String name = device.getName() == null ? "" : device.getName();
            String lower = name.toLowerCase(java.util.Locale.ROOT);
            if (lower.contains("oculus") || lower.contains("meta") || lower.contains("quest") || lower.contains("touch")
                    || lower.contains("virtual") || lower.contains("uinput")) continue;
            return name.isEmpty() ? "controller " + id : name;
        }
        return null;
    }

    public void gccUpdateTouchPad() {
        long now = System.currentTimeMillis();
        if (now < gccPadCheckAt || !gccMode()) return;
        gccPadCheckAt = now + 1000;
        String physical = gccPhysicalPad();
        boolean touch = physical == null;
        if (gccTouchPad != null && gccTouchPad == touch) return;
        if (gccTouchPad == null) { gccSavedKeys = keysEmulation; gccSavedMouse = mouseEmulation; }
        gccTouchPad = touch;
        if (touch) {
            // Only the pad: mapped keys or a mouse on top would press things twice.
            gamepadEmulation = true; keysEmulation = false; mouseEmulation = false;
            android.util.Log.i("GTA2Ceiling", "no game controller: the Touch controllers act as an Xbox pad");
        } else {
            if (gamepadEmulation) getWinHandler().sendVirtualGamepadState(new com.winlator.cmod.inputcontrols.GamepadState());
            gamepadEmulation = false; keysEmulation = gccSavedKeys; mouseEmulation = gccSavedMouse;
            android.util.Log.i("GTA2Ceiling", "game controller connected (" + physical + "): Touch pad off");
        }
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

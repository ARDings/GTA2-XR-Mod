// SPDX-License-Identifier: GPL-3.0-or-later
package com.winlator.cmod.core;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.res.AssetManager;
import android.os.Build;
import java.security.MessageDigest;

/** Read-only assets from the exact official donor; all extracted files belong to this fork. */
public final class GccAssets {
    private static final String DONOR = "com.winlator.cmod";
    // DER certificate measured from the installed, APK-SHA-verified official dawn-42 on 2026-11-08.
    private static final String SIGNER_SHA256 = "2559de0328c65536a42906a31697514b508dbd72dfa030a313b31a444cfe767e";
    private GccAssets() {}

    public static AssetManager get(Context context) {
        try {
            PackageManager manager = context.getPackageManager();
            int flags = Build.VERSION.SDK_INT >= 28 ? PackageManager.GET_SIGNING_CERTIFICATES : PackageManager.GET_SIGNATURES;
            PackageInfo info = manager.getPackageInfo(DONOR, flags);
            long version = Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
            if (version != 24 || !"dawn-42".equals(info.versionName)) throw new SecurityException("Official WinlatorXR dawn-42 is required");
            Signature[] signers = Build.VERSION.SDK_INT >= 28
                ? info.signingInfo.getApkContentsSigners() : info.signatures;
            if (signers == null || signers.length != 1) throw new SecurityException("Unexpected runtime signer count");
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(signers[0].toByteArray());
            StringBuilder hex = new StringBuilder();
            for (byte value : digest) hex.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
            if (!SIGNER_SHA256.equals(hex.toString())) throw new SecurityException("Runtime signature does not match the official donor");
            return manager.getResourcesForApplication(DONOR).getAssets();
        } catch (Exception error) {
            throw new IllegalStateException("Install the verified official WinlatorXR dawn-42 before opening GTA2Ceiling Host. Its private containers are never imported.", error);
        }
    }
}

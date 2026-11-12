package com.gta2ceiling.roomprobe;

import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;
import android.view.WindowManager;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/** Separate diagnostic app. It reads existing Scene data and never starts Room Setup. */
public final class RoomProbeActivity extends Activity {
    private static final String SCENE = "com.oculus.permission.USE_SCENE";
    private static final int PERMISSION_REQUEST = 31;
    static { System.loadLibrary("roomprobe"); }
    private TextView status;
    private boolean started;
    private native void nativeStart(String reportPath);
    private native void nativeStop();
    private native void nativeSetResumed(boolean resumed);

    @Override public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(32, 32, 32, 32);
        status = new TextView(this);
        status.setTextSize(20);
        status.setText("Room diagnostic: reads the existing ceiling. No new room scan.\n" +
            "The data is not applied to WinlatorXR.");
        panel.addView(status);
        Button grant = new Button(this);
        grant.setText("Check room access");
        grant.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View view) { requestScene(); }
        });
        panel.addView(grant);
        Button close = new Button(this);
        close.setText("Close");
        close.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View view) { finish(); }
        });
        panel.addView(close);
        setContentView(panel);
        requestScene();
    }

    private void writePermissionStatus(String value) {
        String json = "{\"formatVersion\":1,\"source\":\"GTA2CeilingRoomProbe/OpenXR\"," +
            "\"coordinateSpace\":\"NATURAL_STAGE\",\"units\":\"meters\",\"status\":\"" + value +
            "\",\"captureUnixMs\":" + System.currentTimeMillis() + ",\"ceilings\":[],\"events\":[]}";
        try (FileOutputStream out = new FileOutputStream(new File(getFilesDir(), "roomprobe.json"))) {
            out.write(json.getBytes(StandardCharsets.UTF_8));
        } catch (Exception error) { Log.e("GTA2RoomProbe", "Permission report write failed", error); }
        Log.i("GTA2RoomProbe", value);
    }

    private void requestScene() {
        if (started) return;
        if (checkSelfPermission(SCENE) == PackageManager.PERMISSION_GRANTED) {
            started = true;
            writePermissionStatus("SCENE_PERMISSION_GRANTED_STARTING");
            status.setText("Room access granted. OpenXR query running (at most 60 seconds).\n" +
                "Existing room data stays unchanged.");
            nativeStart(new File(getFilesDir(), "roomprobe.json").getAbsolutePath());
        } else {
            writePermissionStatus("SCENE_PERMISSION_REQUIRED");
            requestPermissions(new String[]{SCENE}, PERMISSION_REQUEST);
        }
    }

    @Override public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(code, permissions, results);
        if (code != PERMISSION_REQUEST) return;
        if (results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) requestScene();
        else {
            writePermissionStatus("SCENE_PERMISSION_DENIED");
            status.setText("No room access. No room data was read.\n" +
                "Confirm the request in the headset to continue the diagnostic.");
        }
    }

    // Called from the native worker after it has closed its OpenXR session.
    public void onNativeResult(final String result) {
        runOnUiThread(new Runnable() {
            @Override public void run() { if (!isFinishing()) status.setText(result +
                "\nReport: private app file roomprobe.json\nNothing is sent to WinlatorXR."); }
        });
    }
    @Override protected void onResume() { super.onResume(); nativeSetResumed(true); }
    @Override protected void onPause() { nativeSetResumed(false); super.onPause(); }
    @Override protected void onDestroy() { nativeStop(); super.onDestroy(); }
}

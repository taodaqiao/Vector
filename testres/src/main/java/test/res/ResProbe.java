package test.res;

import android.util.Log;

import de.robv.android.xposed.IXposedHookInitPackageResources;
import de.robv.android.xposed.callbacks.XC_InitPackageResources.InitPackageResourcesParam;

/**
 * Touches the resource-hook path exactly once: the framework defines the dummy super classes and
 * resolves the (possibly renamed) XResources members only when a module implements this interface,
 * so this is the probe that proves that path still works after member renaming.
 */
public class ResProbe implements IXposedHookInitPackageResources {
    private static final String TAG = "ResProbe";

    @Override
    public void handleInitPackageResources(InitPackageResourcesParam resparam) {
        Log.i(TAG, "handleInitPackageResources pkg=" + resparam.packageName
                + " res=" + (resparam.res != null));
        if (resparam.res == null) {
            Log.e(TAG, "res is null, the framework did not hand over XResources");
            return;
        }
        try {
            int id = resparam.res.getIdentifier("app_name", "string", resparam.packageName);
            String original = id == 0 ? null : resparam.res.getString(id);
            Log.i(TAG, "probe id=" + id + " original=" + original);
            if (id != 0) {
                resparam.res.setReplacement(resparam.packageName, "string", "app_name",
                        "probe-ok");
                Log.i(TAG, "replacement installed, now="
                        + resparam.res.getString(id));
            }
        } catch (Throwable t) {
            Log.e(TAG, "probe failed", t);
        }
    }
}

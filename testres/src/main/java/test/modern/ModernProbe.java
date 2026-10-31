package test.modern;

import android.util.Log;

import io.github.libxposed.api.XposedModule;

/** Exercises the modern (API 102) entry: META-INF/xposed/java_init.list plus XposedModule. */
public class ModernProbe extends XposedModule {
    private static final String TAG = "ModernProbe";

    @Override
    public void onModuleLoaded(ModuleLoadedParam param) {
        Log.i(TAG, "onModuleLoaded process=" + param.getProcessName()
                + " system=" + param.isSystemServer());
    }

    @Override
    public void onPackageLoaded(PackageLoadedParam param) {
        Log.i(TAG, "onPackageLoaded pkg=" + param.getPackageName()
                + " loader=" + (param.getDefaultClassLoader() != null)
                + " first=" + param.isFirstPackage());
    }

    @Override
    public void onPackageReady(PackageReadyParam param) {
        Log.i(TAG, "onPackageReady pkg=" + param.getPackageName()
                + " factory=" + (param.getAppComponentFactory() != null));
    }
}

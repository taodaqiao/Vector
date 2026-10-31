# Verification fixtures

Two throwaway modules that exist only to prove the two module-loading paths still work after the
de-identification work in `../debrand`. They are not shipped and are not part of the framework.

| Flavor | Packaging | What it proves |
|---|---|---|
| `legacy` | `src/legacy/assets/xposed_init` → `test.res.ResProbe` | `IXposedHookInitPackageResources` fires, `XResources` builds on top of `rt.dummy.*`, and the renamed `translateResId`/`translateAttrId` resolve — the replacement actually changes the value returned by `getString`. |
| `modern` | `src/modern/resources/META-INF/xposed/{module.prop,java_init.list}` → `test.modern.ModernProbe` | The API 101+ loader instantiates the entry class, and `onModuleLoaded`/`onPackageLoaded`/`onPackageReady` receive a non-null class loader and `AppComponentFactory`. |

Note the entry class must **extend** `io.github.libxposed.api.XposedModule`; merely implementing
`XposedModuleInterface` is skipped with `Class does not extend XposedModule`. The modern API has no
resources callback — resource hooks are legacy-only in this framework.

## Run one

```powershell
.\gradlew.bat :testres:assembleLegacyRelease --console=plain --max-workers=2
# first time only: keytool -genkeypair -keystore testres\probe.jks -storetype PKCS12 `
#   -alias probe -storepass probepass -keypass probepass -keyalg RSA -keysize 2048 `
#   -validity 3650 -dname "CN=probe"
$bt = "$env:LOCALAPPDATA\Android\Sdk\build-tools\37.0.0"
& "$bt\apksigner.bat" sign --ks testres\probe.jks --ks-pass pass:probepass `
    --key-pass pass:probepass --out testres\probe.apk `
    testres\build\outputs\apk\legacy\release\testres-legacy-release-unsigned.apk
adb install -r testres\probe.apk
adb shell su -c 'sh /data/adb/modules/perf_patch/cli modules enable test.res'
adb shell su -c 'sh /data/adb/modules/perf_patch/cli scope add test.res com.android.settings/0'
adb shell am force-stop com.android.settings; adb shell am start -a android.settings.SETTINGS
adb logcat -d -v brief | Select-String 'ResProbe'
```

`modern` is the same with `assembleModernRelease`, the `modern/release` artifact path, and
`Select-String 'ModernProbe'`.

## Undo

```powershell
adb shell su -c 'sh /data/adb/modules/perf_patch/cli scope rm test.res com.android.settings/0'
adb shell su -c 'sh /data/adb/modules/perf_patch/cli modules disable test.res'
```

Leaving the module installed but disabled is harmless: it is an ordinary APK with no launcher
activity, and the manager only loads it when it is enabled *and* the process is in its scope.

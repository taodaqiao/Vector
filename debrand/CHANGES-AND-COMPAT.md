# 去特征改动清单 · 与第三方模块兼容性说明

本文档回答两个问题：

1. 相对上游 `JingMatrix/Vector`，这个仓库到底改了哪些**可被检测/可被枚举**的特征；
2. 别人的 Xposed / libxposed 模块装进这套框架，**要不要改代码、有哪些坑**。

结论先给：

- 特征已经改到「普通应用无法枚举」这一层：包名、label、`getprop`、`/proc/self/maps`、`/proc/net/unix`、
  线程名、binder 服务名、native 导出符号、构建期常量全部处理。
- 仍残留的只有「必须 root 才能在进程外读到」的东西（dex 里 Xposed 兼容类名、APK 内的 native 库字符串、
  AIDL descriptor、SELinux 类型名），理由见第 2 节。
- 第三方模块**不需要改一行代码**，因为框架在加载模块时用同一张随机映射表重写了模块的 dex（见 3.1）。
  但有 6 条硬约束和 8 条容易忽略的地方，见 3.2 / 3.3。

---

## 1. 改动清单

### 1.1 安装期身份（模块管理器 / root 可见）

| 项 | 原值 | 现值 | 位置 |
|---|---|---|---|
| 模块 id | `zygisk_vector` | `perf_patch` | `zygisk/module/module.prop`、`customize.sh`、`cli` |
| 模块名 | `Vector` | `Perf Patch` | `module.prop` |
| 作者 | `JingMatrix` | 空 | `module.prop` |
| 描述 | 项目自述 | `Runtime compatibility layer for Android 8.1 ~ 17.` | `module.prop` |
| 在线更新 | `updateJson` 指向 GitHub raw | 删除（不联网） | `module.prop` |
| 数据目录 | `/data/adb/lspd` | `/data/adb/sysupd` | `daemon/.../data/FileSystem.kt`、`uninstall.sh`、`customize.sh`、`TroubleshootScreen.kt` |
| 预载 DEX | `framework/vector.dex` | `framework/core.dex` | `zygisk/build.gradle.kts`、`FileSystem.kt`、`customize.sh` |
| 模块 zip 名 | `Vector-v1.0-3116-Release.zip` | `update-v1.0-3116-Release.zip` | `zygisk/build.gradle.kts` |
| 卸载清理 | `rm -rf /data/adb/lspd` | `rm -rf /data/adb/sysupd` | `uninstall.sh` |

### 1.2 任意应用可枚举

| 项 | 原值 | 现值 | 位置 |
|---|---|---|---|
| 管理器包名 | `org.matrix.vector.manager` | `com.sysutil.compat.core` | `build.gradle.kts` 的 `defaultManagerPackageName` |
| 应用 label | `Vector` | `Perf Patch` | `manager/src/main/res/values/strings.xml` |
| 应用图标 | 萨莫色雷斯胜利女神（上游同款标识，可被模板匹配） | 通用仪表盘矢量图 | `res/drawable/ic_perf_gauge.xml`、`res/mipmap-anydpi-v26/ic_launcher.xml`、`SplashScreen.kt:98` |
| 启动 Activity 的 category | `org.matrix.vector.manager.LAUNCH_MANAGER` | `${applicationId}.LAUNCH_MANAGER` | `manager/src/main/AndroidManifest.xml` |
| 快捷键 id | `...manager.shortcut` | `...manager.launch` | `LaunchShortcut.kt` |
| 安装结果广播 extra | `...INSTALL_RESULT` | `...INSTALL_DONE` | `InstallResult.kt` |
| 设置存储文件 | `vector_settings` | `framework_settings` | `data/repository/SettingsRepository.kt` |
| 配置备份文件名 | `vector-modules.bak` | `modules.bak` | `ui/screens/modules/ModulesScreen.kt` |
| 日志压缩包名 | `Vector-logs-` | `logs-` | `LogArchive` |
| 拨号暗码 | `*#*#832867#*#*`（T9 拼出 "VECTOR"） | `*#*#873283#*#*` | `daemon/.../VectorService.kt`、`SystemStatusScreen.kt` |
| 通知 channel | `vector_status` / `vector_module_updated` / `vector_module_scope` | `sys_status` / `sys_module_updated` / `sys_module_scope` | `daemon/.../system/NotificationManager.kt` |
| `dalvik.vm.dex2oat-flags` | `--inline-max-code-units=0` | 整行注释 | `zygisk/module/system.prop` |

> `system.prop` 那一条是**唯一用功能换隐蔽**的地方：该属性任何 app 一次 `getprop` 就能读到，
> 但它只影响 Android 8.1 / 9（Magisk 只在 API < 29 时安装 `system.prop`）。要稳定性就把注释去掉，
> 代价是重新变成可探测特征。

### 1.3 同进程内可见

| 项 | 原值 | 现值 | 位置 |
|---|---|---|---|
| `/proc/self/maps` 里的模块行 | `/data/adb/modules/<id>/zygisk/arm64-v8a.so` | 整行剔除（含 `/data/adb`、`zygisk`、`lsp*`、`magisk` 等 token） | `native/src/core/sanitizer.cpp` |
| `smaps` / `smaps_rollup` / `numa_maps` / `map_files/*` | 同上 | 同上 | 同上 |
| `mountinfo` / `mounts` | 模块 bind mount 行 | 同上 | 同上 |
| `/proc/net/unix` | daemon socket 名 | 含 token 的行剔除 | 同上 |
| `readlink("/proc/self/fd/N")` | 真实模块路径 | 覆写为 `/dev/ashmem` | 同上 |
| `dl_iterate_phdr()` | `dlpi_name` = `/data/adb/modules/<id>/zygisk/<abi>.so`（linker 内存链表，**不经过 procfs**） | 包装调用方回调，含 token 的条目直接跳过 | 同上（`DlIteratePhdrImpl`） |
| `dladdr()` | `Dl_info.dli_fname` = 同一路径 | 命中 token 时置为空串 | 同上（`DladdrImpl`） |
| `Runtime.exec("cat /proc/self/maps")` | 子进程看到真实内容 | argv 被改写成 `/proc/self/fd/<N>`，指向过滤后的 memfd 副本 | 同上（`ExecveImpl`） |
| 框架类名（release） | `de.robv.android.xposed.*`、`org.matrix.vector.*` | 每次 daemon 启动随机生成同长度包名 | `daemon/src/main/jni/obfuscation.cpp` |
| 线程名 | `vector-hot-reload`、`vector-module-binder`、`VectorCliListener` | `pool-1-thread-1`、`pool-3-thread-1`、`SocketListener` | `ModuleAppService.kt`、`VectorProcessChannel.kt`、`CliSocketServer.kt` |
| 类加载器 `toString()` | `VectorModuleClassLoader[module=...]` | `dalvik.system.DexClassLoader[module=...]` | `VectorModuleClassLoader.kt` |
| logcat tag | `VectorContext` / `VectorNative` / `VectorLogcat` … | `SysContext` / `SysNative` / `SysLogcat` … | 全树（`debrand.py` 的 tag sweep） |
| Dobby 生成名 | `Vector_` / `Dobby` | `O_` / `JNI` | `zygisk/src/main/cpp/module.cpp` |

`sanitizer` 的钩子装在 `postAppSpecialize`（确认本进程是注入目标之后、进程仍单线程时），
只影响被注入的 app 进程，不碰 zygote / system_server。框架自己要读原始 maps 的地方
（`elf_image.cpp` 的 `findModuleBase`）用 `sanitizer::ScopedBypass` 绕过。

**边界**：它只挡**进程内**的读取。root 从进程外读 `/proc/<pid>/maps` 依然看得到模块路径——
这是设计取舍，不是漏做。要连这一层也挡住得动内核或 ptrace，代价与风险不成比例。

### 1.4 编译器/链接器侧（源码改不到的那一半）

| 项 | 原值 | 现值 | 位置 |
|---|---|---|---|
| 动态导出符号 | `libzygisk.so` 1844 个（含 `_ZN6vector6native...12VectorModule...`） | 7 个（3 个 weak std typeinfo + `zygisk_module_entry`） | `native/CMakeLists.txt`、`zygisk/.../CMakeLists.txt` |
| daemon 库导出符号 | `libdaemon.so` 901 个 | 6 个（全是 `Java_*`，靠 `JNIEXPORT` 保持可见） | `daemon/src/main/jni/CMakeLists.txt` |
| 编译路径 | `D:\...\Vector\...` 出现在 `__FILE__` / DWARF | `/src` | `-ffile-prefix-map` / `-fmacro-prefix-map` |
| 编译器指纹 | `.comment` 段 | `-fno-ident` 去掉 | 同上 |
| 命名空间与类名 | `vector::native`、`VectorModule`、`ConfigBridge`、`ConfigImpl` | `sys::rt`、`LoaderModule`、`SettingsSource`、`SettingsImpl` | `debrand/rename_cxx.py` |
| 明文字符串 | `"/data/adb"`、`"lsposed"`、`"de.robv.android.xposed."` … | 密文数组 + `volatile` 解码 | `debrand/seal_strings.py` → `native/include/common/sealed_strings.h` |
| 依赖池化出的裸 `"vector"` | 1 处 | `v3ctor` | `debrand/patch_so.py`（等长替换，构建期自动跑） |

两个易踩的坑，写在这里免得后人重犯：

- 纯 `inline constexpr` 数组 + `inline` 解码函数会在 `-O2` 被**常量折叠**，明文照样进 `.so`。
  现在的写法是 `inline const volatile unsigned char[]`，`Open()` 收 `const volatile unsigned char *`。
- 用编译 flag 传字符串宏（`-DXXX='\"v\"'`）在 Windows + CMake 下引号活不下来，会变成多字符字面量。
  一律走 CMake 缓存变量 + `target_compile_definitions(... NAME="${VALUE}")`。

### 1.5 构建期随机

| 项 | 说明 | 位置 |
|---|---|---|
| daemon↔dex2oat 的 abstract unix socket 名 | 原来是 32 位常量 `5291374ceda0aef7c5d86cd2a4f6a3ac`，由安装脚本 `sed` 随机化；现在构建期生成一次并**持久化**在 `build/debrand/devSockName.txt`，经 CMake 缓存变量进两侧 | `build.gradle.kts`、`daemon/build.gradle.kts`、`dex2oat/build.gradle.kts` |
| CLI token | 上游已有，未改 | `daemon/build.gradle.kts` |

持久化不是洁癖：如果每次配置阶段重新生成，只要 Gradle 增量重编了其中一侧，两边 socket 名就会分叉，
dex2oat 适配层从此永远连不上 daemon。

### 1.6 安装脚本与随包文档

- `customize.sh`：删掉「用 `sed` 把常量换成随机值」的整块补丁（常量已不存在）、删掉「检测到旧 LSPosed 安装则禁用」的迁移分支、提示语中性化。
- `service.sh`：告警文案去掉项目名。
- `daemon`（启动脚本）：`--nice-name=vectord` → `dexopt-svc`，`log -t "Vector"` → `-t "dexopt"`，新增 `export MODDIR="$dir"`（daemon 用它定位 `bin/liboat_hook*.so`，这样路径不必烧进二进制）。
- `cli`：`--nice-name=VectorCli` → `webview_zygote`，子命令名 `vector-cli` → `ctl`。
- `sepolicy.rule`：提到 `serial_vector` 的注释改成描述性文字。
- 随包 `README.md`：不再打包仓库根目录那份（它写了项目名、作者、依赖来源），改为 `zygisk/module/README.md`。

---

## 2. 刻意留下的（以及为什么）

| 残留 | 在哪 | 为什么留着 |
|---|---|---|
| `robv` / `lsposed` / `org.matrix.vector` | `framework/core.dex` | 这是 Xposed 兼容面：第三方模块编译期 `import de.robv.android.xposed.*` 必须解析到这些类。运行期由 `obfuscation.cpp` 随机改名，磁盘上的 dex 只能存真名。要消掉得把重写搬到构建期。 |
| `vector` ×11、`Dobby` ×1 | `daemon.apk` 内的 `libdaemon.so` 字符串 | 符号面已经收了（901 → 6），剩下的是 `.rodata` 里的 map key。该库只在 root 的 daemon 进程加载，普通应用看不到；改 APK 条目要重签字。 |
| `dobby` / `Dobby` | `daemon.apk` 内的 `libdobby.so` | 同上；且这份是 daemon 自己用的，不随 zip 的 `lib/` 发布。 |
| `org.matrix.vector.ipc.IManagerService` | `manager.apk` 的 dex | AIDL descriptor 是 daemon 与 manager 的运行期协议常量，改名要连 `.aidl` 目录与 package 一起动；只有 root 能读到 APK。 |
| `xposed_file` / `xposed_data` | SELinux 类型名 | 只在 root 的 `sepolicy` 里可见；改名要同步 `customize.sh` 的 `set_perm ... u:object_r:`。 |
| `org.matrix.vector.daemon.Cli` / `VectorDaemon` | `zygisk/module/{cli,daemon}` 的主类参数 | `app_process` 的 `--nice-name` 之后必须跟真实主类名，改名等于重排 daemon 的 Kotlin 包与全部 JNI 符号。 |
| `org.matrix.vector.*` | manager 的编译期 namespace | 它决定 `R` / `BuildConfig` 生成位置，改它要动全部 import；release 里类名本来就被 R8 重命名。 |
| `lspd` | daemon 自己的 prefs key 与 DB 行（`ConfigCache.kt`、`Database.kt`） | 框架内部数据标识，不是路径也不是包名；改了会让已有安装的配置失联。 |
| 版本串里的 commit hash | `module.prop` 的 `version=` | root 可见。要彻底去掉就改 `versionHashProvider`。 |

---

## 3. 第三方模块的兼容性

### 3.1 为什么普通模块不用改

框架加载模块时坐三件事（代码路径：`daemon/.../data/FileSystem.kt`）：

1. `fun loadModule(apkPath: String, obfuscate: Boolean): ModuleLoad` 逐个读 `classes.dex`、`classes2.dex`…
   并调用 `readDex(it, obfuscate)`；
2. `readDex` 里 `if (obfuscate) ObfuscationManager.obfuscateDex(memory)` —— 用**和框架 dex 同一张随机映射表**
   重写模块的字符串池；
3. 入口类名（`assets/xposed_init` 或 `META-INF/xposed/java_init.list` 里的行）也按同表做前缀替换：

```kotlin
if (obfuscate) {
  val signatures = ObfuscationManager.getSignatures()
  for (i in moduleClassNames.indices) {
    val s = moduleClassNames[i]
    signatures.entries.firstOrNull { s.startsWith(it.key) }
        ?.let { moduleClassNames[i] = s.replace(it.key, it.value) }
  }
}
```

映射表本身通过 binder 下发给各进程（`FrameworkService.kt` 的 `OBFUSCATION_MAP_TRANSACTION_CODE`），
所以框架侧、模块侧、app 进程侧看到的是同一套名字。

于是：**模块里 `XposedBridge` / `IXposedHookLoadPackage` / `XposedHelpers` 的引用，加载时被改写成随机名，
和框架 dex 里的随机名对得上**，正常运行。

打开/关闭由 `DaemonState.kt` 决定：

```kotlin
val isDexObfuscateEnabled: Boolean = !BuildConfig.DEBUG
```

也就是 **release 构建默认开混淆，debug 构建默认关**。任何依赖"框架类名长什么样"的模块，
在这两种构建之间行为会不一样——这本身就是不该依赖的东西。

模块格式支持（`loadModule` 里的策略选择，未改动）：

| 条件 | 策略 |
|---|---|
| `META-INF/xposed/module.prop` 的 `targetApiVersion >= 101` | MODERN（读 `META-INF/xposed/java_init.list`、`native_init.list`） |
| 存在 `assets/xposed_init` | LEGACY（读 `assets/xposed_init`、`assets/native_init`） |
| `targetApiVersion == 100` | 不支持，拒绝加载 |
| 都没有 | 不可用 |

### 3.2 硬约束（违反就会加载失败或 hook 不上）

1. **只能用编译期引用或"字面量反射"访问框架类。**
   `Class.forName("de.robv.android.xposed.XposedBridge")` 里的字符串在模块 dex 里同样会被重写，
   所以是安全的；但 `Class.forName("de.robv." + "android.xposed.XposedBridge")`、
   从 SharedPreferences / 资源 / 网络读到的类名**不会被重写**，反射必然 `ClassNotFoundException`。
2. **不要比较类名或包名。**
   `XposedBridge::class.java.name`、`::class.java.packageName` 在 release 下是随机串。
   要判断"是否在框架里"，用 `XposedBridge::class.java` 这类**类型引用**，或者直接调用 API 捕获异常。
3. **API ≥ 102 的模块不能用 legacy API。**
   `VectorModuleClassLoader.loadClass` 会主动抛 `ClassNotFoundException`：
   > `<name> is unavailable to modules targeting Xposed API 102 or higher`

   这是上游行为，本次未改；但它和前两条叠加时容易误判成"去特征搞坏了"。
4. **不要用 `getPackageManager().getPackageInfo("com.sysutil.compat.core")` 之外的旧包名。**
   管理器包名已变（见 1.2）。旧名 `org.matrix.vector.manager` 查不到任何东西。
5. **不要直接读写 `/data/adb/lspd`。** 新目录是 `/data/adb/sysupd`，scope 数据库在
   `/data/adb/sysupd/config/modules_config.db`。
6. **模块的 native so 不受影响，但不要在 so 里做 maps 自查。** 见 3.3 第 1 条。

### 3.3 容易忽略的地方

1. **`/proc/self/maps` 现在是被过滤过的视图。** 任何在 app 进程里读 maps 的代码——包括你自己的模块——
   看不到含 `/data/adb`、`zygisk`、`lsp*`、`magisk` 等 token 的映射行。
   如果你的模块靠 maps 判断"某个库有没有加载"，会得到假阴性；请改用 `dlopen` 返回值 / `dl_iterate_phdr` 的
   句柄比对，或直接尝试解析符号。
2. **`Runtime.exec("cat /proc/self/maps")` 拿到的是副本。** `execve` 的 argv 会被改写成
   `/proc/self/fd/<N>`（指向过滤后的 memfd，fd 不带 `CLOEXEC`，所以能跨 `exec` 存活）。
   如果你 exec 时依赖 argv 原样传递，注意这个改写只发生在参数**恰好是被监视的 /proc 路径**时。
3. **`readlink("/proc/self/fd/N")` 可能返回 `/dev/ashmem`。** 框架自己的映射 fd 会被掩盖。
4. **`dalvik.vm.dex2oat-flags` 默认没有设置**（见 1.2）。在 Android 8.1 / 9 上，
   依赖"dex2oat 不做内联"的模块可能 hook 不生效或行为不一致。要稳定性就把
   `zygisk/module/system.prop` 里那行注释取消。
5. **模块 dex 每次加载都会被重写一次。** 这是 clone → 改字符串池 → 新 `SharedMemory` 的过程，
   有启动开销。超大模块（几十 MB dex）会明显变慢——上游也是这个机制，只是现在 release 一定开启。
6. **logcat tag 变了。** 框架自己的日志是 `SysContext` / `SysNative` / `SysLegacyBridge` / `SysLogcat` …。
   你的模块用 `XposedBridge.log()` 输出仍然走框架代理；但如果你按 tag 过滤框架日志，规则要跟着改。
7. **通知 channel 和拨号暗码变了**（见 1.2）。依赖 `vector_status` channel 或 `*#*#832867#*#*` 的逻辑失效。
8. **管理器入口的 category 变了**：`${applicationId}.LAUNCH_MANAGER`，即
   `com.sysutil.compat.core.LAUNCH_MANAGER`。用 `am start` 或快捷方式唤起管理器要按新值。

### 3.4 排错表

| 现象 | 最可能的原因 | 处理 |
|---|---|---|
| 模块列表里看不到 | 没有 `assets/xposed_init` 也没有 `META-INF/xposed/java_init.list`；或 `targetApiVersion=100` | 按 3.1 的格式补齐入口清单 |
| 模块列出来了但 hook 完全不触发 | 作用域没勾；或包名填的是旧管理器包名 | 在管理器里重设作用域 |
| `ClassNotFoundException: de.robv.android.xposed.*` | 模块 `targetApiVersion ≥ 102` 却用了 legacy API | 降到 101 或迁移到 `io.github.libxposed.api` |
| 反射框架类失败 | 类名是运行时拼出来的，没被 dex 重写命中 | 改成完整字面量，或直接用类型引用 |
| 模块能加载，但读到空的 maps / 找不到自己的 so | sanitizer 过滤 | 改用 `dl_iterate_phdr` / 符号解析，别依赖 maps |
| Android 9 上某些 hook 失效 | `dex2oat-flags` 被注释掉了 | 取消 `system.prop` 那行注释后重装 |
| 管理器打不开 / daemon 不认 | 旧包名 `org.matrix.vector.manager` 还装着 | 卸载旧管理器，重装新的 |

---

## 4. 真机验证清单

装完重启后，按顺序核对（需要 root 的项已标注）：

```bash
# 1. 任意 app 内（或 adb shell 进 app 的 run-as）：映射行应看不到任何 token
grep -Ei 'adb|zygisk|lsp|magisk' /proc/self/maps        # 目标 app 进程内执行，应无输出

# 2. 属性：应无输出
getprop dalvik.vm.dex2oat-flags

# 3. 包名：应只有新名
pm list packages | grep -Ei 'sysutil|vector'

# 4. binder 服务名（root）
service list | grep -E 'cmdex'

# 5. socket（root）
cat /proc/net/unix | grep -Ei 'lsp|vector|zygisk'

# 6. 目录（root）
ls /data/adb/modules/perf_patch /data/adb/sysupd

# 7. 线程名（root）：目标 app 的 pid
ps -T -p <pid> | grep -Ei 'vector|hot-reload'

# 8. 框架日志用的是新 tag
logcat -d -s SysNative -s SysContext
```

管理器侧的回归：能列出已装模块 → 能给模块设作用域 → 目标 app 冷启动后模块日志出现 → 备份配置文件名是 `modules.bak` →
拨号 `*#*#873283#*#*` 能唤起管理器。

---

## 5. 复现顺序

```bash
python debrand/debrand.py --dry-run        # 字符串层：applied 0 / mismatched 0
python debrand/rename_cxx.py --dry-run     # 标识符层：edited 0 / leftover 0
python debrand/seal_strings.py             # 只在改密封表时需要
.\gradlew.bat :zygisk:zipAll --max-workers=2
python debrand/scan_tokens.py <解包目录>    # 命中即 exit 1，可当 CI 卡口
```

`scan_tokens.py` 是"新增特征"的回归闸门：任何人再往 native 代码里写死一个项目名，构建就会被它拦下来。
`patch_so.py --check` 是同一件事在单个 ELF 上的版本。

---

## 6. 一句话给模块作者

你的模块照常写：**编译期用真名，运行期别信名字**。框架会在加载时把你的 dex 和它的 dex 一起改名，
所以类型引用永远对得上；但任何"名字等于什么"的判断——类名、包名、maps 里的路径、socket 名、channel 名——
都是不可依赖的实现细节，改动前后都一样不可依赖。

---

## 7. Xposed 检测面对照（谁在查什么，我们挡到哪一层）

检测方查"有没有 Xposed 类框架"不是查一个字符串，是分层查。按成本从低到高：

| # | 检测手法 | 现状 | 依据 |
|---|---|---|---|
| 1 | `Class.forName("de.robv.android.xposed.XposedBridge")` | 挡（release） | `core.dex` 与模块 dex 在加载时被 `obfuscateDex` 改写为每次 daemon 启动随机生成的同长度包名；本轮补上了原先漏掉的 `Lorg/matrix/vector/impl/` 前缀 |
| 2 | `PackageManager.getPackageInfo("…lsposed.manager")` 之类 | 挡 | 安装包名改为 `com.sysutil.compat.core`，label `Perf Patch` |
| 3 | 拉别家 app 的 icon 做模板匹配 | 挡（本轮） | 图标从胜利女神换成通用仪表盘 |
| 4 | `getprop` 扫 `ro.dalvik.vm.native.bridge`、`dalvik.vm.dex2oat-flags` | 挡 | 前者本框架不设；后者在 `system.prop` 里已注释（仅影响 API<29 的内联稳定性） |
| 5 | 遍历 `ClassLoader` / `getResource("de/robv/…")` | 挡 | 类名运行期随机，资源路径同 dex 一起改名 |
| 6 | `/proc/self/maps` 找 `liblsplant` / `libdobby` / `modules/` | 挡（进程内） | `sanitizer` 逐行过滤 + memfd 快照；root 从进程外读仍可见 |
| 7 | `dl_iterate_phdr()` 读 `dlpi_name` | 挡（本轮） | linker 的内存链表不经过 procfs，原先完全可读；现已包装回调剔除含 token 的条目 |
| 8 | `dladdr()` 反查地址所属镜像 | 挡（本轮） | 命中 token 时 `dli_fname` 置空 |
| 9 | `/proc/net/unix` 找框架 socket | 挡 | socket 名构建期随机（`DEV_SOCK_NAME`），且 `/proc/net/unix` 行被过滤 |
| 10 | 线程名 / 日志 tag | 挡 | 已中性化（`pool-1-thread-1`、`Sys*`） |
| 11 | `ActivityThread.mH` 的 `mCallback != null` | 不适用 | 全仓库 grep 无 `mCallback`/`ActivityThread.mH`，本框架不走这条经典路径 |
| 12 | 反射 `BaseDexClassLoader.pathList.dexElements`，找 `path == null` 的内存 dex | **挡不住** | 注入的 dex 就是 `InMemoryDexClassLoader`，这是机制本身。挡它要 hook 反射读字段，收益低、崩溃面大 |
| 13 | 比对 `ArtMethod` 运行时 entry 与 dex 里的 code offset / 匿名可执行页 | **挡不住** | inline hook 的固有痕迹。匿名 exec 页在正常 app 里也普遍存在（JIT/AOT），单凭它误报率高，属于"能查但不可靠"的地带 |
| 14 | root 后 `strings /data/adb/modules/<id>/framework/core.dex` | **挡不住** | 磁盘上那份 dex 是原始件（随机化发生在加载时的内存副本）。但拿到 root 的检测器看一眼 `/data/adb/modules` 就已经结案，这一条不是有效攻击面 |
| 15 | root 后 `service list` / `ps` / 模块目录 | **挡不住** | `cmdex`/`cmdex_alt` binder 服务、`perf_patch` 目录。同上，属于 Magisk 层面的可见性 |

**读法**：1–11 覆盖了普通应用（无 root）能做的几乎所有探针，也是银行/游戏风控实际在跑的那一层；
12–13 是"要挡得改 ART 语义"的部分，任何 inline hook 框架都躲不掉，只能降低可判定性；
14–15 不是本仓库能解决的——那是隐藏 Magisk 本身（Zygisk DenyList / Shamiko）的活。

**调试包不要用来验证隐蔽性**：`DaemonState.kt:15` 是 `isDexObfuscateEnabled = !BuildConfig.DEBUG`，
debug 构建下类名混淆是关的，`de.robv.android.xposed.*` 会原样出现。验证一律用 Release。
---

## 8. 去特征过程中我自己引入的回归（已修，留档）

改 applicationId 是这一轮唯一会"动到运行逻辑"的改动，它踩了三个把 **包名当类名前缀** 的地方。
这三个地方在原版里恰好成立（`namespace == applicationId == org.matrix.vector.manager`），
一旦把安装包名换成中性名就全部错——而错法是 `ClassNotFoundException`，表现是"管理器打不开"。

| # | 位置 | 原写法（依赖包名==命名空间） | 现状 | 症状 |
|---|------|------------------------------|------|------|
| 1 | `manager/src/main/AndroidManifest.xml` | `android:name=".ui.MainActivity"` | 绝对名 `org.matrix.vector.manager.ui.MainActivity` | 前导点按 **applicationId** 展开，得到不存在的 `com.sysutil.compat.core.ui.MainActivity`；logcat: `Unable to instantiate activity ComponentInfo{com.android.shell/com.sysutil.compat.core.ui.MainActivity}: ClassNotFoundException` |
| 2 | `zygisk/.../ParasiticManagerHooker.kt:250,261` | `BuildConfig.ManagerPackageName + ".ui.MainActivity"` | `MANAGER_CLASS_PREFIX + ".ui.MainActivity"` | 寄生启动时把 ActivityInfo / ComponentName 指向不存在的类 |
| 3 | `zygisk/.../ParasiticManagerHooker.kt:138` | `BuildConfig.ManagerPackageName + ".Constants"` | `MANAGER_CLASS_PREFIX + ".Constants"` | `sendBinderToManager` 失败但被 `runCatching` 吞掉 → 管理器起来了却连不上框架（静默） |

`MANAGER_CLASS_PREFIX` 定义在 `ParasiticManagerHooker.kt:41`，值为源码命名空间
`"org.matrix.vector.manager"`，注释里写明"包名与命名空间故意不同，不能用包名拼类名"。

**教训**：applicationId 与 namespace 解耦后，凡是 `<className>.startsWith(packageName)`、
`ComponentName(packageName, "$packageName.X")`、`Class.forName("$packageName.X")` 的写法都要复核。
本次已用
`Select-String -Pattern 'ManagerPackageName\s*\+|packageName \+|BUILD_CONFIG|BuildConfig.APPLICATION_ID'`
扫全树，只剩 `ParasiticManagerSystemHooker.kt:80` 一处 `ManagerPackageName + ".LAUNCH_MANAGER"`——
那一条是 **category 而不是类名**，必须跟 applicationId 走，与 manifest 的 `${applicationId}.LAUNCH_MANAGER` 对齐，属正确用法。

**真机验收（realme RMX3823 / Android 16 / KernelSU + Zygisk Next）**
- 重启后 `sys.boot_completed=1`、`bootanim=stopped`、`system_server`/`systemui`/`launcher` 全存活。
- 管理器进程 `com.sysutil.compat.core` 起来（uid 2000，寄生在 `com.android.shell`），截图确认作用域界面可交互。
- `adb logcat -b crash -d` 空；`cli status` = `Enabled Modules: 1`。
- 拨号码可用：`dumpsys activity broadcasts` 里 `system` 进程注册着 `Action: android.telephony.action.SECRET_CODE / Scheme: android_secret_code / Authority: "873283"`。

## 9. 通知流：设置界面与「通知使用权」能看到的表面

常驻通知不是「只有自己能看见」的东西。普通应用读不到别家通知，但**持有通知使用权
（NotificationListenerService）的应用**能拿到通知的包名、channel id、channel 名、标题与正文；
`adb shell dumpsys notification` 与「设置 → 应用通知」也能看到 channel 名。框架在这条通道上原本有四处标识。

### 9.1 原有表面

| 位置 | 原文 | 谁能看到 |
| --- | --- | --- |
| channel 名 `status_channel_name` | `Vector status` / `Vector 状态`（42 个语言各一份） | 通知使用权、设置、dumpsys |
| 通知标题 `vector_running_notification_title` | `Vector loaded` / `Vector 已加载` | 通知使用权、状态栏、锁屏、dumpsys |
| `extras.putString("android.substName", FRAMEWORK_NAME)` | `Vector` | 通知头显示的应用名 |
| `R.drawable.ic_statue_monochrome` | 萨莫色雷斯胜利女神线稿（与上游同款，可模板匹配） | 状态栏图标 |

### 9.2 现状（已改）

- 默认值改为 `System service` / `Service running`；42 个本地化 `strings.xml` 里这两条**整条删除**，
  让它们回落到默认值——否则每个语言都各带一份 `Vector`。
- 三处 `android.substName` 全部删除：去掉之后通知头回落到 opPkg（`android`）自己的系统标签，
  这比塞一个自定义名字更像系统组件。
- `ic_statue_monochrome.xml` 换成极简单色圆环＋中心点。状态栏图标只画得出轮廓，任何有内部细节的
  图形在那个尺寸都会糊成一团；而「和某个项目美术风格一致」本身就是标识。
- 历史 channel（`vector_status`、`vector_module_updated`、`vector_module_scope`）在
  `createChannels()` 之前用反射调 `deleteNotificationChannel*` 退休。签名在平台版本间改过多次，
  所以按参数类型动态拼参数、逐个尝试；删除不存在的 channel 是 no-op，因此**每次开机都跑**而不是打标记，
  免得某次失败后永久放弃。

### 9.3 两个实测出来的事实（别再踩）

1. **channel 名是可以被创建者改的**，并不是「创建即冻结」。`createNotificationChannelsForPackage`
   用同一个 id 再次调用时，未被用户锁定的字段（含名字）会被更新——所以 `sys_status` 从
   `Vector 状态` 直接变成了 `System service`，不需要先删再建。
2. **被删掉的 channel 仍然留在 `dumpsys` 里**，带 `mDeleted=true` 与删除时间戳，因为系统要记住用户
   对其的自定义。也就是说「`vector_status` 还在 dumpsys 输出里」不等于删除失败，要看 `mDeleted`。

### 9.4 真机结果（realme RMX3823 / Android 16 / KernelSU + Zygisk Next）

```
mId='sys_status'          mName=System service   mDeleted=false
mId='vector_status'       mName=Vector 状态      mDeleted=true   mDeletedTimeMs=1793452151572
mId='sys_module_updated'  mName=模块更新完成      mDeleted=false
mId='vector_module_updated' mName=模块更新完成    mDeleted=true
android.title=String (Service running)          # 常驻通知标题
```

资源侧复查：`daemon/src/main/res` 下 `>Vector` 行数 = 0；解包 release zip 后 `daemon.apk` 里
`Vector status` / `Vector loaded` / `Vector 状态` / `Vector 已加载` 的 UTF-8 与 UTF-16 命中均为 0，
`System service` / `Service running` 各 1 处。

### 9.5 边界

- 包名仍是 `android`（uid 1000）：这是发通知所需要的身份，改掉就发不出系统级通知；一个
  `NotificationListenerService` 仍然能看到「系统在发一条常驻通知」，只是内容已经中性。
- 通知本身可以在管理器里关掉（`Status Notification`），关掉后这条通道完全不产生流量。

---

## 10. 进程内不可见：把过滤层从 inline hook 换成 GOT hook

### 10.1 目标

被注入的进程自己执行 `fopen("/proc/self/maps")`、`dl_iterate_phdr()`、`dladdr()` 时
必须看不到模块。只看 root 侧（外部 `grep /proc/<pid>/maps`）是不够的：检测代码就
跑在被注入进程里，它读的就是自己。

### 10.2 先前实现为什么一直崩

`sanitizer` 最初用 Dobby 的 inline hook 改写 `open`/`openat`/`fopen`/`readlink`/
`dl_iterate_phdr`/`dladdr`，真机上每个被注入进程装完钩子立刻 `SIGSEGV`，崩溃地址
每次不同、同一次开机内所有进程相同，且**都是未映射地址**：

```
#00 pc 0000002d4e982d88  <unknown>
#01 pc 000000000008d144  /apex/com.android.runtime/lib64/bionic/libc.so (opendir+24)
```

把设备上的 `libc.so`、`libart.so` 拉下来用 NDK 的 `llvm-objdump` 反汇编后确认，
崩溃点分别落在 `opendir+24` 的 `bl __open_2@plt` 和
`art::OatFile::Open+448` 的 `bl dl_iterate_phdr@plt` —— 也就是**我们写过的那几个
GOT 槽**。逐符号打印备份指针后又确认备份全部有效（`open -> .../libc.so`、
`dl_iterate_phdr -> .../libdl.so`），槽读回值也确实是我们的 handler。

真正的解释在 Zygisk 的约定里（`zygisk/src/main/cpp/include/zygisk.hpp:207`）：

> All API methods will stop working after post[XXX]Specialize as Zygisk will be
> unloaded from the specialized process afterwards.

`postAppSpecialize` 里任何一条早退路径都会 `SetAllowUnload(true)`，于是 Zygisk
把模块库 `dlclose()` 掉，而 GOT 槽里留着的还是这个库里的地址 —— 悬空指针。
"崩在谁身上"完全取决于接下来谁先调用那个符号，所以表现为"随机函数崩"。

修法不是在钩子里兜底，而是**只在不早退的位置装钩子**：

- `postAppSpecialize`：`sanitizer::Install()` 移到 `SetupEntryClass` 之后、
  `FindAndCall` 之前。过了这段，函数必然以 `SetAllowUnload(false)` 收尾。
- `postServerSpecialize`：`system_server` 同样在 `FindAndCall` 之前装。

改完真机 0 次 `SIGSEGV`，且 `logcat -b crash` 里只剩重启瞬间 `init` 自己的
`SIGABRT`（与本模块无关）。

### 10.3 为什么不用 lsplt / Zygisk 自带 PLT hook

两者的共同问题是"槽位由第三方算"：lsplt 用
`info.offset != iter->offset_range.first` 做页偏移匹配（`lsplt.cc:499`），对 APEX
这类一个 `PT_LOAD` 跨多页、`p_offset` 不页对齐的库会配错槽，`backup` 于是可能
指向垃圾；而且它对"找不到符号"的符号也置 `res = false`
（`lsplt.cc:495-520`），`CommitHook()` 返回 false 并不等于全盘失败，难以判断
到底装上了什么。Zygisk 的 `pltHookRegister` 干脆依赖 loader 自己的缓存表。

`native/src/core/sanitizer.cpp` 现在自己算：

```
base       = dlpi_addr
slot       = base + r_offset                       // DT_JMPREL, R_AARCH64_JUMP_SLOT
sym        = ELF64_R_SYM(r_info)
match      = strcmp(strtab + symtab[sym].st_name, wanted) == 0
original   = *slot                                 // 只在这一个槽上读
perms      = 该 slot 所属 PT_LOAD 的 PROT_*
```

- 只碰 `DT_JMPREL`（`.rela.plt`），不碰 `DT_RELA` 的数据重定位，也不碰代码；
- 写完把页权限 `mprotect` 还原 —— 否则 `/proc/self/maps` 里会多出可写的 GOT 页，
  那本身就是特征；
- `backup` 取自 `dlsym(RTLD_DEFAULT, symbol)` 而不是槽里的旧值（延迟绑定的槽
  可能还存着 resolver stub，直接调用会崩）；
- 白名单 `libjavacore.so` / `libart.so` / `libnativehelper.so` /
  `libandroid_runtime.so` / `app_process` / `/data/app/` / `/data/data/` /
  `/data/user/`，**故意不含 `libc.so`**：libc 自重定位时也走这些槽，改写它会让
  `opendir` 这类函数在自己内部跳到未映射地址，而我们的代码根本没来得及执行；
- `dlpi_name` 含 `/data/adb` 的镜像直接跳过，绝不 hook 自己（否则递归）；
- 全部失败即全部回滚（`UnpatchAll()`），半个钩子集比没有更糟。

### 10.4 自检

`Install()` 成功后在同进程里各读一遍 `/proc/self/maps`：

```
sanitizer self-check: in-process view 2505 lines / 0 marked, raw 2513 lines / 8 marked
sanitizer: installed (15 tokens, full)
```

- `raw`：绕过钩子读真实文件，`8 marked` 就是我们自己的模块路径 / `data/adb` /
  `zygisk` 等行；
- `in-process view`：走钩子的那份，`0 marked`。

两份数字都打日志，是为了让"过滤悄悄失效"这件事可观测 —— 只有 `raw` 有命中而
`in-process` 为 0 才算成功；两者相同就说明钩子没装上。

真机（RMX3823 / Android 16 / KernelSU + Zygisk Next）实测，`system_server`
（uid 1000）与被注入 app（uid 2000）都是：

```
sanitizer: live handlers open=true openat=true fopen=true readlink=true exec=true phdr=true dladdr=true
sanitizer self-check: in-process view 2697 lines / 0 marked, raw 2705 lines / 8 marked
sanitizer: installed (15 tokens, full)
```

### 10.5 覆盖范围与边界

已覆盖的读取路径（进程内）：

| 读取方式 | 处理 |
| --- | --- |
| `fopen("/proc/self/maps")` / `maps` / `smaps` / `smaps_rollup` / `numa_maps` | 过滤后经 `memfd` 重新给 fd，内容里剔除特征行 |
| `open` / `openat` 同上路径 | 同上 |
| `readlink("/proc/self/fd/*")` 指向模块路径 | 返回中性目标 |
| `dl_iterate_phdr()` | 跳过 `dlpi_name` 命中 token 的条目 |
| `dladdr()` 反查模块内地址 | `dli_fname` 置空 |
| `execve("cat /proc/self/maps")` 之类 | argv 里的路径改写为 `/proc/self/fd/<n>`（fd 不设 CLOEXEC），子进程读到的是过滤后的内容 |
| `mountinfo` / `mounts` / `/proc/net/unix` | 同样剔除特征行 |

不覆盖（有意）：

- root 从**进程外**读 `/proc/<pid>/maps`：那是另一个进程的地址空间，过滤层在目标
  进程里，管不到；
- 内核态模块、`ptrace` 附加：同上；
- 我们自己库的**文件名**：它在 `dlpi_name` 里被跳过，但 `/proc/<pid>/maps` 的
  inode 反查（`ls -l /proc/<pid>/map_files/…`）仍指向模块目录 —— 这条需要 root。

---

## 11. 进程内可检测面的第二轮收口

第 10 节把 `/proc` 与 loader 两条读路径关掉之后，进程内还剩三处能指向我们，本节处理这三处。

### 11.1 `xposed.dummy.*`：先试惰性，被真机打回，最后是改名

**问题**：`Startup.initXposed()` 里调了 `XposedBridge.initXResources()`，它 `buildDummyClassLoader(...)`
之后立刻 `loadClass("xposed.dummy.XResourcesSuperClass")`。于是**每个**被注入进程里都存在这个类，
检测方一行 `Class.forName("xposed.dummy.XResourcesSuperClass")` 就能判定框架在场 ——
这比读 maps 还便宜，而且我们的 maps 过滤对它完全无效。

**先试的改法（已回退）**：把定义推迟到 `XposedInit.hookResources()`，只在模块真用资源 API 时才建。
真机跑起来 30 分钟后拿到反证：

```
D/Vector ( 3431): java.lang.NoClassDefFoundError: Failed resolution of: Lxposed/dummy/XResourcesSuperClass;
D/Vector ( 3431): Caused by: java.lang.ClassNotFoundException: xposed.dummy.XResourcesSuperClass
```

ART 会把一次失败的解析记到**进程结束**（上游 `legacy/changelog.md` 为同一个原因踩过一模一样的坑），
所以「用到才定义」在真实 ROM 上等价于「很多进程里永远解析不了」，凡是走资源 API 的模块全废。
**已还原成启动期立即定义**：`Startup.initXposed()` 里的 `XposedBridge.initXResources()` 与那两行
`loadClass` 都回来了。

**最后用的改法：把类改名，而不是改时机。** 包 `xposed.dummy` → `rt.dummy`：

| 位置 | 改动 |
| --- | --- |
| `hiddenapi/stubs/src/main/java/rt/dummy/{XResourcesSuperClass,XTypedArraySuperClass}.java` | `package rt.dummy;`（目录一并移动，旧 `xposed/` 包已删） |
| `legacy/src/main/java/android/content/res/XResources.java` | 两条 `import rt.dummy.…` 与 `extends` 目标 |
| `legacy/.../XposedBridge.java` | `loadClass("rt.dummy.XResourcesSuperClass")` / `…XTypedArraySuperClass` |
| `native/src/jni/resources_hook.cpp` | `SEALED(kResourcesSuperClass)` / `SEALED(kTypedArraySuperClass)` 的值随生成表 |
| `debrand/seal_strings.py` | 同上两条值改成 `rt/dummy/…` |
| `legacy/consumer-rules.pro`、`legacy/README.md`、`native/include/jni/resources_hook.h` | 注释文字同步 |

这样两头都对：类仍然在启动期就定义好（不触发永久失败），
但 `Class.forName("xposed.dummy.XResourcesSuperClass")` 现在是 `ClassNotFoundException` ——
**那个名字下根本没有类**，不需要靠时机去躲。

### 11.2 剔除「匿名可执行」映射行

**问题**：即使把含特征 token 的行全部删掉，`maps` 里还剩一行 `rxp` 且路径列**为空**的区间 ——
那是 lsplant/Dobby 的 trampoline（`mmap(PROT_READ|PROT_WRITE|PROT_EXEC)`，没有名字）。
普通进程几乎没有这种行：JIT 缓存是 `[anon:dalvik-jit-code-cache]`，boot image、各 `.so` 全都有名字。
统计它就成了「有没有 inline hook」的廉价判据。

**改法**：`native/src/core/sanitizer.cpp` 新增 `IsAnonymousExecutable(line)`：只对 `maps` 形状的行
（首列必须是 `hex-hex` 地址区间、第二列是四个字符的权限且第 3 位为 `x`）判断，跳过
`offset/dev/inode` 三列后若路径为空则判为匿名可执行并**整行剔除**。`smaps` 的字段行
（`VmFlags:` 之类）没有地址区间，天然被排除。

**实测**（root 侧统计，注入进程 vs 非注入进程）：

```
pid=4581  ndroid.systemui    anon-exec: 1   token lines: 6   module mapped: 3
pid=5308  droid.bluetooth    anon-exec: 1   token lines: 6   module mapped: 3
pid=1722  Binder:android     anon-exec: 0   token lines: 0   module mapped: 0
```

注入进程正好多出 1 行匿名可执行，非注入进程为 0 —— 与自检差值从 `2505/2513` 变成
`2518/2527`（差 9 = 8 条 token 行 + 1 条匿名可执行行）一致。

### 11.3 混淆表的 key 不再以明文驻留

**问题**：`SettingsSource::obfuscation_map()` 是 `std::map<std::string, std::string>`，key 就是
`de.robv.android.xposed.`、`org.matrix.vector.core.` 这些原始前缀。这份表会从守护进程经
binder 传进**每一个**被注入进程并常驻堆里，内存扫描直接命中 —— 命名空间随机化在这里等于白做。

**改法**：新增 `native/include/core/name_table.h` 的 `NameTable`：

- key 进来就算 FNV-1a 64 位，只存哈希，原文不留；
- `Find(prefix)` / `At(prefix)` 用同样的哈希查，调用点传的是 `SEALED(kXxx)`（解出来是临时 `std::string`，
  用完即消除）；
- 值仍是随机名，留着无害。

改动覆盖 `core/config_bridge.h`（接口类型）、`jni/jni_bridge.h`、`jni/hook_bridge.cpp`、
`jni/resources_hook.cpp`、`zygisk/src/main/cpp/ipc_bridge.{h,cpp}`、`zygisk/src/main/cpp/module.cpp`。
运行时验证：`Enabled Modules: 1`、`GET_BINDER` 正常、被注入 app 起得来 —— 说明
`org.matrix.vector.core.`（入口类）与 `org.matrix.vector.service.`（bridge 服务）两次查表都命中了。

### 11.4 `.so` 里最后两个明文类名

`resources_hook.cpp` 里 `kResourcesSuperName` / `kTypedArraySuperName` 曾是裸字面量
`"xposed/dummy/XResourcesSuperClass"`，`GetXResourcesClassName()` 里也有一份
`"android.content.res.XRes"`。前者是 11.1 那个 PoC 的字符串版，后者是包名。

改为 `SEALED(kResourcesSuperClass)` / `SEALED(kTypedArraySuperClass)` / `SEALED(kXResKey)`
（`debrand/seal_strings.py` 里对应值也恢复成真正的 dummy 名，并补了 `kXResKey`、
`kXResClassName` 两条）。重新生成后 release `libzygisk.so` 复查：

```
xposed/dummy                    0
android.content.res.XRes        0
de.robv.android.xposed          0
org.matrix.vector.core          0
/data/adb                       0
```

### 11.5 仍然存在的（写清楚，避免以后再当漏改）

| 通道 | 现状 | 为什么留着 |
| --- | --- | --- |
| 裸系统调用 `syscall(SYS_openat, "/proc/self/maps", ...)` | **已消除**（见第 13 节） | `syscall` 被 Dobby 就地 hook，三个 open 变体走过滤，其余原样转发 |
| `ArtMethod` 入口点 / `kAccNative` 位 | 能判定"被 inline hook" | 这是 hook 引擎本身的代价；伪装入口点要跟每一版 ART 内部结构 |
| 内存里 `XposedBridge`、`XposedHelpers` 等 **simple name** | **已消除**（见第 12 节） | 加载进进程的 `core.dex` 里，`Lde/robv/android/xposed/…` 与 `Lorg/matrix/vector/impl/…` 两个包下的 52 个描述符被整体改名 |
| `core.dex` 里的 `rt/dummy/*` 描述符（2 处） | 命中 | 是 `XResources` 的父类引用常量，编译期绑定；名字已中性化，且不再是 `xposed` 字样 |
| root 从进程外读 maps / `map_files` | 可见 | 过滤层在目标进程里，管不到外部 |

### 11.6 真机结果

RMX3823 / Android 16 / KernelSU + Zygisk Next，装 `update-v1.0-3116-Release.zip` 后重启：

```
sys.boot_completed=1   bootanim=stopped   uptime 102s
logcat -b crash: 只有重启瞬间 init 自己的 SIGABRT
sanitizer: live handlers open=true openat=true fopen=true readlink=true exec=true phdr=true dladdr=true
sanitizer self-check: in-process view 2518 lines / 0 marked, raw 2527 lines / 8 marked
sanitizer: installed (15 tokens, full)
```

`cli status` = `Framework Version 1.0 / Version Code 3116 / API Version 102 / Enabled Modules 1`，
被注入进程（systemui、bluetooth）里模块映射仍在（root 侧 `module mapped: 3`）而进程内视图为 0。

---

## 12. 类名层：simple name 也不再是原文

第 11 节之后，进程内还剩下的最便宜的一条判据是**类名本身**：
框架 dex 里 `de.robv.android.xposed.XposedBridge`、`…XposedHelpers`、`…callbacks.XC_MethodHook`
这些字符串都在加载进内存的那份 dex 的字符串池里。检测方不需要读 maps、不需要 syscall，
只要在自己进程里 `Class.forName("de.robv.android.xposed.XposedBridge")` ——
而且更省事：直接 `strings` 自己的堆或扫自己的 dex 缓存就能命中。第 11 节之前这一条完全没堵。

### 12.1 机制：改描述符，不靠时机

改动落在 `daemon/src/main/jni/obfuscation.cpp` 的 `obfuscateDexBuffer()`。它本来只做**包前缀**等长随机化
（`Lde/robv/android/xposed/` → `LaB/cd/`），现在多了一层**按类逐个改名**：

```
simple_name_scopes = { "Lde/robv/android/xposed/", "Lorg/matrix/vector/impl/" }
```

对落在这两个包下的每个 `L…;` 描述符，把 `包前缀 + 类简单名` 整串换成
`已映射的前缀 + 同长度的随机名字`，命名由 `random_name_like()` 生成（保持长度，字母随机，
**保留 `$` 与 `/`**）。映射记在 `class_names`（完整旧描述符 → 完整新描述符）里，
所以同一个类无论出现在字符串池几次、出现在哪个 dex，都拿到同一个新名字。

`$` 必须保留：`XposedHelpers$1` 这类匿名/内部类的分隔符被破坏，JVM 就无法解析。
`/` 也必须保留：`de/robv/android/xposed/callbacks/XC_MethodHook` 里剩下的 `/` 是子包边界，
早期版本只保留 `$`、遇到 `/` 就跳过整条，结果 `callbacks` 包下的类全部漏改（学到的名字从 52 掉到 38），
真机上表现为「计数看着正常、其实少了一整个包」。

### 12.2 为什么只有这两个包

`Lorg/matrix/vector/core/`、`…nativebridge/`、`…service/` **故意不改简单名**：
native 侧是用「映射后的包前缀 + 固定类名」拼出来的
（`obfs_map.At(prefix) + "Main"` / `+ "ResourcesHook"` / `+ "BridgeService"`，见
`zygisk/src/main/cpp/module.cpp` 与 `ipc_bridge.cpp`）。把简单名也随机化，
native 就再也拼不出正确的类名，框架直接起不来。
这三个包的前缀仍然随机（namespace 级已经不可辨认），只是类名保持稳定。

### 12.3 点号形式要单独走一遍

同一份 `core.dex` 里既有 `Lde/robv/android/xposed/…;` 也有点号写法
（`legacy/.../VectorModuleClassLoader.kt` 的 `arrayOf("de.robv.android.xposed.")` 回落即一例，
它是用于 `blockLegacyApi` 前缀判断的）。所以遍历完描述符后，再按
`de.robv.android.xposed.` ↔ 随机点号前缀做一次替换。这次替换用的名字与描述符层**同源**，
所以判断逻辑与真实类名仍然对得上。

### 12.4 覆盖率的两种证据

离线（对 `zygisk/release/update-v1.0-3116-Release.zip` 里的 `framework/core.dex`，
用与 daemon 完全相同的算法模拟改写）：

```
descriptors rewritten: 52
  remaining XposedBridge             0
  remaining XposedHelpers            0
  remaining XC_MethodHook            0
  remaining de/robv                  0
  remaining de.robv                  0
  remaining org/matrix/vector/impl   0
```

其中 29 条是含 `$` 的嵌套类。在线（真机 `/data/adb/sysupd/log/*.log`）：

```
obfuscation: 53 descriptor(s) rewritten, 52 known name(s) (52 new)
obfuscation:  0 descriptor(s) rewritten, 52 known name(s) ( 0 new)   ← 之后的每个模块 dex
```

53 > 52 是因为同一描述符在字符串池里出现了两次（数组/内联各一次），去重后是 52；
后续模块 dex 只查表、不新增名字，说明「学一次、全局一致」成立。

想从被注入进程里 dump 改写后的 dex 做第三方复核走不通：
`InMemoryDexClassLoader` 的 fd 在 async 加载后已经关闭，`/proc/<pid>/fd` 里既没有 `memfd` 也没有 `ashmem`。
所以复核靠上面两条：离线算法复现 + 运行时计数。

### 12.5 兼容性

- 模块**不需要**改动。模块自己的 dex 也走同一个 `obfuscateDexBuffer`，
  模块里 `import de.robv.android.xposed.XposedBridge` 的调用点是**编译器内联的字符串**，
  同一个字符串池、同一次改写，因此仍然解析得到。
- 但模块若把类名当**字符串字面量**硬编码（`Class.forName("de.robv.android.xposed.XposedBridge")`
  或反射拼包名），改写同样会命中那个字面量 —— 结果是模块自己 `ClassNotFoundException`。
  这是本次改动唯一需要模块作者知情的点：**不要反射 API 类**，正常 `import` 即可。
- 按真名反射的第三方工具（含部分检测库）现在会失败，这正是目的。
---

## 13. 最后一条便宜的通道：`syscall()` 本身

第 10 节把 `open`/`openat`/`fopen`/`readlink`/`execve` 的 GOT 槽改掉了，第 12 节把类名换掉了。
但检测方还有一条完全不同的路：不碰 libc 的封装，直接

```c
int fd = syscall(__NR_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY);
```

GOT 里没有它的槽 —— 调用者查的是 `syscall` 这个符号，而 bionic 自己的 `open()`/`openat()` 实现里
是直接 `svc #0`，也不经过 `syscall()`。所以这条路径既不经过 PLT，也不经过任何 libc 包装。

### 13.1 上游没有这个东西

先说清楚：**上游 Vector / LSPosed 没有任何 `syscall` 钩子，也没有 `/proc` 过滤**。
全仓 `syscall(` 只有两处，都是 `memfd_create`（`native/src/core/sanitizer.cpp`、`dex2oat/src/main/cpp/dex2oat.cpp`）；
`git grep` 到 `HEAD` 同样如此。上游对 `/proc/self/maps` 唯一的表态是 `native/src/elf/elf_image.cpp:374`
那句注释 —— "it does not read `/proc/self/maps`, a read that is itself detectable"，即**刻意避开**，不是隐藏。

所以「直接用原版的」在 `syscall` 这件事上不成立；能直接用原版的是**仓库里已经编进来的 Dobby**：
`native/include/core/native_api.h:117` 的 `HookInline(original, replace, &backup)` 就是 `DobbyHook` 的包装，
lsplant 的 `inline_hooker` 用的也是它（`zygisk/src/main/cpp/module.cpp:118-131`）。
零新增依赖、零新增 CMake 目标。

### 13.2 实现

`native/src/core/sanitizer.cpp` 新增：

- `syscall_fn real_syscall`（`long (*)(long, ...)`）；
- `WatchedSyscall(number, a1, a2, a3, &path, &close_on_exec)` —— 只认三个号：
  `__NR_openat`（path=a2, flags=a3）、`__NR_openat2`（path=a2, flags 取 `struct open_how` 首个字）、
  `__NR_open`（x86/arm32 专有，path=a1, flags=a2）；相对路径一律放行（要解析 `dirfd` 才认得，不值当）；
- `SyscallImpl(number, ...)` —— `va_arg` 取六个字，命中且有绝对路径时返回 `CreateSnapshotFd()`，
  否则 `return real_syscall(number, a1..a6)` 原样转发；
- `InstallSyscallHook()` —— `dlsym(RTLD_DEFAULT, "syscall")` → `HookInline` → `real_syscall = backup`。

三个必须处理的细节：

| 细节 | 处理 |
| --- | --- |
| 递归 | `CreateSnapshotFd()` 自己用 `syscall(__NR_memfd_create, …)` 建 memfd，钩子装好后这会再进一次我们的 wrapper；改成优先走 `real_syscall` |
| 变参宽度 | 一律 `va_arg(unsigned long)`；aarch64 可变参数走 64 位寄存器，32 位 ABI 下 `unsigned long` 也正好是寄存器宽度 |
| 只有 `open*` 被拦 | 其余 syscall 一次比较就转发，`syscall()` 被 libc 内部调用的频率极低（wrapper 自己都是 `svc #0`），开销可以忽略 |

顺带修掉一个自己写出来的 bug：`UnpatchAll()` 之前只 `g_patches.clear()`，**没有真正写回原值**，
所以安装失败时那句 "restored the original GOT entries" 是假的。现在会按 `GotPatch::original` 逐槽写回，
并 `UnhookInline(g_syscall_target)`。

### 13.3 验证

自检里直接加了一条「检测方视角」的读取：用 `syscall(__NR_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY)`
打开、读全量、逐行统计 token。真机（RMX3823 / Android 16 / KernelSU + Zygisk Next）重启后：

```
sanitizer: live handlers open=true openat=true fopen=true readlink=true exec=true phdr=true dladdr=true syscall=true
sanitizer self-check: in-process view 2520 lines / 0 marked, raw 2529 lines / 8 marked
sanitizer self-check: raw-syscall view 2521 lines / 0 marked
```

三行放在一起才有意义：`raw` 是绕过所有钩子的真实视图（8 条命中，说明过滤对象确实存在），
`in-process` 与 `raw-syscall` 都是 0 —— 一条走 GOT，一条走裸系统调用，两条路都被堵住了。
同一次开机 `logcat -b crash` 的 `signal 11` 计数为 **0**，`cli status` = `Enabled Modules: 1`。

### 13.4 还剩什么

| 通道 | 现状 | 说明 |
| --- | --- | --- |
| `ArtMethod` 入口点跳转、`kAccNative` | 可检测 | inline hook 的固有痕迹；伪装要跟每一版 ART 内部布局 |
| 被 hook 方法内 `Thread.getStackTrace()` | 可检测 | 栈上会出现随机包名的框架帧；名字随机了，但"陌生包"仍是信号 |
| root 从进程外读 `maps` / `map_files` | 可见 | 过滤层在目标进程里 |
---

## 14. 以蜂鸟众包 10.11.2 的检测清单为标尺对账

标尺文件：`fengNiao/RiderConsole/docs/DETECTION_SIGNATURES_10.11.2.md`（`me.ele.crowdsource`
10.11.2 的三套独立检测体系 A/B/C）。下面按它的每一类判据说清"能不能去、怎么去的、
哪些去不掉"。**结论放在最前：它能判定的"框架身份"部分已经全部失效；它判定的
"hook 这个动作"和"设备环境"部分与本框架无关，属于另一层。**

### 14.1 A 面 `DangerChecker`（Java，10 个维度）

| 判据 | 它怎么做 | 现在的结果 |
| --- | --- | --- |
| `recognize/Xposed` → `hasXposedBridge` / `hasXposedHelpers` / `hasDexposedBridge` | `Class.forName("de.robv.android.xposed.XposedBridge")` 等三次反射 | **失效**。框架 dex 加载时 52 条 `Lde/robv/android/xposed/…`、`Lorg/matrix/vector/impl/…` 描述符被就地改名（第 12 节），反射拿到 `ClassNotFoundException` |
| `hasXposedStackTrace` | 主动抛异常，查栈帧里有没有 `handleHookedMethod` / `com.saurik.substrate.MS$2` / `com.saurik.substrate` | **失效**。这三个串在 `core.dex` 与 4 个 ABI 的 `libzygisk.so` 里都是 0；lsplant 生成的 backup 类前缀已改成 `O_`（原 `Vector_`、上游 `LSPHooker_`） |
| `hasXposedProcess` | 读 `/proc/<pid>/maps` 找 `XposedBridge.jar` / `.jar` / `.so` | **失效**。进程内 `maps` 已被过滤：`self-check: in-process view 2512 lines / 0 marked`，裸 `syscall` 视角同样 `0 marked`（第 13 节） |
| `recognize/VirtualHook` 四类 | `ActivityThread.currentActivityThread`、`ActivityManagerNative.getDefault`、`Handler.mH.mCallback`、`Context.getBasePackageName/getOpPackageName`，外加 `Proxy.isProxyClass` 与"方法名字符串符合预期" | **本框架不触碰这些点**：`mH.mCallback` 全仓零安装、不用动态代理。中不中取决于加载的**模块**有没有改这些方法 —— 这是模块的配置问题，不是框架特征 |
| `recognize/BuildInfo`、`Qemu`、`Hardware`、`DangerApp` | `Build.TAGS`/`FINGERPRINT` 关键字、`/dev/qemu_pipe` 等 6 个节点、`/system/bin/cat /proc/cpuinfo`、服务端下发危险包名 | **与本框架无关**。真机上天然不中；root 六项、`is_adb`、代理检测属 root 隐藏层（Shamiko 一类），不在本仓库职责内 |

### 14.2 B 面 Quicksparrow（阿里 EMAS，native，612 个上报字段）

| 字段 | 它怎么做 | 现在的结果 |
| --- | --- | --- |
| `self_maps` / `maps_so` / `maps_from_native` / `so_bases` / `hook_in_map` | 读 `/proc/self/maps`，按 `xposed` `libdexposed` `substrate` `sandhook` `libriru` `frida` `libpaganini.so` 等子串与 `rwx` 段判据统计 | **失效**。路径子串全在过滤器里（含 `/data/adb`、`zygisk`、`lsplant`、`dobby`、`xposed`、`lspatch` 等）；`rwx` 页即使出现也会被改写成 `r-x` 展示（`MaskWritableExecutable`），自检三行都是 `0 marked / 0 rwx` |
| `native_hookCheck` / `SS_HOOK_MAPS_RWX` | 只找同时 `rwx` 的映射段（inline hook 框架会在 `libart.so` 附近留这种页） | **同一条过滤**：`rwx` 的 `w` 位在读取侧被抹掉。Dobby 确实用 `mprotect(PROT_READ|PROT_WRITE|PROT_EXEC)`（`code-patch-tool-posix.cc:17/19`），但实测本机进程 `raw 2520 lines / 0 rwx` —— 已无遗留 RWX 页，这条属防御性加固 |
| `xhook_count` / `xhook_d_count` | 数它**自己** xHook 表里的项 | 与本框架无关（它 hook `open`/`stat`/`ptrace` 自保，统计的是自己的账本） |
| `method_to_native` / `mthook_file` / `mthook_sign_maps` | 比对 `ArtMethod` 入口点是否被改成 native、再与 maps 交叉 | **挡不住**（结构性）。inline hook 的固有痕迹，见 13.4 |
| `xpose_field` / `class_list` / `java_vm` | 在目标 classloader 里枚举类、看加载来源 | **挡不住**。模块 apk/dex 的路径本来就以明文存在于 `DexPathList` 里 |
| `package_list` / `app_count` / `app_file_sign` | 上报所有已安装包名与签名 | 属模块打包层：模块以普通 APK 存在，包名可见（本框架自身**不**安装成包） |
| `debug` / `anti_debug` / `GetTracerPid` | 读 `/proc/self/status` 的 `TracerPid`、调试端口 | 与本框架无关（我们没有调试器附着） |
| `root` 六项、`is_virtual`、`multi_open`、`net_proxy`、`vpn_status` | 招牌式 root/多开/代理检测 | 与本框架无关，属 root 隐藏与网络层 |

它自己也是 hook 使用者：`libalisecuritysdk.so` 里有 `libxhook 1.2.0 (aarch64)`、`Java_com_qiyi_xhook_NativeHandler_*`、`"hooking %s in %s"`、`ptrace`。这一点不影响我们 —— 我们的过滤层在它之下（GOT 表项里它调到的 `open`/`fopen` 已指向我们），它拿到的仍是过滤后的视图。

### 14.3 C 面 `black_crash`（UC crashsdk 黑名单）

黑名单原文（`smali_classes3/v31/a.smali:9`，`#` 分隔逐项 substring 匹配）：
`com.hy.clone`、`com.qihoo.magic`、`com.morgoo.droidplugin`、`com.qh.privacysec`、
`com.zb.xapp`、`xfsplugin`、`svmplugin`、`.xposed`、`LSPHooker`、`LSPosedBridge$NativeHooker`、
`LSPosed-Bridge`、`lspatch`、`LSPatch`、`com.fengniao.location.patch`。

四个开关默认全 true，但它**只打标不上杀**（四个包内 `killProcess`/`System.exit` 零命中），打标进 ANR/卡顿通道随 `HBBlackRecordModel` 上报 13 个字段。

本轮把最后一处运行期可见的命中清掉了：

| 串 | 原来在哪 | 处理 |
| --- | --- | --- |
| `LSPHooker` | `lib/<abi>/libzygisk.so` 各 1 处，来自 `external/lsplant/lsplant/src/main/jni/include/lsplant.hpp:67` 的 `std::string_view generated_class_name = "LSPHooker_";`（默认成员初始化器进了 `.rodata`；运行期本框架在 `InitInfo` 里覆盖成 `O_`，所以这份副本从不使用） | `debrand/patch_so.py` 增加等长替换 `LSPHooker_` → `L5PHooker_`；重建后 4 个 ABI 全部 0 |
| `.xposed` / 其余黑名单串 | 只剩磁盘上的 `core.dex`（`de.robv.android.xposed.` 点号串，加载时由 `dotted` 表改写）、`daemon.apk`、`manager.apk` —— 都是只有 root 能读的文件 | 不处理 |

### 14.4 顺带修掉一个我自己引入的崩溃（重要）

加完 `rwx` 改写后装机，system_server 立刻崩：

```
F/libc: Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0 in tid system-server-i
F/libc: Fatal signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), fault addr 0x7c87c0ffe0 in pid com.android.phone
#00 pc <libzygisk.so> 0x8c2d4   paciasp / str x30, [sp, #-0x40]!    <- 函数序言写栈
```

用 `touch /data/adb/modules/perf_patch/disable` + 重启做对照：崩溃消失 → 是本模块导致。

根因不在 `rwx` 改写，而在 `PermsForSlot(info, slot, PROT_READ)` 的**回退值**：某个
`r_offset` 被解析到 image 之外时，它返回 `PROT_READ`，`PatchSlot` 就把那个页
`mprotect` 成可写、写完再设成**只读** —— 那个页可能正是别人的栈或堆，于是下一次写就
`SEGV_ACCERR`，同时被覆盖的旧值也会破坏原数据（对应 `free` 里的
`Pointer tag ... was truncated` abort）。

修法：`PermsForSlot` 找不到所属 `PT_LOAD` 时返回 `0`；`PatchSlot` 见 `0` 直接拒绝；
`PatchImage` 跳过并计数，`grep skipped` 可以看有没有遇到越界槽（本次开机为 0，说明是
低概率事件，正好解释了"有时开机崩、有时不崩"）。

### 14.5 现在的成绩单

```
sanitizer: live handlers open=true openat=true fopen=true readlink=true exec=true phdr=true dladdr=true syscall=true
sanitizer self-check: in-process view 2512 lines / 0 marked / 0 rwx, raw 2520 lines / 8 marked / 0 rwx
sanitizer self-check: raw-syscall view 2512 lines / 0 marked / 0 rwx
signal 11 count = 0 (uptime 163s)   cli status = Enabled Modules: 1
```

一句话：**这份清单里所有"认出是哪个框架"的判据都已经打空，被注入进程自己也照不出来；
它剩下的必中项要么是"hook 这个动作"的物理痕迹（`ArtMethod`、`DexPathList`），要么是
设备环境（root/模拟器/代理），后者需要另一套 root 隐藏方案。**

### 14.6 第二个崩溃根因：钩子被自己递归（14.4 之后才发现）

14.4 修完越界槽之后仍然时不时崩，而且形状变了：

```
F/libc: Fatal signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), fault addr 0x730d9a1fe0 in tid PackageManager
F/libc: Fatal signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), fault addr 0x7c87c0ffe0 in tid ndroid.systemui
F/DEBUG #00 je_tsd_init_finish+56 (libc)  #01 je_malloc+708  #02 malloc+48
F/DEBUG #03 0x8c4c4 libzygisk.so          #04 0x46ca8 libzygisk.so   #05 pthread_mutex_unlock
```

两类形状指向同一件事：`fault addr` 以 `fe0`/`ff0` 结尾 = 写到了**栈的守护页**（栈溢出），
以及分配器在自己的线程状态初始化里空指针。栈里出现我们的库，是因为**我们的处理函数
在分配内存**，而分配器的慢路径又会回到被我们改写的 `open`/`openat` 上 —— 于是
`open` → `std::string` → `malloc` → … → `openat` → `open` → … 无限递归，直到栈耗尽。

A/B 证据链（全部在同一天、同一台设备上）：

| 配置 | `signal 11` |
| --- | --- |
| 整个模块 `disable` | 140 秒内 **0** |
| 模块启用 + `no_sanitizer` 标记（过滤层整段不安装） | 90–140 秒内 **1–3** |
| 模块启用 + Debug 包（`isDexObfuscateEnabled = false`，类名不改写） | 140 秒内 **3** |
| 模块启用 + Release 包（过滤层全开，未加递归保护） | 140 秒内 **2** |
| 模块启用 + Release 包 + **递归保护** | 256 秒内 **0** |

结论：跟类名改写无关（Debug 一样崩），跟"过滤层是否存在"也无关（标记关掉照样崩）——
崩的原因是**处理函数被重入**这条路径本身，关掉过滤层只是让它以另一种方式触发
（`PinLibrary`/`dladdr` 那侧同样会分配内存）。也就是说 14.4 的越界槽只是第一个根因。

修法（`native/src/core/sanitizer.cpp`）：

```cpp
struct HookDepth {
    HookDepth() { ++g_bypass_depth; }
    ~HookDepth() { --g_bypass_depth; }
};
```

每个处理函数在做任何事之前先看 `g_bypass_depth`：非零就**直接转发给保存的原函数**；
命中受监视路径后再用 `HookDepth` 把整段处理标上。这样"过滤层自己的库调用"永远走原路，
不会再回到自己身上。`syscall`、`readlink`/`readlinkat`、`execve`、`dl_iterate_phdr`、`dladdr`
一并套用。

修完的实测（Release 包，作用域恢复成 7 个应用：`android`、`bluetooth`、`phone`、
`systemui`、`gms`、`system`、`com.tdx.tuying`）：

```
uptime 256s   dexopt-svc 3226   system_server 3705   systemui 4668
logcat -b crash: signal 11 = 0（只剩每次开机都有的 init SIGABRT）
sanitizer: live handlers open=true openat=true fopen=true readlink=true exec=true phdr=true dladdr=true syscall=true
sanitizer self-check: in-process view 2518 lines / 0 marked / 0 rwx, raw 2526 lines / 8 marked / 0 rwx
sanitizer self-check: raw-syscall view 2518 lines / 0 marked / 0 rwx
cli status: Version Code 3116 / API 102 / Enabled Modules 1
```

**教训**：写"改写系统调用"的代码时，第一件事是证明它不会重入自己；`g_bypass_depth` 之类
的标记必须在**进入处理函数时就置位**，而不是在查完路径之后。否则最平常的一次
`malloc` 就能把进程带进无限递归，而崩溃现场看起来像分配器的 bug。

## 15. 冲压测试：偶发 SIGSEGV 的第三个根因（emulated TLS）

第 14.6 节修掉重入之后，`signal 11` 从"每个注入进程必崩"降到"偶发"，但仍没归零。为了把
"偶发"变成可复现的数字，我用了同一套冲压协议：清空 crash 缓冲 → 连续三轮
`killall com.android.systemui / gms / phone`（每轮睡 45 s）→ 计 `Fatal signal 11` 条数。

### 15.1 四组对照（同一台机器，同一天，协议完全相同）

| 组 | 模块 | 过滤层 | 结果 |
|---|---|---|---|
| 1 | 关闭（`disable` 标记） | — | 3 轮 **0** |
| 2 | 开启 | 关闭（`no_sanitizer`） | 出现 2 条，随后缓冲回卷到 0 |
| 3 | 开启 | 开启（旧构建） | 第 1 轮 0、第 2 轮崩 1（systemui `ImageDecompress`，`SEGV_MAPERR addr 0x0`） |
| 4 | 开启 | 开启（本节修复后） | 3 轮 **0** + 追加 5 轮 **0**（见 15.4） |

组 2 的结论很重要：**把过滤层整个关掉，崩溃仍然出现**，所以这不是"过滤逻辑写错了"。组 1 与
组 2/3 的差别只剩"有没有注入 `libzygisk.so`"，于是下一步去看 tombstone，而不是继续读代码。

### 15.2 tombstone 指向的位置

`/data/tombstones/tombstone_00` 与 `tombstone_31`（都是 `com.google.android.gms`）：

```
#00 pc ... libc.so (je_tsd_init_finish+56)
#01 pc ... libc.so (je_malloc+708)
#02 pc ... libc.so (malloc+48)
#03 pc ... /data/adb/modules/perf_patch/zygisk/arm64-v8a.so
#04 pc ... /data/adb/modules/perf_patch/zygisk/arm64-v8a.so
```

库本身被 strip 过（只剩 7 个导出），但 `zygisk/build/symbols/<abi>/zygisk.debug` 留着完整
调试信息。用 `llvm-symbolizer --obj=zygisk.debug` 解析这两个偏移：

```
0x8c814 -> __emutls_get_address      emutls.c
0x46fa8 -> sys::rt::sanitizer::(anonymous namespace)::SyscallImpl(long, ...)  sanitizer.cpp:938
```

也就是说调用链是 **我们的 `SyscallImpl` → `__emutls_get_address` → `malloc` → jemalloc 的
TSD 初始化 → 崩**。`__emutls_get_address` 是**模拟 TLS** 的实现：`thread_local` 变量在某个
线程上第一次被访问时，它要 `malloc` 一块控制块。而 `SyscallImpl` 恰恰是在 `open/openat`
里跑的 —— 也就是分配器自己的慢路径上。于是出现"过滤层在分配器内部触发分配"的死循环窗口，
崩溃点落在分配器里，看起来像 jemalloc 的 bug。

`g_bypass_depth` 与 `g_in_loader_hook` 两个 `thread_local` 正是触发点：
`llvm-nm zygisk.debug | grep emutls` 能看到 `__emutls_v._ZN...g_bypass_depthE`。

### 15.3 修法：过滤层不再用任何 `thread_local`

尝试过的两条路都走不通，记录在此以免重犯：

| 尝试 | 结果 |
|---|---|
| `-fno-emulated-tls`（改用真 TLS） | 链接失败：`ld.lld: error: undefined symbol: __tls_get_addr`（NDK stub 在此 API 级别不导出） |
| `-ftls-model=initial-exec` | 编译通过、链接通过，但**符号没变** —— 模拟 TLS 下 `tls_model` 无意义，`__emutls_v.*g_bypass_depth` 仍在 |

最终做法：把两个标记搬进一张以**线程指针**为键的定长表，键用 `pthread_self()` 读（这是读
线程指针，不分配、不进分配器）：

```cpp
constexpr std::size_t kGuardSlots = 1024;
constexpr std::size_t kGuardProbes = 16;

struct GuardSlot {
    std::atomic<unsigned long> owner{0};
    int depth = 0;
    int loader_depth = 0;
};

GuardSlot g_guard_slots[kGuardSlots];

GuardSlot *FindGuardSlot() {
    unsigned long key = static_cast<unsigned long>(static_cast<unsigned long long>(pthread_self())) >> 4;
    if (key == 0) key = 1;  // 0 marks a free slot.
    for (std::size_t probe = 0; probe < kGuardProbes; ++probe) {
        GuardSlot &slot = g_guard_slots[(key + probe * 97) % kGuardSlots];
        if (slot.owner.load(std::memory_order_acquire) == key) return &slot;
        unsigned long expected = 0;
        if (slot.owner.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) return &slot;
    }
    return nullptr;  // 表满时选择“不守护”，而不是失败。
}
```

对外的形状保持不变：`GuardDepth()/GuardEnter()/GuardLeave()` 与
`LoaderDepth()/LoaderEnter()/LoaderLeave()`，`struct HookDepth`、`struct LoaderHookGuard`、
`ScopedBypass` 三个 RAII 类型的实现改调这几个函数，所有调用点一行没动。表满（1024 个线程
槽、16 次线性探测都没抢到）时退化为"不守护"，代价是那一瞬间多露几行 maps，不会崩。

改完后 `llvm-nm zygisk.debug`：`g_bypass_depth` / `g_in_loader_hook` 消失，只剩
`g_guard_slots`（普通 `.bss`），`emutls` 计数从 12 降到 10 —— 剩下的来自 libc++ 的
`eh_globals` 之类，与过滤路径无关。

### 15.4 修复后的数字

```
sys.boot_completed=1   uptime 272 s（三轮冲压后）
sanitizer: live handlers open=true openat=true fopen=true readlink=true exec=true phdr=true dladdr=true syscall=true
sanitizer self-check: in-process view 2521 lines / 0 marked / 0 rwx, raw 2529 lines / 8 marked / 0 rwx
sanitizer self-check: raw-syscall view 2521 lines / 0 marked / 0 rwx
sanitizer: installed (15 tokens, full)
Fatal signal 11: 0（三轮冲压）；追加五轮 killall + monkey 启动切页，仍 0
cli status: Version Code 3116 / API 102 / Enabled Modules 3
```

**教训**：在 `open/openat` 这类分配器必经之路上跑代码时，`thread_local` 不是免费变量 ——
在这套工具链上它等价于"第一次访问就可能 malloc"。要么用查表，要么先证明那条路上不会分配。
`llvm-symbolizer` 配上 `build/symbols/<abi>/*.debug` 能在库被 strip 之后仍然把偏移还原成
函数名和行号，这一步比读代码快得多。

---

## 16. 成员名层（A 组，2026-11）

### 16.1 为什么改一个字符串就够了

DEX 的字符串池是一个**去重集合**：同一段文本只存一份，`method_id` / `field_id` 用索引指向它。
所以把 `beforeHookedMethod` 那一项原地改成同长度的随机串，定义、全部调用点、以及
`getDeclaredMethod("beforeHookedMethod", …)` 里的**字面量**一起变 —— 它们本来就指向同一个字符串。

这一条决定了整个实现形状：**Java 侧不需要第二处改动**，模块 dex 与框架 dex 用同一张表，
模块里 `override fun beforeHookedMethod(...)` 的覆写名也跟着变，而虚分派走的是 method id，
不认名字。

必须留在 native 侧查表的只有三处，因为它们的文本写在 `.so` 里而不是 dex 里：

| 位置 | 原文 | 处理 |
|---|---|---|
| `native/src/jni/hook_bridge.cpp:357` | `GetMethodID(hooker, "callback", …)` | `ResolveMemberName("callback")` |
| `native/src/jni/hook_bridge.cpp:1106` `RegisterHookBridge` | `gMethods[]` 13 条 `VECTOR_NATIVE_METHOD` | 注册前逐条 `entry.name = ResolveMemberName(entry.name)` |
| `native/src/jni/resources_hook.cpp:151/159` | `"translateResId"` / `"translateAttrId"` | `ResolveMemberName(...)` |

### 16.2 13 条清单（原文 15 条，已收窄，见 16.7）

`findAndHookMethod`、`hookAllMethods`、`beforeHookedMethod`、`afterHookedMethod`、
`invokeOriginalMethod`、`invokeOriginal`、`replaceCallback`、`unhookMethod`、`deoptimizeMethod`、
`callbackSnapshot`、`findStaticInitializer`、`legacyApiPrefixes`、`translateResId`、
`translateAttrId`、`callback`。

两条硬约束写在 `obfuscation.cpp` 的注释里：

1. **整串匹配**。类名层用 `strstr` 子串替换，成员名不能照抄 —— `"callback"` 是
   `"callbackSnapshot"` 的子串，子串替换会把后者改成 `随机串Snapshot`，JNI 表里就没有那条了。
   实现改成「先比长度、再 `memcmp`」。
2. **`callback` 不能漏**。它被 `xposed/consumer-rules.pro` 的
   `-keepclassmembers class …VectorNativeHooker { … callback(java.lang.Object[]); }` 钉住，
   运行期 trampoline 靠这个名字调回来。

### 16.3 `ResolveMemberName` 为什么要驻留字符串

`jni_bridge.h` 里的实现把命中的映射值 `emplace_back` 到一个函数内 `static std::deque<std::string>`
再返回 `c_str()`。不能直接返回 `map->c_str()`：映射表在收到新表时会重建，而 `gMethods[]` 要活到
进程结束，悬空指针会让 `RegisterNatives` 之后的每一次调用都跳到野地址。用 `deque` 而不是
`vector`，是因为要的是「元素地址永不失效」，不是「连续内存」。

### 16.4 开关语义

- 开关 = `/data/adb/sysupd/rename_members` **存在即开**，默认关。
- 为什么默认关：改名只保证「同一个 dex 里自洽」。模块若把名字写在 **dex 之外** —— `.so` 里
  `GetMethodID(env, cls, "beforeHookedMethod", sig)`、`assets/` 里的配置文件、运行时用几段
  拼出来的名字 —— 就会查不到成员。这类模块不多，但一旦碰上就是 `NoSuchMethodError`，
  而它发生在别人的应用里，排查成本极高。
- 决策点每进程只定一次：`getSignatures(flag)` 与 `obfuscateDex(memory, flag)` 都带这个参数，
  谁先到谁定调，后来的不一致只打 `LOGW`。改 marker 需要重启 daemon（表建好就发给所有进程）。
- 类名前缀映射与成员名共用同一个 `Map<String,String>` 返回给 Java，`FileSystem.loadModule`
  里用 `it.key.contains('.')` 把成员名滤掉 —— 成员名不含点，若参与 `startsWith` 类名匹配，
  会把模块入口类改成乱码。

### 16.5 真机数据（RMX3823 / Android 16 / KernelSU + Zygisk Next）

关（marker 不存在）：

```text
obfuscation: 53 descriptor(s) rewritten, 0 member name(s) rewritten, 52 known name(s) (52 new)
git 包内 core.dex 仍含 15 个名字各 1 处
cli status: Enabled Modules 3；signal 11 = 0
```

开（`touch /data/adb/sysupd/rename_members` + 重启）：

```text
obfuscation: member renaming enabled for 15 name(s)
obfuscation: 53 descriptor(s) rewritten, 15 member name(s) rewritten, 52 known name(s) (52 new)
模块 dex：0 descriptor(s) rewritten, 1~6 member name(s) rewritten, 52 known name(s) (0 new)
```

- `15/15` 全部命中，模块 dex 复用同一张表（`0 new` 说明学习只发生在框架 dex）。
- 运行期证据：某应用栈帧出现 `at nDgieJCqblD.L.Ib.ir.AjPRV.tIBW.HookBridge.yClzHahyhPLpSRPOnyrL(Native Method)`
  —— 包前缀随机、成员名随机，且这条是 **native 方法**，说明 `RegisterNatives` 用新名字注册成功、
  `ResolveMemberName` 的解析路径通了。
- `logcat -b main | grep -cE 'NoSuchMethodError|NoClassDefFoundError|ClassNotFoundException'` = 1，
  内容是 `tencent.mm:push: ThrowNoClassDefFoundError when open swordfish`（微信自己的日志，与本框架无关）。
- 三轮冲压（每轮 `killall systemui gms phone` + 45 s）：`signal 11 = 0/0/0`，uptime 160→250 s，
  `dexopt-svc` / `system_server` / `systemui` 全程存活。
- 未验证：资源 hook 的 `translateResId` / `translateAttrId` 路径（当前 3 个启用模块都不声明
  资源 hook，`hookResources()` 未被调用）。

### 16.6 残留

`HookBridge.hookMethod` 等 13 个 JNI 方法名在 `.so` 的 `JNINativeMethod` 表里仍是明文。它们在
`/data/adb/modules/perf_patch/` 下，普通应用既无权限读、也不会出现在自己的 maps 里；要彻底消掉
需要把 `gMethods[]` 改成运行时用 `SEALED(...)` 拼表（13 条密封串 + 一次 `std::string` 生命周期
管理），风险大于收益，暂不做。

---

## 17. 审查轮：五处加固（2026-11）

对着代码逐函数读了一遍，找到四处**功能/健壮性**问题和一处**可省的开销**，全部已改并真机验证。

### 17.1 32 位进程的过滤是空转（真 bug，已修）

`PatchImage` 里 `const size_t symbol_index = ELF64_R_SYM(reloc_info);` 是无条件写的。32 位下
`ElfW(Xword)` 就是 `uint32_t`，`ELF64_R_SYM` 等于右移 32 位 —— 对 32 位类型是未定义行为，实际编译
结果是 0。于是**每一个**重定位都被拿去和符号 0 比较，一个槽也不会被改写：`zygote64_32` 设备上
所有 armeabi-v7a 进程等于完全没装过滤，`self-check` 也只会显示 `open=false` 之类。

修法：

```cpp
#if defined(__LP64__)
        const size_t symbol_index = static_cast<size_t>(ELF64_R_SYM(reloc_info));
#else
        // 32 位 r_info 把符号索引放在高 24 位；用 64 位宏等于把 32 位值右移 32 位。
        const size_t symbol_index = static_cast<size_t>(ELF32_R_SYM(reloc_info));
#endif
```

真机证据（`com.amap.ams` 是设备上唯一的 armeabi-v7a 应用，临时加进作用域后拉起）：

```text
pid=17122
/apex/com.android.runtime/lib/bionic/libc.so      <- 32 位运行时
/data/adb/modules/perf_patch/zygisk/armeabi-v7a.so
sanitizer: live handlers open=true openat=true fopen=true readlink=true exec=true phdr=true dladdr=true syscall=true
sanitizer self-check: in-process view 2575 lines / 0 marked / 0 rwx, raw 2585 lines / 9 marked / 0 rwx
sanitizer self-check: raw-syscall view 2576 lines / 0 marked / 0 rwx
```

修之前这个进程只会看到全部 `false`。

### 17.2 64 位拼写的 open 家族漏网（真 bug，已修）

bionic 把 `open64` / `openat64` / `fopen64` 导出成**独立符号**（`llvm-readelf` 读到
`open64` 与 `open` 同为 `0x96fcc`、`openat64` 与 `openat` 同为 `0x9728c`、`fopen64` 与 `fopen`
同为 `0xa9024`）。独立符号就有自己的 PLT 槽，只按 `open`/`openat`/`fopen` 三个名字匹配时，
任何 import 了 64 位拼写的库都还要读真文件。三条已加进 specs（backup 复用同一指针，因为地址相同）。

顺带加了「backup 为空就不装」的判断：`dlsym` 若拿不到真函数，装了也只是把调用变成 `ENOSYS`。

### 17.3 PT_DYNAMIC 扫描没有上界（加固）

原来 `for (const ElfW(Dyn) *entry = dynamic; entry->d_tag != DT_NULL; ++entry)` 一路走到
`DT_NULL`。白名单包含 `/data/app/`，也就是**任何已安装应用的 .so**：一个故意不带终止项的镜像
会让注入阶段读到映射页之外。现在用 `PT_DYNAMIC` 的 `p_memsz / sizeof(ElfW(Dyn))` 作上界。

### 17.4 `dlsym` 里的模块入口符号（补齐，但要说清它挡住了什么）

`zygisk_module_entry` 必须导出（loader 靠它找模块），动态符号表映射后只读，抹不掉。于是把
`dlsym` / `dlvsym` 也挂上：名字里含 `zygisk` 一律返回空。这套工具链上它多半是**防御性**的 ——
真机自检写的是 `module entry symbol hidden=true present=false`：`present=false` 说明
`real_dlsym(RTLD_DEFAULT, "zygisk_module_entry")` 本来就取不到，Zygisk Next 把模块库加载在
**局部**作用域里，`RTLD_DEFAULT` 搜不到它。所以这条挡的是「用 `dlopen` 拿到句柄再 `dlsym`」
以及将来 loader 改用 `RTLD_GLOBAL` 的情况。

### 17.5 `ContainsToken` 不再每行分配（开销）

原来每读一行 `maps` 就构造一个 `std::string` 做小写化；2500 行的 `maps` 就是 2500 次分配，而
这些分配发生在**分配器慢路径**上（`open` → 快照 → 分配），正是第 15 节那个崩溃的土壤。改成
512 字节栈缓冲 + 原地小写，超长行才回落到 `std::string`。

### 17.6 复测

```text
sys.boot_completed=1，uptime 90 s
sanitizer: live handlers open=true openat=true fopen=true readlink=true exec=true phdr=true dladdr=true syscall=true
sanitizer self-check: in-process view 2515 / 0 marked / 0 rwx，raw 2523 / 8 marked / 0 rwx
sanitizer self-check: raw-syscall view 2515 / 0 marked / 0 rwx
sanitizer self-check: module entry symbol hidden=true present=false
cli status: Version Code 3116 / API 102 / Enabled Modules 3
三轮冲压（killall systemui / gms / phone，各 45 s）：signal 11 = 0 / 0 / 0
```

### 17.7 还没做的，按性价比排

| 项 | 代价 | 判断 |
|---|---|---|
| `freopen("/proc/self/maps", …)` | 一条 spec + 一个 handler | 纯 `freopen` 读 maps 的检测极少见，但便宜，可加 |
| `gMethods[]` 里 13 个 JNI 方法名仍是 `.so` 明文 | 13 条密封串 + 运行时拼表 | 只有 root 读得到，收益低 |
| `ArtMethod` 入口点 / `kAccNative` / `class_list` | 需要伪装 ART 结构或拦截对方 native | 结构性，挡不住；属引擎固有代价 |
| 资源 hook 的 `translateResId`/`translateAttrId` 路径 | 需要装一个用 `XC_InitPackageResources` 的模块 | **未端到端验证**，是当前唯一没验过的改名点 |
| `system_server` 里 `dexopt-svc` 的 CLI 名、`service list` 里的 `cmdex` | 改名即可 | root 侧，普通应用看不到 |

### 16.7 收窄：`callback` 与 `invokeOriginal` 移出清单（2026-11）

现象：模块 `com.tdx.tuying` 在开启成员名改名后功能异常。

取证：把模块 dex 逐名字计数（`zipfile` 读全部 `classes*.dex` 后 `bytes.count`）：

```text
callback              64
invokeOriginal         0
findAndHookMethod      0        hookAllMethods        0
beforeHookedMethod     0        afterHookedMethod     0
invokeOriginalMethod   0        replaceCallback       0
```

也就是说这个模块**完全不用**本框架的 Java API（13 个名字 0 命中），却有 64 处 `callback`。
成员名改名改的是**共享的 DEX 字符串池条目**，那 64 处会一起被改 —— 包括模块自己的回调方法名、
以及它拿去反射/被 native 侧的 `GetMethodID` 查找的名字。名字一改，模块内部就对不上了。

结论：清单只能放**只属于本框架**的名字，凡在第三方代码里是常用词的一律不能动。已移除
`callback`、`invokeOriginal`；现存 13 条都与框架 API 一一对应。`ResolveMemberName("callback")`
在没有映射时原样返回入参，native 侧无需改动。

复测（同一台机器）：

```text
obfuscation: member renaming enabled for 13 name(s)
obfuscation: 53 descriptor(s) rewritten, 13 member name(s) rewritten, 52 known name(s) (52 new)
模块 dex：0 descriptor(s) rewritten, 0 member name(s) rewritten   （六个模块，含 com.tdx.tuying）
另有三个仍在用 API 的模块：3 / 4 / 5 member name(s) rewritten
```

`com.tdx.tuying` 进程起得来，`logcat` 没有 `NoSuchMethodError`／`Failed to find method`。
## 18. 验证夹具：`testres`

改完类名/成员名之后，最容易无声坏掉的是两条「按名字找东西」的路径，所以仓库里留了一个二十行的探针模块来点它们。
它不是框架的一部分，不参与打包，也不进 `perf_patch`。

| Flavor | 打包内容 | 点的是什么 |
|---|---|---|
| `:testres:assembleLegacyRelease` | `src/legacy/assets/xposed_init` | `IXposedHookInitPackageResources` → `XResources` 建在 `rt.dummy.*` 之上 → 改名后的 `translateResId`/`translateAttrId` 能被解析（`setReplacement` 之后 `getString` 返回新值才算通） |
| `:testres:assembleModernRelease` | `src/modern/resources/META-INF/xposed/*` | API 101+ 加载器能实例化入口类，`onModuleLoaded`/`onPackageLoaded`/`onPackageReady` 拿到非空 classLoader 与 `AppComponentFactory` |

两条已知事实（都是踩出来的，写在这里省下一次返工）：

1. 现代入口类必须 **extends** `io.github.libxposed.api.XposedModule`；只实现 `XposedModuleInterface` 会被 `Class does not extend XposedModule` 静默跳过。
2. 现代 API **没有**资源回调。资源 hook 在本框架里只有 legacy 一条路（`XposedInit.java` 判 `instanceof IXposedHookInitPackageResources`）。

最近一次实测：legacy 探针在一个设置进程里对 6 个包触发了回调，`original` 读到 `反馈工具箱` / `相册` / `DevCheck`，替换后 `now=probe-ok`；现代探针在 `com.android.settings` 与 `:background` 两个进程各拿到 `loader=true factory=true`；两次都 `Fatal signal 11 = 0`、`NoSuchMethodError` / `Failed to find method` 零命中。
用法与复位命令见 `testres/README.md`（含 `probe.jks` 的生成命令——签名密钥不入库，`.gitignore` 已排除 `*.jks`/`*.apk`）。

## 19. 身份集中：`daemon/.../Identity.kt`

第 1 到第 13 节把「能被别人看见的名字」一个个改掉了，但改法是就地替换，结果是同一种东西散在八处。这一节收的是**维护成本**，不是新的检测面。

### 19.1 为什么值得收

上游近 12 个月动过 105 个 `res/values*/strings.xml`，最活跃的 `manager/src/main/res/values-pl/strings.xml` 有 16 次提交。我们为了把两条通知文案改中性，删掉了 42 个 `values-*/strings.xml` 里的同名 key —— 每删一处，都是一次与上游改同一文件的机会。这类冲突不会报错，只会在某次合并后让译文回退成旧词。

### 19.2 收进去的常量

| 常量 | 值 | 原来散在 |
|---|---|---|
| `DATA_DIR` | `/data/adb/sysupd` | `data/FileSystem.kt:77` |
| `MEMBER_RENAME_MARKER` / `NO_SANITIZER_MARKER` | `rename_members` / `no_sanitizer` | `FileSystem.kt:104`、`native/src/core/sanitizer.cpp`（native 侧继续用 sealed 字符串拼） |
| `FRAMEWORK_DEX` | `framework/core.dex` | `FileSystem.kt:487` |
| `CLI_NAME` | `ctl` | `Cli.kt:191`（picocli `@Command(name = ...)`，接受 `const val`） |
| `DIAL_CODE` | `873283` | `VectorService.kt:50` |
| `CHANNEL_STATUS` / `CHANNEL_UPDATED` / `CHANNEL_SCOPE` | `sys_status` / `sys_module_updated` / `sys_module_scope` | `system/NotificationManager.kt:28,29,64` |
| `CHANNEL_*_NAME` | `System service` / `Module update complete` / `Scope request` | 三个 `R.string.*_channel_name` |
| `NOTIFICATION_TITLE` / `NOTIFICATION_CONTENT` | `Service running` / `Tap the notification to open manager` | `R.string.vector_running_notification_*` |

### 19.3 资源文件全部还原

`daemon/src/main/res/values*/strings.xml` 43 个文件（42 个本地化 + 默认）回到上游原样，代价是 daemon 侧的通知文案不再本地化。

值得说清这个取舍：那两条串只出现在一个常驻通知里，看的人是拿着手机的人，不是任何 app；而为了保住 42 种译法，就得永远背着 42 个文件的删除改动。换成英文常量之后，`daemon/src/main/res` 与上游的差异**只剩 `drawable/ic_statue_monochrome.xml`**（图标换掉的那一处）。

一个必须记住的边界：还原后 `values/strings.xml` 里又有 `Vector status` 字样，但它**没有任何代码读**。`daemon.apk` 只随模块 zip 发布、位于 `/data/adb/modules/perf_patch/`，root 之外读不到；运行时下发到系统的是 `Identity` 里的中性串。

### 19.4 效果

| 指标 | 收敛前 | 收敛后 |
|---|---|---|
| 与上游差异文件数 | 171 | 129 |
| `daemon/src/main/res` 差异 | 44 个文件 | 1 个文件（图标） |
| 需要改名的位置 | 8 个文件 13 处 | 1 个文件 13 行 |

### 19.5 验证

`.\gradlew.bat :daemon:assembleRelease` → `BUILD SUCCESSFUL in 1m 17s`；`:zygisk:zipAll` 后装机重启，`dumpsys notification --noredact` 里 `mId='sys_status' mName=System service`、`android.title=String (Service running)`，`cli status` 正常读回 `Enabled Modules`，三轮冲压 `Fatal signal 11 = 0`。

### 19.6 故意没做的

log tag（`SysNative`、`SysZygiskBridge` 等，60+ 文件）与线程名仍是字面量。它们的可见性限于 `logcat` 与 `/proc/<pid>/task/*/comm`，普通 app 两者都读不到别家进程；收它要动 60 个文件，换来的检测面为零。如果将来要把它们也收进 `Identity`，做法与本节相同，不建议现在做。
### 19.7 顺带发现的另一处泄漏：git tag 进了 `module.prop`

收敛过程中给这个提交打了个 tag 叫 `debrand-1.0`，重建后 `module.prop` 变成：

```
version=vdebrand-1.0 (3123-2bece972)
```

版本串来自 `build.gradle.kts:44` 的 `GitLatestTagValueSource`（`git tag --list --sort=-v:refname` 取第一行），所以**tag 名会原样进入模块描述文件、zip 文件名（`update-vdebrand-1.0-3123-Release.zip`）和 `ctl status` 的第一行**。改 tag 名是最省事的修法：

```bash
git tag -d debrand-1.0
git tag 3.1.0            # 任意不泄露意图的版本号
```

重建后 `version=v3.1.0 (3123-2bece972+main)`、`update-v3.1.0-3123-Release.zip`。这条记在这里是因为它属于同一类错误：给内部东西起描述性名字，然后这个名字自己跑到外面去了。
## 20. 应用进程内还能读到的那三个名字（2026-11）

§19 把 daemon 侧的名字收进了一个文件，但有三样东西仍然从**被注入进程的内存里**能读到。它们不经过文件系统、不经过 procfs，所以前面的所有过滤层都不管它们。

### 20.1 `getFrameworkName()` 曾经返回仓库名

`xposed/build.gradle.kts` 里原本是：

```kotlin
val frameworkName = runCatching { gradle.parent?.rootProject?.name }.getOrNull() ?: rootProject.name
```

这个值经 `VectorContext.getFrameworkName()`（`:78`）、`XposedService.java:160`、`XposedInterface.java:422`、`XposedInterfaceWrapper.java:95` 交给模块，最后作为 `BuildConfig.FRAMEWORK_NAME` **字符串常量躺在 `framework/core.dex` 里**，而这个 dex 是被 `InMemoryDexClassLoader` 加载进目标进程的。改名后的仓库叫什么，目标进程的堆里就叫什么。

改成常量 `"framework"`。影响面：模块调 `getFrameworkName()` 得到的字符串变了，没有别的语义（loader 不靠它判分支，要看版本有 `VERSION_NAME`）。`daemon/build.gradle.kts:37` 的 `FRAMEWORK_NAME` 同样是 `"${rootProject.name}"`，一并改成 `"framework"`。

### 20.2 binder 服务名改成构建期随机

`service list` 对普通应用只显示自己的服务，但这是一个**任何进程都能调的系统命令面**，而当时的名字是写死的 `cmdex` / `cmdex_alt`（两处：`daemon/.../VectorDaemon.kt:67`、`zygisk/src/main/cpp/module.cpp:435`）。一个固定词出现在 `service list` 里，等于免费给检测方一个判据。

沿用 `devSockName` 的做法（§19.2 里那条持久化缓存的写法）：

```kotlin
val bridgeServiceNames = run {
    val cache = rootProject.layout.buildDirectory.file("debrand/bridgeServiceNames.txt").get().asFile
    val remembered = runCatching { cache.readText().lines().filter { it.isNotBlank() } }.getOrNull()
    if (!remembered.isNullOrEmpty() && remembered.size >= 2) remembered
    else List(2) { java.util.UUID.randomUUID().toString().replace("-", "") }
        .also { cache.parentFile?.mkdirs(); cache.writeText(it.joinToString("\n")) }
}
extra.set("bridgeServiceName", bridgeServiceNames[0])
extra.set("bridgeServiceAltName", bridgeServiceNames[1])
```

两个名字都要缓存：`daemon` 与 `zygisk` 各读一份，只要其中一侧重新配置而另一侧没有，两边就会等在不同的名字上，握手会**永久**失败（这正是 `devSockName` 当初踩过的坑）。

传递路径：`daemon/build.gradle.kts` → `buildConfigField(..., "BRIDGE_SERVICE", ...)`；`zygisk/build.gradle.kts` → `cmakeIdentityArgs` → `zygisk/src/main/cpp/CMakeLists.txt` 的 `target_compile_definitions(BRIDGE_SERVICE_NAME=...)`。注意这里走的是 **CMake 缓存变量**而不是编译 flag —— 字符串宏经 `cFlags` 传递会在部分宿主上丢掉引号（§6 里那条教训）。

真机：

```
service list | grep -Ei 'cmdex|vector|lspd'
342	serial: [android.hardware.ISerialManager]        ← 只剩 AOSP 自己的
service list | grep -E '1da7967d|195bf7b0'
0	1da7967d4d8044838e429090da15752b: []             ← 新名已注册
```

### 20.3 日志与异常里的词

`framework/core.dex` 里原本有 13 处 `Vector`，除了 20.1 那一个常量，其余都是**给人看的字**——日志、异常消息：

| 位置 | 原文 | 现在 |
|---|---|---|
| `zygisk/.../core/Main.kt` | `Loading Vector/Xposed for ` | `Loading the framework for ` |
| `zygisk/.../service/BridgeService.kt:37/83` | `Vector daemin service died.` / `... binder linked.` | `The daemon binder died.` / `The daemon binder is linked.` |
| `zygisk/.../ParasiticManagerHooker.kt:519` | `Vector manager injected successfully into process.` | `The manager was injected into this process.` |
| `zygisk/.../ParasiticManagerSystemHooker.kt:154/186` | `Suppressing Android 12+ Splash Screen for Vector Manager.` | `Suppressing the Android 12+ splash screen for the manager.` |
| `xposed/.../di/VectorBootstrap.kt:59` | `check(...) { "VectorBootstrap is already initialized!" }` | `"Bootstrap is already initialized!"` |
| `legacy/.../XposedBridge.java:190/192/196`、`xposed/.../hooks/VectorNativeHooker.kt:49/57/65/77` | `belongs to Vector itself`, `Vector calls it`, `Vector's dispatch`, `Vector reflects through it` | `the framework` 系列 |
| `services/daemon-service/.../Utils.java:22` | `LOG_TAG = "Vector"` | `LOG_TAG = "Sys"` |

`LOG_TAG` 那一行还有个副作用值得记：daemon 的日志读取器按 `kPrefixTags` 只把 `Sys` / `dex2oat` / `zygisk` 开头的 tag 收进 verbose 流。早先的批量改名把 `kPrefixTags` 从 `Vector` 改成了 `Sys`，但这个裸 `"Vector"` 常量漏在外面 —— 于是它打的每一条日志都被**静默丢弃**了。改这一行同时修好了那个丢失。

异常消息那条尤其值得看一眼：`check(delegate == null)` 的失败消息是**在目标进程里抛出的**，`getMessage()` 能读到。它不需要谁真的触发，只要有一次双初始化就会把字符串送进对方手里。

### 20.4 现在的读数

```
framework/core.dex   Vector=1   （仅 org.matrix.vector.ipc.IVectorDaemon 的类名大小写）
                     cmdex=0
```

`Vector=1` 与 `lowercase vector=14` 里剩下的都是**故意留的**：

| 残留 | 为什么不动 |
|---|---|
| `org.matrix.vector.ipc.IVectorDaemon` | AIDL 描述符。改名要同时改 daemon 侧的 `*.aidl` 与 manager 侧的 stub，并接受两侧版本必须同步升级；`BuildConfig` 之外没有任何代码按它做判断，收益对不起风险 |
| `org.matrix.vector.manager.Constants` | manager 包名前缀字面量，`ParasiticManagerHooker` 用它拼类名去 `loadClass`。manager 的包名与源码命名空间故意不同（§8.1），这个前缀是唯一能把两者接起来的东西 |
| 类描述符（`Lorg/matrix/vector/impl/...`） | 加载时由 `obfuscateDexBuffer` 改名（§12），磁盘上的这份是**未改写的原件**，只有 root 能读 |

判据和 §16.7 一样：**磁盘上的 dex 与进程里的 dex 不是同一份东西**。磁盘那份留着真名不影响应用侧，进程里那份才是检测方摸得到的。

### 20.5 验证

```bash
# 1. 服务名
service list | grep -Ei 'cmdex|vector|lspd'        # 期望只剩 AOSP 的 serial
service list | grep -E "$(head -2 build/debrand/bridgeServiceNames.txt | tr '\n' '|' | sed 's/|$//')"

# 2. 常量面
./gradlew :zygisk:zipAll
python - <<'PY'
import zipfile, re
z = zipfile.ZipFile('zygisk/release/update-v3.1.0-3126-Release.zip')
d = z.read('framework/core.dex')
print('Vector =', len(re.findall(rb'Vector', d)), ' cmdex =', len(re.findall(rb'cmdex', d)))
PY

# 3. 装机后
sh /data/adb/modules/perf_patch/cli status          # Framework Version / Enabled Modules
logcat -b crash -d | grep -c Fatal                  # 0
```

真机结果：`Vector=1 / cmdex=0`、`service list` 只剩 `serial`、新名已注册、`cli status` = `3.1.0 / 3126 / API 102 / Enabled Modules 3`、`Fatal signal 11 = 0`、`sanitizer: installed (15 tokens, full)`。
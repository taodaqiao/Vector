# 去特征补丁说明

本目录下的改动把框架从"能被普通应用枚举出来"降到"只有 root 才能在进程外看到痕迹"。
分三类：**安装期身份**、**进程内可见特征**、**编译期常量**。

`debrand.py` 是全部字符串替换的唯一事实来源，幂等，可反复执行：

```
python debrand/debrand.py --dry-run     # 只看差异
python debrand/debrand.py               # 落地
```

新增的 native 代码（`native/src/core/sanitizer.cpp`、`native/include/core/sanitizer.h`）
不是脚本生成的，它随仓库一起走。

完整的改动清单（按检测通道分组）与**第三方模块兼容性说明**在
[`CHANGES-AND-COMPAT.md`](CHANGES-AND-COMPAT.md)；本文件只记去特征本身。

---

## 1. 检测通道与处理

| # | 通道 | 谁能看到 | 原值 | 现值 | 位置 |
|---|------|----------|------|------|------|
| 1 | `/proc/self/maps` 映射行 | 同进程任意代码 | `/data/adb/modules/zygisk_vector/zygisk/arm64-v8a.so` | 该行被剔除 | `native/src/core/sanitizer.cpp` |
| 2 | `/proc/self/smaps*`、`map_files/*`、`mountinfo` | 同上 | 同上 | 同上 | 同上 |
| 3 | `Runtime.exec("cat /proc/self/maps")` | 子进程 | 同上 | argv 被改写成 `/proc/self/fd/N` | `sanitizer.cpp` `ExecveImpl` |
| 4 | `/proc/net/unix` | 同进程 | 守护进程 socket 名 | 含 token 的行被剔除 | 同上 |
| 5 | `readlink("/proc/self/fd/N")` | 同进程 | 真实模块路径 | 替换为 `/dev/ashmem` | 同上 |
| 6 | `Class.forName("org.matrix.vector.impl.*")` | 同进程 | 真名（debug 构建） | 随机同长包名 | `daemon/src/main/jni/obfuscation.cpp` |
| 7 | `PackageManager` 包名 | 任意 app（`QUERY_ALL_PACKAGES`） | `org.matrix.vector.manager` | `com.sysutil.compat.core` | `build.gradle.kts` |
| 8 | 应用 label | 同上 | `Vector` | `Perf Patch` | `manager/src/main/res/values/strings.xml` |
| 9 | `getprop dalvik.vm.dex2oat-flags` | 任意 app | `--inline-max-code-units=0` | 注释掉 | `zygisk/module/system.prop` |
| 10 | `/proc/self/task/*/comm` | 同进程 | `vector-hot-reload`、`VectorCliListener` | `pool-1-thread-1`、`SocketListener` | 各 `Thread(r, ...)` |
| 11 | binder 服务名 | root 侧 `service list` | `serial` / `serial_vector` | `cmdex` / `cmdex_alt` | `module.cpp`、`VectorDaemon.kt` |
| 12 | logcat tag | `READ_LOGS` | `VectorContext` … | `SysContext` … | 全树扫（`SWEEP_PATTERN`） |
| 13 | 通知 channel id | 通知监听器 | `vector_status` … | `sys_status` … | `NotificationManager.kt` |
| 14 | 模块 id / `module.prop` / `updateJson` | root 管理器 | `zygisk_vector`、指向 GitHub raw | `perf_patch`、无 `updateJson` | `zygisk/module/module.prop` |
| 15 | 模块数据目录 | root 侧 | `/data/adb/lspd` | `/data/adb/sysupd` | `FileSystem.kt`、脚本 |
| 16 | DEX 文件名 | root 侧 | `framework/vector.dex` | `framework/core.dex` | `zygisk/build.gradle.kts`、`customize.sh` |
| 17 | Dobby 生成名 | 内存扫描 | `Vector_` / `Dobby` | `O_` / `JNI` | `module.cpp` |
| 18 | 类加载器 `toString()` | 同进程 | `VectorModuleClassLoader[...]` | `dalvik.system.DexClassLoader[...]` | `VectorModuleClassLoader.kt` |
| 19 | 启动器 zip 名 | 用户/root | `Vector-v*.zip` | `update-v*.zip` | `zygisk/build.gradle.kts` |
| 20 | CLI 名与线程名 | `ps -T` | `VectorCli` | `webview_zygote` | `zygisk/module/cli` |
| 21 | 匿名可执行映射行（trampoline） | 同进程 | 注入进程比普通进程多 1 行 | 该行被剔除 | `sanitizer.cpp` `IsAnonymousExecutable` |
| 22 | maps 里的 `rwx` 权限列 | 同进程 | Dobby 打补丁时 `mprotect(RWX)` | 只把 `w` 改 `-`，其余列原样 | `sanitizer.cpp` `MaskWritableExecutable` |
| 23 | `dl_iterate_phdr` / `dladdr` | 同进程 | `dlpi_name` 暴露模块路径 | 命中 token 的条目跳过 / `dli_fname` 置空 | `sanitizer.cpp` |
| 24 | `syscall(__NR_openat, …)` | 同进程 | 绕过 GOT 拿到真实 maps | 与 `open` 同一份过滤内容 | `sanitizer.cpp` `SyscallImpl` |
| 25 | lsplant 的 `LSPHooker_` 前缀 | 同进程 `.rodata` | `black_crash` 黑名单项 | `L5PHooker_`（等长替换） | `debrand/patch_so.py` |

`obfuscation.cpp` 的运行时 DEX 混淆表新增了 `Lorg/matrix/vector/impl/` 一项：这是 Xposed
模块的 namespace（`xposed/build.gradle.kts` 的 `namespace = "org.matrix.vector.impl"`），
此前被漏掉，注入后的类名在 debug 构建里是真名，`Class.forName` 一次就能确认框架存在。

类**简单名**本身也已不再是原文（第 12 节）：

| 项 | 原状 | 现状 | 位置 |
| --- | --- | --- | --- |
| `de.robv.android.xposed.*` 下的 52 个类 | 描述符原文进内存 dex | 同长度随机名，`$` 与 `/` 保留 | `obfuscation.cpp` 的 `simple_name_scopes` |
| `org.matrix.vector.impl.*` 下的类 | 同上 | 同上 | 同上 |
| `xposed.dummy.*` 两个存根 | `Class.forName` 命中 | 包名改为 `rt.dummy.*` | `hiddenapi/stubs`、`XResources.java` |
| `org.matrix.vector.{core,nativebridge,service}.*` | — | **只随机前缀，类名保持** | native 侧要按「前缀 + 类名」拼串，见 12.2 |

崩溃/失败的教训也已回写进文档：`xposed.dummy` 曾试过「用到才定义」，真机上 ART 会把失败解析
记到进程结束，走资源 API 的模块全废（`NoClassDefFoundError: Lxposed/dummy/XResourcesSuperClass`），
所以改成**改名而不是改时机**。

裸系统调用那条也堵上了（见 `CHANGES-AND-COMPAT.md` 第 13 节）：上游**没有** `syscall` 钩子，
能直接复用的是仓库里已经编进来的 Dobby —— `HookInline` 就是 `DobbyHook` 的包装。
现在 `syscall(__NR_openat, AT_FDCWD, "/proc/self/maps", …)` 与 `open("/proc/self/maps")` 拿到的是同一份过滤内容：

```
sanitizer self-check: in-process view 2520 lines / 0 marked, raw 2529 lines / 8 marked
sanitizer self-check: raw-syscall view 2521 lines / 0 marked
```

---

## 2. sanitizer 的工作方式

`sanitizer::Install()` 在 `postAppSpecialize` 里、**所有早退路径之后**（`SetupEntryClass`
之后、`FindAndCall` 之前）调用。位置很关键：早退分支会 `SetAllowUnload(true)` 把模块 image
交还 Zygisk，钩子表里就留下指向已卸载页面的指针 —— 这正是早期版本 "所有进程同一地址 SIGSEGV"
的成因。过了这段，`postAppSpecialize` 必然以 `SetAllowUnload(false)` 收尾，image 留到进程结束。
`postServerSpecialize` 里也有一次，覆盖 system_server。

过滤路径**不使用任何 `thread_local`**：本工程按模拟 TLS 构建，`thread_local` 首次访问会走
`__emutls_get_address` → `malloc`，而钩子就跑在分配器慢路径上，会把分配器自己踩崩
（tombstone 里表现为 `je_tsd_init_finish ← je_malloc ← malloc ← libzygisk.so`）。
守卫深度改用 `pthread_self()` 键控的定长表（1024 槽、16 次探测、表满退化为"不守护"）。

钩住的 in-process 读取入口：

```
open  __open_2  openat  __openat_2  fopen  readlink  readlinkat  execve
```

外加一条不属于 GOT 的：`syscall`（Dobby 就地 hook，只拦 `openat`/`openat2`/`open` 三个号，其余原样转发）。
`__open_2`/`__openat_2` 目前不在名单里 —— 它们是 FORTIFY 包装，libc 自己会调，改它会连 `opendir()` 一起打坏。

命中被监视路径时（`/proc/<self|thread-self|数字>/` + `maps|smaps|smaps_rollup|numa_maps`、
`mountinfo|mounts`、`net/unix`、`task/<tid>/*`、`map_files/*`、`fd/<n>`）：

1. 用原始 `open` 读真内容（上限 8 MiB）；
2. 逐行剔除含 token 的行（15 条 token，全小写：`/data/adb`、`lsposed`、`lspd`、`vector`、
   `zygisk`、`lsplant`、`dobby`、`xposed`、`robv`、`libxposed`、`magisk`、`kernelsu`、`apatch`、
   `riru`、`shamiko`）—— `/data/adb` 一条就覆盖了所有模块目录，顺带把 Magisk 自身痕迹一起压掉；
   这些 token 在二进制里是 XOR 掩码常量（`debrand/seal_strings.py` 生成），所以"隐藏者自己
   不把要隐藏的词写进 `.rodata`"；
3. 写进 `memfd:jit-cache`（名字与 ART JIT 的匿名映射一致，不引入新特征），返回该 fd。

`execve` 走另一条路：把 argv 里指向这些路径的实参改写成 `/proc/self/fd/N`，且该 fd
**不设 `FD_CLOEXEC`**，因此子进程（`cat`、`toybox`、`sh -c`）读到的是过滤后的副本。

框架自己需要原始 maps 的地方（`native/src/elf/elf_image.cpp` 的 linker 兜底路径）用
`sanitizer::ScopedBypass` 临时抬高**本线程**的守卫深度，读到未过滤内容；`ScopedBypass` 与
`HookDepth` 操作同一个 `depth` 字段，所以"钩子内部"与"框架自身"共享一套重入保护。

**重入是这类代码的第一号风险**：`open` → `std::string` → `malloc` → 分配器慢路径 → `openat`
→ 又进 `open`，没有守卫就是无限递归直到栈耗尽（真机上表现为 `SEGV_ACCERR` 写栈守护页，
`fault addr 0x7c87c0ffe0`）。每个 handler 命中路径的第一行都是 `HookDepth depth_guard;`。

**边界**：这层只处理进程内读取。root 从进程外读 `/proc/<pid>/maps` 仍然能看到真实路径，
那需要内核模块或 zygisk detach 级别的隐藏，不在本补丁范围内。

---

## 3. 构建

```bash
# 需要 Android SDK 37 / NDK 30.0.16248370 / JDK 21
./gradlew :zygisk:zipAll
```

`sanitizer.cpp` 由 `native/CMakeLists.txt` 的 `GLOB_RECURSE src/*.cpp` 自动纳入，
`HookInline` / `UnhookInline` 来自 `native/include/core/native_api.h`（Dobby 包装）。
GOT 槽的写入是自己算的（`DT_JMPREL` + `DT_STRTAB`/`DT_SYMTAB`，`mprotect` 后按所属
`PT_LOAD` 还原权限），不经过第三方 PLT 库：唯一失败模式就是"写错槽"，而写错槽会让
`real_open` 变成野指针 —— 早期用 lsplt 时整进程同址 `SIGSEGV` 就是这么来的。
`__open_2` / `__openat_2` 不在名单里：它们是 FORTIFY 包装，libc 自己会调，改写会连带打坏
`opendir()`（崩溃点 `opendir+24 → bl __open_2@plt`）。

---

## 4. 验证

设备侧（root）：

```bash
adb push debrand/selfcheck.sh /data/local/tmp/ && adb shell su -c 'sh /data/local/tmp/selfcheck.sh'
```

进程内（唯一能真正验证 sanitizer 的方式）——把下面代码丢进任意测试 app 跑一次：

```kotlin
val maps = File("/proc/self/maps").readLines()
check(maps.none { it.contains("/data/adb") }) { "maps leak" }
check(maps.none { Regex("""^[0-9a-f]+-[0-9a-f]+ rwx """).containsMatchIn(it) }) { "rwx leak" }
check(maps.count { Regex("""^[0-9a-f]+-[0-9a-f]+ [rwx-]{4} [0-9a-f]+ [0-9a-f]{2}:[0-9a-f]{2} \d+\s*$""")
    .containsMatchIn(it) } == 0) { "anonymous exec leak" }
check(Runtime.getRuntime().exec(arrayOf("cat", "/proc/self/maps"))
    .inputStream.bufferedReader().readLines().none { it.contains("/data/adb") }) { "exec leak" }
check(File("/proc/net/unix").readLines().none { it.contains("lspd") }) { "unix leak" }
for (name in listOf(
    "de.robv.android.xposed.XposedBridge",
    "de.robv.android.xposed.XposedHelpers",
    "de.robv.android.xposed.DexposedBridge",
    "org.matrix.vector.core.Main",
    "xposed.dummy.XResourcesSuperClass",
)) {
    check(runCatching { Class.forName(name) }.isFailure) { "class leak: $name" }
}
```

栈回溯那条（《检测特征》文档里的 `hasXposedStackTrace`）用同一段代码顺手验：

```kotlin
val frames = Throwable().stackTrace.joinToString("\n") { it.className + "." + it.methodName }
check(!frames.contains("handleHookedMethod")) { "stack leak" }
check(!frames.contains("com.saurik.substrate")) { "substrate leak" }
```

裸 `syscall` 那条只能从 native 侧验（Kotlin 没有等价入口），一行就够：

```c
int fd = (int)syscall(__NR_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY);
// 读全量后 grep "/data/adb"：应当 0 命中
```

---

## 5. 已知残留（有意保留 / 需另做）

- **root 侧可见**：`/data/adb/sysupd`、`/data/adb/modules/perf_patch`、daemon 进程、
  `service list` 里的 `cmdex`。目录号随机、名字中性，但存在本身可见。
- **方法名已可改，默认关**：`findAndHookMethod` / `beforeHookedMethod` / `afterHookedMethod` /
  `hookAllMethods` / `invokeOriginalMethod` 等 15 个成员名，开关是
  `/data/adb/sysupd/rename_members`（存在即开）。机制、清单与真机数据见
  `CHANGES-AND-COMPAT.md` 第 16 节。关着的时候这些名字仍然是明面（各 1 处）。
- **native 侧那 13 个 JNI 方法名**：`HookBridge.hookMethod` 等写在 `JNINativeMethod` 表里，
  是 `.so` 里的明文字面量。它们只在 `/data/adb/modules/perf_patch/` 里，应用读不到；要消掉
  得把表改成运行时用密封串拼（未做）。
- **hook 引擎自省的物理痕迹**：`ArtMethod` 入口点被改写成 trampoline、`kAccNative` 变化；
  以及被 hook 方法内 `Thread.getStackTrace()` 会看到随机包名帧。这两条挡不住，属引擎固有代价。
- **manager 的编译期 namespace** 仍是 `org.matrix.vector.*`：它决定 `R` / `BuildConfig`
  生成位置，改它要动全部 import，收益低（release 构建里类名被 R8 重命名）。
- **UPDATE 仍指向 GitHub**：`GitHubRepository.kt` 的 `REPO` / `ZIP_PREFIX` 未改，保留自更新
  能力。要彻底去掉网络签名，改这两处并删掉 `updateJson`（已删）。
- **`kExactTags` 里的 `LSPlant` / `Dobby` / `Magisk` 等**：只做日志路由，属于第三方 tag，
  未动，避免收不到相应日志。

---

## 6. 第二轮：编译器侧残留（源码改不到的那一半）

第一轮之后 `libzygisk.so` 仍然带着 `vector`×186 / `lsplant`×849 / `Dobby`，原因不是字符串
字面量，而是**编译器生成的表**：C++ mangled 导出符号、RTTI typeinfo 名、`.symtab` 里的本地
符号名。`llvm-nm -D --defined-only` 当时有 1844 个导出符号。第二轮按"符号面 → 类型名 →
常量折叠 → 逐字节修补"的顺序处理。

### 6.1 符号面收敛

`native/CMakeLists.txt` 与 `zygisk/src/main/cpp/CMakeLists.txt` 加：

```cmake
-fvisibility=hidden -fvisibility-inlines-hidden -fno-ident
-ffile-prefix-map=${VECTOR_ROOT}=/src -fmacro-prefix-map=${VECTOR_ROOT}=/src
target_link_options(... -Wl,--exclude-libs,ALL)
```

导出符号 1844 → 7，其中唯一有意义的还是 `zygisk_module_entry`（`zygisk.hpp` 用
`[[gnu::visibility("default")]]` 显式声明）。`libzygisk.so` 体积 1,091,304 → 585,504 B。

### 6.2 密封字符串曾被常量折叠

`seal_strings.py` 生成的数组最初写成 `inline constexpr unsigned char[]` + `inline Open()`，
编译器把整个循环在编译期算完，**把明文当常量发射**——改完 `lsposed` 还在。现在数组是
`inline const volatile unsigned char[]`，`Open()` 收 `const volatile unsigned char *`：
volatile 读取不可被折叠，明文只在进程栈上出现。

### 6.3 类型名：`rename_cxx.py`

`N6vector6native6module12VectorModuleE` 这类 typeinfo 名来自命名空间与类名，只能改标识符。
`debrand/rename_cxx.py` 幂等，替换 `vector::native` → `sys::rt`、`VectorModule` →
`LoaderModule`、`ConfigBridge` → `SettingsSource`、`ConfigImpl` → `SettingsImpl`，覆盖
`native/`、`zygisk/`、`dex2oat/` 下的 `.cpp/.h/.hpp`。改完 `vector` 命中 22 → 1。

### 6.4 逐字节修补：`patch_so.py`

最后 1 处是依赖里池化出来的裸 `"vector"`，以及将来可能再出现的同类残留。`debrand/patch_so.py`
做**等长**替换（`vector`→`v3ctor`、`lsplant`→`lspl4nt`、`Dobby`→`D0bby`、`JingMatrix`→
`J1ngMatr1x`、`lspd`→`l5pd`、`robv`→`r0bv`），长度不变所以 ELF 偏移、段大小、指令立即数都不动。
它只处理 ELF：**已签名的 APK 不能这样改**，会被摘要校验拒绝。

`zygisk/build.gradle.kts` 的 `prepareModuleFiles<Variant>` 在打包前对
`lib/**/*.so`、`bin/**/dex2oat`、`bin/**/*.so` 逐个跑它，因此 zip 里的产物永远是修好的。

### 6.5 握手 socket 名改为构建期随机

`5291374ceda0aef7c5d86cd2a4f6a3ac` 曾是 daemon 与 dex2oat 之间的 abstract unix socket 名，
被安装脚本 `sed` 换成随机值。这串常量本身就是跨产物签名：zip、两个二进制、活进程打开的
socket 都能对上。

现在 `build.gradle.kts` 每次构建生成一次 `devSockName`（UUID 去连字符），经 CMake 缓存变量
分别进 `daemon/src/main/jni/CMakeLists.txt` 与 `dex2oat/src/main/cpp/CMakeLists.txt` 的
`target_compile_definitions(... DEV_SOCK_NAME="${DEV_SOCK_NAME}")`；两侧同一次构建，必然一致。
`customize.sh` 里的 `DEV_PATH` 补丁块随之删除。

### 6.6 模块 zip 的身份

- zip 不再打包仓库 `README.md`（它写了项目名、作者、依赖来源），改为
  `zygisk/module/README.md`：只讲安装与排错，没有可匹配的名字。
- `service.sh` 的告警前缀、`daemon` 的 `--nice-name=vectord` 与 `log -t "Vector"` 已中性化。
- `customize.sh` 删掉了"禁用旧 LSPosed 安装"的迁移分支（`zygisk_lsposed` 字面量）。
- `sepolicy.rule` 里提到 `serial_vector` 的注释改写为描述性文字。

### 6.7 复查数据（arm64 release，`update-v1.0-3116-Release.zip` 解包后）

```
lib/<4 个 ABI>/libzygisk.so   {'com.sysutil.compat.core': 1}
```

四个 ABI 一致；`llvm-nm -D --defined-only` 只剩 3 个 std typeinfo（weak）+
`zygisk_module_entry`；`llvm-readelf -d` 依赖仍是 liblog/libz/libm/libdl/libc，ELF 结构完好。
`com.sysutil.compat.core` 是 manager 包名，daemon 要按它判断"这是不是管理器进程"，属于
运行期必需常量。

### 6.8 仍未处理（附理由）

| 残留 | 在哪 | 为什么留着 |
|------|------|-----------|
| `robv` / `lsposed` / `org.matrix.vector` ×多 | `framework/core.dex` | 这是 Xposed 兼容面：第三方模块 `import de.robv.android.xposed.*` 必须能解析到这些类。运行期由 `obfuscation.cpp` 随机重命名，但**磁盘上的 dex 只能是真名**。要消掉得把改写搬到构建期，与本轮预算不符。 |
| `lsplant` / `dobby` / `vector` ×多 | `daemon.apk` 里的 `libdaemon.so` 等 | 这些库只在 root 的 daemon 进程加载，普通应用看不到；改 APK 内条目要重签/重打包，风险高于收益。 |
| `org.matrix.vector.ipc.IManagerService` | `manager.apk` 的 dex | AIDL descriptor 是 daemon 与 manager 的运行期协议常量，两端必须同名；改名要连 `.aidl` 目录与 package 一起动。只有 root 能读到 APK。 |
| `xposed_file` / `xposed_data` | SELinux 类型名 | 类型名由 `sepolicy.rule` 注入，改它要同步 `customize.sh` 的 `set_perm ... u:object_r:`，且这些名字只在 root 的 `sepolicy` 里可见。 |
| `org.matrix.vector.daemon.Cli` / `VectorDaemon` | `zygisk/module/{cli,daemon}` | app_process 的 `--nice-name` 之后跟的是主类全名，必须与 daemon APK 内的类一致；改它等于重排 daemon 的 Kotlin 包与全部 JNI 符号名。 |

这些都在"只有 root 能在进程外读到"的层面，与本轮目标（普通应用枚举不出来）不冲突。

---

## 7. 复现与验证

```
python debrand/debrand.py --dry-run        # 字符串层：应为 applied 0 / mismatched 0
python debrand/rename_cxx.py --dry-run     # 标识符层：应为 edited 0 / leftover 0
python debrand/seal_strings.py             # 需要改密封表时再跑
.\gradlew.bat :zygisk:zipAll --max-workers=2
python debrand/scan_tokens.py debrand/_unpacked/release
```

`scan_tokens.py` 命中即 exit 1，可以直接当 CI 卡口。设备侧用 `debrand/selfcheck.sh`
（需要 root）核对 `getprop`、`pm list packages`、`service list`、`/proc/net/unix`、
目标进程的线程名与 fd。

---

## 8. 稳定性：三次真机崩溃的根因（读代码前先读这节）

这一层动了每个进程的 `open`/`openat`/`fopen`/`readlink`/`execve`/`syscall`，任何错误都是
**系统级**的。真机上出过三次，全部已修，记在这里是因为它们的教训比代码本身重要。

| # | 现象 | 根因 | 修法 |
|---|------|------|------|
| 1 | 每个被注入进程立刻 `SIGSEGV`，同一次开机内**全进程同一地址**（`0x2d4e982d88`、`0x2e10bd6b5c`…） | 钩子装完就把模块 image 交还 Zygisk（早退路径 `SetAllowUnload(true)`），GOT 槽里留下指向已卸载页面的指针 | `Install()` 移到所有早退路径之后；`SetAllowUnload(false)` 收尾 |
| 2 | `system_server` 间歇 `SEGV_ACCERR`，写栈守护页 | `PermsForSlot` 在"地址不属于本 image 任何 `PT_LOAD`"时回退 `PROT_READ`，把无关页 `mprotect` 成只读 | `PermsForSlot` 找不到段返回 `0`，`PatchSlot` 见 `0` 拒绝写，`PatchImage` 计数并 `LOGW` |
| 3 | `gms`/`systemui` 崩在 `je_tsd_init_finish ← je_malloc ← malloc`，栈里夹着我们的 handler | 过滤路径用了 `thread_local`，模拟 TLS 下首次访问要 `malloc`，而钩子就在分配器慢路径上 | 去掉 `thread_local`，改 `pthread_self()` 键控的 1024 槽定长表 |

冲压测试的用法（每轮 45 s，重启后跑 3 轮）：

```bash
adb shell su -c 'logcat -b crash -c'
for i in 1 2 3; do
  adb shell su -c 'killall com.android.systemui com.google.android.gms com.android.phone'
  sleep 45
  adb shell su -c 'logcat -b crash -d -v brief | grep -c "Fatal signal 11"'
done
```

判读要点：`logcat -b crash` 会回卷，计数从 2 掉到 0 **不等于**崩溃消失，要看 `Fatal signal`
原文或 `/data/tombstones/`。当前基线是 8 轮冲压 0 崩溃（模块开启 + 过滤全开）。

崩溃定位的手法（本仓库两条都用过，值得保留）：

```
llvm-readelf -S libzygisk.so | grep debuglink        # 找出 .gnu_debuglink
ls zygisk/build/symbols/<abi>/zygisk.debug           # strip 后的符号都在这儿
llvm-symbolizer --obj=zygisk.debug --functions --inlines 0x8c814 0x46fa8
adb shell su -c 'sed -n "/^backtrace:/,/^$/p" /data/tombstones/tombstone_00'
```

（另有一条不影响运行期可见性的残留：`zygisk/module/sepolicy.rule` 与个别注释里仍写着旧名。）

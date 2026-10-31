#!/usr/bin/env python3
"""Table-driven removal of the framework's identifying strings.

Three kinds of rule, all idempotent and all reported the same way:

  EDITS         exact (file, old, new) substring replacement
  REGEX_EDITS   (file, pattern, replacement, marker) applied once; ``marker``
                proves the rule already ran, so a second run is a no-op
  FILE_CONTENTS whole-file rewrites for files whose real content is entirely
                identifying (module.prop, system.prop)

A rule whose old text is missing from a file that has not been converted is
reported as a mismatch instead of being silently dropped.

Usage:
    python debrand.py [--dry-run] [--root <path>]
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# --- Module identity -------------------------------------------------------
# The Magisk module id becomes a directory name, so it is the one string a
# process cannot avoid carrying around.
MODULE_ID_OLD = "zygisk_vector"
MODULE_ID_NEW = "perf_patch"

DATA_DIR_OLD = "/data/adb/lspd"
DATA_DIR_NEW = "/data/adb/sysupd"

# Name of the framework DEX inside the module zip.
DEX_OLD = "framework/vector.dex"
DEX_NEW = "framework/core.dex"

# Binder rendezvous names claimed by the daemon in servicemanager.  A detector
# with root can simply list every registered service.
BRIDGE_OLD = '"serial_vector" else "serial"'
BRIDGE_NEW = '"cmdex_alt" else "cmdex"'
BRIDGE_C_OLD = 'is_late_inject ? "serial_vector" : "serial"'
BRIDGE_C_NEW = 'is_late_inject ? "cmdex_alt" : "cmdex"'

# The installed package name a detector can look up with PackageManager.  The
# compile-time namespace stays as it is: it decides where R and BuildConfig are
# generated, and moving it would touch every import in the module.
MANAGER_ID_OLD = 'val defaultManagerPackageName = "org.matrix.vector.manager"'
MANAGER_ID_NEW = 'val defaultManagerPackageName = "com.sysutil.compat.core"'

EDITS: list[tuple[str, str, str]] = [
    # --- Module packaging ------------------------------------------------
    (
        "zygisk/module/uninstall.sh",
        "rm -rf " + DATA_DIR_OLD,
        "rm -rf " + DATA_DIR_NEW,
    ),
    ("zygisk/module/customize.sh", '- Vector version ${VERSION}', "- Version ${VERSION}"),
    ("zygisk/module/customize.sh", "framework/vector.dex", DEX_NEW),
    ("zygisk/module/customize.sh", '- Welcome to Vector!', "- Installed."),
    # --- Launcher script -------------------------------------------------
    ("zygisk/module/cli", '$DEXDIR/zygisk_vector', "$DEXDIR/" + MODULE_ID_NEW),
    ("zygisk/module/cli", "No vector module path found", "module directory not found"),
    ("zygisk/module/cli", "--nice-name=VectorCli", "--nice-name=webview_zygote"),
    # --- Build ------------------------------------------------------------
    (
        "zygisk/build.gradle.kts",
        'rename("classes.dex", "vector.dex")',
        'rename("classes.dex", "core.dex")',
    ),
    (
        "zygisk/build.gradle.kts",
        '"Vector-v${versionNameProvider.get()}-${versionCodeProvider.get()}-$variantCapped.zip"',
        '"update-v${versionNameProvider.get()}-${versionCodeProvider.get()}-$variantCapped.zip"',
    ),
    ("build.gradle.kts", MANAGER_ID_OLD, MANAGER_ID_NEW),
    (
        "manager/build.gradle.kts",
        "namespace = defaultManagerPackageName",
        'namespace = "org.matrix.vector.manager"',
    ),
    # --- Daemon: paths and identity --------------------------------------
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/data/FileSystem.kt",
        'Paths.get("/data/adb/lspd")',
        'Paths.get("' + DATA_DIR_NEW + '")',
    ),
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/data/FileSystem.kt",
        'FileInputStream("framework/vector.dex")',
        'FileInputStream("' + DEX_NEW + '")',
    ),
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/data/FileSystem.kt",
        '"Vector ${BuildConfig.BUILD_TYPE} ${BuildConfig.VERSION_NAME} "',
        '"framework ${BuildConfig.BUILD_TYPE} ${BuildConfig.VERSION_NAME} "',
    ),
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/VectorDaemon.kt",
        "get() = if (startedAfterSystemServer) " + BRIDGE_OLD,
        "get() = if (startedAfterSystemServer) " + BRIDGE_NEW,
    ),
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/VectorDaemon.kt",
        'setAppName("org.matrix.vector.daemon", 0)',
        'setAppName("android.bg", 0)',
    ),
    # --- Daemon: names visible from /proc and ps -------------------------
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/env/CliSocketServer.kt",
        'serverThread.name = "VectorCliListener"',
        'serverThread.name = "SocketListener"',
    ),
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/ipc/ModuleAppService.kt",
        'Thread(r, "vector-hot-reload")',
        'Thread(r, "pool-1-thread-1")',
    ),
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/ipc/ModuleAppService.kt",
        'Thread(r, "vector-module-binder")',
        'Thread(r, "pool-3-thread-1")',
    ),
    (
        "xposed/src/main/kotlin/org/matrix/vector/impl/core/VectorProcessChannel.kt",
        'Thread(it, "vector-hot-reload")',
        'Thread(it, "pool-1-thread-1")',
    ),
    ("daemon/src/main/kotlin/org/matrix/vector/daemon/Cli.kt", 'name = "vector-cli"', 'name = "ctl"'),
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/Cli.kt",
        'version = ["Vector CLI ${BuildConfig.VERSION_NAME}"]',
        'version = ["ctl ${BuildConfig.VERSION_NAME}"]',
    ),
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/Cli.kt",
        'description = ["A fast, scriptable CLI for configuring the Vector Framework daemon."]',
        'description = ["A scriptable CLI for the framework daemon."]',
    ),
    # --- Per-process artefacts -------------------------------------------
    (
        "xposed/src/main/kotlin/org/matrix/vector/impl/utils/VectorModuleClassLoader.kt",
        '"VectorModuleClassLoader[module=$apkPath, ${super.toString()}]"',
        '"dalvik.system.DexClassLoader[module=$apkPath, ${super.toString()}]"',
    ),
    # --- Zygisk native ----------------------------------------------------
    ('zygisk/src/main/cpp/module.cpp', '.generated_class_name = "Vector_",', '.generated_class_name = "O_",'),
    ('zygisk/src/main/cpp/module.cpp', '.generated_source_name = "Dobby",', '.generated_source_name = "JNI",'),
    ("zygisk/src/main/cpp/module.cpp", BRIDGE_C_OLD, BRIDGE_C_NEW),
    ('zygisk/src/main/cpp/module.cpp', 'LOGD("Vector Zygisk module loaded");', 'LOGD("framework module loaded");'),
    # --- Daemon logcat router --------------------------------------------
    # The prefix table has to keep matching the tags the sweep gives the rest
    # of the tree, otherwise the framework's own lines stop reaching its log.
    (
        "daemon/src/main/jni/logcat.cpp",
        '// Partial matches for dynamic components like Zygisk modules or Vector/LSPosed components.\n'
        'constexpr auto kPrefixTags = std::array{"LSPosed"sv, "Vector"sv, "dex2oat"sv, "zygisk"sv};',
        '// Partial matches for dynamic components: tool tags and this framework\'s own\n'
        '// components, which now share the neutral "Sys" prefix (see the tag sweep).\n'
        'constexpr auto kPrefixTags = std::array{"Sys"sv, "dex2oat"sv, "zygisk"sv};',
    ),
    # --- Manager: constants that outlive the process ----------------------
    (
        "manager/src/main/kotlin/org/matrix/vector/manager/ipc/InstallResult.kt",
        '"org.matrix.vector.manager.INSTALL_RESULT"',
        '"org.matrix.vector.manager.INSTALL_DONE"',
    ),
    (
        "manager/src/main/kotlin/org/matrix/vector/manager/data/repository/LaunchShortcut.kt",
        '"org.matrix.vector.manager.shortcut"',
        '"org.matrix.vector.manager.launch"',
    ),
    (
        "manager/src/main/kotlin/org/matrix/vector/manager/data/repository/SettingsRepository.kt",
        '"vector_settings"',
        '"framework_settings"',
    ),
    (
        "manager/src/main/kotlin/org/matrix/vector/manager/data/log/LogArchiveName.kt",
        '"Vector-logs-',
        '"logs-',
    ),
    # --- Externally visible: PackageManager reads both of these -----------
    (
        "manager/src/main/AndroidManifest.xml",
        '<category android:name="org.matrix.vector.manager.LAUNCH_MANAGER" />',
        '<category android:name="${applicationId}.LAUNCH_MANAGER" />',
    ),
    (
        "manager/src/main/res/values/strings.xml",
        '<string name="app_name" translatable="false">Vector</string>',
        '<string name="app_name" translatable="false">Perf Patch</string>',
    ),
    # --- Notification channels: readable by a notification listener --------
    # These rewrite the constant declarations, not every occurrence of the old id: the ids also
    # appear in ``retireOldChannels()``, where they have to stay literal for the delete call to
    # hit the channels created by earlier builds.
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/system/NotificationManager.kt",
        'private const val STATUS_CHANNEL_ID = "vector_status"',
        'private const val STATUS_CHANNEL_ID = "sys_status"',
    ),
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/system/NotificationManager.kt",
        'private const val UPDATED_CHANNEL_ID = "vector_module_updated"',
        'private const val UPDATED_CHANNEL_ID = "sys_module_updated"',
    ),
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/system/NotificationManager.kt",
        'val SCOPE_CHANNEL_ID = "vector_module_scope"',
        'val SCOPE_CHANNEL_ID = "sys_module_scope"',
    ),
    # Splash logo: a literal in the shipped manager APK.
    (
        "manager-ui/src/main/kotlin/org/matrix/vector/ui/ambience/MatrixRenderer.kt",
        '"VECTORXPOSED"',
        '"SYSTEMCORE"',
    ),
    # Dial code 832867 is "VECTOR" on a phone keypad, so it was a derivable
    # signature rather than a secret.  873283 spells nothing in particular.
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/VectorService.kt",
        'private const val SECRET_CODE = "832867"',
        'private const val SECRET_CODE = "873283"',
    ),
    (
        "manager/src/main/kotlin/org/matrix/vector/manager/ui/screens/home/SystemStatusScreen.kt",
        'private const val SECRET_CODE = "*#*#832867#*#*"',
        'private const val SECRET_CODE = "*#*#873283#*#*"',
    ),
    # Name of the file a config backup writes; the user sees it in the picker.
    (
        "manager/src/main/kotlin/org/matrix/vector/manager/ui/screens/modules/ModulesScreen.kt",
        '"vector-modules.bak"',
        '"modules.bak"',
    ),
]

# (file, pattern, replacement, marker).  ``replacement`` may use \1 backrefs;
# it is applied through Match.expand, so a ``$`` in the replacement is literal.
REGEX_EDITS: list[tuple[str, str, str, str]] = [
    # The two hook libraries used to be addressed by the hard-coded module id.
    # The directory on disk is whatever the installer created, so it is derived
    # from our own executable path instead of being baked into the daemon.
    (
        "daemon/src/main/kotlin/org/matrix/vector/daemon/env/Dex2OatServer.kt",
        r'(?m)^[ \t]*openDex2oat\(4, "/data/adb/modules/zygisk_vector/bin/liboat_hook32\.so"\)\n'
        r'[ \t]*openDex2oat\(5, "/data/adb/modules/zygisk_vector/bin/liboat_hook64\.so"\)\n',
        "    val moduleDir = moduleDirectory()\n"
        '    openDex2oat(4, "$moduleDir/bin/liboat_hook32.so")\n'
        '    openDex2oat(5, "$moduleDir/bin/liboat_hook64.so")\n',
        '"$moduleDir/bin/liboat_hook32.so"',
    ),
    # The runtime DEX obfuscator rewrites the package prefixes of every class the
    # framework loads, which is what defeats Class.forName probing.  The Xposed
    # implementation namespace was missing from its table, so those classes kept
    # their real names under a debug build.
    (
        "daemon/src/main/jni/obfuscation.cpp",
        r'(\{"Lorg/matrix/vector/core/", ""\},\n)',
        '\\1    {"Lorg/matrix/vector/impl/", ""},\n',
        '"Lorg/matrix/vector/impl/"',
    ),
]

# The module's public identity, as Magisk and every root-management UI shows it.
MODULE_PROP = """id=perf_patch
name=Perf Patch
version=${versionName} (${versionCode}-${versionHash})
versionCode=${versionCode}
author=
description=Runtime compatibility layer for Android 8.1 ~ 17.
"""

# dalvik.vm.dex2oat-flags is a public property: any application can read it with
# a single getprop, and --inline-max-code-units=0 is not a value the platform
# ever sets by itself.
SYSTEM_PROP = """# `dalvik.vm.dex2oat-flags` is a public property: every application can read it
# with a single getprop, and `--inline-max-code-units=0` is not a value the
# platform ever sets on its own.  Leaving it out costs inlining-dependent hooks
# on Android 8.1 and 9 only (Magisk only installs this file below API 29);
# restore the line if hook stability there matters more than the signature.
#dalvik.vm.dex2oat-flags=--inline-max-code-units=0
"""

FILE_CONTENTS: dict[str, str] = {
    "zygisk/module/module.prop": MODULE_PROP,
    "zygisk/module/system.prop": SYSTEM_PROP,
}

# Logcat tags are a signature for anything holding READ_LOGS (a root-capable
# detector greps the buffer for the framework's name).  Every tag in the tree is
# a quoted single identifier that starts with the framework name, and the class
# identifiers of the same name are unquoted, so this pattern only ever touches
# tag literals -- including the routing tables in logcat.cpp that have to agree
# with them, which is why they all move together.
SWEEP_SUFFIXES = (".kt", ".java", ".cpp", ".h")
SWEEP_PATTERN = r'"Vector([A-Za-z0-9_]+)"'
SWEEP_REPLACEMENT = r'"Sys\1"'

# Failures that are expected when an upstream file has moved; they are reported
# but do not stop the run.
OPTIONAL_PREFIXES = ("manager/", "xposed/")


class Report:
    def __init__(self) -> None:
        self.applied = 0
        self.skipped = 0
        self.missing: list[tuple[str, str]] = []
        self.changed: set[str] = set()

    def read(self, root: str, rel_path: str) -> str | None:
        path = os.path.join(root, rel_path)
        if not os.path.exists(path):
            self.missing.append((rel_path, "file does not exist"))
            return None
        with open(path, "r", encoding="utf-8", newline="") as handle:
            return handle.read()

    def write(self, root: str, rel_path: str, text: str, dry_run: bool) -> None:
        self.applied += 1
        self.changed.add(rel_path)
        if dry_run:
            return
        with open(os.path.join(root, rel_path), "w", encoding="utf-8", newline="") as handle:
            handle.write(text)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = parser.parse_args()

    root = os.path.abspath(args.root)
    report = Report()

    for rel_path, old, new in EDITS:
        text = report.read(root, rel_path)
        if text is None:
            continue
        if new in text and old not in text:
            report.skipped += 1
            continue
        if old not in text:
            report.missing.append((rel_path, old))
            continue
        report.write(root, rel_path, text.replace(old, new), args.dry_run)

    for rel_path, pattern, replacement, marker in REGEX_EDITS:
        text = report.read(root, rel_path)
        if text is None:
            continue
        if marker in text:
            report.skipped += 1
            continue
        compiled = re.compile(pattern)
        if not compiled.search(text):
            report.missing.append((rel_path, pattern))
            continue
        updated = compiled.sub(lambda m: m.expand(replacement), text, count=1)
        report.write(root, rel_path, updated, args.dry_run)

    for rel_path, content in FILE_CONTENTS.items():
        text = report.read(root, rel_path)
        if text is None:
            continue
        if text == content:
            report.skipped += 1
            continue
        report.write(root, rel_path, content, args.dry_run)

    sweep = re.compile(SWEEP_PATTERN)
    skipped_dirs = {"build", ".git", ".gradle", ".idea", "debrand"}
    swept = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [name for name in dirnames if name not in skipped_dirs]
        for filename in filenames:
            if not filename.endswith(SWEEP_SUFFIXES):
                continue
            path = os.path.join(dirpath, filename)
            rel_path = os.path.relpath(path, root).replace(os.sep, "/")
            with open(path, "r", encoding="utf-8", newline="") as handle:
                text = handle.read()
            updated = sweep.sub(SWEEP_REPLACEMENT, text)
            if updated == text:
                continue
            swept += len(sweep.findall(text))
            report.write(root, rel_path, updated, args.dry_run)

    print(f"root: {root}")
    print(
        f"edits applied: {report.applied}, already converted: {report.skipped}, "
        f"mismatched: {len(report.missing)}, tags swept: {swept}"
    )
    for rel_path, what in report.missing:
        optional = rel_path.startswith(OPTIONAL_PREFIXES)
        print(f"  {'OPTIONAL' if optional else 'MISSING '}  {rel_path}: {what}")
    if args.dry_run:
        print("dry run: nothing written")
    else:
        print(f"files rewritten: {len(report.changed)}")
        for rel_path in sorted(report.changed):
            print(f"  wrote    {rel_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

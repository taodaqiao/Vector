/*
 * The names this build answers to.
 *
 * Upstream hard-codes them where they are used, which means renaming the framework
 * touches dozens of files and every one of them is a merge conflict waiting to
 * happen. Collecting them here keeps that cost in one place: a fork that wants
 * different names edits this file, and the rest of the tree stays upstream-shaped.
 *
 * Two rules for anything added below.
 *
 * First, nothing here may be read by the code being detected. These values are the
 * framework's own private naming; if a target app can observe a string, it belongs
 * in the code that faces the target, not in this table.
 *
 * Second, the user-visible strings are plain English rather than resource lookups
 * on purpose. Localised copies of "Vector status" would have to be edited in every
 * locale to stay neutral, which is exactly the merge surface this file exists to
 * avoid. A daemon notification is read by the person holding the phone, not by an
 * app, so a single neutral wording is enough.
 */
package org.matrix.vector.daemon

object Identity {
    /** Where the daemon keeps its state. Also the prefix of the two marker files below. */
    const val DATA_DIR = "/data/adb/sysupd"

    /** Marker that turns on member-name renaming; absent means off. */
    const val MEMBER_RENAME_MARKER = "rename_members"

    /** Marker that forces the in-process filter off, for bisecting a crash. */
    const val NO_SANITIZER_MARKER = "no_sanitizer"

    /** The dex the framework preloads from inside the module directory. */
    const val FRAMEWORK_DEX = "framework/core.dex"

    /** Name shown by `ctl --version` and in the version banner of the log. */
    const val CLI_NAME = "ctl"

    /** Dial *#*#873283#*#* to open the manager. Not a word: a phone keypad code. */
    const val DIAL_CODE = "873283"

    /** Notification channel ids. Stable, because a channel id cannot be renamed once created. */
    const val CHANNEL_STATUS = "sys_status"
    const val CHANNEL_UPDATED = "sys_module_updated"
    const val CHANNEL_SCOPE = "sys_module_scope"

    /** Notification channel names, shown in Settings and to notification listeners. */
    const val CHANNEL_STATUS_NAME = "System service"
    const val CHANNEL_UPDATED_NAME = "Module update complete"
    const val CHANNEL_SCOPE_NAME = "Scope request"

    /** Body of the ongoing notification. */
    const val NOTIFICATION_TITLE = "Service running"
    const val NOTIFICATION_CONTENT = "Tap the notification to open manager"
}

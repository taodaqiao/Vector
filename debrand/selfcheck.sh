#!/system/bin/sh
# Device-side probe for the identifying surfaces this patch is meant to remove.
# Run as root:  su -c 'sh selfcheck.sh [package.name]'
#
# A "[!!]" line is a leftover.  A "[ok]" line means the surface is clean or the
# string is legitimately part of the platform.
#
# The last section reads another process's /proc entries as root.  That view is
# NOT what a detecting app sees -- the sanitizer only filters reads made from
# inside the target process, so those lines are informational, not verdicts.
# See README.md section 4 for the in-process probe that is the real test.

set -u

PKG="${1:-}"
GREP='grep -Ei'

found=0
bad() { echo "[!!] $*"; found=$((found + 1)); }
ok() { echo "[ok] $*"; }
info() { echo "     $*"; }
head2() { echo; echo "== $* =="; }

head2 "public properties"
if getprop | $GREP 'dex2oat-flags|lsposed|vector|zygisk' | $GREP -v '^$' >/dev/null 2>&1; then
    getprop | $GREP 'dex2oat-flags|lsposed|vector|zygisk'
    bad "a readable property still carries the framework's name or flags"
else
    ok "no framework-shaped property"
fi

head2 "installed packages"
if pm list packages 2>/dev/null | $GREP 'vector|lsposed|perf_patch|sysutil' >/dev/null 2>&1; then
    pm list packages | $GREP 'vector|lsposed|perf_patch|sysutil'
    bad "package name is enumerable"
else
    ok "no framework-shaped package name"
fi

head2 "binder services"
if service list 2>/dev/null | $GREP 'serial|cmdex|vector|lspd' >/dev/null 2>&1; then
    service list | $GREP 'serial|cmdex|vector|lspd'
    bad "binder service name is enumerable"
else
    ok "no framework-shaped binder service"
fi

head2 "abstract unix sockets"
if $GREP 'lspd|vector|lsposed' /proc/net/unix >/dev/null 2>&1; then
    $GREP 'lspd|vector|lsposed' /proc/net/unix
    bad "/proc/net/unix carries a framework socket name"
else
    ok "/proc/net/unix clean"
fi

head2 "module and data directories"
for dir in /data/adb/modules/perf_patch /data/adb/sysupd; do
    if [ -e "$dir" ]; then
        info "$dir exists (root-only path; expected once installed)"
    else
        info "$dir absent"
    fi
done
ls -d /data/adb/modules/* 2>/dev/null | while read -r m; do
    info "module: $m"
done

head2 "logcat"
if logcat -d -t 400 2>/dev/null | $GREP 'vector|lsposed|xposed' >/dev/null 2>&1; then
    logcat -d -t 400 | $GREP 'vector|lsposed|xposed' | head -n 10
    bad "log buffer still mentions the framework"
else
    ok "log buffer clean"
fi

if [ -n "$PKG" ]; then
    pid=$(pidof "$PKG" 2>/dev/null | tr ' ' '\n' | head -n 1)
    head2 "target process: $PKG${pid:+ (pid $pid)}"
    if [ -z "$pid" ]; then
        info "not running; start the app first to inspect its threads and fd links"
    else
        leaked=$(ls /proc/"$pid"/task 2>/dev/null | while read -r tid; do
            cat /proc/"$pid"/task/"$tid"/comm 2>/dev/null
        done | $GREP 'vector|lspd|xposed|zygisk|lsplant|dobby')
        if [ -n "$leaked" ]; then
            echo "$leaked"
            bad "thread name leaks the framework"
        else
            ok "thread names clean"
        fi

        leakedfd=$(ls -l /proc/"$pid"/fd 2>/dev/null | $GREP -i 'lspd|vector|zygisk|adb/modules')
        if [ -n "$leakedfd" ]; then
            echo "$leakedfd"
            bad "fd link leaks the framework"
        else
            ok "fd links clean"
        fi

        # Root view: informational only, the sanitizer does not apply here.
        info "--- root view of maps (expected to leak; not what the app reads) ---"
        $GREP 'lspd|vector|zygisk|adb/modules|lsplant|dobby' /proc/"$pid"/maps 2>/dev/null |
            head -n 5
    fi
fi

echo
if [ "$found" -eq 0 ]; then
    echo "result: no externally visible leftover in the checked surfaces"
else
    echo "result: $found surface(s) still identifiable"
fi
exit 0

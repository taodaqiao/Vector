# Perf Patch

A runtime compatibility layer for Android 8.1 through 17.

## Install

Flash the module zip in your root manager and reboot. The companion app is
installed automatically under the name **Perf Patch**; if it does not appear,
its activity can be started from the root manager's module action button.

## Requirements

* Android 8.1 (API 27) or newer.
* A root manager that provides Zygisk.
* SELinux enforcing is supported; the module ships its own policy rules.

## Usage

Open the companion app to manage which applications the layer is active for.
Scope changes take effect after the target application is restarted.

## Troubleshooting

If the layer is inactive after a reboot:

* confirm the module is enabled in the root manager and that Zygisk is on;
* check that the companion app is installed, since the layer needs it to hand
  out its configuration;
* collect a log from inside the companion app and review the earliest entries,
  which is where load failures are reported.

## Uninstall

Disable or remove the module in the root manager and reboot. Removing the
module also removes its working directory.

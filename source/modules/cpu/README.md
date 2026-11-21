# Optional CPU wait optimization

The CPU module and its optimization both default to Off. To make the control
available in UB820 Helper, add this section to `sub/modules.cfg`:

```ini
[cpu]
enabled=1
optimize=off
```

Then choose **CPU optimization → On** in Helper. The choice is saved after the
player confirms the change. A saved choice in `sub/ub820_menu.cfg` takes precedence
over the initial value in `modules.cfg`, as with the other Helper settings.
If saving fails, the menu reports that the change was applied but not saved.

The kernel component is loaded into RAM only when optimization is first
requested. Off restores the original instructions; the inactive component
remains resident until the kernel restarts. The helper does not unload it or
repeat an uncertain activation automatically. An unavailable result is not
reported as a successful On or Off.

When a saved On is applied automatically at helper start, the helper first
writes `auto_on_armed=1` to the `[cpu]` section of `sub/ub820_menu.cfg`. It
clears the marker when a fresh check 30 minutes after the verified On still
finds it On, or when you choose Off; a clear the stick did not save is tried
again every minute. If the marker is still set at the next start (for example
after a freeze, or a power cycle within those 30 minutes), the
optimization is not turned on automatically; the menu says so and **On** can
still be chosen by hand. If the marker cannot be saved, the automatic On is
skipped. An unreadable menu configuration at startup also leaves automatic On
disabled for that run. You can still select On in the menu.

If the kernel component reports an unexpected state while its identity is
verified, choosing **Off** still restores the original instructions. Other
states need a player restart.

This is experimental. Firmware 1.82 has passed short tests of menus, remote and
front-panel controls, Blu-ray playback, external subtitles, pause, seeking and
chapter changes. Firmware 1.85 has matching build guards but has not been tested
with this CPU optimization on a player. Extended playback and power/standby/CEC
behavior remain to be verified.

The immutable kernel artifact, its source, license and reproducible build inputs
are documented in [kernel/README.md](kernel/README.md). Kernel operations run in
a separate worker so that the Helper event loop can continue responding.

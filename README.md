# iss — Instant Space Switcher

Eliminates the macOS sliding animation when 3-finger swiping between spaces.

## How it works

`iss` installs a session-level `CGEventTap` for the private DockControl and companion gesture events used by horizontal trackpad swipes. It suppresses the real horizontal swipe, detects its direction, and posts a synthetic gesture sequence. Vertical swipes, including Mission Control and App Exposé, pass through.

On macOS versions before 27, the synthetic sequence uses the original DockControl fields and high velocity. On macOS 27 and later, `CGEventPost` events need an embedded raw IOHID queue payload in serialized CGEvent field 4205. `iss` constructs the payload, including the fluid-touch and velocity records, appends it to each synthetic phase, and posts the augmented events.

Each DockControl event is paired with a companion gesture event. A passthrough counter lets those synthetic events pass through the tap without being intercepted again. The real terminal event is also allowed to complete the Dock’s native gesture state on macOS 27.

The macOS 27 path deliberately does not pre-check space boundaries with `CGSGetActiveSpace()`. That API can lag behind the Dock after a synthetic switch, so the Dock itself handles attempts to move past the first or last space.

No SIP disable or code injection is required. The event type and field indices are undocumented system details, and the macOS 27 IOHID payload layout is reverse-engineered.

## Compatibility

The pre-27 event path supports the macOS versions where the original synthetic DockControl mechanism works. macOS 27 and later use the serialized IOHID payload path described above. Direction encoding differs between macOS 26 and macOS 27, so the running OS—not the SDK used to build `iss`—determines the interpretation.

## Install

```
# builds, installs to ~/.local/bin/iss, starts launch agent
# use sudo make install PREFIX=/usr/local if you want but it's not necessary
make install
```

Runs automatically at login via launchd. Grant Accessibility permission when prompted.

## Uninstall

```
make uninstall
```

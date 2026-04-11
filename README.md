# iss — Instant Space Switcher

Eliminates the macOS sliding animation when 3-finger swiping between spaces.

## How it works

A CGEventTap intercepts trackpad dock-swipe gestures before the Dock sees them, suppresses the originals, and posts synthetic Begin+End gesture events with high velocity (±400). The Dock treats these as instantaneous swipes and switches spaces with no animation.

**No SIP disable required.** CGEventTap and CGEventPost are public, supported APIs — the same mechanism macOS uses for Accessibility features. No code injection, no DYLD tricks, no system file modification. The only undocumented parts are the CGEvent field indices (55, 110, 132, etc.) used to read/write gesture metadata, which are just integer constants passed to public functions. SIP has nothing to protect here.

Vertical swipes (Mission Control, App Exposé) are left untouched.

## Compatibility

Should work on all macOS since 10.11 El Capitan.
The private event field indices and dock-swipe HID type have been stable since 3-finger swipe was introduced.

## Install

```bash
# builds, installs to ~/.local/bin/iss, starts launch agent
# use sudo make install PREFIX=/usr/local if you want but it's not necessary

# (if needed: xcode-select --install)
git clone https://github.com/joshuarli/iss.git
cd iss
make install
```

Runs automatically at login via launchd. Grant Accessibility permission when prompted.

## Uninstall

```bash
# from the cloned iss repository
make uninstall
```

// iss — Instant Space Switcher
//
// Eliminates the macOS sliding animation when 3-finger swiping between spaces.
//
// How it works:
//   1. A CGEventTap intercepts trackpad dock-swipe gesture events (private
//      CGS event type 30, HID type 23) before the Dock sees them.
//   2. Real gesture events are suppressed (callback returns NULL).
//   3. On the first event with a clear direction, a synthetic Begin+End
//      gesture pair is posted with high velocity (±400), causing the Dock
//      to switch spaces instantly — no animation.
//   4. A passthrough counter prevents the tap from re-intercepting its own
//      synthetic events (CGEvent field tags don't survive CGEventPost).
//   5. Companion kCGSEventGesture events are also suppressed during an
//      active swipe to keep the event stream consistent.
//
// Vertical swipes (Mission Control, App Exposé) are left untouched.
// Does not require disabling SIP.

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <float.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>

// These are private CGEvent fields used by the WindowServer and Dock for
// trackpad gesture routing. They were discovered via reverse engineering.
static const CGEventField kCGSEventTypeField            = 55;  // real event type
static const CGEventField kCGEventGestureHIDType        = 110; // IOHIDEventType
static const CGEventField kCGEventGestureScrollY        = 119;
static const CGEventField kCGEventGestureSwipeMotion    = 123; // horiz vs vert
static const CGEventField kCGEventGestureSwipeProgress  = 124; // cumulative distance
static const CGEventField kCGEventGestureSwipeVelocityX = 129;
static const CGEventField kCGEventGestureSwipeVelocityY = 130;
static const CGEventField kCGEventGesturePhase          = 132; // began/changed/ended
static const CGEventField kCGEventScrollGestureFlagBits = 135; // direction hint
static const CGEventField kCGEventGestureZoomDeltaX     = 139; // required, reason unknown

enum { kCGSEventGesture = 29, kCGSEventDockControl = 30 };
enum { kIOHIDEventTypeDockSwipe = 23 };
enum { kCGGestureMotionHorizontal = 1 };
enum { kGestureBegan = 1, kGestureChanged = 2, kGestureEnded = 4, kGestureCancelled = 8 };

extern int CGSMainConnectionID(void);
extern uint64_t CGSGetActiveSpace(int cid);
extern CFArrayRef CGSCopyManagedDisplaySpaces(int cid);

// macOS 27 validates synthetic dock swipes against this serialized IOHID
// queue payload, which is attached to CGEvent field 4205.
#pragma pack(push, 1)

typedef struct {
    uint32_t size;
    uint32_t type;
    uint32_t options;
    uint8_t depth;
    uint8_t reserved[3];
} IOHIDEventBase;

typedef struct {
    IOHIDEventBase base;
    int32_t position_x;
    int32_t position_y;
    int32_t position_z;
    uint32_t swipe_mask;
    uint16_t gesture_motion;
    uint16_t gesture_flavor;
    int32_t swipe_progress;
} IOHIDFluidTouchGestureData;

typedef struct {
    IOHIDEventBase base;
    int32_t velocity_x;
    int32_t velocity_y;
    int32_t velocity_z;
} IOHIDVelocityEventData;

typedef struct {
    uint64_t timestamp;
    uint64_t sender_id;
    uint32_t options;
    uint32_t attribute_length;
    uint32_t event_count;
} IOHIDSystemQueueElementHeader;

#pragma pack(pop)

static const uint32_t kIOHIDEventTypeVelocity = 9;
static const uint32_t kIOHIDEventTypeFluidTouchGesture = 23;
static const uint16_t kIOHIDGestureFlavorDockPrimary = 3;

static int32_t double_to_fixed1616(double value) {
    int32_t fixed = (int32_t)(value * 65536.0);
    if (fixed == 0 && value != 0.0) return value > 0.0 ? 1 : -1;
    return fixed;
}

static uint8_t *generate_iohid_payload(CGEventRef event, size_t *out_length) {
    int64_t phase = CGEventGetIntegerValueField(event, (CGEventField)132);
    int64_t motion = CGEventGetIntegerValueField(event, (CGEventField)123);
    double progress = CGEventGetDoubleValueField(event, (CGEventField)124);
    double pos_x = CGEventGetDoubleValueField(event, (CGEventField)125);
    double pos_y = CGEventGetDoubleValueField(event, (CGEventField)126);
    double vel_x = CGEventGetDoubleValueField(event, (CGEventField)129);
    double vel_y = CGEventGetDoubleValueField(event, (CGEventField)130);
    int64_t swipe_mask = CGEventGetIntegerValueField(event, (CGEventField)115);

    bool include_velocity = (vel_x != 0.0 || vel_y != 0.0 || phase == 4);
    uint32_t event_count = include_velocity ? 2 : 1;
    size_t payload_length = sizeof(IOHIDSystemQueueElementHeader)
                          + sizeof(IOHIDFluidTouchGestureData);
    if (include_velocity) payload_length += sizeof(IOHIDVelocityEventData);

    uint8_t *payload = malloc(payload_length);
    if (!payload) return NULL;
    memset(payload, 0, payload_length);

    IOHIDSystemQueueElementHeader *header = (IOHIDSystemQueueElementHeader *)payload;
    uint64_t timestamp = CGEventGetTimestamp(event);
    header->timestamp = timestamp ? timestamp : mach_absolute_time();
    header->event_count = event_count;

    IOHIDFluidTouchGestureData *fluid =
        (IOHIDFluidTouchGestureData *)(payload + sizeof(IOHIDSystemQueueElementHeader));
    fluid->base.size = sizeof(IOHIDFluidTouchGestureData);
    fluid->base.type = kIOHIDEventTypeFluidTouchGesture;
    fluid->base.options = (uint32_t)((phase & 0xFF) << 24);
    fluid->position_x = double_to_fixed1616(pos_x);
    fluid->position_y = double_to_fixed1616(pos_y);
    fluid->swipe_mask = (uint32_t)swipe_mask;
    fluid->gesture_motion = (uint16_t)motion;
    fluid->gesture_flavor = kIOHIDGestureFlavorDockPrimary;
    fluid->swipe_progress = double_to_fixed1616(progress);

    if (include_velocity) {
        IOHIDVelocityEventData *velocity = (IOHIDVelocityEventData *)
            (payload + sizeof(IOHIDSystemQueueElementHeader)
             + sizeof(IOHIDFluidTouchGestureData));
        velocity->base.size = sizeof(IOHIDVelocityEventData);
        velocity->base.type = kIOHIDEventTypeVelocity;
        velocity->base.depth = 1;
        velocity->velocity_x = double_to_fixed1616(vel_x);
        velocity->velocity_y = double_to_fixed1616(vel_y);
    }

    *out_length = payload_length;
    return payload;
}

// Adds the raw IOHID payload needed for synthetic dock swipes on macOS 27.
static CGEventRef augment_dock_swipe_event(CGEventRef event) {
    if (!event) return NULL;

    CFDataRef data = CGEventCreateData(kCFAllocatorDefault, event);
    if (!data) return NULL;

    const uint8_t *bytes = CFDataGetBytePtr(data);
    CFIndex length = CFDataGetLength(data);
    if (length < 4 || bytes[0] != 0 || bytes[1] != 0
        || bytes[2] != 0 || bytes[3] != 2) {
        CFRelease(data);
        return NULL;
    }

    size_t payload_length = 0;
    uint8_t *payload = generate_iohid_payload(event, &payload_length);
    if (!payload) {
        CFRelease(data);
        return NULL;
    }

    size_t new_length = (size_t)length + 4 + payload_length;
    uint8_t *new_bytes = malloc(new_length);
    if (!new_bytes) {
        free(payload);
        CFRelease(data);
        return NULL;
    }

    memcpy(new_bytes, bytes, length);
    new_bytes[length] = (uint8_t)(payload_length >> 8);
    new_bytes[length + 1] = (uint8_t)payload_length;
    new_bytes[length + 2] = (uint8_t)(4205 >> 8);
    new_bytes[length + 3] = (uint8_t)4205;
    memcpy(new_bytes + length + 4, payload, payload_length);

    free(payload);
    CFRelease(data);

    CFDataRef new_data = CFDataCreate(kCFAllocatorDefault, new_bytes, (CFIndex)new_length);
    free(new_bytes);
    if (!new_data) return NULL;

    CGEventRef result = CGEventCreateFromData(kCFAllocatorDefault, new_data);
    CFRelease(new_data);
    return result;
}

static bool requires_event_augmentation(void) {
    static int cached_result = -1;
    if (cached_result != -1) return cached_result;

    const char *force_override = getenv("ISS_FORCE_EVENT_AUGMENTATION");
    if (force_override) {
        cached_result = strcmp(force_override, "1") == 0;
        return cached_result;
    }

    char version[32];
    size_t size = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &size, NULL, 0) != 0) {
        cached_result = 0;
        return false;
    }

    int major = 0, minor = 0, patch = 0;
    if (sscanf(version, "%d.%d.%d", &major, &minor, &patch) < 1) {
        cached_result = 0;
        return false;
    }

    cached_result = major >= 27;
    return cached_result;
}

static CFMachPortRef tap;
static bool swipeTracking, swipeFired;
static int passthrough; // synthetic events remaining to let through

// Create a DockControl event with fields common to both Begin and End phases.
static CGEventRef make_dock_event(int phase, bool right) {
    CGEventRef ev = CGEventCreate(NULL);
    if (!ev) return NULL;
    CGEventSetIntegerValueField(ev, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(ev, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(ev, kCGEventGesturePhase, phase);
    // Empirically, ±FLT_TRUE_MIN here makes switching instant.
    const float flagsProgress = right ? FLT_TRUE_MIN : -FLT_TRUE_MIN;
    int32_t scrollGestureFlagDirection;
    memcpy(&scrollGestureFlagDirection, &flagsProgress, sizeof(scrollGestureFlagDirection));
    CGEventSetIntegerValueField(ev, kCGEventScrollGestureFlagBits, scrollGestureFlagDirection);
    CGEventSetIntegerValueField(ev, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    CGEventSetDoubleValueField(ev, kCGEventGestureScrollY, 0);
    CGEventSetDoubleValueField(ev, kCGEventGestureZoomDeltaX, FLT_TRUE_MIN);
    return ev;
}

static CGEventRef make_augmented_dock_event(int phase, bool right) {
    CGEventRef ev = CGEventCreate(NULL);
    if (!ev) return NULL;

    CGEventSetIntegerValueField(ev, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(ev, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(ev, kCGEventGesturePhase, phase);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeProgress, right ? -1.0 : 1.0);
    CGEventSetIntegerValueField(ev, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    CGEventSetIntegerValueField(ev, (CGEventField)134, phase);
    CGEventSetDoubleValueField(ev, (CGEventField)138, 3.0);
    CGEventSetDoubleValueField(ev, (CGEventField)169, (double)mach_absolute_time());
    CGEventSetDoubleValueField(ev, (CGEventField)125, 0.1);
    if (phase == kGestureEnded) {
        CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityX,
                                   right ? -9999.0 : 9999.0);
    }
    return ev;
}

// Post a paired (companion gesture + dock control) event to the session tap.
static bool post_pair(CGEventRef dock) {
    CGEventRef companion = CGEventCreate(NULL);
    if (!companion) { CFRelease(dock); return false; }
    CGEventSetIntegerValueField(companion, kCGSEventTypeField, kCGSEventGesture);
    CGEventPost(kCGSessionEventTap, dock);
    CGEventPost(kCGSessionEventTap, companion);
    CFRelease(dock); CFRelease(companion);
    return true;
}

// Check whether there is a space to switch to in the given direction.
// Queries the private CGS API for the per-display space list and finds
// the active space's position within it.
static bool can_switch(bool right) {
    int cid = CGSMainConnectionID();
    uint64_t active = CGSGetActiveSpace(cid);
    CFArrayRef displays = CGSCopyManagedDisplaySpaces(cid);
    if (!displays) return true;

    bool can = true;
    for (CFIndex i = 0; i < CFArrayGetCount(displays); i++) {
        CFDictionaryRef display = CFArrayGetValueAtIndex(displays, i);
        CFArrayRef spaces = CFDictionaryGetValue(display, CFSTR("Spaces"));
        if (!spaces) continue;
        CFIndex count = CFArrayGetCount(spaces);
        for (CFIndex j = 0; j < count; j++) {
            CFDictionaryRef space = CFArrayGetValueAtIndex(spaces, j);
            CFNumberRef sid = CFDictionaryGetValue(space, CFSTR("ManagedSpaceID"));
            if (!sid) continue;
            int64_t val;
            CFNumberGetValue(sid, kCFNumberSInt64Type, &val);
            if ((uint64_t)val == active) {
                if (right && j == count - 1) can = false;
                if (!right && j == 0) can = false;
                goto done;
            }
        }
    }
done:
    CFRelease(displays);
    return can;
}

static void post_augmented_switch(bool right) {
    CGEventRef events[3] = { NULL, NULL, NULL };
    const int phases[3] = { kGestureBegan, kGestureChanged, kGestureEnded };

    for (int i = 0; i < 3; i++) {
        CGEventRef event = make_augmented_dock_event(phases[i], right);
        if (!event) goto cleanup;
        events[i] = augment_dock_swipe_event(event);
        CFRelease(event);
        if (!events[i]) goto cleanup;
    }

    passthrough += 6;
    for (int i = 0; i < 3; i++) {
        post_pair(events[i]);
        events[i] = NULL;
    }
    return;

cleanup:
    for (int i = 0; i < 3; i++) {
        if (events[i]) CFRelease(events[i]);
    }
}

static void post_switch(bool right) {
    bool augmented = requires_event_augmentation();
    if (!augmented && !can_switch(right)) return;

    if (augmented) {
        post_augmented_switch(right);
        return;
    }

    double sign = right ? 1.0 : -1.0;
    CGEventRef begin = make_dock_event(kGestureBegan, right);
    if (!begin) return;

    CGEventRef changed = make_dock_event(kGestureChanged, right);
    if (!changed) { CFRelease(begin); return; }

    CGEventRef end = make_dock_event(kGestureEnded, right);
    if (!end) { CFRelease(begin); CFRelease(changed); return; }
    CGEventSetDoubleValueField(end, kCGEventGestureSwipeVelocityX, sign * 400.0);
    CGEventSetDoubleValueField(end, kCGEventGestureSwipeVelocityY, 0);

    passthrough += 6; // 3 pairs × 2 events
    post_pair(begin);
    post_pair(changed);
    post_pair(end);
}

// Intercepts real horizontal dock swipes. Direction is determined from swipe
// progress (Changed phase) or velocity (Ended phase, fallback for discrete
// swipes that skip Changed entirely). Returns NULL to suppress the original
// event, or ev to pass it through.
static CGEventRef cb(CGEventTapProxy proxy, CGEventType type, CGEventRef ev, void *ctx) {
    (void)proxy; (void)ctx;

    // System disabled our tap (callback too slow) — re-enable.
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        CGEventTapEnable(tap, true);
        return ev;
    }

    int et = (int)CGEventGetIntegerValueField(ev, kCGSEventTypeField);

    // Let our own synthetic events pass through untouched.
    if (passthrough > 0 && (et == kCGSEventDockControl || et == kCGSEventGesture)) {
        passthrough--;
        return ev;
    }

    // Only intercept horizontal dock swipes (not Mission Control, App Exposé, etc.).
    if (et == kCGSEventDockControl
        && (int)CGEventGetIntegerValueField(ev, kCGEventGestureHIDType) == kIOHIDEventTypeDockSwipe
        && (int)CGEventGetIntegerValueField(ev, kCGEventGestureSwipeMotion) == kCGGestureMotionHorizontal) {

        int phase = (int)CGEventGetIntegerValueField(ev, kCGEventGesturePhase);

        if (phase == kGestureBegan) {
            swipeTracking = true; swipeFired = false; return NULL;
        }
        if (phase == kGestureChanged && swipeTracking) {
            if (!swipeFired) {
                double p = CGEventGetDoubleValueField(ev, kCGEventGestureSwipeProgress);
                if (p != 0.0) { swipeFired = true; post_switch(p < 0); }
            }
            return NULL;
        }
        if (phase == kGestureEnded && swipeTracking) {
            if (!swipeFired) {
                double v = CGEventGetDoubleValueField(ev, kCGEventGestureSwipeVelocityX);
                if (v != 0.0) post_switch(v < 0);
            }
            swipeTracking = swipeFired = false;
            if (requires_event_augmentation()) {
                // Let the Dock close its native gesture state after the
                // synthetic macOS 27 event sequence has switched spaces.
                CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityX, 0);
                CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityY, 0);
                CGEventSetDoubleValueField(ev, kCGEventGestureSwipeProgress, 0);
                return ev;
            }
            return NULL;
        }
        if (phase == kGestureCancelled) {
            swipeTracking = swipeFired = false; return NULL;
        }
        return swipeTracking ? NULL : ev;
    }

    // Suppress companion gesture events paired with the dock swipe.
    if (et == kCGSEventGesture && swipeTracking) return NULL;
    return ev;
}

static volatile sig_atomic_t running = 1;
static void stop(int s) { (void)s; running = 0; }

int main(void) {
    // Prompt for Accessibility permission if not already granted.
    const void *k[] = { kAXTrustedCheckOptionPrompt }, *v[] = { kCFBooleanTrue };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, k, v, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    bool ok = AXIsProcessTrustedWithOptions(opts);
    CFRelease(opts);
    if (!ok) { fprintf(stderr, "Grant Accessibility permission, then re-run.\n"); return 1; }

    // Listen for gesture + dock control events.
    tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        (1ULL << kCGSEventGesture) | (1ULL << kCGSEventDockControl), cb, NULL);
    if (!tap) { fprintf(stderr, "Failed to create event tap.\n"); return 1; }

    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(NULL, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    signal(SIGINT, stop); signal(SIGTERM, stop);

    fprintf(stderr, "iss: instant swipe active\n");
    while (running) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);

    CGEventTapEnable(tap, false);
    CFRunLoopRemoveSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
    CFRelease(src); CFRelease(tap);
    return 0;
}

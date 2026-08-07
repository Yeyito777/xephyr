#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
    Display *display;
    XIEventMask event_mask;
    unsigned char mask[XIMaskLen(XI_RawMotion)] = { 0 };
    int xi_opcode, event_base, error_base;
    int major = 2, minor = 0;
    int grab_pointer = 0;

    if (argc == 3 && strcmp(argv[2], "--grab") == 0)
        grab_pointer = 1;
    else if (argc != 2) {
        fprintf(stderr, "usage: %s DISPLAY [--grab]\n", argv[0]);
        return 2;
    }

    display = XOpenDisplay(argv[1]);
    if (!display) {
        fprintf(stderr, "could not open display %s\n", argv[1]);
        return 1;
    }

    if (!XQueryExtension(display, "XInputExtension", &xi_opcode,
                         &event_base, &error_base) ||
        XIQueryVersion(display, &major, &minor) != Success) {
        fprintf(stderr, "display %s does not provide XInput2\n", argv[1]);
        return 1;
    }

    event_mask.deviceid = XIAllMasterDevices;
    event_mask.mask_len = sizeof(mask);
    event_mask.mask = mask;
    XISetMask(mask, XI_RawMotion);
    XISelectEvents(display, DefaultRootWindow(display), &event_mask, 1);

    if (grab_pointer &&
        XGrabPointer(display, DefaultRootWindow(display), True,
                     PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync,
                     DefaultRootWindow(display), None,
                     CurrentTime) != GrabSuccess) {
        fprintf(stderr, "guest XGrabPointer failed\n");
        return 1;
    }
    XSync(display, False);

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("READY xi=%d.%d grabbed=%d\n", major, minor, grab_pointer);

    for (;;) {
        XEvent event;

        XNextEvent(display, &event);
        if (event.xcookie.type != GenericEvent ||
            event.xcookie.extension != xi_opcode ||
            !XGetEventData(display, &event.xcookie)) {
            continue;
        }

        if (event.xcookie.evtype == XI_RawMotion) {
            XIRawEvent *raw = event.xcookie.data;
            const double *values = raw->raw_values;
            double dx = 0.0, dy = 0.0;
            int axis;

            for (axis = 0; axis < raw->valuators.mask_len * 8; axis++) {
                if (!XIMaskIsSet(raw->valuators.mask, axis))
                    continue;
                if (axis == 0)
                    dx = *values;
                else if (axis == 1)
                    dy = *values;
                values++;
            }

            printf("RAW dx=%.3f dy=%.3f source=%d\n",
                   dx, dy, raw->sourceid);
        }

        XFreeEventData(display, &event.xcookie);
    }
}

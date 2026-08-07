#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Window
find_named_window(Display *display, Window parent, const char *title)
{
    Window root, parent_return, *children = NULL;
    unsigned int nchildren = 0, i;
    char *name = NULL;
    Window found = None;

    if (XFetchName(display, parent, &name) && name) {
        if (strcmp(name, title) == 0)
            found = parent;
        XFree(name);
        if (found != None)
            return found;
    }

    if (!XQueryTree(display, parent, &root, &parent_return,
                    &children, &nchildren)) {
        return None;
    }

    for (i = 0; i < nchildren && found == None; i++)
        found = find_named_window(display, children[i], title);

    if (children)
        XFree(children);
    return found;
}

static int
print_pointer_position(Display *display, Window window)
{
    Window root_return, child_return;
    int root_x, root_y, window_x, window_y;
    unsigned int state;

    if (!XQueryPointer(display, window, &root_return, &child_return,
                       &root_x, &root_y, &window_x, &window_y, &state)) {
        fprintf(stderr, "pointer is not on the same screen\n");
        return 1;
    }
    printf("%d %d\n", window_x, window_y);
    return 0;
}

int
main(int argc, char **argv)
{
    Display *display;
    Window root, window = None;
    const char *command;

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s DISPLAY window-move TITLE X Y\n"
                "       %s DISPLAY window-query TITLE\n"
                "       %s DISPLAY root-warp X Y\n"
                "       %s DISPLAY root-query\n",
                argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }

    display = XOpenDisplay(argv[1]);
    if (!display) {
        fprintf(stderr, "could not open display %s\n", argv[1]);
        return 1;
    }

    root = DefaultRootWindow(display);
    command = argv[2];

    if (strcmp(command, "root-warp") == 0) {
        if (argc != 5)
            return 2;
        XWarpPointer(display, None, root, 0, 0, 0, 0,
                     atoi(argv[3]), atoi(argv[4]));
        XSync(display, False);
        return 0;
    }
    else if (strcmp(command, "root-query") == 0) {
        if (argc != 3)
            return 2;
        return print_pointer_position(display, root);
    }

    if ((strcmp(command, "window-move") == 0 && argc != 6) ||
        (strcmp(command, "window-query") == 0 && argc != 4)) {
        return 2;
    }

    window = find_named_window(display, root, argv[3]);
    if (window == None) {
        fprintf(stderr, "could not find window titled %s\n", argv[3]);
        return 1;
    }

    if (strcmp(command, "window-move") == 0) {
        XWarpPointer(display, None, window, 0, 0, 0, 0,
                     atoi(argv[4]), atoi(argv[5]));
        XSync(display, False);
        return 0;
    }
    else {
        return print_pointer_position(display, window);
    }
}

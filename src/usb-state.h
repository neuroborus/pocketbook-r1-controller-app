#ifndef USB_STATE_H
#define USB_STATE_H

#include <stdint.h>
#include <string.h>

enum usb_state { USB_UNKNOWN = -1, USB_DISCONNECTED = 0, USB_CONNECTED = 1 };

static enum usb_state usb_parse_state(const char *text)
{
    size_t len = strcspn(text, "\r\n");
    if (len == 12 && !strncmp(text, "DISCONNECTED", len)) return USB_DISCONNECTED;
    if ((len == 10 && !strncmp(text, "CONFIGURED", len)) ||
        (len == 9 && !strncmp(text, "CONNECTED", len))) return USB_CONNECTED;
    return USB_UNKNOWN;
}

struct usb_disconnect_gate { uint64_t since; int pending; };

static int usb_disconnect_ready(struct usb_disconnect_gate *gate, enum usb_state state, uint64_t now)
{
    if (state != USB_DISCONNECTED) {
        gate->pending = 0;
        return 0;
    }
    if (!gate->pending) {
        gate->pending = 1;
        gate->since = now;
    }
    return now - gate->since >= 1000;
}

#endif

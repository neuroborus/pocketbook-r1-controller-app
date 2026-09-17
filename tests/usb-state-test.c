#include <assert.h>
#include <stdio.h>
#include "../src/usb-state.h"

int main(void)
{
    assert(usb_parse_state("CONFIGURED\n") == USB_CONNECTED);
    assert(usb_parse_state("CONNECTED\n") == USB_CONNECTED);
    assert(usb_parse_state("DISCONNECTED\n") == USB_DISCONNECTED);
    assert(usb_parse_state("DISCONNECTED\r\n") == USB_DISCONNECTED);
    assert(usb_parse_state("") == USB_UNKNOWN);
    assert(usb_parse_state("DISCONNECTED-invalid") == USB_UNKNOWN);
    struct usb_disconnect_gate gate = {0};
    assert(!usb_disconnect_ready(&gate, USB_CONNECTED, 0));
    assert(!usb_disconnect_ready(&gate, USB_CONNECTED, 900001)); /* Former deadline. */
    assert(!usb_disconnect_ready(&gate, USB_UNKNOWN, 900002));
    assert(!usb_disconnect_ready(&gate, USB_DISCONNECTED, 900100));
    assert(!usb_disconnect_ready(&gate, USB_CONNECTED, 900500)); /* Transient disconnect. */
    assert(!usb_disconnect_ready(&gate, USB_DISCONNECTED, 901000));
    assert(!usb_disconnect_ready(&gate, USB_UNKNOWN, 901500)); /* Transient sysfs loss. */
    assert(!usb_disconnect_ready(&gate, USB_DISCONNECTED, 902000));
    assert(!usb_disconnect_ready(&gate, USB_DISCONNECTED, 902999));
    assert(usb_disconnect_ready(&gate, USB_DISCONNECTED, 903000));
    assert(!usb_disconnect_ready(&gate, USB_CONNECTED, 903001));
    puts("PASS: connected handshake, missing/truncated state, transient disconnect, stable disconnect, and waiting beyond 15 minutes");
    return 0;
}

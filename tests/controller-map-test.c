#include <assert.h>
#include <stdio.h>
#include "../src/controller-map.h"
static struct controller_map m;
static int event(int type,int code,int value,uint64_t now)
{
    struct input_event e={.type=(uint16_t)type,.code=(uint16_t)code,.value=value};
    return controller_map_event(&m,&e,now);
}
static int key(int code,uint64_t now)
{
    assert(!event(EV_KEY,code,1,now));
    assert(!event(EV_KEY,code,2,now+10));
    return event(EV_KEY,code,0,now+70);
}
int main(void)
{
    controller_map_reset(&m);
    assert(!event(EV_KEY,304,0,0));
    assert(key(304,100)==104 && key(305,200)==109 && key(114,300)==158);
    assert(key(BTN_LEFT,400)==0 && key(KEY_VOLUMEUP,500)==0 && key(BTN_RIGHT,600)==0);
    assert(event(EV_ABS,ABS_X,0,1000)==CONTROLLER_NAV+0x13);
    assert(!event(EV_ABS,ABS_X,0,1010));
    assert(!event(EV_ABS,ABS_X,1,1020));
    assert(event(EV_ABS,ABS_X,2,1030)==CONTROLLER_NAV+0x14);
    assert(event(EV_ABS,ABS_Y,0,1040)==CONTROLLER_NAV+0x11);
    assert(event(EV_ABS,ABS_Y,2,1050)==CONTROLLER_NAV+0x12);
    assert(!event(EV_ABS,ABS_Y,9,1060));
    assert(key(308,1400)==104 && key(304,1500)==109);
    assert(key(307,1600)==116 && key(305,1700)==158 && key(310,1800)==CONTROLLER_NAV+0x0a);
    assert(key(313,1900)==0 && key(312,2000)==0);
    /* Release after a mode change must not turn M+D into a Back command. */
    assert(key(114,2100)==0);
    assert(key(114,2500)==158);
    assert(!event(EV_KEY,304,1,2700));
    controller_map_reset(&m);
    assert(!event(EV_KEY,304,0,2800));
    puts("PASS: mouse/gamepad mappings, navigation edges, mode-switch guard, companions, repeats and reconnect release safety");
}

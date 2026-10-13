/*
 * Miyoo Mini (Plus) input: reads the buttons straight from evdev
 * (/dev/input/event0) and fills controller 1 the way the N64 game expects.
 *
 *   A       -> A (accelerate / confirm)      B      -> B (brake / back)
 *   R1, R2  -> R (hop / drift)               L1, L2 -> Z (item)
 *   Y       -> C-up (camera)                 X      -> C-right
 *   START   -> START                         SELECT -> L (and L1 outside races)
 *   D-pad   -> analog stick (steering ramps up while held) + D-pad bits
 *   MENU alone: a press, a long press (0.7 s) and a double press (within 0.3 s) do what
 *   OnionOS's Tweaks set for games (by default: the Game Switcher, back to the menu, the
 *   previous game), the state being saved first; MENU with another key (OnionOS's
 *   MENU+POWER screenshot, MENU+volume brightness...) does none of them
 *   MENU+R2 -> save state, MENU+L2 -> load state (as in OnionOS's RetroArch)
 *   SELECT+START held 1 s -> quit
 *   (port/savestate_miyoo.c does all of these between two frames)
 *   X held, then SELECT -> save a capture of the next frame (capture_NN.bin, for debugging)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>

#include <ultra64.h>
#include <macros.h>
#include <common_structs.h>
#include "main.h"
#include <defines.h>
#include "port_miyoo.h"

#define N64_A      0x8000
#define N64_B      0x4000
#define N64_Z      0x2000
#define N64_START  0x1000
#define N64_DU     0x0800
#define N64_DD     0x0400
#define N64_DL     0x0200
#define N64_DR     0x0100
#define N64_L      0x0020
#define N64_R      0x0010
#define N64_CU     0x0008
#define N64_CD     0x0004
#define N64_CL     0x0002
#define N64_CR     0x0001

enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_X, K_Y, K_L1, K_R1, K_L2, K_R2, K_START, K_SELECT, K_MENU, K_COUNT };

static int ev_fd = -1;
static unsigned char held[K_COUNT];
static double quit_since;
static int quit_flag;
/* MENU on its own, timed as OnionOS's keymon times it for RetroArch: a press shorter than
   0.7 s is a single press (held longer: a long press, at 0.7 s); a second press within 0.3 s
   of the first is a double press (the single press waits that long only when a double press
   does something). No other key (mapped or not: POWER, volume...) may be down meanwhile. */
#define MENU_LONG_MS 700.0
#define MENU_DOUBLE_MS 300.0
static unsigned char down_any[KEY_MAX / 8 + 1];   /* every key code down now */
static int menu_down, menu_mixed, menu_long_done, menu_single_wait, menu_skip_release;
static double menu_press_t;

static int others_down(void) {
    for (int i = 0; i < (int) sizeof(down_any); i++) {
        unsigned char m = down_any[i];
        if (i == KEY_ESC / 8)
            m &= (unsigned char) ~(1u << (KEY_ESC % 8));
        if (m)
            return 1;
    }
    return 0;
}

static int key_index(int code) {
    switch (code) {
        case KEY_UP: return K_UP;
        case KEY_DOWN: return K_DOWN;
        case KEY_LEFT: return K_LEFT;
        case KEY_RIGHT: return K_RIGHT;
        case KEY_SPACE: return K_A;
        case KEY_LEFTCTRL: return K_B;
        case KEY_LEFTSHIFT: return K_X;
        case KEY_LEFTALT: return K_Y;
        case KEY_E: return K_L1;
        case KEY_T: return K_R1;
        case KEY_TAB: return K_L2;
        case KEY_BACKSPACE: return K_R2;
        case KEY_ENTER: return K_START;
        case KEY_RIGHTCTRL: return K_SELECT;
        case KEY_ESC: return K_MENU;
        default: return -1;
    }
}

void port_input_init(void) {
    ev_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (ev_fd < 0)
        printf("input: sem /dev/input/event0\n");
}

void port_input_poll(void) {
    struct input_event ev[32];
    if (ev_fd < 0)
        return;
    for (;;) {
        ssize_t r = read(ev_fd, ev, sizeof(ev));
        if (r <= 0)
            break;
        int n = (int) (r / sizeof(ev[0]));
        for (int i = 0; i < n; i++) {
            if (ev[i].type != EV_KEY)
                continue;
            const int code = ev[i].code;
            if (code >= 0 && code < KEY_MAX) {
                if (ev[i].value) down_any[code / 8] |= (unsigned char) (1u << (code % 8));
                else down_any[code / 8] &= (unsigned char) ~(1u << (code % 8));
            }
            if (code == KEY_ESC && ev[i].value == 1) {          /* MENU pressed */
                const double now = port_now_ms();
                if (menu_single_wait && now - menu_press_t <= MENU_DOUBLE_MS) {
                    menu_single_wait = 0;                       /* the second press: double */
                    menu_skip_release = 1;
                    port_state_menu(PORT_MENU_DOUBLE);
                } else {
                    menu_skip_release = 0;
                }
                menu_down = 1;
                menu_mixed = others_down();
                menu_long_done = 0;
                menu_press_t = now;
            } else if (code == KEY_ESC && ev[i].value == 0) {   /* MENU released */
                if (menu_down && !menu_mixed && !menu_long_done && !menu_skip_release && !others_down()) {
                    if (port_state_menu_double_used())
                        menu_single_wait = 1;                   /* single, unless a second press comes */
                    else
                        port_state_menu(PORT_MENU_SINGLE);
                }
                menu_down = 0;
                menu_skip_release = 0;
            } else if (code != KEY_ESC && ev[i].value == 1 && menu_down) {
                menu_mixed = 1;                                 /* another key with MENU */
                if (code == KEY_BACKSPACE)                      /* MENU+R2 */
                    port_state_request(PORT_REQ_SAVE);
                else if (code == KEY_TAB)                       /* MENU+L2 */
                    port_state_request(PORT_REQ_LOAD);
            }
            int k = key_index(code);
            if (k < 0)
                continue;
            held[k] = (ev[i].value != 0); /* 1 = press, 2 = repeat, 0 = release */
        }
        if (n < 32)
            break;
    }

    double now = port_now_ms();
    if (menu_down && !menu_mixed && !menu_long_done && !menu_skip_release && now - menu_press_t >= MENU_LONG_MS) {
        menu_long_done = 1;                                     /* held: long press */
        if (!others_down())
            port_state_menu(PORT_MENU_LONG);
    }
    if (menu_single_wait && now - menu_press_t > MENU_DOUBLE_MS) {
        menu_single_wait = 0;
        port_state_menu(PORT_MENU_SINGLE);
    }

    int combo = held[K_SELECT] && held[K_START];
    if (combo) {
        if (quit_since == 0)
            quit_since = now;
        else if (now - quit_since >= 1000.0)
            quit_flag = 1;
    } else {
        quit_since = 0;
    }
}

int port_input_quit_requested(void) {
    return quit_flag;
}

static void set_controller(struct Controller *c, u16 buttons, s16 sx, s16 sy, u16 stick) {
    c->rawStickX = sx;
    c->rawStickY = sy;
    c->buttonPressed = buttons & (buttons ^ c->button);
    c->buttonDepressed = c->button & (buttons ^ c->button);
    c->button = buttons;
    c->stickPressed = stick & (stick ^ c->stickDirection);
    c->stickDepressed = c->stickDirection & (stick ^ c->stickDirection);
    c->stickDirection = stick;
}

void update_controller(s32 index) {
    static int steer;   /* frames the current steering direction has been held */
    static int steer_dir;
    struct Controller *c = &gControllers[index];

    if (index != 0) {
        set_controller(c, 0, 0, 0, 0);
        return;
    }

    port_input_poll();

    u16 b = 0;
    if (held[K_A]) b |= N64_A;
    if (held[K_B]) b |= N64_B;
    /* L1 is the item (Z) in a race; in the menus it is L as well, as printed on the main
       menu ("L: OPTION"), besides SELECT */
    if (gGamestate != RACING) {
        if (held[K_L1]) b |= N64_L;
        if (held[K_L2]) b |= N64_Z;
    } else if (held[K_L1] || held[K_L2]) {
        b |= N64_Z;
    }
    if (held[K_R1] || held[K_R2]) b |= N64_R;
    /* MENU+L2 / MENU+R2 are the state keys: L2 and R2 do not reach the game while MENU is held */
    if (menu_down) {
        if (held[K_L2] && (gGamestate != RACING || !held[K_L1])) b &= ~N64_Z;
        if (held[K_R2] && !held[K_R1]) b &= ~N64_R;
    }
    if (held[K_Y]) b |= N64_CU;
    if (held[K_X]) b |= N64_CR;
    if (held[K_START]) b |= N64_START;
    if (held[K_SELECT]) b |= N64_L;
    if (held[K_UP]) b |= N64_DU;
    if (held[K_DOWN]) b |= N64_DD;
    if (held[K_LEFT]) b |= N64_DL;
    if (held[K_RIGHT]) b |= N64_DR;

    /* SELECT+X: frame capture for debugging; X does not reach the game while SELECT is held */
    {
        static int cap_was_down;
        int cap_down = held[K_SELECT] && held[K_X];
        if (cap_down && !cap_was_down) {
            extern void gfx_sw_request_capture(void);
            gfx_sw_request_capture();
        }
        cap_was_down = cap_down;
        if (held[K_SELECT])
            b &= ~N64_CR;
        if (cap_down)   /* hold X first, then SELECT: the game never sees L (OPTION in the menu) */
            b &= ~N64_L;
    }

    /* SELECT+START is the quit combo: don't let it pause the game first */
    if (held[K_SELECT] && held[K_START])
        b &= ~(N64_START | N64_L);

    /* digital steering: start at a gentle angle and ramp up to a full turn */
    int dir = held[K_LEFT] ? -1 : held[K_RIGHT] ? 1 : 0;
    if (held[K_LEFT] && held[K_RIGHT])
        dir = 0;
    if (dir != steer_dir) {
        steer_dir = dir;
        steer = 0;
    } else if (dir != 0 && steer < 16) {
        steer++;
    }
    int mag = 40 + steer * 4;
    if (mag > 72)
        mag = 72;
    s16 sx = (s16) (dir * mag);
    s16 sy = held[K_UP] ? 60 : held[K_DOWN] ? -60 : 0;

    u16 stick = 0;
    if (held[K_LEFT]) stick |= N64_DL;
    if (held[K_RIGHT]) stick |= N64_DR;
    if (held[K_UP]) stick |= N64_DU;
    if (held[K_DOWN]) stick |= N64_DD;

    set_controller(c, b, sx, sy, stick);
}

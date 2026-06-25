/*
 * mouse-remap.c — configurable low-level mouse remapper for Linux
 *
 * Each physical input (a button or a scroll direction) is mapped to an action
 * via a config file. It grabs the physical mouse (EVIOCGRAB) so the desktop
 * never sees raw events, then re-emits transformed events through a virtual
 * uinput mouse. No X11/Wayland dependency, no external libs.
 *
 * Build:  cc -O2 -o mouse-remap mouse-remap.c
 *
 * Usage:
 *   sudo ./mouse-remap [DEVICE] [--config FILE] [--hold-ms N]
 *   sudo ./mouse-remap --list        list candidate mice + scores
 *   sudo ./mouse-remap --watch        show which device fires (never grabs)
 *
 * Config file: one "source=action" per line (# comments allowed).
 *   sources:  left right middle side extra wheel_up wheel_down
 *   actions:  none
 *             click:left | click:right | click:middle
 *             double:left | double:right | double:middle
 *             hold:left | hold:right | hold:middle      (press-through; drag)
 *             tapdouble:left | tapdouble:right | ...     (tap=double, hold=hold)
 *             scroll:up | scroll:down
 *   plus:     hold_ms=150
 *
 * Stop a running remap with Ctrl-C, or unplug/replug the mouse.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_HOLD_MS 150

/* --- sources -------------------------------------------------------------- */
enum {
    SRC_LEFT, SRC_RIGHT, SRC_MIDDLE, SRC_SIDE, SRC_EXTRA,
    SRC_WHEEL_UP, SRC_WHEEL_DOWN, SRC_COUNT
};

/* --- actions -------------------------------------------------------------- */
enum mode { M_NONE, M_CLICK, M_HOLD, M_DOUBLE, M_TAPDOUBLE, M_SCROLL };
struct action { enum mode mode; int button; int scroll_dir; };

static struct action map[SRC_COUNT];
static int hold_ms = DEFAULT_HOLD_MS;

/* per-source tap-vs-hold state */
static int  pending[SRC_COUNT];
static int  held[SRC_COUNT];
static long press_ms[SRC_COUNT];

static int src_fd = -1;
static int uin_fd = -1;
static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

/* --- low-level emit ------------------------------------------------------- */
#define BITS_PER_LONG (8 * (int)sizeof(long))
#define NLONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
static int test_bit(int bit, const unsigned long *arr) {
    return (arr[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL;
}
static void emit(int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    if (write(uin_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        perror("write(uinput)");
}
static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }
static void msleep(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static void click(int button) {
    emit(EV_KEY, button, 1); syn();
    emit(EV_KEY, button, 0); syn();
}
static void double_click(int button) {
    click(button); msleep(18); click(button);
}

/* --- action application --------------------------------------------------- */
static void commit_hold(int src) {
    if (!pending[src]) return;
    emit(EV_KEY, map[src].button, 1); syn();
    pending[src] = 0;
    held[src] = 1;
}
static void commit_expired(void) {
    long n = now_ms();
    for (int s = 0; s < SRC_COUNT; s++)
        if (pending[s] && n - press_ms[s] >= hold_ms) commit_hold(s);
}

static void apply_button(int src, int value) {
    if (value != 0 && value != 1) return; /* ignore autorepeat */
    struct action a = map[src];
    switch (a.mode) {
        case M_NONE:
            break;
        case M_CLICK:
            if (value == 1) click(a.button);
            break;
        case M_DOUBLE:
            if (value == 1) double_click(a.button);
            break;
        case M_HOLD:
            emit(EV_KEY, a.button, value); syn();
            break;
        case M_SCROLL:
            if (value == 1) { emit(EV_REL, REL_WHEEL, a.scroll_dir); syn(); }
            break;
        case M_TAPDOUBLE:
            if (value == 1) {
                pending[src] = 1; held[src] = 0; press_ms[src] = now_ms();
            } else {
                if (pending[src]) { double_click(a.button); pending[src] = 0; }
                else if (held[src]) { emit(EV_KEY, a.button, 0); syn(); held[src] = 0; }
            }
            break;
    }
}

static void apply_wheel(int src) {
    struct action a = map[src];
    switch (a.mode) {
        case M_NONE:      break;
        case M_CLICK:
        case M_HOLD:      click(a.button); break; /* hold-per-tick = click */
        case M_DOUBLE:
        case M_TAPDOUBLE: double_click(a.button); break;
        case M_SCROLL:    emit(EV_REL, REL_WHEEL, a.scroll_dir); syn(); break;
    }
}

static int code_to_src(int code) {
    switch (code) {
        case BTN_LEFT:   return SRC_LEFT;
        case BTN_RIGHT:  return SRC_RIGHT;
        case BTN_MIDDLE: return SRC_MIDDLE;
        case BTN_SIDE:   return SRC_SIDE;
        case BTN_EXTRA:  return SRC_EXTRA;
        default:         return -1;
    }
}

static void handle(const struct input_event *ev) {
    switch (ev->type) {
        case EV_REL:
            if (ev->code == REL_X || ev->code == REL_Y) {
                emit(EV_REL, ev->code, ev->value); /* movement passes through */
            } else if (ev->code == REL_WHEEL && ev->value != 0) {
                apply_wheel(ev->value > 0 ? SRC_WHEEL_UP : SRC_WHEEL_DOWN);
            }
            break;
        case EV_KEY: {
            int src = code_to_src(ev->code);
            if (src < 0) { emit(EV_KEY, ev->code, ev->value); syn(); }
            else apply_button(src, ev->value);
            break;
        }
        case EV_SYN:
            syn();
            break;
        default:
            break;
    }
}

/* --- config --------------------------------------------------------------- */
static int parse_button(const char *s) {
    if (!strcmp(s, "left"))   return BTN_LEFT;
    if (!strcmp(s, "right"))  return BTN_RIGHT;
    if (!strcmp(s, "middle")) return BTN_MIDDLE;
    return BTN_LEFT;
}
static struct action parse_action(const char *s) {
    struct action a = { M_NONE, BTN_LEFT, 0 };
    if (!strcmp(s, "none")) return a;
    const char *colon = strchr(s, ':');
    if (!colon) return a;
    char mode[16]; size_t n = (size_t)(colon - s);
    if (n >= sizeof(mode)) n = sizeof(mode) - 1;
    memcpy(mode, s, n); mode[n] = '\0';
    const char *arg = colon + 1;

    if (!strcmp(mode, "click"))      { a.mode = M_CLICK;     a.button = parse_button(arg); }
    else if (!strcmp(mode, "double")){ a.mode = M_DOUBLE;    a.button = parse_button(arg); }
    else if (!strcmp(mode, "hold"))  { a.mode = M_HOLD;      a.button = parse_button(arg); }
    else if (!strcmp(mode, "tapdouble")) { a.mode = M_TAPDOUBLE; a.button = parse_button(arg); }
    else if (!strcmp(mode, "scroll")) {
        a.mode = M_SCROLL;
        a.scroll_dir = strcmp(arg, "down") == 0 ? -1 : 1;
    }
    return a;
}
static int src_index(const char *s) {
    if (!strcmp(s, "left"))       return SRC_LEFT;
    if (!strcmp(s, "right"))      return SRC_RIGHT;
    if (!strcmp(s, "middle"))     return SRC_MIDDLE;
    if (!strcmp(s, "side"))       return SRC_SIDE;
    if (!strcmp(s, "extra"))      return SRC_EXTRA;
    if (!strcmp(s, "wheel_up"))   return SRC_WHEEL_UP;
    if (!strcmp(s, "wheel_down")) return SRC_WHEEL_DOWN;
    return -1;
}
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}
static void set_defaults(void) {
    map[SRC_LEFT]       = parse_action("tapdouble:left");
    map[SRC_RIGHT]      = parse_action("hold:right");
    map[SRC_MIDDLE]     = parse_action("click:right");
    map[SRC_SIDE]       = parse_action("hold:left");
    map[SRC_EXTRA]      = parse_action("hold:left");
    map[SRC_WHEEL_UP]   = parse_action("click:right");
    map[SRC_WHEEL_DOWN] = parse_action("click:right");
}
static int load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror("open(config)"); return -1; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p), *val = trim(eq + 1);
        if (!strcmp(key, "hold_ms")) { hold_ms = atoi(val); if (hold_ms < 0) hold_ms = 0; continue; }
        int si = src_index(key);
        if (si >= 0) map[si] = parse_action(val);
    }
    fclose(f);
    return 0;
}

/* --- device inspection ---------------------------------------------------- */
struct caps { int is_pointer, wheel, has_side, n_buttons; char name[256]; };
static struct caps inspect(int fd) {
    struct caps c; memset(&c, 0, sizeof(c));
    unsigned long ev[NLONGS(EV_MAX)], rel[NLONGS(REL_MAX)], key[NLONGS(KEY_MAX)];
    memset(ev, 0, sizeof(ev)); memset(rel, 0, sizeof(rel)); memset(key, 0, sizeof(key));
    if (ioctl(fd, EVIOCGNAME(sizeof(c.name)), c.name) < 0) c.name[0] = '\0';
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0) return c;
    if (!test_bit(EV_REL, ev) || !test_bit(EV_KEY, ev)) return c;
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) < 0) return c;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key) < 0) return c;
    c.is_pointer = test_bit(REL_X, rel) && test_bit(REL_Y, rel) && test_bit(BTN_LEFT, key);
    c.wheel = test_bit(REL_WHEEL, rel);
    c.has_side = test_bit(BTN_SIDE, key) || test_bit(BTN_EXTRA, key);
    for (int b = BTN_LEFT; b <= BTN_TASK; b++) if (test_bit(b, key)) c.n_buttons++;
    return c;
}
static int score_of(const struct caps *c) {
    if (!c->is_pointer) return -1;
    if (strstr(c->name, "mouse-remap")) return -1;
    return c->n_buttons * 2 + (c->wheel ? 3 : 0) + (c->has_side ? 5 : 0);
}
static void list_devices(void) {
    printf("Scanning /dev/input/event* for mice...\n\n");
    int best = -1, best_score = -1;
    for (int i = 0; i < 64; i++) {
        char path[64]; snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY); if (fd < 0) continue;
        struct caps c = inspect(fd); int s = score_of(&c);
        if (c.is_pointer) {
            printf("  %-20s score=%-3d buttons=%d wheel=%d side=%d  \"%s\"\n",
                   path, s, c.n_buttons, c.wheel, c.has_side, c.name);
            if (s > best_score) { best_score = s; best = i; }
        }
        close(fd);
    }
    if (best >= 0)
        printf("\n=> Best guess: /dev/input/event%d\n", best);
    else printf("\nNo mouse-like device found.\n");
}
static void watch_devices(void) {
    struct pollfd pfds[64]; char paths[64][320]; int n = 0;
    for (int i = 0; i < 64 && n < 64; i++) {
        char path[64]; snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK); if (fd < 0) continue;
        struct caps c = inspect(fd); if (!c.is_pointer) { close(fd); continue; }
        pfds[n].fd = fd; pfds[n].events = POLLIN;
        snprintf(paths[n], sizeof(paths[n]), "%.63s (%.240s)", path, c.name); n++;
    }
    if (n == 0) { printf("No mouse-like devices found.\n"); return; }
    printf("Watching %d device(s). Move/click your mouse. Ctrl-C to stop.\n\n", n);
    char last[320] = "";
    while (running) {
        if (poll(pfds, n, 500) <= 0) continue;
        for (int i = 0; i < n; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            struct input_event ev;
            while (read(pfds[i].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if ((ev.type == EV_REL || ev.type == EV_KEY) && strcmp(last, paths[i])) {
                    printf(">> activity on %s\n", paths[i]);
                    snprintf(last, sizeof(last), "%.319s", paths[i]);
                }
            }
        }
    }
    for (int i = 0; i < n; i++) close(pfds[i].fd);
}
static int find_best_mouse(char *name_out, size_t name_sz) {
    int best_fd = -1, best_score = -1; char best_path[64] = "";
    for (int i = 0; i < 64; i++) {
        char path[64]; snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY); if (fd < 0) continue;
        struct caps c = inspect(fd); int s = score_of(&c);
        if (s > best_score) {
            best_score = s;
            if (best_fd >= 0) close(best_fd);
            best_fd = fd;
            snprintf(best_path, sizeof(best_path), "%s", path);
            if (name_out && name_sz) snprintf(name_out, name_sz, "%s", c.name);
        } else close(fd);
    }
    if (best_fd >= 0)
        fprintf(stderr, "Auto-selected %s (%s)\n", best_path, name_out ? name_out : "?");
    return best_fd;
}

/* --- virtual device ------------------------------------------------------- */
static int create_virtual_mouse(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open(/dev/uinput)"); return -1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY); ioctl(fd, UI_SET_EVBIT, EV_REL); ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT); ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT); ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_RELBIT, REL_X); ioctl(fd, UI_SET_RELBIT, REL_Y); ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    struct uinput_setup s; memset(&s, 0, sizeof(s));
    s.id.bustype = BUS_USB; s.id.vendor = 0x1234; s.id.product = 0x5678;
    snprintf(s.name, sizeof(s.name), "mouse-remap virtual mouse");
    if (ioctl(fd, UI_DEV_SETUP, &s) < 0) { perror("UI_DEV_SETUP"); close(fd); return -1; }
    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); close(fd); return -1; }
    return fd;
}

/* -------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    set_defaults();

    const char *dev_path = NULL;
    int hold_override = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list"))  { list_devices();  return 0; }
        if (!strcmp(argv[i], "--watch")) { watch_devices(); return 0; }
        if (!strcmp(argv[i], "--config") && i + 1 < argc) { load_config(argv[++i]); continue; }
        if (!strcmp(argv[i], "--hold-ms") && i + 1 < argc) { hold_override = atoi(argv[++i]); continue; }
        dev_path = argv[i];
    }
    if (hold_override >= 0) hold_ms = hold_override; /* CLI wins over config */

    char name[256] = {0};
    if (dev_path) {
        src_fd = open(dev_path, O_RDONLY);
        if (src_fd < 0) { perror("open(device)"); return 1; }
        fprintf(stderr, "Using %s\n", dev_path);
    } else {
        src_fd = find_best_mouse(name, sizeof(name));
        if (src_fd < 0) { fprintf(stderr, "No mouse found. Try: sudo %s --list\n", argv[0]); return 1; }
    }

    uin_fd = create_virtual_mouse();
    if (uin_fd < 0) { close(src_fd); return 1; }
    msleep(200);

    if (ioctl(src_fd, EVIOCGRAB, (void *)1) < 0)
        perror("EVIOCGRAB (another grabber active?)");
    fprintf(stderr, "Remapping active (hold %d ms). Ctrl-C to stop.\n", hold_ms);

    /* nonblocking so poll governs timing for pending holds */
    int flags = fcntl(src_fd, F_GETFL, 0);
    fcntl(src_fd, F_SETFL, flags | O_NONBLOCK);

    struct input_event ev;
    while (running) {
        int timeout = -1;
        long n = now_ms();
        for (int s = 0; s < SRC_COUNT; s++) {
            if (!pending[s]) continue;
            long left = hold_ms - (n - press_ms[s]);
            if (left < 0) left = 0;
            if (timeout < 0 || left < timeout) timeout = (int)left;
        }
        struct pollfd p = {src_fd, POLLIN, 0};
        int r = poll(&p, 1, timeout);
        if (r == 0) { commit_expired(); continue; }
        if (r < 0) { if (errno == EINTR) continue; perror("poll"); break; }
        ssize_t got = read(src_fd, &ev, sizeof(ev));
        if (got == (ssize_t)sizeof(ev)) handle(&ev);
        else if (got < 0 && errno != EAGAIN && errno != EINTR) { perror("read"); break; }
        commit_expired();
    }

    fprintf(stderr, "\nReleasing mouse...\n");
    ioctl(src_fd, EVIOCGRAB, (void *)0);
    ioctl(uin_fd, UI_DEV_DESTROY);
    close(uin_fd);
    close(src_fd);
    return 0;
}

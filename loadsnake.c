/*
 * XScreensaver loadsnake hack
 * 
 * Displays system load like the old classic Novell Netware's snake
 * (aka Netware worm) console screensaver.
 *
 * Shamelessly hacked from popsquares.c by Levi Burton.
 *
 * TODO
 * - Snakes collision detection
 * - Better snake movement. Now sometimes they hang on themselves... :-)
 * - Too long snakes cause too many color shades
 *
 * Copyright (c) 2007-2011 Cosimo Streppone <cosimo@cpan.org>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.  No representations are made about the suitability of this
 * software for any purpose.  It is provided "as is" without express or 
 * implied warranty.
 */

#include <assert.h>
#include "screenhack.h"
#include "colors.h"

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
# include "xdbe.h"
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

#define     SNAKE_MIN_LEN   5
#define     SNAKE_MAX_LEN   64
#define     SNAKE_TAIL_LEN  3
#define     MAX_SNAKES      10
#define     COLORSETS       8

typedef struct _snake {
    int cpu;
    int x[SNAKE_MAX_LEN];
    int y[SNAKE_MAX_LEN];
    int length;
    int direction;
    int color;
    int stay_straight;
} snake;

struct state {
    Display *dpy;
    Window window;

    /* Starting parameters */
    int delay, subdivision, border, ncolors, dbuf;
    int cpus, sw, sh, gw, gh;

    /* Cpu status */
    int usr [MAX_SNAKES];
    int sys [MAX_SNAKES];
    int nice[MAX_SNAKES];
    int idle[MAX_SNAKES];

    XWindowAttributes xgwa;
    GC gc; 
    XColor **colors;
    snake *snakes;
    Pixmap b, ba, bb;
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
    XdbeBackBuffer backb;
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
};

/* Get number of system processors */
static int
loadsnake_processors (void)
{
    int cpus = 0;

#ifdef __linux__
    cpus = sysconf(_SC_NPROCESSORS_ONLN);
#endif

#if defined(__FreeBSD__) || defined(HW_NCPU)
    int len = sizeof cpus;
    sysctlbyname("hw.ncpu", &cpus, (void*)(&len), NULL, 0);
#endif

    if (cpus < 1)
        cpus = 1;

    return (cpus);
}


/* Get load of single CPU */
static int
loadsnake_getcpuload (void *closure, int cpu)
{
    static char line[100];
    struct state *st = (struct state *) closure;

    int p_usr  = 0,
        p_nice = 0,
        p_sys  = 0,
        p_idle = 0,
        load = 0,
        snake_len = 0;

    FILE *f;
    char src[5] = "cpu\0\0";

    /* There could be some option to force no. of snakes? */
    if (cpu > st->cpus)
        return 0;

    if (cpu > 9)
        return 0;

    src[3] = '0' + cpu;
    src[4] = 0;

    if (NULL != (f = fopen("/proc/stat", "r"))) {
        while (! feof(f) && 0 == load) {
            fgets (line, 98, f);
            if (! strncasecmp(src, line, 4)) {
                p_usr  = st->usr[cpu];
                p_nice = st->nice[cpu];
                p_sys  = st->sys [cpu];
                p_idle = st->idle[cpu];

                if (4 == sscanf( &line[5], "%d %d %d %d", &(st->usr[cpu]), &(st->nice[cpu]), &(st->sys[cpu]), &(st->idle[cpu]))) {
                    /* printf("Matched: %d\n", matched); */
                    /* printf("CPU Line [%s", line); */
                    load = st->usr[cpu]  - p_usr
                         + st->nice[cpu] - p_nice
                         + st->sys[cpu]  - p_sys
                         + st->idle[cpu] - p_idle;
                    /* printf("Load is %d. Idle is %d\n", load, st->idle[cpu]-p_idle); */
                    if (0 == load)
                        load = 1;
                    load = (load - (st->idle[cpu] - p_idle)) * 100 / load;
                    /* printf("Load on cpu %d is %d%%\n", cpu, load); */
                    break;
                }
                else
                    return 2;
            }
        }
    	fclose(f);
    }

    /* Grow snake exponentially with load:
       max length is reached when load = 100 */
    snake_len = load * load * SNAKE_MAX_LEN / 10000.0;

    if (snake_len < SNAKE_MIN_LEN)
        snake_len = SNAKE_MIN_LEN;

    return (snake_len);
}


/* Get whole system load measure */
static int
loadsnake_getsystemload (void)
{
    char load[10];
    float l1 = 0;
    int l2 = 0;

    FILE *f = fopen("/proc/loadavg", "r");
    if (f != NULL) {
        fgets(load, 10, f);
        fclose(f);
        sscanf(load, "%f", &l1);
        /* printf("Load from /proc/loadavg is %f\n", l1); */
    }
    else {
        l1 = 0;
    }
    l1 *= 1000.0;

    l2 = (int) l1;
    l2 /= 1000;

    return l2;
}


static XColor *
make_colorset (struct state *st, const int *rgb) {

    int h1, h2 = 0;
    double s1, v1, s2, v2 = 0;
    XColor *colset = (XColor *) calloc (st->ncolors, sizeof(XColor));

    /* Start: always black background rgb(0,0,0), tail */
    rgb_to_hsv(0, 0, 0, &h1, &s1, &v1);

    /* Finish: foreground, head */
    rgb_to_hsv(rgb[0], rgb[1], rgb[2], &h2, &s2, &v2);

    make_color_ramp(
        st->dpy, st->xgwa.colormap,
        h1, s1, v1,
        h2, s2, v2,
        colset, &st->ncolors,
        True, True, False
    );

    return colset;
}


/* Pre-define the colorsets for the different CPU snakes */
static XColor **
init_colorsets (struct state *st)
{
    int n;
    int rgb[COLORSETS][3] = {
        { 0xFFFF, 0,      0      }, /* 1st snake, red */
        { 0,      0,      0xFFFF }, /* 2nd, blue */
        { 0,      0xFFFF, 0      }, /* green */
        { 0xFFFF, 0xFFFF, 0      }, /* yellow */
        { 0xFFFF, 0,      0xFFFF }, /* purple */
        { 0xFFFF, 0xFFFF, 0      }, /* ... */
        { 0,      0xFFFF, 0xFFFF },
        { 0x7FFF, 0x7FFF, 0xFFFF },
    };

    XColor** colset = (XColor **) calloc (COLORSETS, sizeof(XColor *));

    for (n = 0; n < COLORSETS; n++)
        colset[n] = make_colorset(st, rgb[n]);

    return colset;
}

/* Init XScreensaver module */
static void *
loadsnake_init (Display *dpy, Window window)
{
    struct state *st = (struct state *) calloc (1, sizeof(*st));
    int n;

    XColor fg, bg;
    XGCValues gcv;
   
    st->dpy = dpy;
    st->window = window;

    st->delay = get_integer_resource (st->dpy, "delay", "Integer");
    st->subdivision = get_integer_resource(st->dpy, "subdivision", "Integer");
    st->border = get_integer_resource(st->dpy, "border", "Integer");
    st->ncolors = get_integer_resource(st->dpy, "ncolors", "Integer");
    st->dbuf = get_boolean_resource(st->dpy, "doubleBuffer", "Boolean");

# ifdef HAVE_COCOA	/* Don't second-guess Quartz's double-buffering */
    st->dbuf = False;
# endif

    XGetWindowAttributes (st->dpy, st->window, &st->xgwa);

    fg.pixel = get_pixel_resource (
        st->dpy, st->xgwa.colormap, "foreground", "Foreground"
    );
    bg.pixel = get_pixel_resource (
        st->dpy, st->xgwa.colormap, "background", "Background"
    );

    XQueryColor (st->dpy, st->xgwa.colormap, &fg);
    XQueryColor (st->dpy, st->xgwa.colormap, &bg);

    st->sw = st->xgwa.width / st->subdivision;
    st->sh = st->xgwa.height / st->subdivision;
    st->gw = st->sw ? st->xgwa.width / st->sw : 0;
    st->gh = st->sh ? st->xgwa.height / st->sh : 0;
    st->cpus = loadsnake_processors();

    gcv.foreground = fg.pixel;
    gcv.background = bg.pixel;
    st->gc = XCreateGC (st->dpy, st->window, GCForeground|GCBackground, &gcv);

    if (st->ncolors < 2) {
        fprintf (stderr, "%s: insufficient colors!\n", progname);
        exit (1);
    }
    st->colors = init_colorsets(st);

    /* Start various snakes at random points and directions */
    st->snakes = (snake *) calloc (st->cpus, sizeof(snake));

    for (n = 0; n < st->cpus; n++) {
        snake *s = (snake *) &st->snakes[n];
        s->cpu  = n;
        s->x[0] = random() % st->subdivision;
        s->y[0] = random() % st->subdivision;
        s->direction = ((random() % 9) >> 1) << 1;
        s->length = SNAKE_MIN_LEN;
        s->color = n % COLORSETS;
        s->stay_straight = 3;
        /* fprintf (stderr, "Snake %d starting at %d,%d dir %d length %d\n", s->cpu, s->x, s->y, s->direction, s->length); */
    }

    if (st->dbuf) {
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
        st->b = xdbe_get_backbuffer (st->dpy, st->window, XdbeUndefined);
        st->backb = st->b;
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
        if (!st->b) {
            st->ba = XCreatePixmap (st->dpy, st->window, st->xgwa.width, st->xgwa.height, st->xgwa.depth);
            st->bb = XCreatePixmap (st->dpy, st->window, st->xgwa.width, st->xgwa.height, st->xgwa.depth);
            st->b = st->ba;
        }
    }
    else {
        st->b = st->window;
    }

    return st;
}

/* Move the snake in random direction */
static void
loadsnake_move (void *closure, snake *s)
{
    int n = 0, dir = 0;
    int x = 0, y = 0;

    struct state *st = (struct state *) closure;

    /* Snake head position */
    x = s->x[0];
    y = s->y[0];

    /* and direction */
    dir = s->direction;

    /* 0=up, 2=right, 4=down, 6=left */
    switch(dir)
    {
        case 0: y++;      break;
        case 1: y++; x++; break;
        case 2:      x++; break;
        case 3: y--; x++; break;
        case 4: y--;      break;
        case 5: y--; x--; break;
        case 6:      x--; break;
        case 7: y++; x--; break;
    }

    /* Check bounds and change direction */
    if (x < 0 && (dir >= 5 && dir <= 7)) {
        x = 1;
        dir -= 4;
    }
    else if (y < 0 && (dir >= 3 && dir <= 5)) {
        y = 1;
        dir -= 4;
    }
    else if (x >= st->gw && (dir >= 1 && dir <= 3)) {
        x = st->gw - 1;
        dir += 4;
    }
    else if (y >= st->gh && (dir == 7 || dir == 0 || dir == 1)) {
        y = st->gh - 1;
        dir += 4;
    }
    else if (s->stay_straight == 0) {
        /* Slightly change snake heading */
        int rnd = random() % 128;
        if(rnd > 110)
            dir += 2;
        else if(rnd > 100)
            dir -= 2;
        else if(rnd == 1)
            dir++;
        else if(rnd == 2)
            dir--;
        s->stay_straight = 2;
    }
    else {
        s->stay_straight--;
    }

    if (dir < 0)
        dir = -dir;
    dir = dir % 8;

    s->direction = dir;

    /* Copy x,y coords in "tail" positions */
    for(n = s->length - 1; n > 0; n--) {
        s->x[n] = s->x[n-1];
        s->y[n] = s->y[n-1];
    }

    /* New head position */
    s->x[0] = x;
    s->y[0] = y;

}


static void
loadsnake_clear (struct state *st)
{
    int x = 0, y = 0;

    XSetForeground (st->dpy, st->gc, st->colors[0][0].pixel);

    for (y = 0; y < st->gh; y++)
        for (x = 0; x < st->gw; x++)
            XFillRectangle (st->dpy, st->b, st->gc, st->sw * x, st->sh * y,
                            st->border ? st->sw - st->border : st->sw, 
                            st->border ? st->sh - st->border : st->sh);
}

static void
loadsnake_drawsnake (struct state *st, snake *s)
{
    int i, n, body_col, tail_pos = 0;
    XColor *snake_colset;

    /* Select the colorset for this snake */
    snake_colset = st->colors[s->color];

    /* printf("Drawing snake from 0 to %d\n", s->length); */

    /* Tail is always SNAKE_TAIL_LEN blocks long */
    tail_pos = s->length - SNAKE_TAIL_LEN;

    assert(SNAKE_TAIL_LEN < SNAKE_MIN_LEN);
    assert(s->length >= SNAKE_MIN_LEN);
    assert(tail_pos > 0);

    /* Draw the snake tail with color gradient */
    i = SNAKE_TAIL_LEN;
    for (n = s->length - 1; n >= tail_pos; n--, i--) {
        int grad_col = (st->ncolors >> 1) / (i + 1);
        XSetForeground (st->dpy, st->gc, snake_colset[grad_col].pixel);
        XFillRectangle (
            st->dpy, st->b, st->gc, st->sw * s->x[n], st->sh * s->y[n],
            st->border ? st->sw - st->border : st->sw, 
            st->border ? st->sh - st->border : st->sh
        );
    }

    /* Use last color of the ramp (main color) to draw head and body */
    body_col = st->ncolors >> 1;

    /* Draw the snake head and body with primary color */
    XSetForeground (st->dpy, st->gc, snake_colset[body_col].pixel);

    for (n = tail_pos - 1; n >= 0; n--)
        XFillRectangle (
            st->dpy, st->b, st->gc, st->sw * s->x[n], st->sh * s->y[n],
            st->border ? st->sw - st->border : st->sw, 
            st->border ? st->sh - st->border : st->sh
        );

}

/*
 Grow snake as its processor load increases
 Growth rate is mitigated. Snake can only grow by 1 'position' at a time
 */
static int
loadsnake_grow (void *closure, snake *s)
{
    struct state *st = (struct state *) closure;
    int newlen = loadsnake_getcpuload(st, s->cpu);
    int len = s->length;

    if (newlen > len) {
        int x, y;

        x = s->x[len - 1];
        y = s->y[len - 1];

        /* printf("Growing! Last pos is (%d, %d)\n", x, y); */

        switch(s->direction) {
            case 0: y--;      break;
            case 1: y--; x--; break;
            case 2:      x--; break;
            case 3: y++; x--; break;
            case 4: y++;      break;
            case 5: y++; x++; break;
            case 6:      x++; break;
            case 7: y--; x++; break;
        }

        len++;

        if (len >= SNAKE_MAX_LEN)
            len = SNAKE_MAX_LEN - 1;

        /* printf("Appending in pos (%d, %d)\n", x, y); */

        s->x[len] = x;
        s->y[len] = y;
    }
    else if (newlen < len) {
        len--;
        if (len < SNAKE_MIN_LEN)
            len = SNAKE_MIN_LEN;
        s->x[len + 1] = 0;
        s->y[len + 1] = 0;
    }

    s->length = len;

    return(len);
}

static unsigned long
loadsnake_draw (Display *dpy, Window window, void *closure)
{
    struct state *st = (struct state *) closure;
    int n;

    /* We shouldn't clear video area */
    loadsnake_clear(st);

    for (n = 0; n < st->cpus; n++) {
        snake *s = (snake *) &st->snakes[n];
        loadsnake_grow(st, s);
        /* printf("Snake %d grown at length %d\n", n, loadsnake_grow(st, s)); */
        loadsnake_move(st, s);
        loadsnake_drawsnake(st, s);
    }

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
    if (st->backb) {
        XdbeSwapInfo info[1];
        info[0].swap_window = st->window;
        info[0].swap_action = XdbeUndefined;
        XdbeSwapBuffers (st->dpy, info, 1);
    }
    else
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
        if (st->dbuf) {
            XCopyArea (st->dpy, st->b, st->window, st->gc, 0, 0, 
                       st->xgwa.width, st->xgwa.height, 0, 0);
            st->b = (st->b == st->ba ? st->bb : st->ba);
        }

    /* Make snakes walk faster if load increases (as Netware's version) */
    n = loadsnake_getsystemload();

    /* The more the system is loaded, the faster the snakes */
    st->delay = (int) 1.0 / (n + 1.0) * 500000.0;
    if (st->delay < 10000)
        st->delay = 10000;

    return st->delay;
}

static void
loadsnake_reshape (Display *dpy, Window window, void *closure, 
                 unsigned int w, unsigned int h)
{
}

static Bool
loadsnake_event (Display *dpy, Window window, void *closure, XEvent *event)
{
        return False;
}

static void
loadsnake_free (Display *dpy, Window window, void *closure)
{
        struct state *st = (struct state *) closure;
        XColor **colset = st->colors;
        int n = COLORSETS;

        while (n--)
                free(colset[n]);

        free(st->colors);
        free(st->snakes);
        free(st);
}

static const char *loadsnake_defaults [] = {
        ".background: #000000",
        ".foreground: #FF0000",
        "*delay: 200000",
        "*subdivision: 40",
        "*border: 0",
        "*ncolors: 32",
        "*doubleBuffer: False",
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
        "*useDBE: True",
        "*useDBEClear: True",
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
        0
};

static XrmOptionDescRec loadsnake_options [] = {
        { "-fg", ".foreground", XrmoptionSepArg, 0},
        { "-bg", ".background", XrmoptionSepArg, 0},
        { "-delay",     ".delay", XrmoptionSepArg, 0 },
        { "-subdivision", ".subdivision", XrmoptionSepArg, 0 },
        { "-border", ".border", XrmoptionSepArg, 0},
        { "-ncolors",   ".ncolors", XrmoptionSepArg, 0 },
        { "-db",        ".doubleBuffer", XrmoptionNoArg, "True" },
        { "-no-db",     ".doubleBuffer", XrmoptionNoArg, "False" },
        { 0, 0, 0, 0 }
};


XSCREENSAVER_MODULE ("LoadSnake", loadsnake)

/* vim: ts=8 sw=8 tw=0 et
 */

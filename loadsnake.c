/*
 * $Header: /my/cvsroot/xscreensaver/loadsnake/loadsnake.c,v 1.4 2007/02/28 21:46:31 cosimo Exp $
 *
 * XScreensaver loadsnake hack
 * 
 * Displays system load like the old classic Novell Netware's console screensaver.
 * Shamelessly hacked from popsquares.c by Levi Burton.
 *
 * TODO
 * - Improve the code for different colors sets by CPU
 * - Snakes collision detection
 * - Better snake movement. Now sometimes they hang on themselves... :-)
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

#include "screenhack.h"
#include "colors.h"

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
# include "xdbe.h"
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

#define     SNAKE_MAX_LEN   30
#define     MAX_SNAKES      10

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
    XColor *colors;
    XColor *gcolors;
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
    int len = sizeof(cpus);
    sysctlbyname("hw.ncpu", &cpus, (void*)(&len), NULL, 0);
#endif

    if (cpus < 1) cpus = 1;
    return (cpus);
}


/* Get load of single CPU */
static int
loadsnake_getcpuload (void *closure, int cpu)
{
    static char line[100];
    struct state *st = (struct state *) closure;

    int p_usr  = 0;
    int p_nice = 0;
    int p_sys  = 0;
    int p_idle = 0;

    int load = 0;

    FILE *f;
    char src[5] = "cpu\0\0";

    if( cpu > 9 ) return 0;
    src[3] = '0' + cpu;
    src[4] = 0;

    if (NULL != (f = fopen ("/proc/stat", "r"))) {
        while (! feof(f) && load == 0) {
            fgets (line, 98, f);
            if (strncasecmp(src, line, 4) == 0) {
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
                    if( load == 0 ) load = 1;
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

    /* XXX Rivedere... */
         if(load > 1000) load /= 50;
    else if(load > 200)  load /= 10;
    else if(load > 100)  load /= 5;
    else if(load > 20)   load /= 3;
    else load /= 2;

    /* Minimum snake length is 3 */
    if( load < 3 ) load = 3;

    return(load);
}


/* Get whole system load measure */
static int
loadsnake_getsystemload (void)
{
    char load[10];
    float l1 = 0;
    int l2   = 0;

    FILE *f = fopen("/proc/loadavg", "r");
    if(f != NULL)
    {
        fgets(load, 10, f);
        fclose(f);
        sscanf(load, "%f", &l1);
        /* printf("Load from /proc/loadavg is %f\n", l1); */
    }
    else
    {
        l1 = 0;
    }
    l1 *= 1000;
    l2 = (int) l1;
    l2 /= 1000;
    return(l2);
}

/* Init XScreensaver module */
static void *
loadsnake_init (Display *dpy, Window window)
{
  struct state *st = (struct state *) calloc (1, sizeof(*st));
  int n;
  int h1, h2 = 0;
  double s1, v1, s2, v2 = 0;

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

  fg.pixel = get_pixel_resource (st->dpy, st->xgwa.colormap, "foreground", "Foreground");
  bg.pixel = get_pixel_resource (st->dpy, st->xgwa.colormap, "background", "Background");

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

  st->colors = (XColor *) calloc (st->ncolors, sizeof(XColor));
  st->gcolors = (XColor *) calloc (st->ncolors, sizeof(XColor));
  st->snakes = (snake *) calloc (st->cpus, sizeof(snake));

  rgb_to_hsv (fg.red, fg.green, fg.blue, &h1, &s1, &v1);
  rgb_to_hsv (bg.red, bg.green, bg.blue, &h2, &s2, &v2);
  make_color_ramp (st->dpy, st->xgwa.colormap,
                   h2, s2, v2,
                   h1, s1, v1,
                   st->colors, &st->ncolors,
                   True, True, False);

  rgb_to_hsv (0, 0, 40000, &h1, &s1, &v1);
  rgb_to_hsv (0, 0, 0, &h2, &s2, &v2);

  make_color_ramp (st->dpy, st->xgwa.colormap,
                   h2, s2, v2,
                   h1, s1, v1,
                   st->gcolors, &st->ncolors,
                   True, True, False);

  if (st->ncolors < 2) {
      fprintf (stderr, "%s: insufficient colors!\n", progname);
      exit (1);
  }

  /* Start various snakes at random points and directions */
  for (n = 0; n < st->cpus; n++)
  {
      snake *s= (snake *) &st->snakes[n];
      s->cpu  = n;
      s->x[0] = random() % st->subdivision;
      s->y[0] = random() % st->subdivision;
      s->direction = ((random() % 9) >> 1) << 1;
      s->length = 1;
      s->color = n % 2;   /* 2 color sets, red and green */
      s->stay_straight = 3;
      /* fprintf (stderr, "Snake %d starting at %d,%d dir %d length %d\n", s->cpu, s->x, s->y, s->direction, s->length); */
  }

  if (st->dbuf)
    {
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
      st->b = xdbe_get_backbuffer (st->dpy, st->window, XdbeUndefined);
      st->backb = st->b;
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
      if (!st->b)                      
        {
          st->ba = XCreatePixmap (st->dpy, st->window, st->xgwa.width, st->xgwa.height, st->xgwa.depth);
          st->bb = XCreatePixmap (st->dpy, st->window, st->xgwa.width, st->xgwa.height, st->xgwa.depth);
          st->b = st->ba;
        }
    }
  else 
    {
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
    if( x < 0 && (dir >= 5 && dir <= 7) )
    {
        x = 1;
        dir -= 4;
    }
    else if( y < 0 && (dir >= 3 && dir <= 5) )
    {
        y = 1;
        dir -= 4;
    }
    else if( x >= st->gw && (dir >= 1 && dir <= 3) )
    {
        x = st->gw - 1;
        dir += 4;
    }
    else if( y >= st->gh && (dir == 7 || dir == 0 || dir == 1) )
    {
        y = st->gh - 1;
        dir += 4;
    }
    else if( s->stay_straight == 0 )
    {
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
    else
    {
        s->stay_straight--;
    }

    if(dir < 0) dir = -dir;
    dir = dir % 8;

    s->direction = dir;

    /* Copy x,y coords in "tail" positions */
    for(n = s->length - 1; n > 0; n--)
    {
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
    for( y = 0; y < st->gh; y++)
        for( x = 0; x < st->gw; x++)
        {
            XSetForeground (st->dpy, st->gc, st->colors[0].pixel);
            XFillRectangle (st->dpy, st->b, st->gc, st->sw * x, st->sh * y,
                            st->border ? st->sw - st->border : st->sw, 
                            st->border ? st->sh - st->border : st->sh);
        }
}

static void
loadsnake_drawsnake (struct state *st, snake *s)
{
    XColor *snake_colors;
    int c, n;

    c = st->ncolors >> 4;

    if (s->color == 0) 
        snake_colors = st->colors;
    else
        snake_colors = st->gcolors;

    /* printf("Drawing snake from 0 to %d\n", s->length); */
    for(n = s->length - 1; n >= 0; n--)
    {
        /* printf("    draw pos (%d, %d)\n", s->x[n], s->y[n]); */
        /* XSetForeground (st->dpy, st->gc, st->colors[c].pixel); */
        XSetForeground (st->dpy, st->gc, snake_colors[c].pixel);
        XFillRectangle (st->dpy, st->b, st->gc, st->sw * s->x[n], st->sh * s->y[n],
                        st->border ? st->sw - st->border : st->sw, 
                        st->border ? st->sh - st->border : st->sh);
        c += st->ncolors / s->length / 2;
    }
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

    if(newlen > len)
    {
        int x, y;

        x = s->x[len - 1];
        y = s->y[len - 1];

        /* printf("Growing! Last pos is (%d, %d)\n", x, y); */

        switch(s->direction)
        {
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

        if(len >= SNAKE_MAX_LEN)
            len = SNAKE_MAX_LEN - 1;

        /* printf("Appending in pos (%d, %d)\n", x, y); */

        s->x[len] = x;
        s->y[len] = y;
    }
    else if(newlen < len)
    {
        len--;
        if(len < 2)
            len = 2;
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

    for (n = 0; n < st->cpus; n++)
    {
        snake *s = (snake *) &st->snakes[n];
        loadsnake_grow(st, s);
        /* printf("Snake %d grown at length %d\n", n, loadsnake_grow(st, s)); */
        loadsnake_move(st, s);
        loadsnake_drawsnake(st, s);
    }

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
    if (st->backb) 
    {
        XdbeSwapInfo info[1];
        info[0].swap_window = st->window;
        info[0].swap_action = XdbeUndefined;
        XdbeSwapBuffers (st->dpy, info, 1);
    }
    else
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
        if (st->dbuf)
        {
            XCopyArea (st->dpy, st->b, st->window, st->gc, 0, 0, 
                       st->xgwa.width, st->xgwa.height, 0, 0);
            st->b = (st->b == st->ba ? st->bb : st->ba);
        }

    /* Make snakes walk faster if load increases (as Netware's version) */
    n = loadsnake_getsystemload();
    if(n > 10)
        st->delay = 10000;
    else if(n > 5)
        st->delay = 50000;
    else if(n > 2)
        st->delay = 100000;
    else if(n > 1)
        st->delay = 300000;
    else
        st->delay = 600000;

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
        free (st->colors);
        free (st->gcolors);
        free (st->snakes);
        free (st);
}

static const char *loadsnake_defaults [] = {
  ".background: #000000",
  ".foreground: #FF0000",
  "*delay: 200000",
  "*subdivision: 40",
  "*border: 0",
  "*ncolors: 128",
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

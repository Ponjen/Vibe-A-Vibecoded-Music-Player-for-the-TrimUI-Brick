#define _GNU_SOURCE
#include <SDL.h>
#include <SDL_ttf.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <time.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/stat.h>
#include "library.h"
#include "engine.h"
#include "browse.h"
#include "dl.h"

#define CTL_PATH "/tmp/vibe.ctl"   /* echo toggle|play|pause|next|prev|show|quit > this */
#define PID_PATH "/tmp/vibe.pid"
#define UI_PATH  "/tmp/vibe.ui"    /* exists while the UI owns the screen (launch.sh waits on it) */
#define DEFAULT_MUSIC "/mnt/SDCARD/Music"

/* ============================ graphics state ============================ */
enum { F_S, F_M, F_L, F_XL, F_NOTE, NF };
#define NFB 10
static TTF_Font *fonts[NF];
static TTF_Font *fbf[NFB][NF];     /* fallback fonts (fonts/fb_*): Arabic, Hebrew, Thai, ... */
static int nfb;
static SDL_Window *win;
static SDL_Renderer *ren;
static int W = 1024, H = 768;
static float sc = 1.f;
#define S(x) ((int)((x) * sc + 0.5f))

static const SDL_Color WHITE = { 255, 255, 255, 255 };
static const SDL_Color GRAY  = { 170, 170, 182, 255 };
static const SDL_Color DIM   = { 120, 120, 132, 255 };
static const SDL_Color ACCENT = { 100, 210, 150, 255 };

/* ---------- text cache ---------- */
#define TCN 400
typedef struct { uint32_t h, col; int font, maxw, w, hgt; char *s; SDL_Texture *t; } TC;
static TC tc[TCN];
static int tc_next;

static void tc_clear(void) {
    for (int i = 0; i < TCN; i++) {
        if (tc[i].t) SDL_DestroyTexture(tc[i].t);
        free(tc[i].s);
        memset(&tc[i], 0, sizeof tc[i]);
    }
}

/* ---- per-character font fallback (SDL_ttf has none built in) ---- */
static int u8next(const char **pp) {
    const unsigned char *p = (const unsigned char *)*pp; int cp, n;
    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
    else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
    else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) { cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); n = 4; }
    else { cp = 0xFFFD; n = 1; }
    *pp += n;
    return cp;
}
static TTF_Font *font_for(int f, int cp) {
    if (cp <= 0x20 || cp > 0xFFFF || TTF_GlyphIsProvided(fonts[f], (Uint16)cp)) return fonts[f];
    for (int j = 0; j < nfb; j++) if (fbf[j][f] && TTF_GlyphIsProvided(fbf[j][f], (Uint16)cp)) return fbf[j][f];
    return fonts[f];
}
typedef struct { TTF_Font *ft; char s[640]; int len; } Run;
static Run runs_[24];
static int split_runs(int f, const char *s) {
    int n = 0;
    for (const char *p = s; *p; ) {
        const char *nx = p; int cp = u8next(&nx);
        TTF_Font *ft = font_for(f, cp);
        if (n == 0 || runs_[n - 1].ft != ft) {
            if (n == 24) break;
            runs_[n].ft = ft; runs_[n].len = 0; runs_[n].s[0] = 0; n++;
        }
        Run *r = &runs_[n - 1];
        int l = (int)(nx - p);
        if (r->len + l < (int)sizeof r->s - 1) { memcpy(r->s + r->len, p, (size_t)l); r->len += l; r->s[r->len] = 0; }
        p = nx;
    }
    return n;
}
static int chain_width(int f, const char *s) {
    int total = 0, n = split_runs(f, s);
    for (int i = 0; i < n; i++) { int w = 0; TTF_SizeUTF8(runs_[i].ft, runs_[i].s, &w, NULL); total += w; }
    return total;
}
static SDL_Surface *render_chain(int f, const char *s, SDL_Color c) {
    int n = split_runs(f, s);
    if (n == 0) return NULL;
    if (n == 1) return TTF_RenderUTF8_Blended(runs_[0].ft, runs_[0].s, c);
    SDL_Surface *part[24];
    int w = 0, h = TTF_FontHeight(fonts[f]), asc = TTF_FontAscent(fonts[f]);
    TTF_Font *fts[24];
    for (int i = 0; i < n; i++) {
        fts[i] = runs_[i].ft;
        part[i] = TTF_RenderUTF8_Blended(runs_[i].ft, runs_[i].s, c);
        if (part[i]) { w += part[i]->w; if (part[i]->h > h) h = part[i]->h; }
    }
    SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, w > 0 ? w : 1, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (out) {
        SDL_FillRect(out, NULL, 0);
        int x = 0;
        for (int i = 0; i < n; i++) {
            if (!part[i]) continue;
            int dy = asc - TTF_FontAscent(fts[i]); if (dy < 0) dy = 0;
            SDL_Rect d = { x, dy, 0, 0 };
            SDL_SetSurfaceBlendMode(part[i], SDL_BLENDMODE_NONE);       /* straight copy onto the transparent target */
            SDL_BlitSurface(part[i], NULL, out, &d);
            x += part[i]->w;
        }
    }
    for (int i = 0; i < n; i++) if (part[i]) SDL_FreeSurface(part[i]);
    return out;
}
/* Word-wrap (breaks inside long words, e.g. CJK) into one surface; max 8 lines. */
static SDL_Surface *render_wrapped(int f, const char *s, int maxw, SDL_Color c) {
    enum { MAXL = 8 };
    static char line[MAXL][400];
    char cur[400]; int nl = 0, cl = 0, lastsp = -1;
    cur[0] = 0;
    const char *p = s;
    while (*p && nl < MAXL) {
        const char *nx = p; int cp = u8next(&nx), n = (int)(nx - p);
        if (cp == '\n') { memcpy(line[nl++], cur, (size_t)cl + 1); cl = 0; cur[0] = 0; lastsp = -1; p = nx; continue; }
        if (cl + n >= (int)sizeof cur - 1) { memcpy(line[nl++], cur, (size_t)cl + 1); cl = 0; cur[0] = 0; lastsp = -1; if (nl >= MAXL) break; }
        memcpy(cur + cl, p, (size_t)n); cl += n; cur[cl] = 0;
        if (cp == ' ') lastsp = cl - 1;
        if (cl > n && chain_width(f, cur) > maxw) {
            if (lastsp > 0) {
                memcpy(line[nl], cur, (size_t)lastsp); line[nl][lastsp] = 0; nl++;
                int rem = cl - (lastsp + 1);
                memmove(cur, cur + lastsp + 1, (size_t)rem + 1); cl = rem;
            } else {
                memcpy(line[nl], cur, (size_t)(cl - n)); line[nl][cl - n] = 0; nl++;
                memcpy(cur, p, (size_t)n); cur[n] = 0; cl = n;
            }
            lastsp = -1;
        }
        p = nx;
    }
    if (cl > 0 && nl < MAXL) memcpy(line[nl++], cur, (size_t)cl + 1);
    if (nl == 0) return NULL;
    SDL_Surface *ls[MAXL]; int w = 1, skip = TTF_FontLineSkip(fonts[f]);
    for (int i = 0; i < nl; i++) { ls[i] = render_chain(f, line[i], c); if (ls[i] && ls[i]->w > w) w = ls[i]->w; }
    SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, w, skip * nl, 32, SDL_PIXELFORMAT_ARGB8888);
    if (out) {
        SDL_FillRect(out, NULL, 0);
        for (int i = 0; i < nl; i++) if (ls[i]) {
            SDL_SetSurfaceBlendMode(ls[i], SDL_BLENDMODE_NONE);
            SDL_Rect d = { 0, i * skip, 0, 0 };
            SDL_BlitSurface(ls[i], NULL, out, &d);
        }
    }
    for (int i = 0; i < nl; i++) if (ls[i]) SDL_FreeSurface(ls[i]);
    return out;
}

#define WRAP 0x100
static TC *text_get(int fw, const char *s, int maxw, SDL_Color c) {
    int wrap = fw & WRAP, f = fw & 0xff;
    if (!fonts[f] || !s || !*s) return NULL;
    uint32_t col = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) h = (h ^ *p) * 16777619u;
    h ^= (uint32_t)(fw * 131 + maxw * 7) ^ col;
    for (int i = 0; i < TCN; i++)
        if (tc[i].t && tc[i].h == h && tc[i].font == fw && tc[i].maxw == maxw && tc[i].col == col && !strcmp(tc[i].s, s))
            return &tc[i];
    char buf[640];
    snprintf(buf, sizeof buf, "%s", s);
    int w = 0;
    w = chain_width(f, buf);
    if (!wrap && maxw > 0 && w > maxw) {               /* trim to fit and add an ellipsis */
        size_t n = strlen(buf);
        int ok = 0;
        while (n > 0) {
            n--;
            while (n > 0 && (((unsigned char)buf[n]) & 0xC0) == 0x80) n--;
            buf[n] = 0;
            char t[700];
            snprintf(t, sizeof t, "%s\xE2\x80\xA6", buf);
            w = chain_width(f, t);
            if (w <= maxw) { snprintf(buf, sizeof buf, "%s", t); ok = 1; break; }
        }
        if (!ok) snprintf(buf, sizeof buf, "\xE2\x80\xA6");
    }
    SDL_Surface *sf = wrap ? render_wrapped(f, s, maxw, c) : render_chain(f, buf, c);
    if (!sf) return NULL;
    TC *e = &tc[tc_next]; tc_next = (tc_next + 1) % TCN;
    if (e->t) SDL_DestroyTexture(e->t);
    free(e->s);
    e->t = SDL_CreateTextureFromSurface(ren, sf);
    e->w = sf->w; e->hgt = sf->h;
    SDL_FreeSurface(sf);
    e->s = strdup(s); e->h = h; e->col = col; e->font = fw; e->maxw = maxw;
    return e->t ? e : NULL;
}

/* align: 0 left, 1 center, 2 right (x is the anchor) */
static int draw_text(int f, const char *s, int x, int y, int maxw, int align, SDL_Color c) {
    TC *t = text_get(f, s, maxw, c);
    if (!t) return 0;
    SDL_Rect d = { x, y, t->w, t->hgt };
    if (align == 1) d.x = x - t->w / 2; else if (align == 2) d.x = x - t->w;
    SDL_RenderCopy(ren, t->t, NULL, &d);
    return t->w;
}

static int draw_text_wrap(int f, const char *s, int x, int y, int w, int maxh, SDL_Color c) {
    TC *t = text_get(f | WRAP, s, w, c);
    if (!t) return 0;
    int h = t->hgt > maxh ? maxh : t->hgt, sw = t->w > w ? w : t->w;
    SDL_Rect src = { 0, 0, sw, h }, d = { x, y, sw, h };
    SDL_RenderCopy(ren, t->t, &src, &d);
    return h;
}

/* ---------- primitives ---------- */
static void fill(int x, int y, int w, int h, int r, int g, int b, int a) {
    SDL_SetRenderDrawColor(ren, r, g, b, a);
    SDL_Rect d = { x, y, w, h };
    SDL_RenderFillRect(ren, &d);
}
static void circle(int cx, int cy, int r, int cr, int cg, int cb) {
    SDL_SetRenderDrawColor(ren, cr, cg, cb, 255);
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}
static void tri(int cx, int cy, int h, int dir, int r, int g, int b) {   /* dir +1 right, -1 left */
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    for (int i = 0; i < h; i++) {
        float f = 1.f - fabsf((i - h / 2.f) / (h / 2.f));
        int len = (int)(f * h * 0.9f);
        int y = cy - h / 2 + i;
        int x0 = cx - (int)(h * 0.35f) * dir;
        SDL_RenderDrawLine(ren, x0, y, x0 + len * dir, y);
    }
}

/* ============================ thumbnails / art ============================ */
#define THUMB_CAP 150
static SDL_Texture **th_tex; static uint8_t *th_state; static uint32_t *th_used, *th_col;
static int th_loaded, inflight, th_n, lbusy; static uint32_t frame_no;
static SDL_Texture *now_tex, *blur_tex;
static SDL_Texture *glow_tex; static int glow_sz, glow_n;
static uint32_t now_col;
static int now_wanted = -1, now_state;      /* 0 none, 1 pending, 2 loaded, 3 no art */

typedef struct { int song, kind, size; } Job;
typedef struct { int song, kind, ok; Img img; } Res;
#define QN 64
static Job jq[QN]; static int jq_n;
static Res rq[QN]; static int rq_n;
static pthread_mutex_t lmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lcv = PTHREAD_COND_INITIALIZER;
static int lquit; static pthread_t loader_thr;

static void *loader_main(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&lmu);
        while (!lquit && jq_n == 0) pthread_cond_wait(&lcv, &lmu);
        if (lquit) { pthread_mutex_unlock(&lmu); break; }
        Job j = jq[0]; memmove(&jq[0], &jq[1], (size_t)(jq_n - 1) * sizeof(Job)); jq_n--; lbusy = 1;
        pthread_mutex_unlock(&lmu);
        Res r; memset(&r, 0, sizeof r);
        r.song = j.song; r.kind = j.kind;
        r.ok = art_extract(g_songs[j.song].path, j.size, &r.img) == 0;
        pthread_mutex_lock(&lmu);
        if (rq_n < QN) rq[rq_n++] = r; else if (r.ok) free(r.img.px);
        lbusy = 0;
        pthread_mutex_unlock(&lmu);
    }
    return NULL;
}

static void request_thumb(int song, int size) {
    if (th_state[song] != 0 || inflight >= 24) return;
    pthread_mutex_lock(&lmu);
    if (jq_n < QN) {
        jq[jq_n++] = (Job){ song, 0, size };
        th_state[song] = 1; inflight++;
        pthread_cond_signal(&lcv);
    }
    pthread_mutex_unlock(&lmu);
}
static void request_now(int song, int size) {
    pthread_mutex_lock(&lmu);
    if (jq_n < QN) {
        memmove(&jq[1], &jq[0], (size_t)jq_n * sizeof(Job));
        jq[0] = (Job){ song, 1, size }; jq_n++;
        pthread_cond_signal(&lcv);
    }
    pthread_mutex_unlock(&lmu);
}

static SDL_Texture *tex_from_px(const Img *im) {
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, im->size, im->size);
    if (t) { SDL_UpdateTexture(t, NULL, im->px, im->size * 4); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); }
    return t;
}

/* Round the corners of a square RGBA image by fading alpha (grid/list thumbnails only). */
static void round_corners(Img *im) {
    int n = im->size; float r = n * 0.12f;
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
        if (!((x < r || x >= n - r) && (y < r || y >= n - r))) continue;
        float cx = x < r ? r : n - r, cy = y < r ? r : n - r;
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        float d = sqrtf(dx * dx + dy * dy) - r, a = d <= -0.5f ? 1.f : d >= 0.5f ? 0.f : 0.5f - d;
        im->px[((size_t)y * n + x) * 4 + 3] = (uint8_t)(a * 255);
    }
}

/* Saturated "neon" version of the art's dominant color, packed 0xRRGGBB. */
static uint32_t neon_color(const Img *im) {
    double R = 0, G = 0, B = 0, Wt = 0;
    for (int y = 0; y < im->size; y += 4) for (int x = 0; x < im->size; x += 4) {
        const uint8_t *p = im->px + ((size_t)y * im->size + x) * 4;
        int mx = p[0] > p[1] ? p[0] : p[1]; if (p[2] > mx) mx = p[2];
        int mn = p[0] < p[1] ? p[0] : p[1]; if (p[2] < mn) mn = p[2];
        double w = (mx - mn) + 16.0; if (mx < 40) w *= 0.1;
        R += p[0] * w; G += p[1] * w; B += p[2] * w; Wt += w;
    }
    if (Wt <= 0) return 0x00DCFF;
    double r = R / Wt / 255, g = G / Wt / 255, b = B / Wt / 255;
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), d = mx - mn, h = 0, sat = mx > 0 ? d / mx : 0;
    if (d > 0) {
        if (mx == r) h = fmod((g - b) / d, 6); else if (mx == g) h = (b - r) / d + 2; else h = (r - g) / d + 4;
        h /= 6; if (h < 0) h += 1;
    }
    if (sat > 0.12) sat = fmax(sat, 0.8); else sat = 0.0;        /* gray art -> white glow */
    double v = 1.0, c = v * sat, hh = h * 6, xx = c * (1 - fabs(fmod(hh, 2) - 1)), m = v - c, rr, gg, bb;
    if (hh < 1) { rr = c; gg = xx; bb = 0; } else if (hh < 2) { rr = xx; gg = c; bb = 0; }
    else if (hh < 3) { rr = 0; gg = c; bb = xx; } else if (hh < 4) { rr = 0; gg = xx; bb = c; }
    else if (hh < 5) { rr = xx; gg = 0; bb = c; } else { rr = c; gg = 0; bb = xx; }
    return ((uint32_t)((rr + m) * 255) << 16) | ((uint32_t)((gg + m) * 255) << 8) | (uint32_t)((bb + m) * 255);
}

/* Gaussian-style blur: shrink to 64x64, then three box-blur passes (approximates a Gaussian). */
static SDL_Texture *make_blur(const Img *im) {
    enum { N = 64, R = 4 };
    static uint8_t a[N * N * 4], b[N * N * 4];
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        int x0 = x * im->size / N, x1 = (x + 1) * im->size / N, y0 = y * im->size / N, y1 = (y + 1) * im->size / N;
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        long s[3] = { 0, 0, 0 }; int cnt = 0;
        for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
            const uint8_t *p = im->px + ((size_t)yy * im->size + xx) * 4;
            s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; cnt++;
        }
        uint8_t *o = a + (y * N + x) * 4;
        o[0] = s[0] / cnt; o[1] = s[1] / cnt; o[2] = s[2] / cnt; o[3] = 255;
    }
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {      /* horizontal a -> b */
            int s[3] = { 0, 0, 0 };
            for (int k = -R; k <= R; k++) { int xx = x + k; xx = xx < 0 ? 0 : xx >= N ? N - 1 : xx;
                const uint8_t *p = a + (y * N + xx) * 4; s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; }
            uint8_t *o = b + (y * N + x) * 4; o[0] = s[0] / (2 * R + 1); o[1] = s[1] / (2 * R + 1); o[2] = s[2] / (2 * R + 1); o[3] = 255;
        }
        for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {      /* vertical b -> a */
            int s[3] = { 0, 0, 0 };
            for (int k = -R; k <= R; k++) { int yy = y + k; yy = yy < 0 ? 0 : yy >= N ? N - 1 : yy;
                const uint8_t *p = b + (yy * N + x) * 4; s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; }
            uint8_t *o = a + (y * N + x) * 4; o[0] = s[0] / (2 * R + 1); o[1] = s[1] / (2 * R + 1); o[2] = s[2] / (2 * R + 1); o[3] = 255;
        }
    }
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, N, N);
    if (t) SDL_UpdateTexture(t, NULL, a, N * 4);
    return t;
}

/* Drop queued art jobs and wait for the loader to go idle (before the song list is replaced). */
static void loader_quiesce(void) {
    pthread_mutex_lock(&lmu); jq_n = 0; pthread_mutex_unlock(&lmu);
    for (int t = 0; t < 500; t++) {
        pthread_mutex_lock(&lmu); int b = lbusy; pthread_mutex_unlock(&lmu);
        if (!b) break;
        SDL_Delay(10);
    }
    pthread_mutex_lock(&lmu);
    for (int i = 0; i < rq_n; i++) if (rq[i].ok) free(rq[i].img.px);
    rq_n = 0;
    pthread_mutex_unlock(&lmu);
    inflight = 0;
}
/* (Re)allocate the per-song texture/state arrays for the current g_nsongs. */
static void thumbs_realloc(void) {
    if (th_tex) for (int i = 0; i < th_n; i++) if (th_tex[i]) SDL_DestroyTexture(th_tex[i]);
    free(th_tex); free(th_state); free(th_used); free(th_col);
    th_n = g_nsongs;
    th_tex = calloc(th_n ? th_n : 1, sizeof(SDL_Texture *));
    th_state = calloc(th_n ? th_n : 1, 1);
    th_used = calloc(th_n ? th_n : 1, sizeof(uint32_t));
    th_col = calloc(th_n ? th_n : 1, sizeof(uint32_t));
    th_loaded = 0;
    if (now_tex) { SDL_DestroyTexture(now_tex); now_tex = NULL; }
    if (blur_tex) { SDL_DestroyTexture(blur_tex); blur_tex = NULL; }
    now_wanted = -1; now_state = 0;
}

static int loader_poll(void) {
    int got = 0;
    for (;;) {
        Res r;
        pthread_mutex_lock(&lmu);
        if (!rq_n) { pthread_mutex_unlock(&lmu); break; }
        r = rq[0]; memmove(&rq[0], &rq[1], (size_t)(rq_n - 1) * sizeof(Res)); rq_n--;
        pthread_mutex_unlock(&lmu);
        got = 1;
        if (r.song < 0 || r.song >= th_n) { if (r.ok) free(r.img.px); continue; }
        if (r.kind == 0) {
            if (inflight > 0) inflight--;
            if (ren && th_state[r.song] == 1) {
                if (r.ok) {
                    if (th_loaded >= THUMB_CAP) {          /* evict least-recently drawn */
                        int best = -1; uint32_t bu = 0xffffffffu;
                        for (int i = 0; i < g_nsongs; i++)
                            if (th_state[i] == 2 && th_used[i] < bu && th_used[i] + 2 < frame_no) { best = i; bu = th_used[i]; }
                        if (best >= 0) { SDL_DestroyTexture(th_tex[best]); th_tex[best] = NULL; th_state[best] = 0; th_loaded--; }
                    }
                    round_corners(&r.img);
                    th_tex[r.song] = tex_from_px(&r.img);
                    th_state[r.song] = th_tex[r.song] ? 2 : 3;
                    th_used[r.song] = frame_no;
                    th_col[r.song] = neon_color(&r.img);
                    if (th_tex[r.song]) th_loaded++;
                } else th_state[r.song] = 3;
            }
        } else if (ren && r.song == now_wanted && now_state == 1) {
            if (r.ok) { now_tex = tex_from_px(&r.img); blur_tex = make_blur(&r.img); now_col = neon_color(&r.img); now_state = 2; }
            else now_state = 3;
        }
        if (r.ok) free(r.img.px);
    }
    return got;
}

/* ============================ platform bits ============================ */
static int bl_saved = -1, bl_default = 128;

static int bl_read(void) {
    glob_t g; int v = -1;
    if (glob("/sys/class/backlight/*/brightness", 0, NULL, &g) == 0) {
        FILE *f = fopen(g.gl_pathv[0], "r"); if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
        globfree(&g); return v;
    }
    FILE *f = fopen("/sys/class/disp/disp/attr/lcdbl", "r");
    if (f) {
        char b[256]; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f);
        char *p = b, *end; long last = -1;
        while (*p) { long x = strtol(p, &end, 10); if (end != p) { last = x; p = end; } else p++; }
        if (last > 0 && last <= 255) v = (int)last;
    }
    return v;
}
static void bl_write(int v) {
    glob_t g;
    if (glob("/sys/class/backlight/*/brightness", 0, NULL, &g) == 0) {
        FILE *f = fopen(g.gl_pathv[0], "w"); if (f) { fprintf(f, "%d", v); fclose(f); }
        globfree(&g); return;
    }
    FILE *f = fopen("/sys/class/disp/disp/attr/lcdbl", "w");
    if (f) { fprintf(f, "lcd0 1 %d", v); fclose(f); }
}

static char scr_off_cmd[200], scr_on_cmd[200];      /* optional shell commands from buttons.ini */
static int screen_ioctl = 1, screen_backlight = 0, screen_blank = 0;
/* screen_ioctl: change the brightness through /dev/disp, the way the stock OS does (0 = dark, then put the old value back).
   screen_backlight / screen_blank are extra, off by default: cutting the backlight or blanking the framebuffer behind the
   stock OS's back confused its brightness keys and hid its overlays after waking. */
static int bl_via_ioctl;
static void disp_backlight(int on) {                /* DISP_LCD_BACKLIGHT_ENABLE = 0x104, DISABLE = 0x105 */
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) return;
    unsigned long args[4] = { 0, 0, 0, 0 };
    if (ioctl(fd, on ? 0x104 : 0x105, args)) {}
    close(fd);
}
static int disp_get_brightness(void) {              /* DISP_LCD_GET_BRIGHTNESS = 0x103 (returns the value) */
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) return -1;
    unsigned long args[4] = { 0, 0, 0, 0 };
    int v = ioctl(fd, 0x103, args);
    close(fd);
    return v;
}
static int disp_set_brightness(int v) {             /* DISP_LCD_SET_BRIGHTNESS = 0x102 */
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) return -1;
    unsigned long args[4] = { 0, (unsigned long)v, 0, 0 };
    int r = ioctl(fd, 0x102, args);
    close(fd);
    return r;
}
static void panel_power(int on) {
    const char *cmd = on ? scr_on_cmd : scr_off_cmd;
    if (cmd[0]) { if (system(cmd)) {} }
    if (screen_backlight) disp_backlight(on);
    if (screen_blank) {                                       /* 4 = power the panel down, 0 = back on */
        FILE *f = fopen("/sys/class/graphics/fb0/blank", "w");
        if (f) { fprintf(f, "%d", on ? 0 : 4); fclose(f); }
    }
}

/* ============================ input ============================ */
enum { BT_UP, BT_DOWN, BT_LEFT, BT_RIGHT, BT_A, BT_B, BT_X, BT_Y, BT_L1, BT_R1, BT_SELECT, BT_START, BT_MENU, BT_L2, BT_R2, BT_COUNT };
/* Joystick button indices (TrimUI Brick stock mapping). Override in buttons.ini. */
static int jmap[BT_COUNT] = { -1, -1, -1, -1, 1, 0, 3, 2, 4, 5, 6, 7, 8, -1, -1 };
static const char *bt_names[BT_COUNT] = { "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "L1", "R1", "SELECT", "START", "MENU", "L2", "R2" };

/* ---- global hotkeys (work while the player is hidden / in background) ---- */
enum { HK_PAUSE, HK_NEXT, HK_PREV, HK_N };
typedef struct { unsigned mask; int hold, armed, fired; Uint32 t0; } Hotkey;
static Hotkey hks[HK_N];
static int jsmap[BT_COUNT] = { -1, -1, -1, -1, 1, 0, 3, 2, 4, 5, 6, 7, 8, -1, -1 };   /* /dev/input/js0 button numbers */
static int audio_log = 1;
static int led_on_cfg = 1, led_screen_off = 1;    /* LED sync (menu), and "LEDs dark while the screen is off" (buttons.ini) */
static int toast_on = 1, clock_12h = 0, vis_ms = 33;     /* vis_ms: milliseconds per visualizer frame */
static int axis_l2 = 2, axis_r2 = 5;      /* L2/R2 triggers are analog axes on the Brick */

static int bt_from_name(const char *n) {
    for (int i = 0; i < BT_COUNT; i++) if (!strcasecmp(n, bt_names[i])) return i;
    return -1;
}
static void parse_hk(Hotkey *h, const char *v) {      /* "MENU+R1" or "MENU+SELECT@800" (hold ms) */
    char b[64]; snprintf(b, sizeof b, "%s", v);
    b[strcspn(b, "\r\n")] = 0;
    h->mask = 0; h->hold = 0; h->armed = h->fired = 0;
    char *at = strchr(b, '@'); if (at) { *at = 0; h->hold = atoi(at + 1); }
    for (char *t = strtok(b, "+ \t"); t; t = strtok(NULL, "+ \t")) {
        int i = bt_from_name(t);
        if (i >= BT_A) h->mask |= 1u << i;
    }
}

static void load_buttons(void) {
    parse_hk(&hks[HK_PAUSE], "MENU+START");
    parse_hk(&hks[HK_NEXT],  "MENU+R1");
    parse_hk(&hks[HK_PREV],  "MENU+L1");
    FILE *f = fopen("buttons.ini", "r");
    if (!f) return;
    char line[300];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='); if (!eq || line[0] == '#') continue;
        *eq = 0; const char *val = eq + 1; int v = atoi(val);
        line[strcspn(line, " \t")] = 0;
        if (!strcasecmp(line, "screen_ioctl")) { screen_ioctl = v; continue; }
        if (!strcasecmp(line, "screen_backlight")) { screen_backlight = v; continue; }
        if (!strcasecmp(line, "screen_blank")) { screen_blank = v; continue; }
        if (!strcasecmp(line, "audio_log"))    { audio_log = v; continue; }
        if (!strcasecmp(line, "led_screen_off")) { led_screen_off = v; continue; }
        if (!strcasecmp(line, "vis_sync_ms"))  { engine_set_vis_sync(v); continue; }
        if (!strcasecmp(line, "vis_ms"))       { vis_ms = v < 16 ? 16 : v > 200 ? 200 : v; continue; }
        if (!strcasecmp(line, "clock_12h"))    { clock_12h = v; continue; }
        if (!strcasecmp(line, "screen_off_cmd")) { snprintf(scr_off_cmd, sizeof scr_off_cmd, "%s", val); scr_off_cmd[strcspn(scr_off_cmd, "\r\n")] = 0; continue; }
        if (!strcasecmp(line, "screen_on_cmd"))  { snprintf(scr_on_cmd, sizeof scr_on_cmd, "%s", val); scr_on_cmd[strcspn(scr_on_cmd, "\r\n")] = 0; continue; }
        if (!strncasecmp(line, "js_", 3)) { int i = bt_from_name(line + 3); if (i >= BT_A) jsmap[i] = v; continue; }
        if (!strcasecmp(line, "hk_pause")) { parse_hk(&hks[HK_PAUSE], val); continue; }
        if (!strcasecmp(line, "hk_next"))  { parse_hk(&hks[HK_NEXT], val);  continue; }
        if (!strcasecmp(line, "hk_prev"))  { parse_hk(&hks[HK_PREV], val);  continue; }
        if (!strcasecmp(line, "toast"))    { toast_on = v; continue; }
        if (!strcasecmp(line, "axis_l2"))  { axis_l2 = v; continue; }
        if (!strcasecmp(line, "axis_r2"))  { axis_r2 = v; continue; }
        if (!strcasecmp(line, "brightness")) { bl_default = v; continue; }
        int i = bt_from_name(line); if (i >= BT_A) jmap[i] = v;
    }
    fclose(f);
}

/* ============================ app state ============================ */
enum { SCR_GRID, SCR_NOW, SCR_SEARCH, SCR_EQ, SCR_TAGS, SCR_PLNAME };
enum { M_RESUME, M_BG, M_OFF, M_QUIT, M_USB, M_THEME, M_RESCAN, M_SEARCH, M_LED, M_EQ, M_TAGS, M_SETTINGS, M_ADDPL };
static int scr = SCR_GRID, dirty = 1, pending_song = -1, anim;
static int menu_open, menu_sel, menu_n, menu_ids[12];
static int screen_off, bg, want_quit, dbg;
static const char *music_root = DEFAULT_MUSIC;
/* what was playing last time (resume.cfg), and how the current queue was built (so it can be rebuilt) */
static int q_kind;                                        /* 1 = Songs order, 2 = album then the rest, 3 = whole library (shuffle all) */
static int rs_valid, rs_kind, rs_shuffle, rs_repeat; static double rs_pos; static char rs_path[512];
static Uint32 last_input;            /* ticks of the last button event (for audio.log) */
static int usb_on, usb_dirty;      /* USB Music (MTP) mode; dirty = library may have changed */
static int vis_mode;               /* 0 off, 1 bars, 2 waveform, 3 circle */
static void led_restore(void);
typedef struct { char name[40]; uint32_t color, bg, bar, color2; int has_bg, has_bar, has_c2; } Theme;   /* color2 (optional) turns the glow into a gradient */   /* color 0 = follow the album art */
static Theme themes[64]; static int nthemes, theme_cur;
static int theme_open, theme_sel, theme_top, theme_orig;
static int set_open, set_sel, set_n, set_ids[4];          /* Settings sub-menu (Theme, LED sync, Rescan) */
static int pick_open, pick_sel, pick_top;                 /* "Add to playlist" picker */
#define TINT_ALPHA 204                                    /* Now Playing: strength (0-255) of the theme-colored veil over the blurred art; alternate themes only */

/* themes that were renamed keep working from an old themes.ini / settings.cfg */
static const char *theme_alias(const char *n) {
    static const char *const m[][2] = { { "Gawr Gura", "Shark" }, { "Nanashi Mumei", "Owl" }, { "Shiori Novella", "Archiver" }, { "Mori Calliope", "Death" } };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) if (!strcasecmp(n, m[i][0])) return m[i][1];
    return n;
}
static void themes_init(void) {
    static const struct { const char *n; uint32_t c, c2; } d[] = {
        { "Default", 0, 0 }, { "Monet Purple", 0xD0BCFF, 0 }, { "Monet Blue", 0xA8C8FF, 0 }, { "Monet Teal", 0x7FD6CC, 0 },
        { "Monet Green", 0x9FD89C, 0 }, { "Monet Orange", 0xFFB77A, 0 }, { "Monet Rose", 0xFFB0C8, 0 }, { "Detective", 0xD9A441, 0 },
        { "Jerboa", 0xE8956A, 0 }, { "Shark", 0x4FA3FF, 0 }, { "Fish", 0x5EC2B0, 0 }, { "Priestess", 0x9C63D6, 0 }, { "Phoenix", 0xFF6A1F, 0 },
        { "Death", 0xA8203F, 0 }, { "Hope", 0xD86BD0, 0 }, { "Warden", 0x2F5FE0, 0 }, { "Rat", 0xFF4B55, 0 }, { "Nature", 0x6BD66F, 0 },
        { "Tapir", 0xC4CAD6, 0 }, { "Owl", 0xD9B38C, 0 }, { "Orca", 0x22C1D6, 0 }, { "Space", 0xFFE06B, 0 }, { "Archiver", 0x9B7BFF, 0 },
        { "Demon", 0xC2185B, 0 }, { "Rock", 0xE1A0C8, 0 }, { "Hellhounds", 0xF78FB3, 0x8FD3FF }, { "Queen", 0xE02845, 0 },
        { "Snow Leopard", 0xA9C7E8, 0 }, { "Automaton", 0x7FC4B0, 0 }, { "Gremlin", 0xA6E34B, 0 },
    };
    int have_ini = 0;                                /* a themes.ini with at least one theme is the whole list (plus Default); the built-ins are only used without one */
    { FILE *tf = fopen("themes.ini", "r"); char tl[96];
      if (tf) { while (fgets(tl, sizeof tl, tf)) if (tl[0] != '#' && strchr(tl, '=')) { have_ini = 1; break; } fclose(tf); } }
    for (unsigned i = 0; i < sizeof d / sizeof d[0]; i++) { if (i > 0 && have_ini) break; snprintf(themes[nthemes].name, sizeof themes[0].name, "%s", d[i].n); themes[nthemes].color = d[i].c; themes[nthemes].color2 = d[i].c2; themes[nthemes].has_c2 = d[i].c2 != 0; nthemes++; }
    FILE *f = fopen("themes.ini", "r");              /* optional: "Name=RRGGBB" overrides or adds themes */
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='); if (!eq || line[0] == '#') continue;
        *eq = 0;
        uint32_t c[4] = { 0, 0, 0, 0 }; int ok[4] = { 0, 0, 0, 0 }, n = 0;     /* glow[,background[,bars[,glow2]]]  -- empty field = automatic */
        for (char *p = eq + 1; n < 4; ) {
            char *end; c[n] = (uint32_t)strtoul(p, &end, 16) & 0xFFFFFF; ok[n] = end != p; n++;
            while (*end == ' ' || *end == '\t') end++;
            if (*end != ',') break;
            p = end + 1;
        }
        int found = -1; const char *tname = theme_alias(line);
        for (int i = 0; i < nthemes; i++) if (!strcasecmp(themes[i].name, tname)) found = i;
        if (found < 0 && nthemes < 64) { found = nthemes++; snprintf(themes[found].name, sizeof themes[0].name, "%s", tname); }
        if (found > 0 && ok[0]) { themes[found].color = c[0]; themes[found].has_bg = ok[1]; themes[found].bg = c[1]; themes[found].has_bar = ok[2]; themes[found].bar = c[2]; themes[found].has_c2 = ok[3]; themes[found].color2 = c[3]; }
    }
    fclose(f);
}
static uint32_t pick_color(uint32_t art) { return theme_cur > 0 ? themes[theme_cur].color : art; }
/* kind 0 = background, 1 = top/bottom bars (and menu boxes). Default theme keeps its fixed colors;
   other themes use the explicit color from themes.ini, or a dark shade of the glow color. */
static void theme_rgb(int kind, int dr, int dg, int db, int *r, int *g, int *b) {
    if (theme_cur <= 0) { *r = dr; *g = dg; *b = db; return; }
    const Theme *t = &themes[theme_cur];
    if (kind == 0 ? t->has_bg : t->has_bar) {
        uint32_t c = kind == 0 ? t->bg : t->bar;
        *r = (c >> 16) & 255; *g = (c >> 8) & 255; *b = c & 255; return;
    }
    float k = kind == 0 ? 0.09f : 0.20f;
    *r = (int)(((t->color >> 16) & 255) * k); *g = (int)(((t->color >> 8) & 255) * k); *b = (int)((t->color & 255) * k);
}
static void settings_save(void) {
    FILE *f = fopen("settings.cfg", "w"); if (!f) return;
    fprintf(f, "theme=%s\nvis=%d\nled=%d\neq=", themes[theme_cur].name, vis_mode, led_on_cfg);
    for (int k = 0; k < 10; k++) fprintf(f, "%d%s", (int)lroundf(engine_eq_get(k)), k < 9 ? "," : "\n");
    fclose(f);
}
static void settings_load(void) {
    FILE *f = fopen("settings.cfg", "r"); if (!f) return;
    char line[96];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "vis=", 4)) vis_mode = atoi(line + 4) & 3;
        else if (!strncmp(line, "led=", 4)) led_on_cfg = atoi(line + 4) != 0;
        else if (!strncmp(line, "eq=", 3)) { char *p = line + 3; for (int k = 0; k < 10 && *p; k++) { engine_eq_set(k, (float)atoi(p)); char *c = strchr(p, ','); if (!c) break; p = c + 1; } }
        else if (!strncmp(line, "theme=", 6)) { const char *tn = theme_alias(line + 6); for (int i = 0; i < nthemes; i++) if (!strcasecmp(themes[i].name, tn)) theme_cur = i; }
    }
    fclose(f);
}
static Snap sn;
static Uint32 ev_base;
static int g_cols = 4, g_rows = 2, g_T, g_gx, g_x0, g_rowh, g_vis = 2, g_lrow = 104, g_lvis = 6;

static void grid_layout(void) {
    int area = H - S(60) - S(40);
    g_rowh = (area - S(40)) / g_rows;                 /* room above row 1 for the raised selection */
    g_T = g_rowh - S(66) - S(8);                      /* tile = row minus label space */
    int tw = (W - S(40)) / g_cols - S(8);
    if (g_T > tw) g_T = tw;
    g_gx = S(8);                                      /* tiles nearly touching */
    g_x0 = (W - (g_cols * g_T + (g_cols - 1) * g_gx)) / 2;
    g_vis = g_rows;
    g_lrow = S(104);
    g_lvis = area / g_lrow; if (g_lvis < 1) g_lvis = 1;
}

/* ---------- gfx lifecycle ---------- */
static const char *FONT_PATHS[] = { "fonts/font.otf", "fonts/font.ttf", NULL };

static int gfx_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) return -1;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    int w = 1024, h = 768; SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0) { w = dm.w; h = dm.h; }
    win = SDL_CreateWindow("Vibe", 0, 0, w, h, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
    if (!win) return -1;
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) return -1;
    SDL_GetRendererOutputSize(ren, &W, &H);
    sc = H / 768.f;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_ShowCursor(SDL_DISABLE);
    int px[NF] = { S(20), S(25), S(32), S(42), S(130) };
    for (int i = 0; i < NF; i++)
        for (int p = 0; FONT_PATHS[p] && !fonts[i]; p++) fonts[i] = TTF_OpenFont(FONT_PATHS[p], px[i]);
    nfb = 0;
    glob_t fg;
    if (glob("fonts/fb_*", 0, NULL, &fg) == 0) {
        for (size_t k = 0; k < fg.gl_pathc && nfb < NFB; k++) {
            int any = 0;
            for (int i = 0; i < NF; i++) { if (i == F_NOTE) continue; fbf[nfb][i] = TTF_OpenFont(fg.gl_pathv[k], px[i]); if (fbf[nfb][i]) any = 1; }
            if (any) nfb++;
        }
        globfree(&fg);
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) SDL_JoystickOpen(i);
    grid_layout();
    return 0;
}

static void gfx_shutdown(void) {
    tc_clear();
    if (th_tex) for (int i = 0; i < g_nsongs; i++) {
        if (th_tex[i]) { SDL_DestroyTexture(th_tex[i]); th_tex[i] = NULL; }
        th_state[i] = 0;
    }
    th_loaded = 0;
    if (glow_tex) { SDL_DestroyTexture(glow_tex); glow_tex = NULL; glow_sz = 0; }
    if (now_tex) { SDL_DestroyTexture(now_tex); now_tex = NULL; }
    if (blur_tex) { SDL_DestroyTexture(blur_tex); blur_tex = NULL; }
    now_wanted = -1; now_state = 0;
    for (int i = 0; i < NF; i++) if (fonts[i]) { TTF_CloseFont(fonts[i]); fonts[i] = NULL; }
    for (int j = 0; j < NFB; j++) for (int i = 0; i < NF; i++) if (fbf[j][i]) { TTF_CloseFont(fbf[j][i]); fbf[j][i] = NULL; }
    nfb = 0;
    if (ren) { SDL_DestroyRenderer(ren); ren = NULL; }
    if (win) { SDL_DestroyWindow(win); win = NULL; }
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK);
}

/* ============================ drawing ============================ */
static void fmt_time(double s, char *o) { int t = (int)s; snprintf(o, 16, "%d:%02d", t / 60, t % 60); }

static void fill_round(int x, int y, int w, int h, int r, int cr, int cg, int cb, int ca) {
    SDL_SetRenderDrawColor(ren, cr, cg, cb, ca);
    for (int i = 0; i < h; i++) {
        float off = i < r ? r - i - 0.5f : (i >= h - r ? i - (h - r) + 0.5f : 0.f);
        int inset = off > 0 ? r - (int)sqrtf((float)(r * r) - off * off) : 0;
        SDL_RenderDrawLine(ren, x + inset, y + i, x + w - 1 - inset, y + i);
    }
}

static void draw_placeholder(int x, int y, int sz, int big) {
    fill_round(x, y, sz, sz, big ? 0 : sz * 12 / 100, 44, 44, 54, 255);
    TC *t = text_get(big ? F_NOTE : F_L, "\xE2\x99\xAA", 0, DIM);
    if (t) { SDL_Rect d = { x + (sz - t->w) / 2, y + (sz - t->hgt) / 2, t->w, t->hgt }; SDL_RenderCopy(ren, t->t, NULL, &d); }
}

/* ---------- neon glow (color follows the highlighted album's art) ---------- */
static float gcol[3] = { 0, 220, 255 };
static int gtarget[3] = { 0, 220, 255 };
static uint32_t th_color(int song) { uint32_t c = th_col[song]; return c ? c : 0x00DCFFu; }
static void glow_target(uint32_t c) { gtarget[0] = (c >> 16) & 255; gtarget[1] = (c >> 8) & 255; gtarget[2] = c & 255; }
static void glow_step(void) {
    for (int i = 0; i < 3; i++) {
        float d = gtarget[i] - gcol[i];
        if (fabsf(d) > 1.5f) { gcol[i] += d * 0.3f; anim = 1; } else gcol[i] = (float)gtarget[i];
    }
}
/* Gradient themes (a 4th color in themes.ini): the glow runs from the glow color (left) to color2 (right). */
static int grad_on(void) { return theme_cur > 0 && themes[theme_cur].has_c2; }
static void grad_rgb(float t, int *r, int *g, int *b) {
    *r = (int)gcol[0]; *g = (int)gcol[1]; *b = (int)gcol[2];
    if (!grad_on()) return;
    if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
    uint32_t c = themes[theme_cur].color2;
    *r += (int)((((c >> 16) & 255) - *r) * t); *g += (int)((((c >> 8) & 255) - *g) * t); *b += (int)(((c & 255) - *b) * t);
}
static void grad_rect(int x, int y, int w, int h, int a, int lighten, int gx, int gw) {
    int n = grad_on() ? 24 : 1;
    for (int i = 0; i < n; i++) {
        int x0 = x + w * i / n, x1 = x + w * (i + 1) / n; if (x1 <= x0) continue;
        int r, g, b; grad_rgb(((x0 + x1) * 0.5f - gx) / (gw > 0 ? gw : 1), &r, &g, &b);
        if (lighten) { r = (r + 255) / 2; g = (g + 255) / 2; b = (b + 255) / 2; }
        SDL_SetRenderDrawColor(ren, r, g, b, a);
        SDL_RenderDrawLine(ren, x0, y, x1 - 1, y); SDL_RenderDrawLine(ren, x0, y + h - 1, x1 - 1, y + h - 1);
    }
    for (int side = 0; side < 2; side++) {
        int r, g, b; grad_rgb((float)side, &r, &g, &b);
        if (lighten) { r = (r + 255) / 2; g = (g + 255) / 2; b = (b + 255) / 2; }
        SDL_SetRenderDrawColor(ren, r, g, b, a);
        int xx = side ? x + w - 1 : x;
        SDL_RenderDrawLine(ren, xx, y, xx, y + h - 1);
    }
}
static void glow_rect(int x, int y, int w, int h) {
    for (int k = 12; k >= 2; k--) {
        float f = 1.f - (k - 2) / 10.f;
        grad_rect(x - k, y - k, w + 2 * k, h + 2 * k, (int)(130 * f * f), 0, x, w);
    }
    grad_rect(x - 1, y - 1, w + 2, h + 2, 255, 0, x, w);
    grad_rect(x, y, w, h, 255, 1, x, w);
}

/* Thick rounded glow for the grid selection: pre-rendered once, tinted with the glow color, added on top. */
static void ensure_glow(int Ts) {
    if (glow_tex && glow_sz == Ts) return;
    if (glow_tex) { SDL_DestroyTexture(glow_tex); glow_tex = NULL; }
    int M = S(48), N = Ts + 2 * M, r = Ts * 12 / 100;
    uint8_t *px = malloc((size_t)N * N * 4);
    if (!px) return;
    float hs = Ts / 2.f, core = S(6), sigma = S(20);
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        float fx = fabsf(x + 0.5f - N / 2.f) - (hs - r), fy = fabsf(y + 0.5f - N / 2.f) - (hs - r);
        float ox = fmaxf(fx, 0.f), oy = fmaxf(fy, 0.f);
        float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(fx, fy), 0.f) - r;     /* signed distance to the rounded tile */
        float a;
        if (d < -1.f) a = 0.f;
        else if (d < 0.f) a = 1.f + d;
        else if (d <= core) a = 1.f;
        else a = expf(-powf((d - core) / sigma, 1.25f));
        uint8_t *p = px + ((size_t)y * N + x) * 4;
        p[0] = p[1] = p[2] = 255; p[3] = (uint8_t)(a * 255);
    }
    glow_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, N, N);
    if (glow_tex) { SDL_UpdateTexture(glow_tex, NULL, px, N * 4); SDL_SetTextureBlendMode(glow_tex, SDL_BLENDMODE_ADD); glow_sz = Ts; glow_n = N; }
    free(px);
}
static void draw_big_glow(int cx, int cy, int Ts, float amt) {
    if (amt <= 0.01f) return;
    ensure_glow(Ts);
    if (!glow_tex) return;
    float f = (1.f + 0.12f * amt) / 1.12f;
    int dw = (int)(glow_n * f);
    SDL_Rect d = { cx - dw / 2, cy - dw / 2, dw, dw };
    int strips = grad_on() ? 8 : 1;                           /* gradient themes: draw the glow in vertical strips, each tinted along the gradient */
    for (int pass = 0; pass < 2; pass++) {
        SDL_SetTextureAlphaMod(glow_tex, (Uint8)((pass ? 100 : 255) * amt));     /* second, weaker pass: bleed onto neighbors */
        for (int i = 0; i < strips; i++) {
            int r, g, b; grad_rgb((i + 0.5f) / strips, &r, &g, &b);
            SDL_SetTextureColorMod(glow_tex, (Uint8)r, (Uint8)g, (Uint8)b);
            SDL_Rect sr = { glow_n * i / strips, 0, glow_n * (i + 1) / strips - glow_n * i / strips, glow_n };
            SDL_Rect dr = { d.x + dw * i / strips, d.y, d.x + dw * (i + 1) / strips - (d.x + dw * i / strips), dw };
            if (strips == 1) SDL_RenderCopy(ren, glow_tex, NULL, &d); else SDL_RenderCopy(ren, glow_tex, &sr, &dr);
        }
    }
}

/* ---------- visualizer (now playing) ---------- */
#define VIS_N 1024
static float vis_lv[48];
static void fft_(float *re, float *im) {
    for (int i = 1, j = 0; i < VIS_N; i++) {
        int bit = VIS_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= VIS_N; len <<= 1) {
        float ang = -2.f * (float)M_PI / len, wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < VIS_N; i += len) {
            float cr = 1.f, ci = 0.f;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float vr = re[b] * cr - im[b] * ci, vi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - vr; im[b] = im[a] - vi; re[a] += vr; im[a] += vi;
                float nc = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nc;
            }
        }
    }
}
static void spectrum(const float *mono, float *out, int nb) {
    static float re[VIS_N], im[VIS_N];
    for (int i = 0; i < VIS_N; i++) { re[i] = mono[i] * (0.5f - 0.5f * cosf(2.f * (float)M_PI * i / (VIS_N - 1))); im[i] = 0.f; }
    fft_(re, im);
    for (int b = 0; b < nb; b++) {
        int i0 = (int)powf(300.f, (float)b / nb), i1 = (int)powf(300.f, (float)(b + 1) / nb);
        if (i1 <= i0) i1 = i0 + 1;
        float m = 0.f;
        for (int i = i0; i < i1 && i < VIS_N / 2; i++) { float mag = sqrtf(re[i] * re[i] + im[i] * im[i]) / (VIS_N / 2.f); if (mag > m) m = mag; }
        float lv = (20.f * log10f(m + 1e-6f) + 65.f) / 65.f;
        lv *= 1.f + 0.5f * b / nb;                                  /* tilt: highs are naturally quieter */
        out[b] = lv < 0.f ? 0.f : lv > 1.f ? 1.f : lv;
    }
}
static void thick_line(int x0, int y0, int x1, int y1, float nx, float ny) {
    for (int o = -1; o <= 1; o++) SDL_RenderDrawLine(ren, x0 + (int)(nx * o), y0 + (int)(ny * o), x1 + (int)(nx * o), y1 + (int)(ny * o));
}
static void draw_visualizer(int cx, int cy, int A) {
    if (!vis_mode) return;
    static int16_t pcm[VIS_N * 2];
    static float mono[VIS_N];
    int got = (sn.paused || !sn.valid) ? 0 : engine_peek(pcm, VIS_N);
    for (int i = 0; i < VIS_N; i++) mono[i] = i < got ? (pcm[2 * i] + pcm[2 * i + 1]) / 65536.f : 0.f;
    int r = (int)gcol[0], g = (int)gcol[1], b = (int)gcol[2];
    if (vis_mode == 2) {                                            /* waveform */
        int t0 = 0;                                                 /* start at a rising zero crossing so the shape holds still */
        for (int i = 1; i < 320; i++) if (mono[i - 1] < 0.f && mono[i] >= 0.f) { t0 = i; break; }
        enum { NP = 220 };
        int px = 0, py = H / 2, amp = H * 30 / 100;
        SDL_SetRenderDrawColor(ren, r, g, b, 210);
        for (int p = 0; p < NP; p++) {
            int s0 = t0 + p * 3;
            float v = (mono[s0] + mono[s0 + 1] + mono[s0 + 2]) / 3.f * 2.f;
            v = v < -1.f ? -1.f : v > 1.f ? 1.f : v;
            int x = p * W / (NP - 1), y = H / 2 - (int)(v * amp);
            if (p) thick_line(px, py, x, y, 0.f, 1.f);
            px = x; py = y;
        }
        return;
    }
    float lv[48];
    spectrum(mono, lv, 48);
    for (int i = 0; i < 48; i++) vis_lv[i] = lv[i] > vis_lv[i] ? lv[i] : vis_lv[i] * 0.88f;   /* fast attack, slow decay */
    if (vis_mode == 1) {                                            /* bars */
        int bw = W / 48;
        for (int i = 0; i < 48; i++) { int h = (int)(vis_lv[i] * H * 0.55f); fill(i * bw + 2, H - h, bw - 4, h, r, g, b, 150); }
    } else {                                                        /* circle */
        float r0 = A * 0.62f, maxl = A * 0.38f;
        SDL_SetRenderDrawColor(ren, r, g, b, 170);
        for (int j = 0; j < 72; j++) {
            int idx = j < 36 ? j : 71 - j;
            float ang = (float)j / 72.f * 6.2831853f - 1.5707963f, c = cosf(ang), sn_ = sinf(ang);
            float l = vis_lv[idx * 47 / 35] * maxl + S(4);
            thick_line(cx + (int)(c * r0), cy + (int)(sn_ * r0), cx + (int)(c * (r0 + l)), cy + (int)(sn_ * (r0 + l)), -sn_, c);
        }
    }
}

/* ---------- playlists (playlists.cfg: a "#Name" line, then one song path per line) ---------- */
typedef struct { char name[64]; char **p; int n, cap; } Playlist;
static Playlist *pls; static int npl, plcap;
static char toast_msg[120]; static Uint32 toast_t;
static void ui_toast(const char *m) { snprintf(toast_msg, sizeof toast_msg, "%s", m); toast_t = SDL_GetTicks() | 1; }
static void pl_save(void) {
    FILE *f = fopen("playlists.cfg", "w"); if (!f) return;
    for (int i = 0; i < npl; i++) { fprintf(f, "#%s\n", pls[i].name); for (int j = 0; j < pls[i].n; j++) fprintf(f, "%s\n", pls[i].p[j]); }
    fclose(f);
}
static int pl_new(const char *name) {                         /* returns the new playlist's index, or -1 */
    if (npl == plcap) { int nc = plcap ? plcap * 2 : 8; Playlist *np = realloc(pls, (size_t)nc * sizeof *np); if (!np) return -1; pls = np; plcap = nc; }
    memset(&pls[npl], 0, sizeof pls[npl]); snprintf(pls[npl].name, sizeof pls[npl].name, "%s", name);
    return npl++;
}
static int pl_add_path(int pi, const char *path) {            /* 1 added, 0 already there, -1 error */
    Playlist *P = &pls[pi];
    for (int j = 0; j < P->n; j++) if (!strcmp(P->p[j], path)) return 0;
    if (P->n == P->cap) { int nc = P->cap ? P->cap * 2 : 16; char **np = realloc(P->p, (size_t)nc * sizeof *np); if (!np) return -1; P->p = np; P->cap = nc; }
    P->p[P->n] = strdup(path); if (!P->p[P->n]) return -1;
    P->n++; return 1;
}
static void pl_remove_at(int pi, int j) { Playlist *P = &pls[pi]; free(P->p[j]); memmove(&P->p[j], &P->p[j + 1], (size_t)(P->n - j - 1) * sizeof *P->p); P->n--; }
static void pl_delete(int pi) {
    for (int j = 0; j < pls[pi].n; j++) free(pls[pi].p[j]);
    free(pls[pi].p);
    memmove(&pls[pi], &pls[pi + 1], (size_t)(npl - pi - 1) * sizeof *pls); npl--;
}
static void pl_load(void) {
    FILE *f = fopen("playlists.cfg", "r"); if (!f) return;
    char line[700]; int cur = -1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        if (line[0] == '#') cur = pl_new(line + 1); else if (cur >= 0) pl_add_path(cur, line);
    }
    fclose(f);
}
/* The open playlist's songs as library indices (songs that are no longer in the library are skipped); plv_pi = index in the playlist file. */
static int *plv_song, *plv_pi; static int plv_n, plv_cap;
static void plv_rebuild(int pi) {
    plv_n = 0;
    if (pi < 0 || pi >= npl) return;
    Playlist *P = &pls[pi];
    if (P->n > plv_cap) {
        int *a = realloc(plv_song, (size_t)P->n * sizeof(int)), *b = realloc(plv_pi, (size_t)P->n * sizeof(int));
        if (a) plv_song = a;
        if (b) plv_pi = b;
        if (!a || !b) return;
        plv_cap = P->n;
    }
    for (int j = 0; j < P->n; j++)
        for (int i = 0; i < g_nsongs; i++) if (!strcmp(g_songs[i].path, P->p[j])) { plv_song[plv_n] = i; plv_pi[plv_n] = j; plv_n++; break; }
}

/* ---------- browse model: tabs + drill-down views ---------- */
enum { VK_ALBUMS, VK_ARTISTS, VK_SONGS, VK_ARTIST_ALBUMS, VK_ALBUM_SONGS, VK_DOWNLOAD, VK_PLAYLISTS, VK_PL_SONGS };
#define NTABS 5                                            /* Albums, Artists, Songs, Playlists, Download */
typedef struct { int kind, param, sel, top; float fy, ox, oy; int has_o; int csel, psel; float camt, pamt; } View;   /* fy = animated scroll row, ox/oy = animated selection */
static View vs[4], saved_root[NTABS];
static void reset_roots(void) {
    saved_root[0] = (View){ VK_ALBUMS, 0, 0, 0 };
    saved_root[1] = (View){ VK_ARTISTS, 0, 0, 0 };
    saved_root[2] = (View){ VK_SONGS, 0, 0, 0 };
    saved_root[3] = (View){ VK_PLAYLISTS, 0, 0, 0 };
    saved_root[4] = (View){ VK_DOWNLOAD, 0, 0, 0 };
}
static int vd, tab;
static int on_dl(void) { return scr == SCR_GRID && vd == 0 && vs[0].kind == VK_DOWNLOAD; }

static int is_grid(int kind) { return kind != VK_SONGS && kind != VK_ALBUM_SONGS && kind != VK_PL_SONGS; }
static int view_n(const View *v) {
    switch (v->kind) {
    case VK_ALBUMS: return g_nalbums;
    case VK_ARTISTS: return g_nartists;
    case VK_SONGS: return g_nsongs;
    case VK_DOWNLOAD: return 0;
    case VK_PLAYLISTS: return npl + 1;                 /* row 0 is "+ New Playlist" */
    case VK_PL_SONGS: return plv_n;
    case VK_ARTIST_ALBUMS: return g_artists[v->param].nalb;
    default: return g_albums[v->param].n;
    }
}
static int album_of(const View *v, int i) { return v->kind == VK_ALBUMS ? i : g_artists[v->param].albums[i]; }
static int song_of(const View *v, int i) { return v->kind == VK_SONGS ? g_sorted[i] : v->kind == VK_PL_SONGS ? plv_song[i] : g_albums[v->param].songs[i]; }
/* Album tile art = first track that actually has art. */
static int album_rep(int a) {
    Album *A = &g_albums[a];
    while (A->rep + 1 < A->n && th_state[A->songs[A->rep]] == 3) A->rep++;
    return A->songs[A->rep];
}
static int item_rep(const View *v, int i) {
    switch (v->kind) {
    case VK_ARTISTS: return album_rep(g_artists[i].albums[0]);
    case VK_ALBUMS: case VK_ARTIST_ALBUMS: return album_rep(album_of(v, i));
    default: return song_of(v, i);
    }
}

static int bat_pct = -1, bat_chg; static Uint32 bat_t;
static void bat_update(void) {
    Uint32 now = SDL_GetTicks();
    if (bat_t && now - bat_t < 20000) return;
    bat_t = now | 1;
    bat_pct = -1;
    glob_t g;
    if (glob("/sys/class/power_supply/*/capacity", 0, NULL, &g) == 0) {
        for (size_t k = 0; k < g.gl_pathc && bat_pct < 0; k++) {
            FILE *f = fopen(g.gl_pathv[k], "r"); int v = -1;
            if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
            if (v < 0 || v > 100) continue;
            bat_pct = v; bat_chg = 0;
            char sp[300]; snprintf(sp, sizeof sp, "%s", g.gl_pathv[k]);
            char *sl = strrchr(sp, '/');
            if (sl) { snprintf(sl, sizeof sp - (size_t)(sl - sp), "/status"); f = fopen(sp, "r");
                      if (f) { char st[24] = ""; if (fgets(st, sizeof st, f)) bat_chg = st[0] == 'C' || st[0] == 'F'; fclose(f); } }
        }
        globfree(&g);
    }
}
static int wifi_on, wifi_link, bt_on; static Uint32 radio_t;
static int sysread(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f); out[strcspn(out, " \r\n")] = 0;
    return 1;
}
/* Wi-Fi / Bluetooth state from rfkill (enabled) and the network interface (connected). Checked every few seconds. */
static void radio_update(void) {
    Uint32 now = SDL_GetTicks();
    if (radio_t && now - radio_t < 4000) return;
    radio_t = now | 1;
    int rk_wifi = 0, rk_bt = 0; glob_t g; char p[300], t[32], st[8];
    if (glob("/sys/class/rfkill/rfkill*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            snprintf(p, sizeof p, "%s/type", g.gl_pathv[i]);  if (!sysread(p, t, sizeof t)) continue;
            snprintf(p, sizeof p, "%s/state", g.gl_pathv[i]); if (!sysread(p, st, sizeof st)) continue;
            if (!strcmp(t, "wlan") && st[0] == '1') rk_wifi = 1;
            if (!strcmp(t, "bluetooth") && st[0] == '1') rk_bt = 1;
        }
        globfree(&g);
    }
    wifi_link = 0; int iface_up = 0;
    if (glob("/sys/class/net/wl*/operstate", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) { if (sysread(g.gl_pathv[i], t, sizeof t)) { if (!strcmp(t, "up")) wifi_link = 1; if (strcmp(t, "down")) iface_up = 1; } }
        globfree(&g);
    }
    wifi_on = rk_wifi || wifi_link || iface_up;
    bt_on = rk_bt || access("/sys/class/bluetooth/hci0", F_OK) == 0;
}
static void icon_wifi(int cx, int cy, int sz, int r, int g, int b) {      /* three arcs and a dot; (cx, cy) is the dot */
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    for (int k = 1; k <= 3; k++) for (int th = -45; th < 45; th += 6) {
        float rad = (float)sz * k / 3.f, a1 = th * (float)M_PI / 180.f, a2 = (th + 6) * (float)M_PI / 180.f;
        for (int w = 0; w < 2; w++) {
            float rr = rad - w;
            SDL_RenderDrawLine(ren, cx + (int)(sinf(a1) * rr), cy - (int)(cosf(a1) * rr), cx + (int)(sinf(a2) * rr), cy - (int)(cosf(a2) * rr));
        }
    }
    circle(cx, cy, S(2) + 1, r, g, b);
}
static void icon_bt(int cx, int cy, int h, int r, int g, int b) {        /* the Bluetooth rune */
    int u = h / 2, hx = u / 2 + 1;
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    thick_line(cx, cy - u, cx, cy + u, 1.f, 0.f);
    thick_line(cx, cy - u, cx + hx, cy - u / 2, 0.f, 1.f); thick_line(cx + hx, cy - u / 2, cx - hx, cy + u / 2, 0.f, 1.f);
    thick_line(cx, cy + u, cx + hx, cy + u / 2, 0.f, 1.f); thick_line(cx + hx, cy + u / 2, cx - hx, cy - u / 2, 0.f, 1.f);
}

static int draw_clock_battery(void) {                       /* returns the x where the clock text starts */
    bat_update();
    time_t t = time(NULL); struct tm tmv; char cb[24];
    localtime_r(&t, &tmv);
    strftime(cb, sizeof cb, clock_12h ? "%I:%M %p" : "%H:%M", &tmv);
    int left = W - S(24);                                   /* battery on the far right, clock just left of it */
    if (bat_pct >= 0) {
        char pb[16]; snprintf(pb, sizeof pb, "%d%%", bat_pct);
        int tw = draw_text(F_S, pb, W - S(24), S(16), 0, 2, GRAY);
        int iw = S(28), ih = S(14), ix = W - S(24) - tw - S(12) - iw, iy = S(23);
        SDL_SetRenderDrawColor(ren, 200, 200, 210, 255);
        SDL_Rect o = { ix, iy, iw, ih }; SDL_RenderDrawRect(ren, &o);
        fill(ix + iw, iy + S(4), S(3), ih - S(8), 200, 200, 210, 255);
        int r = 110, g = 220, b = 140;
        if (bat_chg) { r = (int)gcol[0]; g = (int)gcol[1]; b = (int)gcol[2]; }
        else if (bat_pct <= 15) { r = 255; g = 70; b = 70; }
        fill(ix + 2, iy + 2, (iw - 4) * bat_pct / 100, ih - 4, r, g, b, 255);
        left = ix - S(30);
    }
    int cw = draw_text(F_M, cb, left, S(13), 0, 2, WHITE);
    int ix = left - cw - S(18);                                  /* Bluetooth and Wi-Fi icons just left of the clock */
    radio_update();
    { int c = bt_on ? 240 : 95; icon_bt(ix - S(6), S(30), S(22), c, c, c); ix -= S(26); }          /* white = on, grey = off */
    { int c = wifi_on ? 240 : 95; icon_wifi(ix - S(13), S(40), S(18), c, c, c); ix -= S(34); }
    return ix + S(14);
}

static void draw_header(const View *v, int n) {
    int hr, hg, hb;
    theme_rgb(1, 0, 0, 0, &hr, &hg, &hb);
    fill(0, 0, W, S(60), hr, hg, hb, 255);
    char b[200] = "";
    int tabs_end = S(24);
    if (vd == 0) {
        static const char *tn[NTABS] = { "Albums", "Artists", "Songs", "Playlists", "Download" };
        int x = S(24);
        for (int i = 0; i < NTABS; i++) {
            int w = draw_text(F_L, tn[i], x, S(10), 0, 0, i == tab ? WHITE : DIM);
            if (i == tab) {
                fill(x, S(50), w, S(8), (int)gcol[0], (int)gcol[1], (int)gcol[2], 70);
                fill(x, S(53), w, S(3), (int)gcol[0], (int)gcol[1], (int)gcol[2], 255);
            }
            x += w + S(34);
        }
        tabs_end = x - S(34);
    } else if (v->kind == VK_ALBUM_SONGS) {
        tabs_end = S(24) + draw_text(F_L, g_albums[v->param].name, S(24), S(10), W * 2 / 3, 0, WHITE);
        snprintf(b, sizeof b, "%s  \xC2\xB7  %d tracks", g_albums[v->param].artist, n);
    } else if (v->kind == VK_PL_SONGS) {
        tabs_end = S(24) + draw_text(F_L, pls[v->param].name, S(24), S(10), W * 2 / 3, 0, WHITE);
        snprintf(b, sizeof b, "%d song%s", n, n == 1 ? "" : "s");
    } else {
        tabs_end = S(24) + draw_text(F_L, g_artists[v->param].name, S(24), S(10), W * 2 / 3, 0, WHITE);
        snprintf(b, sizeof b, "%d albums", n);
    }
    int cl = draw_clock_battery();
    int gap = cl - S(24) - (tabs_end + S(24)), xr = cl - S(24);
    if (b[0] && gap > S(70)) draw_text(F_S, b, xr, S(18), gap, 2, GRAY);
    fill(0, S(59), W, 1, (int)gcol[0], (int)gcol[1], (int)gcol[2], 110);
}

static void draw_dl(void);
static void draw_playlists(View *v, int n);
static void draw_browse(void) {
    View *v = &vs[vd];
    int n = view_n(v);
    if (v->kind == VK_DOWNLOAD) { draw_dl(); return; }
    if (v->kind == VK_PLAYLISTS) { draw_playlists(v, n); return; }
    anim = 0;
    int bgr, bgg, bgb;
    theme_rgb(0, 0, 0, 0, &bgr, &bgg, &bgb);
    fill(0, 0, W, H, bgr, bgg, bgb, 255);
    if (n > 0) { if (v->sel >= n) v->sel = n - 1; glow_target(pick_color(th_color(item_rep(v, v->sel)))); }
    glow_step();
    int top = S(60), area_h = H - top - S(40);
    SDL_Rect area = { 0, top, W, area_h };
    SDL_RenderSetClipRect(ren, &area);
    if (is_grid(v->kind)) {
        SDL_RenderSetClipRect(ren, NULL);              /* let the glow bleed; header/footer are drawn after */
        int sr = v->sel / g_cols;
        if (sr < v->top) v->top = sr;
        if (sr >= v->top + g_vis) v->top = sr - g_vis + 1;
        float dfy = (float)v->top - v->fy;
        if (fabsf(dfy) > 0.01f) { v->fy += dfy * 0.35f; anim = 1; } else v->fy = (float)v->top;
        /* selection "pop": the new tile grows/raises while the old one settles back; art never moves between tiles */
        if (!v->has_o) { v->csel = v->sel; v->camt = 1.f; v->psel = -1; v->pamt = 0.f; v->has_o = 1; }
        if (v->sel != v->csel) { v->psel = v->csel; v->pamt = v->camt; v->csel = v->sel; v->camt = 0.f; }
        if (v->camt < 1.f) { v->camt += (1.f - v->camt) * 0.4f; if (v->camt > 0.97f) v->camt = 1.f; anim = 1; }
        if (v->psel >= 0) { v->pamt -= v->pamt * 0.4f; if (v->pamt < 0.03f) { v->pamt = 0.f; v->psel = -1; } else anim = 1; }
        int raise = S(12), nr = 0;
        struct { int x, y, rep, sel; float amt; char l1[160], l2[160]; } rz[2];
        int rlo = (int)floorf(fminf(v->fy, (float)v->top)), rhi = (int)ceilf(fmaxf(v->fy, (float)v->top)) + g_vis;
        if (rlo < 0) rlo = 0;
        for (int r = rlo; r <= rhi; r++) for (int c = 0; c < g_cols; c++) {
            int i = r * g_cols + c;
            if (i >= n) break;
            int x = g_x0 + c * (g_T + g_gx), y = top + S(40) + (int)((r - v->fy) * g_rowh);
            if (y > H || y + g_rowh < 0) continue;
            int rep = item_rep(v, i);
            request_thumb(rep, g_T);
            const char *l1, *l2; char sub[48];
            if (v->kind == VK_ARTISTS) { l1 = g_artists[i].name; snprintf(sub, sizeof sub, "%d album%s", g_artists[i].nalb, g_artists[i].nalb == 1 ? "" : "s"); l2 = sub; }
            else { int a = album_of(v, i); l1 = g_albums[a].name; l2 = g_albums[a].artist; }
            float amt = i == v->sel ? v->camt : (i == v->psel ? v->pamt : 0.f);
            if (amt >= 0.001f && nr < 2) {             /* drawn later, on top */
                rz[nr].x = x; rz[nr].y = y; rz[nr].rep = rep; rz[nr].sel = (i == v->sel); rz[nr].amt = amt;
                snprintf(rz[nr].l1, sizeof rz[nr].l1, "%s", l1); snprintf(rz[nr].l2, sizeof rz[nr].l2, "%s", l2);
                nr++; continue;
            }
            if (th_state[rep] == 2) { th_used[rep] = frame_no; SDL_Rect d = { x, y, g_T, g_T }; SDL_RenderCopy(ren, th_tex[rep], NULL, &d); }
            else draw_placeholder(x, y, g_T, 0);
            draw_text(F_M, l1, x, y + g_T + S(6), g_T, 0, GRAY);
            draw_text(F_S, l2, x, y + g_T + S(38), g_T, 0, DIM);
        }
        if (nr == 2 && rz[0].sel) { __typeof__(rz[0]) t = rz[0]; rz[0] = rz[1]; rz[1] = t; }     /* selected tile on top */
        for (int k = 0; k < nr; k++) {
            float am = rz[k].amt;
            int cx = rz[k].x + g_T / 2, cy = rz[k].y + g_T / 2 - (int)(raise * am);
            int Tsi = (int)(g_T * (1.f + 0.12f * am) + 0.5f);
            draw_big_glow(cx, cy, g_T * 112 / 100, am);
            SDL_Rect d = { cx - Tsi / 2, cy - Tsi / 2, Tsi, Tsi };
            if (th_state[rz[k].rep] == 2) { th_used[rz[k].rep] = frame_no; SDL_RenderCopy(ren, th_tex[rz[k].rep], NULL, &d); }
            else draw_placeholder(d.x, d.y, Tsi, 0);
            draw_text(F_M, rz[k].l1, rz[k].x, rz[k].y + g_T + S(6), g_T, 0, rz[k].sel ? WHITE : GRAY);
            draw_text(F_S, rz[k].l2, rz[k].x, rz[k].y + g_T + S(38), g_T, 0, rz[k].sel ? GRAY : DIM);
        }
    } else {
        int sr = v->sel;
        if (sr < v->top) v->top = sr;
        if (sr >= v->top + g_lvis) v->top = sr - g_lvis + 1;
        float dfy = (float)v->top - v->fy;
        if (fabsf(dfy) > 0.01f) { v->fy += dfy * 0.35f; anim = 1; } else v->fy = (float)v->top;
        int th = g_lrow - S(12), x = S(28), w = W - S(56);
        int ilo = (int)floorf(fminf(v->fy, (float)v->top)), ihi = (int)ceilf(fmaxf(v->fy, (float)v->top)) + g_lvis;
        if (ilo < 0) ilo = 0;
        for (int i = ilo; i <= ihi && i < n; i++) {
            int song = song_of(v, i), isel = (i == v->sel);
            int y = top + (int)((i - v->fy) * g_lrow) + S(6);
            request_thumb(song, g_T);
            int tx = x + (v->kind == VK_ALBUM_SONGS ? S(44) : 0);
            if (v->kind == VK_ALBUM_SONGS) { char nb[16]; snprintf(nb, sizeof nb, "%d", i + 1); draw_text(F_M, nb, x + S(30), y + th / 2 - S(14), 0, 2, DIM); tx += S(8); }
            if (th_state[song] == 2) { th_used[song] = frame_no; SDL_Rect d = { tx, y, th, th }; SDL_RenderCopy(ren, th_tex[song], NULL, &d); }
            else draw_placeholder(tx, y, th, 0);
            char sub[300];
            snprintf(sub, sizeof sub, "%s%s%s", g_songs[song].artist, (g_songs[song].artist[0] && g_songs[song].album[0]) ? "  \xC2\xB7  " : "", g_songs[song].album);
            draw_text(F_L, g_songs[song].title, tx + th + S(18), y + S(4), w - th - S(80), 0, isel ? WHITE : GRAY);
            draw_text(F_M, sub, tx + th + S(18), y + S(54), w - th - S(80), 0, DIM);
        }
        float ty = (float)(top + (int)((v->sel - v->fy) * g_lrow) + S(6));
        if (!v->has_o) { v->oy = ty; v->has_o = 1; }
        float dd = ty - v->oy;
        if (fabsf(dd) > 0.5f) { v->oy += dd * 0.4f; anim = 1; } else v->oy = ty;
        glow_rect(x - S(8), (int)v->oy - S(3), w + S(16), th + S(6));
    }
    SDL_RenderSetClipRect(ren, NULL);
    if (n == 0 && v->kind == VK_PL_SONGS) {
        draw_text(F_M, "This playlist is empty.", W / 2, H / 2 - S(40), W - S(100), 1, GRAY);
        draw_text(F_S, "Open a song's Now Playing screen, press MENU and choose Add to playlist.", W / 2, H / 2, W - S(100), 1, DIM);
    }
    draw_header(v, n);
    int br, bg_, bb;
    theme_rgb(1, 0, 0, 0, &br, &bg_, &bb);
    fill(0, H - S(40), W, S(40), br, bg_, bb, 255);
    fill(0, H - S(40), W, 1, (int)gcol[0], (int)gcol[1], (int)gcol[2], 110);
    draw_text(F_S, v->kind == VK_PL_SONGS
        ? "A Play   B Back   START Shuffle playlist   X Remove song   Y Now playing   SELECT Screen off   MENU Options"
        : vd == 0
        ? "A Open/Play   L1/R1 Tabs   START Shuffle all   X Shuffle on/off   Y Now playing   SELECT Screen off   MENU Options"
        : "A Open/Play   B Back   START Shuffle all   X Shuffle on/off   Y Now playing   SELECT Screen off   MENU Options",
        W / 2, H - S(30), W - S(20), 1, DIM);
}

static int now_art_size(void) {
    int a = H - S(24) - S(190);
    if (a > W * 11 / 20) a = W * 11 / 20;
    return a;
}

static void update_now_art(int song) {
    if (song == now_wanted) return;
    if (now_tex) { SDL_DestroyTexture(now_tex); now_tex = NULL; }
    if (blur_tex) { SDL_DestroyTexture(blur_tex); blur_tex = NULL; }
    now_wanted = song; now_state = 0;
    if (song >= 0) { now_state = 1; request_now(song, now_art_size()); }
}

static void icon_halo(int x0, int y0, int w, int h, int r, int g, int b) {
    for (int k = 3; k >= 1; k--) fill_round(x0 - S(5 * k), y0 - S(5 * k), w + S(10 * k), h + S(10 * k), S(8 + 4 * k), r, g, b, 22);
}
static void icon_shuffle(int cx, int cy, int on) {
    int r = on ? (int)gcol[0] : 120, g = on ? (int)gcol[1] : 120, b = on ? (int)gcol[2] : 132;
    int w = S(40), h = S(26), x0 = cx - w / 2, y0 = cy - h / 2;
    int ax = x0 + w * 28 / 100, bx2 = x0 + w * 62 / 100, xe = x0 + w - S(10);
    if (on) icon_halo(x0, y0, w, h, r, g, b);
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    thick_line(x0, y0 + h, ax, y0 + h, 0.f, 1.f); thick_line(ax, y0 + h, bx2, y0, 0.f, 1.f); thick_line(bx2, y0, xe, y0, 0.f, 1.f);
    thick_line(x0, y0, ax, y0, 0.f, 1.f); thick_line(ax, y0, bx2, y0 + h, 0.f, 1.f); thick_line(bx2, y0 + h, xe, y0 + h, 0.f, 1.f);
    tri(xe, y0, S(12), 1, r, g, b);
    tri(xe, y0 + h, S(12), 1, r, g, b);
}
static void icon_repeat(int cx, int cy, int mode) {
    int on = mode != 0;
    int r = on ? (int)gcol[0] : 120, g = on ? (int)gcol[1] : 120, b = on ? (int)gcol[2] : 132;
    int w = S(40), h = S(26), x0 = cx - w / 2, y0 = cy - h / 2, c4 = S(4), c8 = S(8);
    if (on) icon_halo(x0, y0, w, h, r, g, b);
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    thick_line(x0 + c4, y0, x0 + w - c8, y0, 0.f, 1.f);
    thick_line(x0 + w, y0 + c4, x0 + w, y0 + h - c4, 1.f, 0.f);
    thick_line(x0 + c8, y0 + h, x0 + w - c4, y0 + h, 0.f, 1.f);
    thick_line(x0, y0 + c4, x0, y0 + h - c4, 1.f, 0.f);
    thick_line(x0 + w - c8, y0, x0 + w, y0 + c4, 0.f, 1.f);
    thick_line(x0 + w, y0 + h - c4, x0 + w - c4, y0 + h, 0.f, 1.f);
    thick_line(x0 + c8, y0 + h, x0, y0 + h - c4, 0.f, 1.f);
    thick_line(x0, y0 + c4, x0 + c4, y0, 0.f, 1.f);
    tri(x0 + w - c8, y0, S(12), 1, r, g, b);
    tri(x0 + c8, y0 + h, S(12), -1, r, g, b);
    if (mode == 2) draw_text(F_S, "1", cx, cy - S(13), 0, 1, (SDL_Color){ (Uint8)r, (Uint8)g, (Uint8)b, 255 });
}

static void draw_now(void) {
    int song = sn.valid ? sn.song : pending_song;
    update_now_art(song);
    { int r0, g0, b0; theme_rgb(0, 14, 14, 18, &r0, &g0, &b0); fill(0, 0, W, H, r0, g0, b0, 255); }
    if (blur_tex) {
        int side = W > H ? W : H;
        SDL_Rect d = { (W - side) / 2, (H - side) / 2, side, side };
        SDL_RenderCopy(ren, blur_tex, NULL, &d);
    }
    if (theme_cur > 0) fill(0, 0, W, H, (int)gcol[0] * 45 / 100, (int)gcol[1] * 45 / 100, (int)gcol[2] * 45 / 100, TINT_ALPHA);   /* other themes: veil in a darkened copy of the current glow color */
    else { int tr, tg, tb; theme_rgb(0, 0, 0, 0, &tr, &tg, &tb); fill(0, 0, W, H, tr, tg, tb, 120); }          /* default theme: plain dark veil */
    if (song < 0) { draw_text(F_L, "Loading...", W / 2, H / 2, 0, 1, WHITE); return; }

    anim = 0;
    glow_target(pick_color(now_state == 2 ? now_col : th_color(song)));
    glow_step();
    int top = S(24), A = now_art_size(), ax = S(40);

    draw_visualizer(ax + A / 2, top + A / 2, A);       /* over the blur, behind the art */

    fill(ax + S(6), top + S(8), A, A, 0, 0, 0, 90);
    if (now_tex) { SDL_Rect d = { ax, top, A, A }; SDL_RenderCopy(ren, now_tex, NULL, &d); }
    else if (th_state[song] == 2) { SDL_Rect d = { ax, top, A, A }; SDL_RenderCopy(ren, th_tex[song], NULL, &d); }
    else draw_placeholder(ax, top, A, 1);
    glow_rect(ax - 2, top - 2, A + 4, A + 4);          /* thin glow, square corners */

    int tx = ax + A + S(40), tw = W - tx - S(36), ty = top + S(40);
    int h = draw_text_wrap(F_L, g_songs[song].title, tx, ty, tw, S(150), WHITE);
    const char *ar = g_songs[song].artist[0] ? g_songs[song].artist : "Unknown artist";
    draw_text(F_M, ar, tx, ty + h + S(12), tw, 0, GRAY);
    if (g_songs[song].album[0]) draw_text(F_S, g_songs[song].album, tx, ty + h + S(52), tw, 0, DIM);

    int bw = S(640), bx = (W - bw) / 2, by = H - S(150);               /* player: bottom center */
    fill(bx, by, bw, S(6), 255, 255, 255, 60);
    double frac = sn.dur > 0 ? sn.elapsed / sn.dur : 0; if (frac > 1) frac = 1;
    { int pw = (int)(bw * frac), segs = grad_on() ? 32 : 1;                 /* progress bar (gradient across the full width on gradient themes) */
      for (int i = 0; i < segs; i++) {
          int x0 = bx + bw * i / segs, x1 = bx + bw * (i + 1) / segs, r, g, b;
          if (x0 >= bx + pw) break;
          if (x1 > bx + pw) x1 = bx + pw;
          grad_rgb((i + 0.5f) / segs, &r, &g, &b); fill(x0, by, x1 - x0, S(6), r, g, b, 255);
      } }
    char t1[16], t2[16];
    fmt_time(sn.elapsed, t1); fmt_time(sn.dur, t2);
    draw_text(F_S, t1, bx, by + S(12), 0, 0, GRAY);
    if (sn.dur > 0) draw_text(F_S, t2, bx + bw, by + S(12), 0, 2, GRAY);

    int cy = H - S(86), cx = W / 2, R = S(30);
    tri(cx - S(118), cy, S(34), -1, 255, 255, 255);
    fill(cx - S(118) - S(24), cy - S(17), S(5), S(34), 255, 255, 255, 255);
    circle(cx, cy, R, 255, 255, 255);
    if (sn.paused) tri(cx, cy, S(32), 1, 20, 20, 24);
    else { fill(cx - S(11), cy - S(14), S(7), S(28), 20, 20, 24, 255); fill(cx + S(4), cy - S(14), S(7), S(28), 20, 20, 24, 255); }
    tri(cx + S(118), cy, S(34), 1, 255, 255, 255);
    fill(cx + S(118) + S(19), cy - S(17), S(5), S(34), 255, 255, 255, 255);

    icon_shuffle(bx + S(30), cy, sn.shuffle);
    icon_repeat(bx + bw - S(30), cy, sn.repeat);
    static const char *vn[] = { "Off", "Bars", "Wave", "Circle" };
    char hint[200];
    snprintf(hint, sizeof hint, "A Pause   L1/R1 Track   Left/Right Seek   L2/R2 Visualizer: %s   X Shuffle   Y Repeat   START Album   B Back", vn[vis_mode]);
    draw_text(F_S, hint, W / 2, H - S(26), W - S(20), 1, DIM);
}

/* ---------- shared pieces: footer, title header, on-screen keyboard ---------- */
static void draw_footer(const char *hint) {
    int br, bg_, bb; theme_rgb(1, 0, 0, 0, &br, &bg_, &bb);
    fill(0, H - S(40), W, S(40), br, bg_, bb, 255);
    fill(0, H - S(40), W, 1, (int)gcol[0], (int)gcol[1], (int)gcol[2], 110);
    draw_text(F_S, hint, W / 2, H - S(30), W - S(20), 1, DIM);
}
static void draw_toast(void) {
    if (!toast_msg[0]) return;
    Uint32 now = SDL_GetTicks();
    if (now - toast_t > 2600) { toast_msg[0] = 0; return; }
    int w = chain_width(F_M, toast_msg) + S(48); if (w > W - S(60)) w = W - S(60);
    int h = S(52), x = (W - w) / 2, y = H - S(110);
    fill_round(x, y, w, h, S(14), 24, 24, 30, 235);
    draw_text(F_M, toast_msg, W / 2, y + (h - S(34)) / 2, w - S(30), 1, ACCENT);
}
static void draw_playlists(View *v, int n) {
    int bgr, bgg, bgb; theme_rgb(0, 0, 0, 0, &bgr, &bgg, &bgb);
    anim = 0; glow_target(pick_color(0x00DCFF)); glow_step();
    fill(0, 0, W, H, bgr, bgg, bgb, 255);
    int y0 = S(60), rowh = S(78), x = S(28), w = W - S(56), vis = (H - y0 - S(40) - S(10)) / rowh;
    if (v->sel >= n) v->sel = n - 1;
    if (v->sel < 0) v->sel = 0;
    if (v->sel < v->top) v->top = v->sel;
    if (v->sel >= v->top + vis) v->top = v->sel - vis + 1;
    for (int i = v->top; i < n && i < v->top + vis; i++) {
        int y = y0 + S(8) + (i - v->top) * rowh, sel = (i == v->sel);
        if (i == 0) draw_text(F_L, "+  New Playlist", x + S(20), y + S(18), w - S(40), 0, sel ? WHITE : ACCENT);
        else {
            char sub[48]; snprintf(sub, sizeof sub, "%d song%s", pls[i - 1].n, pls[i - 1].n == 1 ? "" : "s");
            draw_text(F_L, pls[i - 1].name, x + S(20), y + S(6), w - S(40), 0, sel ? WHITE : GRAY);
            draw_text(F_S, sub, x + S(20), y + S(42), w - S(40), 0, DIM);
        }
        if (sel) glow_rect(x - S(8), y, w + S(16), rowh - S(10));
    }
    draw_footer(v->sel == 0 ? "A Create   L1/R1 Tabs   START Shuffle all   Y Now playing   SELECT Screen off   MENU Options"
                            : "A Open   X Delete   L1/R1 Tabs   START Shuffle all   Y Now playing   SELECT Screen off   MENU Options");
    draw_header(v, n);
}
static void draw_title_header(const char *title, const char *right) {
    int hr, hg, hb; theme_rgb(1, 0, 0, 0, &hr, &hg, &hb);
    fill(0, 0, W, S(60), hr, hg, hb, 255);
    int tw = draw_text(F_L, title, S(24), S(10), W / 2, 0, WHITE);
    int cl = draw_clock_battery();                                   /* text goes in the gap between the title and the clock */
    int gap = cl - S(24) - (S(24) + tw + S(24));
    if (right && right[0] && gap > S(70)) draw_text(F_S, right, cl - S(24), S(18), gap, 2, GRAY);
    fill(0, S(59), W, 1, (int)gcol[0], (int)gcol[1], (int)gcol[2], 110);
}

typedef struct { char text[200]; int layer, x, y, cur; } Kbd;       /* cur = byte offset of the typing cursor */
static const char *KB[3][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._/" },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._/" },
    { "!@#$%^&*()", "[]{}<>|\\~`", ":;'\"?=+,.-", "/_:&%#@$()" } };
enum { KR_NONE, KR_CHANGED, KR_SUBMIT, KR_CANCEL, KR_LEFT, KR_RIGHT };

static void key_rgb(int *r, int *g, int *b) {
    int br, bg_, bb; theme_rgb(1, 0, 0, 0, &br, &bg_, &bb);
    *r = br + 30 > 255 ? 255 : br + 30; *g = bg_ + 30 > 255 ? 255 : bg_ + 30; *b = bb + 38 > 255 ? 255 : bb + 38;
}
static void kbd_box(const Kbd *k, int x, int y, int w, int h, int focused, const char *placeholder) {
    int r, g, b; key_rgb(&r, &g, &b);
    fill_round(x, y, w, h, S(10), r, g, b, 255);
    int tx = x + S(14), ty = y + (h - S(40)) / 2, maxw = w - S(28);
    int len = (int)strlen(k->text), cur = k->cur < 0 ? 0 : k->cur > len ? len : k->cur;
    if (!len) {
        if (!focused) draw_text(F_L, placeholder, tx, ty, maxw, 0, DIM);
        else { draw_text(F_L, placeholder, tx + S(6), ty, maxw - S(6), 0, DIM); fill(tx, ty + S(3), S(3), S(36), 255, 255, 255, 255); }
        return;
    }
    int st = 0; char pre[220];                       /* long text scrolls so the cursor stays inside the box */
    for (;;) {
        int n = cur - st; memcpy(pre, k->text + st, (size_t)n); pre[n] = 0;
        if (st >= cur || chain_width(F_L, pre) <= maxw - S(30)) break;
        st++; while (st < len && ((unsigned char)k->text[st] & 0xC0) == 0x80) st++;
    }
    draw_text(F_L, k->text + st, tx, ty, maxw, 0, WHITE);
    if (focused) fill(tx + chain_width(F_L, pre), ty + S(3), S(3), S(36), 255, 255, 255, 255);
}
static void kbd_draw(const Kbd *k, int x0, int y0, int kw, int kh, int focused, const char *a3, const char *a4) {
    int r, g, b; key_rgb(&r, &g, &b);
    int f = kw >= S(70) ? F_L : F_M, rad = kw / 6;
    for (int ry = 0; ry < 4; ry++) for (int cx = 0; cx < 10; cx++) {
        int x = x0 + cx * kw, y = y0 + ry * kh, sel = focused && k->y == ry && k->x == cx;
        fill_round(x + 2, y + 2, kw - 4, kh - 4, rad, r, g, b, 255);
        char ch[2] = { KB[k->layer][ry][cx], 0 };
        draw_text(f, ch, x + kw / 2, y + kh / 2 - (f == F_L ? S(20) : S(16)), 0, 1, sel ? WHITE : GRAY);
        if (sel) glow_rect(x + 2, y + 2, kw - 4, kh - 4);
    }
    static const char *lname[3] = { "abc", "ABC", "#+=" };
    char l0[24]; snprintf(l0, sizeof l0, "Layer: %s", lname[k->layer]);
    const char *lab[5] = { l0, "Space", "Delete", a3, a4 };
    int aw = 2 * kw, ay = y0 + 4 * kh + S(6), af = kw >= S(70) ? F_M : F_S;
    for (int i = 0; i < 5; i++) {
        int x = x0 + i * aw, sel = focused && k->y == 4 && k->x == i;
        fill_round(x + 2, ay + 2, aw - 4, kh - 4, rad, r, g, b, 255);
        draw_text(af, lab[i], x + aw / 2, ay + kh / 2 - (af == F_M ? S(16) : S(13)), aw - S(8), 1, sel ? WHITE : GRAY);
        if (sel) glow_rect(x + 2, ay + 2, aw - 4, kh - 4);
    }
}
/* edges: 1 = moving off the left/right edge reports KR_LEFT/KR_RIGHT (so a neighbor can take focus), 0 = wrap */
static void kbd_insert(Kbd *k, const char *str) {          /* inserts at the cursor */
    size_t len = strlen(k->text), n = strlen(str);
    if (len + n >= sizeof k->text - 1) return;
    memmove(k->text + k->cur + n, k->text + k->cur, len - (size_t)k->cur + 1);
    memcpy(k->text + k->cur, str, n); k->cur += (int)n;
}
static void kbd_backspace(Kbd *k) {                    /* removes the whole character before the cursor (Japanese etc. is several bytes) */
    int c = k->cur; if (c <= 0) return;
    int p = c - 1; while (p > 0 && ((unsigned char)k->text[p] & 0xC0) == 0x80) p--;
    memmove(k->text + p, k->text + c, strlen(k->text) - (size_t)c + 1); k->cur = p;
}
static void kbd_move(Kbd *k, int d) {
    int len = (int)strlen(k->text);
    if (d < 0) { if (k->cur > 0) { int p = k->cur - 1; while (p > 0 && ((unsigned char)k->text[p] & 0xC0) == 0x80) p--; k->cur = p; } }
    else if (k->cur < len) { int p = k->cur + 1; while (p < len && ((unsigned char)k->text[p] & 0xC0) == 0x80) p++; k->cur = p; }
}
static int kbd_press(Kbd *k, int bt, int edges) {
    int len = (int)strlen(k->text);
    if (k->cur > len) k->cur = len;
    if (k->cur < 0) k->cur = 0;
    int rowlen = k->y == 4 ? 5 : 10;
    switch (bt) {
    case BT_LEFT:  if (k->x > 0) k->x--; else if (edges) return KR_LEFT; else k->x = rowlen - 1; break;
    case BT_RIGHT: if (k->x < rowlen - 1) k->x++; else if (edges) return KR_RIGHT; else k->x = 0; break;
    case BT_UP:    if (k->y == 0) { k->y = 4; k->x /= 2; } else if (k->y == 4) { k->y = 3; k->x *= 2; } else k->y--; break;
    case BT_DOWN:  if (k->y == 3) { k->y = 4; k->x /= 2; } else if (k->y == 4) { k->y = 0; k->x *= 2; } else k->y++; break;
    case BT_L1:    kbd_move(k, -1); break;                      /* move the typing cursor */
    case BT_R1:    kbd_move(k, 1); break;
    case BT_Y:     k->layer = (k->layer + 1) % 3; break;
    case BT_START: return KR_SUBMIT;
    case BT_L2: case BT_R2: return KR_CANCEL;                 /* quick exit from any keyboard */
    case BT_B:     if (!len) return KR_CANCEL; if (k->cur > 0) { kbd_backspace(k); return KR_CHANGED; } break;
    case BT_X:     kbd_insert(k, " "); return KR_CHANGED;
    case BT_A:
        if (k->y < 4) { char ch[2] = { KB[k->layer][k->y][k->x], 0 }; kbd_insert(k, ch); return KR_CHANGED; }
        else if (k->x == 0) k->layer = (k->layer + 1) % 3;
        else if (k->x == 1) { kbd_insert(k, " "); return KR_CHANGED; }
        else if (k->x == 2) { if (k->cur > 0) { kbd_backspace(k); return KR_CHANGED; } }
        else if (k->x == 3) return KR_SUBMIT;
        else return KR_CANCEL;
        break;
    }
    return KR_NONE;
}

/* ---------- playlist name screen (SCR_PLNAME) ---------- */
static Kbd plkb; static int plname_song = -1, plname_ret = SCR_GRID;
static void plname_open(int song, int ret) { memset(&plkb, 0, sizeof plkb); plname_song = song; plname_ret = ret; scr = SCR_PLNAME; }
static void draw_footer(const char *hint);
static void draw_plname(void) {
    int bgr, bgg, bgb; theme_rgb(0, 0, 0, 0, &bgr, &bgg, &bgb);
    anim = 0; glow_target(pick_color(0x00DCFF)); glow_step();
    fill(0, 0, W, H, bgr, bgg, bgb, 255);
    int top = S(60), kw = S(86), kh = S(74), x0 = (W - 10 * kw) / 2;
    kbd_box(&plkb, x0, top + S(24), 10 * kw, S(60), 1, "Playlist name");
    kbd_draw(&plkb, x0, top + S(110), kw, kh, 1, "Create", "Cancel");
    draw_footer("A Type   B Delete   X Space   Y Layer   L1/R1 Cursor   L2 Exit   START Create");
    draw_title_header("New playlist", plname_song >= 0 ? "the current song is added" : "");
}
static void plname_press(int bt) {
    int r = kbd_press(&plkb, bt, 0);
    if (r == KR_CANCEL) { scr = plname_ret; return; }
    if (r != KR_SUBMIT) return;
    char name[64]; const char *a = plkb.text; while (*a == ' ') a++;
    snprintf(name, sizeof name, "%s", a);
    size_t n = strlen(name);
    if (n > 0) {                                                       /* a 63-byte cut may have split the last character: drop it */
        size_t q = n - 1; while (q > 0 && ((unsigned char)name[q] & 0xC0) == 0x80) q--;
        unsigned char lead = (unsigned char)name[q]; size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (q + need > n) { name[q] = 0; n = q; }
    }
    while (n > 0 && name[n - 1] == ' ') name[--n] = 0;
    if (!name[0]) { ui_toast("Type a name first"); return; }
    int pi = pl_new(name);
    if (pi < 0) { ui_toast("Couldn't create the playlist"); scr = plname_ret; return; }
    if (plname_song >= 0) pl_add_path(pi, g_songs[plname_song].path);
    pl_save();
    char m[160]; snprintf(m, sizeof m, plname_song >= 0 ? "Created \"%s\" and added the song" : "Created \"%s\"", name); ui_toast(m);
    scr = plname_ret;
    if (plname_ret == SCR_GRID && vd == 0 && vs[0].kind == VK_PLAYLISTS) vs[0].sel = pi + 1;
}

/* ---------- download tab (yt-dlp) ---------- */
enum { DS_HOME, DS_KEYS, DS_SEARCH, DS_QUEUE };            /* DS_KEYS = type a URL; DS_SEARCH = keyboard + results */
static int ds = DS_HOME, ds_sel, ds_top, dfocus, dsel, drow, dprev_state;
static Kbd dlkb, urlkb;
static char dl_flash[120]; static Uint32 dl_flash_t;
static DlResult dres[DL_MAXRES]; static int dres_n, dres_state;
static DlJob djob[DL_MAXJOB]; static int djob_n;
static const char *DS_ITEMS[] = { "Search YouTube Music", "Enter URL or video ID", "Import links.txt from Music/Downloads", "Download queue", "Update downloader (yt-dlp)" };
#define DS_NITEMS 5

static void dl_flash_msg(const char *m) { snprintf(dl_flash, sizeof dl_flash, "%s", m); dl_flash_t = SDL_GetTicks(); }

static void draw_dl(void) {
    int bgr, bgg, bgb; theme_rgb(0, 0, 0, 0, &bgr, &bgg, &bgb);
    anim = 0; glow_target(pick_color(0x00DCFF)); glow_step();
    fill(0, 0, W, H, bgr, bgg, bgb, 255);
    dres_n = dl_get_results(dres, DL_MAXRES, &dres_state);
    djob_n = dl_get_jobs(djob, DL_MAXJOB);
    if (dl_flash[0] && SDL_GetTicks() - dl_flash_t > 3500) dl_flash[0] = 0;
    int gr = (int)gcol[0], gg = (int)gcol[1], gb = (int)gcol[2];
    int top = S(60), rowh = S(78), x = S(28), w = W - S(56), vis = (H - top - S(40) - S(10)) / rowh;
    const char *title = "", *hint = ""; char rt[64] = "";
    if (ds == DS_HOME) {
        int act = 0; for (int i = 0; i < djob_n; i++) if (djob[i].state < 2) act++;
        for (int i = 0; i < DS_NITEMS; i++) {
            int y = top + S(8) + i * rowh; char lb[96];
            if (i == 3) snprintf(lb, sizeof lb, "Download queue  (%d active, %d total)", act, djob_n); else snprintf(lb, sizeof lb, "%s", DS_ITEMS[i]);
            draw_text(F_L, lb, x + S(20), y + S(16), w - S(40), 0, i == ds_sel ? WHITE : GRAY);
            if (i == ds_sel) glow_rect(x - S(8), y, w + S(16), rowh - S(10));
        }
        char us[200]; int ust = 0; dl_update_status(us, sizeof us, &ust);
        int yb = top + S(8) + DS_NITEMS * rowh + S(6);
        draw_text(F_S, "MP3 320 kbps with tags and cover art \xE2\x86\x92 Music/Downloads.  Wi-Fi must be on.", x + S(20), yb, w - S(40), 0, DIM);
        if (ust) draw_text(F_S, us, x + S(20), yb + S(30), w - S(40), 0, GRAY);
        if (!dl_available()) draw_text(F_S, "Downloader files missing: bin/yt-dlp, bin/ffmpeg, bin/qjs (rebuild the package)", x + S(20), yb + S(60), w - S(40), 0, (SDL_Color){ 255, 110, 110, 255 });
        hint = "A Select   L1/R1 Tabs   MENU Options";
    } else if (ds == DS_KEYS) {
        title = "Enter URL or video ID";
        int kw = S(86), kh = S(74), x0 = (W - 10 * kw) / 2;
        kbd_box(&urlkb, x0, top + S(24), 10 * kw, S(60), 1, "https://music.youtube.com/watch?v=...");
        kbd_draw(&urlkb, x0, top + S(110), kw, kh, 1, "OK", "Back");
        hint = "A Type   B Delete   X Space   Y Layer   L1/R1 Cursor   L2 Exit   START OK";
    } else if (ds == DS_SEARCH) {
        title = "Search YouTube Music";
        if (dres_n) snprintf(rt, sizeof rt, "%d results", dres_n);
        if (dprev_state == 1 && dres_state == 2 && dres_n) { dfocus = 1; dsel = 0; drow = 0; }   /* results just arrived */
        dprev_state = dres_state;
        int lw = W / 2, bot = H - S(40);
        kbd_box(&dlkb, S(16), top + S(14), lw - S(32), S(56), dfocus == 0, "Song or artist");
        int kw = (lw - S(32)) / 10, kh = S(78);
        kbd_draw(&dlkb, S(16), top + S(86), kw, kh, dfocus == 0, "Search", "Back");
        fill(lw, top + S(10), 1, bot - top - S(20), gr, gg, gb, 70);
        int gx0 = lw + S(10), gw = W - gx0 - S(16), gtop = top + S(10), rh = S(72), vr = (bot - gtop) / rh;
        int cx0 = gx0 + gw / 2;
        if (dres_state == 1) draw_text(F_M, "Searching...", cx0, H / 2 - S(20), 0, 1, WHITE);
        else if (dres_state == 3 && !dres_n) { draw_text(F_M, "No results", cx0, H / 2 - S(40), 0, 1, WHITE); draw_text(F_S, "Check Wi-Fi, or run \"Update downloader\"", cx0, H / 2, gw - S(20), 1, GRAY); }
        else if (!dres_n) draw_text(F_S, "Type a song or artist, then press START", cx0, H / 2 - S(10), gw - S(20), 1, DIM);
        else {
            if (dsel >= dres_n) dsel = dres_n - 1;
            if (dsel < drow) drow = dsel;
            if (dsel >= drow + vr) drow = dsel - vr + 1;
            for (int i = drow; i < dres_n && i < drow + vr; i++) {
                int y = gtop + (i - drow) * rh, sel = (i == dsel && dfocus == 1);
                const char *chn = dres[i].channel[0] ? dres[i].channel : dres[i].artist;
                char dur[16] = ""; if (dres[i].dur > 0) snprintf(dur, sizeof dur, "%d:%02d", dres[i].dur / 60, dres[i].dur % 60);
                if (dur[0]) draw_text(F_S, dur, gx0 + gw - S(14), y + S(12), 0, 2, sel ? GRAY : DIM);
                draw_text(F_M, dres[i].title, gx0 + S(14), y + S(6), gw - S(28) - (dur[0] ? S(70) : 0), 0, sel ? WHITE : GRAY);
                draw_text(F_S, chn, gx0 + S(14), y + S(38), gw - S(28), 0, sel ? GRAY : DIM);
                if (sel) glow_rect(gx0, y, gw, rh - S(6));
            }
        }
        hint = dfocus == 0 ? "A Type   B Delete   X Space   Y Layer   L1/R1 Cursor   L2 Exit   START Search"
                           : "A Download   X Download all   Y Queue   B / Left Keyboard";
    } else {
        title = "Download queue"; snprintf(rt, sizeof rt, "%d total", djob_n);
        if (djob_n == 0) draw_text(F_M, "Nothing queued yet", W / 2, H / 2 - S(20), 0, 1, GRAY);
        else {
            if (ds_sel < ds_top) ds_top = ds_sel;
            if (ds_sel >= ds_top + vis) ds_top = ds_sel - vis + 1;
            for (int i = ds_top; i < djob_n && i < ds_top + vis; i++) {
                int y = top + S(8) + (i - ds_top) * rowh; char sb[220];
                static const char *sname[4] = { "Queued", "", "Done", "Failed" };
                if (djob[i].state == 1) snprintf(sb, sizeof sb, "%d%%  %s", djob[i].pct, djob[i].msg);
                else if (djob[i].state == 0) snprintf(sb, sizeof sb, "Queued");
                else snprintf(sb, sizeof sb, "%s: %s", sname[djob[i].state], djob[i].msg);
                draw_text(F_L, djob[i].label, x + S(20), y + S(2), w - S(40), 0, i == ds_sel ? WHITE : GRAY);
                draw_text(F_S, sb, x + S(20), y + S(40), w - S(40), 0, djob[i].state == 3 ? (SDL_Color){ 255, 110, 110, 255 } : djob[i].state == 2 ? ACCENT : DIM);
                if (djob[i].state == 1) { fill(x + S(20), y + S(66), w - S(40), S(3), 255, 255, 255, 50); fill(x + S(20), y + S(66), (w - S(40)) * djob[i].pct / 100, S(3), gr, gg, gb, 255); }
                if (i == ds_sel) glow_rect(x - S(8), y - S(2), w + S(16), rowh - S(8));
            }
        }
        hint = "B Back";
    }
    if (dl_flash[0]) draw_text(F_M, dl_flash, W / 2, H - S(84), W - S(80), 1, ACCENT);
    draw_footer(hint);
    if (ds == DS_HOME) draw_header(&vs[0], 0); else draw_title_header(title, rt);
}

/* ---------- search screen (menu > Search songs) ---------- */
static Kbd skb;
static int sfocus, sres[400], snres, ssel, srow;

static void search_update(void) {
    snres = 0; ssel = 0; srow = 0;
    char buf[200]; snprintf(buf, sizeof buf, "%s", skb.text);
    char *tok[8]; int nt = 0;
    for (char *p = strtok(buf, " "); p && nt < 8; p = strtok(NULL, " ")) tok[nt++] = p;
    if (!nt) return;
    for (int q = 0; q < g_nsongs && snres < 400; q++) {
        int i = g_sorted[q]; const Song *s = &g_songs[i]; int ok = 1;
        for (int t = 0; t < nt && ok; t++)
            ok = strcasestr(s->title, tok[t]) || strcasestr(s->artist, tok[t]) || strcasestr(s->album, tok[t]) || strcasestr(s->albumartist, tok[t]);
        if (ok) sres[snres++] = i;
    }
}

static void draw_search(void) {
    int bgr, bgg, bgb; theme_rgb(0, 0, 0, 0, &bgr, &bgg, &bgb);
    anim = 0; glow_target(pick_color((sfocus == 1 && snres) ? th_color(sres[ssel]) : 0x00DCFF)); glow_step();
    fill(0, 0, W, H, bgr, bgg, bgb, 255);
    int top = S(60), bot = H - S(40), lw = W / 2;
    kbd_box(&skb, S(16), top + S(14), lw - S(32), S(56), sfocus == 0, "Search your songs");
    int kw = (lw - S(32)) / 10, kh = S(78);
    kbd_draw(&skb, S(16), top + S(86), kw, kh, sfocus == 0, "Clear", "Close");
    fill(lw, top + S(10), 1, bot - top - S(20), (int)gcol[0], (int)gcol[1], (int)gcol[2], 70);

    /* right half: results, 2 columns x 3 rows (3 rows fit the height; 4 would shrink the art too far) */
    int gx0 = lw + S(10), cw = (W - gx0 - S(10)) / 2, gtop = top + S(10), ch = (bot - gtop - S(6)) / 3;
    int T = ch - S(58) - S(10); if (T > cw - S(28)) T = cw - S(28);
    int Ts = T * 112 / 100;
    if (!skb.text[0]) draw_text(F_M, "Type to search your library", gx0 + (W - gx0) / 2, H / 2 - S(20), W - gx0 - S(30), 1, DIM);
    else if (snres == 0) draw_text(F_M, "No matches", gx0 + (W - gx0) / 2, H / 2 - S(20), W - gx0 - S(30), 1, DIM);
    else {
        int sr = ssel / 2;
        if (sr < srow) srow = sr;
        if (sr >= srow + 3) srow = sr - 2;
        int sx = 0, sy = 0, ss = -1;
        for (int r = srow; r < srow + 3; r++) for (int c = 0; c < 2; c++) {
            int i = r * 2 + c;
            if (i >= snres) break;
            int song = sres[i], x = gx0 + c * cw + (cw - T) / 2, y = gtop + (r - srow) * ch + S(14);
            request_thumb(song, g_T);
            if (i == ssel && sfocus == 1) { sx = x; sy = y; ss = song; continue; }
            if (th_state[song] == 2) { th_used[song] = frame_no; SDL_Rect d = { x, y, T, T }; SDL_RenderCopy(ren, th_tex[song], NULL, &d); }
            else draw_placeholder(x, y, T, 0);
            draw_text(F_S, g_songs[song].title, x, y + T + S(6), T, 0, GRAY);
            draw_text(F_S, g_songs[song].artist, x, y + T + S(32), T, 0, DIM);
        }
        if (ss >= 0) {
            int cx = sx + T / 2, cy = sy + T / 2 - S(8);
            draw_big_glow(cx, cy, Ts, 1.f);
            SDL_Rect d = { cx - Ts / 2, cy - Ts / 2, Ts, Ts };
            if (th_state[ss] == 2) { th_used[ss] = frame_no; SDL_RenderCopy(ren, th_tex[ss], NULL, &d); }
            else draw_placeholder(d.x, d.y, Ts, 0);
            draw_text(F_S, g_songs[ss].title, sx, sy + T + S(6), T, 0, WHITE);
            draw_text(F_S, g_songs[ss].artist, sx, sy + T + S(32), T, 0, GRAY);
        }
    }
    char rt[48] = ""; if (skb.text[0]) snprintf(rt, sizeof rt, "%d%s result%s", snres, snres >= 400 ? "+" : "", snres == 1 ? "" : "s");
    draw_footer(sfocus == 0 ? "A Type   B Delete   X Space   Y Layer   L1/R1 Cursor   L2 Exit   START Results" : "A Play   B / Left  Keyboard   MENU Options");
    draw_title_header("Search songs", rt);
}

static void draw_menu(void) {
    static const char *names[] = { "Resume", "Keep playing in background", "Turn screen off", "Quit" };
    fill(0, 0, W, H, 0, 0, 0, 170);
    if (theme_open) {
        int rows = nthemes < 8 ? nthemes : 8;
        int bw = S(640), bh = S(110) + rows * S(52), bx = (W - bw) / 2, by = (H - bh) / 2;
        { int mr, mg, mb; theme_rgb(1, 20, 20, 26, &mr, &mg, &mb); fill(bx, by, bw, bh, mr, mg, mb, 250); }
        SDL_SetRenderDrawColor(ren, (int)gcol[0], (int)gcol[1], (int)gcol[2], 255);
        SDL_Rect fr = { bx, by, bw, bh }; SDL_RenderDrawRect(ren, &fr);
        draw_text(F_L, "Theme", W / 2, by + S(12), 0, 1, WHITE);
        for (int k = 0; k < rows; k++) {
            int i = theme_top + k, y = by + S(66) + k * S(52);
            if (i == theme_sel) fill(bx + S(16), y, bw - S(32), S(46), 60, 60, 80, 255);
            uint32_t c = themes[i].color;
            if (c) { fill_round(bx + S(34), y + S(8), S(30), S(30), S(8), (c >> 16) & 255, (c >> 8) & 255, c & 255, 255);
                     if (themes[i].has_c2) { uint32_t c2 = themes[i].color2; fill_round(bx + S(46), y + S(20), S(18), S(18), S(6), (c2 >> 16) & 255, (c2 >> 8) & 255, c2 & 255, 255); } }
            else { SDL_SetRenderDrawColor(ren, 200, 200, 210, 255); SDL_Rect o = { bx + S(34), y + S(8), S(30), S(30) }; SDL_RenderDrawRect(ren, &o); }
            draw_text(F_M, themes[i].name, bx + S(84), y + S(8), bw - S(120), 0, i == theme_orig ? ACCENT : WHITE);
        }
        draw_text(F_S, "A Select   B Cancel   (green = current)", W / 2, by + bh - S(34), 0, 1, GRAY);
        return;
    }
    if (pick_open) {                                    /* "Add to playlist" picker: row 0 is + New Playlist */
        int n = npl + 1, rows = n < 8 ? n : 8;
        int bw = S(640), bh = S(110) + rows * S(52), bx = (W - bw) / 2, by = (H - bh) / 2;
        { int mr, mg, mb; theme_rgb(1, 20, 20, 26, &mr, &mg, &mb); fill(bx, by, bw, bh, mr, mg, mb, 250); }
        SDL_SetRenderDrawColor(ren, (int)gcol[0], (int)gcol[1], (int)gcol[2], 255);
        SDL_Rect fr = { bx, by, bw, bh }; SDL_RenderDrawRect(ren, &fr);
        draw_text(F_L, "Add to playlist", W / 2, by + S(12), 0, 1, WHITE);
        for (int k = 0; k < rows; k++) {
            int i = pick_top + k, y = by + S(66) + k * S(52);
            if (i == pick_sel) fill(bx + S(16), y, bw - S(32), S(46), 60, 60, 80, 255);
            if (i == 0) draw_text(F_M, "+  New Playlist", bx + S(34), y + S(8), bw - S(70), 0, ACCENT);
            else { char lb[120]; snprintf(lb, sizeof lb, "%s  (%d)", pls[i - 1].name, pls[i - 1].n); draw_text(F_M, lb, bx + S(34), y + S(8), bw - S(70), 0, WHITE); }
        }
        draw_text(F_S, "A Add   B Cancel", W / 2, by + bh - S(34), 0, 1, GRAY);
        return;
    }
    const int *ids = set_open ? set_ids : menu_ids; int mn = set_open ? set_n : menu_n, msel = set_open ? set_sel : menu_sel;
    int extra = (!set_open && usb_dirty) ? S(40) : 0, rowp = mn > 9 ? S(46) : S(58);
    int bw = S(640), bh = S(90) + mn * rowp + extra, bx = (W - bw) / 2, by = (H - bh) / 2;
    { int mr, mg, mb; theme_rgb(1, 20, 20, 26, &mr, &mg, &mb); fill(bx, by, bw, bh, mr, mg, mb, 250); }
    SDL_SetRenderDrawColor(ren, (int)gcol[0], (int)gcol[1], (int)gcol[2], 255);
    SDL_Rect fr = { bx, by, bw, bh }; SDL_RenderDrawRect(ren, &fr);
    draw_text(F_L, set_open ? "Settings" : "Options", W / 2, by + S(14), 0, 1, WHITE);
    for (int i = 0; i < mn; i++) {
        int y = by + S(74) + i * rowp, id = ids[i];
        if (i == msel) fill(bx + S(16), y, bw - S(32), rowp - S(8), 60, 60, 80, 255);
        char tl[80]; const char *label = names[0];
        if (id == M_THEME) { snprintf(tl, sizeof tl, "Theme: %s", themes[theme_cur].name); label = tl; }
        else if (id == M_LED) { snprintf(tl, sizeof tl, "LED sync: %s", led_on_cfg ? "On" : "Off"); label = tl; }
        else if (id == M_EQ) label = "Equalizer";
        else if (id == M_TAGS) label = "Edit tags";
        else if (id == M_ADDPL) label = "Add to playlist";
        else if (id == M_SETTINGS) label = "Settings";
        else if (id == M_SEARCH) label = "Search songs";
        else if (id == M_RESCAN) label = "Rescan songs";
        else if (id == M_USB) label = usb_on ? "USB Mode (Linux Only): ON  (A to stop)" : "USB Mode (Linux Only)";
        else label = names[id];
        draw_text(F_M, label, W / 2, y + (rowp - S(34)) / 2, bw - S(60), 1, id == M_USB && usb_on ? ACCENT : WHITE);
    }
    if (extra) draw_text(F_S, "Use Settings > Rescan songs to load any music you added", W / 2, by + bh - S(34), bw - S(40), 1, GRAY);
}

/* ============================ actions ============================ */
static void brightness_restore(void) {
    if (bl_via_ioctl) disp_set_brightness(bl_saved);
    else bl_write(bl_saved > 0 ? bl_saved : bl_default);
}
static void wake(void) {
    if (!screen_off) return;
    screen_off = 0;
    panel_power(1);
    brightness_restore();
    dirty = 1;
}
static void screen_off_enter(void) {
    int dv = screen_ioctl ? disp_get_brightness() : -1;
    bl_via_ioctl = dv > 0 && dv <= 255;
    if (bl_via_ioctl) bl_saved = dv;
    else { int v = bl_read(); bl_saved = v > 0 ? v : bl_default; }
    menu_open = 0; screen_off = 1;
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren); SDL_RenderPresent(ren);
    if (bl_via_ioctl) disp_set_brightness(0); else bl_write(0);
    panel_power(0);
}

static void tags_open(void);
static void open_menu(void) {
    menu_n = 0;
    menu_ids[menu_n++] = M_RESUME;
    if (scr == SCR_NOW) {                                                  /* only offered from the Now Playing screen */
        menu_ids[menu_n++] = M_EQ; menu_ids[menu_n++] = M_TAGS;
        if (sn.valid) menu_ids[menu_n++] = M_ADDPL;
    }
    if (sn.valid && !sn.ended) menu_ids[menu_n++] = M_BG;
    menu_ids[menu_n++] = M_SEARCH;
#ifndef NO_USB
    menu_ids[menu_n++] = M_USB;
#endif
    menu_ids[menu_n++] = M_SETTINGS;
    menu_ids[menu_n++] = M_QUIT;
    set_n = 0; set_ids[set_n++] = M_THEME; set_ids[set_n++] = M_LED; set_ids[set_n++] = M_RESCAN;
    menu_sel = 0; menu_open = 1; theme_open = 0; set_open = 0; pick_open = 0;
}

static void enter_bg(void) {
    menu_open = 0; bg = 1;
    gfx_shutdown();
    unlink(UI_PATH);                       /* tells launch.sh to hand the screen back to the OS */
}
static void leave_bg(void) {
    if (!bg) return;
    if (gfx_init() < 0) { gfx_shutdown(); return; }
    bg = 0; dirty = 1; menu_open = 0;
    scr = sn.valid ? SCR_NOW : SCR_GRID;
}

static void usb_toggle(void) {
    int starting = !usb_on;
    FILE *bf = fopen("/tmp/vibe.usbbusy", "w"); if (bf) fclose(bf);
    char cmd[112];
    snprintf(cmd, sizeof cmd, "sh ./usbmusic.sh %s >> app.log 2>&1 &", starting ? "start" : "stop");
    if (system(cmd)) {}
    for (Uint32 t0 = SDL_GetTicks(); SDL_GetTicks() - t0 < 25000; ) {   /* wait for the script, max 25 s */
        if (access("/tmp/vibe.usbbusy", F_OK) != 0) break;
        SDL_PumpEvents();
        fill(0, 0, W, H, 0, 0, 0, 255);
        draw_text(F_L, starting ? "Starting USB Music mode..." : "Stopping USB Music mode...", W / 2, H / 2 - S(20), 0, 1, WHITE);
        SDL_RenderPresent(ren);
        SDL_Delay(100);
    }
    int was = usb_on;
    usb_on = access("/tmp/vibe.usb", F_OK) == 0;
    if (was && !usb_on) usb_dirty = 1;
    dirty = 1;
}

static void switch_tab(int d);
static void kb_submit(void) {
    if (!urlkb.text[0]) return;
    if (dl_enqueue(urlkb.text, urlkb.text)) { dl_flash_msg("Queued"); ds = DS_QUEUE; ds_sel = ds_top = 0; }
    else dl_flash_msg("That isn't a valid URL or video id");
}
static void dl_press(int bt) {
    dres_n = dl_get_results(dres, DL_MAXRES, &dres_state);
    djob_n = dl_get_jobs(djob, DL_MAXJOB);
    if (ds == DS_HOME) {
        switch (bt) {
        case BT_L1:   switch_tab(-1); break;
        case BT_R1:   switch_tab(1); break;
        case BT_UP:   ds_sel = ds_sel > 0 ? ds_sel - 1 : DS_NITEMS - 1; break;
        case BT_DOWN: ds_sel = ds_sel < DS_NITEMS - 1 ? ds_sel + 1 : 0; break;
        case BT_A:
            if (ds_sel == 0) { ds = DS_SEARCH; dfocus = 0; }
            else if (ds_sel == 1) { memset(&urlkb, 0, sizeof urlkb); ds = DS_KEYS; }
            else if (ds_sel == 2) { int n = dl_import_links(); char m[80]; if (n < 0) snprintf(m, sizeof m, "No links.txt in Music/Downloads"); else snprintf(m, sizeof m, "Queued %d link%s", n, n == 1 ? "" : "s"); dl_flash_msg(m); }
            else if (ds_sel == 3) { ds = DS_QUEUE; ds_sel = ds_top = 0; }
            else { dl_update_start(); dl_flash_msg("Updating yt-dlp..."); }
            break;
        }
    } else if (ds == DS_KEYS) {
        int r = kbd_press(&urlkb, bt, 0);
        if (r == KR_SUBMIT) kb_submit(); else if (r == KR_CANCEL) { ds = DS_HOME; ds_sel = 0; }
    } else if (ds == DS_SEARCH) {
        if (dfocus == 0) {
            int r = kbd_press(&dlkb, bt, 1);
            if (r == KR_SUBMIT) { if (dlkb.text[0]) { dl_search_start(dlkb.text); dsel = drow = 0; } }
            else if (r == KR_CANCEL) { ds = DS_HOME; ds_sel = 0; }
            else if (r == KR_RIGHT && dres_n) dfocus = 1;
        } else {
            switch (bt) {
            case BT_UP:    if (dres_n) dsel = dsel > 0 ? dsel - 1 : dres_n - 1; break;
            case BT_DOWN:  if (dres_n) dsel = dsel < dres_n - 1 ? dsel + 1 : 0; break;
            case BT_L1:    dsel -= 6; if (dsel < 0) dsel = 0; break;
            case BT_R1:    dsel += 6; if (dsel >= dres_n) dsel = dres_n - 1; if (dsel < 0) dsel = 0; break;
            case BT_LEFT:  dfocus = 0; break;
            case BT_B:     dfocus = 0; break;
            case BT_Y:     ds = DS_QUEUE; ds_sel = ds_top = 0; break;
            case BT_A:     { char lb[300]; snprintf(lb, sizeof lb, "%s%s%s", dres[dsel].title, dres[dsel].artist[0] ? " - " : "", dres[dsel].artist);
                             dl_flash_msg(dl_enqueue(dres[dsel].id, lb) ? "Queued" : "Queue full"); } break;
            case BT_X:     { int c = 0; for (int i = 0; i < dres_n; i++) { char lb[300]; snprintf(lb, sizeof lb, "%s - %s", dres[i].title, dres[i].artist); c += dl_enqueue(dres[i].id, lb); }
                             char m[48]; snprintf(m, sizeof m, "Queued %d songs", c); dl_flash_msg(m); } break;
            }
        }
    } else {
        switch (bt) {
        case BT_UP:   if (djob_n) ds_sel = ds_sel > 0 ? ds_sel - 1 : djob_n - 1; break;
        case BT_DOWN: if (djob_n) ds_sel = ds_sel < djob_n - 1 ? ds_sel + 1 : 0; break;
        case BT_B:    ds = DS_HOME; ds_sel = 0; break;
        }
    }
}

static void search_press(int bt) {
    if (sfocus == 0) {
        if (bt == BT_START) { if (snres) sfocus = 1; return; }
        int r = kbd_press(&skb, bt, 1);
        if (r == KR_CHANGED) search_update();
        else if (r == KR_CANCEL) scr = SCR_GRID;
        else if (r == KR_SUBMIT) { skb.text[0] = 0; search_update(); }          /* the "Clear" button */
        else if (r == KR_RIGHT && snres) sfocus = 1;
    } else {
        switch (bt) {
        case BT_UP:    if (ssel >= 2) ssel -= 2; break;
        case BT_DOWN:  if (ssel + 2 < snres) ssel += 2; else if (ssel / 2 < (snres - 1) / 2) ssel = snres - 1; break;
        case BT_LEFT:  if (ssel % 2 == 0) sfocus = 0; else ssel--; break;
        case BT_RIGHT: if (ssel % 2 == 0 && ssel + 1 < snres) ssel++; break;
        case BT_B:     sfocus = 0; break;
        case BT_A: {
            int song = sres[ssel], p = 0;
            while (p < g_nsongs && g_sorted[p] != song) p++;
            pending_song = song; engine_start_queue(g_sorted, g_nsongs, p < g_nsongs ? p : 0); q_kind = 1; rs_valid = 0; scr = SCR_NOW;
            break; }
        }
    }
}

/* ---------- equalizer screen (MENU on Now Playing) ---------- */
static int eq_band;
static const char *EQ_LABELS[10] = { "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
static const struct { const char *name; float g[10]; } EQ_PRESETS[] = {
    { "Flat",         {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 } },
    { "Bass boost",   {  6,  5,  4,  2,  0,  0,  0,  0,  0,  0 } },
    { "Treble boost", {  0,  0,  0,  0,  0,  0,  2,  4,  5,  6 } },
    { "Vocal",        { -2, -2, -1,  1,  3,  4,  3,  1,  0, -1 } },
    { "Rock",         {  5,  4,  2, -1, -2, -1,  2,  4,  5,  5 } },
    { "Electronic",   {  5,  4,  1,  0, -2,  2,  1,  2,  4,  5 } },
    { "Acoustic",     {  4,  4,  3,  1,  1,  1,  2,  3,  3,  2 } },
};
#define EQ_NPRESETS 7
static char eq_flash[64]; static Uint32 eq_flash_t;
static int eq_preset_index(void) {
    for (int p = 0; p < EQ_NPRESETS; p++) { int same = 1; for (int k = 0; k < 10; k++) if (fabsf(engine_eq_get(k) - EQ_PRESETS[p].g[k]) > 0.05f) same = 0; if (same) return p; }
    return -1;
}
static void draw_eq(void) {
    int bgr, bgg, bgb; theme_rgb(0, 0, 0, 0, &bgr, &bgg, &bgb);
    anim = 0; glow_target(pick_color(0x00DCFF)); glow_step();
    fill(0, 0, W, H, bgr, bgg, bgb, 255);
    int gr = (int)gcol[0], gg = (int)gcol[1], gb = (int)gcol[2];
    int top = S(60), bot = H - S(40), gx = S(40), cw = (W - 2 * gx) / 10, ty0 = top + S(80), ty1 = bot - S(170), mid = (ty0 + ty1) / 2;
    for (int k = 0; k < 10; k++) {
        int cx = gx + k * cw + cw / 2, sel = (k == eq_band); float g = engine_eq_get(k);
        int y = mid - (int)(g / 12.f * (ty1 - ty0) / 2.f);
        fill(cx - S(3), ty0, S(6), ty1 - ty0, 255, 255, 255, 40);
        fill(cx - S(16), mid - 1, S(32), 2, 255, 255, 255, 110);
        if (y < mid) fill(cx - S(3), y, S(6), mid - y, gr, gg, gb, 255); else fill(cx - S(3), mid, S(6), y - mid, gr, gg, gb, 255);
        fill_round(cx - S(16), y - S(9), S(32), S(18), S(7), sel ? 255 : gr, sel ? 255 : gg, sel ? 255 : gb, 255);
        char v[16]; snprintf(v, sizeof v, "%+d", (int)lroundf(g));
        draw_text(F_M, v, cx, ty0 - S(46), 0, 1, sel ? WHITE : GRAY);
        draw_text(F_S, EQ_LABELS[k], cx, ty1 + S(14), 0, 1, sel ? WHITE : DIM);
        if (sel) glow_rect(cx - cw / 2 + S(8), ty0 - S(52), cw - S(16), (ty1 - ty0) + S(104));
    }
    draw_text(F_S, "Hz", gx + S(2), ty1 + S(40), 0, 0, DIM);
    int pi = eq_preset_index(); char pl[96];
    snprintf(pl, sizeof pl, "L1/R1   Preset: %s", pi >= 0 ? EQ_PRESETS[pi].name : "Custom");
    const char *btn[3] = { "X   Reset to default", pl, "B   Done" };
    int kr, kg, kb_; key_rgb(&kr, &kg, &kb_);
    int bw3 = (W - 2 * gx - 2 * S(20)) / 3, by = bot - S(104);
    for (int i = 0; i < 3; i++) {
        int x = gx + i * (bw3 + S(20));
        fill_round(x, by, bw3, S(64), S(12), kr, kg, kb_, 255);
        draw_text(F_M, btn[i], x + bw3 / 2, by + S(15), bw3 - S(20), 1, WHITE);
    }
    if (eq_flash[0] && SDL_GetTicks() - eq_flash_t > 2500) eq_flash[0] = 0;
    if (eq_flash[0]) draw_text(F_M, eq_flash, W / 2, by - S(40), 0, 1, ACCENT);
    draw_footer("Left/Right Band   Up/Down Gain   L1/R1 Preset   X Reset to default   B Done");
    draw_title_header("Equalizer", "applies to all music");
}
static void eq_press(int bt) {
    switch (bt) {
    case BT_LEFT:  eq_band = eq_band > 0 ? eq_band - 1 : 9; break;
    case BT_RIGHT: eq_band = eq_band < 9 ? eq_band + 1 : 0; break;
    case BT_UP:    engine_eq_set(eq_band, engine_eq_get(eq_band) + 1.f); break;
    case BT_DOWN:  engine_eq_set(eq_band, engine_eq_get(eq_band) - 1.f); break;
    case BT_L1: case BT_R1: {
        int p = eq_preset_index();
        p = bt == BT_R1 ? (p + 1) % EQ_NPRESETS : (p <= 0 ? EQ_NPRESETS - 1 : p - 1);
        for (int k = 0; k < 10; k++) engine_eq_set(k, EQ_PRESETS[p].g[k]);
        break; }
    case BT_X:     engine_eq_reset(); snprintf(eq_flash, sizeof eq_flash, "Reset to default"); eq_flash_t = SDL_GetTicks(); break;
    case BT_A: case BT_B: settings_save(); scr = SCR_NOW; break;
    }
}

/* ---------- tag editor (MENU on Now Playing) ---------- */
static Tags tg; static int tag_sel, tag_edit, tag_song = -1, tag_err; static Kbd tagkb; static char tag_msg[200];
static const char *TAG_NAMES[7] = { "Title", "Artist", "Album", "Album artist", "Track number", "Year", "Genre" };
static char *tag_field(int i) {
    switch (i) { case 0: return tg.title; case 1: return tg.artist; case 2: return tg.album; case 3: return tg.albumartist; case 4: return tg.track; case 5: return tg.date; default: return tg.genre; }
}
static size_t tag_cap(int i) {
    switch (i) { case 0: return sizeof tg.title; case 1: return sizeof tg.artist; case 2: return sizeof tg.album; case 3: return sizeof tg.albumartist; case 4: return sizeof tg.track; case 5: return sizeof tg.date; default: return sizeof tg.genre; }
}
static void tags_open(void) {
    if (!sn.valid) return;
    tag_song = sn.song; tag_sel = 0; tag_edit = 0; tag_msg[0] = 0; tag_err = 0;
    if (tags_read(g_songs[tag_song].path, &tg) != 0) { memset(&tg, 0, sizeof tg); snprintf(tag_msg, sizeof tag_msg, "Couldn't read the tags of this file"); tag_err = 1; }
    scr = SCR_TAGS;
}
static void draw_tags(void) {
    int bgr, bgg, bgb; theme_rgb(0, 0, 0, 0, &bgr, &bgg, &bgb);
    anim = 0; glow_target(pick_color(th_color(tag_song >= 0 ? tag_song : 0))); glow_step();
    fill(0, 0, W, H, bgr, bgg, bgb, 255);
    int top = S(60), rowh = S(62), x = S(28), w = W - S(56);
    const char *fname = tag_song >= 0 ? strrchr(g_songs[tag_song].path, '/') : NULL;
    if (tag_edit) {
        int kw = S(86), kh = S(74), x0 = (W - 10 * kw) / 2;
        kbd_box(&tagkb, x0, top + S(24), 10 * kw, S(60), 1, TAG_NAMES[tag_sel]);
        kbd_draw(&tagkb, x0, top + S(110), kw, kh, 1, "OK", "Cancel");
        draw_footer("A Type   B Delete   X Space   Y Layer   L1/R1 Cursor   L2 Exit   START OK");
        char t[64]; snprintf(t, sizeof t, "Edit: %s", TAG_NAMES[tag_sel]);
        draw_title_header(t, "");
        return;
    }
    for (int i = 0; i < 9; i++) {
        int y = top + S(10) + i * rowh;
        if (i < 7) {
            draw_text(F_S, TAG_NAMES[i], x + S(20), y + S(18), S(200), 0, DIM);
            const char *v = tag_field(i);
            draw_text(F_M, v[0] ? v : "(empty)", x + S(240), y + S(12), w - S(260), 0, i == tag_sel ? (v[0] ? WHITE : GRAY) : (v[0] ? GRAY : DIM));
        } else draw_text(F_M, i == 7 ? "Save to file" : "Cancel", x + S(20), y + S(12), w - S(40), 0, i == 7 ? ACCENT : (i == tag_sel ? WHITE : GRAY));
        if (i == tag_sel) glow_rect(x - S(8), y, w + S(16), rowh - S(8));
    }
    if (tag_msg[0]) draw_text(F_M, tag_msg, W / 2, H - S(92), W - S(80), 1, tag_err ? (SDL_Color){ 255, 110, 110, 255 } : ACCENT);
    draw_footer("A Edit   Up/Down Choose   B Back   (changes the song's file)");
    draw_title_header("Edit tags", fname ? fname + 1 : "");
}
static void tags_save(void) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
    draw_text(F_L, "Saving tags...", W / 2, H / 2 - S(20), 0, 1, WHITE);
    SDL_RenderPresent(ren);
    char err[160] = "";
    if (tags_write(g_songs[tag_song].path, &tg, err, sizeof err) == 0) {
        Song *sg = &g_songs[tag_song];
        if (tg.title[0]) snprintf(sg->title, sizeof sg->title, "%s", tg.title);
        snprintf(sg->artist, sizeof sg->artist, "%s", tg.artist);
        snprintf(sg->album, sizeof sg->album, "%s", tg.album);
        snprintf(sg->albumartist, sizeof sg->albumartist, "%s", tg.albumartist);
        browse_free(); browse_build();                              /* the song may belong to a different album/artist now */
        vd = 0;
        reset_roots();
        vs[0] = saved_root[tab];
        snres = 0; skb.text[0] = 0;
        snprintf(tag_msg, sizeof tag_msg, "Saved"); tag_err = 0;
    } else { snprintf(tag_msg, sizeof tag_msg, "Not saved: %s", err); tag_err = 1; }
}
static void tags_press(int bt) {
    if (tag_edit) {
        int r = kbd_press(&tagkb, bt, 0);
        if (r == KR_SUBMIT) { snprintf(tag_field(tag_sel), tag_cap(tag_sel), "%s", tagkb.text); tag_edit = 0; }
        else if (r == KR_CANCEL) tag_edit = 0;
        return;
    }
    switch (bt) {
    case BT_UP:   tag_sel = tag_sel > 0 ? tag_sel - 1 : 8; break;
    case BT_DOWN: tag_sel = tag_sel < 8 ? tag_sel + 1 : 0; break;
    case BT_B:    scr = SCR_NOW; break;
    case BT_A:
        if (tag_sel < 7) { memset(&tagkb, 0, sizeof tagkb); snprintf(tagkb.text, sizeof tagkb.text, "%s", tag_field(tag_sel)); tagkb.cur = (int)strlen(tagkb.text); tag_edit = 1; tag_msg[0] = 0; }
        else if (tag_sel == 7) tags_save();
        else scr = SCR_NOW;
        break;
    }
}

static void scan_cb(int n);
static void rescan_library(void) {
    snres = 0; skb.text[0] = 0;
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
    draw_text(F_L, "Rescanning...", W / 2, H / 2 - S(20), 0, 1, WHITE);
    SDL_RenderPresent(ren);
    engine_reset();                                   /* playback stops: song indices are about to change */
    loader_quiesce();
    Song *old = g_songs;
    g_songs = NULL; g_nsongs = 0;
    lib_scan(music_root, "cache/library2.tsv", scan_cb);
    free(old);
    browse_free(); browse_build();
    thumbs_realloc();
    engine_resize();
    vd = 0; pending_song = -1; scr = SCR_GRID; usb_dirty = 0;
    reset_roots();
    vs[0] = saved_root[tab];
    snres = 0; skb.text[0] = 0;
    dirty = 1;
}

static void theme_press(int bt) {
    switch (bt) {
    case BT_UP:   theme_sel = theme_sel > 0 ? theme_sel - 1 : nthemes - 1; theme_cur = theme_sel; break;          /* live preview, wraps */
    case BT_DOWN: theme_sel = theme_sel < nthemes - 1 ? theme_sel + 1 : 0; theme_cur = theme_sel; break;
    case BT_A:    theme_orig = theme_cur; settings_save(); theme_open = 0; break;
    case BT_B: case BT_MENU: theme_cur = theme_orig; theme_open = 0; break;
    }
    if (theme_sel < theme_top) theme_top = theme_sel;
    if (theme_sel >= theme_top + 8) theme_top = theme_sel - 7;
}

static void pick_press(int bt) {
    int n = npl + 1;
    switch (bt) {
    case BT_UP:   pick_sel = pick_sel > 0 ? pick_sel - 1 : n - 1; break;
    case BT_DOWN: pick_sel = pick_sel < n - 1 ? pick_sel + 1 : 0; break;
    case BT_B: case BT_MENU: pick_open = 0; break;
    case BT_A:
        pick_open = 0; menu_open = 0;
        if (!sn.valid) break;
        if (pick_sel == 0) plname_open(sn.song, SCR_NOW);
        else {
            int r = pl_add_path(pick_sel - 1, g_songs[sn.song].path); char m[160];
            if (r > 0) { pl_save(); snprintf(m, sizeof m, "Added to \"%s\"", pls[pick_sel - 1].name); }
            else if (r == 0) snprintf(m, sizeof m, "Already in \"%s\"", pls[pick_sel - 1].name);
            else snprintf(m, sizeof m, "Couldn't add the song");
            ui_toast(m);
        }
        break;
    }
    if (pick_sel < pick_top) pick_top = pick_sel;
    if (pick_sel >= pick_top + 8) pick_top = pick_sel - 7;
}
static void settings_press(int bt) {
    switch (bt) {
    case BT_UP:   set_sel = set_sel > 0 ? set_sel - 1 : set_n - 1; break;
    case BT_DOWN: set_sel = set_sel < set_n - 1 ? set_sel + 1 : 0; break;
    case BT_B: case BT_MENU: set_open = 0; break;
    case BT_A:
        switch (set_ids[set_sel]) {
        case M_THEME: theme_open = 1; theme_sel = theme_orig = theme_cur; theme_top = theme_sel > 7 ? theme_sel - 7 : 0; break;
        case M_LED: led_on_cfg = !led_on_cfg; settings_save(); if (!led_on_cfg) led_restore(); break;
        case M_RESCAN: set_open = 0; rescan_library(); menu_open = 0; break;
        }
        break;
    }
}

static void menu_press(int bt) {
    switch (bt) {
    case BT_UP:   menu_sel = menu_sel > 0 ? menu_sel - 1 : menu_n - 1; break;               /* wrap around */
    case BT_DOWN: menu_sel = menu_sel < menu_n - 1 ? menu_sel + 1 : 0; break;
    case BT_B: case BT_MENU: menu_open = 0; break;
    case BT_A:
        switch (menu_ids[menu_sel]) {
        case M_RESUME: menu_open = 0; break;
        case M_BG: enter_bg(); break;
        case M_SETTINGS: set_open = 1; set_sel = 0; break;
        case M_ADDPL: pick_open = 1; pick_sel = 0; pick_top = 0; break;
        case M_USB: usb_toggle(); break;
        case M_SEARCH: menu_open = 0; scr = SCR_SEARCH; sfocus = 0; break;
        case M_EQ: menu_open = 0; scr = SCR_EQ; break;
        case M_TAGS: menu_open = 0; tags_open(); break;
        case M_QUIT: want_quit = 1; break;
        }
        break;
    }
}

/* Play an album from track `startpos`; the queue then continues through the rest of the
   library (next albums...), and shuffle draws from the whole library. */
static void play_album_seek(int a, int startpos, double seek) {
    int *q = malloc((size_t)g_nsongs * sizeof(int)), k = 0;
    for (int t = 0; t < g_nalbums; t++) {
        Album *A = &g_albums[(a + t) % g_nalbums];
        for (int j = 0; j < A->n; j++) q[k++] = A->songs[j];
    }
    pending_song = g_albums[a].songs[startpos];
    engine_start_queue_seek(q, k, startpos, seek);
    free(q);
    q_kind = 2; rs_valid = 0;
    scr = SCR_NOW;
}
static void play_album(int a, int startpos) { play_album_seek(a, startpos, 0.0); }

/* Resume what was playing when the Brick was switched off (Y on the browse screens, before playing anything else). */
static void resume_playback(void) {
    int song = -1;
    for (int i = 0; i < g_nsongs; i++) if (!strcmp(g_songs[i].path, rs_path)) { song = i; break; }
    rs_valid = 0;
    if (song < 0) return;
    engine_set_modes(rs_shuffle, rs_repeat);
    pending_song = song;
    if (rs_kind == 2) {
        for (int a = 0; a < g_nalbums; a++) for (int t = 0; t < g_albums[a].n; t++)
            if (g_albums[a].songs[t] == song) { play_album_seek(a, t, rs_pos); return; }
    }
    if (rs_kind == 3) { engine_start_queue_seek(NULL, 0, song, rs_pos); q_kind = 3; }
    else {
        int p = 0; while (p < g_nsongs && g_sorted[p] != song) p++;
        engine_start_queue_seek(g_sorted, g_nsongs, p < g_nsongs ? p : 0, rs_pos); q_kind = 1;
    }
    scr = SCR_NOW;
}
static void push_view(int kind, int param) {
    if (vd >= 3) return;
    vd++; vs[vd] = (View){ kind, param, 0, 0 };
}
static void switch_tab(int d) {
    saved_root[tab] = vs[0];
    tab = (tab + d + NTABS) % NTABS;
    vs[0] = saved_root[tab];
}
static int pl_confirm = -1;
static void pl_press(int bt) {                               /* the Playlists tab: row 0 = + New Playlist */
    View *v = &vs[0]; int n = npl + 1, conf = pl_confirm; pl_confirm = -1;
    switch (bt) {
    case BT_L1:   switch_tab(-1); break;
    case BT_R1:   switch_tab(1); break;
    case BT_UP:   v->sel = v->sel > 0 ? v->sel - 1 : n - 1; break;
    case BT_DOWN: v->sel = v->sel < n - 1 ? v->sel + 1 : 0; break;
    case BT_LEFT: v->sel -= 6; if (v->sel < 0) v->sel = 0; break;
    case BT_RIGHT: v->sel += 6; if (v->sel >= n) v->sel = n - 1; break;
    case BT_A:
        if (v->sel == 0) plname_open(-1, SCR_GRID);
        else { plv_rebuild(v->sel - 1); push_view(VK_PL_SONGS, v->sel - 1); }
        break;
    case BT_X:
        if (v->sel > 0) {
            if (conf == v->sel) { pl_delete(v->sel - 1); pl_save(); if (v->sel > npl) v->sel = npl; ui_toast("Playlist deleted"); }
            else { pl_confirm = v->sel; ui_toast("Press X again to delete this playlist"); }
        }
        break;
    case BT_Y: if (sn.valid || pending_song >= 0) scr = SCR_NOW; else if (rs_valid) resume_playback(); break;
    case BT_START: pending_song = engine_shuffle_queue(NULL, 0); q_kind = 3; rs_valid = 0; scr = SCR_NOW; break;
    }
}
static void browse_press(int bt) {
    View *v = &vs[vd];
    int n = view_n(v), grid = is_grid(v->kind);
    if (v->kind == VK_DOWNLOAD) { dl_press(bt); return; }
    if (v->kind == VK_PLAYLISTS) { pl_press(bt); return; }
    if (n <= 0 && bt != BT_B && bt != BT_L1 && bt != BT_R1) return;
    switch (bt) {
    case BT_LEFT:  if (grid) { if (v->sel > 0) v->sel--; } else { v->sel -= g_lvis; if (v->sel < 0) v->sel = 0; } break;
    case BT_RIGHT: if (grid) { if (v->sel < n - 1) v->sel++; } else { v->sel += g_lvis; if (v->sel >= n) v->sel = n - 1; } break;
    case BT_UP:    if (grid) { if (v->sel >= g_cols) v->sel -= g_cols; } else if (v->sel > 0) v->sel--; break;
    case BT_DOWN:  if (grid) v->sel = v->sel + g_cols < n ? v->sel + g_cols : n - 1;
                   else if (v->sel < n - 1) v->sel++;
                   break;
    case BT_L1: case BT_R1: {
        int d = bt == BT_R1 ? 1 : -1;
        if (vd == 0) switch_tab(d);
        else { v->sel += d * (grid ? g_cols * g_vis : g_lvis); if (v->sel < 0) v->sel = 0; if (v->sel >= n) v->sel = n - 1; }
        break; }
    case BT_B: if (vd > 0) vd--; break;
    case BT_A:
        switch (v->kind) {
        case VK_ALBUMS: case VK_ARTIST_ALBUMS: push_view(VK_ALBUM_SONGS, album_of(v, v->sel)); break;
        case VK_ARTISTS: push_view(VK_ARTIST_ALBUMS, v->sel); break;
        case VK_SONGS: pending_song = g_sorted[v->sel]; engine_start_queue(g_sorted, g_nsongs, v->sel); q_kind = 1; rs_valid = 0; scr = SCR_NOW; break;
        case VK_ALBUM_SONGS: play_album(v->param, v->sel); break;
        case VK_PL_SONGS: pending_song = plv_song[v->sel]; engine_start_queue(plv_song, plv_n, v->sel); q_kind = 1; rs_valid = 0; scr = SCR_NOW; break;
        }
        break;
    case BT_START:
        if (v->kind == VK_PL_SONGS) { pending_song = engine_shuffle_queue(plv_song, plv_n); q_kind = 1; }
        else { pending_song = engine_shuffle_queue(NULL, 0); q_kind = 3; }
        rs_valid = 0; scr = SCR_NOW; break;
    case BT_X:
        if (v->kind == VK_PL_SONGS) {
            pl_remove_at(v->param, plv_pi[v->sel]); pl_save(); plv_rebuild(v->param);
            if (v->sel >= plv_n && v->sel > 0) v->sel = plv_n - 1;
            ui_toast("Removed from the playlist");
        } else engine_toggle_shuffle();
        break;
    case BT_Y: if (sn.valid || pending_song >= 0) scr = SCR_NOW; else if (rs_valid) resume_playback(); break;
    }
}
/* Opens the album that contains `song` in the Albums tab (as if you had drilled into it). */
static void open_album_of(int song) {
    for (int a = 0; a < g_nalbums; a++)
        for (int t = 0; t < g_albums[a].n; t++)
            if (g_albums[a].songs[t] == song) {
                saved_root[tab] = vs[0];
                tab = 0; vs[0] = saved_root[0];
                vs[0].kind = VK_ALBUMS; vs[0].sel = a;
                vd = 1; vs[1] = (View){ VK_ALBUM_SONGS, a, t, 0 };
                scr = SCR_GRID;
                return;
            }
}

static void now_press(int bt) {
    switch (bt) {
    case BT_A:     engine_toggle_pause(); break;
    case BT_B:     scr = SCR_GRID; break;
    case BT_L1:    engine_prev(); break;
    case BT_R1:    engine_next(); break;
    case BT_LEFT:  engine_seek(-10); break;
    case BT_RIGHT: engine_seek(10); break;
    case BT_X:     engine_toggle_shuffle(); break;
    case BT_Y:     engine_cycle_repeat(); break;
    case BT_START: if (sn.valid) open_album_of(sn.song); break;
    case BT_L2:    vis_mode = (vis_mode + 3) % 4; settings_save(); break;
    case BT_R2:    vis_mode = (vis_mode + 1) % 4; settings_save(); break;
    }
}
static void press(int bt) {
    dirty = 1;
    if (screen_off) { if (bt == BT_SELECT) wake(); return; }
    if (menu_open) { if (theme_open) theme_press(bt); else if (pick_open) pick_press(bt); else if (set_open) settings_press(bt); else menu_press(bt); return; }
    if (bt == BT_MENU) { open_menu(); return; }
    if (bt == BT_SELECT) { screen_off_enter(); return; }
    if (scr == SCR_GRID) browse_press(bt); else if (scr == SCR_SEARCH) search_press(bt); else if (scr == SCR_EQ) eq_press(bt); else if (scr == SCR_TAGS) tags_press(bt); else if (scr == SCR_PLNAME) plname_press(bt); else now_press(bt);
}

/* ---------- hold-to-repeat for the d-pad ---------- */
static int held[4]; static Uint32 nextfire[4];
static void dir_press(int d) { if (!held[d]) { held[d] = 1; nextfire[d] = SDL_GetTicks() + 350; press(d); } }
static void dir_release(int d) { held[d] = 0; }
static int any_held(void) { return held[0] || held[1] || held[2] || held[3]; }

static void handle_cmd(int code) {
    switch (code) {
    case 1: engine_toggle_pause(); break;
    case 2: engine_set_pause(0); break;
    case 3: engine_set_pause(1); break;
    case 4: engine_next(); break;
    case 5: engine_prev(); break;
    case 6: leave_bg(); break;
    case 7: want_quit = 1; break;
    }
    dirty = 1;
}

static void process_event(SDL_Event *e) {
    static int hat_prev, ax_prev[2], trig[2];
    if (e->type == SDL_QUIT) { want_quit = 1; return; }
    if (e->type == ev_base) { handle_cmd(e->user.code); return; }
    if (bg) return;
    last_input = SDL_GetTicks();
    if (dbg) {
        FILE *f = fopen("input.log", "a");
        if (f) { fprintf(f, "type=0x%x btn=%d hat=%d axis=%d/%d key=%d\n", e->type, e->jbutton.button, e->jhat.value,
                         e->jaxis.axis, e->jaxis.value, e->key.keysym.sym); fclose(f); }
    }
    switch (e->type) {
    case SDL_KEYDOWN: {
        if (e->key.repeat) break;
        if (screen_off) { if (e->key.keysym.sym == SDLK_TAB) wake(); break; }
        int k = e->key.keysym.sym;
        if (k == SDLK_UP) dir_press(BT_UP); else if (k == SDLK_DOWN) dir_press(BT_DOWN);
        else if (k == SDLK_LEFT) dir_press(BT_LEFT); else if (k == SDLK_RIGHT) dir_press(BT_RIGHT);
        else if (k == SDLK_RETURN || k == SDLK_SPACE) press(BT_A);
        else if (k == SDLK_BACKSPACE) press(BT_B);
        else if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_m) press(BT_MENU);
        else if (k == SDLK_x) press(BT_X); else if (k == SDLK_y) press(BT_Y);
        else if (k == SDLK_l) press(BT_L1); else if (k == SDLK_r) press(BT_R1);
        else if (k == SDLK_PAGEUP) press(BT_L2); else if (k == SDLK_PAGEDOWN) press(BT_R2);
        else if (k == SDLK_s) press(BT_START); else if (k == SDLK_TAB) press(BT_SELECT);
        break; }
    case SDL_KEYUP: {
        int k = e->key.keysym.sym;
        if (k == SDLK_UP) dir_release(BT_UP); else if (k == SDLK_DOWN) dir_release(BT_DOWN);
        else if (k == SDLK_LEFT) dir_release(BT_LEFT); else if (k == SDLK_RIGHT) dir_release(BT_RIGHT);
        break; }
    case SDL_JOYBUTTONDOWN:
        if (screen_off) { if (jmap[BT_SELECT] == e->jbutton.button) wake(); break; }
        for (int i = BT_A; i < BT_COUNT; i++) if (jmap[i] == e->jbutton.button) { press(i); break; }
        break;
    case SDL_JOYHATMOTION: {
        int v = e->jhat.value;
        if (screen_off) { hat_prev = v; break; }
        static const int bits[4] = { SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT };
        for (int d = 0; d < 4; d++) {
            if ((v & bits[d]) && !(hat_prev & bits[d])) dir_press(d);
            if (!(v & bits[d]) && (hat_prev & bits[d])) dir_release(d);
        }
        hat_prev = v; break; }
    case SDL_JOYAXISMOTION: {
        int a = e->jaxis.axis;
        if (a == axis_l2 || a == axis_r2) {
            int *st = &trig[a == axis_r2], pr = e->jaxis.value > 16000;
            if (pr && !*st) { *st = 1; if (!screen_off) press(a == axis_l2 ? BT_L2 : BT_R2); }
            else if (!pr) *st = 0;
            break;
        }
        if (a > 1) break;
        int v = e->jaxis.value, s = v > 16000 ? 1 : v < -16000 ? -1 : 0;
        if (screen_off) { ax_prev[a] = s; break; }
        int neg = a == 0 ? BT_LEFT : BT_UP, pos = a == 0 ? BT_RIGHT : BT_DOWN;
        if (s != ax_prev[a]) {
            if (ax_prev[a] == -1) dir_release(neg);
            if (ax_prev[a] == 1) dir_release(pos);
            if (s == -1) dir_press(neg);
            if (s == 1) dir_press(pos);
            ax_prev[a] = s;
        }
        break; }
    }
}

/* ============================ control FIFO ============================ */
static void *ctl_main(void *a) {
    (void)a;
    int fd = open(CTL_PATH, O_RDWR);
    if (fd < 0) return NULL;
    char buf[256];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        buf[n] = 0;
        for (char *l = strtok(buf, "\r\n"); l; l = strtok(NULL, "\r\n")) {
            int code = !strcmp(l, "toggle") ? 1 : !strcmp(l, "play") ? 2 : !strcmp(l, "pause") ? 3 :
                       !strcmp(l, "next") ? 4 : !strcmp(l, "prev") ? 5 : !strcmp(l, "show") ? 6 :
                       !strcmp(l, "quit") ? 7 : 0;
            if (code) { SDL_Event ev; SDL_zero(ev); ev.type = ev_base; ev.user.code = code; SDL_PushEvent(&ev); }
        }
    }
    return NULL;
}

static void alog(const char *fmt, ...) {            /* appends to audio.log in the app folder */
    if (!audio_log) return;
    FILE *f = fopen("audio.log", "a");
    if (!f) return;
    fprintf(f, "[%6u s] ", (unsigned)(SDL_GetTicks() / 1000));
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static void cpu_state(char *out, size_t n) {
    char fq[32] = "?", gv[32] = "?"; FILE *f;
    if ((f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r"))) { if (fscanf(f, "%31s", fq) != 1) {} fclose(f); }
    if ((f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r"))) { if (fscanf(f, "%31s", gv) != 1) {} fclose(f); }
    snprintf(out, n, "cpu0 %s kHz, governor %s", fq, gv);
}

/* ============================ LEDs ============================
   The Brick's LEDs are driven through /sys/class/led_anim: per zone a hex colour (effect_rgb_hex_<zone>) and an
   effect (effect_<zone>: 0 off, 4 static). Zones: m = top strip, f1/f2 = front function keys, lr = shoulder lights.
   While the UI is visible the LEDs follow the glow colour; the stock settings are put back when the UI is hidden
   (a game starts), when the app quits, and when LED sync is turned off. The OS's own LED switch is respected. */
static const char *LED_ZONES[4] = { "m", "f1", "f2", "lr" };
static char led_saved_hex[4][16]; static int led_saved_fx[4];
static int led_cur;                                   /* 0 = stock is in control, 1 = following the glow, 2 = dark */

static int led_read(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f); out[strcspn(out, " \r\n")] = 0;
    return 1;
}
static void led_put(const char *path, const char *val) { FILE *f = fopen(path, "w"); if (f) { fputs(val, f); fclose(f); } }
static void led_zone(int z, const char *hex, int fx) {
    char p[96], v[16];
    if (hex) { snprintf(p, sizeof p, "/sys/class/led_anim/effect_rgb_hex_%s", LED_ZONES[z]); led_put(p, hex); }
    snprintf(p, sizeof p, "/sys/class/led_anim/effect_%s", LED_ZONES[z]); snprintf(v, sizeof v, "%d", fx); led_put(p, v);
}
static void led_save(void) {
    for (int z = 0; z < 4; z++) {
        char p[96], t[16] = "";
        snprintf(p, sizeof p, "/sys/class/led_anim/effect_rgb_hex_%s", LED_ZONES[z]);
        if (!led_read(p, led_saved_hex[z], sizeof led_saved_hex[z])) snprintf(led_saved_hex[z], sizeof led_saved_hex[z], "FFFFFF");
        snprintf(p, sizeof p, "/sys/class/led_anim/effect_%s", LED_ZONES[z]);
        led_saved_fx[z] = led_read(p, t, sizeof t) ? atoi(t) : 4;
    }
}
static void led_restore(void) {
    if (!led_cur) return;
    for (int z = 0; z < 4; z++) led_zone(z, led_saved_hex[z], led_saved_fx[z]);
    led_cur = 0;
}
static int led_stock_switch(void) {                   /* the OS's own "LEDs on/off" setting */
    FILE *f = fopen("/mnt/UDISK/system.json", "r"); if (!f) return 1;
    char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    char *p = strstr(buf, "\"ledswitch\""); if (!p) return 1;
    p = strchr(p, ':'); return p ? atoi(p + 1) != 0 : 1;
}
static void led_tick(void) {
    static Uint32 last_ms, json_ms; static int stock_on = 1; static uint32_t last_rgb = 0xFFFFFFFFu;
    Uint32 now = SDL_GetTicks();
    if (!json_ms || now - json_ms > 5000) { json_ms = now | 1; stock_on = led_stock_switch(); }
    int low = bat_pct >= 0 && bat_pct <= 10 && !bat_chg;               /* leave the stock low-battery warning alone */
    int want = (!led_on_cfg || !stock_on || bg || low) ? 0 : (screen_off && led_screen_off ? 2 : 1);
    if (want != led_cur) {
        if (want == 0) led_restore();
        else {
            if (led_cur == 0) led_save();
            if (want == 2) for (int z = 0; z < 4; z++) led_zone(z, NULL, 0);
            led_cur = want; last_rgb = 0xFFFFFFFFu;
        }
    }
    if (led_cur == 1 && now - last_ms >= 80) {
        last_ms = now;
        int r = (int)gcol[0], g = (int)gcol[1], b = (int)gcol[2];
        if (last_rgb != 0xFFFFFFFFu) {                                   /* skip tiny changes */
            int dr = r - (int)((last_rgb >> 16) & 255), dg = g - (int)((last_rgb >> 8) & 255), db = b - (int)(last_rgb & 255);
            if (abs(dr) < 4 && abs(dg) < 4 && abs(db) < 4) return;
        }
        char hex[8]; snprintf(hex, sizeof hex, "%02X%02X%02X", r & 255, g & 255, b & 255);
        for (int z = 0; z < 4; z++) led_zone(z, hex, 4);
        last_rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

/* ---- resume.cfg ---- */
static void resume_save(void) {
    if (!sn.valid || sn.ended || sn.song < 0 || sn.song >= g_nsongs) return;
    FILE *f = fopen("resume.cfg.tmp", "w");
    if (!f) return;
    double pos = sn.dur > 0 && sn.elapsed > sn.dur - 3.0 ? 0.0 : sn.elapsed;      /* nearly finished: start that song over */
    fprintf(f, "kind=%d\nshuffle=%d\nrepeat=%d\npos=%.1f\npath=%s\n", q_kind ? q_kind : 1, sn.shuffle, sn.repeat, pos, g_songs[sn.song].path);
    fclose(f);
    rename("resume.cfg.tmp", "resume.cfg");
}
static void resume_load(void) {
    FILE *f = fopen("resume.cfg", "r");
    if (!f) return;
    char line[600]; int got = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "kind=", 5)) rs_kind = atoi(line + 5);
        else if (!strncmp(line, "shuffle=", 8)) rs_shuffle = atoi(line + 8);
        else if (!strncmp(line, "repeat=", 7)) rs_repeat = atoi(line + 7);
        else if (!strncmp(line, "pos=", 4)) rs_pos = atof(line + 4);
        else if (!strncmp(line, "path=", 5)) { snprintf(rs_path, sizeof rs_path, "%s", line + 5); got = 1; }
    }
    fclose(f);
    rs_valid = got;
}
static void resume_tick(void) {                       /* save on song/pause changes, and every few seconds while playing */
    static Uint32 last_ms; static int last_song = -1, last_paused = -1; static double last_pos = -100;
    if (!sn.valid) return;
    Uint32 now = SDL_GetTicks();
    int forced = sn.song != last_song || sn.paused != last_paused;
    if (!forced && (now - last_ms < 6000 || fabs(sn.elapsed - last_pos) < 1.0)) return;
    last_ms = now; last_song = sn.song; last_paused = sn.paused; last_pos = sn.elapsed;
    resume_save();
}

static void on_sig(int s) { (void)s; want_quit = 1; }

static void scan_cb(int n) {
    if (n % 20) return;
    SDL_PumpEvents();
    SDL_SetRenderDrawColor(ren, 18, 18, 22, 255); SDL_RenderClear(ren);
    char b[64]; snprintf(b, sizeof b, "Scanning library... %d", n);
    draw_text(F_L, b, W / 2, H / 2 - S(20), 0, 1, WHITE);
    SDL_RenderPresent(ren);
}

/* ---------- hotkey thread: reads the controller's /dev/input/js0 without grabbing it ---------- */
struct jsev { uint32_t time; int16_t value; uint8_t type, number; };

static void osd_toast(const char *text) {
    if (!toast_on) return;
    char clean[80]; size_t n = 0;
    for (const char *p = text; *p && n < 60; p++) {                     /* keep UTF-8, drop shell metacharacters */
        if (strchr("'\"\\$`;&|<>\n\r", *p)) continue;
        clean[n++] = *p;
    }
    while (n > 0 && (((unsigned char)clean[n]) & 0xC0) == 0x80 && n < 60) n--;   /* don't cut a character in half */
    clean[n] = 0;
    const char *script = n <= 12 ? "short" : n <= 30 ? "" : n <= 45 ? "long" : "extra_long";
    char cmd[300];
    snprintf(cmd, sizeof cmd, "/usr/trimui/osd/show_default_msg%s%s.sh '%s' >/dev/null 2>&1 &",
             script[0] ? "_" : "", script, clean);
    if (system(cmd)) {}
}

static void fire_hotkey(int action) {
    if (!bg) return;                          /* while the UI is visible, normal buttons apply */
    Snap s;
    if (action == HK_PAUSE) { engine_toggle_pause(); engine_snapshot(&s); osd_toast(s.paused ? "Paused" : "Playing"); }
    else {
        if (action == HK_NEXT) engine_next(); else engine_prev();
        SDL_Delay(300);                       /* let the new track start so we can name it */
        engine_snapshot(&s);
        if (s.valid) osd_toast(g_songs[s.song].title);
    }
}

static void *hotkey_main(void *a) {
    (void)a;
    int fd = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;
    unsigned down = 0;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 50) > 0) {
            struct jsev e;
            while (read(fd, &e, sizeof e) == (ssize_t)sizeof e) {
                if ((e.type & 0x7f) != 1) continue;                     /* buttons only */
                if (dbg) { FILE *f = fopen("input.log", "a"); if (f) { fprintf(f, "js button=%d value=%d\n", e.number, e.value); fclose(f); } }
                for (int i = BT_A; i < BT_COUNT; i++)
                    if (jsmap[i] == e.number) { if (e.value) down |= 1u << i; else down &= ~(1u << i); }
                for (int k = 0; k < HK_N; k++) {
                    Hotkey *h = &hks[k];
                    if (h->mask && down == h->mask) {
                        if (!h->armed) { h->armed = 1; h->fired = 0; h->t0 = SDL_GetTicks();
                                         if (h->hold == 0) { h->fired = 1; fire_hotkey(k); } }
                    } else h->armed = h->fired = 0;
                }
            }
        }
        for (int k = 0; k < HK_N; k++) {                                 /* long-press hotkeys */
            Hotkey *h = &hks[k];
            if (h->armed && !h->fired && h->hold > 0 && SDL_GetTicks() - h->t0 >= (Uint32)h->hold) { h->fired = 1; fire_hotkey(k); }
        }
    }
    return NULL;
}

static void cleanup(void) {
    dl_shutdown();
    resume_save();
    led_restore();
    if (usb_on) { if (system("sh ./usbmusic.sh stop >> app.log 2>&1")) {} }
    if (screen_off) { panel_power(1); brightness_restore(); }
    pthread_mutex_lock(&lmu); lquit = 1; pthread_cond_signal(&lcv); pthread_mutex_unlock(&lmu);
    engine_shutdown();
    pthread_join(loader_thr, NULL);
    gfx_shutdown();
    TTF_Quit(); SDL_Quit();
    unlink(PID_PATH); unlink(CTL_PATH); unlink(UI_PATH);
}

/* ============================ main ============================ */
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--screentest")) {        /* ./vibe --screentest : try each way of turning the screen off */
        load_buttons();
        setvbuf(stdout, NULL, _IONBF, 0);
        printf("1/4 brightness through /dev/disp (what the stock OS does): dark for 4 s\n");
        { int v = disp_get_brightness(); printf("   current brightness: %d\n", v); disp_set_brightness(0); sleep(4); disp_set_brightness(v > 0 ? v : bl_default); sleep(3); }
        printf("2/4 backlight ioctl: dark for 4 s\n"); disp_backlight(0); sleep(4); disp_backlight(1); sleep(3);
        printf("3/4 fb0 blank: dark for 4 s\n");
        { FILE *f = fopen("/sys/class/graphics/fb0/blank", "w"); if (f) { fprintf(f, "4"); fclose(f); } else printf("   (no /sys/class/graphics/fb0/blank)\n"); }
        sleep(4);
        { FILE *f = fopen("/sys/class/graphics/fb0/blank", "w"); if (f) { fprintf(f, "0"); fclose(f); } }
        sleep(3);
        printf("4/4 sysfs lcdbl brightness 0: note whether it is fully dark\n"); bl_write(0); sleep(4); bl_write(bl_default);
        printf("done\n");
        return 0;
    }
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGPIPE, SIG_IGN);
    dbg = getenv("MP_DEBUG") != NULL || access("debug", F_OK) == 0;
    lib_init();
    load_buttons();
    themes_init(); settings_load();
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER) < 0) return 1;
    TTF_Init();
    ev_base = SDL_RegisterEvents(1);

    FILE *pf = fopen(PID_PATH, "w"); if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    unlink(CTL_PATH); mkfifo(CTL_PATH, 0666);
    pthread_t ct; pthread_create(&ct, NULL, ctl_main, NULL); pthread_detach(ct);
    pthread_t ht; pthread_create(&ht, NULL, hotkey_main, NULL); pthread_detach(ht);

    if (gfx_init() < 0) { fprintf(stderr, "video init failed: %s\n", SDL_GetError()); unlink(PID_PATH); unlink(UI_PATH); return 1; }

    mkdir("cache", 0755);
    const char *root = getenv("MUSIC_DIR"); if (!root) root = DEFAULT_MUSIC;
    music_root = root;
    { char cwd[400]; if (getcwd(cwd, sizeof cwd)) dl_init(cwd, music_root); }
    lib_scan(root, "cache/library2.tsv", scan_cb);
    resume_load();
    pl_load();

    if (g_nsongs == 0) {
        char m[600]; snprintf(m, sizeof m, "No music found in %s", root);
        for (int done = 0; !done && !want_quit;) {
            SDL_SetRenderDrawColor(ren, 18, 18, 22, 255); SDL_RenderClear(ren);
            draw_text(F_L, m, W / 2, H / 2 - S(30), W - S(40), 1, WHITE);
            draw_text(F_M, "Supported: MP3 FLAC OPUS OGG M4A WAV. Press any button to exit.", W / 2, H / 2 + S(20), W - S(40), 1, GRAY);
            SDL_RenderPresent(ren);
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, 1000) && (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_KEYDOWN || e.type == SDL_QUIT)) done = 1;
        }
        cleanup(); return 0;
    }

    thumbs_realloc();
    browse_build();
    reset_roots();
    vs[0] = saved_root[0];
    if (engine_init() < 0) fprintf(stderr, "audio init failed: %s\n", SDL_GetError());
    alog("==== start: %s", engine_audio_info());
    pthread_create(&loader_thr, NULL, loader_main, NULL);

    Uint32 last_draw = 0;
    while (!want_quit) {
        int to = bg ? 2000 : screen_off ? 5000 : (any_held() || anim) ? 16 :
                 (inflight > 0 || jq_n > 0) ? 20 : (on_dl() && dl_busy()) ? 200 : (scr == SCR_NOW && !sn.paused) ? (vis_mode ? vis_ms / 2 : 250) : 1000;
        SDL_Event e;
        int got = SDL_WaitEventTimeout(&e, to);
        while (got) { process_event(&e); got = SDL_PollEvent(&e); }
        Uint32 now = SDL_GetTicks();
        for (int d = 0; d < 4; d++) if (held[d] && now >= nextfire[d]) { nextfire[d] = now + 70; press(d); }
        if (loader_poll() && !bg) dirty = 1;
        { static time_t lm; time_t nm = time(NULL) / 60; if (nm != lm) { lm = nm; dirty = 1; } }
        engine_snapshot(&sn);
        { static Uint32 last_diag; char dl[200], cs[96];
          if (audio_log && now - last_diag >= 1000) { last_diag = now;
              if (engine_diag(dl, sizeof dl)) { cpu_state(cs, sizeof cs); alog("%s | screen=%s ui=%s idle=%us | %s", dl, screen_off ? "off" : "on", bg ? "background" : scr == SCR_NOW ? "now-playing" : "browse", (unsigned)((now - last_input) / 1000), cs); } } }
        led_tick();
        if (sn.valid) rs_valid = 0;                    /* something else is playing now: the old resume point no longer applies */
        resume_tick();
        if (bg) { if (sn.ended) break; continue; }   /* playlist finished while hidden: free the CPU */
        if (g_track_changed) { g_track_changed = 0; dirty = 1; if (sn.valid) alog("track: %s", g_songs[sn.song].path); }
        if (scr == SCR_NOW && !sn.paused && now - last_draw >= (Uint32)(vis_mode ? vis_ms : 250)) dirty = 1;
        if (dl_new_files) { dl_new_files = 0; usb_dirty = 1; }
        if (toast_msg[0] && now - toast_t > 2600) { toast_msg[0] = 0; dirty = 1; }
        if (on_dl() && now - last_draw >= 250 && (dl_busy() || dl_flash[0])) dirty = 1;
        if (!screen_off && dirty && ren) {
            frame_no++;
            grid_layout();
            if (scr == SCR_GRID) draw_browse(); else if (scr == SCR_SEARCH) draw_search(); else if (scr == SCR_EQ) draw_eq(); else if (scr == SCR_TAGS) draw_tags(); else if (scr == SCR_PLNAME) draw_plname(); else draw_now();
            if (menu_open) draw_menu();
            draw_toast();
            SDL_RenderPresent(ren);
            last_draw = now; dirty = 0;
            if (anim) dirty = 1;
        }
    }
    cleanup();
    return 0;
}

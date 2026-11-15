#define _GNU_SOURCE
#include "browse.h"
#include "library.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

int *g_sorted;
Album *g_albums;   int g_nalbums;
Artist *g_artists; int g_nartists;

typedef struct { int idx; char key[160]; } K;
static int cmpk(const void *a, const void *b) {
    const K *x = a, *y = b;
    int c = strcasecmp(x->key, y->key);
    return c ? c : x->idx - y->idx;
}

/* Album tag, or the song's parent folder name when untagged. */
static void album_name(const Song *s, char *out, size_t n) {
    if (s->album[0]) { snprintf(out, n, "%s", s->album); return; }
    const char *e = strrchr(s->path, '/');
    if (!e) { snprintf(out, n, "Unknown album"); return; }
    const char *q = e;
    while (q > s->path && q[-1] != '/') q--;
    snprintf(out, n, "%.*s", (int)(e - q), q);
    if (!out[0]) snprintf(out, n, "Unknown album");
}

static int cmp_title(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    int r = strcasecmp(g_songs[x].title, g_songs[y].title);
    return r ? r : x - y;
}

/* ---------------- artist credits ---------------- */
typedef struct { char key[160]; char disp[128]; int album; } Pair;
static Pair *pairs; static int npairs, cappairs;
static char (*known)[160]; static int nknown;

static int cmp_str160(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static int cmp_pair(const void *a, const void *b) {
    const Pair *x = a, *y = b; int c = strcmp(x->key, y->key);
    if (c) return c;
    c = strcmp(x->disp, y->disp);
    return c ? c : x->album - y->album;
}
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int cmp_artist(const void *a, const void *b) { return strcasecmp(((const Artist *)a)->name, ((const Artist *)b)->name); }

/* Lower-cases, drops punctuation and sorts the words, so word order and punctuation don't matter. */
static void norm_key(const char *s, char *key, size_t n) {
    char buf[256]; size_t k = 0;
    for (; *s && k < sizeof buf - 1; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x80 || isalnum(c)) buf[k++] = (char)tolower(c);
        else if (c == '+') buf[k++] = '+';
        else buf[k++] = ' ';
    }
    buf[k] = 0;
    char *tok[24]; int nt = 0;
    for (char *p = strtok(buf, " "); p && nt < 24; p = strtok(NULL, " ")) tok[nt++] = p;
    for (int i = 1; i < nt; i++) for (int j = i; j > 0 && strcmp(tok[j - 1], tok[j]) > 0; j--) { char *t = tok[j]; tok[j] = tok[j - 1]; tok[j - 1] = t; }
    key[0] = 0;
    for (int i = 0; i < nt; i++) { if (i) strncat(key, " ", n - strlen(key) - 1); strncat(key, tok[i], n - strlen(key) - 1); }
}

/* Splits s at the separators in `seps` (matched case-insensitively), writing trimmed names. Returns the count. */
static int split_at(const char *s, const char *const *seps, char names[][128], int max) {
    char tmp[256]; size_t k = 0; int prev_space = 0;
    for (; *s && k < sizeof tmp - 1; s++) {                       /* brackets become spaces; runs of spaces collapse */
        char c = (*s == '(' || *s == ')' || *s == '[' || *s == ']') ? ' ' : *s;
        if (c == ' ') { if (prev_space) continue; prev_space = 1; } else prev_space = 0;
        tmp[k++] = c;
    }
    tmp[k] = 0;
    int n = 0; size_t start = 0;
    for (size_t i = 0; i <= k; i++) {
        size_t sl = 0;
        if (i < k) for (int q = 0; seps[q]; q++) { size_t l = strlen(seps[q]); if (!strncasecmp(tmp + i, seps[q], l)) { sl = l; break; } }
        if (i == k || sl) {
            size_t a = start, e = i;
            while (a < e && tmp[a] == ' ') a++;
            while (e > a && tmp[e - 1] == ' ') e--;
            if (e > a && n < max) { snprintf(names[n], 128, "%.*s", (int)(e - a), tmp + a); n++; }
            if (sl) { i += sl - 1; start = i + 1; }
        }
    }
    return n;
}

static const char *const STRONG[] = { " x ", " \xC3\x97 ", " feat. ", " feat ", " ft. ", " ft ", " featuring ", ";", NULL };
static const char *const WEAK[]   = { " & ", ", ", ",", " and ", "/", NULL };
static const char *const ALLSEP[] = { " x ", " \xC3\x97 ", " feat. ", " feat ", " ft. ", " ft ", " featuring ", ";", " & ", ", ", ",", " and ", "/", "(", ")", NULL };

static int is_known(const char *name) {
    char key[160]; norm_key(name, key, sizeof key);
    return nknown && bsearch(key, known, (size_t)nknown, 160, cmp_str160) != NULL;
}

/* The artists credited by one credit string. Words like "A x B" always split; "A & B", "A, B", "A/B" only when
   one of the names is an artist that appears alone elsewhere in the library (so "Simon & Garfunkel" stays whole). */
static int credit_names(const char *credit, char names[][128], int max) {
    char part[8][128]; int np = split_at(credit, STRONG, part, 8), n = 0;
    for (int i = 0; i < np; i++) {
        char sub[8][128]; int ns = split_at(part[i], WEAK, sub, 8), any = 0;
        for (int j = 0; j < ns && ns > 1; j++) if (is_known(sub[j])) any = 1;
        if (ns > 1 && any) for (int j = 0; j < ns && n < max; j++) snprintf(names[n++], 128, "%s", sub[j]);
        else if (n < max) snprintf(names[n++], 128, "%s", part[i]);
    }
    return n;
}

static void add_pair(const char *name, int album) {
    char key[160]; norm_key(name, key, sizeof key);
    if (!key[0]) return;
    for (int i = npairs - 1; i >= 0 && pairs[i].album == album; i--) if (!strcmp(pairs[i].key, key)) return;   /* once per album */
    if (npairs == cappairs) { cappairs = cappairs ? cappairs * 2 : 1024; pairs = realloc(pairs, (size_t)cappairs * sizeof(Pair)); }
    Pair *p = &pairs[npairs++];
    snprintf(p->key, sizeof p->key, "%s", key); snprintf(p->disp, sizeof p->disp, "%s", name); p->album = album;
}

static void collect_known(const char *s) {
    char tmp[2][128];
    if (!s[0] || split_at(s, ALLSEP, tmp, 2) != 1) return;           /* only credits with no separator at all */
    char key[160]; norm_key(s, key, sizeof key);
    if (!key[0]) return;
    known = realloc(known, (size_t)(nknown + 1) * 160);
    snprintf(known[nknown++], 160, "%s", key);
}

static void build_artists(void) {
    npairs = 0; nknown = 0;
    for (int i = 0; i < g_nsongs; i++) { collect_known(g_songs[i].artist); collect_known(g_songs[i].albumartist); }
    qsort(known, (size_t)nknown, 160, cmp_str160);
    for (int a = 0; a < g_nalbums; a++) {
        char names[16][128]; int nn;
        nn = credit_names(g_albums[a].artist, names, 16);
        for (int i = 0; i < nn; i++) add_pair(names[i], a);
        for (int t = 0; t < g_albums[a].n; t++) {
            nn = credit_names(g_songs[g_albums[a].songs[t]].artist, names, 16);
            for (int i = 0; i < nn; i++) add_pair(names[i], a);
        }
    }
    qsort(pairs, (size_t)npairs, sizeof(Pair), cmp_pair);
    g_artists = calloc(npairs ? (size_t)npairs : 1, sizeof(Artist)); g_nartists = 0;
    for (int i = 0; i < npairs;) {
        int j = i; while (j < npairs && !strcmp(pairs[j].key, pairs[i].key)) j++;
        /* the name used most often becomes the display name */
        int best = i, bestn = 0;
        for (int x = i; x < j;) { int y = x; while (y < j && !strcmp(pairs[y].disp, pairs[x].disp)) y++; if (y - x > bestn) { bestn = y - x; best = x; } x = y; }
        Artist *r = &g_artists[g_nartists++];
        snprintf(r->name, sizeof r->name, "%s", pairs[best].disp);
        r->albums = malloc((size_t)(j - i) * sizeof(int)); r->nalb = 0;
        for (int x = i; x < j; x++) r->albums[r->nalb++] = pairs[x].album;
        qsort(r->albums, (size_t)r->nalb, sizeof(int), cmp_int);
        int w = 0; for (int x = 0; x < r->nalb; x++) if (x == 0 || r->albums[x] != r->albums[x - 1]) r->albums[w++] = r->albums[x];
        r->nalb = w;
        for (int x = 0; x < r->nalb; x++) r->nsongs += g_albums[r->albums[x]].n;
        i = j;
    }
    qsort(g_artists, (size_t)g_nartists, sizeof(Artist), cmp_artist);
    free(pairs); pairs = NULL; npairs = cappairs = 0;
    free(known); known = NULL; nknown = 0;
}

void browse_build(void) {
    g_sorted = malloc((g_nsongs ? g_nsongs : 1) * sizeof(int));
    for (int i = 0; i < g_nsongs; i++) g_sorted[i] = i;
    qsort(g_sorted, g_nsongs, sizeof(int), cmp_title);

    int n = g_nsongs;
    K *k = malloc((n ? n : 1) * sizeof(K));
    char tmp[128];
    for (int i = 0; i < n; i++) { k[i].idx = i; album_name(&g_songs[i], tmp, sizeof tmp); snprintf(k[i].key, sizeof k[i].key, "%s", tmp); }
    qsort(k, n, sizeof(K), cmpk);

    g_albums = calloc(n ? n : 1, sizeof(Album));
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && !strcasecmp(k[j].key, k[i].key)) j++;
        Album *a = &g_albums[g_nalbums++];
        snprintf(a->name, sizeof a->name, "%s", k[i].key);
        a->n = j - i; a->songs = malloc((size_t)a->n * sizeof(int));
        for (int t = 0; t < a->n; t++) a->songs[t] = k[i + t].idx;
        const Song *s0 = &g_songs[a->songs[0]];
        if (s0->albumartist[0]) snprintf(a->artist, sizeof a->artist, "%s", s0->albumartist);
        else {
            int various = 0;
            for (int t = 1; t < a->n; t++) if (strcasecmp(g_songs[a->songs[t]].artist, s0->artist)) { various = 1; break; }
            snprintf(a->artist, sizeof a->artist, "%s", various ? "Various artists" : s0->artist);
        }
        if (!a->artist[0]) snprintf(a->artist, sizeof a->artist, "Unknown artist");
        i = j;
    }
    free(k);

    /* artists: every name credited on an album's songs gets the album. A credit such as "A x B" or "A feat. B"
       is split into its names, and names that differ only in word order ("Kiara Takanashi" / "Takanashi Kiara")
       are one artist. */
    build_artists();
}

void browse_free(void) {
    for (int i = 0; i < g_nalbums; i++) free(g_albums[i].songs);
    for (int i = 0; i < g_nartists; i++) free(g_artists[i].albums);
    free(g_albums); free(g_artists); free(g_sorted);
    g_albums = NULL; g_artists = NULL; g_sorted = NULL; g_nalbums = g_nartists = 0;
}

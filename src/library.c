#define _GNU_SOURCE
#include "library.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/avutil.h>
#include <math.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

Song *g_songs;
int   g_nsongs;

typedef struct { Song s; long long mt, sz; } Rec;
static Rec *recs;  static int nrecs, caprecs;
static Rec *cache; static int ncache;
static int s_dirty;
static ScanCB s_cb;
static size_t rootlen;

void lib_init(void) { av_log_set_level(AV_LOG_QUIET); }

/* ---------- helpers ---------- */
static void cpy(char *d, size_t n, const char *s) {
    snprintf(d, n, "%s", s ? s : "");
    for (; *d; d++) if (*d == '\t' || *d == '\n' || *d == '\r') *d = ' ';
}

static const char *tagget(AVFormatContext *fc, const char *k) {
    AVDictionaryEntry *e = av_dict_get(fc->metadata, k, NULL, 0);
    if (e && e->value[0]) return e->value;
    for (unsigned i = 0; i < fc->nb_streams; i++) {   /* ogg/opus keep tags per stream */
        e = av_dict_get(fc->streams[i]->metadata, k, NULL, 0);
        if (e && e->value[0]) return e->value;
    }
    return NULL;
}

static void read_tags(const char *path, Song *s) {
    AVFormatContext *fc = NULL;
    snprintf(s->path, sizeof s->path, "%s", path);
    s->title[0] = s->artist[0] = s->album[0] = s->albumartist[0] = 0;
    if (avformat_open_input(&fc, path, NULL, NULL) == 0) {
        const char *aa = tagget(fc, "album_artist");
        if (!aa) aa = tagget(fc, "albumartist");
        const char *a = tagget(fc, "artist");
        if (!a) a = aa;
        cpy(s->albumartist, sizeof s->albumartist, aa);
        cpy(s->title,  sizeof s->title,  tagget(fc, "title"));
        cpy(s->artist, sizeof s->artist, a);
        cpy(s->album,  sizeof s->album,  tagget(fc, "album"));
        avformat_close_input(&fc);
    }
    if (!s->title[0]) {
        const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
        cpy(s->title, sizeof s->title, b);
        char *d = strrchr(s->title, '.'); if (d) *d = 0;
    }
}

/* Natural, case-insensitive compare; '/' sorts lowest so folders group together. */
static int natcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0' && isdigit((unsigned char)a[1])) a++;
            while (*b == '0' && isdigit((unsigned char)b[1])) b++;
            const char *pa = a, *pb = b;
            while (isdigit((unsigned char)*pa)) pa++;
            while (isdigit((unsigned char)*pb)) pb++;
            long la = pa - a, lb = pb - b;
            if (la != lb) return la < lb ? -1 : 1;
            int c = strncmp(a, b, la);
            if (c) return c < 0 ? -1 : 1;
            a = pa; b = pb;
        } else {
            int ca = *a == '/' ? 1 : tolower((unsigned char)*a);
            int cb = *b == '/' ? 1 : tolower((unsigned char)*b);
            if (ca != cb) return ca < cb ? -1 : 1;
            a++; b++;
        }
    }
    return (*a ? 1 : 0) - (*b ? 1 : 0);
}
static int cmp_nat(const void *a, const void *b) {
    return natcmp(((const Rec *)a)->s.path + rootlen, ((const Rec *)b)->s.path + rootlen);
}
static int cmp_path(const void *a, const void *b) {
    return strcmp(((const Rec *)a)->s.path, ((const Rec *)b)->s.path);
}

static int ext_ok(const char *name) {
    static const char *e[] = { "mp3", "flac", "opus", "ogg", "m4a", "wav", NULL };
    const char *d = strrchr(name, '.');
    if (!d) return 0;
    for (int i = 0; e[i]; i++) if (!strcasecmp(d + 1, e[i])) return 1;
    return 0;
}

/* ---------- cache ---------- */
static void cache_load(const char *file) {
    FILE *fp = fopen(file, "r");
    if (!fp) return;
    static char line[2048];
    int cap = 0;
    while (fgets(line, sizeof line, fp)) {
        char *fld[7], *p = line; int n = 0;
        line[strcspn(line, "\r\n")] = 0;
        while (n < 7) {
            fld[n++] = p;
            char *t = strchr(p, '\t');
            if (!t) break;
            *t = 0; p = t + 1;
        }
        if (n < 7) continue;
        if (ncache == cap) { cap = cap ? cap * 2 : 1024; cache = realloc(cache, cap * sizeof(Rec)); }
        Rec *c = &cache[ncache++];
        memset(c, 0, sizeof *c);
        snprintf(c->s.path,   sizeof c->s.path,   "%s", fld[0]);
        c->mt = atoll(fld[1]); c->sz = atoll(fld[2]);
        snprintf(c->s.title,  sizeof c->s.title,  "%s", fld[3]);
        snprintf(c->s.artist, sizeof c->s.artist, "%s", fld[4]);
        snprintf(c->s.album,  sizeof c->s.album,  "%s", fld[5]);
        snprintf(c->s.albumartist, sizeof c->s.albumartist, "%s", fld[6]);
    }
    fclose(fp);
    qsort(cache, ncache, sizeof(Rec), cmp_path);
}

static void cache_save(const char *file) {
    char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp", file);
    FILE *fp = fopen(tmp, "w");
    if (!fp) return;
    for (int i = 0; i < nrecs; i++) {
        Rec *r = &recs[i];
        fprintf(fp, "%s\t%lld\t%lld\t%s\t%s\t%s\t%s\n", r->s.path, r->mt, r->sz,
                r->s.title, r->s.artist, r->s.album, r->s.albumartist);
    }
    fclose(fp);
    rename(tmp, file);
}

/* ---------- scan ---------- */
static void add_rec(const char *path, const struct stat *st) {
    if (nrecs == caprecs) { caprecs = caprecs ? caprecs * 2 : 1024; recs = realloc(recs, caprecs * sizeof(Rec)); }
    Rec *r = &recs[nrecs];
    memset(r, 0, sizeof *r);
    r->mt = st->st_mtime; r->sz = st->st_size;
    Rec *hit = NULL;
    if (ncache) {
        static Rec key;
        snprintf(key.s.path, sizeof key.s.path, "%s", path);
        hit = bsearch(&key, cache, ncache, sizeof(Rec), cmp_path);
    }
    if (hit && hit->mt == r->mt && hit->sz == r->sz) r->s = hit->s;
    else { read_tags(path, &r->s); s_dirty = 1; }
    nrecs++;
    if (s_cb) s_cb(nrecs);
}

static void scan_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;           /* skips ._AppleDouble junk too */
        char p[512];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) scan_dir(p);
        else if (S_ISREG(st.st_mode) && ext_ok(e->d_name)) add_rec(p, &st);
    }
    closedir(d);
}

int lib_scan(const char *root, const char *cache_file, ScanCB cb) {
    s_cb = cb;
    nrecs = 0; caprecs = 0; ncache = 0; s_dirty = 0;     /* allow rescans */
    cache_load(cache_file);
    char r[512]; snprintf(r, sizeof r, "%s", root);
    size_t n = strlen(r); while (n > 1 && r[n - 1] == '/') r[--n] = 0;
    rootlen = n + 1;
    scan_dir(r);
    qsort(recs, nrecs, sizeof(Rec), cmp_nat);
    if (s_dirty || nrecs != ncache) cache_save(cache_file);
    g_songs = malloc((nrecs ? nrecs : 1) * sizeof(Song));
    for (int i = 0; i < nrecs; i++) g_songs[i] = recs[i].s;
    g_nsongs = nrecs;
    free(recs); recs = NULL; free(cache); cache = NULL;
    return g_nsongs;
}

/* ---------- embedded art ---------- */
static AVStream *find_pic(AVFormatContext *fc) {
    for (unsigned i = 0; i < fc->nb_streams; i++) {
        AVStream *st = fc->streams[i];
        if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) && st->attached_pic.size > 0) return st;
    }
    return NULL;
}

int art_extract(const char *path, int size, Img *out) {
    AVFormatContext *fc = NULL;
    AVCodecContext *cc = NULL;
    AVFrame *fr = NULL;
    struct SwsContext *sws = NULL;
    uint8_t *tmp = NULL, *px = NULL;
    int ret = -1, got = 0;

    if (avformat_open_input(&fc, path, NULL, NULL) < 0) return -1;
    AVStream *st = find_pic(fc);
    if (!st) { avformat_find_stream_info(fc, NULL); st = find_pic(fc); }
    if (!st) goto done;
    const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c) goto done;
    cc = avcodec_alloc_context3(c);
    if (!cc) goto done;
    avcodec_parameters_to_context(cc, st->codecpar);
    if (avcodec_open2(cc, c, NULL) < 0) goto done;
    fr = av_frame_alloc();
    if (avcodec_send_packet(cc, &st->attached_pic) >= 0) {
        got = avcodec_receive_frame(cc, fr) >= 0;
        if (!got) { avcodec_send_packet(cc, NULL); got = avcodec_receive_frame(cc, fr) >= 0; }
    }
    if (!got || fr->width <= 0 || fr->height <= 0) goto done;

    int w = fr->width, h = fr->height, m = w < h ? w : h;
    double k = (double)size / m;
    int tw = (int)(w * k + 0.5), th = (int)(h * k + 0.5);
    if (tw < size) tw = size;
    if (th < size) th = size;
    sws = sws_getContext(w, h, (enum AVPixelFormat)fr->format, tw, th, AV_PIX_FMT_RGBA,
                         SWS_AREA, NULL, NULL, NULL);
    if (!sws) goto done;
    tmp = malloc((size_t)tw * th * 4);
    px  = malloc((size_t)size * size * 4);
    if (!tmp || !px) goto done;
    uint8_t *dst[4] = { tmp, NULL, NULL, NULL };
    int ls[4] = { tw * 4, 0, 0, 0 };
    sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, h, dst, ls);
    int x0 = (tw - size) / 2, y0 = (th - size) / 2;
    for (int y = 0; y < size; y++) {
        uint8_t *row = px + (size_t)y * size * 4;
        memcpy(row, tmp + ((size_t)(y + y0) * tw + x0) * 4, (size_t)size * 4);
        for (int x = 0; x < size; x++) row[x * 4 + 3] = 255;
    }
    out->px = px; out->size = size; px = NULL; ret = 0;
done:
    free(tmp); free(px);
    if (sws) sws_freeContext(sws);
    if (fr) av_frame_free(&fr);
    if (cc) avcodec_free_context(&cc);
    avformat_close_input(&fc);
    return ret;
}

/* ============================ tag editing ============================ */
static const char *first_tag(AVFormatContext *fc, const char *a, const char *b, const char *c) {
    const char *v = tagget(fc, a);
    if (!v && b) v = tagget(fc, b);
    if (!v && c) v = tagget(fc, c);
    return v;
}

int tags_read(const char *path, Tags *t) {
    memset(t, 0, sizeof *t);
    AVFormatContext *fc = NULL;
    if (avformat_open_input(&fc, path, NULL, NULL) < 0) return -1;
    snprintf(t->title,       sizeof t->title,       "%s", first_tag(fc, "title", NULL, NULL) ? first_tag(fc, "title", NULL, NULL) : "");
    snprintf(t->artist,      sizeof t->artist,      "%s", first_tag(fc, "artist", NULL, NULL) ? first_tag(fc, "artist", NULL, NULL) : "");
    snprintf(t->album,       sizeof t->album,       "%s", first_tag(fc, "album", NULL, NULL) ? first_tag(fc, "album", NULL, NULL) : "");
    snprintf(t->albumartist, sizeof t->albumartist, "%s", first_tag(fc, "album_artist", "albumartist", NULL) ? first_tag(fc, "album_artist", "albumartist", NULL) : "");
    snprintf(t->track,       sizeof t->track,       "%s", first_tag(fc, "track", "tracknumber", NULL) ? first_tag(fc, "track", "tracknumber", NULL) : "");
    snprintf(t->date,        sizeof t->date,        "%s", first_tag(fc, "date", "year", NULL) ? first_tag(fc, "date", "year", NULL) : "");
    snprintf(t->genre,       sizeof t->genre,       "%s", first_tag(fc, "genre", NULL, NULL) ? first_tag(fc, "genre", NULL, NULL) : "");
    avformat_close_input(&fc);
    return 0;
}

static const char *mux_for(const char *path) {
    const char *d = strrchr(path, '.');
    if (!d) return NULL;
    d++;
    if (!strcasecmp(d, "mp3")) return "mp3";
    if (!strcasecmp(d, "flac")) return "flac";
    if (!strcasecmp(d, "ogg")) return "ogg";
    if (!strcasecmp(d, "opus")) return "opus";
    if (!strcasecmp(d, "m4a")) return "ipod";
    if (!strcasecmp(d, "wav")) return "wav";
    return NULL;
}
static void md_put(AVDictionary **md, const char *key, const char *val) { av_dict_set(md, key, val && val[0] ? val : NULL, 0); }

int tags_write(const char *path, const Tags *t, char *err, size_t en) {
    AVFormatContext *ic = NULL, *oc = NULL, *vc = NULL;
    AVPacket *pkt = NULL; AVDictionary *opts = NULL;
    int *map = NULL, rc = -1, wrote_header = 0, first_audio = -1, n_out = 0;
    double in_dur = 0;
    char tmp[600], bak[600];
    const char *fmt = mux_for(path);
    if (!fmt) { snprintf(err, en, "This file type can't be tagged"); return -1; }
    const char *ext = strrchr(path, '.'), *sl = strrchr(path, '/');
    int dirlen = sl ? (int)(sl - path + 1) : 0;
    snprintf(tmp, sizeof tmp, "%.*s.vibe_edit%s", dirlen, path, ext);       /* hidden, so the library scan ignores it */
    snprintf(bak, sizeof bak, "%.*s.vibe_bak%s", dirlen, path, ext);

    if (avformat_open_input(&ic, path, NULL, NULL) < 0 || avformat_find_stream_info(ic, NULL) < 0) { snprintf(err, en, "Can't read the file"); goto end; }
    in_dur = ic->duration > 0 ? ic->duration / (double)AV_TIME_BASE : 0;
    if (avformat_alloc_output_context2(&oc, NULL, fmt, tmp) < 0 || !oc) { snprintf(err, en, "This build can't write %s files", fmt); goto end; }
    map = av_malloc_array(ic->nb_streams ? ic->nb_streams : 1, sizeof(int));
    if (!map) { snprintf(err, en, "Out of memory"); goto end; }

    const AVStream *in_audio = NULL;
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *is = ic->streams[i];
        int pic = (is->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
        map[i] = -1;
        if (is->codecpar->codec_type != AVMEDIA_TYPE_AUDIO && !pic) continue;
        if (!pic && first_audio >= 0) continue;                              /* one audio stream is all a music file has */
        AVStream *os = avformat_new_stream(oc, NULL);
        if (!os || avcodec_parameters_copy(os->codecpar, is->codecpar) < 0) { snprintf(err, en, "Stream setup failed"); goto end; }
        os->codecpar->codec_tag = 0;
        os->time_base = is->time_base;
        if (pic) {
            os->disposition = AV_DISPOSITION_ATTACHED_PIC;
            if (av_packet_ref(&os->attached_pic, &is->attached_pic) < 0) { snprintf(err, en, "Cover copy failed"); goto end; }
        } else { first_audio = n_out; in_audio = is; }
        map[i] = (int)n_out++;
    }
    if (first_audio < 0) { snprintf(err, en, "No audio stream found"); goto end; }

    /* tags: everything already there, with our fields replaced (an empty field removes the tag) */
    av_dict_copy(&oc->metadata, ic->metadata, 0);
    if (in_audio) av_dict_copy(&oc->metadata, in_audio->metadata, AV_DICT_DONT_OVERWRITE);     /* ogg/opus keep tags per stream */
    int vorbis = !strcmp(fmt, "flac") || !strcmp(fmt, "ogg") || !strcmp(fmt, "opus");
    md_put(&oc->metadata, "title", t->title);
    md_put(&oc->metadata, "artist", t->artist);
    md_put(&oc->metadata, "album", t->album);
    md_put(&oc->metadata, "date", t->date);
    md_put(&oc->metadata, "genre", t->genre);
    if (vorbis) {                                                             /* the names players expect in Vorbis comments */
        md_put(&oc->metadata, "track", NULL); md_put(&oc->metadata, "album_artist", NULL);
        md_put(&oc->metadata, "TRACKNUMBER", t->track); md_put(&oc->metadata, "ALBUMARTIST", t->albumartist);
    } else {
        md_put(&oc->metadata, "track", t->track); md_put(&oc->metadata, "album_artist", t->albumartist);
    }
    if (!strcmp(fmt, "ogg") || !strcmp(fmt, "opus")) {                        /* the Ogg muxer writes comments from the stream */
        AVStream *as = oc->streams[first_audio];
        av_dict_free(&as->metadata);
        av_dict_copy(&as->metadata, oc->metadata, 0);
    }
    if (!strcmp(fmt, "mp3")) av_dict_set(&opts, "id3v2_version", "3", 0);

    if (!(oc->oformat->flags & AVFMT_NOFILE) && avio_open(&oc->pb, tmp, AVIO_FLAG_WRITE) < 0) { snprintf(err, en, "Can't write next to the song (SD card full?)"); goto end; }
    if (avformat_write_header(oc, &opts) < 0) { snprintf(err, en, "Couldn't start writing the tags"); goto end; }
    wrote_header = 1;

    pkt = av_packet_alloc();
    if (!pkt) { snprintf(err, en, "Out of memory"); goto end; }
    while (av_read_frame(ic, pkt) >= 0) {
        int oi = pkt->stream_index >= 0 && pkt->stream_index < (int)ic->nb_streams ? map[pkt->stream_index] : -1;
        if (oi < 0) { av_packet_unref(pkt); continue; }
        AVStream *is = ic->streams[pkt->stream_index], *os = oc->streams[oi];
        av_packet_rescale_ts(pkt, is->time_base, os->time_base);
        pkt->stream_index = oi; pkt->pos = -1;
        if (av_interleaved_write_frame(oc, pkt) < 0) { av_packet_unref(pkt); snprintf(err, en, "Write failed (SD card full?)"); goto end; }
    }
    if (av_write_trailer(oc) < 0) { snprintf(err, en, "Couldn't finish the file"); goto end; }
    wrote_header = 0;
    if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
    avformat_close_input(&ic);                                                 /* release the original before swapping */

    /* check the new file before it replaces anything */
    if (avformat_open_input(&vc, tmp, NULL, NULL) < 0 || avformat_find_stream_info(vc, NULL) < 0) { snprintf(err, en, "The new file didn't verify; original kept"); goto end; }
    int has_audio = 0;
    for (unsigned i = 0; i < vc->nb_streams; i++) if (vc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) has_audio = 1;
    double out_dur = vc->duration > 0 ? vc->duration / (double)AV_TIME_BASE : 0;
    avformat_close_input(&vc);
    if (!has_audio || (in_dur > 0 && out_dur > 0 && fabs(in_dur - out_dur) > 1.5)) { snprintf(err, en, "The new file didn't verify; original kept"); goto end; }

    if (rename(path, bak) != 0) { snprintf(err, en, "Couldn't replace the original"); goto end; }
    if (rename(tmp, path) != 0) { rename(bak, path); snprintf(err, en, "Couldn't replace the original"); goto end; }
    unlink(bak);
    rc = 0;
end:
    if (wrote_header && oc) av_write_trailer(oc);
    if (oc) { if (oc->pb && !(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb); avformat_free_context(oc); }
    if (ic) avformat_close_input(&ic);
    if (vc) avformat_close_input(&vc);
    if (pkt) av_packet_free(&pkt);
    av_dict_free(&opts);
    av_freep(&map);
    if (rc != 0) unlink(tmp);
    return rc;
}

/* ============================ image files (search-result covers) ============================ */
static int frame_to_square(AVFrame *fr, int size, Img *out) {
    int w = fr->width, h = fr->height, m = w < h ? w : h;
    if (w <= 0 || h <= 0) return -1;
    double k = (double)size / m;
    int tw = (int)(w * k + 0.5), th = (int)(h * k + 0.5);
    if (tw < size) tw = size;
    if (th < size) th = size;
    struct SwsContext *sws = sws_getContext(w, h, (enum AVPixelFormat)fr->format, tw, th, AV_PIX_FMT_RGBA, SWS_AREA, NULL, NULL, NULL);
    if (!sws) return -1;
    uint8_t *tmp = malloc((size_t)tw * th * 4), *px = malloc((size_t)size * size * 4);
    if (!tmp || !px) { free(tmp); free(px); sws_freeContext(sws); return -1; }
    uint8_t *dst[4] = { tmp, NULL, NULL, NULL };
    int ls[4] = { tw * 4, 0, 0, 0 };
    sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, h, dst, ls);
    int x0 = (tw - size) / 2, y0 = (th - size) / 2;
    for (int y = 0; y < size; y++) {
        uint8_t *row = px + (size_t)y * size * 4;
        memcpy(row, tmp + ((size_t)(y + y0) * tw + x0) * 4, (size_t)size * 4);
        for (int x = 0; x < size; x++) row[x * 4 + 3] = 255;
    }
    free(tmp); sws_freeContext(sws);
    out->px = px; out->size = size;
    return 0;
}

int img_load_file(const char *path, int size, Img *out) {
    AVFormatContext *fc = NULL; AVCodecContext *cc = NULL; AVFrame *fr = NULL; AVPacket *pk = NULL;
    const AVCodec *c = NULL; int ret = -1, got = 0, si;
    if (avformat_open_input(&fc, path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(fc, NULL) < 0) goto done;
    si = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, &c, 0);
    if (si < 0 || !c) goto done;
    cc = avcodec_alloc_context3(c);
    if (!cc) goto done;
    avcodec_parameters_to_context(cc, fc->streams[si]->codecpar);
    if (avcodec_open2(cc, c, NULL) < 0) goto done;
    pk = av_packet_alloc(); fr = av_frame_alloc();
    if (!pk || !fr) goto done;
    while (!got && av_read_frame(fc, pk) >= 0) {
        if (pk->stream_index == si && avcodec_send_packet(cc, pk) >= 0 && avcodec_receive_frame(cc, fr) >= 0) got = 1;
        av_packet_unref(pk);
    }
    if (!got) { avcodec_send_packet(cc, NULL); got = avcodec_receive_frame(cc, fr) >= 0; }
    if (got) ret = frame_to_square(fr, size, out);
done:
    if (pk) av_packet_free(&pk);
    if (fr) av_frame_free(&fr);
    if (cc) avcodec_free_context(&cc);
    if (fc) avformat_close_input(&fc);
    return ret;
}

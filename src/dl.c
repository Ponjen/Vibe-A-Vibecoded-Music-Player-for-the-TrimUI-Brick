/*
 * YouTube Music downloader: drives the standalone yt-dlp binary (bin/yt-dlp) with a static ffmpeg
 * (bin/ffmpeg, needs libmp3lame) and a QuickJS runtime (bin/qjs; current yt-dlp needs one for YouTube).
 * Songs are saved as 320 kbps MP3 with tags and square cover art to <Music>/Downloads.
 */
#define _GNU_SOURCE
#include "dl.h"
#include "library.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/statvfs.h>

extern char **environ;

volatile int dl_new_files;

static char app_dir[400], out_dir[460], bin_dir[420], cache_dir[420];
static char cacert_p[440], ytdlp[440], ffmpeg_p[440], qjs_p[440], qjs_opt[460], path_env[900];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static volatile pid_t cur_pid;            /* child being waited on by the download worker */
static int quitting;

static DlResult results[DL_MAXRES]; static int nres, search_state;
static DlJob jobs[DL_MAXJOB]; static int njobs, next_job, worker_started;
static pthread_t worker;
static char upd_msg[200]; static int upd_state;
static int busy_search, busy_update, search_gen, search_logged;

/* ---------------- helpers ---------------- */
static void dl_log(const char *fmt, ...) {            /* <app folder>/dl.log: what the downloader saw, for troubleshooting */
    char p[480]; snprintf(p, sizeof p, "%s/dl.log", app_dir);
    FILE *f = fopen(p, "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static int exists(const char *p) { return access(p, X_OK) == 0 || access(p, R_OK) == 0; }
int dl_available(void) { return exists(ytdlp) && exists(ffmpeg_p) && exists(qjs_p); }

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    char *p = s; while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}
static void urlenc(const char *s, char *o, size_t n) {
    size_t k = 0;
    for (; *s && k + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o[k++] = (char)c;
        else if (c == ' ') o[k++] = '+';
        else k += (size_t)snprintf(o + k, n - k, "%%%02X", c);
    }
    o[k] = 0;
}

/* Runs argv, calling cb for every output line (stdout+stderr). Returns the exit code, or -1. */
static int spawn_lines(const char *const *argv, void (*cb)(const char *line, void *ctx), void *ctx, volatile pid_t *pidp) {
    int pfd[2];
    if (pipe(pfd)) return -1;
    /* clean environment: no LD_LIBRARY_PATH (the stock libs must not shadow yt-dlp's bundled ones) */
    size_t ne = 0; while (environ[ne]) ne++;
    char **envp = malloc((ne + 6) * sizeof(char *)); size_t k = 0;
    for (size_t i = 0; i < ne; i++)
        if (strncmp(environ[i], "LD_LIBRARY_PATH=", 16) && strncmp(environ[i], "LD_PRELOAD=", 11) && strncmp(environ[i], "PATH=", 5) &&
            strncmp(environ[i], "TMPDIR=", 7) && strncmp(environ[i], "PYTHONHOME=", 11)) envp[k++] = environ[i];
    envp[k++] = path_env; envp[k++] = "TMPDIR=/tmp"; envp[k++] = "LANG=C.UTF-8"; envp[k] = NULL;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); free(envp); return -1; }
    if (pid == 0) {
        dup2(pfd[1], 1); dup2(pfd[1], 2);
        close(pfd[0]); close(pfd[1]);
        execve(argv[0], (char *const *)argv, envp);
        _exit(127);
    }
    close(pfd[1]);
    free(envp);
    if (pidp) *pidp = pid;
    FILE *f = fdopen(pfd[0], "r");
    char buf[1200];
    while (f && fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        cb(buf, ctx);
    }
    if (f) fclose(f);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (pidp) *pidp = 0;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

#define COMMON_ARGS(a, n) \
    a[n++] = ytdlp; a[n++] = "--ignore-config"; a[n++] = "--no-colors"; a[n++] = "--newline"; a[n++] = "--no-mtime"; \
    a[n++] = "--ffmpeg-location"; a[n++] = bin_dir; a[n++] = "--js-runtimes"; a[n++] = qjs_opt; \
    a[n++] = "--cache-dir"; a[n++] = cache_dir; a[n++] = "--socket-timeout"; a[n++] = "20"; a[n++] = "--retries"; a[n++] = "5";

/* ---------------- init ---------------- */
void dl_init(const char *app, const char *music_root) {
    snprintf(app_dir, sizeof app_dir, "%s", app);
    snprintf(bin_dir, sizeof bin_dir, "%s/bin", app);
    snprintf(cache_dir, sizeof cache_dir, "%s/cache/yt-dlp", app);
    snprintf(out_dir, sizeof out_dir, "%s/Downloads", music_root);
    snprintf(ytdlp, sizeof ytdlp, "%s/yt-dlp", bin_dir);
    snprintf(ffmpeg_p, sizeof ffmpeg_p, "%s/ffmpeg", bin_dir);
    snprintf(qjs_p, sizeof qjs_p, "%s/qjs", bin_dir);
    snprintf(cacert_p, sizeof cacert_p, "%s/cacert.pem", bin_dir);
    snprintf(qjs_opt, sizeof qjs_opt, "quickjs:%s", qjs_p);
    const char *op = getenv("PATH");
    snprintf(path_env, sizeof path_env, "PATH=%s:%s", bin_dir, op ? op : "/usr/bin:/bin");
    char tmp[480];
    snprintf(tmp, sizeof tmp, "%s/cache", app); mkdir(tmp, 0755);
    mkdir(cache_dir, 0755);
    chmod(ytdlp, 0755); chmod(ffmpeg_p, 0755); chmod(qjs_p, 0755);
    snprintf(tmp, sizeof tmp, "%s/ffprobe", bin_dir); chmod(tmp, 0755);
}

void dl_shutdown(void) {
    pthread_mutex_lock(&mu); quitting = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    pid_t p = cur_pid;
    if (p > 0) kill(p, SIGTERM);
}

/* ---------------- search ---------------- */
static void search_line(const char *line, void *ctx) {
    (void)ctx;
    char b[1200]; snprintf(b, sizeof b, "%s", line);
    char *f[5] = { b, NULL, NULL, NULL, NULL }; int n = 1;
    for (char *p = b; *p && n < 5; p++) if (*p == '\t') { *p = 0; f[n++] = p + 1; }
    pthread_mutex_lock(&mu);
    if (search_logged < 3) { search_logged++; dl_log("search row: %s", line); }          /* raw rows, to see which fields YouTube gives us */
    pthread_mutex_unlock(&mu);
    if (n < 5 || strlen(f[0]) != 11) return;                      /* not a result row */
    pthread_mutex_lock(&mu);
    if (nres < DL_MAXRES) {
        DlResult *r = &results[nres++];
        snprintf(r->id, sizeof r->id, "%s", f[0]);
        snprintf(r->title, sizeof r->title, "%s", f[1]);
        snprintf(r->artist, sizeof r->artist, "%s", strcmp(f[2], "NA") ? f[2] : "");
        snprintf(r->channel, sizeof r->channel, "%s", strcmp(f[3], "NA") ? f[3] : "");
        r->dur = (int)atof(f[4]);
    }
    pthread_mutex_unlock(&mu);
}

static int run_search(const char *target) {
    const char *a[40]; int n = 0;
    COMMON_ARGS(a, n)
    a[n++] = "--flat-playlist"; a[n++] = "--playlist-end"; a[n++] = "20";
    a[n++] = "--print"; a[n++] = "%(id)s\t%(title)s\t%(uploader,creator,artist)s\t%(channel,uploader)s\t%(duration)s";
    a[n++] = target; a[n] = NULL;
    return spawn_lines(a, search_line, NULL, NULL);
}

typedef struct { char *q; int gen; } SearchArg;

static void *search_thread(void *arg) {
    SearchArg *sa = arg; char *q = sa->q; free(sa);
    char enc[400], url[500], ys[300];
    urlenc(q, enc, sizeof enc);
    snprintf(url, sizeof url, "https://music.youtube.com/search?q=%s", enc);
    run_search(url);                                   /* YouTube Music search */
    pthread_mutex_lock(&mu); int got = nres; pthread_mutex_unlock(&mu);
    if (got == 0) {                                    /* fallback: plain YouTube search */
        snprintf(ys, sizeof ys, "ytsearch20:%s", q);
        run_search(ys);
    }
    pthread_mutex_lock(&mu);
    search_state = nres > 0 ? 2 : 3;
    busy_search = 0;
    pthread_mutex_unlock(&mu);
    free(q);
    return NULL;
}

void dl_search_start(const char *query) {
    pthread_mutex_lock(&mu);
    if (busy_search) { pthread_mutex_unlock(&mu); return; }
    busy_search = 1; search_state = 1; nres = 0; search_logged = 0; search_gen++;
    SearchArg *sa = malloc(sizeof *sa); sa->q = strdup(query); sa->gen = search_gen;
    pthread_mutex_unlock(&mu);
    pthread_t t;
    if (pthread_create(&t, NULL, search_thread, sa)) { free(sa->q); free(sa); pthread_mutex_lock(&mu); busy_search = 0; search_state = 3; pthread_mutex_unlock(&mu); return; }
    pthread_detach(t);
}

int dl_get_results(DlResult *out, int max, int *state) {
    pthread_mutex_lock(&mu);
    int n = nres < max ? nres : max;
    memcpy(out, results, (size_t)n * sizeof(DlResult));
    if (state) *state = search_state;
    pthread_mutex_unlock(&mu);
    return n;
}

/* ---------------- downloads ---------------- */
/* One yt-dlp process downloads a whole batch of queued songs (starting yt-dlp is slow on this hardware), and
   reports through marked lines: MPSTART <id>, MPPROG <id> <bytes> <total> <estimate>, MPDONE <id> <path>. */
enum { F_OTHER, F_UPDATE, F_NETWORK, F_UNAVAILABLE };

static int has(const char *low, const char *const *list) { for (; *list; list++) if (strstr(low, *list)) return 1; return 0; }
/* Sorts one of yt-dlp's error messages. The order matters: rate limits look like "unavailable ... try again later". */
static int classify(const char *msg) {
    char low[400]; size_t i = 0;
    for (; msg[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)msg[i]);
    low[i] = 0;
    static const char *const rate[] = { "try again later", "rate-limit", "rate limit", "http error 429", "too many requests", NULL };
    static const char *const gone[] = { "video unavailable", "private video", "has been removed", "not available in your country", "not available from your location",
        "members-only", "members only", "account associated with this video has been terminated", "no longer available", "copyright", "confirm your age", "inappropriate for some users", NULL };
    static const char *const net[] = { "unable to download webpage", "timed out", "connection reset", "connection refused", "name resolution", "name or service not known",
        "network is unreachable", "no route to host", "connection aborted", "eof occurred", "http error 5", "ssl:", NULL };
    static const char *const upd[] = { "signature", "nsig", "unable to extract", "requested format is not available", "sign in to confirm you", "http error 403",
        "http error 400", "please report this issue", "javascript", "js runtime", "challenge", NULL };
    if (has(low, rate)) return F_NETWORK;
    if (has(low, gone)) return F_UNAVAILABLE;
    if (has(low, net)) return F_NETWORK;
    if (has(low, upd)) return F_UPDATE;
    return F_OTHER;
}
static void fail_text(int cls, const char *raw, char *out, size_t n) {
    switch (cls) {
    case F_NETWORK:     snprintf(out, n, "Network problem - is Wi-Fi on? (%.90s)", raw); break;
    case F_UNAVAILABLE: snprintf(out, n, "YouTube won't give this track (removed, private, region-locked or age-restricted)"); break;
    case F_UPDATE:      snprintf(out, n, "YouTube changed - run \"Update downloader\" and retry (%.80s)", raw); break;
    default:            snprintf(out, n, "%.150s", raw[0] ? raw : "Download failed"); break;
    }
}

typedef struct {
    int idx[16]; char id[16][16]; int settled[16]; int n;     /* batch of single songs, matched by video id */
    int solo;                                                  /* job index when the run holds one job (playlist/album) */
    int done_n, cur;                                           /* solo: tracks finished; batch: index of the song in progress */
    char last_err[240];
} Batch;

static int batch_find(const Batch *b, const char *id) { for (int i = 0; i < b->n; i++) if (!strcmp(b->id[i], id)) return i; return -1; }

static void job_fail(int ji, const char *raw) {                /* mu held */
    char t[200]; int cls = classify(raw);
    fail_text(cls, raw, t, sizeof t);
    jobs[ji].state = 3; snprintf(jobs[ji].msg, sizeof jobs[ji].msg, "%.159s", t);
}
static double num(const char *s) { char *e; double v = strtod(s, &e); return e == s ? 0 : v; }

static void batch_line(const char *line, void *ctx) {
    Batch *b = ctx;
    pthread_mutex_lock(&mu);
    if (!strncmp(line, "MPSTART ", 8)) {
        int k = b->solo >= 0 ? -2 : batch_find(b, line + 8);
        if (k >= 0) { b->cur = k; jobs[b->idx[k]].state = 1; jobs[b->idx[k]].pct = 0; snprintf(jobs[b->idx[k]].msg, sizeof jobs[0].msg, "Downloading"); }
        else if (k == -2) { jobs[b->solo].pct = 0; snprintf(jobs[b->solo].msg, sizeof jobs[0].msg, "Track %d: downloading", b->done_n + 1); }
    } else if (!strncmp(line, "MPPROG ", 7)) {
        char tmp[200]; snprintf(tmp, sizeof tmp, "%s", line + 7);
        char *sv, *id = strtok_r(tmp, " ", &sv), *t1 = strtok_r(NULL, " ", &sv), *t2 = strtok_r(NULL, " ", &sv), *t3 = strtok_r(NULL, " ", &sv);
        if (id && t1) {
            double done = num(t1), total = t2 ? num(t2) : 0, est = t3 ? num(t3) : 0;
            if (total <= 0) total = est;
            int k = b->solo >= 0 ? -2 : batch_find(b, id), ji = k >= 0 ? b->idx[k] : (k == -2 ? b->solo : -1);
            if (ji >= 0 && total > 0) jobs[ji].pct = (int)(done * 100.0 / total > 99 ? 99 : done * 100.0 / total);
        }
    } else if (!strncmp(line, "MPDONE ", 7)) {
        char id[16] = ""; const char *path = strchr(line + 7, ' ');
        snprintf(id, sizeof id, "%.11s", line + 7);
        const char *base = path ? strrchr(path, '/') : NULL;
        if (b->solo >= 0) {
            b->done_n++; jobs[b->solo].pct = 100; snprintf(jobs[b->solo].msg, sizeof jobs[0].msg, "%d saved", b->done_n); dl_new_files = 1;
        } else {
            int k = batch_find(b, id);
            if (k >= 0 && !b->settled[k]) {
                b->settled[k] = 1; DlJob *j = &jobs[b->idx[k]];
                j->state = 2; j->pct = 100; snprintf(j->msg, sizeof j->msg, "Saved: %s", base ? base + 1 : "Music/Downloads"); dl_new_files = 1;
            }
        }
    } else if (!strncmp(line, "ERROR:", 6)) {
        const char *m = line + 6; while (*m == ' ') m++;
        snprintf(b->last_err, sizeof b->last_err, "%s", m);
        int k = -1;
        const char *rb = strstr(m, "] ");                               /* "[youtube] <id>: <message>" names its song */
        if (b->solo < 0 && m[0] == '[' && rb && strlen(rb) > 14 && rb[13] == ':') { char id[16]; snprintf(id, sizeof id, "%.11s", rb + 2); k = batch_find(b, id); }
        if (k < 0 && b->solo < 0) k = b->cur;
        if (k >= 0 && !b->settled[k]) { b->settled[k] = 1; job_fail(b->idx[k], m); }
    }
    pthread_mutex_unlock(&mu);
}

/* Video id out of a watch URL, or "" when the URL isn't one. */
static void video_id(const char *url, char *out) {
    out[0] = 0;
    const char *p = strstr(url, "v=");
    if (!p) return;
    p += 2;
    int ok = 1; for (int i = 0; i < 11; i++) if (!(isalnum((unsigned char)p[i]) || p[i] == '-' || p[i] == '_')) ok = 0;
    if (ok) { memcpy(out, p, 11); out[11] = 0; }
}

static int run_batch(Batch *b, const char *playlist_url) {
    mkdir(out_dir, 0755); mkdir("/tmp/vibe_dl", 0755);
    struct statvfs sv;
    if (statvfs(out_dir, &sv) == 0 && (unsigned long long)sv.f_bavail * sv.f_frsize < (200ULL << 20)) {
        pthread_mutex_lock(&mu); snprintf(b->last_err, sizeof b->last_err, "Not enough free space on the SD card (need 200 MB)"); pthread_mutex_unlock(&mu);
        return 98;
    }
    char batchfile[80] = "/tmp/vibe_dl/batch.txt";
    if (!playlist_url) {
        FILE *f = fopen(batchfile, "w"); if (!f) return -1;
        pthread_mutex_lock(&mu);
        for (int i = 0; i < b->n; i++) fprintf(f, "%s\n", jobs[b->idx[i]].url);
        pthread_mutex_unlock(&mu);
        fclose(f);
    }
    const char *a[72]; int n = 0;
    COMMON_ARGS(a, n)
    a[n++] = "-f"; a[n++] = "bestaudio/best";
    a[n++] = "-x"; a[n++] = "--audio-format"; a[n++] = "mp3"; a[n++] = "--audio-quality"; a[n++] = "320K";   /* highest MP3 bitrate */
    a[n++] = "--embed-metadata"; a[n++] = "--embed-thumbnail"; a[n++] = "--convert-thumbnails"; a[n++] = "jpg";
    a[n++] = "--ppa";                                       /* crop the cover to a centered square */
    a[n++] = "ThumbnailsConvertor+FFmpeg_o:-c:v mjpeg -vf crop=\"'if(gt(ih,iw),iw,ih)':'if(gt(iw,ih),ih,iw)'\"";
    a[n++] = "--windows-filenames"; a[n++] = "--trim-filenames"; a[n++] = "150"; a[n++] = "--no-overwrites";
    a[n++] = (playlist_url && b->solo >= 0 && jobs[b->solo].playlist) ? "--yes-playlist" : "--no-playlist";
    a[n++] = "-P"; a[n++] = out_dir; a[n++] = "-P"; a[n++] = "temp:/tmp/vibe_dl";
    a[n++] = "-o";
    a[n++] = (playlist_url && b->solo >= 0 && jobs[b->solo].playlist) ? "%(playlist_title)s/%(playlist_index)02d - %(title)s.%(ext)s" : "%(artist,uploader)s - %(title)s.%(ext)s";
    a[n++] = "--progress"; a[n++] = "--no-simulate";
    a[n++] = "--progress-template"; a[n++] = "download:MPPROG %(info.id)s %(progress.downloaded_bytes)s %(progress.total_bytes)s %(progress.total_bytes_estimate)s";
    a[n++] = "--print"; a[n++] = "before_dl:MPSTART %(id)s";
    a[n++] = "--print"; a[n++] = "after_move:MPDONE %(id)s %(filepath)s";
    if (playlist_url) a[n++] = playlist_url; else { a[n++] = "--batch-file"; a[n++] = batchfile; }
    a[n] = NULL;
    return spawn_lines(a, batch_line, b, &cur_pid);
}

static void *worker_main(void *arg) {
    (void)arg;
    for (;;) {
        Batch b; memset(&b, 0, sizeof b); b.solo = -1; b.cur = -1;
        char solo_url[300] = ""; int use_solo = 0;
        pthread_mutex_lock(&mu);
        while (!quitting && next_job >= njobs) pthread_cond_wait(&cv, &mu);
        if (quitting) { pthread_mutex_unlock(&mu); break; }
        /* collect the next songs: single watch URLs go together, anything else (playlists, odd URLs) runs alone */
        while (next_job < njobs && b.n < 12) {
            int i = next_job; char vid[16]; video_id(jobs[i].url, vid);
            if (jobs[i].playlist || !vid[0]) {
                if (b.n == 0) { use_solo = 1; b.solo = i; snprintf(solo_url, sizeof solo_url, "%s", jobs[i].url); next_job++; jobs[i].state = 1; snprintf(jobs[i].msg, sizeof jobs[i].msg, "Starting..."); }
                break;
            }
            b.idx[b.n] = i; snprintf(b.id[b.n], sizeof b.id[0], "%s", vid); b.n++; next_job++;
            jobs[i].state = 1; jobs[i].pct = 0; snprintf(jobs[i].msg, sizeof jobs[i].msg, "Waiting for yt-dlp...");
        }
        pthread_mutex_unlock(&mu);
        int rc = run_batch(&b, use_solo ? solo_url : NULL);
        pthread_mutex_lock(&mu);
        if (quitting) { pthread_mutex_unlock(&mu); break; }
        if (use_solo) {
            DlJob *j = &jobs[b.solo];
            if (b.done_n > 0 || rc == 0) { j->state = 2; j->pct = 100; snprintf(j->msg, sizeof j->msg, "%d track%s saved to Music/Downloads", b.done_n, b.done_n == 1 ? "" : "s"); if (b.done_n) dl_new_files = 1; }
            else job_fail(b.solo, b.last_err[0] ? b.last_err : "Download failed");
        } else {
            for (int k = 0; k < b.n; k++) if (!b.settled[k]) {                 /* songs yt-dlp never reached fail as the run did */
                char why[240]; snprintf(why, sizeof why, "%s", b.last_err[0] ? b.last_err : (rc == 0 ? "yt-dlp finished without this song" : "yt-dlp stopped unexpectedly"));
                job_fail(b.idx[k], why);
            }
        }
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

int dl_enqueue(const char *input, const char *label) {
    char in[300]; snprintf(in, sizeof in, "%s", input); trim(in);
    char url[300];
    if (!in[0]) return 0;
    if (!strncmp(in, "http", 4)) snprintf(url, sizeof url, "%s", in);
    else {
        size_t n = strlen(in); int ok = n == 11;
        for (size_t i = 0; ok && i < n; i++) if (!isalnum((unsigned char)in[i]) && in[i] != '-' && in[i] != '_') ok = 0;
        if (!ok) return 0;
        snprintf(url, sizeof url, "https://music.youtube.com/watch?v=%.11s", in);
    }
    pthread_mutex_lock(&mu);
    if (njobs >= DL_MAXJOB) { pthread_mutex_unlock(&mu); return 0; }
    DlJob *j = &jobs[njobs++];
    memset(j, 0, sizeof *j);
    snprintf(j->url, sizeof j->url, "%s", url);
    snprintf(j->label, sizeof j->label, "%s", label && label[0] ? label : url);
    j->playlist = strstr(url, "/playlist") || (strstr(url, "list=") && !strstr(url, "v="));
    if (!worker_started) { worker_started = 1; pthread_create(&worker, NULL, worker_main, NULL); pthread_detach(worker); }
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    return 1;
}

int dl_import_links(void) {
    char p[480]; snprintf(p, sizeof p, "%s/links.txt", out_dir);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    char line[320]; int n = 0;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        n += dl_enqueue(line, line);
    }
    fclose(f);
    return n;
}

int dl_get_jobs(DlJob *out, int max) {
    pthread_mutex_lock(&mu);
    int n = njobs < max ? njobs : max;
    memcpy(out, jobs, (size_t)n * sizeof(DlJob));
    pthread_mutex_unlock(&mu);
    return n;
}

/* ---------------- update ---------------- */
static void upd_line(const char *line, void *ctx) {
    (void)ctx;
    pthread_mutex_lock(&mu);
    if (line[0]) snprintf(upd_msg, sizeof upd_msg, "%s", line);
    pthread_mutex_unlock(&mu);
}
static void *update_thread(void *arg) {
    (void)arg;
    const char *a[40]; int n = 0;
    COMMON_ARGS(a, n)
    a[n++] = "-U"; a[n] = NULL;
    int rc = spawn_lines(a, upd_line, NULL, NULL);
    pthread_mutex_lock(&mu);
    if (rc != 0) { char t[200]; snprintf(t, sizeof t, "%.150s", upd_msg); snprintf(upd_msg, sizeof upd_msg, "Update failed (exit %d): %s", rc, t); }
    upd_state = 2; busy_update = 0;
    pthread_mutex_unlock(&mu);
    return NULL;
}
void dl_update_start(void) {
    pthread_mutex_lock(&mu);
    if (busy_update) { pthread_mutex_unlock(&mu); return; }
    busy_update = 1; upd_state = 1; snprintf(upd_msg, sizeof upd_msg, "Checking for updates...");
    pthread_mutex_unlock(&mu);
    pthread_t t;
    if (pthread_create(&t, NULL, update_thread, NULL)) { pthread_mutex_lock(&mu); busy_update = 0; upd_state = 2; pthread_mutex_unlock(&mu); return; }
    pthread_detach(t);
}
void dl_update_status(char *buf, size_t n, int *state) {
    pthread_mutex_lock(&mu);
    snprintf(buf, n, "%s", upd_msg);
    if (state) *state = upd_state;
    pthread_mutex_unlock(&mu);
}

int dl_busy(void) {
    pthread_mutex_lock(&mu);
    int b = busy_search || busy_update || (next_job < njobs) || (njobs > 0 && jobs[njobs - 1].state == 1);
    for (int i = 0; !b && i < njobs; i++) if (jobs[i].state == 1) b = 1;
    pthread_mutex_unlock(&mu);
    return b;
}

/*
 * Gapless engine.
 * One decoder thread decodes track after track into a single fixed-format
 * (44.1 kHz, S16, stereo) ring buffer. The SDL audio callback just drains the
 * ring, so there is never a flush or reopen between tracks. "Marks" record the
 * ring position where each track begins so the UI knows what is audible.
 */
#define _GNU_SOURCE
#include "engine.h"
#include "library.h"
#include <SDL.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <math.h>

#define OUT_RATE    44100
#define RING_FRAMES (1 << 17)          /* ~3 s */
#define MAXMARK     16
#define RING_HISTORY 16384         /* played audio kept behind the read position, for the visualizer */

typedef struct { int64_t frame; int song; double dur, offset; } Mark;

static int16_t ring[RING_FRAMES * 2];
static int64_t rd_pos, wr_pos;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_space = PTHREAD_COND_INITIALIZER, cv_cmd = PTHREAD_COND_INITIALIZER;
static volatile int generation, quit;
static int req_pending, req_pos; static double req_seek;
static int dec_finished, ended, paused = 1, shuffle_on, repeat_mode = 1;
static Mark marks[MAXMARK]; static int nmarks;
static Mark cur; static int cur_valid;
static int *order, *base; static int norder, nbase, dec_pos;
static SDL_AudioDeviceID dev;
static pthread_t dec_thr;
volatile int g_track_changed;

/* ---- diagnostics: what happens around an audible stutter ---- */
static volatile int dbg_underruns, dbg_late, dbg_late_max, dbg_lockwait, dbg_lock_max, dbg_fill_min = 1 << 30, dbg_grace;
static int64_t dbg_last_us;
static int64_t cb_time_us, cb_rd;           /* when the audio callback last ran, and where the read position was */
static int period_frames = 4096, vis_sync_ms;
static char audio_info[200] = "";
static int64_t now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000; }

/* ---------------- equalizer: 10 peaking biquads on the output, applied in the audio callback ---------------- */
#define EQ_BANDS 10
static const float EQ_FREQ[EQ_BANDS] = { 31.f, 62.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f };
typedef struct { float b0, b1, b2, a1, a2; } Biquad;
typedef struct { int active; float pre; char on[EQ_BANDS]; Biquad bq[EQ_BANDS]; } EqSet;
static EqSet eq_sets[2];
static volatile int eq_cur;                 /* the callback reads eq_sets[eq_cur]; changes are built in the other one, then swapped in */
static float eq_gain[EQ_BANDS];             /* dB, -12..+12 */
static float eq_z[2][EQ_BANDS][2];          /* filter memory per channel */

static void eq_rebuild(void) {
    EqSet *e = &eq_sets[1 - eq_cur];
    float maxg = 0.f; int any = 0;
    for (int k = 0; k < EQ_BANDS; k++) {
        float g = eq_gain[k];
        e->on[k] = fabsf(g) >= 0.05f;
        if (e->on[k]) any = 1;
        if (g > maxg) maxg = g;
        float A = powf(10.f, g / 40.f), w0 = 2.f * (float)M_PI * EQ_FREQ[k] / OUT_RATE, al = sinf(w0) / (2.f * 1.41f), c = cosf(w0);
        float a0 = 1.f + al / A;
        e->bq[k].b0 = (1.f + al * A) / a0; e->bq[k].b1 = -2.f * c / a0; e->bq[k].b2 = (1.f - al * A) / a0;
        e->bq[k].a1 = -2.f * c / a0;       e->bq[k].a2 = (1.f - al / A) / a0;
    }
    e->active = any;
    e->pre = powf(10.f, -maxg / 20.f);      /* make room for boosts so they don't clip */
    __sync_synchronize();
    eq_cur = 1 - eq_cur;
}
void engine_eq_set(int band, float db) {
    if (band < 0 || band >= EQ_BANDS) return;
    eq_gain[band] = db < -12.f ? -12.f : db > 12.f ? 12.f : db;
    eq_rebuild();
}
float engine_eq_get(int band) { return band >= 0 && band < EQ_BANDS ? eq_gain[band] : 0.f; }
void engine_eq_reset(void) { for (int k = 0; k < EQ_BANDS; k++) eq_gain[k] = 0.f; eq_rebuild(); }

static void eq_process(int16_t *s, int n) {
    const EqSet *e = &eq_sets[eq_cur];
    if (!e->active) return;
    for (int i = 0; i < n; i++) for (int c = 0; c < 2; c++) {
        float x = (float)s[i * 2 + c] * e->pre;
        for (int k = 0; k < EQ_BANDS; k++) {
            if (!e->on[k]) continue;
            const Biquad *q = &e->bq[k]; float *z = eq_z[c][k];
            float y = q->b0 * x + z[0];
            z[0] = q->b1 * x - q->a1 * y + z[1];
            z[1] = q->b2 * x - q->a2 * y;
            x = y;
        }
        s[i * 2 + c] = (int16_t)(x > 32767.f ? 32767 : x < -32768.f ? -32768 : x);
    }
}

static void wait_space(int ms) {          /* mu must be held */
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&cv_space, &mu, &ts);
}

/* ---------- audio callback ---------- */
static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    static int rt_done;
    if (!rt_done) {                         /* run the audio thread real-time so UI load can't starve it */
        rt_done = 1;
        struct sched_param sp; sp.sched_priority = 20;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp)) {}
    }
    int want = len / 4;
    int16_t *out = (int16_t *)stream;
    int64_t t0 = now_us();
    int64_t period_us = (int64_t)want * 1000000LL / OUT_RATE;
    int late = 0;
    if (dbg_last_us && t0 - dbg_last_us > period_us * 3 / 2 && dbg_grace <= 0) { late = (int)((t0 - dbg_last_us) / 1000); dbg_late++; if (late > dbg_late_max) dbg_late_max = late; }
    dbg_last_us = t0;
    pthread_mutex_lock(&mu);
    int lw = (int)((now_us() - t0) / 1000);
    if (lw >= 3) { dbg_lockwait++; if (lw > dbg_lock_max) dbg_lock_max = lw; }       /* waited for the lock for >= 3 ms */
    int avail = (int)(wr_pos - rd_pos);
    int n = paused ? 0 : (avail < want ? avail : want);
    if (dbg_grace > 0) dbg_grace--;
    else if (!paused) {
        if (avail < dbg_fill_min) dbg_fill_min = avail;
        if (n < want && cur_valid && !(dec_finished && avail == 0)) dbg_underruns++;      /* ran out of decoded audio */
    }
    if (n > 0) {
        int idx = (int)(rd_pos & (RING_FRAMES - 1));
        int first = RING_FRAMES - idx; if (first > n) first = n;
        memcpy(out, &ring[idx * 2], (size_t)first * 4);
        if (n > first) memcpy(out + first * 2, &ring[0], (size_t)(n - first) * 4);
        rd_pos += n;
    }
    if (n < want) memset(out + n * 2, 0, (size_t)(want - n) * 4);
    cb_time_us = t0; cb_rd = rd_pos;
    while (nmarks > 0 && marks[0].frame <= rd_pos) {
        cur = marks[0]; cur_valid = 1;
        memmove(&marks[0], &marks[1], (size_t)(nmarks - 1) * sizeof(Mark));
        nmarks--; g_track_changed = 1;
    }
    if (dec_finished && !ended && nmarks == 0 && rd_pos == wr_pos) {
        ended = 1; paused = 1; g_track_changed = 1;
    }
    if (n > 0) pthread_cond_signal(&cv_space);
    pthread_mutex_unlock(&mu);
    if (n > 0) eq_process(out, n);               /* after the lock is released: the maths must not hold up the decoder */
}

/* ---------- decoder side ---------- */
static int ring_write(int gen, const int16_t *s, int frames) {
    pthread_mutex_lock(&mu);
    while (frames > 0) {
        if (gen != generation || quit) { pthread_mutex_unlock(&mu); return 0; }
        int space = RING_FRAMES - RING_HISTORY - (int)(wr_pos - rd_pos);
        if (space <= 0) { wait_space(500); continue; }
        int n = space < frames ? space : frames;
        int idx = (int)(wr_pos & (RING_FRAMES - 1));
        int first = RING_FRAMES - idx; if (first > n) first = n;
        memcpy(&ring[idx * 2], s, (size_t)first * 4);
        if (n > first) memcpy(&ring[0], s + first * 2, (size_t)(n - first) * 4);
        wr_pos += n; s += n * 2; frames -= n;
    }
    pthread_mutex_unlock(&mu);
    return 1;
}

static int push_mark(int gen, int song, double dur, double offset) {
    pthread_mutex_lock(&mu);
    while (nmarks >= MAXMARK && gen == generation && !quit) wait_space(200);
    if (gen != generation || quit) { pthread_mutex_unlock(&mu); return 0; }
    marks[nmarks].frame = wr_pos; marks[nmarks].song = song;
    marks[nmarks].dur = dur; marks[nmarks].offset = offset; nmarks++;
    pthread_mutex_unlock(&mu);
    return 1;
}

static int emit(int gen, SwrContext *swr, AVCodecContext *cc, AVFrame *fr, int16_t **buf, int *cap) {
    int64_t delay = swr_get_delay(swr, cc->sample_rate);
    int n_in = fr ? fr->nb_samples : 0;
    int need = (int)av_rescale_rnd(delay + n_in, OUT_RATE, cc->sample_rate, AV_ROUND_UP) + 64;
    if (need > *cap) { *buf = realloc(*buf, (size_t)need * 4); *cap = need; }
    uint8_t *o[1] = { (uint8_t *)*buf };
    int n = swr_convert(swr, o, need, fr ? (const uint8_t **)fr->extended_data : NULL, n_in);
    if (n <= 0) return 1;
    return ring_write(gen, *buf, n);
}

/* returns 1 = finished normally, 0 = aborted (new request), -1 = failed to open */
static int decode_track(int gen, int pos, double seek) {
    char path[512]; int song;
    AVFormatContext *fc = NULL; AVCodecContext *cc = NULL; SwrContext *swr = NULL;
    AVPacket *pkt = NULL; AVFrame *fr = NULL; int16_t *buf = NULL; int bufcap = 0;
    const AVCodec *codec = NULL; AVChannelLayout outl; int si, result = -1; double dur;

    pthread_mutex_lock(&mu);
    if (pos >= norder || order[pos] >= g_nsongs) { pthread_mutex_unlock(&mu); return 0; }   /* library changed under us */
    song = order[pos];
    snprintf(path, sizeof path, "%s", g_songs[song].path);
    pthread_mutex_unlock(&mu);

    if (avformat_open_input(&fc, path, NULL, NULL) < 0) goto end;
    if (avformat_find_stream_info(fc, NULL) < 0) goto end;
    si = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (si < 0 || !codec) goto end;
    cc = avcodec_alloc_context3(codec);
    if (!cc) goto end;
    avcodec_parameters_to_context(cc, fc->streams[si]->codecpar);
    if (cc->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&cc->ch_layout, cc->ch_layout.nb_channels ? cc->ch_layout.nb_channels : 2);
    if (avcodec_open2(cc, codec, NULL) < 0) goto end;
    av_channel_layout_default(&outl, 2);
    if (swr_alloc_set_opts2(&swr, &outl, AV_SAMPLE_FMT_S16, OUT_RATE,
                            &cc->ch_layout, cc->sample_fmt, cc->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0)
        goto end;
    dur = fc->duration > 0 ? fc->duration / (double)AV_TIME_BASE : 0;
    if (seek > 0) {
        av_seek_frame(fc, -1, (int64_t)(seek * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(cc);
    }
    if (!push_mark(gen, song, dur, seek)) { result = 0; goto end; }

    pkt = av_packet_alloc(); fr = av_frame_alloc();
    result = 1;
    while (result == 1 && av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) >= 0) {
            while (avcodec_receive_frame(cc, fr) >= 0) {
                if (!emit(gen, swr, cc, fr, &buf, &bufcap)) { result = 0; break; }
                av_frame_unref(fr);
            }
        }
        av_packet_unref(pkt);
        if (generation != gen || quit) result = 0;
    }
    if (result == 1) {                      /* drain decoder, then resampler */
        avcodec_send_packet(cc, NULL);
        while (result == 1 && avcodec_receive_frame(cc, fr) >= 0) {
            if (!emit(gen, swr, cc, fr, &buf, &bufcap)) result = 0;
            av_frame_unref(fr);
        }
        if (result == 1 && !emit(gen, swr, cc, NULL, &buf, &bufcap)) result = 0;
    }
end:
    free(buf);
    if (pkt) av_packet_free(&pkt);
    if (fr) av_frame_free(&fr);
    if (swr) swr_free(&swr);
    if (cc) avcodec_free_context(&cc);
    if (fc) avformat_close_input(&fc);
    return result;
}

static int next_pos(int pos) {              /* mu held */
    if (repeat_mode == 2) return pos;
    if (pos + 1 < norder) return pos + 1;
    if (repeat_mode == 1) return 0;
    return -1;
}

static void *decoder_main(void *arg) {
    (void)arg;
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -5)) {}      /* decoder beats the UI for CPU */
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!quit && !req_pending) pthread_cond_wait(&cv_cmd, &mu);
        if (quit) { pthread_mutex_unlock(&mu); break; }
        req_pending = 0;
        int gen = generation, pos = req_pos; double seek = req_seek;
        pthread_mutex_unlock(&mu);
        int fails = 0;
        for (;;) {
            pthread_mutex_lock(&mu); dec_pos = pos; pthread_mutex_unlock(&mu);
            int r = decode_track(gen, pos, seek);
            seek = 0;
            if (r == 0) break;
            fails = r < 0 ? fails + 1 : 0;
            pthread_mutex_lock(&mu);
            int np = (gen != generation) ? -2 : next_pos(dec_pos);
            if (np >= 0 && fails >= norder) np = -1;
            if (np == -1) dec_finished = 1;
            pthread_mutex_unlock(&mu);
            if (np < 0) break;
            pos = np;
        }
    }
    return NULL;
}

/* ---------- control (main thread) ---------- */
static void play_locked(int pos, double seek, int keep_pause) {
    generation++;
    dbg_grace = 12;                          /* seeks/skips legitimately refill the buffer */
    rd_pos = wr_pos; nmarks = 0;
    req_pending = 1; req_pos = pos; req_seek = seek;
    if (!keep_pause) paused = 0;
    ended = 0; dec_finished = 0;
    pthread_cond_signal(&cv_cmd);
    pthread_cond_broadcast(&cv_space);
}
static void apply_pause(void) { SDL_PauseAudioDevice(dev, paused); }

static int find_pos(int song) {
    for (int i = 0; i < norder; i++) if (order[i] == song) return i;
    return 0;
}
static void shuffle_order(void) {
    for (int i = norder - 1; i > 0; i--) { int j = rand() % (i + 1); int t = order[i]; order[i] = order[j]; order[j] = t; }
}
static void set_base(const int *songs, int n) {
    if (!songs) { n = g_nsongs; for (int i = 0; i < n; i++) base[i] = i; }
    else memcpy(base, songs, (size_t)n * sizeof(int));
    nbase = n;
}
static int order_build(int startpos) {      /* mu held; returns position to start at */
    norder = nbase;
    memcpy(order, base, (size_t)norder * sizeof(int));
    if (!shuffle_on) return startpos;
    int s = order[startpos];
    shuffle_order();
    int j = find_pos(s); int t = order[0]; order[0] = order[j]; order[j] = t;
    return 0;
}

int engine_init(void) {
    pthread_mutexattr_t ma;                 /* priority inheritance: a UI thread holding the lock can't stall the RT audio thread */
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_destroy(&mu);
    pthread_mutex_init(&mu, &ma);
    srand((unsigned)time(NULL));
    order = malloc(sizeof(int) * (g_nsongs ? g_nsongs : 1));
    base  = malloc(sizeof(int) * (g_nsongs ? g_nsongs : 1));
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = OUT_RATE; want.format = AUDIO_S16SYS; want.channels = 2;
    want.samples = 4096; want.callback = audio_cb;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) return -1;
    if (have.samples > 0) period_frames = have.samples;
    snprintf(audio_info, sizeof audio_info, "SDL audio driver=%s  asked 44100/4096 got freq=%d samples=%d channels=%d format=0x%x",
             SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", have.freq, have.samples, have.channels, have.format);
    pthread_create(&dec_thr, NULL, decoder_main, NULL);
    return 0;
}

void engine_shutdown(void) {
    pthread_mutex_lock(&mu);
    quit = 1; generation++;
    pthread_cond_broadcast(&cv_cmd); pthread_cond_broadcast(&cv_space);
    pthread_mutex_unlock(&mu);
    pthread_join(dec_thr, NULL);
    if (dev) SDL_CloseAudioDevice(dev);
}

/* songs == NULL means the whole library and startpos is a song index. seek = start offset in seconds. */
void engine_start_queue_seek(const int *songs, int n, int startpos, double seek) {
    pthread_mutex_lock(&mu);
    set_base(songs, n);
    int pos = order_build(startpos);
    play_locked(pos, seek, 0);
    pthread_mutex_unlock(&mu);
    apply_pause();
}
void engine_start_queue(const int *songs, int n, int startpos) { engine_start_queue_seek(songs, n, startpos, 0.0); }
void engine_set_modes(int shuffle, int repeat) {
    pthread_mutex_lock(&mu); shuffle_on = shuffle != 0; repeat_mode = ((repeat % 3) + 3) % 3; pthread_mutex_unlock(&mu);
}

int engine_shuffle_queue(const int *songs, int n) {
    pthread_mutex_lock(&mu);
    set_base(songs, n);
    shuffle_on = 1;
    int pos = order_build(rand() % nbase);
    int song = order[pos];
    play_locked(pos, 0, 0);
    pthread_mutex_unlock(&mu);
    apply_pause();
    return song;
}

void engine_set_pause(int p) {
    pthread_mutex_lock(&mu);
    if (!p && ended && cur_valid) play_locked(find_pos(cur.song), 0, 0);
    else paused = p;
    p = paused;
    pthread_mutex_unlock(&mu);
    SDL_PauseAudioDevice(dev, p);
}
void engine_toggle_pause(void) {
    pthread_mutex_lock(&mu); int p = !paused; pthread_mutex_unlock(&mu);
    engine_set_pause(p);
}

void engine_next(void) {
    pthread_mutex_lock(&mu);
    if (norder) {
        int cs = cur_valid ? cur.song : order[dec_pos];
        play_locked((find_pos(cs) + 1) % norder, 0, 0);
    }
    pthread_mutex_unlock(&mu);
    apply_pause();
}

void engine_prev(void) {
    pthread_mutex_lock(&mu);
    if (norder) {
        int cs = cur_valid ? cur.song : order[dec_pos];
        int p = find_pos(cs);
        double el = cur_valid ? cur.offset + (double)(rd_pos - cur.frame) / OUT_RATE : 0;
        play_locked(el > 3.0 ? p : (p - 1 + norder) % norder, 0, 0);
    }
    pthread_mutex_unlock(&mu);
    apply_pause();
}

void engine_seek(double delta) {
    pthread_mutex_lock(&mu);
    if (!cur_valid || !norder) { pthread_mutex_unlock(&mu); return; }
    double el = cur.offset + (double)(rd_pos - cur.frame) / OUT_RATE;
    double t = el + delta; if (t < 0) t = 0;
    if (cur.dur > 0 && t > cur.dur - 1.0) { pthread_mutex_unlock(&mu); engine_next(); return; }
    play_locked(find_pos(cur.song), t, 1);
    cur.offset = t; cur.frame = rd_pos;
    pthread_mutex_unlock(&mu);
}

void engine_toggle_shuffle(void) {
    pthread_mutex_lock(&mu);
    shuffle_on = !shuffle_on;
    if (norder) {
        int ds = order[dec_pos];
        norder = nbase;
        memcpy(order, base, (size_t)norder * sizeof(int));
        if (shuffle_on) {
            shuffle_order();
            int j = find_pos(ds); int t = order[0]; order[0] = order[j]; order[j] = t;
            dec_pos = 0;
        } else {                                            /* back to queue order: find where this song sits in the queue */
            dec_pos = 0;
            for (int i = 0; i < nbase; i++) if (base[i] == ds) { dec_pos = i; break; }
        }
        if (dec_finished && repeat_mode == 0) { /* nothing queued; leave ended state alone */ }
    }
    pthread_mutex_unlock(&mu);
}

void engine_cycle_repeat(void) {
    pthread_mutex_lock(&mu); repeat_mode = (repeat_mode + 1) % 3; pthread_mutex_unlock(&mu);
}

void engine_snapshot(Snap *s) {
    pthread_mutex_lock(&mu);
    s->valid = cur_valid; s->song = cur.song; s->dur = cur.dur;
    double el = cur_valid ? cur.offset + (double)(rd_pos - cur.frame) / OUT_RATE : 0;
    if (el < 0) el = 0;
    if (cur.dur > 0 && el > cur.dur) el = cur.dur;
    s->elapsed = el;
    s->paused = paused; s->ended = ended; s->shuffle = shuffle_on; s->repeat = repeat_mode;
    pthread_mutex_unlock(&mu);
}

/* Copies `frames` stereo frames of what is being heard right now (for the visualizer). The audio callback only
   advances the read position in whole periods (~90 ms), so the audible position is estimated from the time since
   the last callback: two periods are queued in the device, and they play out in real time. */
void engine_set_vis_sync(int ms) { vis_sync_ms = ms; }
int engine_peek(int16_t *out, int frames) {
    pthread_mutex_lock(&mu);
    int64_t pos = rd_pos;
    if (!paused && cb_time_us) {
        int64_t el = (now_us() - cb_time_us) * OUT_RATE / 1000000;
        if (el < 0) el = 0;
        if (el > 2 * period_frames) el = 2 * period_frames;
        pos = cb_rd - 2 * period_frames + el - (int64_t)vis_sync_ms * OUT_RATE / 1000;
    }
    if (pos < rd_pos - (RING_HISTORY - 1024)) pos = rd_pos - (RING_HISTORY - 1024);   /* older audio is gone */
    if (pos < 0) pos = 0;
    int64_t avail = wr_pos - pos;
    int n = avail < frames ? (int)avail : frames;
    if (n < 0) n = 0;
    int idx = (int)(pos & (RING_FRAMES - 1)), first = RING_FRAMES - idx;
    if (first > n) first = n;
    memcpy(out, &ring[idx * 2], (size_t)first * 4);
    if (n > first) memcpy(out + first * 2, &ring[0], (size_t)(n - first) * 4);
    pthread_mutex_unlock(&mu);
    return n;
}

/* Stop everything and forget the queue (the song list is about to be replaced). */
void engine_reset(void) {
    pthread_mutex_lock(&mu);
    generation++;
    rd_pos = wr_pos; nmarks = 0; cur_valid = 0;
    norder = 0; nbase = 0; dec_pos = 0;
    req_pending = 0; paused = 1; ended = 0; dec_finished = 0;
    pthread_cond_broadcast(&cv_space);
    pthread_mutex_unlock(&mu);
    SDL_PauseAudioDevice(dev, 1);
}
/* Resize the queue buffers for the new g_nsongs. */
void engine_resize(void) {
    pthread_mutex_lock(&mu);
    size_t n = (size_t)(g_nsongs ? g_nsongs : 1) * sizeof(int);
    order = realloc(order, n); base = realloc(base, n);
    pthread_mutex_unlock(&mu);
}

const char *engine_audio_info(void) { return audio_info; }
/* Returns 1 (and fills buf) if anything bad happened since the last call. */
int engine_diag(char *buf, size_t sz) {
    int u = dbg_underruns, l = dbg_late, lw = dbg_lockwait;
    if (!u && !l && !lw) return 0;
    snprintf(buf, sz, "underruns=%d  late_callbacks=%d (worst %d ms)  lock_waits=%d (worst %d ms)  min_ring_fill=%d frames",
             u, l, dbg_late_max, lw, dbg_lock_max, dbg_fill_min);
    dbg_underruns = dbg_late = dbg_lockwait = 0; dbg_late_max = dbg_lock_max = 0; dbg_fill_min = 1 << 30;
    return 1;
}

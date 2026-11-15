#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int    valid;      /* a track has actually started playing */
    int    song;       /* index into g_songs */
    int    paused, ended, shuffle, repeat;   /* repeat: 0 off, 1 all, 2 one */
    double elapsed, dur;
} Snap;

extern volatile int g_track_changed;

int  engine_init(void);
void engine_shutdown(void);

void engine_start_queue(const int *songs, int n, int startpos); /* songs==NULL: whole library, startpos = song index */
int  engine_shuffle_queue(const int *songs, int n);              /* returns first song */
void engine_toggle_pause(void);
void engine_set_pause(int p);
void engine_next(void);
void engine_prev(void);
void engine_seek(double delta_sec);
void engine_toggle_shuffle(void);
void engine_cycle_repeat(void);
void engine_snapshot(Snap *s);
int  engine_peek(int16_t *out, int frames);   /* upcoming samples, for the visualizer */
void engine_reset(void);     /* stop and clear the queue */
void engine_resize(void);    /* call after g_songs changed */
const char *engine_audio_info(void);          /* which SDL audio driver / format we actually got */
int  engine_diag(char *buf, size_t sz);          /* 1 if there were underruns / late callbacks since the last call */
void engine_set_vis_sync(int ms);          /* visualizer timing offset (positive = later) */
void engine_start_queue_seek(const int *songs, int n, int startpos, double seek);   /* like engine_start_queue, starting seek seconds in */
void engine_set_modes(int shuffle, int repeat);                                          /* restore shuffle / repeat (0 off, 1 all, 2 one) */
void  engine_eq_set(int band, float db);   /* 10 bands (31 Hz .. 16 kHz), -12..+12 dB, applies to everything */
float engine_eq_get(int band);
void  engine_eq_reset(void);                  /* back to flat */

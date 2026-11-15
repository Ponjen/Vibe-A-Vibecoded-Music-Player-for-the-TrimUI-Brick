#pragma once
#include <stddef.h>
#include <stdint.h>

#define DL_MAXRES 25
#define DL_MAXJOB 200

typedef struct { char id[16]; char title[160]; char artist[120]; char channel[120]; int dur; } DlResult;
/* state: 0 queued, 1 running, 2 done, 3 failed */
typedef struct { char label[160]; char url[300]; int playlist, state, pct; char msg[160]; } DlJob;

extern volatile int dl_new_files;          /* set when a download finished (library needs a rescan) */

void dl_init(const char *app_dir, const char *music_root);
void dl_shutdown(void);
int  dl_available(void);                   /* bin/yt-dlp, bin/ffmpeg and bin/qjs are present */

void dl_search_start(const char *query);
int  dl_get_results(DlResult *out, int max, int *state);   /* state: 0 idle, 1 searching, 2 done, 3 failed */

int  dl_enqueue(const char *input, const char *label);     /* a URL or a bare 11-character video id */
int  dl_import_links(void);                                /* Music/Downloads/links.txt, one URL per line */
int  dl_get_jobs(DlJob *out, int max);

void dl_update_start(void);                                /* yt-dlp -U */
void dl_update_status(char *buf, size_t n, int *state);    /* state: 0 idle, 1 running, 2 finished */

int  dl_busy(void);                        /* anything running (search, update, download) */

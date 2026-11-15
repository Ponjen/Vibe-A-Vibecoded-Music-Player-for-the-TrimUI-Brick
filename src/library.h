#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char path[512];
    char title[128];
    char artist[128];
    char album[128];
    char albumartist[128];
} Song;

extern Song *g_songs;   /* sorted by natural path order */
extern int   g_nsongs;

typedef void (*ScanCB)(int count);

void lib_init(void);
/* Recursively scans root. Uses/updates a tab-separated metadata cache. */
int  lib_scan(const char *root, const char *cache_file, ScanCB cb);

typedef struct { uint8_t *px; int size; } Img;
/* Extracts the song's own embedded art, center-cropped to size x size RGBA.
   Returns 0 on success. Caller frees out->px. Thread-safe. */
int  art_extract(const char *path, int size, Img *out);
int  img_load_file(const char *path, int size, Img *out);   /* a .jpg/.png/.webp file, centre-cropped square, RGBA */

/* ---- tag editing ---- */
typedef struct { char title[256], artist[256], album[256], albumartist[256], track[16], date[32], genre[128]; } Tags;
int tags_read(const char *path, Tags *t);                                    /* 0 on success */
/* Rewrites the file with new tags (audio is copied, not re-encoded). Returns 0 on success, else fills err. */
int tags_write(const char *path, const Tags *t, char *err, size_t errn);

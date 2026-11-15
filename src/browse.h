#pragma once
typedef struct { char name[128]; char artist[128]; int *songs; int n; int rep; } Album;
typedef struct { char name[128]; int *albums; int nalb; int nsongs; } Artist;
extern Album  *g_albums;  extern int g_nalbums;
extern Artist *g_artists; extern int g_nartists;
extern int *g_sorted;   /* song indices sorted alphabetically by title */
void browse_build(void);
void browse_free(void);   /* call after lib_scan */

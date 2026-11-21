#ifndef UB_DISC_ID_H
#define UB_DISC_ID_H
#include <stddef.h>

/* The mounted Blu-ray: /mnt/odd, or else the first UDF/ISO 9660 mount in
 * /proc/mounts, holding BDMV/index.bdmv. 1 and its path in out, or 0. */
int ub_disc_root(char *out,size_t size);
/* A 32-hex-digit identifier of the disc mounted at root: the first 128 bits
 * of SHA-256 over BDMV/index.bdmv, BDMV/MovieObject.bdmv and the sorted
 * (name, type, size) lists of BDMV/PLAYLIST and BDMV/STREAM, each section
 * tagged and length-prefixed. Stub index/MovieObject files shared by every
 * disc of a box set are told apart by their playlists and streams. Only
 * directory metadata and stat are used for those (no stream is read).
 * 0 and id, or -1 (no index.bdmv, an unreadable part, a file over 1 MiB or
 * more than 4096 entries in a directory). */
int ub_disc_id(const char *root,char id[33]);
#endif

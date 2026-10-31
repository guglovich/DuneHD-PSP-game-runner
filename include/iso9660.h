/*
 * ISO9660 / CSO file system reader interface.
 */

#ifndef ISO9660_H
#define ISO9660_H

#include <stdint.h>

/* ISO file handle structure */
struct iso_file {
    int fd;                   /* Host file descriptor */
    uint32_t extent_lba;      /* Starting LBA of file data */
    uint32_t size;            /* File size in bytes */
    uint32_t offset;          /* Current read offset */
    uint8_t is_cso;           /* 1 if CSO, 0 if ISO */
    uint32_t cso_block_size;  /* CSO block size (if CSO) */
    uint32_t cso_align;       /* CSO alignment */
    uint32_t cso_total_blocks; /* Total CSO blocks */
    struct cso_index_entry *cso_index; /* CSO block index */
};

/* CSO block index entry */
struct cso_index_entry {
    uint32_t offset;  /* byte offset in CSO file */
};

/* Global ISO/CSO context */
struct iso_context {
    struct iso_file *file;    /* Currently open ISO/CSO file */
    char *path;               /* Path to ISO/CSO file */
};
extern struct iso_context g_iso_ctx;

/* Initialize ISO/CSO context with given image path */
int iso_init(const char *iso_path);

/* Cleanup ISO/CSO context */
void iso_cleanup(void);

/* Check if path is UMD (disc0:/ or umd0:/) */
int iso_is_umd_path(const char *path);

/* Resolve UMD path for file operations */
int iso_resolve_umd_path(const char *psp_path, char *host_path, uint32_t host_len);

/* Modified path resolver that supports UMD via ISO/CSO */
int iofs_resolve_path_iso(const char *psp_path, char *host_path, uint32_t host_len);

/* Find file in ISO/CSO */
int iso_find_file(struct iso_file *iso, const char *path, uint32_t *out_extent, uint32_t *out_size);

/* Read sector from ISO/CSO */
int iso_read_sector(struct iso_file *iso, uint32_t lba, void *buf);

#endif /* ISO9660_H */

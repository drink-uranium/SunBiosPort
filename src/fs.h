#ifndef FS_H
#define FS_H

#include "blockdev.h"

struct fnode {
	u64 size;
	bool dir, link;
	u64 a, b;	/* fs specific: inode number / extent / cluster */
	u8 raw[160];	/* fs specific: cached inode or directory record */
};

struct fs;
typedef void (*dir_cb)(void *arg, const char *name, const struct fnode *n);

struct fs_ops {
	const char *name;
	int (*mount)(struct fs *fs);
	int (*root)(struct fs *fs, struct fnode *out);
	int (*readdir)(struct fs *fs, struct fnode *dir, dir_cb cb, void *arg);
	int (*read)(struct fs *fs, struct fnode *f, u64 off, u32 len, void *buf);
	bool case_insensitive;
};

struct fs {
	struct volume vol;
	const struct fs_ops *ops;
	void *priv;
	bool ci;
};

struct file {
	struct fs *fs;
	struct fnode node;
	u64 size;
};

/* Partition enumeration: fills up to max volumes (index 0 = whole disk). */
int part_scan(struct blockdev *bd, struct volume *vols, int max);
/* Try every filesystem driver on a volume. */
int fs_mount(struct volume *v, struct fs *fs);
int fs_open(struct fs *fs, const char *path, struct file *f);
int fs_read(struct file *f, u64 off, u32 len, void *buf);
int fs_list(struct fs *fs, const char *path, dir_cb cb, void *arg);

extern const struct fs_ops iso9660_ops, fat_ops, ext2_ops, ufs_ops;

#endif

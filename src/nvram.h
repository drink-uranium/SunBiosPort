#ifndef NVRAM_H
#define NVRAM_H

#include "lib.h"

void nv_init(void);
bool nv_was_reset(void);
const char *nv_get(const char *name);
bool nv_get_bool(const char *name);
long nv_get_int(const char *name);
int nv_set(const char *name, const char *val);
int nv_set_default(const char *name);
void nv_set_defaults(void);
void nv_print(const char *name);
void nv_commit(void);

/* device aliases */
const char *devalias_get(const char *name);
void devalias_set(const char *name, const char *path);
void devalias_print(const char *name);
void devalias_init(void);

#endif

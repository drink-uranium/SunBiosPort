#ifndef OBP_H
#define OBP_H

#include "lib.h"

#ifndef OBP_REV
#define OBP_REV "2.15"
#endif

void obp_banner(void);
void obp_cpu_model(char *out, size_t n);
u32 obp_serial(void);
u32 obp_hostid(void);

#endif

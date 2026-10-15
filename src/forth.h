#ifndef FORTH_H
#define FORTH_H

#include "lib.h"

typedef s32 cell;
typedef u32 ucell;

struct word;
typedef void (*code_fn)(struct word *);

struct word {
	struct word *link;
	code_fn code;
	cell param;
	cell does;
	u8 flags;
	u8 len;
	char name[34];
	const char *help;
};

#define F_IMM 1
#define F_HIDDEN 2

void forth_init(void);
/* Interpret one line; returns 0 on success, -1 if an error aborted it. */
int forth_interpret(const char *line);
int forth_evaluate(const char *text);
bool forth_compiling(void);
void forth_quit(void);	/* the "ok" loop */

/* used by command words implemented elsewhere */
void fpush(cell v);
cell fpop(void);
char *forth_parse_rest(void);
char *forth_parse_word(void);
NORETURN void forth_abort(const char *msg);
struct word *forth_add_prim(const char *name, code_fn fn, u8 flags, const char *help);

void obp_words_init(void);	/* obp.c */
void devtree_init(void);
void show_devs(const char *root);
void devtree_ls(void);
void devtree_dev(const char *path);
void devtree_pwd(void);
void devtree_props(void);

#endif

#ifndef _NL_TYPES_H
#define _NL_TYPES_H

#include <sys/types.h>

typedef int nl_item;
typedef int nl_catd;

#define NL_SETD 1
#define NL_CAT_LOCALE 1

nl_catd catopen(const char *name, int oflag);
char *catgets(nl_catd catd, int set_id, int msg_id, const char *s);
int catclose(nl_catd catd);

#endif

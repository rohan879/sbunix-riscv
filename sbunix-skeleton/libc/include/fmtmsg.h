#ifndef _FMTMSG_H
#define _FMTMSG_H

#define MM_HARD     0x00000001L
#define MM_SOFT     0x00000002L
#define MM_FIRM     0x00000004L
#define MM_APPL     0x00000008L
#define MM_UTIL     0x00000010L
#define MM_OPSYS    0x00000020L

#define MM_RECOVER  0x00000100L
#define MM_NRECOV   0x00000200L

#define MM_HALT     0x00000400L
#define MM_ERROR    0x00000800L
#define MM_WARNING  0x00001000L
#define MM_INFO     0x00002000L
#define MM_NOSEV    0x00004000L

#define MM_PRINT    0x00010000L
#define MM_CONSOLE  0x00020000L

#define MM_OK       0
#define MM_NOTOK    (-1)
#define MM_NOMSG    1
#define MM_NOCON    4

#define MM_NULLLBL  ((char *)0)
#define MM_NULLSEV  0
#define MM_NULLMC   0
#define MM_NULLTXT  ((char *)0)
#define MM_NULLACT  ((char *)0)
#define MM_NULLTAG  ((char *)0)

int fmtmsg(long classification, const char *label, int severity,
           const char *text, const char *action, const char *tag);

#endif

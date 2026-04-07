#ifndef _TAR_H
#define _TAR_H

/* ENH: POSIX <tar.h> header constants for os-test compliance.
 * Values match POSIX.1-2008 / IEEE 1003.1 tar archive format. */

/* General definitions */
#define TMAGIC   "ustar"  /* ustar plus null byte */
#define TMAGLEN  6         /* length of TMAGIC including null */
#define TVERSION "00"      /* 00 without null byte */
#define TVERSLEN 2         /* length of TVERSION */

/* Typeflag field values */
#define REGTYPE  '0'  /* regular file */
#define AREGTYPE '\0' /* regular file (alternate) */
#define LNKTYPE  '1'  /* hard link */
#define SYMTYPE  '2'  /* symbolic link */
#define CHRTYPE  '3'  /* character special */
#define BLKTYPE  '4'  /* block special */
#define DIRTYPE  '5'  /* directory */
#define FIFOTYPE '6'  /* FIFO special */
#define CONTTYPE '7'  /* contiguous file (reserved) */

/* Mode field bit definitions (octal) */
#define TSUID   04000  /* set UID on execution */
#define TSGID   02000  /* set GID on execution */
#define TSVTX   01000  /* sticky bit */
#define TUREAD  00400  /* owner read */
#define TUWRITE 00200  /* owner write */
#define TUEXEC  00100  /* owner execute */
#define TGREAD  00040  /* group read */
#define TGWRITE 00020  /* group write */
#define TGEXEC  00010  /* group execute */
#define TOREAD  00004  /* other read */
#define TOWRITE 00002  /* other write */
#define TOEXEC  00001  /* other execute */

#endif /* _TAR_H */

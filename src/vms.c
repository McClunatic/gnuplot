/* GNUPLOT - vms.c */

/*[
 * Copyright 1986 - 1993, 1998, 2004   Thomas Williams, Colin Kelley
 *
 * Permission to use, copy, and distribute this software and its
 * documentation for any purpose with or without fee is hereby granted,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.
 *
 * Permission to modify the software is granted, but not the right to
 * distribute the complete modified source code.  Modifications are to
 * be distributed as patches to the released version.  Permission to
 * distribute binaries produced by compiling modified sources is granted,
 * provided you
 *   1. distribute the corresponding source modifications from the
 *    released version in the form of a patch file along with the binaries,
 *   2. add special version identification to distinguish your version
 *    in addition to the base release version number,
 *   3. provide your name and address as the primary contact for the
 *    support of your modified version, and
 *   4. retain our contact information in regard to use of the base
 *    software.
 * Permission to distribute the released version of the source code along
 * with corresponding source modifications in the form of a patch file is
 * granted with same provisions 2 through 4 for binary distributions.
 *
 * This software is provided "as is" without express or implied warranty
 * to the extent permitted by applicable law.
]*/

/* drop in popen() / pclose() for VMS
 * (originally written by drd for port of perl to vms)
 */

#include "syscfg.h"     /* for the prototypes */
#include "stdfn.h"


/* these are needed to modify terminal characteristics */
# ifndef VWS_XMAX
   /* avoid duplicate warning; VWS includes these */
#  include <descrip.h>
#  include <ssdef.h>
# endif                         /* !VWS_MAX */
# include <iodef.h>
# include <ttdef.h>
# include <tt2def.h>
# include <dcdef.h>
# include <stat.h>
# include <fab.h>

static int something_in_this_file;

int vms_vkid;	/* Virtual keyboard id */
int vms_ktid;	/* key table id, for translating keystrokes */

#ifdef PIPES

/* (to aid porting) - how are errors dealt with */

#define ERROR(msg) { fprintf(stderr, "%s\nFile %s line %d\n", msg, __FILE__, __LINE__); }
#define FATAL(msg) { fprintf(stderr, "%s\nFile %s line %d\n", msg, __FILE__, __LINE__); exit(EXIT_FAILURE); }


#include <dvidef.h>
#include <syidef.h>
#include <jpidef.h>
#include <ssdef.h>
#include <descrip.h>

#ifdef __DECC             /* DECC does not automatically search */
#include <lib$routines.h>
#include <starlet.h>      /* for the sys$... routines */
#endif  /* __DECC */

#ifndef EXIT_FAILURE                  /* not in older VAXC <stdlib.h> */
#define EXIT_FAILURE 0x10000002       /* (STS$K_ERROR | STS$M_INHIB_MSG */
#endif

#define _cksts(call) \
  if (!(sts=(call))&1) FATAL("Internal error") else {}

static void
create_mbx(unsigned short int *chan, struct dsc$descriptor_s *namdsc)
{
	static unsigned long int mbxbufsiz;
		long int syiitm = SYI$_MAXBUF, dviitm = DVI$_DEVNAM;
	unsigned long sts;  /* for _cksts */

  if (!mbxbufsiz) {
    /*
     * Get the SYSGEN parameter MAXBUF, and the smaller of it and the
     * preprocessor consant BUFSIZ from stdio.h as the size of the
     * 'pipe' mailbox.
     */

    _cksts(lib$getsyi(&syiitm, &mbxbufsiz, 0, 0, 0, 0));
    if (mbxbufsiz > BUFSIZ) mbxbufsiz = BUFSIZ;
  }
  _cksts(sys$crembx(0,chan,mbxbufsiz,mbxbufsiz,0,0,0));

  _cksts(lib$getdvi(&dviitm, chan, NULL, NULL, namdsc, &namdsc->dsc$w_length));
  namdsc->dsc$a_pointer[namdsc->dsc$w_length] = '\0';

}  /* end of create_mbx() */

struct pipe_details
{
    struct pipe_details *next;
    FILE *fp;
    int pid;
    unsigned long int completion;
};

static struct pipe_details *open_pipes = NULL;
static $DESCRIPTOR(nl_desc, "NL:");
static int waitpid_asleep = 0;

static void
popen_completion_ast(unsigned long int unused)
{
  if (waitpid_asleep) {
    waitpid_asleep = 0;
    sys$wake(0,0);
  }
}

FILE *
popen(char *cmd, char *mode)
{
    static char mbxname[64];
    unsigned short int chan;
    unsigned long int flags=1;  /* nowait - gnu c doesn't allow &1 */
    struct pipe_details *info;
    struct dsc$descriptor_s namdsc = {sizeof mbxname, DSC$K_DTYPE_T,
                                      DSC$K_CLASS_S, mbxname},
                            cmddsc = {0, DSC$K_DTYPE_T,
                                      DSC$K_CLASS_S, 0};
	unsigned long sts;

    if (!(info=malloc(sizeof(struct pipe_details))))
    {
    	ERROR("Cannot malloc space");
    	return NULL;
    }

    info->completion=0;  /* I assume this will remain 0 until terminates */

    /* create mailbox */
    create_mbx(&chan,&namdsc);

    /* open a FILE* onto it */
    info->fp=fopen(mbxname, mode);

    /* give up other channel onto it */
    _cksts(sys$dassgn(chan));

    if (!info->fp)
        return NULL;

    cmddsc.dsc$w_length=strlen(cmd);
    cmddsc.dsc$a_pointer=cmd;

    if (strcmp(mode,"r")==0) {
      _cksts(lib$spawn(&cmddsc, &nl_desc, &namdsc, &flags,
                     0  /* name */, &info->pid, &info->completion,
                     0, popen_completion_ast,0,0,0,0));
    }
    else {
      _cksts(lib$spawn(&cmddsc, &namdsc, 0 /* sys$output */, &flags,
                     0  /* name */, &info->pid, &info->completion));
    }

    info->next=open_pipes;  /* prepend to list */
    open_pipes=info;

    return info->fp;
}

int pclose(FILE *fp)
{
    struct pipe_details *info, *last = NULL;
    unsigned long int abort = SS$_TIMEOUT, retsts;
    unsigned long sts;

    for (info = open_pipes; info != NULL; last = info, info = info->next)
        if (info->fp == fp) break;

    if (info == NULL)
      /* get here => no such pipe open */
      FATAL("pclose() - no such pipe open ???");

    if (!info->completion) { /* Tap them gently on the shoulder . . .*/
      _cksts(sys$forcex(&info->pid,0,&abort));
      sleep(1);
    }
    if (!info->completion)  /* We tried to be nice . . . */
      _cksts(sys$delprc(&info->pid));

    fclose(info->fp);
    /* remove from list of open pipes */
    if (last) last->next = info->next;
    else open_pipes = info->next;
    retsts = info->completion;
    free(info);

    return retsts;
}  /* end of pclose() */


/* sort-of waitpid; use only with popen() */
/*{{{unsigned long int waitpid(unsigned long int pid, int *statusp, int flags)*/
unsigned long int
waitpid(unsigned long int pid, int *statusp, int flags)
{
    struct pipe_details *info;
    unsigned long int abort = SS$_TIMEOUT;
    unsigned long sts;

    for (info = open_pipes; info != NULL; info = info->next)
        if (info->pid == pid) break;

    if (info != NULL) {  /* we know about this child */
      while (!info->completion) {
        waitpid_asleep = 1;
        sys$hiber();
      }

      *statusp = info->completion;
      return pid;
    }
    else {  /* we haven't heard of this child */
      $DESCRIPTOR(intdsc,"0 00:00:01");
      unsigned long int ownercode = JPI$_OWNER, ownerpid, mypid;
      unsigned long int interval[2];

      _cksts(lib$getjpi(&ownercode,&pid,0,&ownerpid,0,0));
      _cksts(lib$getjpi(&ownercode,0,0,&mypid,0,0));
      if (ownerpid != mypid)
        FATAL("pid not a child");

      _cksts(sys$bintim(&intdsc,interval));
      while ((sts=lib$getjpi(&ownercode,&pid,0,&ownerpid,0,0)) & 1) {
        _cksts(sys$schdwk(0,0,interval,0));
        _cksts(sys$hiber());
      }
      _cksts(sts);

      /* There's no easy way to find the termination status a child we're
       * not aware of beforehand.  If we're really interested in the future,
       * we can go looking for a termination mailbox, or chase after the
       * accounting record for the process.
       */
      *statusp = 0;
      return pid;
    }

}  /* end of waitpid() */

#endif /* PIPES */

/*
 * Since vax/vms is the only remaining user of locally-coded strftime,
 * move gnuplot's original strftime.c here.
 */
#ifdef VAXCRTL
/*
 * Implementation of strftime for systems missing this (e.g. vaxctrl)
 *
 * This code was written based on the NeXT strftime man-page, sample output of
 * the function and an ANSI-C quickreference chart. This code does not use
 * parts of any existing strftime implementation.
 *
 * Apparently not all format chars are implemented, but this was all I had in
 * my documentation.
 *
 * (written by Alexander Lehmann)
 */

#define NOTIMEZONE

static void fill(char *from, char **pto, size_t *pmaxsize);
static void number(int num, int pad, char **pto, size_t *pmaxsize);

static void
fill(char *from, char **pto, size_t *pmaxsize)
{
    safe_strncpy(*pto, from, *pmaxsize);
    if (*pmaxsize < strlen(from)) {
	(*pto) += *pmaxsize;
	*pmaxsize = 0;
    } else {
	(*pto) += strlen(from);
	(*pmaxsize) -= strlen(from);
    }
}

static void
number(int num, int pad, char **pto, size_t *pmaxsize)
{
    char str[100];

    sprintf(str, "%0*d", pad, num);
    fill(str, pto, pmaxsize);
}

size_t
strftime(char *s, size_t max, const char *format, const struct tm *tp)
{
    char *start = s;
    size_t maxsize = max;

    if (max > 0) {
	while (*format && max > 0) {
	    if (*format != '%') {
		*s++ = *format++;
		max--;
	    } else {
		format++;
		switch (*format++) {
		case 'a':	/* abbreviated weekday name */
		    if (tp->tm_wday >= 0 && tp->tm_wday <= 6)
			fill(abbrev_day_names[tp->tm_wday], &s, &max);
		    break;
		case 'A':	/* full name of the weekday */
		    if (tp->tm_wday >= 0 && tp->tm_wday <= 6)
			fill(full_day_names[tp->tm_wday], &s, &max);
		    break;
		case 'b':	/* abbreviated month name */
		    if (tp->tm_mon >= 0 && tp->tm_mon <= 11)
			fill(abbrev_month_names[tp->tm_mon], &s, &max);
		    break;
		case 'B':	/* full name of month */
		    if (tp->tm_mon >= 0 && tp->tm_mon <= 11)
			fill(full_month_names[tp->tm_mon], &s, &max);
		    break;
		case 'c':	/* locale's date and time representation */
		    strftime(s, max, "%a %b %X %Y", tp);
		    max -= strlen(s);
		    s += strlen(s);
		    break;
		case 'd':	/* day of the month (01-31) */
		    number(tp->tm_mday, 2, &s, &max);
		    break;
		case 'H':	/* hour of the day (00-23) */
		    number(tp->tm_hour, 2, &s, &max);
		    break;
		case 'I':	/* hour of the day (01-12) */
		    number((tp->tm_hour + 11) % 12 + 1, 2, &s, &max);
		    break;
		case 'j':	/* day of the year (001-366) */
		    number(tp->tm_yday + 1, 3, &s, &max);
		    break;
		case 'm':	/* month of the year (01-12) */
		    number(tp->tm_mon + 1, 2, &s, &max);
		    break;
		case 'M':	/* minute (00-59) */
		    number(tp->tm_min, 2, &s, &max);
		    break;
		case 'p':	/* locale's version of AM or PM */
		    fill(tp->tm_hour >= 6 ? "PM" : "AM", &s, &max);
		    break;
		case 'S':	/* seconds (00-59) */
		    number(tp->tm_sec, 2, &s, &max);
		    break;
		case 'U':	/* week number of the year (00-53) with Sunday as the first day of the week */
		    number((tp->tm_yday - (tp->tm_yday - tp->tm_wday + 7) % 7 + 7) / 7, 1, &s, &max);
		    break;
		case 'w':	/* weekday (Sunday = 0 to Saturday = 6) */
		    number(tp->tm_wday, 1, &s, &max);
		    break;
		case 'W':	/* week number of the year (00-53) with Monday as the first day of the week */
		    number((tp->tm_yday - (tp->tm_yday - tp->tm_wday + 8) % 7 + 7) / 7, 2, &s, &max);
		    break;
		case 'x':	/* locale's date representation */
		    strftime(s, max, "%a %b %d %Y", tp);
		    max -= strlen(s);
		    s += strlen(s);
		    break;
		case 'X':	/* locale's time representation */
#ifndef NOTIMEZONE
		    strftime(s, max, "%H:%M:%S %Z", tp);
#else
		    strftime(s, max, "%H:%M:%S", tp);
#endif
		    max -= strlen(s);
		    s += strlen(s);
		    break;
		case 'y':	/* two-digit year representation (00-99) */
		    number(tp->tm_year % 100, 2, &s, &max);
		    break;
		case 'Y':	/* four-digit year representation */
		    number(tp->tm_year + 1900, 2, &s, &max);
		    break;
#ifndef NOTIMEZONE
		case 'Z':	/* time zone name */
		    fill(tp->tm_zone, &s, &max);
		    break;
#endif
		case '%':	/* percent sign */
		default:
		    *s++ = *(format - 1);
		    max--;
		    break;
		}
	    }
	}
	if (s - start < maxsize) {
	    *s++ = '\0';
	} else {
	    *(s - 1) = '\0';
	}
    }
    return s - start;
}
#endif /* VAXCRTL */

/*
 * Everything below here moved out of term.c in Jan 2019
 * It is quite possible that additional #includes are needed for this to compile
 */

# ifndef TT2$M_DECCRT3          /* VT300 not defined as of VAXC v2.4 */
#  define TT2$M_DECCRT3 0X80000000
# endif

static unsigned short chan;
static int old_char_buf[3], cur_char_buf[3];
$DESCRIPTOR(sysoutput_desc, "SYS$OUTPUT");

/* Look first for decw$display (decterms do regis).  Determine if we
 * have a regis terminal and save terminal characteristics */
char *
vms_init()
{
    /* Save terminal characteristics in old_char_buf and
       initialise cur_char_buf to current settings. */
    int i;
#ifdef X11
    if (getenv("DECW$DISPLAY"))
	return ("x11");
#endif
    atexit(vms_reset);
    sys$assign(&sysoutput_desc, &chan, 0, 0);
    sys$qiow(0, chan, IO$_SENSEMODE, 0, 0, 0, old_char_buf, 12, 0, 0, 0, 0);
    for (i = 0; i < 3; ++i)
	cur_char_buf[i] = old_char_buf[i];
    sys$dassgn(chan);

    /* Test if terminal is regis */
    if ((cur_char_buf[2] & TT2$M_REGIS) == TT2$M_REGIS)
	return ("regis");
    return (NULL);
}

/* set terminal to original state */
void
vms_reset()
{
    int i;

    sys$assign(&sysoutput_desc, &chan, 0, 0);
    sys$qiow(0, chan, IO$_SETMODE, 0, 0, 0, old_char_buf, 12, 0, 0, 0, 0);
    for (i = 0; i < 3; ++i)
	cur_char_buf[i] = old_char_buf[i];
    sys$dassgn(chan);
}

/* set terminal mode to tektronix */
void
term_mode_tek()
{
    long status;

    if (gpoutfile != stdout)
	return;                 /* don't modify if not stdout */
    sys$assign(&sysoutput_desc, &chan, 0, 0);
    cur_char_buf[0] = 0x004A0000 | DC$_TERM | (TT$_TEK401X << 8);
    cur_char_buf[1] = (cur_char_buf[1] & 0x00FFFFFF) | 0x18000000;

    cur_char_buf[1] &= ~TT$M_CRFILL;
    cur_char_buf[1] &= ~TT$M_ESCAPE;
    cur_char_buf[1] &= ~TT$M_HALFDUP;
    cur_char_buf[1] &= ~TT$M_LFFILL;
    cur_char_buf[1] &= ~TT$M_MECHFORM;
    cur_char_buf[1] &= ~TT$M_NOBRDCST;
    cur_char_buf[1] &= ~TT$M_NOECHO;
    cur_char_buf[1] &= ~TT$M_READSYNC;
    cur_char_buf[1] &= ~TT$M_REMOTE;
    cur_char_buf[1] |= TT$M_LOWER;
    cur_char_buf[1] |= TT$M_TTSYNC;
    cur_char_buf[1] |= TT$M_WRAP;
    cur_char_buf[1] &= ~TT$M_EIGHTBIT;
    cur_char_buf[1] &= ~TT$M_MECHTAB;
    cur_char_buf[1] &= ~TT$M_SCOPE;
    cur_char_buf[1] |= TT$M_HOSTSYNC;

    cur_char_buf[2] &= ~TT2$M_APP_KEYPAD;
    cur_char_buf[2] &= ~TT2$M_BLOCK;
    cur_char_buf[2] &= ~TT2$M_DECCRT3;
    cur_char_buf[2] &= ~TT2$M_LOCALECHO;
    cur_char_buf[2] &= ~TT2$M_PASTHRU;
    cur_char_buf[2] &= ~TT2$M_REGIS;
    cur_char_buf[2] &= ~TT2$M_SIXEL;
    cur_char_buf[2] |= TT2$M_BRDCSTMBX;
    cur_char_buf[2] |= TT2$M_EDITING;
    cur_char_buf[2] |= TT2$M_INSERT;
    cur_char_buf[2] |= TT2$M_PRINTER;
    cur_char_buf[2] &= ~TT2$M_ANSICRT;
    cur_char_buf[2] &= ~TT2$M_AVO;
    cur_char_buf[2] &= ~TT2$M_DECCRT;
    cur_char_buf[2] &= ~TT2$M_DECCRT2;
    cur_char_buf[2] &= ~TT2$M_DRCS;
    cur_char_buf[2] &= ~TT2$M_EDIT;
    cur_char_buf[2] |= TT2$M_FALLBACK;

    status = sys$qiow(0, chan, IO$_SETMODE, 0, 0, 0, cur_char_buf, 12, 0, 0, 0, 0);
    if (status == SS$_BADPARAM) {
	/* terminal fallback utility not installed on system */
	cur_char_buf[2] &= ~TT2$M_FALLBACK;
	sys$qiow(0, chan, IO$_SETMODE, 0, 0, 0, cur_char_buf, 12, 0, 0, 0, 0);
    } else {
	if (status != SS$_NORMAL)
	    lib$signal(status, 0, 0);
    }
    sys$dassgn(chan);
}

/* set terminal mode back to native */
void
term_mode_native()
{
    int i;

    if (gpoutfile != stdout)
	return;                 /* don't modify if not stdout */
    sys$assign(&sysoutput_desc, &chan, 0, 0);
    sys$qiow(0, chan, IO$_SETMODE, 0, 0, 0, old_char_buf, 12, 0, 0, 0, 0);
    for (i = 0; i < 3; ++i)
	cur_char_buf[i] = old_char_buf[i];
    sys$dassgn(chan);
}

/* set terminal mode pasthru */
void
term_pasthru()
{
    if (gpoutfile != stdout)
	return;                 /* don't modify if not stdout */
    sys$assign(&sysoutput_desc, &chan, 0, 0);
    cur_char_buf[2] |= TT2$M_PASTHRU;
    sys$qiow(0, chan, IO$_SETMODE, 0, 0, 0, cur_char_buf, 12, 0, 0, 0, 0);
    sys$dassgn(chan);
}

/* set terminal mode nopasthru */
void
term_nopasthru()
{
    if (gpoutfile != stdout)
	return;                 /* don't modify if not stdout */
    sys$assign(&sysoutput_desc, &chan, 0, 0);
    cur_char_buf[2] &= ~TT2$M_PASTHRU;
    sys$qiow(0, chan, IO$_SETMODE, 0, 0, 0, cur_char_buf, 12, 0, 0, 0, 0);
    sys$dassgn(chan);
}

void
fflush_binary()
{
    typedef short int INT16;    /* signed 16-bit integers */
    INT16 k;            /* loop index */

    if (gpoutfile != stdout) {
	/* Stupid VMS fflush() raises error and loses last data block
	   unless it is full for a fixed-length record binary file.
	   Pad it here with NULL characters. */
	for (k = (INT16) ((*gpoutfile)->_cnt); k > 0; --k)
	    putc('\0', gpoutfile);
	fflush(gpoutfile);
    }
}

void
done(int status)
{
    term_reset();
    gp_exit(status);
}

void
vms_init_screen()
{
    /* initialise screen management routines for command recall */
    unsigned int ierror;
    if (ierror = smg$create_virtual_keyboard(&vms_vkid) != SS$_NORMAL)
	done(ierror);
    if (ierror = smg$create_key_table(&vms_ktid) != SS$_NORMAL)
	done(ierror);
}


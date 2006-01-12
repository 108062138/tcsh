/* $Header: /src/pub/tcsh/sh.glob.c,v 3.66 2006/01/12 18:15:25 christos Exp $ */
/*
 * sh.glob.c: Regular expression expansion
 */
/*-
 * Copyright (c) 1980, 1991 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "sh.h"

RCSID("$Id: sh.glob.c,v 3.66 2006/01/12 18:15:25 christos Exp $")

#include "tc.h"
#include "tw.h"

#include "glob.h"

static int pargsiz, gargsiz;

/*
 * Values for gflag
 */
#define	G_NONE	0		/* No globbing needed			*/
#define	G_GLOB	1		/* string contains *?[] characters	*/
#define	G_CSH	2		/* string contains ~`{ characters	*/

#define	GLOBSPACE	100	/* Alloc increment			*/


#define LBRC '{'
#define RBRC '}'
#define LBRK '['
#define RBRK ']'
#define EOS '\0'

Char  **gargv = NULL;
int     gargc = 0;
Char  **pargv = NULL;
static int pargc = 0;

/*
 * globbing is now done in two stages. In the first pass we expand
 * csh globbing idioms ~`{ and then we proceed doing the normal
 * globbing if needed ?*[
 *
 * Csh type globbing is handled in globexpand() and the rest is
 * handled in glob() which is part of the 4.4BSD libc.
 *
 */
static	Char	 *globtilde	(Char **, Char *);
static	Char     *handleone	(Char *, Char **, int);
static	Char	**libglob	(Char **);
static	Char	**globexpand	(Char **, int);
static	int	  globbrace	(const Char *, Char ***);
static  void	  expbrace	(Char ***, Char ***, int);
static	void	  pword		(struct Strbuf *);
static	void	  backeval	(struct Strbuf *, Char *, int);

static Char *
globtilde(Char **nv, Char *s)
{
    Char *name, *u, *home, *res;

    u = s;
    for (s++; *s && *s != '/' && *s != ':'; s++)
	continue;
    name = Strnsave(u + 1, s - (u + 1));
    home = gethdir(name);
    if (home == NULL) {
	if (adrof(STRnonomatch)) {
	    xfree(name);
	    return u;
	}
	blkfree(nv);
	if (*name) {
	    char *cname;

	    cname = short2str(name);
	    xfree(name);
	    stderror(ERR_UNKUSER, cname);
	} else {
	    xfree(name);
	    stderror(ERR_NOHOME);
	}
    }
    xfree(name);
    if (home[0] == '/' && home[1] == '\0' && s[0] == '/')
	res = Strsave(s);
    else
	res = Strspl(home, s);
    xfree(home);
    xfree(u);
    return res;
}

/* Returns a newly allocated string, old or NULL */
Char *
globequal(Char *old)
{
    int     dig;
    const Char *dir;
    Char    *b;

    /*
     * kfk - 17 Jan 1984 - stack hack allows user to get at arbitrary dir names
     * in stack. PWP: let =foobar pass through (for X windows)
     */
    if (old[1] == '-' && (old[2] == '\0' || old[2] == '/')) {
	/* =- */
	const Char *olddir = varval (STRowd);

	if (olddir && *olddir &&
	    !dcwd->di_next->di_name && !dcwd->di_prev->di_name)
	    return Strspl(olddir, &old[2]);
	dig = -1;
	b = &old[2];
    }
    else if (Isdigit(old[1])) {
	/* =<number> */
	dig = old[1] - '0';
	for (b = &old[2]; Isdigit(*b); b++)
	    dig = dig * 10 + (*b - '0');
	if (*b != '\0' && *b != '/')
	    /* =<number>foobar */
	    return old;
    }
    else
	/* =foobar */
	return old;

    dir = getstakd(dig);
    if (dir == NULL)
	return NULL;
    return Strspl(dir, b);
}

static int
globbrace(const Char *s, Char ***bl)
{
    struct Strbuf gbuf = Strbuf_INIT;
    int     i, len;
    const Char *p, *pm, *pe, *pl;
    Char  **nv, **vl;
    size_t prefix_len, size = GLOBSPACE;

    len = 0;
    /* copy part up to the brace */
    for (p = s; *p != LBRC; p++)
	;
    prefix_len = p - s;

    /* check for balanced braces */
    for (i = 0, pe = ++p; *pe; pe++)
	if (*pe == LBRK) {
	    /* Ignore everything between [] */
	    for (++pe; *pe != RBRK && *pe != EOS; pe++)
		continue;
	    if (*pe == EOS)
		return (-RBRK);
	}
	else if (*pe == LBRC)
	    i++;
	else if (*pe == RBRC) {
	    if (i == 0)
		break;
	    i--;
	}

    if (i != 0 || *pe == '\0')
	return (-RBRC);

    nv = vl = xmalloc(sizeof(Char *) * size);
    *vl = NULL;
    Strbuf_appendn(&gbuf, s, prefix_len);

    for (i = 0, pl = pm = p; pm <= pe; pm++)
	switch (*pm) {
	case LBRK:
	    for (++pm; *pm != RBRK && *pm != EOS; pm++)
		continue;
	    if (*pm == EOS) {
		*vl = NULL;
		blkfree(nv);
		xfree(gbuf.s);
		return (-RBRK);
	    }
	    break;
	case LBRC:
	    i++;
	    break;
	case RBRC:
	    if (i) {
		i--;
		break;
	    }
	    /* FALLTHROUGH */
	case ',':
	    if (i && *pm == ',')
		break;
	    else {
		gbuf.len = prefix_len;
		Strbuf_appendn(&gbuf, pl, pm - pl);
		Strbuf_append(&gbuf, pe + 1);
		Strbuf_terminate(&gbuf);
		*vl++ = Strsave(gbuf.s);
		len++;
		pl = pm + 1;
		if (vl == &nv[size]) {
		    size += GLOBSPACE;
		    nv = xrealloc(nv, size * sizeof(Char *));
		    vl = &nv[size - GLOBSPACE];
		}
	    }
	    break;
	default:
	    break;
	}
    *vl = NULL;
    *bl = nv;
    xfree(gbuf.s);
    return (len);
}


static void
expbrace(Char ***nvp, Char ***elp, int size)
{
    Char **vl, **el, **nv, *s;

    vl = nv = *nvp;
    if (elp != NULL)
	el = *elp;
    else
	el = vl + blklen(vl);

    for (s = *vl; s; s = *++vl) {
	Char  **vp, **bp;

	/* leave {} untouched for find */
	if (s[0] == '{' && (s[1] == '\0' || (s[1] == '}' && s[2] == '\0')))
	    continue;
	if (Strchr(s, '{') != NULL) {
	    Char  **bl;
	    int     len;

	    if ((len = globbrace(s, &bl)) < 0) {
		xfree(nv);
		stderror(ERR_MISSING, -len);
	    }
	    xfree(s);
	    if (len == 1) {
		*vl-- = *bl;
		xfree(bl);
		continue;
	    }
	    if (&el[len] >= &nv[size]) {
		size_t l, e;
		l = &el[len] - &nv[size];
		size += GLOBSPACE > l ? GLOBSPACE : l;
		l = vl - nv;
		e = el - nv;
		nv = xrealloc(nv, size * sizeof(Char *));
		vl = nv + l;
		el = nv + e;
	    }
	    /* nv vl   el     bl
	     * |  |    |      |
	     * -.--..--	      x--
	     *   |            len
	     *   vp
	     */
	    vp = vl--;
	    *vp = *bl;
	    len--;
	    for (bp = el; bp != vp; bp--)
		bp[len] = *bp;
	    el += len;
	    /* nv vl    el bl
	     * |  |     |  |
	     * -.-x  ---    --
	     *   |len
	     *   vp
	     */
	    vp++;
	    for (bp = bl + 1; *bp; *vp++ = *bp++)
		continue;
	    xfree(bl);
	}

    }
    if (elp != NULL)
	*elp = el;
    *nvp = nv;
}

static Char **
globexpand(Char **v, int noglob)
{
    Char   *s;
    Char  **nv, **vl, **el;
    int     size = GLOBSPACE;


    nv = vl = xmalloc(sizeof(Char *) * size);
    *vl = NULL;

    /*
     * Step 1: expand backquotes.
     */
    while ((s = *v++) != '\0') {
	if (Strchr(s, '`')) {
	    int     i;

	    (void) dobackp(s, 0);
	    for (i = 0; i < pargc; i++) {
		*vl++ = pargv[i];
		if (vl == &nv[size]) {
		    size += GLOBSPACE;
		    nv = xrealloc(nv, size * sizeof(Char *));
		    vl = &nv[size - GLOBSPACE];
		}
	    }
	    xfree(pargv);
	    pargv = NULL;
	}
	else {
	    *vl++ = Strsave(s);
	    if (vl == &nv[size]) {
		size += GLOBSPACE;
		nv = xrealloc(nv, size * sizeof(Char *));
		vl = &nv[size - GLOBSPACE];
	    }
	}
    }
    *vl = NULL;

    if (noglob)
	return (nv);

    /*
     * Step 2: expand braces
     */
    el = vl;
    expbrace(&nv, &el, size);


    /*
     * Step 3: expand ~ =
     */
    vl = nv;
    for (s = *vl; s; s = *++vl)
	switch (*s) {
	    Char *ns;
	case '~':
	    *vl = globtilde(nv, s);
	    break;
	case '=':
	    if ((ns = globequal(s)) == NULL) {
		if (!adrof(STRnonomatch)) {
		    /* Error */
		    blkfree(nv);
		    stderror(ERR_DEEP);
		}
	    }
	    if (ns && ns != s) {
		/* Expansion succeeded */
		xfree(s);
		*vl = ns;
	    }
	    break;
	default:
	    break;
	}
    vl = nv;

    /*
     * Step 4: expand .. if the variable symlinks==expand is set
     */
    if (symlinks == SYM_EXPAND) {
	for (s = *vl; s; s = *++vl) {
	    *vl = dnormalize(s, 1);
	    xfree(s);
	}
    }

    return nv;
}

static Char *
handleone(Char *str, Char **vl, int action)
{
    size_t chars;
    Char **t, *p, *strp;

    switch (action) {
    case G_ERROR:
	setname(short2str(str));
	blkfree(vl);
	stderror(ERR_NAME | ERR_AMBIG);
	break;
    case G_APPEND:
	chars = 0;
	for (t = vl; (p = *t++) != NULL; chars++)
	    chars += Strlen(p);
	str = xmalloc(chars * sizeof(Char));
	for (t = vl, strp = str; (p = *t++) != '\0'; chars++) {
	    while (*p)
		 *strp++ = *p++ & TRIM;
	    *strp++ = ' ';
	}
	*--strp = '\0';
	blkfree(vl);
	break;
    case G_IGNORE:
	str = Strsave(strip(*vl));
	blkfree(vl);
	break;
    default:
	break;
    }
    return (str);
}

static Char **
libglob(Char **vl)
{
    int     gflgs = GLOB_QUOTE | GLOB_NOMAGIC | GLOB_ALTNOT;
    glob_t  globv;
    char   *ptr;
    int     nonomatch = adrof(STRnonomatch) != 0, magic = 0, match = 0;

    if (!vl || !vl[0])
	return(vl);

    globv.gl_offs = 0;
    globv.gl_pathv = 0;
    globv.gl_pathc = 0;

    if (nonomatch)
	gflgs |= GLOB_NOCHECK;

    do {
	ptr = short2qstr(*vl);
	switch (glob(ptr, gflgs, 0, &globv)) {
	case GLOB_ABEND:
	    globfree(&globv);
	    setname(ptr);
	    stderror(ERR_NAME | ERR_GLOB);
	    /* NOTREACHED */
	case GLOB_NOSPACE:
	    globfree(&globv);
	    stderror(ERR_NOMEM);
	    /* NOTREACHED */
	default:
	    break;
	}
	if (globv.gl_flags & GLOB_MAGCHAR) {
	    match |= (globv.gl_matchc != 0);
	    magic = 1;
	}
	gflgs |= GLOB_APPEND;
    }
    while (*++vl);
    vl = (globv.gl_pathc == 0 || (magic && !match && !nonomatch)) ? 
	NULL : blk2short(globv.gl_pathv);
    globfree(&globv);
    return (vl);
}

Char   *
globone(Char *str, int action)
{
    Char   *v[2], **vl, **vo;
    int gflg, noglob;

    noglob = adrof(STRnoglob) != 0;
    v[0] = str;
    v[1] = 0;
    gflg = tglob(v);
    if (gflg == G_NONE)
	return (strip(Strsave(str)));

    if (gflg & G_CSH) {
	/*
	 * Expand back-quote, tilde and brace
	 */
	vo = globexpand(v, noglob);
	if (noglob || (gflg & G_GLOB) == 0) {
	    vl = vo;
	    goto result;
	}
    }
    else if (noglob || (gflg & G_GLOB) == 0)
	return (strip(Strsave(str)));
    else
	vo = v;

    vl = libglob(vo);
    if ((gflg & G_CSH) && vl != vo)
	blkfree(vo);
    if (vl == NULL) {
	setname(short2str(str));
	stderror(ERR_NAME | ERR_NOMATCH);
    }
 result:
    if (vl[0] == NULL) {
	xfree(vl);
	return (Strsave(STRNULL));
    }
    if (vl[1]) 
	return (handleone(str, vl, action));
    else {
	str = strip(*vl);
	xfree(vl);
	return (str);
    }
}

Char  **
globall(Char **v, int gflg)
{
    Char  **vl, **vo;
    int noglob;

    if (!v || !v[0]) {
	gargv = saveblk(v);
	gargc = blklen(gargv);
	return (gargv);
    }

    noglob = adrof(STRnoglob) != 0;

    if (gflg & G_CSH)
	/*
	 * Expand back-quote, tilde and brace
	 */
	vl = vo = globexpand(v, noglob);
    else
	vl = vo = saveblk(v);

    if (!noglob && (gflg & G_GLOB)) {
	vl = libglob(vo);
	if (vl != vo)
	    blkfree(vo);
    }
    else
	trim(vl);

    gargc = vl ? blklen(vl) : 0;
    return (gargv = vl);
}

void
ginit(void)
{
    gargsiz = GLOBSPACE;
    gargv = xmalloc(sizeof(Char *) * gargsiz);
    gargv[0] = 0;
    gargc = 0;
}

void
rscan(Char **t, void (*f) (Char))
{
    Char *p;

    while ((p = *t++) != '\0')
	while (*p)
	    (*f) (*p++);
}

void
trim(Char **t)
{
    Char *p;

    while ((p = *t++) != '\0')
	while (*p)
	    *p++ &= TRIM;
}

int
tglob(Char **t)
{
    int gflag;
    const Char *p;

    gflag = 0;
    while ((p = *t++) != '\0') {
	if (*p == '~' || *p == '=')
	    gflag |= G_CSH;
	else if (*p == '{' &&
		 (p[1] == '\0' || (p[1] == '}' && p[2] == '\0')))
	    continue;
	while (*p != '\0') {
	    if (*p == '`') {
		gflag |= G_CSH;
#ifdef notdef
		/*
		 * We do want to expand echo `echo '*'`, so we don't\
		 * use this piece of code anymore.
		 */
		p++;
		while (*p && *p != '`') 
		    if (*p++ == '\\') {
			if (*p)		/* Quoted chars */
			    p++;
			else
			    break;
		    }
		if (!*p)		/* The matching ` */
		    break;
#endif
	    }
	    else if (*p == '{')
		gflag |= G_CSH;
	    else if (isglob(*p))
		gflag |= G_GLOB;
	    else if (symlinks == SYM_EXPAND && 
		p[1] && ISDOTDOT(p) && (p == *(t-1) || *(p-1) == '/') )
	    	gflag |= G_CSH;
	    p++;
	}
    }
    return gflag;
}

/*
 * Command substitute cp.  If literal, then this is a substitution from a
 * << redirection, and so we should not crunch blanks and tabs, separating
 * words only at newlines.
 */
Char  **
dobackp(Char *cp, int literal)
{
    struct Strbuf word = Strbuf_INIT;
    Char *lp, *rp, *ep;

    if (pargv) {
#ifdef notdef
	abort();
#endif
	blkfree(pargv);
    }
    pargsiz = GLOBSPACE;
    pargv = xmalloc(sizeof(Char *) * pargsiz);
    pargv[0] = NULL;
    pargc = 0;
    for (;;) {
	for (lp = cp; *lp != '\0' && *lp != '`'; lp++)
	    ;
	Strbuf_appendn(&word, cp, lp - cp);
	if (*lp == 0) {
	    if (word.len != 0)
		pword(&word);
	    return pargv;
	}
	lp++;
	for (rp = lp; *rp && *rp != '`'; rp++)
	    if (*rp == '\\') {
		rp++;
		if (!*rp)
		    goto oops;
	    }
	if (!*rp) {
	oops: xfree(word.s);
	    stderror(ERR_UNMATCHED, '`');
	}
	ep = Strsave(lp);
	ep[rp - lp] = 0;
	backeval(&word, ep, literal);
	cp = rp + 1;
    }
}


static void
backeval(struct Strbuf *word, Char *cp, int literal)
{
    int icnt;
    Char c, *ip;
    struct command faket;
    int    hadnl;
    int     pvec[2], quoted;
    Char   *fakecom[2], ibuf[BUFSIZE];
    char    tibuf[BUFSIZE];

    hadnl = 0;
    icnt = 0;
    quoted = (literal || (cp[0] & QUOTE)) ? QUOTE : 0;
    faket.t_dtyp = NODE_COMMAND;
    faket.t_dflg = F_BACKQ;
    faket.t_dlef = 0;
    faket.t_drit = 0;
    faket.t_dspr = 0;
    faket.t_dcom = fakecom;
    fakecom[0] = STRfakecom1;
    fakecom[1] = 0;

    /*
     * We do the psave job to temporarily change the current job so that the
     * following fork is considered a separate job.  This is so that when
     * backquotes are used in a builtin function that calls glob the "current
     * job" is not corrupted.  We only need one level of pushed jobs as long as
     * we are sure to fork here.
     */
    psavejob();

    /*
     * It would be nicer if we could integrate this redirection more with the
     * routines in sh.sem.c by doing a fake execute on a builtin function that
     * was piped out.
     */
    mypipe(pvec);
    if (pfork(&faket, -1) == 0) {
	jmp_buf_t osetexit;
	struct command *volatile t;

	(void) close(pvec[0]);
	(void) dmove(pvec[1], 1);
	(void) dmove(SHDIAG,  2);
	initdesc();
	closem();
	/*
	 * Bugfix for nested backquotes by Michael Greim <greim@sbsvax.UUCP>,
	 * posted to comp.bugs.4bsd 12 Sep. 1989.
	 */
	if (pargv)		/* mg, 21.dec.88 */
	    blkfree(pargv), pargv = 0, pargsiz = 0;
	/* mg, 21.dec.88 */
	arginp = cp;
	for (arginp = cp; *cp; cp++) {
	    *cp &= TRIM;
	    if (is_set(STRcsubstnonl) && (*cp == '\n' || *cp == '\r'))
		*cp = ' ';
	}

        /*
	 * In the child ``forget'' everything about current aliases or
	 * eval vectors.
	 */
	alvec = NULL;
	evalvec = NULL;
	alvecp = NULL;
	evalp = NULL;

	t = NULL;
	getexit(osetexit);
	for (;;) {

	    if (paraml.next && paraml.next != &paraml)
		freelex(&paraml);
	    
	    paraml.next = paraml.prev = &paraml;
	    paraml.word = STRNULL;
	    (void) setexit();
	    justpr = 0;
	    
	    /*
	     * For the sake of reset()
	     */
	    freelex(&paraml);
	    if (t)
		freesyn(t), t = NULL;

	    if (haderr) {
		/* unwind */
		doneinp = 0;
		resexit(osetexit);
		reset();
	    }
	    if (seterr) {
		xfree(seterr);
		seterr = NULL;
	    }

	    (void) lex(&paraml);
	    if (seterr)
		stderror(ERR_OLD);
	    alias(&paraml);
	    t = syntax(paraml.next, &paraml, 0);
	    if (seterr)
		stderror(ERR_OLD);
#ifdef SIGTSTP
	    (void) sigignore(SIGTSTP);
#endif
#ifdef SIGTTIN
	    (void) sigignore(SIGTTIN);
#endif
#ifdef SIGTTOU
	    (void) sigignore(SIGTTOU);
#endif
	    execute(t, -1, NULL, NULL, TRUE);

	    freelex(&paraml);
	    freesyn(t), t = NULL;
	}
    }
    xfree(cp);
    (void) close(pvec[1]);
    c = 0;
    ip = NULL;
    do {
	int     cnt = 0;
	char   *tmp;

	tmp = tibuf;
	for (;;) {
	    while (icnt == 0) {
		int     i, eof;

		ip = ibuf;
		do
		    icnt = read(pvec[0], tmp, tibuf + BUFSIZE - tmp);
		while (icnt == -1 && errno == EINTR);
		eof = 0;
		if (icnt <= 0) {
		    if (tmp == tibuf)
			goto eof;
		    icnt = 0;
		    eof = 1;
		}
		icnt += tmp - tibuf;
		i = 0;
		tmp = tibuf;
		while (tmp < tibuf + icnt) {
		    int len;

		    len = normal_mbtowc(&ip[i], tmp, tibuf + icnt - tmp);
		    if (len == -1) {
		        reset_mbtowc();
		        if (!eof && (size_t)(tibuf + icnt - tmp) < MB_CUR_MAX) {
			    break; /* Maybe a partial character */
			}
			ip[i] = (unsigned char) *tmp | INVALID_BYTE; /* Error */
		    }
		    if (len <= 0)
		        len = 1;
		    i++;
		    tmp += len;
		}
		if (tmp != tibuf)
		    memmove (tibuf, tmp, tibuf + icnt - tmp);
		tmp = tibuf + (tibuf + icnt - tmp);
		icnt = i;
	    }
	    if (hadnl)
		break;
	    --icnt;
	    c = (*ip++ & TRIM);
	    if (c == 0)
		break;
#ifdef WINNT_NATIVE
	    if (c == '\r')
	    	c = ' ';
#endif /* WINNT_NATIVE */
	    if (c == '\n') {
		/*
		 * Continue around the loop one more time, so that we can eat
		 * the last newline without terminating this word.
		 */
		hadnl = 1;
		continue;
	    }
	    if (!quoted && (c == ' ' || c == '\t'))
		break;
	    cnt++;
	    Strbuf_append1(word, c | quoted);
	}
	/*
	 * Unless at end-of-file, we will form a new word here if there were
	 * characters in the word, or in any case when we take text literally.
	 * If we didn't make empty words here when literal was set then we
	 * would lose blank lines.
	 */
	if (c != 0 && (cnt || literal))
	    pword(word);
	hadnl = 0;
    } while (c > 0);
 eof:
    (void) close(pvec[0]);
    pwait();
    prestjob();
}

static void
pword(struct Strbuf *word)
{
    Char *s;

    if (pargc == pargsiz - 1) {
	pargsiz += GLOBSPACE;
	pargv = xrealloc(pargv, pargsiz * sizeof(Char *));
    }
    s = Strbuf_finish(word);
    NLSQuote(s);
    pargv[pargc++] = s;
    pargv[pargc] = NULL;
    *word = Strbuf_init;
}

int
Gmatch(const Char *string, const Char *pattern)
{
    return Gnmatch(string, pattern, NULL);
}

int
Gnmatch(const Char *string, const Char *pattern, const Char **endstr)
{
    Char **blk, **p;
    const Char *tstring = string;
    int	   gpol = 1, gres = 0;

    if (*pattern == '^') {
	gpol = 0;
	pattern++;
    }

    blk = xmalloc(GLOBSPACE * sizeof(Char *));
    blk[0] = Strsave(pattern);
    blk[1] = NULL;

    expbrace(&blk, NULL, GLOBSPACE);

    if (endstr == NULL)
	/* Exact matches only */
	for (p = blk; *p; p++) 
	    gres |= t_pmatch(string, *p, &tstring, 1) == 2 ? 1 : 0;
    else {
	const Char *end;

	/* partial matches */
        end = Strend(string);
	for (p = blk; *p; p++)
	    if (t_pmatch(string, *p, &tstring, 1) != 0) {
		gres |= 1;
		if (end > tstring)
		    end = tstring;
	    }
	*endstr = end;
    }

    blkfree(blk);
    return(gres == gpol);
} 

/* t_pmatch():
 *	Return 2 on exact match, 	
 *	Return 1 on substring match.
 *	Return 0 on no match.
 *	*estr will point to the end of the longest exact or substring match.
 */
int
t_pmatch(const Char *string, const Char *pattern, const Char **estr, int cs)
{
    NLSChar stringc, patternc, rangec;
    int     match, negate_range;
    const Char *pestr, *nstring;

    for (nstring = string;; string = nstring) {
	stringc = *nstring++;
	TRIM_AND_EXTEND(nstring, stringc);
	patternc = *pattern++;
	TRIM_AND_EXTEND(pattern, patternc);
	switch (patternc) {
	case '\0':
	    *estr = string;
	    return (stringc == '\0' ? 2 : 1);
	case '?':
	    if (stringc == 0)
		return (0);
	    break;
	case '*':
	    if (!*pattern) {
		*estr = Strend(string);
		return (2);
	    }
	    pestr = NULL;

	    for (;;) {
		switch(t_pmatch(string, pattern, estr, cs)) {
		case 0:
		    break;
		case 1:
		    pestr = *estr;/*FIXME: does not guarantee longest match */
		    break;
		case 2:
		    return 2;
		default:
		    abort();	/* Cannot happen */
		}
		stringc = *string++;
		if (!stringc)
		    break;
		TRIM_AND_EXTEND(string, stringc);
	    }

	    if (pestr) {
		*estr = pestr;
		return 1;
	    }
	    else
		return 0;

	case '[':
	    match = 0;
	    if ((negate_range = (*pattern == '^')) != 0)
		pattern++;
	    while ((rangec = *pattern++) != '\0') {
		if (rangec == ']')
		    break;
		TRIM_AND_EXTEND(pattern, rangec);
		if (match)
		    continue;
		if (*pattern == '-' && pattern[1] != ']') {
		    NLSChar rangec2;
		    pattern++;
		    rangec2 = *pattern++;
		    TRIM_AND_EXTEND(pattern, rangec2);
		    match = (globcharcoll(stringc, rangec2, 0) <= 0 &&
			globcharcoll(rangec, stringc, 0) <= 0);
		}
		else 
		    match = (stringc == rangec);
	    }
	    if (rangec == '\0')
		stderror(ERR_NAME | ERR_MISSING, ']');
	    if ((!match) && (stringc == '\0'))
		return (0);
	    if (match == negate_range)
		return (0);
	    break;
	default:
	    TRIM_AND_EXTEND(pattern, patternc);
	    if (cs ? patternc  != stringc
#if defined (NLS) && defined (WIDE_STRINGS)
		: towlower(patternc) != towlower(stringc))
#else
		: Tolower(patternc) != Tolower(stringc))
#endif
		return (0);
	    break;
	}
    }
}

void
Gcat(Char *s)
{
    gargv[gargc] = s;
    if (++gargc >= gargsiz) {
	gargsiz += GLOBSPACE;
	gargv = xrealloc(gargv, gargsiz * sizeof(Char *));
    }
    gargv[gargc] = 0;
}

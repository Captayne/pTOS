/*      GEMPD.C         1/27/84 - 03/20/85      Lee Jay Lorenzen        */
/*      merge High C vers. w. 2.2               8/21/87         mdf     */

/*
*       Copyright 1999, Caldera Thin Clients, Inc.
*                 2002-2022 The EmuTOS development team
*
*       This software is licenced under the GNU Public License.
*       Please see LICENSE.TXT for further information.
*
*                  Historical Copyright
*       -------------------------------------------------------------
*       GEM Application Environment Services              Version 2.3
*       Serial No.  XXXX-0000-654321              All Rights Reserved
*       Copyright (C) 1987                      Digital Research Inc.
*       -------------------------------------------------------------
*/

#include "emutos.h"
#include "struct.h"
#include "aesvars.h"
#include "obdefs.h"
#include "gemlib.h"

#include "geminit.h"
#include "gemasm.h"
#include "gempd.h"
#include "biosext.h"
#include "sched_abi.h"

#include "string.h"

/* returns the AESPD for the given index */
AESPD *pd_index(WORD i)
{
    return (i<2) ? &D.g_int[i].a_pd : &D.g_acc[i-2].a_pd;
}

/* returns the AESPD for the given name, or if pname is NULL for the given pid */
AESPD *fpdnm(char *pname, UWORD pid)
{
    WORD    i;
    AESPD   *p;

    for (i = 0; i < totpds; i++)
    {
        p = pd_index(i);
        if (pname != NULL)
        {
            if (strncmp(pname, p->p_name, AP_NAMELEN) == 0)
                return p;
        }
        else
            if (p->p_pid == pid)
                return p;
    }

    return NULL;
}


static AESPD *getpd(void)
{
    AESPD *p;

    /* we got all our memory so link it  */
    p = pd_index(curpid);
    p->p_pid = curpid++;

    /* return the pd we got */
    return p;
}


/*
 * name an AESPD from the 8 first chars of the given string, stopping at the first
 * '.' (remove the file extension)
 */
void p_nameit(AESPD *p, char *pname)
{
    char *s, *d;
    int i;

    for (i = 0, s = pname, d = p->p_name; (i < AP_NAMELEN) && *s && (*s != '.'); i++)
        *d++ = *s++;
    for ( ; i < 8; i++)
        *d++ = ' ';
}


/* set the application directory of an AESPD */
void p_setappdir(AESPD *pd, char *pfilespec)
{
    char *p;
    char *plast;
    char *pdest;

    /* find the position *after* the last path separator */
    for (p = plast = pfilespec; *p; )   /* assume no path separator */
    {
        if (*p++ == PATHSEP)
            plast = p;          /* after path separator ... */
    }

    /* copy the directory name including the final path separator */
    for (pdest = pd->p_appdir, p = pfilespec; p < plast; )
        *pdest++ = *p++;
    *pdest = '\0';
}


/*
 * Where every process but the first begins: it finds out which it is,
 * and runs its code.  That code never returns.
 */
static void process_start(void)
{
    k_task_t me = k_current();
    WORD i;

    for (i = 0; i < totpds; i++)
    {
        if (pd_index(i)->p_task == me)
        {
            rlr = pd_index(i);
            break;
        }
    }
    (*rlr->p_entry)();
}

AESPD *pstart(PFVOID pcode, char *pfilespec, LONG ldaddr)
{
    AESPD *px;

    /* create process to execute it */
    px = getpd();
    px->p_ldaddr = ldaddr;

    /* copy in name of file */
    p_nameit(px, pfilespec);
    p_setappdir(px, pfilespec);

    /* a kernel task on the process's private AES stack, runnable at once */
    px->p_entry = pcode;
    px->p_stat &= ~WAITIN;
    px->p_task = k_task_create(process_start, px->p_uda->u_super,
                               (ULONG)(&px->p_uda->u_supstk + 1)
                               - (ULONG)px->p_uda->u_super);
    if (!px->p_task)
        panic("AES: no kernel task for %8.8s\n", px->p_name);

    return px;
}

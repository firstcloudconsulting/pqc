/* -*-pgsql-c-*- */
/*
* $Header: /cvsroot/pgpool/pgpool/pool_stream.c,v 1.10 2007/07/11 01:40:17 y-asaba Exp $
*
* pgpool: a language independent connection pool server for PostgreSQL 
* written by Tatsuo Ishii
*
* Copyright (c) 2003-2007	PgPool Global Development Group
*
* Permission to use, copy, modify, and distribute this software and
* its documentation for any purpose and without fee is hereby
* granted, provided that the above copyright notice appear in all
* copies and that both that copyright notice and this permission
* notice appear in supporting documentation, and that the name of the
* author not be used in advertising or publicity pertaining to
* distribution of the software without specific, written prior
* permission. The author makes no representations about the
* suitability of this software for any purpose.  It is provided "as
* is" without express or implied warranty.
*
* pool_stream.c: stream I/O modules
*
*/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "pool.h"

#define READBUFSZ 1024
#define WRITEBUFSZ 8192

static int mystrlen(char *str, int upper, int *flag);
static int mystrlinelen(char *str, int upper, int *flag);
static int save_pending_data(POOL_CONNECTION *cp, void *data, int len);
static int consume_pending_data(POOL_CONNECTION *cp, void *data, int len);

/*
* open read/write file descriptors.
* returns POOL_CONNECTION on success otherwise NULL.
*/
POOL_CONNECTION *pool_open(int fd)
{
	POOL_CONNECTION *cp;

	cp = (POOL_CONNECTION *)malloc(sizeof(POOL_CONNECTION));
	if (cp == NULL)
	{
		pool_error("pool_open: malloc failed: %s", strerror(errno));
		return NULL;
	}

	memset(cp, 0, sizeof(*cp));

	/* initialize write buffer */
	cp->wbuf = malloc(WRITEBUFSZ);
	if (cp->wbuf == NULL)
	{
		pool_error("pool_open: malloc failed");
		return NULL;
	}
	cp->wbufsz = WRITEBUFSZ;
	cp->wbufpo = 0;

	/* initialize pending data buffer */
	cp->hp = malloc(READBUFSZ);
	if (cp->hp == NULL)
	{
		pool_error("pool_open: malloc failed");
		return NULL;
	}
	cp->bufsz = READBUFSZ;
	cp->po = 0;
	cp->len = 0;
	cp->sbuf = NULL;
	cp->sbufsz = 0;
	cp->buf2 = NULL;
	cp->sbufsz = 0;

	cp->fd = fd;

	memset((void *) &cp->raddr, 0, sizeof(cp->raddr));
	cp->auth_arg = NULL;
	cp->database = NULL;
	cp->username = NULL;

	return cp;
}

/*
* close read/write file descriptors.
*/
void pool_close(POOL_CONNECTION *cp)
{
	close(cp->fd);
	free(cp->wbuf);
	free(cp->hp);
	if (cp->sbuf)
		free(cp->sbuf);
	if (cp->buf2)
		free(cp->buf2);
	if (cp->isbackend)
		pool_discard_params(&cp->params);
	if (cp->auth_arg)
		free(cp->auth_arg);
	if (cp->database)
		free(cp->database);
	if (cp->username)
		free(cp->username);
	free(cp);
}

/*
* read len bytes from cp
* returns 0 on success otherwise -1.
*/
int pool_read(POOL_CONNECTION *cp, void *buf, int len)
{
	static char readbuf[READBUFSZ];

	int consume_size;
	int readlen;
	int notimeout;

	consume_size = consume_pending_data(cp, buf, len);
	len -= consume_size;
	buf += consume_size;

	/* if this is the secondary backend, then set timeout */
	notimeout = !cp->issecondary_backend;

	while (len > 0)
	{
		if (pool_check_fd(cp, notimeout))
		{
			if (cp->issecondary_backend)
			{
				pool_log("pool_read: secondary data is not ready. abort this session");
				exit(1);
			}
			else
			{
				pool_error("pool_read: pool_check_fd failed (%s)", strerror(errno));
			    return -1;
			}
		}

		readlen = read(cp->fd, readbuf, READBUFSZ);
		if (readlen == -1)
		{
			if (errno == EINTR || errno == EAGAIN)
			{
				pool_debug("pool_read: retrying due to %s", strerror(errno));
				continue;
			}

			pool_error("pool_read: read failed (%s)", strerror(errno));

			if (cp->isbackend)
			{
			    /* fatal error, notice to parent and exit */
			    notice_backend_error(!cp->issecondary_backend);
			    exit(1);
			}
			else
			{
			    return -1;
			}
		}
		else if (readlen == 0)
		{
			if (cp->isbackend)
			{
				pool_error("pool_read2: EOF encountered with backend");
				return -1;

#ifdef NOT_USED
			    /* fatal error, notice to parent and exit */
			    notice_backend_error(!cp->issecondary_backend);
				exit(1);
#endif
			}
			else
			{
				/*
				 * if backend offers authentication method, frontend could close connection
				 */
				return -1;
			}
		}

		if (len < readlen)
		{
			/* overrun. we need to save remaining data to pending buffer */
			if (save_pending_data(cp, readbuf+len, readlen-len))
				return -1;
			memmove(buf, readbuf, len);
			break;
		}

		memmove(buf, readbuf, readlen);
		buf += readlen;
		len -= readlen;
	}

	return 0;
}

/*
* read exactly len bytes from cp
* returns buffer address on success otherwise NULL.
*/
char *pool_read2(POOL_CONNECTION *cp, int len)
{
	char *buf;
	int req_size;
	int alloc_size;
	int consume_size;
	int readlen;
	int notimeout;

	req_size = cp->len + len;

	if (req_size > cp->bufsz2)
	{
		alloc_size = ((req_size+1)/READBUFSZ+1)*READBUFSZ;
		cp->buf2 = realloc(cp->buf2, alloc_size);
		if (cp->buf2 == NULL)
		{
			pool_error("pool_read2: failed to realloc");
			exit(1);
		}
		cp->bufsz2 = alloc_size;
	}

	buf = cp->buf2;

	consume_size = consume_pending_data(cp, buf, len);
	len -= consume_size;
	buf += consume_size;

	/* if this is the secondary backend, then set timeout */
	notimeout = !cp->issecondary_backend;
#ifdef NOT_USED
	pool_debug("notimeout: %d isbackend: %d issecondary_backend: %d", notimeout,
			   cp->isbackend, cp->issecondary_backend);
#endif

	while (len > 0)
	{
		if (pool_check_fd(cp, notimeout))
		{
			if (cp->issecondary_backend)
			{
				pool_log("pool_read2: secondary data is not ready. abort this session");
				exit(1);
			}
			else
			{
				pool_error("pool_read2: pool_check_fd failed (%s)", strerror(errno));
			    return NULL;
			}
		}

		readlen = read(cp->fd, buf, len);
		if (readlen == -1)
		{
			if (errno == EINTR || errno == EAGAIN)
			{
				pool_debug("pool_read2: retrying due to %s", strerror(errno));
				continue;
			}

			pool_error("pool_read2: read failed (%s)", strerror(errno));

			if (cp->isbackend)
			{
			    /* fatal error, notice to parent and exit */
			    notice_backend_error(!cp->issecondary_backend);
			    exit(1);
			}
			else
			{
			    return NULL;
			}
		}
		else if (readlen == 0)
		{
			if (cp->isbackend)
			{
				pool_error("pool_read2: EOF encountered with backend");
				return NULL;

#ifdef NOT_USED
			    /* fatal error, notice to parent and exit */
			    notice_backend_error(!cp->issecondary_backend);
				exit(1);
#endif
			}
			else
			{
				/*
				 * if backend offers authentication method, frontend could close connection
				 */
				return NULL;
			}
		}

		buf += readlen;
		len -= readlen;
	}

	return cp->buf2;
}

/*
* write len bytes to cp the write buffer.
* returns 0 on success otherwise -1.
*/
int pool_write(POOL_CONNECTION *cp, void *buf, int len)
{
	int reqlen;

	if (len < 0)
	{
		pool_error("pool_write: invalid request size: %d", len);
		return -1;
	}

	if (cp->no_forward)
		return 0;

	/* check buffer size */
	reqlen = cp->wbufpo + len;

	if (reqlen > cp->wbufsz)
	{
		char *p;

		reqlen = (reqlen/WRITEBUFSZ+1)*WRITEBUFSZ;
		p = realloc(cp->wbuf, reqlen);
		if (p == NULL)
		{
			pool_error("pool_write: realloc failed");
			return -1;
		}
		cp->wbuf = p;
		cp->wbufsz = reqlen;
	}

	memcpy(cp->wbuf+cp->wbufpo, buf, len);
	cp->wbufpo += len;

	return 0;
}

/*
 * flush write buffer
 */
int pool_flush_it(POOL_CONNECTION *cp)
{
	int sts;
	int wlen;
	int offset;
	wlen = cp->wbufpo;

	if (wlen == 0)
	{
		return 0;
	}

	offset = 0;

	for (;;)
	{
		errno = 0;

#ifdef NOT_USED
		if (!cp->isbackend)
		{
			fd_set	writemask;
			fd_set	exceptmask;

			FD_ZERO(&writemask);
			FD_ZERO(&exceptmask);
			FD_SET(cp->fd, &writemask);
			FD_SET(cp->fd, &exceptmask);

			sts = select(cp->fd+1, NULL, &writemask, &exceptmask, NULL);
			if (sts == -1)
			{
				if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)
					continue;

				pool_error("pool_flush_it: select() failed. reason: %s", strerror(errno));
				return -1;
			}
			else if (sts == 0)
			{
				continue;
			}
			else if (FD_ISSET(cp->fd, &exceptmask))
			{
				pool_log("pool_flush_it: exception occured");
				return -1;
			}
		}
#endif
		sts = write(cp->fd, cp->wbuf + offset, wlen);

		if (sts > 0)
		{
			wlen -= sts;

			if (wlen == 0)
			{
				/* write completed */
				break;
			}

			else if (wlen < 0)
			{
				pool_error("pool_flush_it: invalid write size %d", sts);
				return -1;
			}

			else
			{
				/* need to write remaining data */
				offset += sts;
				continue;
			}
		}

		else if (errno == EAGAIN || errno == EINTR)
		{
			continue;
		}

		else
		{
			pool_error("pool_flush_it: write failed (%s) offset: %d wlen: %d",
					   strerror(errno), offset, wlen);
			return -1;
		}
	}

	cp->wbufpo = 0;

	return 0;
}

/*
* flush write buffer and degenerate/failover if error occurs
*/
int pool_flush(POOL_CONNECTION *cp)
{
	if (pool_flush_it(cp) == -1)
	{
		if (cp->isbackend)
		{
			notice_backend_error(!cp->issecondary_backend);
			exit(1);
		}
		else
		{
			return -1;
		}
	}
	return 0;
}

/*
* combo of pool_write and pool_flush
*/
int pool_write_and_flush(POOL_CONNECTION *cp, void *buf, int len)
{
	if (pool_write(cp, buf, len))
		return -1;
	return pool_flush(cp);
}

/* 
 * read a string until EOF or NULL is encountered.
 * if line is not 0, read until new line is encountered.
*/
char *pool_read_string(POOL_CONNECTION *cp, int *len, int line)
{
	int readp;
	int readsize;
	int readlen;
	int strlength;
	int flag;
	int consume_size;
	int notimeout;

#ifdef DEBUG
	static char pbuf[READBUFSZ];
#endif

	*len = 0;
	readp = 0;

	/* initialize read buffer */
	if (cp->sbufsz == 0)
	{
		cp->sbuf = malloc(READBUFSZ);
		if (cp->sbuf == NULL)
		{
			pool_error("pool_read_string: malloc failed");
			return NULL;
		}
		cp->sbufsz = READBUFSZ;
		*cp->sbuf = '\0';
	}

	/* any pending data? */
	if (cp->len)
	{
		if (line)
			strlength = mystrlinelen(cp->hp+cp->po, cp->len, &flag);
		else
			strlength = mystrlen(cp->hp+cp->po, cp->len, &flag);

		/* buffer is too small? */
		if ((strlength + 1) > cp->sbufsz)
		{
			cp->sbufsz = ((strlength+1)/READBUFSZ+1)*READBUFSZ;
			cp->sbuf = realloc(cp->sbuf, cp->sbufsz);
			if (cp->sbuf == NULL)
			{
				pool_error("pool_read_string: realloc failed");
				return NULL;
			}
		}

		/* consume pending and save to read string buffer */
		consume_size = consume_pending_data(cp, cp->sbuf, strlength);

		*len = strlength;

		/* is the string null terminated? */
		if (consume_size == strlength && !flag)
		{
			/* not null or line terminated.
			 * we need to read more since we have not encountered NULL or new line yet
			 */
			readsize = cp->sbufsz - strlength;
			readp = strlength;
		}
		else
		{
			pool_debug("pool_read_string: read all from pending data. po:%d len:%d",
					   cp->po, cp->len);
			return cp->sbuf;
		}
	} else
	{
		readsize = cp->sbufsz;
	}

	/* if this is the secondary backend, then set timeout */
	notimeout = !cp->issecondary_backend;

	for (;;)
	{
		if (pool_check_fd(cp, notimeout))
		{
			if (cp->issecondary_backend)
			{
				pool_log("pool_read_string: secondary data is not ready. abort this session");
				exit(1);
			}
			else
			{
				pool_error("pool_read_string: pool_check_fd failed (%s)", strerror(errno));
			    return NULL;
			}
		}

		readlen = read(cp->fd, cp->sbuf+readp, readsize);
		if (readlen == -1)
		{
			pool_error("pool_read_string: read() failed. reason:%s", strerror(errno));

			if (cp->isbackend)
			{
			    notice_backend_error(!cp->issecondary_backend);
			    exit(1);
			}
			else
			{
			    return NULL;
			}
		}
		else if (readlen == 0)	/* EOF detected */
		{
			/*
			 * just returns an error, not trigger failover or degeneration
			 */
			pool_error("pool_read_string: read () EOF detected");
			return NULL;
		}

		/* check overrun */
		if (line)
			strlength = mystrlinelen(cp->sbuf+readp, readlen, &flag);
		else
			strlength = mystrlen(cp->sbuf+readp, readlen, &flag);

		if (strlength < readlen)
		{
			save_pending_data(cp, cp->sbuf+readp+strlength, readlen-strlength);
			*len += strlength;
			pool_debug("pool_read_string: total result %d with pending data po:%d len:%d", *len, cp->po, cp->len);
			return cp->sbuf;
		}

		*len += readlen;

		/* encountered null or newline? */
		if (flag)
		{
			/* ok we have read all data */
			pool_debug("pool_read_string: total result %d ", *len);
			break;
		}

		readp += readlen;
		readsize = READBUFSZ;

		if ((*len+readsize) > cp->sbufsz)
		{
			cp->sbufsz += READBUFSZ;

			cp->sbuf = realloc(cp->sbuf, cp->sbufsz);
			if (cp->sbuf == NULL)
			{
				pool_error("pool_read_string: realloc failed");
				return NULL;
			}
		}
	}
	return cp->sbuf;
}

/*
 * returns the byte length of str, including \0, no more than upper.
 * if encountered \0, flag is set to non 0.
 * example:
 *	mystrlen("abc", 2) returns 2
 *	mystrlen("abc", 3) returns 3
 *	mystrlen("abc", 4) returns 4
 *	mystrlen("abc", 5) returns 4
 */
static int mystrlen(char *str, int upper, int *flag)
{
	int len;

	*flag = 0;

	for (len = 0;len < upper; len++, str++)
	{
	    if (!*str)
	    {
			len++;
			*flag = 1;
			break;
	    }
	}
	return len;
}

/*
 * returns the byte length of str terminated by \n or \0 (including \n or \0), no more than upper.
 * if encountered \0 or \n, flag is set to non 0.
 * example:
 *	mystrlinelen("abc", 2) returns 2
 *	mystrlinelen("abc", 3) returns 3
 *	mystrlinelen("abc", 4) returns 4
 *	mystrlinelen("abc", 5) returns 4
 *	mystrlinelen("abcd\nefg", 4) returns 4
 *	mystrlinelen("abcd\nefg", 5) returns 5
 *	mystrlinelen("abcd\nefg", 6) returns 5
 */
static int mystrlinelen(char *str, int upper, int *flag)
{
	int len;

	*flag = 0;

	for (len = 0;len < upper; len++, str++)
	{
	    if (!*str || *str == '\n')
	    {
			len++;
			*flag = 1;
			break;
	    }
	}
	return len;
}

/*
 * save pending data
 */
static int save_pending_data(POOL_CONNECTION *cp, void *data, int len)
{
	int reqlen;
	size_t realloc_size;
	char *p;

	/* to be safe */
	if (cp->len == 0)
		cp->po = 0;

	reqlen = cp->po + cp->len + len;

	/* pending buffer is enough? */
	if (reqlen > cp->bufsz)
	{
		/* too small, enlarge it */
		realloc_size = (reqlen/READBUFSZ+1)*READBUFSZ;
		p = realloc(cp->hp, realloc_size);
		if (p == NULL)
		{
			pool_error("save_pending_data: realloc failed");
			return -1;
		}

		cp->bufsz = realloc_size;
		cp->hp = p;
	}

	memmove(cp->hp + cp->po + cp->len, data, len);
	cp->len += len;

	return 0;
}

/*
 * consume pending data. returns actually consumed data length.
 */
static int consume_pending_data(POOL_CONNECTION *cp, void *data, int len)
{
	int consume_size;

	if (cp->len <= 0)
		return 0;

	consume_size = Min(len, cp->len);
	memmove(data, cp->hp + cp->po, consume_size);
	cp->len -= consume_size;

	if (cp->len <= 0)
		cp->po = 0;
	else
		cp->po += consume_size;

	return consume_size;
}

/*
 * pool_unread: Put back data to input buffer
 */
int pool_unread(POOL_CONNECTION *cp, void *data, int len)
{
	void *p = cp->hp;
	int n = cp->len + len;
	int realloc_size;
	
	if (cp->bufsz < n)
	{
		realloc_size = (n/READBUFSZ+1)*READBUFSZ;
		p = realloc(cp->hp, realloc_size);
		if (p == NULL)
		{
			pool_error("pool_unread: realloc failed");
			return -1;
		}
		cp->hp = p;
	}
	if (cp->len != 0)
		memmove(p + len, cp->hp + cp->po, cp->len);
	memmove(p, data, len);
	cp->len = n;
	cp->po = 0;
	return 0;
}

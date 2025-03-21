/*	$NetBSD: strtonum.c,v 1.7 2024/01/20 16:13:39 christos Exp $	*/
/*-
 * Copyright (c) 2014 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Christos Zoulas.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__RCSID("$NetBSD: strtonum.c,v 1.7 2024/01/20 16:13:39 christos Exp $");

#include "namespace.h"

#define _OPENBSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>

long long
strtonum(const char *nptr, long long minval, long long maxval,
         const char **errstr)
{
	int e;
	long long rv;
	const char *resp;
	char *eptr;

	if (errstr == NULL)
		errstr = &resp;

	if (minval > maxval)
		goto out;

	rv = (long long)strtoi(nptr, &eptr, 10, minval, maxval, &e);

	switch (e) {
	case 0:
		*errstr = NULL;
		return rv;
	case ECANCELED:
	case ENOTSUP:
		goto out;
	case ERANGE:
		if (*eptr)
			goto out;
		*errstr = rv == maxval ? "too large" : "too small";
		return 0;
	default:
		abort();
	}

out:
	*errstr = "invalid";
	return 0;
}

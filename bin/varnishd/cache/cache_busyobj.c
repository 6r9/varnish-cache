/*-
 * Copyright (c) 2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Martin Blix Grydeland <martin@varnish-software.com>
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Handle backend connections and backend request structures.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

#include "cache.h"

static struct mempool		*vbopool;

struct vbo {
	unsigned		magic;
#define VBO_MAGIC		0xde3d8223
	struct lock		mtx;
	unsigned		refcount;
	char			*end;
	struct busyobj		bo;
};

/*--------------------------------------------------------------------
 */

void
VBO_Init(void)
{

	vbopool = MPL_New("vbo", &cache_param->vbo_pool,
	    &cache_param->workspace_backend);
	AN(vbopool);
}

/*--------------------------------------------------------------------
 * BusyObj handling
 */

static struct vbo *
vbo_New(void)
{
	struct vbo *vbo;
	unsigned sz;

	vbo = MPL_Get(vbopool, &sz);
	AN(vbo);
	vbo->magic = VBO_MAGIC;
	vbo->end = (char *)vbo + sz;
	Lck_New(&vbo->mtx, lck_busyobj);
	return (vbo);
}

void
VBO_Free(struct vbo **vbop)
{
	struct vbo *vbo;

	AN(vbop);
	vbo = *vbop;
	*vbop = NULL;
	CHECK_OBJ_NOTNULL(vbo, VBO_MAGIC);
	AZ(vbo->refcount);
	Lck_Delete(&vbo->mtx);
	MPL_Free(vbopool, vbo);
}

struct busyobj *
VBO_GetBusyObj(struct worker *wrk)
{
	struct vbo *vbo = NULL;
	uint16_t nhttp;
	unsigned sz;
	char *p;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);

	if (wrk->nvbo != NULL) {
		vbo = wrk->nvbo;
		wrk->nvbo = NULL;
	}

	if (vbo == NULL)
		vbo = vbo_New();

	CHECK_OBJ_NOTNULL(vbo, VBO_MAGIC);
	AZ(vbo->refcount);

	AZ(vbo->bo.magic);
	vbo->refcount = 1;
	vbo->bo.magic = BUSYOBJ_MAGIC;
	vbo->bo.vbo = vbo;

	p = (void*)(vbo + 1);
	p = (void*)PRNDUP(p);
	assert(p < vbo->end);

	nhttp = (uint16_t)cache_param->http_max_hdr;
	sz = HTTP_estimate(nhttp);

	vbo->bo.bereq = HTTP_create(p, nhttp);
	p += sz;
	p = (void*)PRNDUP(p);
	assert(p < vbo->end);

	vbo->bo.beresp = HTTP_create(p, nhttp);
	p += sz;
	p = (void*)PRNDUP(p);
	assert(p < vbo->end);

	sz = cache_param->vsl_buffer;
	VSL_Setup(vbo->bo.vsl, p, sz);
	p += sz;
	p = (void*)PRNDUP(p);
	assert(p < vbo->end);

	WS_Init(vbo->bo.ws, "bo", p, vbo->end - p);

	return (&vbo->bo);
}

void
VBO_RefBusyObj(const struct busyobj *busyobj)
{
	struct vbo *vbo;

	CHECK_OBJ_NOTNULL(busyobj, BUSYOBJ_MAGIC);
	vbo = busyobj->vbo;
	CHECK_OBJ_NOTNULL(vbo, VBO_MAGIC);
	Lck_Lock(&vbo->mtx);
	assert(vbo->refcount > 0);
	vbo->refcount++;
	Lck_Unlock(&vbo->mtx);
}

void
VBO_DerefBusyObj(struct worker *wrk, struct busyobj **pbo)
{
	struct busyobj *bo;
	struct vbo *vbo;
	unsigned r;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	AN(pbo);
	bo = *pbo;
	*pbo = NULL;
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	vbo = bo->vbo;
	CHECK_OBJ_NOTNULL(vbo, VBO_MAGIC);
	Lck_Lock(&vbo->mtx);
	assert(vbo->refcount > 0);
	r = --vbo->refcount;
	Lck_Unlock(&vbo->mtx);

	if (r)
		return;

	WSL_Flush(vbo->bo.vsl, 0);
	/* XXX: Sanity checks & cleanup */
	memset(&vbo->bo, 0, sizeof vbo->bo);

	if (cache_param->bo_cache && wrk->nvbo == NULL)
		wrk->nvbo = vbo;
	else
		VBO_Free(&vbo);
}

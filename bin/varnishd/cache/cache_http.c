/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
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
 * HTTP request storage and manipulation
 */

#include "config.h"

#include <stdio.h>

#include "cache.h"

#include "vct.h"

#define HTTPH(a, b, c) char b[] = "*" a ":";
#include "tbl/http_headers.h"
#undef HTTPH

static const enum VSL_tag_e foo[] = {
	[HTTP_Req]	= SLT_ReqRequest,
	[HTTP_Resp]	= SLT_RespRequest,
	[HTTP_Bereq]	= SLT_BereqRequest,
	[HTTP_Beresp]	= SLT_BerespRequest,
	[HTTP_Obj]	= SLT_ObjRequest,
};

static enum VSL_tag_e
http2shmlog(const struct http *hp, int t)
{

	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	if (t > HTTP_HDR_FIRST)
		t = HTTP_HDR_FIRST;
	assert(hp->logtag >= HTTP_Req && hp->logtag <= HTTP_Obj); /*lint !e685*/
	assert(t >= HTTP_HDR_REQ && t <= HTTP_HDR_FIRST);
	return ((enum VSL_tag_e)(foo[hp->logtag] + t));
}

static void
http_VSLH(const struct http *hp, unsigned hdr)
{

	AN(hp->vsl);
	AN(hp->vsl->wid & (VSL_CLIENTMARKER|VSL_BACKENDMARKER));
	VSLbt(hp->vsl, http2shmlog(hp, hdr), hp->hd[hdr]);
}

/*--------------------------------------------------------------------*/
/* List of canonical HTTP response code names from RFC2616 */

static struct http_msg {
	unsigned	nbr;
	const char	*txt;
} http_msg[] = {
#define HTTP_RESP(n, t)	{ n, t},
#include "tbl/http_response.h"
	{ 0, NULL }
};

const char *
http_StatusMessage(unsigned status)
{
	struct http_msg *mp;

	assert(status >= 100 && status <= 999);
	for (mp = http_msg; mp->nbr != 0 && mp->nbr <= status; mp++)
		if (mp->nbr == status)
			return (mp->txt);
	return ("Unknown Error");
}

/*--------------------------------------------------------------------*/

unsigned
HTTP_estimate(unsigned nhttp)
{

	/* XXX: We trust the structs to size-aligned as necessary */
	return (sizeof (struct http) + (sizeof (txt) + 1) * nhttp);
}

struct http *
HTTP_create(void *p, uint16_t nhttp)
{
	struct http *hp;

	hp = p;
	hp->magic = HTTP_MAGIC;
	hp->hd = (void*)(hp + 1);
	hp->shd = nhttp;
	hp->hdf = (void*)(hp->hd + nhttp);
	return (hp);
}

/*--------------------------------------------------------------------*/

void
HTTP_Setup(struct http *hp, struct ws *ws, struct vsl_log *vsl,
    enum httpwhence  whence)
{
	http_Teardown(hp);
	hp->logtag = whence;
	hp->ws = ws;
	hp->vsl = vsl;
}

/*--------------------------------------------------------------------*/

void
http_Teardown(struct http *hp)
{
	uint16_t shd;
	txt *hd;
	unsigned char *hdf;

	/* XXX: This is not elegant, is it efficient ? */
	shd = hp->shd;
	hd = hp->hd;
	hdf = hp->hdf;
	memset(hp, 0, sizeof *hp);
	memset(hd, 0, sizeof *hd * shd);
	memset(hdf, 0, sizeof *hdf * shd);
	hp->magic = HTTP_MAGIC;
	hp->nhd = HTTP_HDR_FIRST;
	hp->shd = shd;
	hp->hd = hd;
	hp->hdf = hdf;
}

/*--------------------------------------------------------------------*/

static int
http_IsHdr(const txt *hh, const char *hdr)
{
	unsigned l;

	Tcheck(*hh);
	AN(hdr);
	l = hdr[0];
	assert(l == strlen(hdr + 1));
	assert(hdr[l] == ':');
	hdr++;
	return (!strncasecmp(hdr, hh->b, l));
}

/*--------------------------------------------------------------------
 * This function collapses multiple headerlines of the same name.
 * The lines are joined with a comma, according to [rfc2616, 4.2bot, p32]
 */

void
http_CollectHdr(struct http *hp, const char *hdr)
{
	unsigned u, v, ml, f = 0, x;
	char *b = NULL, *e = NULL;

	for (u = HTTP_HDR_FIRST; u < hp->nhd; u++) {
		while (u < hp->nhd && http_IsHdr(&hp->hd[u], hdr)) {
			Tcheck(hp->hd[u]);
			if (f == 0) {
				/* Found first header, just record the fact */
				f = u;
				break;
			}
			if (b == NULL) {
				/* Found second header, start our collection */
				ml = WS_Reserve(hp->ws, 0);
				b = hp->ws->f;
				e = b + ml;
				x = Tlen(hp->hd[f]);
				if (b + x < e) {
					memcpy(b, hp->hd[f].b, x);
					b += x;
				} else
					b = e;
			}

			AN(b);
			AN(e);

			/* Append the Nth header we found */
			if (b < e)
				*b++ = ',';
			x = Tlen(hp->hd[u]) - *hdr;
			if (b + x < e) {
				memcpy(b, hp->hd[u].b + *hdr, x);
				b += x;
			} else
				b = e;

			/* Shift remaining headers up one slot */
			for (v = u; v < hp->nhd - 1; v++)
				hp->hd[v] = hp->hd[v + 1];
			hp->nhd--;
		}

	}
	if (b == NULL)
		return;
	AN(e);
	if (b >= e) {
		WS_Release(hp->ws, 0);
		return;
	}
	*b = '\0';
	hp->hd[f].b = hp->ws->f;
	hp->hd[f].e = b;
	WS_ReleaseP(hp->ws, b + 1);
}


/*--------------------------------------------------------------------*/

static unsigned
http_findhdr(const struct http *hp, unsigned l, const char *hdr)
{
	unsigned u;

	for (u = HTTP_HDR_FIRST; u < hp->nhd; u++) {
		Tcheck(hp->hd[u]);
		if (hp->hd[u].e < hp->hd[u].b + l + 1)
			continue;
		if (hp->hd[u].b[l] != ':')
			continue;
		if (strncasecmp(hdr, hp->hd[u].b, l))
			continue;
		return (u);
	}
	return (0);
}

int
http_GetHdr(const struct http *hp, const char *hdr, char **ptr)
{
	unsigned u, l;
	char *p;

	l = hdr[0];
	diagnostic(l == strlen(hdr + 1));
	assert(hdr[l] == ':');
	hdr++;
	u = http_findhdr(hp, l - 1, hdr);
	if (u == 0) {
		if (ptr != NULL)
			*ptr = NULL;
		return (0);
	}
	if (ptr != NULL) {
		p = hp->hd[u].b + l;
		while (vct_issp(*p))
			p++;
		*ptr = p;
	}
	return (1);
}


/*--------------------------------------------------------------------
 * Find a given data element in a header according to RFC2616's #rule
 * (section 2.1, p15)
 */

int
http_GetHdrData(const struct http *hp, const char *hdr,
    const char *field, char **ptr)
{
	char *h, *e;
	unsigned fl;

	if (ptr != NULL)
		*ptr = NULL;
	if (!http_GetHdr(hp, hdr, &h))
		return (0);
	AN(h);
	e = strchr(h, '\0');
	fl = strlen(field);
	while (h + fl <= e) {
		/* Skip leading whitespace and commas */
		if (vct_islws(*h) || *h == ',') {
			h++;
			continue;
		}
		/* Check for substrings before memcmp() */
		if ((h + fl == e || vct_issepctl(h[fl])) &&
		    !memcmp(h, field, fl)) {
			if (ptr != NULL) {
				h += fl;
				while (vct_islws(*h))
					h++;
				*ptr = h;
			}
			return (1);
		}
		/* Skip until end of header or comma */
		while (*h && *h != ',')
			h++;
	}
	return (0);
}

/*--------------------------------------------------------------------
 * Find a given headerfields Q value.
 */

double
http_GetHdrQ(const struct http *hp, const char *hdr, const char *field)
{
	char *h;
	int i;
	double a, b;

	h = NULL;
	i = http_GetHdrData(hp, hdr, field, &h);
	if (!i)
		return (0.);

	if (h == NULL)
		return (1.);
	/* Skip whitespace, looking for '=' */
	while (*h && vct_issp(*h))
		h++;
	if (*h++ != ';')
		return (1.);
	while (*h && vct_issp(*h))
		h++;
	if (*h++ != 'q')
		return (1.);
	while (*h && vct_issp(*h))
		h++;
	if (*h++ != '=')
		return (1.);
	while (*h && vct_issp(*h))
		h++;
	a = 0.;
	while (vct_isdigit(*h)) {
		a *= 10.;
		a += *h - '0';
		h++;
	}
	if (*h++ != '.')
		return (a);
	b = .1;
	while (vct_isdigit(*h)) {
		a += b * (*h - '0');
		b *= .1;
		h++;
	}
	return (a);
}

/*--------------------------------------------------------------------
 * Find a given headerfields value.
 */

int
http_GetHdrField(const struct http *hp, const char *hdr,
    const char *field, char **ptr)
{
	char *h;
	int i;

	if (ptr != NULL)
		*ptr = NULL;

	h = NULL;
	i = http_GetHdrData(hp, hdr, field, &h);
	if (!i)
		return (i);

	if (ptr != NULL && h != NULL) {
		/* Skip whitespace, looking for '=' */
		while (*h && vct_issp(*h))
			h++;
		if (*h == '=') {
			h++;
			while (*h && vct_issp(*h))
				h++;
			*ptr = h;
		}
	}
	return (i);
}

/*--------------------------------------------------------------------
 * XXX: redo with http_GetHdrField() ?
 */

enum sess_close
http_DoConnection(const struct http *hp)
{
	char *p, *q;
	enum sess_close ret;
	unsigned u;

	if (!http_GetHdr(hp, H_Connection, &p)) {
		if (hp->protover < 11)
			return (SC_REQ_HTTP10);
		return (SC_NULL);
	}
	ret = SC_NULL;
	AN(p);
	for (; *p; p++) {
		if (vct_issp(*p))
			continue;
		if (*p == ',')
			continue;
		for (q = p + 1; *q; q++)
			if (*q == ',' || vct_issp(*q))
				break;
		u = pdiff(p, q);
		if (u == 5 && !strncasecmp(p, "close", u))
			ret = SC_REQ_CLOSE;
		u = http_findhdr(hp, u, p);
		if (u != 0)
			hp->hdf[u] |= HDF_FILTER;
		if (!*q)
			break;
		p = q;
	}
	return (ret);
}

/*--------------------------------------------------------------------*/

int
http_HdrIs(const struct http *hp, const char *hdr, const char *val)
{
	char *p;

	if (!http_GetHdr(hp, hdr, &p))
		return (0);
	AN(p);
	if (!strcasecmp(p, val))
		return (1);
	return (0);
}

/*--------------------------------------------------------------------*/

uint16_t
http_GetStatus(const struct http *hp)
{

	return (hp->status);
}

const char *
http_GetReq(const struct http *hp)
{

	Tcheck(hp->hd[HTTP_HDR_REQ]);
	return (hp->hd[HTTP_HDR_REQ].b);
}

/*--------------------------------------------------------------------
 * Dissect the headers of the HTTP protocol message.
 * Detect conditionals (headers which start with '^[Ii][Ff]-')
 */

static uint16_t
http_dissect_hdrs(struct http *hp, char *p, const struct http_conn *htc)
{
	char *q, *r;
	txt t = htc->rxbuf;

	if (*p == '\r')
		p++;

	hp->nhd = HTTP_HDR_FIRST;
	hp->conds = 0;
	r = NULL;		/* For FlexeLint */
	for (; p < t.e; p = r) {

		/* Find end of next header */
		q = r = p;
		while (r < t.e) {
			if (!vct_iscrlf(*r)) {
				r++;
				continue;
			}
			q = r;
			assert(r < t.e);
			r += vct_skipcrlf(r);
			if (r >= t.e)
				break;
			/* If line does not continue: got it. */
			if (!vct_issp(*r))
				break;

			/* Clear line continuation LWS to spaces */
			while (vct_islws(*q))
				*q++ = ' ';
		}

		if (q - p > htc->maxhdr) {
			VSC_C_main->losthdr++;
			VSLb(hp->vsl, SLT_LostHeader, "%.*s",
			    (int)(q - p > 20 ? 20 : q - p), p);
			return (413);
		}

		/* Empty header = end of headers */
		if (p == q)
			break;

		if ((p[0] == 'i' || p[0] == 'I') &&
		    (p[1] == 'f' || p[1] == 'F') &&
		    p[2] == '-')
			hp->conds = 1;

		while (q > p && vct_issp(q[-1]))
			q--;
		*q = '\0';

		if (hp->nhd < hp->shd) {
			hp->hdf[hp->nhd] = 0;
			hp->hd[hp->nhd].b = p;
			hp->hd[hp->nhd].e = q;
			http_VSLH(hp, hp->nhd);
			hp->nhd++;
		} else {
			VSC_C_main->losthdr++;
			VSLb(hp->vsl, SLT_LostHeader, "%.*s",
			    (int)(q - p > 20 ? 20 : q - p), p);
			return (413);
		}
	}
	return (0);
}

/*--------------------------------------------------------------------
 * Deal with first line of HTTP protocol message.
 */

static uint16_t
http_splitline(struct http *hp,
    const struct http_conn *htc, int h1, int h2, int h3)
{
	char *p, *q;

	CHECK_OBJ_NOTNULL(htc, HTTP_CONN_MAGIC);
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);

	/* XXX: Assert a NUL at rx.e ? */
	Tcheck(htc->rxbuf);

	/* Skip leading LWS */
	for (p = htc->rxbuf.b ; vct_islws(*p); p++)
		continue;

	/* First field cannot contain SP, CRLF or CTL */
	q = p;
	for (; !vct_issp(*p); p++) {
		if (vct_isctl(*p))
			return (400);
	}
	hp->hd[h1].b = q;
	hp->hd[h1].e = p;

	/* Skip SP */
	for (; vct_issp(*p); p++) {
		if (vct_isctl(*p))
			return (400);
	}

	/* Second field cannot contain LWS or CTL */
	q = p;
	for (; !vct_islws(*p); p++) {
		if (vct_isctl(*p))
			return (400);
	}
	hp->hd[h2].b = q;
	hp->hd[h2].e = p;

	if (!Tlen(hp->hd[h2]))
		return (413);

	/* Skip SP */
	for (; vct_issp(*p); p++) {
		if (vct_isctl(*p))
			return (400);
	}

	/* Third field is optional and cannot contain CTL */
	q = p;
	if (!vct_iscrlf(*p)) {
		for (; !vct_iscrlf(*p); p++)
			if (!vct_issep(*p) && vct_isctl(*p))
				return (400);
	}
	hp->hd[h3].b = q;
	hp->hd[h3].e = p;

	/* Skip CRLF */
	p += vct_skipcrlf(p);

	*hp->hd[h1].e = '\0';
	http_VSLH(hp, h1);

	*hp->hd[h2].e = '\0';
	http_VSLH(hp, h2);

	if (hp->hd[h3].e != NULL) {
		*hp->hd[h3].e = '\0';
		http_VSLH(hp, h3);
	}

	return (http_dissect_hdrs(hp, p, htc));
}

/*--------------------------------------------------------------------*/

static void
http_ProtoVer(struct http *hp)
{

	if (!strcasecmp(hp->hd[HTTP_HDR_PROTO].b, "HTTP/1.0"))
		hp->protover = 10;
	else if (!strcasecmp(hp->hd[HTTP_HDR_PROTO].b, "HTTP/1.1"))
		hp->protover = 11;
	else
		hp->protover = 9;
}


/*--------------------------------------------------------------------*/

uint16_t
http_DissectRequest(struct req *req)
{
	struct http_conn *htc;
	struct http *hp;
	uint16_t retval;

	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	htc = req->htc;
	CHECK_OBJ_NOTNULL(htc, HTTP_CONN_MAGIC);
	hp = req->http;
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);

	retval = http_splitline(hp, htc,
	    HTTP_HDR_REQ, HTTP_HDR_URL, HTTP_HDR_PROTO);
	if (retval != 0) {
		VSLbt(req->vsl, SLT_HttpGarbage, htc->rxbuf);
		return (retval);
	}
	http_ProtoVer(hp);
	return (retval);
}

/*--------------------------------------------------------------------*/

uint16_t
http_DissectResponse(struct http *hp, const struct http_conn *htc)
{
	int j;
	uint16_t retval = 0;
	char *p;


	CHECK_OBJ_NOTNULL(htc, HTTP_CONN_MAGIC);
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);

	if (http_splitline(hp, htc,
	    HTTP_HDR_PROTO, HTTP_HDR_STATUS, HTTP_HDR_RESPONSE))
		retval = 503;

	if (retval == 0 && memcmp(hp->hd[HTTP_HDR_PROTO].b, "HTTP/1.", 7))
		retval = 503;

	if (retval == 0 && Tlen(hp->hd[HTTP_HDR_STATUS]) != 3)
		retval = 503;

	if (retval == 0) {
		hp->status = 0;
		p = hp->hd[HTTP_HDR_STATUS].b;
		for (j = 100; j != 0; j /= 10) {
			if (!vct_isdigit(*p)) {
				retval = 503;
				break;
			}
			hp->status += (uint16_t)(j * (*p - '0'));
			p++;
		}
		if (*p != '\0')
			retval = 503;
	}

	if (retval != 0) {
		VSLbt(hp->vsl, SLT_HttpGarbage, htc->rxbuf);
		assert(retval >= 100 && retval <= 999);
		hp->status = retval;
	} else {
		http_ProtoVer(hp);
	}

	if (hp->hd[HTTP_HDR_RESPONSE].b == NULL ||
	    !Tlen(hp->hd[HTTP_HDR_RESPONSE])) {
		/* Backend didn't send a response string, use the standard */
		hp->hd[HTTP_HDR_RESPONSE].b =
		    TRUST_ME(http_StatusMessage(hp->status));
		hp->hd[HTTP_HDR_RESPONSE].e =
		    strchr(hp->hd[HTTP_HDR_RESPONSE].b, '\0');
	}
	return (retval);
}

/*--------------------------------------------------------------------*/

void
http_SetH(const struct http *to, unsigned n, const char *fm)
{

	assert(n < to->shd);
	AN(fm);
	to->hd[n].b = TRUST_ME(fm);
	to->hd[n].e = strchr(to->hd[n].b, '\0');
	to->hdf[n] = 0;
}

static void
http_linkh(const struct http *to, const struct http *fm, unsigned n)
{

	assert(n < HTTP_HDR_FIRST);
	Tcheck(fm->hd[n]);
	to->hd[n] = fm->hd[n];
	to->hdf[n] = fm->hdf[n];
}

void
http_ForceGet(const struct http *to)
{
	if (strcmp(http_GetReq(to), "GET"))
		http_SetH(to, HTTP_HDR_REQ, "GET");
}

void
http_SetResp(struct http *to, const char *proto, uint16_t status,
    const char *response)
{

	CHECK_OBJ_NOTNULL(to, HTTP_MAGIC);
	http_SetH(to, HTTP_HDR_PROTO, proto);
	assert(status >= 100 && status <= 999);
	to->status = status;
	http_SetH(to, HTTP_HDR_RESPONSE, response);
}

/*--------------------------------------------------------------------
 * Estimate how much workspace we need to Filter this header according
 * to 'how'.
 */

unsigned
http_EstimateWS(const struct http *fm, unsigned how, uint16_t *nhd)
{
	unsigned u, l;

	l = 0;
	*nhd = HTTP_HDR_FIRST;
	CHECK_OBJ_NOTNULL(fm, HTTP_MAGIC);
	for (u = 0; u < fm->nhd; u++) {
		if (fm->hd[u].b == NULL)
			continue;
		if (fm->hdf[u] & HDF_FILTER)
			continue;
#define HTTPH(a, b, c) \
		if (((c) & how) && http_IsHdr(&fm->hd[u], (b))) \
			continue;
#include "tbl/http_headers.h"
#undef HTTPH
		l += PRNDUP(Tlen(fm->hd[u]) + 1);
		(*nhd)++;
		// fm->hdf[u] |= HDF_COPY;
	}
	return (l);
}

/*--------------------------------------------------------------------*/

static void
http_filterfields(struct http *to, const struct http *fm, unsigned how)
{
	unsigned u;

	CHECK_OBJ_NOTNULL(fm, HTTP_MAGIC);
	CHECK_OBJ_NOTNULL(to, HTTP_MAGIC);
	to->nhd = HTTP_HDR_FIRST;
	to->status = fm->status;
	for (u = HTTP_HDR_FIRST; u < fm->nhd; u++) {
		if (fm->hd[u].b == NULL)
			continue;
		if (fm->hdf[u] & HDF_FILTER)
			continue;
#define HTTPH(a, b, c) \
		if (((c) & how) && http_IsHdr(&fm->hd[u], (b))) \
			continue;
#include "tbl/http_headers.h"
#undef HTTPH
		Tcheck(fm->hd[u]);
		if (to->nhd < to->shd) {
			to->hd[to->nhd] = fm->hd[u];
			to->hdf[to->nhd] = 0;
			to->nhd++;
		} else  {
			VSC_C_main->losthdr++;
			VSLbt(to->vsl, SLT_LostHeader, fm->hd[u]);
		}
	}
}

/*--------------------------------------------------------------------*/

void
http_FilterReq(const struct req *req, unsigned how)
{
	struct http *hp;

	hp = req->busyobj->bereq;
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);

	http_linkh(hp, req->http, HTTP_HDR_REQ);
	http_linkh(hp, req->http, HTTP_HDR_URL);
	if (how == HTTPH_R_FETCH)
		http_SetH(hp, HTTP_HDR_PROTO, "HTTP/1.1");
	else
		http_linkh(hp, req->http, HTTP_HDR_PROTO);
	http_filterfields(hp, req->http, how);
	http_PrintfHeader(hp, "X-Varnish: %u", req->vsl->wid & VSL_IDENTMASK);
}

/*--------------------------------------------------------------------*/

void
http_FilterResp(const struct http *fm, struct http *to, unsigned how)
{

	CHECK_OBJ_NOTNULL(fm, HTTP_MAGIC);
	CHECK_OBJ_NOTNULL(to, HTTP_MAGIC);
	http_SetH(to, HTTP_HDR_PROTO, "HTTP/1.1");
	to->status = fm->status;
	http_linkh(to, fm, HTTP_HDR_RESPONSE);
	http_filterfields(to, fm, how);
}

/*--------------------------------------------------------------------
 * This function copies any header fields which reference foreign
 * storage into our own WS.
 */

void
http_CopyHome(const struct http *hp)
{
	unsigned u, l;
	char *p;

	for (u = 0; u < hp->nhd; u++) {
		if (hp->hd[u].b == NULL)
			continue;
		if (hp->hd[u].b >= hp->ws->s && hp->hd[u].e <= hp->ws->e) {
			http_VSLH(hp, u);
			continue;
		}
		l = Tlen(hp->hd[u]);
		p = WS_Alloc(hp->ws, l + 1);
		if (p != NULL) {
			http_VSLH(hp, u);
			memcpy(p, hp->hd[u].b, l + 1L);
			hp->hd[u].b = p;
			hp->hd[u].e = p + l;
		} else {
			/* XXX This leaves a slot empty */
			VSC_C_main->losthdr++;
			VSLbt(hp->vsl, SLT_LostHeader, hp->hd[u]);
			hp->hd[u].b = NULL;
			hp->hd[u].e = NULL;
		}
	}
}

/*--------------------------------------------------------------------*/

void
http_ClrHeader(struct http *to)
{

	CHECK_OBJ_NOTNULL(to, HTTP_MAGIC);
	to->nhd = HTTP_HDR_FIRST;
	to->status = 0;
	to->protover = 0;
	to->conds = 0;
	memset(to->hd, 0, sizeof *to->hd * to->shd);
}

/*--------------------------------------------------------------------*/

void
http_SetHeader(struct http *to, const char *hdr)
{

	CHECK_OBJ_NOTNULL(to, HTTP_MAGIC);
	if (to->nhd >= to->shd) {
		VSC_C_main->losthdr++;
		VSLb(to->vsl, SLT_LostHeader, "%s", hdr);
		return;
	}
	http_SetH(to, to->nhd++, hdr);
}

/*--------------------------------------------------------------------*/

static void
http_PutField(const struct http *to, int field, const char *string)
{
	char *p;
	unsigned l;

	CHECK_OBJ_NOTNULL(to, HTTP_MAGIC);
	l = strlen(string);
	p = WS_Alloc(to->ws, l + 1);
	if (p == NULL) {
		VSLb(to->vsl, SLT_LostHeader, "%s", string);
		to->hd[field].b = NULL;
		to->hd[field].e = NULL;
		to->hdf[field] = 0;
	} else {
		memcpy(p, string, l + 1L);
		to->hd[field].b = p;
		to->hd[field].e = p + l;
		to->hdf[field] = 0;
	}
}

void
http_PutProtocol(const struct http *to, const char *protocol)
{

	http_PutField(to, HTTP_HDR_PROTO, protocol);
	if (to->hd[HTTP_HDR_PROTO].b == NULL)
		http_SetH(to, HTTP_HDR_PROTO, "HTTP/1.1");
	Tcheck(to->hd[HTTP_HDR_PROTO]);
}

void
http_PutStatus(struct http *to, uint16_t status)
{

	assert(status >= 100 && status <= 999);
	to->status = status;
}

void
http_PutResponse(const struct http *to, const char *response)
{

	http_PutField(to, HTTP_HDR_RESPONSE, response);
	if (to->hd[HTTP_HDR_RESPONSE].b == NULL)
		http_SetH(to, HTTP_HDR_RESPONSE, "Lost Response");
	Tcheck(to->hd[HTTP_HDR_RESPONSE]);
}

void
http_PrintfHeader(struct http *to, const char *fmt, ...)
{
	va_list ap;
	unsigned l, n;

	CHECK_OBJ_NOTNULL(to, HTTP_MAGIC);
	l = WS_Reserve(to->ws, 0);
	va_start(ap, fmt);
	n = vsnprintf(to->ws->f, l, fmt, ap);
	va_end(ap);
	if (n + 1 >= l || to->nhd >= to->shd) {
		VSC_C_main->losthdr++;
		VSLb(to->vsl, SLT_LostHeader, "%s", to->ws->f);
		WS_Release(to->ws, 0);
	} else {
		to->hd[to->nhd].b = to->ws->f;
		to->hd[to->nhd].e = to->ws->f + n;
		to->hdf[to->nhd] = 0;
		WS_Release(to->ws, n + 1);
		to->nhd++;
	}
}
/*--------------------------------------------------------------------*/

void
http_Unset(struct http *hp, const char *hdr)
{
	uint16_t u, v;

	for (v = u = HTTP_HDR_FIRST; u < hp->nhd; u++) {
		if (hp->hd[u].b == NULL)
			continue;
		if (http_IsHdr(&hp->hd[u], hdr))
			continue;
		if (v != u) {
			memcpy(&hp->hd[v], &hp->hd[u], sizeof *hp->hd);
			memcpy(&hp->hdf[v], &hp->hdf[u], sizeof *hp->hdf);
		}
		v++;
	}
	hp->nhd = v;
}

/*--------------------------------------------------------------------*/

void
HTTP_Copy(struct http *to, const struct http * const fm)
{

	to->conds = fm->conds;
	to->logtag = fm->logtag;
	to->status = fm->status;
	to->protover = fm->protover;
	to->nhd = fm->nhd;
	assert(fm->nhd <= to->shd);
	memcpy(to->hd, fm->hd, fm->nhd * sizeof *to->hd);
	memcpy(to->hdf, fm->hdf, fm->nhd * sizeof *to->hdf);
}

/*--------------------------------------------------------------------*/

unsigned
http_Write(const struct worker *w, const struct http *hp, int resp)
{
	unsigned u, l;

	if (resp) {
		l = WRW_WriteH(w, &hp->hd[HTTP_HDR_PROTO], " ");
		http_VSLH(hp, HTTP_HDR_PROTO);

		hp->hd[HTTP_HDR_STATUS].b = WS_Alloc(hp->ws, 4);
		AN(hp->hd[HTTP_HDR_STATUS].b);

		sprintf(hp->hd[HTTP_HDR_STATUS].b, "%3d", hp->status);
		hp->hd[HTTP_HDR_STATUS].e = hp->hd[HTTP_HDR_STATUS].b + 3;

		l += WRW_WriteH(w, &hp->hd[HTTP_HDR_STATUS], " ");
		http_VSLH(hp, HTTP_HDR_STATUS);

		l += WRW_WriteH(w, &hp->hd[HTTP_HDR_RESPONSE], "\r\n");
		http_VSLH(hp, HTTP_HDR_RESPONSE);
	} else {
		AN(hp->hd[HTTP_HDR_URL].b);
		l = WRW_WriteH(w, &hp->hd[HTTP_HDR_REQ], " ");
		http_VSLH(hp, HTTP_HDR_REQ);
		l += WRW_WriteH(w, &hp->hd[HTTP_HDR_URL], " ");
		http_VSLH(hp, HTTP_HDR_URL);
		l += WRW_WriteH(w, &hp->hd[HTTP_HDR_PROTO], "\r\n");
		http_VSLH(hp, HTTP_HDR_PROTO);
	}
	for (u = HTTP_HDR_FIRST; u < hp->nhd; u++) {
		if (hp->hd[u].b == NULL)
			continue;
		AN(hp->hd[u].b);
		AN(hp->hd[u].e);
		l += WRW_WriteH(w, &hp->hd[u], "\r\n");
		http_VSLH(hp, u);
	}
	l += WRW_Write(w, "\r\n", -1);
	return (l);
}

/*--------------------------------------------------------------------*/

void
HTTP_Init(void)
{

#define HTTPH(a, b, c) b[0] = (char)strlen(b + 1);
#include "tbl/http_headers.h"
#undef HTTPH
}

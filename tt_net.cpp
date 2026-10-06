// tt_net.cpp - posting to a Discord webhook without the game ever waiting on
// the network.
//
// Requests go into a small queue and are made by ONE worker thread with plain
// blocking calls and short timeouts. Nothing on the worker touches the
// engine: it gets copies of everything it needs (link, body) and hands
// results back under a lock, which TT_NetFrame picks up on the game thread.
//
//   Windows: WinHTTP (part of Windows), HTTPS included.
//   Linux:   sockets + BearSSL (third_party/bearssl, MIT licence) for HTTPS,
//            checked against the system's root certificates
//            (/etc/ssl/certs/...) or, failing that, the roots built in
//            (tt_trust_anchors.h). The pthread functions are looked up at
//            run time (dlsym), so the plugin still loads on any glibc.
//
// A 429 (rate limited) waits the time Discord asks and tries once more.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "bearssl.h"
#include "tt_trust_anchors.h"
#endif

#include "tt_common.h"
#include "tt_crypto.h"
#include "tt_net.h"

TTNetConfig g_net;

#define TT_NET_TIMEOUT_MS 8000
#define TT_NET_MAX_RESPONSE 65536
#define TT_NET_QUEUE 24

// ---------------------------------------------------------------------------
// TTBuf
TTBuf::~TTBuf() { free(p); }

void TTBuf::addn(const char *s, size_t n)
{
	if (len + n + 1 > cap)
	{
		size_t nc = cap ? cap * 2 : 256;
		while (nc < len + n + 1)
			nc *= 2;
		char *np = (char *)realloc(p, nc);
		if (!np)
			return;
		p = np;
		cap = nc;
	}
	memcpy(p + len, s, n);
	len += n;
	p[len] = 0;
}

void TTBuf::add(const char *s) { if (s) addn(s, strlen(s)); }

void TTBuf::addf(const char *fmt, ...)
{
	char tmp[2048];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(tmp))
		n = (int)sizeof(tmp) - 1;
	addn(tmp, (size_t)n);
}

void TTBuf::json(const char *s)
{
	add("\"");
	for (const unsigned char *c = (const unsigned char *)(s ? s : ""); *c; c++)
	{
		switch (*c)
		{
		case '"':  add("\\\""); break;
		case '\\': add("\\\\"); break;
		case '\n': add("\\n"); break;
		case '\r': add("\\r"); break;
		case '\t': add("\\t"); break;
		default:
			if (*c < 0x20)
				addf("\\u%04x", *c);
			else
				addn((const char *)c, 1);
		}
	}
	add("\"");
}

// Player names are not markdown: escape what Discord would format.
void TTBuf::discord(const char *s)
{
	for (const char *c = s ? s : ""; *c; c++)
	{
		if (strchr("\\*_~`|>#[]()<@:-", *c))
			addn("\\", 1);
		addn(c, 1);
	}
}

// ---------------------------------------------------------------------------
// Settings
void TT_NetConfigDefaults(void)
{
	memset(&g_net, 0, sizeof(g_net));
	g_net.enabled = 1;
}

bool TT_NetConfigKey(const char *key, const char *val)
{
	if (!strcasecmp(key, "net_enabled"))              g_net.enabled = atoi(val);
	else if (!strcasecmp(key, "net_server_name"))     { strncpy(g_net.serverName, val, sizeof(g_net.serverName) - 1); g_net.serverName[sizeof(g_net.serverName) - 1] = 0; }
	else if (!strcasecmp(key, "net_ca_file"))         { strncpy(g_net.caFile, val, sizeof(g_net.caFile) - 1); g_net.caFile[sizeof(g_net.caFile) - 1] = 0; }
	// Settings of the 1.1.0 test builds (joins, live status, website, channels):
	// gone - only the end-of-map awards are posted now. Taken quietly.
	else if (!strncasecmp(key, "net_", 4))            ;
	else
		return false;
	return true;
}

const char *TT_NetServerName(void)
{
	if (g_net.serverName[0])
		return g_net.serverName;
	const char *h = CVAR_GET_STRING("hostname");
	return (h && h[0]) ? h : "TFC server";
}

// ---------------------------------------------------------------------------
// Threads, the portable minimum.
#ifdef _WIN32
typedef CRITICAL_SECTION TTMutex;
static void MxInit(TTMutex *m) { InitializeCriticalSection(m); }
static void MxLock(TTMutex *m) { EnterCriticalSection(m); }
static void MxUnlock(TTMutex *m) { LeaveCriticalSection(m); }
static void MxFree(TTMutex *m) { DeleteCriticalSection(m); }
static HANDLE g_wake = NULL;   // auto-reset event
static HANDLE g_thread = NULL;
static void SleepMs(int ms) { Sleep((DWORD)ms); }
#else
typedef pthread_mutex_t TTMutex;
// Looked up at run time: linking pthread directly would tie the plugin to
// the build machine's glibc version (2.34 moved pthread into libc).
static int (*p_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static int (*p_join)(pthread_t, void **);
static int (*p_mx_init)(pthread_mutex_t *, const pthread_mutexattr_t *);
static int (*p_mx_lock)(pthread_mutex_t *);
static int (*p_mx_unlock)(pthread_mutex_t *);
static int (*p_mx_destroy)(pthread_mutex_t *);
static bool g_pthreadOk = false;
static void MxInit(TTMutex *m) { p_mx_init(m, NULL); }
static void MxLock(TTMutex *m) { p_mx_lock(m); }
static void MxUnlock(TTMutex *m) { p_mx_unlock(m); }
static void MxFree(TTMutex *m) { p_mx_destroy(m); }
static pthread_t g_thread;
static bool g_threadRunning = false;
static void SleepMs(int ms) { usleep((useconds_t)ms * 1000); }

static bool TT_LoadPthread(void)
{
	if (g_pthreadOk)
		return true;
	void *h = dlopen("libpthread.so.0", RTLD_NOW);
	void *any = h ? h : RTLD_DEFAULT;
	p_create = (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *))dlsym(any, "pthread_create");
	p_join = (int (*)(pthread_t, void **))dlsym(any, "pthread_join");
	p_mx_init = (int (*)(pthread_mutex_t *, const pthread_mutexattr_t *))dlsym(any, "pthread_mutex_init");
	p_mx_lock = (int (*)(pthread_mutex_t *))dlsym(any, "pthread_mutex_lock");
	p_mx_unlock = (int (*)(pthread_mutex_t *))dlsym(any, "pthread_mutex_unlock");
	p_mx_destroy = (int (*)(pthread_mutex_t *))dlsym(any, "pthread_mutex_destroy");
	g_pthreadOk = p_create && p_join && p_mx_init && p_mx_lock && p_mx_unlock && p_mx_destroy;
	return g_pthreadOk;
}
#endif

// ---------------------------------------------------------------------------
// The queue
struct TTJob
{
	bool  used;
	char *body;
	char  url[512];
};

struct TTDestState
{
	int    sent, failed;
	int    lastCode;
	long   lastAt;        // unix time
	char   lastError[96];
};

static TTMutex g_mx;
static bool    g_mxReady = false;
static TTJob   g_queue[TT_NET_QUEUE];
static int     g_qHead = 0, g_qCount = 0;
static volatile bool g_stop = false;
static bool    g_started = false;
static TTDestState g_state;
static int     g_dropped = 0;
static bool    g_testPending = false;     // tt_net_test: tell admins in game how it went
static float   g_testUntil = 0;

static void TT_WorkerKick(void)
{
#ifdef _WIN32
	if (g_wake)
		SetEvent(g_wake);
#endif
}

static void TT_FreeJob(TTJob &j)
{
	free(j.body);
	j.body = NULL;
}

// ---------------------------------------------------------------------------
// URLs
struct TTUrl
{
	bool https;
	char host[256];
	int  port;
	char path[1024];   // path + query
};

static bool TT_ParseUrl(const char *u, TTUrl &o)
{
	memset(&o, 0, sizeof(o));
	const char *p;
	if (!strncmp(u, "https://", 8)) { o.https = true; o.port = 443; p = u + 8; }
	else if (!strncmp(u, "http://", 7)) { o.https = false; o.port = 80; p = u + 7; }
	else return false;
	size_t hl = strcspn(p, "/?:");
	if (!hl || hl >= sizeof(o.host))
		return false;
	memcpy(o.host, p, hl);
	o.host[hl] = 0;
	p += hl;
	if (*p == ':')
	{
		o.port = atoi(p + 1);
		if (o.port <= 0 || o.port > 65535)
			return false;
		p += 1 + strspn(p + 1, "0123456789");
	}
	if (*p == '?')
		_snprintf_wc(o.path, sizeof(o.path) - 1, "/%s", p);
	else
		strncpy(o.path, *p ? p : "/", sizeof(o.path) - 1);
	o.path[sizeof(o.path) - 1] = 0;
	return true;
}

// Insert "/messages/<id>" into a webhook link before any "?query", and add
// a query parameter.
static void TT_WebhookUrl(const char *base, const char *suffix, const char *param, char *out, size_t len)
{
	const char *q = strchr(base, '?');
	size_t bl = q ? (size_t)(q - base) : strlen(base);
	while (bl && base[bl - 1] == '/')
		bl--;
	_snprintf_wc(out, len - 1, "%.*s%s%s%s%s%s", (int)bl, base, suffix ? suffix : "",
		(q || param) ? "?" : "", q ? q + 1 : "", (q && param) ? "&" : "", param ? param : "");
	out[len - 1] = 0;
}

// ---------------------------------------------------------------------------
// One HTTP request, blocking (worker thread only).
struct TTResponse
{
	int    code;          // HTTP status, 0 = no response
	char  *body;
	size_t bodyLen;
	char   error[96];
};

static void TT_RespFree(TTResponse &r) { free(r.body); r.body = NULL; r.bodyLen = 0; }

#ifdef _WIN32
static wchar_t *TT_Wide(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
	if (!w)
		return NULL;
	if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n))
		w[0] = 0;
	return w;
}

static bool TT_Http(const char *method, const char *url, const char *headers, const char *body, size_t bodyLen, TTResponse &r)
{
	memset(&r, 0, sizeof(r));
	TTUrl u;
	if (!TT_ParseUrl(url, u))
	{
		strcpy(r.error, "bad link");
		return false;
	}
	bool ok = false;
	HINTERNET s = NULL, c = NULL, q = NULL;
	wchar_t *wHost = TT_Wide(u.host), *wPath = TT_Wide(u.path), *wMethod = TT_Wide(method), *wHdr = TT_Wide(headers ? headers : "");
	do
	{
		if (!wHost || !wPath || !wMethod || !wHdr)
			break;
		s = WinHttpOpen(L"tfc_teams_mm", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!s) { _snprintf_wc(r.error, sizeof(r.error) - 1, "WinHttpOpen %lu", GetLastError()); break; }
		WinHttpSetTimeouts(s, TT_NET_TIMEOUT_MS, TT_NET_TIMEOUT_MS, TT_NET_TIMEOUT_MS, TT_NET_TIMEOUT_MS);
		DWORD protos = 0x00000800 /* TLS 1.2 */ | 0x00002000 /* TLS 1.3 */;
		if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos)))
		{
			protos = 0x00000800;
			WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
		}
		c = WinHttpConnect(s, wHost, (INTERNET_PORT)u.port, 0);
		if (!c) { _snprintf_wc(r.error, sizeof(r.error) - 1, "connect %lu", GetLastError()); break; }
		q = WinHttpOpenRequest(c, wMethod, wPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
			u.https ? WINHTTP_FLAG_SECURE : 0);
		if (!q) { _snprintf_wc(r.error, sizeof(r.error) - 1, "request %lu", GetLastError()); break; }
		if (!WinHttpSendRequest(q, wHdr[0] ? wHdr : WINHTTP_NO_ADDITIONAL_HEADERS, wHdr[0] ? (DWORD)-1L : 0,
			(LPVOID)body, (DWORD)bodyLen, (DWORD)bodyLen, 0)
			|| !WinHttpReceiveResponse(q, NULL))
		{
			DWORD e = GetLastError();
			_snprintf_wc(r.error, sizeof(r.error) - 1, e == ERROR_WINHTTP_TIMEOUT ? "timed out" :
				(e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "name not found" :
				(e == ERROR_WINHTTP_CANNOT_CONNECT ? "cannot connect" :
				(e == ERROR_WINHTTP_SECURE_FAILURE ? "certificate / TLS failure" : "network error %lu"))), e);
			break;
		}
		DWORD code = 0, sz = sizeof(code);
		WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
			&code, &sz, WINHTTP_NO_HEADER_INDEX);
		r.code = (int)code;
		for (;;)
		{
			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(q, &avail) || !avail)
				break;
			if (r.bodyLen + avail > TT_NET_MAX_RESPONSE)
				avail = (DWORD)(TT_NET_MAX_RESPONSE - r.bodyLen);
			if (!avail)
				break;
			char *nb = (char *)realloc(r.body, r.bodyLen + avail + 1);
			if (!nb)
				break;
			r.body = nb;
			DWORD got = 0;
			if (!WinHttpReadData(q, r.body + r.bodyLen, avail, &got) || !got)
				break;
			r.bodyLen += got;
			r.body[r.bodyLen] = 0;
		}
		ok = true;
	} while (0);
	if (q) WinHttpCloseHandle(q);
	if (c) WinHttpCloseHandle(c);
	if (s) WinHttpCloseHandle(s);
	free(wHost); free(wPath); free(wMethod); free(wHdr);
	return ok;
}
#else
// ---- Linux: sockets + BearSSL ----
static br_x509_trust_anchor *g_tas = NULL;
static size_t g_taCount = 0;
static bool g_taLoaded = false;
static char g_taSource[300] = "";

struct TTBytes { unsigned char *p; size_t len, cap; };
static void BytesAdd(void *ctx, const void *data, size_t len)
{
	TTBytes *b = (TTBytes *)ctx;
	if (b->len + len > b->cap)
	{
		size_t nc = b->cap ? b->cap * 2 : 1024;
		while (nc < b->len + len)
			nc *= 2;
		unsigned char *np = (unsigned char *)realloc(b->p, nc);
		if (!np)
			return;
		b->p = np;
		b->cap = nc;
	}
	memcpy(b->p + b->len, data, len);
	b->len += len;
}

static unsigned char *Dup(const unsigned char *p, size_t n)
{
	unsigned char *d = (unsigned char *)malloc(n ? n : 1);
	if (d && n)
		memcpy(d, p, n);
	return d;
}

static bool TT_AddAnchor(const unsigned char *der, size_t len, br_x509_trust_anchor **list, size_t *count, size_t *cap)
{
	TTBytes dn = { 0, 0, 0 };
	br_x509_decoder_context dc;
	br_x509_decoder_init(&dc, BytesAdd, &dn);
	br_x509_decoder_push(&dc, der, len);
	br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
	if (!pk || !dn.len)
	{
		free(dn.p);
		return false;
	}
	if (*count == *cap)
	{
		size_t nc = *cap ? *cap * 2 : 64;
		br_x509_trust_anchor *nl = (br_x509_trust_anchor *)realloc(*list, nc * sizeof(**list));
		if (!nl)
		{
			free(dn.p);
			return false;
		}
		*list = nl;
		*cap = nc;
	}
	br_x509_trust_anchor &ta = (*list)[*count];
	memset(&ta, 0, sizeof(ta));
	ta.dn.data = dn.p;
	ta.dn.len = dn.len;
	ta.flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
	if (pk->key_type == BR_KEYTYPE_RSA)
	{
		ta.pkey.key_type = BR_KEYTYPE_RSA;
		ta.pkey.key.rsa.n = Dup(pk->key.rsa.n, pk->key.rsa.nlen);
		ta.pkey.key.rsa.nlen = pk->key.rsa.nlen;
		ta.pkey.key.rsa.e = Dup(pk->key.rsa.e, pk->key.rsa.elen);
		ta.pkey.key.rsa.elen = pk->key.rsa.elen;
	}
	else if (pk->key_type == BR_KEYTYPE_EC)
	{
		ta.pkey.key_type = BR_KEYTYPE_EC;
		ta.pkey.key.ec.curve = pk->key.ec.curve;
		ta.pkey.key.ec.q = Dup(pk->key.ec.q, pk->key.ec.qlen);
		ta.pkey.key.ec.qlen = pk->key.ec.qlen;
	}
	else
	{
		free(dn.p);
		return false;
	}
	(*count)++;
	return true;
}

static size_t TT_LoadPemFile(const char *path, br_x509_trust_anchor **list, size_t *count, size_t *cap)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	size_t before = *count;
	br_pem_decoder_context pc;
	br_pem_decoder_init(&pc);
	TTBytes der = { 0, 0, 0 };
	bool isCert = false;
	unsigned char buf[4096];
	size_t n;
	bool done = false;
	while (!done)
	{
		n = fread(buf, 1, sizeof(buf), f);
		if (!n)
		{
			// A file that does not end in a newline would leave the last object open.
			buf[0] = '\n';
			n = 1;
			done = true;
		}
		const unsigned char *p = buf;
		while (n > 0)
		{
			size_t t = br_pem_decoder_push(&pc, p, n);
			p += t;
			n -= t;
			switch (br_pem_decoder_event(&pc))
			{
			case BR_PEM_BEGIN_OBJ:
			{
				const char *name = br_pem_decoder_name(&pc);
				isCert = !strcmp(name, "CERTIFICATE") || !strcmp(name, "X509 CERTIFICATE") || !strcmp(name, "TRUSTED CERTIFICATE");
				der.len = 0;
				br_pem_decoder_setdest(&pc, BytesAdd, &der);
				break;
			}
			case BR_PEM_END_OBJ:
				if (isCert && der.len)
					TT_AddAnchor(der.p, der.len, list, count, cap);
				isCert = false;
				der.len = 0;
				break;
			case BR_PEM_ERROR:
				done = true;
				n = 0;
				break;
			default:
				break;
			}
		}
	}
	free(der.p);
	fclose(f);
	return *count - before;
}

static void TT_LoadAnchors(const char *caFile)
{
	if (g_taLoaded)
		return;
	g_taLoaded = true;
	size_t cap = 0;
	static const char *paths[] = {
		"/etc/ssl/certs/ca-certificates.crt",                   // Debian, Ubuntu
		"/etc/pki/tls/certs/ca-bundle.crt",                     // RHEL, CentOS, Fedora
		"/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
		"/etc/ssl/ca-bundle.pem",                               // openSUSE
		"/etc/ca-certificates/extracted/tls-ca-bundle.pem",     // Arch
		"/etc/ssl/cert.pem",                                    // Alpine
		NULL };
	if (caFile && caFile[0] && TT_LoadPemFile(caFile, &g_tas, &g_taCount, &cap))
	{
		_snprintf_wc(g_taSource, sizeof(g_taSource) - 1, "%s (%d roots)", caFile, (int)g_taCount);
		return;
	}
	for (int i = 0; paths[i]; i++)
		if (TT_LoadPemFile(paths[i], &g_tas, &g_taCount, &cap))
		{
			_snprintf_wc(g_taSource, sizeof(g_taSource) - 1, "%s (%d roots)", paths[i], (int)g_taCount);
			return;
		}
	_snprintf_wc(g_taSource, sizeof(g_taSource) - 1, "built-in roots (%d)", (int)TT_BUILTIN_TAS_NUM);
}

static void TT_FreeAnchors(void)
{
	for (size_t i = 0; i < g_taCount; i++)
	{
		br_x509_trust_anchor &t = g_tas[i];
		free(t.dn.data);
		if (t.pkey.key_type == BR_KEYTYPE_RSA) { free(t.pkey.key.rsa.n); free(t.pkey.key.rsa.e); }
		else if (t.pkey.key_type == BR_KEYTYPE_EC) free(t.pkey.key.ec.q);
	}
	free(g_tas);
	g_tas = NULL;
	g_taCount = 0;
	g_taLoaded = false;
}

static int TT_Connect(const char *host, int port, char *err, size_t errLen)
{
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	char ps[8];
	_snprintf_wc(ps, sizeof(ps) - 1, "%d", port);
	ps[sizeof(ps) - 1] = 0;
	if (getaddrinfo(host, ps, &hints, &res) != 0 || !res)
	{
		_snprintf_wc(err, errLen - 1, "name not found");
		return -1;
	}
	int fd = -1;
	for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next)
	{
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0)
			continue;
		int fl = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		int rc = connect(fd, a->ai_addr, a->ai_addrlen);
		if (rc < 0 && errno == EINPROGRESS)
		{
			fd_set w;
			FD_ZERO(&w);
			FD_SET(fd, &w);
			struct timeval tv = { TT_NET_TIMEOUT_MS / 1000, 0 };
			rc = select(fd + 1, NULL, &w, NULL, &tv) == 1 ? 0 : -1;
			if (rc == 0)
			{
				int soerr = 0;
				socklen_t sl = sizeof(soerr);
				getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
				if (soerr)
					rc = -1;
			}
		}
		if (rc < 0)
		{
			close(fd);
			fd = -1;
			continue;
		}
		fcntl(fd, F_SETFL, fl);
		struct timeval to = { TT_NET_TIMEOUT_MS / 1000, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	}
	freeaddrinfo(res);
	if (fd < 0)
		_snprintf_wc(err, errLen - 1, "cannot connect");
	return fd;
}

static int SockRead(void *ctx, unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	for (;;)
	{
		ssize_t r = recv(fd, buf, len, 0);
		if (r < 0 && errno == EINTR)
			continue;
		return r <= 0 ? -1 : (int)r;
	}
}

static int SockWrite(void *ctx, const unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	for (;;)
	{
		ssize_t r = send(fd, buf, len, MSG_NOSIGNAL);
		if (r < 0 && errno == EINTR)
			continue;
		return r <= 0 ? -1 : (int)r;
	}
}

// Has the whole response arrived? (Content-Length or chunked; else wait for close.)
static bool TT_ResponseComplete(const char *d, size_t n)
{
	const char *he = NULL;
	for (size_t i = 0; i + 3 < n; i++)
		if (d[i] == '\r' && d[i + 1] == '\n' && d[i + 2] == '\r' && d[i + 3] == '\n') { he = d + i + 4; break; }
	if (!he)
		return false;
	size_t hl = (size_t)(he - d), bl = n - hl;
	// Lower-cased copy of the headers to search.
	char h[4096];
	size_t c = hl < sizeof(h) - 1 ? hl : sizeof(h) - 1;
	for (size_t i = 0; i < c; i++)
		h[i] = (char)((d[i] >= 'A' && d[i] <= 'Z') ? d[i] + 32 : d[i]);
	h[c] = 0;
	int code = atoi(h + 9);
	if (code == 204 || code == 304 || (code >= 100 && code < 200))
		return true;
	const char *cl = strstr(h, "\ncontent-length:");
	if (cl)
		return bl >= (size_t)atol(cl + 16);
	if (strstr(h, "\ntransfer-encoding:") && strstr(h, "chunked"))
		return bl >= 5 && !memcmp(d + n - 5, "0\r\n\r\n", 5);
	return false;
}

static void TT_ParseResponse(const char *d, size_t n, TTResponse &r)
{
	if (n < 12 || strncmp(d, "HTTP/", 5))
		return;
	const char *sp = (const char *)memchr(d, ' ', n);
	r.code = sp ? atoi(sp + 1) : 0;
	const char *he = NULL;
	for (size_t i = 0; i + 3 < n; i++)
		if (d[i] == '\r' && d[i + 1] == '\n' && d[i + 2] == '\r' && d[i + 3] == '\n') { he = d + i + 4; break; }
	if (!he)
		return;
	size_t hl = (size_t)(he - d);
	bool chunked = false;
	for (size_t i = 0; i + 26 < hl; i++)
		if (!strncasecmp(d + i, "\ntransfer-encoding: chunked", 27)) { chunked = true; break; }
	r.body = (char *)malloc(n - hl + 1);
	if (!r.body)
		return;
	if (!chunked)
	{
		memcpy(r.body, he, n - hl);
		r.bodyLen = n - hl;
	}
	else
	{
		const char *p = he, *end = d + n;
		while (p < end)
		{
			long sz = strtol(p, NULL, 16);
			const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
			if (!nl || sz <= 0)
				break;
			p = nl + 1;
			if (p + sz > end)
				sz = (long)(end - p);
			memcpy(r.body + r.bodyLen, p, (size_t)sz);
			r.bodyLen += (size_t)sz;
			p += sz + 2;
		}
	}
	r.body[r.bodyLen] = 0;
}

static bool TT_Http(const char *method, const char *url, const char *headers, const char *body, size_t bodyLen, TTResponse &r)
{
	memset(&r, 0, sizeof(r));
	TTUrl u;
	if (!TT_ParseUrl(url, u))
	{
		strcpy(r.error, "bad link");
		return false;
	}
	int fd = TT_Connect(u.host, u.port, r.error, sizeof(r.error));
	if (fd < 0)
		return false;

	TTBuf req;
	req.addf("%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: tfc_teams_mm\r\nConnection: close\r\nContent-Length: %d\r\n",
		method, u.path, u.host, (int)bodyLen);
	if (headers)
		req.add(headers);
	req.add("\r\n");

	TTBytes resp = { 0, 0, 0 };
	bool ok = false;
	if (!u.https)
	{
		if (SockWrite(&fd, (const unsigned char *)req.c_str(), req.len) == (int)req.len
			&& (!bodyLen || SockWrite(&fd, (const unsigned char *)body, bodyLen) == (int)bodyLen))
		{
			unsigned char b[4096];
			int got;
			while (resp.len < TT_NET_MAX_RESPONSE && (got = SockRead(&fd, b, sizeof(b))) > 0)
			{
				BytesAdd(&resp, b, (size_t)got);
				if (TT_ResponseComplete((const char *)resp.p, resp.len))
					break;
			}
			ok = resp.len > 0;
			if (!ok)
				_snprintf_wc(r.error, sizeof(r.error) - 1, "no answer");
		}
		else
			_snprintf_wc(r.error, sizeof(r.error) - 1, "send failed");
	}
	else
	{
		TT_LoadAnchors(g_net.caFile);
		br_ssl_client_context *sc = (br_ssl_client_context *)malloc(sizeof(br_ssl_client_context));
		br_x509_minimal_context *xc = (br_x509_minimal_context *)malloc(sizeof(br_x509_minimal_context));
		unsigned char *iobuf = (unsigned char *)malloc(BR_SSL_BUFSIZE_BIDI);
		if (sc && xc && iobuf)
		{
			if (g_taCount)
				br_ssl_client_init_full(sc, xc, g_tas, g_taCount);
			else
				br_ssl_client_init_full(sc, xc, TT_BuiltinTAs, TT_BUILTIN_TAS_NUM);
			br_ssl_engine_set_buffer(&sc->eng, iobuf, BR_SSL_BUFSIZE_BIDI, 1);
			br_ssl_client_reset(sc, u.host, 0);
			br_sslio_context io;
			br_sslio_init(&io, &sc->eng, SockRead, &fd, SockWrite, &fd);
			if (br_sslio_write_all(&io, req.c_str(), req.len) == 0
				&& (!bodyLen || br_sslio_write_all(&io, body, bodyLen) == 0)
				&& br_sslio_flush(&io) == 0)
			{
				unsigned char b[4096];
				int got;
				while (resp.len < TT_NET_MAX_RESPONSE && (got = br_sslio_read(&io, b, sizeof(b))) > 0)
				{
					BytesAdd(&resp, b, (size_t)got);
					if (TT_ResponseComplete((const char *)resp.p, resp.len))
						break;
				}
			}
			int e = br_ssl_engine_last_error(&sc->eng);
			ok = resp.len > 0;
			if (!ok)
			{
				if (e >= BR_ERR_X509_OK + 1 && e < BR_ERR_X509_OK + 64)
					_snprintf_wc(r.error, sizeof(r.error) - 1, "certificate not trusted (x509 error %d)", e);
				else if (e)
					_snprintf_wc(r.error, sizeof(r.error) - 1, "TLS error %d", e);
				else
					_snprintf_wc(r.error, sizeof(r.error) - 1, "no answer");
			}
		}
		else
			_snprintf_wc(r.error, sizeof(r.error) - 1, "out of memory");
		free(sc);
		free(xc);
		free(iobuf);
	}
	close(fd);
	if (ok)
		TT_ParseResponse((const char *)resp.p, resp.len, r);
	free(resp.p);
	if (ok && !r.code)
	{
		ok = false;
		_snprintf_wc(r.error, sizeof(r.error) - 1, "not an HTTP answer");
	}
	return ok;
}
#endif

// ---------------------------------------------------------------------------
// The worker
static float TT_RetryAfter(const TTResponse &r)
{
	const char *ra = r.body ? strstr(r.body, "\"retry_after\"") : NULL;
	if (!ra)
		return 2.0f;
	ra = strchr(ra, ':');
	float s = ra ? (float)atof(ra + 1) : 2.0f;
	return s < 0.2f ? 0.2f : (s > 15.0f ? 15.0f : s);
}

static void TT_Record(bool ok, int code, const char *err)
{
	MxLock(&g_mx);
	TTDestState &s = g_state;
	if (ok) s.sent++; else s.failed++;
	s.lastCode = code;
	s.lastAt = (long)time(NULL);
	strncpy(s.lastError, err ? err : "", sizeof(s.lastError) - 1);
	s.lastError[sizeof(s.lastError) - 1] = 0;
	MxUnlock(&g_mx);
}

static void TT_DoJob(TTJob &j)
{
	const char *json = "Content-Type: application/json\r\n";
	TTResponse r;
	memset(&r, 0, sizeof(r));
	char url[700];
	bool ok = false;
	TT_WebhookUrl(j.url, NULL, "wait=false", url, sizeof(url));
	for (int attempt = 0; attempt < 2 && !g_stop; attempt++)
	{
		ok = TT_Http("POST", url, json, j.body, strlen(j.body), r);
		if (ok && r.code == 429 && attempt == 0)
		{
			float wait = TT_RetryAfter(r);
			TT_RespFree(r);
			for (int t = 0; t < (int)(wait * 10) && !g_stop; t++)
				SleepMs(100);
			continue;
		}
		break;
	}
	bool good = ok && r.code >= 200 && r.code < 300;
	char err[96] = "";
	if (!ok)
		strcpy(err, r.error);
	else if (!good)
	{
		// Discord explains in a JSON "message"; keep the start of it.
		const char *msg = r.body ? strstr(r.body, "\"message\"") : NULL;
		_snprintf_wc(err, sizeof(err) - 1, "HTTP %d%s%.60s", r.code, msg ? " " : "", msg ? msg + 10 : "");
		err[sizeof(err) - 1] = 0;
	}
	TT_Record(good, ok ? r.code : 0, err);
	TT_RespFree(r);
}

#ifdef _WIN32
static DWORD WINAPI TT_Worker(LPVOID)
#else
static void *TT_Worker(void *)
#endif
{
	while (!g_stop)
	{
		TTJob j;
		bool have = false;
		MxLock(&g_mx);
		if (g_qCount)
		{
			j = g_queue[g_qHead];
			g_queue[g_qHead].body = NULL;
			g_qHead = (g_qHead + 1) % TT_NET_QUEUE;
			g_qCount--;
			have = true;
		}
		MxUnlock(&g_mx);
		if (!have)
		{
#ifdef _WIN32
			WaitForSingleObject(g_wake, 500);
#else
			SleepMs(100);
#endif
			continue;
		}
		if (j.used)
			TT_DoJob(j);
		TT_FreeJob(j);
	}
	return 0;
}

static bool TT_StartWorker(void)
{
	if (g_started)
		return true;
#ifdef _WIN32
	g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
	g_thread = CreateThread(NULL, 0, TT_Worker, NULL, 0, NULL);
	g_started = g_thread != NULL;
#else
	if (!TT_LoadPthread())
	{
		TT_Trace("Net: pthread not found - nothing can be sent");
		return false;
	}
	if (!g_mxReady)
	{
		MxInit(&g_mx);
		g_mxReady = true;
	}
	g_stop = false;
	g_started = g_threadRunning = p_create(&g_thread, NULL, TT_Worker, NULL) == 0;
#endif
	if (!g_started)
		TT_Trace("Net: could not start the sending thread");
	return g_started;
}

// ---------------------------------------------------------------------------
// Game-thread side
void TT_NetInit(void)
{
#ifdef _WIN32
	if (!g_mxReady)
	{
		MxInit(&g_mx);
		g_mxReady = true;
	}
#endif
	TT_SecretLoad();
	memset(&g_state, 0, sizeof(g_state));
}

void TT_NetShutdown(void)
{
	if (g_started)
	{
		g_stop = true;
		TT_WorkerKick();
#ifdef _WIN32
		// A request in progress ends at its timeout at the latest.
		WaitForSingleObject(g_thread, TT_NET_TIMEOUT_MS * 3 + 1000);
		CloseHandle(g_thread);
		CloseHandle(g_wake);
		g_thread = g_wake = NULL;
#else
		if (g_threadRunning)
			p_join(g_thread, NULL);
		g_threadRunning = false;
#endif
		g_started = false;
	}
	if (g_mxReady)
	{
		for (int i = 0; i < TT_NET_QUEUE; i++)
			TT_FreeJob(g_queue[i]);
		g_qCount = 0;
		MxFree(&g_mx);
		g_mxReady = false;
	}
#ifndef _WIN32
	TT_FreeAnchors();
#endif
}

bool TT_NetSend(const char *body)
{
	if (!g_net.enabled || !body || !g_secret.discord[0])
		return false;
	if (!g_started && !TT_StartWorker())
		return false;
	char *copy = (char *)malloc(strlen(body) + 1);
	if (!copy)
		return false;
	strcpy(copy, body);
	MxLock(&g_mx);
	if (g_qCount == TT_NET_QUEUE)
	{
		// Full (the network is down): drop the oldest.
		TT_FreeJob(g_queue[g_qHead]);
		g_qHead = (g_qHead + 1) % TT_NET_QUEUE;
		g_qCount--;
		g_dropped++;
	}
	TTJob &j = g_queue[(g_qHead + g_qCount) % TT_NET_QUEUE];
	memset(&j, 0, sizeof(j));
	j.used = true;
	j.body = copy;
	strncpy(j.url, g_secret.discord, sizeof(j.url) - 1);
	g_qCount++;
	MxUnlock(&g_mx);
	TT_WorkerKick();
	return true;
}

void TT_NetFrame(void)
{
	if (!g_started)
		return;
	// Results into the trace log (and, after tt_net_test, to admins in game -
	// someone using amx_rcon never sees the server console).
	static int seenSent, seenFailed;
	char line[160] = "";
	MxLock(&g_mx);
	if (g_state.failed != seenFailed)
		_snprintf_wc(line, sizeof(line) - 1, "Discord: sending FAILED - %s", g_state.lastError);
	else if (g_state.sent != seenSent && (seenSent == 0 || g_testPending))
		_snprintf_wc(line, sizeof(line) - 1, "Discord: sent OK");
	line[sizeof(line) - 1] = 0;
	seenSent = g_state.sent;
	seenFailed = g_state.failed;
	MxUnlock(&g_mx);
	if (line[0])
	{
		TT_Trace("Net: %s", line);
		if (g_testPending)
		{
			for (int i = 1; i <= gpGlobals->maxClients && i <= TT_MAX_PLAYERS; i++)
			{
				edict_t *e = TT_Player(i);
				if (e && TT_IsAdmin(e))
					TT_Say(e, "%s %s", TT_TAG, line);
			}
			g_testPending = false;
		}
	}
	if (g_testPending && gpGlobals->time > g_testUntil + 30.0f)
		g_testPending = false;
}

void TT_NetTestStarted(void)
{
	g_testPending = true;
	g_testUntil = gpGlobals->time + 5.0f;   // collect the results of the next few seconds
}

void TT_NetPrintStatus(void)
{
	char m[200];
	TT_Printf("[Teams] End-of-map awards to Discord: %s\n", g_net.enabled ? "on (net_enabled 1)" : "OFF (net_enabled 0)");
	TT_SecretMask(g_secret.discord, m, sizeof(m));
	TT_Printf("  Webhook: %s\n", m);
	TT_Printf("  Stored in tt_secret.dat, locked to this machine (%s)\n", TT_SecretKeySource());
#ifndef _WIN32
	if (g_taLoaded)
		TT_Printf("  Certificates: %s\n", g_taSource);
#endif
	if (!g_mxReady)
		return;
	MxLock(&g_mx);
	TTDestState s = g_state;
	int q = g_qCount, dropped = g_dropped;
	MxUnlock(&g_mx);
	if (s.sent || s.failed)
	{
		long ago = s.lastAt ? (long)time(NULL) - s.lastAt : 0;
		TT_Printf("  %d sent, %d failed; last %lds ago: %s\n", s.sent, s.failed, ago, s.lastError[0] ? s.lastError : "OK");
	}
	if (dropped)
		TT_Printf("  Queue: %d waiting; %d dropped while the network was down\n", q, dropped);
	else
		TT_Printf("  Queue: %d waiting\n", q);
}

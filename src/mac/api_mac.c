/* src/mac/api_mac.c - Riptide on the classic Mac OS: the stack's calls for programs (RTMAC_API).
 *
 * MacTCP programs reach the stack through the .IPP driver's Control calls, which only queue a
 * parameter block for the stack's deferred task and complete it later (ipp.c, mactcp.c). The
 * Riptide Command Terminal (src/mac/term) runs the Amiga's network tools, which use the BSD socket
 * interface: calls that wait, as on the Amiga. So this file gives programs the core's socket
 * layer itself (rt_api_*), the resolver, sysctl and the configuration commands, called directly
 * in the program's own context, which is how a bsdsocket.library call runs in the calling task
 * on the Amiga.
 *
 * One context runs the core at a time. A call marks the core taken (in_app), and the deferred
 * task, should an interrupt bring it in meanwhile, leaves its work for later (os_mac.c,
 * mac_stack_run); the call gives the core back, and asks for the deferred task if it was put
 * off. A call that must wait sleeps on its client's sleeper (os_mac.c, rt_sleeper_wait): it
 * gives the core back, calls the program's yield (WaitNextEvent: the Mac and the deferred task,
 * which receives the frames, go on), takes the core again and looks once more.
 *
 * The table's address is the answer to the private Control call RTMAC_API (ipp.c); the table
 * itself is in src/mac/prefs/mac_ipp_private.h.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "rt_types.h"
#include "rt_os.h"
#include "rt_errno.h"
#include "rt_bsd.h"
#include "rt_ip.h"
#include "rt_netif.h"
#include "rt_socket.h"
#include "rt_ctl.h"
#include "rt_sysctl.h"
#include "riptide.h"
#include "mac.h"
#include "prefs/mac_ipp_private.h"

/* ---- clients ---- */
static struct mac_client *clients;

static int alive(struct mac_client *c)
{
	for (struct mac_client *k = clients; k; k = k->next)
		if (k == c)
			return 1;
	return 0;
}

#define ENTER(cl) struct mac_client *c = (struct mac_client *)(cl); int r; \
	if (!alive(c)) return -RT_EBADF; \
	mac_core_enter(c)
#define LEAVE() mac_core_leave(); return r

static void *api_attach(void (*yield)(void *), int (*brk)(void *), void *arg)
{
	struct mac_client *c = rt_zalloc(sizeof *c);
	if (!c)
		return NULL;
	c->yield = yield;
	c->brk = brk;
	c->arg = arg;
	if (rt_ctx_init(&c->sc, 64, c) < 0) {
		rt_free(c);
		return NULL;
	}
	mac_core_enter(c);
	c->next = clients;
	clients = c;
	mac_core_leave();
	return c;
}

static void api_detach(void *cl)
{
	struct mac_client *c = cl, **pp;
	if (!alive(c))
		return;
	mac_core_enter(c);
	rt_ctx_destroy(&c->sc);
	for (pp = &clients; *pp; pp = &(*pp)->next)
		if (*pp == c) {
			*pp = c->next;
			break;
		}
	mac_core_leave();
	rt_free(c);
}

/* ---- sockets ---- */
static int api_socket(void *cl, int d, int t, int p) { ENTER(cl); r = rt_api_socket(&c->sc, d, t, p); LEAVE(); }
static int api_bind(void *cl, int fd, const void *a, unsigned long l) { ENTER(cl); r = rt_api_bind(&c->sc, fd, a, (u32)l); LEAVE(); }
static int api_listen(void *cl, int fd, int b) { ENTER(cl); r = rt_api_listen(&c->sc, fd, b); LEAVE(); }
static int api_accept(void *cl, int fd, void *a, unsigned long *l) { ENTER(cl); r = rt_api_accept(&c->sc, fd, a, (u32 *)l); LEAVE(); }
static int api_connect(void *cl, int fd, const void *a, unsigned long l) { ENTER(cl); r = rt_api_connect(&c->sc, fd, a, (u32)l); LEAVE(); }
static int api_sendto(void *cl, int fd, const void *b, unsigned long n, int fl, const void *to, unsigned long tl)
{
	ENTER(cl);
	r = to ? rt_api_sendto(&c->sc, fd, b, (u32)n, fl, to, (u32)tl) : rt_api_send(&c->sc, fd, b, (u32)n, fl);
	LEAVE();
}
static int api_recvfrom(void *cl, int fd, void *b, unsigned long n, int fl, void *from, unsigned long *fl2)
{
	ENTER(cl);
	r = from ? rt_api_recvfrom(&c->sc, fd, b, (u32)n, fl, from, (u32 *)fl2) : rt_api_recv(&c->sc, fd, b, (u32)n, fl);
	LEAVE();
}
static int api_shutdown(void *cl, int fd, int how) { ENTER(cl); r = rt_api_shutdown(&c->sc, fd, how); LEAVE(); }
static int api_setsockopt(void *cl, int fd, int lv, int nm, const void *v, unsigned long l)
{
	ENTER(cl); r = rt_api_setsockopt(&c->sc, fd, lv, nm, v, (u32)l); LEAVE();
}
static int api_getsockopt(void *cl, int fd, int lv, int nm, void *v, unsigned long *l)
{
	ENTER(cl); r = rt_api_getsockopt(&c->sc, fd, lv, nm, v, (u32 *)l); LEAVE();
}
static int api_getsockname(void *cl, int fd, void *a, unsigned long *l) { ENTER(cl); r = rt_api_getsockname(&c->sc, fd, a, (u32 *)l); LEAVE(); }
static int api_getpeername(void *cl, int fd, void *a, unsigned long *l) { ENTER(cl); r = rt_api_getpeername(&c->sc, fd, a, (u32 *)l); LEAVE(); }
static int api_ioctl(void *cl, int fd, unsigned long req, void *arg) { ENTER(cl); r = rt_api_ioctl(&c->sc, fd, (u32)req, arg); LEAVE(); }
static int api_close(void *cl, int fd) { ENTER(cl); r = rt_api_close(&c->sc, fd); LEAVE(); }
static int api_select(void *cl, int nfds, unsigned long *rd, unsigned long *wr, unsigned long *ex, long ms,
                      unsigned long *sigmask)
{
	ENTER(cl);
	r = rt_api_select(&c->sc, nfds, (u32 *)rd, (u32 *)wr, (u32 *)ex, (s32)ms, (u32 *)sigmask);
	LEAVE();
}

/* ---- names ---- */
static int api_resolve(void *cl, const char *name, void *addrs, int max, int fam, char *canon, unsigned long cl2)
{
	ENTER(cl);
	r = rt_resolve_name46(name, addrs, max, fam, canon, (u32)cl2);
	LEAVE();
}
static int api_resolve_addr(void *cl, unsigned long a, char *name, unsigned long len)
{
	ENTER(cl); r = rt_resolve_addr((u32)a, name, (u32)len); LEAVE();
}
static int api_resolve_addr6(void *cl, const void *a, char *name, unsigned long len)
{
	ENTER(cl); r = rt_resolve_addr6(a, name, (u32)len); LEAVE();
}

/* ---- the stack's tables and settings ---- */
static int api_sysctl(void *cl, const long *mib, unsigned long n, void *old, unsigned long *oldlen, const void *nw,
                      unsigned long nl)
{
	u32 ol;
	ENTER(cl);
	ol = oldlen ? (u32)*oldlen : 0;
	if (!mib || !n || n > RT_SYSCTL_MAXMIB + 2) {
		r = -RT_EINVAL;
	} else {
		rt_lock();
		r = rt_sysctl_bymib((const s32 *)mib, (u32)n, old, oldlen ? &ol : NULL, nw, (u32)nl);
		rt_unlock();
		if (oldlen && (r == 0 || r == -RT_ENOMEM))
			*oldlen = ol;
	}
	LEAVE();
}
static int api_command(void *cl, const char *line, char *out, unsigned long outlen)
{
	ENTER(cl);
	if (out && outlen)
		out[0] = 0;
	rt_lock();
	r = rt_ctl_command(line, out, (u32)outlen, mac_config_unknown);
	rt_unlock();
	LEAVE();
}
static int api_if_index(void *cl, const char *name)
{
	struct rt_netif *n;
	ENTER(cl);
	rt_lock();
	n = rt_netif_find(name);
	r = n ? (int)n->index : 0;
	rt_unlock();
	LEAVE();
}
static int api_if_name(void *cl, unsigned long index, char *name)
{
	struct rt_netif *n;
	ENTER(cl);
	rt_lock();
	n = rt_netif_by_index((u32)index);
	if (n)
		rt_strlcpy(name, n->name, RT_IFNAMSIZ);
	r = n ? 0 : -RT_ENXIO;
	rt_unlock();
	LEAVE();
}
static int api_hostname(void *cl, char *name, unsigned long len)
{
	ENTER(cl);
	rt_strlcpy(name, rt_hostname[0] ? rt_hostname : "localhost", (u32)len);
	r = 0;
	LEAVE();
}

const struct rtmac_api mac_api = {
	RTMAC_API_MAGIC, RTMAC_API_VERSION, sizeof(struct rtmac_api),
	api_attach, api_detach,
	api_socket, api_bind, api_listen, api_accept, api_connect, api_sendto, api_recvfrom, api_shutdown,
	api_setsockopt, api_getsockopt, api_getsockname, api_getpeername, api_ioctl, api_close, api_select,
	api_resolve, api_resolve_addr, api_resolve_addr6,
	api_sysctl, api_command, api_if_index, api_if_name, api_hostname,
};

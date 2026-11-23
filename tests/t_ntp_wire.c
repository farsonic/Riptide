/*
 * Riptide - the SNTP client end to end, against tests/ntp_peer.py: a separate
 * process on the simulated Ethernet wire (src/host/wire.c) that is a DHCP,
 * DNS and SNTP server (needs python3, no privileges; exits 77 without it).
 *   1. DHCP: the lease names 192.0.2.1 as its NTP server (option 42); "ntp on"
 *      uses it and steps the clock (rt_set_wallclock_hook) by about +100 s
 *   2. four servers whose answers must be refused: leap indicator 3, stratum
 *      16, transmit time 0, an originate time that is not ours; no step
 *   3. a refused answer (mode 3), then a name looked up (A): the next works
 *   4. a name with only an AAAA record, and an IPv6 address: about -42.5 s
 *   5. a server in NTP era 1 (2036-02-08): the step lands after 2036
 *   6. the log lines: "ntp: time6.example (2001:db8::1) stratum 2, offset -42.499 s, clock set"
 *   Riptide 192.0.2.2/24, 2001:db8::2/64  <-- wire -->  peer 192.0.2.1 ..., 2001:db8::1
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "rt/rt_ctl.h"
#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stdout, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); } } while (0)

#define ERA1_START 2086041600u    /* 2036-02-08 00:00:00 UTC */

static const u8 mac_r[6] = { 0x02, 0x52, 0x54, 0, 2, 0x02 };
static volatile int hook_calls;
static volatile u32 hook_sec;
static volatile int ev_bound;

static int test_hook(u32 sec, u32 usec)
{
	(void)usec;
	hook_sec = sec;
	hook_calls++;
	return 0;
}

static void dhcp_cb(struct rt_netif *n, int ev)
{
	(void)n;
	if (ev == RT_DHCP_EV_BOUND)
		ev_bound++;
}

static int cmd(const char *line, char *out, u32 len)
{
	int r;

	rt_lock();
	r = rt_config_line(line, out, len, NULL);
	rt_unlock();
	return r;
}

/* "ntp status" until it holds want (or the time is up); the last answer in out */
static int status_has(const char *want, char *out, u32 len, int ms)
{
	for (; ms > 0; ms -= 100) {
		cmd("ntp status", out, len);
		if (strstr(out, want))
			return 1;
		usleep(100000);
	}
	cmd("ntp status", out, len);
	return strstr(out, want) != NULL;
}

static int hooked_after(int calls, int ms)
{
	for (; ms > 0 && hook_calls <= calls; ms -= 50)
		usleep(50000);
	return hook_calls > calls;
}

static int file_has(const char *path, const char *want)
{
	static char buf[65536];
	int fd = open(path, O_RDONLY), n;

	if (fd < 0)
		return 0;
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	return strstr(buf, want) != NULL;
}

static const char *find_peer(const char *argv0)
{
	static char path[1024];
	const char *slash = strrchr(argv0, '/');

	if (access("tests/ntp_peer.py", R_OK) == 0)
		return "tests/ntp_peer.py";
	if (slash) {
		snprintf(path, sizeof(path), "%.*s/../../../tests/ntp_peer.py", (int)(slash - argv0), argv0);
		if (access(path, R_OK) == 0)
			return path;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	char pr[64], pp[64], peerlog[80], rtlog[80], out[512], line[160];
	const char *script = find_peer(argv[0]);
	struct rt_netif *nif;
	struct rt_ctl_status st;
	pid_t peer;
	int i, calls, saved_err, logfd;
	long d;

	(void)argc;
	if (!script || system("python3 -c 'import socket, struct' >/dev/null 2>&1") != 0) {
		printf("t_ntp_wire: SKIP (needs python3 and tests/ntp_peer.py)\n");
		return 77;
	}
	snprintf(pr, sizeof(pr), "/tmp/rt_ntpw_%d_r", (int)getpid());
	snprintf(pp, sizeof(pp), "/tmp/rt_ntpw_%d_p", (int)getpid());
	snprintf(peerlog, sizeof(peerlog), "/tmp/rt_ntpw_%d_peer.log", (int)getpid());
	snprintf(rtlog, sizeof(rtlog), "/tmp/rt_ntpw_%d_rt.log", (int)getpid());
	peer = fork();
	if (peer < 0)
		return 1;
	if (peer == 0) {
		int fd = open(peerlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			dup2(fd, 1);
			close(fd);
		}
		execlp("python3", "python3", script, pp, pr, (char *)NULL);
		_exit(127);
	}
	alarm(120);
	for (i = 0; i < 100 && !file_has(peerlog, "ready"); i++)
		usleep(50000);
	CHECK(file_has(peerlog, "ready"));

	/* Riptide's log lines (INFO) into a file of their own, to look at them at the end */
	rt_log_level = RT_LOG_INFO;
	saved_err = dup(2);
	logfd = open(rtlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (logfd >= 0)
		dup2(logfd, 2);

	rt_strlcpy(rt_hostname, "ntptest", sizeof(rt_hostname));
	CHECK(rt_host_start() == 0);
	nif = rt_host_wire_open(pr, pp, mac_r);
	CHECK(nif != NULL);
	if (!nif)
		goto out;
	rt_set_wallclock_hook = test_hook;
	rt_lock();
	rt_netif_up(nif);
	rt_unlock();

	/* 1. the lease's NTP server */
	rt_lock();
	CHECK(rt_dhcp_start(nif, dhcp_cb) == 0);
	rt_unlock();
	for (i = 0; i < 100 && !ev_bound; i++)
		usleep(100000);
	CHECK(ev_bound > 0);
	CHECK(nif->addr == rt_htonl(0xc0000202u));
	CHECK(cmd("ntp on", out, sizeof(out)) == 0);
	CHECK(hooked_after(0, 8000));
	d = (long)hook_sec - (long)time(NULL);
	CHECK(d >= 98 && d <= 101);
	printf("1. DHCP's server: step to now%+ld s\n", d);
	CHECK(status_has("offset +100.", out, sizeof(out), 2000));
	printf("   %s\n", out);
	CHECK(strstr(out, "NTP: 192.0.2.1, stratum 2") && strstr(out, "(clock set)"));
	memset(&st, 0, sizeof(st));
	rt_lock();
	rt_ntp_ctl(&st);
	rt_unlock();
	CHECK(st.ntp_state == RT_CTL_NTP_SYNCED && (st.ntp_flags & RT_CTL_NTPF_STEPPED) &&
	      !(st.ntp_flags & RT_CTL_NTPF_BUSY) && st.ntp_stratum == 2 && !strcmp(st.ntp_server, "192.0.2.1"));
	CHECK(st.ntp_off_ms >= 99000 && st.ntp_off_ms <= 101000);

	/* 2. answers to refuse */
	calls = hook_calls;
	CHECK(cmd("ntp 192.0.2.11 192.0.2.12 192.0.2.13 192.0.2.14", out, sizeof(out)) == 0);
	CHECK(status_has("no valid answer", out, sizeof(out), 15000));
	printf("2. refused: %s\n", out);
	CHECK(hook_calls == calls);
	CHECK(file_has(peerlog, "ntp 192.0.2.11 li3") && file_has(peerlog, "ntp 192.0.2.12 s16") &&
	      file_has(peerlog, "ntp 192.0.2.13 zerotx") && file_has(peerlog, "ntp 192.0.2.14 badorig"));
	memset(&st, 0, sizeof(st));
	rt_lock();
	rt_ntp_ctl(&st);
	rt_unlock();
	CHECK(st.ntp_state == RT_CTL_NTP_FAILED && !strcmp(st.ntp_error, "no valid answer"));

	/* 3. mode 3 refused, then a name (A record) */
	calls = hook_calls;
	CHECK(cmd("ntp 192.0.2.15 time.example", out, sizeof(out)) == 0);
	CHECK(hooked_after(calls, 10000));
	d = (long)hook_sec - (long)time(NULL);
	CHECK(d >= 98 && d <= 101);
	CHECK(file_has(peerlog, "ntp 192.0.2.15 mode3") && file_has(peerlog, "dns A time.example"));
	CHECK(status_has("time.example (192.0.2.1)", out, sizeof(out), 2000));
	printf("3. after mode 3: %s\n", out);

	/* 4. IPv6: an AAAA-only name, then the address itself */
	snprintf(line, sizeof(line), "ifconfig %s inet6 2001:db8::2/64 nodad", nif->name);
	CHECK(cmd(line, out, sizeof(out)) == 0 || (printf("ifconfig: %s\n", out), 0));
	calls = hook_calls;
	CHECK(cmd("ntp time6.example", out, sizeof(out)) == 0);
	CHECK(hooked_after(calls, 10000));
	d = (long)hook_sec - (long)time(NULL);
	CHECK(d >= -44 && d <= -41);
	CHECK(file_has(peerlog, "dns AAAA time6.example") && file_has(peerlog, "ntp 2001:db8::1 v6"));
	CHECK(status_has("time6.example (2001:db8::1)", out, sizeof(out), 2000));
	printf("4. IPv6 by name: %s\n", out);
	CHECK(strstr(out, "offset -42.") != NULL);
	calls = hook_calls;
	CHECK(cmd("ntp 2001:db8::1", out, sizeof(out)) == 0);
	CHECK(hooked_after(calls, 8000));
	d = (long)hook_sec - (long)time(NULL);
	CHECK(d >= -44 && d <= -41);
	CHECK(status_has("NTP: 2001:db8::1, stratum 2", out, sizeof(out), 2000));
	printf("   IPv6 by address: %s\n", out);

	/* 5. NTP era 1 */
	calls = hook_calls;
	CHECK(cmd("ntp 192.0.2.20", out, sizeof(out)) == 0);
	CHECK(hooked_after(calls, 8000));
	printf("5. era 1: step to Unix %lu (2036-02-08 00:00:00 UTC is %lu)\n", (unsigned long)hook_sec,
	       (unsigned long)ERA1_START);
	CHECK(hook_sec >= ERA1_START && hook_sec < ERA1_START + 120);

	/* "ntp sync": a sync now */
	calls = hook_calls;
	CHECK(cmd("ntp sync", out, sizeof(out)) == 0 && strstr(out, "syncing now"));
	CHECK(hooked_after(calls, 8000));

out:
	cmd("ntp off", out, sizeof(out));
	rt_host_stop();
	kill(peer, SIGTERM);
	waitpid(peer, NULL, 0);
	fflush(stderr);
	if (saved_err >= 0) {
		dup2(saved_err, 2);
		close(saved_err);
	}
	if (logfd >= 0)
		close(logfd);
	/* 6. the log lines */
	CHECK(file_has(rtlog, "ntp: 192.0.2.1 stratum 2, offset +100."));
	CHECK(file_has(rtlog, "ntp: time.example (192.0.2.1) stratum 2, offset +100."));
	CHECK(file_has(rtlog, "ntp: time6.example (2001:db8::1) stratum 2, offset -42."));
	CHECK(file_has(rtlog, "s, clock set"));
	fflush(stdout);
	snprintf(line, sizeof(line), "sed -n '/ntp:/p' %s", rtlog);
	if (system(line) != 0)
		printf("(no log)\n");
	if (failures) {
		printf("--- peer:\n");
		fflush(stdout);
		snprintf(line, sizeof(line), "cat %s", peerlog);
		if (system(line) != 0)
			printf("(no peer log)\n");
		printf("--- riptide:\n");
		fflush(stdout);
		snprintf(line, sizeof(line), "cat %s", rtlog);
		if (system(line) != 0)
			printf("(no log)\n");
	}
	unlink(pr);
	unlink(pp);
	unlink(peerlog);
	unlink(rtlog);
	printf("t_ntp_wire: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

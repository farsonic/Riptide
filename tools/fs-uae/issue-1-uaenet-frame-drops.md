**Title:** uaenet.device drops every queued frame after the first one per interrupt; TCP downloads through slirp crawl at < 1 KB/s (3.2.35, also 3.1.66)

### Summary

In `uaenet_int_handler()` (`src/sana2.cpp`) the per-opener flag
`pdevst[].tmp`, which makes sure every opener gets a packet at most once,
is reset once per interrupt instead of once per packet. When more than one
packet is waiting in `dev->readqueue`, the first one is delivered; every
following one then finds `pdev->tmp == 1` for every opener and is freed as
"packet dropped", although the opener has plenty of `CMD_READ`s pending.

slirp emits two or three frames back to back for each ACK from the guest,
so a TCP download loses a segment in nearly every burst. slirp's TCP has
no SACK and only one or two duplicate ACKs follow the gap, so it recovers
by retransmission timeout each time (1.5 s, 3 s, 6 s, ...): downloads run
at a few hundred bytes per second.

### Affected versions

* 3.2.35: reset at `src/sana2.cpp:1429`, check at `:1448`, drop at `:1491`.
* 3.1.66: the same code at the same lines.
* The current master `sana2.cpp` (WinUAE-derived) delivers one packet per
  handler call and waits 100 frames before dropping one, so it does not
  seem to have this problem.

### Code (3.2.35)

```c
	for (i = 0; i < MAX_OPEN_DEVICES; i++)
		pdevst[i].tmp = 0;                 // once per interrupt
	...
			while (dev->readqueue) {
				...
				if (pdev && pdev->tmp == 0) {  // false for every packet after the first
					if (handleread (...)) {
						...
						pdev->tmp = 1;
				...
				if (!gotit) {
					write_log (_T("-> %p packet dropped, LEN=%d\n"), p, p->len);
```

### Log

With logging added to `uaenet_gotdata()` (`enqueue`) and to the accept and
`!gotit` paths of `uaenet_int_handler()`:

```
enqueue   len 1494 tcp seq 64002 data 1440
enqueue   len 1494 tcp seq 65442 data 1440
accepted  len 1494 tcp seq 64002 data 1440
DROPPED   len 1494 tcp seq 65442 data 1440
tx        len   54 tcp ack 65442
enqueue   len 1494 tcp seq 66882 data 1440
enqueue   len 1494 tcp seq 68322 data 1440
accepted  len 1494 tcp seq 66882 data 1440
DROPPED   ...
```

343 of 818 frames slirp handed to `uaenet_gotdata()` were dropped in that
run, while the Amiga stack had 48 IPv4 `CMD_READ`s pending throughout.

### Reproduce

FS-UAE 3.2.35, A1200 (68020, 8 MB Fast RAM), `uae_sana2 = 1`, a SANA-II
TCP/IP stack on `uaenet.device` unit 0 (I used Riptide, an open source
stack, https://github.com/farsonic/Riptide; any stack that keeps several
`CMD_READ`s queued shows it), `python3 -m http.server` on the host, and a
1 MB download from 10.0.2.2.

| FS-UAE 3.2.35 | Result |
|---|---|
| unmodified, Linux | 248 KB in about 500 s |
| unmodified, macOS, ftp from Aminet | about 0.3 KB/s |
| unmodified, macOS, guest TCP window limited to one segment (so slirp never queues two frames) | 227 KB/s |
| with the fix below, Linux | 300 KB in 4 to 5 s, no drops |

### Fix

Reset the flag for each packet; each opener still receives each packet at
most once.

```diff
--- a/src/sana2.cpp
+++ b/src/sana2.cpp
@@ -1436,6 +1436,11 @@ static uae_u32 REGPARAM2 uaenet_int_handler (TrapContext *ctx)
 				uae_u16 type;
 				p = dev->readqueue;
 				type = (p->data[2 * ADDR_SIZE] << 8) | p->data[2 * ADDR_SIZE + 1];
+				/* each opener gets each packet at most once: reset per
+				 * packet, not once per interrupt, or every packet after
+				 * the first one queued for this interrupt is dropped */
+				for (j = 0; j < MAX_OPEN_DEVICES; j++)
+					pdevst[j].tmp = 0;
 				ar = dev->ar;
 				gotit = 0;
 				while (ar) {
```

The same patch as a file:
https://github.com/farsonic/Riptide/blob/main/tools/fs-uae/0001-uaenet-deliver-every-queued-frame.patch

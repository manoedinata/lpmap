// lpmap - expose the logical partitions of an Android "super" partition as
// loop devices. Read-only on the LP metadata: nothing is ever written to super.
//
//   lpmap [-s slot] [-r] <super-dev> list
//   lpmap [-s slot] [-r|-w] <super-dev> map [name...]
//
// map: for each partition (or the named ones) with exactly one linear extent,
// attach /dev/loopN (offset/size of the extent) and create
//   /dev/<super-base>p<index>  (index = 1-based position in the LP table)
//   /dev/lp/<name>
// as symlinks to it. Partitions with the LP readonly attribute map read-only
// unless -w is given; -r forces read-only. Existing symlinks are left alone.
//
// Freestanding (raw aarch64 syscalls): the build container has no aarch64 libc.
// Build: make   (clang with an aarch64 target, lld; no libc needed)
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;

#define SYS_ioctl 29
#define SYS_mkdirat 34
#define SYS_symlinkat 36
#define SYS_openat 56
#define SYS_close 57
#define SYS_write 64
#define SYS_pread64 67
#define SYS_exit 93
#define AT_FDCWD (-100)
#define O_RDWR 2
#define O_RDONLY 0

static long sc(long n, long a, long b, long c, long d)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void out(int fd, const char *s) { sc(SYS_write, fd, (long)s, slen(s), 0); }
static void outn(int fd, u64 v)
{
	char b[24]; int i = 23; b[i] = 0;
	do { b[--i] = '0' + v % 10; v /= 10; } while (v);
	out(fd, b + i);
}
static void die(const char *m) { out(2, "lpmap: "); out(2, m); out(2, "\n"); sc(SYS_exit, 1, 0, 0, 0); }

static void cat(char *dst, unsigned max, const char *a, const char *b, const char *c)
{
	unsigned n = 0; const char *p[3] = { a, b, c };
	for (int i = 0; i < 3; i++)
		for (const char *s = p[i]; s && *s && n < max - 1; s++) dst[n++] = *s;
	dst[n] = 0;
}

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64(const u8 *p) { return rd32(p) | (u64)rd32(p + 4) << 32; }

struct loop_info64 {
	u64 device, inode, rdevice, offset, sizelimit;
	u32 number, encrypt_type, encrypt_key_size, flags;
	u8 file_name[64], crypt_name[64], encrypt_key[32];
	u64 init[2];
};
struct loop_config { u32 fd, block_size; struct loop_info64 info; u64 reserved[8]; };
#define LOOP_CONFIGURE 0x4C0A
#define LOOP_CTL_GET_FREE 0x4C82
#define LO_FLAGS_READ_ONLY 1

static u8 buf[1 << 20];

int main_(int argc, char **argv)
{
	int slot = 0, ro = 0, rw = 0, i = 1;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (seq(argv[i], "-r")) ro = 1;
		else if (seq(argv[i], "-w")) rw = 1;
		else if (seq(argv[i], "-s") && i + 1 < argc) slot = argv[++i][0] - '0';
		else die("usage: lpmap [-s slot] [-r] <super-dev> list|map [name...]");
	}
	if (argc - i < 2) die("usage: lpmap [-s slot] [-r] <super-dev> list|map [name...]");
	const char *dev = argv[i], *cmd = argv[i + 1];
	int domap = seq(cmd, "map");
	if (!domap && !seq(cmd, "list")) die("command must be list or map");

	int fd = sc(SYS_openat, AT_FDCWD, (long)dev, domap && !ro ? O_RDWR : O_RDONLY, 0);
	if (fd < 0) die("cannot open super device");

	/* geometry: reserved 4096, primary geometry at 4096 */
	if (sc(SYS_pread64, fd, (long)buf, 4096, 4096) != 4096) die("read geometry");
	if (rd32(buf) != 0x616c4467) die("no LP geometry magic");
	u32 maxsz = rd32(buf + 40), slots = rd32(buf + 44);
	if (slot < 0 || (u32)slot >= slots) die("bad metadata slot");
	if (maxsz == 0 || maxsz > sizeof buf) die("bad metadata_max_size");

	/* primary metadata: 8192 + 4096 (reserved+2 geometry copies), then slots */
	u64 moff = 12288 + (u64)slot * maxsz;
	if (sc(SYS_pread64, fd, (long)buf, maxsz, moff) != (long)maxsz) die("read metadata");
	if (rd32(buf) != 0x414c5030) die("no LP header magic");
	u32 hs = rd32(buf + 8), ts = rd32(buf + 44);
	if (hs < 128 || hs > 4096 || (u64)hs + ts > maxsz) die("bad header sizes");
	const u8 *t = buf + hs;
	u32 po = rd32(buf + 80), pn = rd32(buf + 84), ps = rd32(buf + 88);
	u32 eo = rd32(buf + 92), en = rd32(buf + 96), es = rd32(buf + 100);
	if (ps < 52 || es < 24 || (u64)po + (u64)pn * ps > ts || (u64)eo + (u64)en * es > ts)
		die("tables out of bounds");

	const char *base = dev;
	for (const char *s = dev; *s; s++) if (*s == '/') base = s + 1;
	if (domap) sc(SYS_mkdirat, AT_FDCWD, (long)"/dev/lp", 0755, 0);

	int bad = 0;
	for (u32 p = 0; p < pn; p++) {
		const u8 *e = t + po + p * ps;
		char name[37]; u32 k;
		for (k = 0; k < 36 && e[k]; k++) name[k] = e[k];
		name[k] = 0;
		u32 attr = rd32(e + 36), fe = rd32(e + 40), ne = rd32(e + 44);
		if ((u64)fe + ne > en) die("extent index out of bounds");

		u64 sectors = 0;
		for (u32 x = 0; x < ne; x++) sectors += rd64(t + eo + (fe + x) * es);

		if (!domap) {
			outn(1, p + 1); out(1, " "); out(1, name);
			out(1, " attr="); outn(1, attr);
			out(1, " extents="); outn(1, ne);
			out(1, " sectors="); outn(1, sectors);
			out(1, " MiB="); outn(1, sectors >> 11);
			for (u32 x = 0; x < ne; x++) {
				const u8 *xe = t + eo + (fe + x) * es;
				out(1, x ? "," : " ["); out(1, rd32(xe + 8) ? "zero" : "lin@");
				outn(1, rd64(xe + 12)); out(1, "+"); outn(1, rd64(xe));
			}
			out(1, ne ? "]\n" : "\n");
			continue;
		}

		int want = argc - i == 2;
		for (int a = i + 2; a < argc; a++) if (seq(argv[a], name)) want = 1;
		if (!want) continue;
		const u8 *xe = t + eo + fe * es;
		if (ne != 1 || rd32(xe + 8) != 0 || rd32(xe + 20) != 0) {
			out(2, "lpmap: skip "); out(2, name); out(2, ": not a single linear extent\n");
			bad = 1; continue;
		}

		char num[12]; int ni = 11; u32 pi = p + 1; num[ni] = 0;
		do { num[--ni] = '0' + pi % 10; pi /= 10; } while (pi);
		char byidx[64], byname[96], tmp[16];
		cat(byidx, sizeof byidx, "/dev/", base, "p"); 
		cat(byidx + slen(byidx), 20, num + ni, 0, 0);
		cat(byname, sizeof byname, "/dev/lp/", name, 0);

		int lc = sc(SYS_openat, AT_FDCWD, (long)"/dev/loop-control", O_RDWR, 0);
		if (lc < 0) die("no /dev/loop-control (CONFIG_BLK_DEV_LOOP / devtmpfs?)");
		long n = sc(SYS_ioctl, lc, LOOP_CTL_GET_FREE, 0, 0);
		sc(SYS_close, lc, 0, 0, 0);
		if (n < 0) die("LOOP_CTL_GET_FREE failed");
		unsigned long ln = 0; char d[8]; int di = 7; d[di] = 0;
		(void)ln; do { d[--di] = '0' + n % 10; n /= 10; } while (n);
		cat(tmp, sizeof tmp, "/dev/loop", d + di, 0);
		int ld = sc(SYS_openat, AT_FDCWD, (long)tmp, O_RDWR, 0);
		if (ld < 0) die("cannot open loop device");

		static struct loop_config cfg;
		u8 *z = (u8 *)&cfg; for (unsigned q = 0; q < sizeof cfg; q++) z[q] = 0;
		cfg.fd = fd; cfg.block_size = 4096;
		cfg.info.offset = rd64(xe + 12) * 512;
		cfg.info.sizelimit = sectors * 512;
		cfg.info.flags = ro || (!rw && (attr & 1)) ? LO_FLAGS_READ_ONLY : 0;
		for (k = 0; k < 63 && name[k]; k++) cfg.info.file_name[k] = name[k];
		if (sc(SYS_ioctl, ld, LOOP_CONFIGURE, (long)&cfg, 0) < 0) die("LOOP_CONFIGURE failed");
		sc(SYS_close, ld, 0, 0, 0);

		sc(SYS_symlinkat, (long)tmp, AT_FDCWD, (long)byidx, 0);
		sc(SYS_symlinkat, (long)tmp, AT_FDCWD, (long)byname, 0);
		out(1, byidx); out(1, " "); out(1, byname); out(1, " -> "); out(1, tmp);
		out(1, cfg.info.flags ? " (ro)\n" : "\n");
	}
	return bad;
}

void __attribute__((naked)) _start(void)
{
	__asm__ volatile(
		"ldr x0, [sp]\n"
		"add x1, sp, #8\n"
		"bl lpmap_main\n"
		"mov x8, #93\n"
		"svc 0\n");
}
int lpmap_main(long argc, char **argv) { return main_(argc, argv); }

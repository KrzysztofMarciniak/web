/*
 * web_pager - mailx-style pager for the output of web_plain.
 *
 *   cc -std=c89 -pedantic -Wall -O2 -o web_pager web_pager.c
 *
 *   wget -qO- 'http://example.com' | web_plain | web_pager
 *   curl -fsSL 'https://example.com' | web_plain | web_pager
 *
 * Reads plain text on stdin. If the text ends with a "References:" list
 * (as written by web_plain), those numbered URLs become the links that
 * 'l', 'N' and 'g N' work on. Commands are read from /dev/tty. Without a
 * /dev/tty, the text is copied to stdout unchanged.
 *
 * No networking in here. Links are followed by running
 *   $WEB_FETCH URL | $WEB_PLAIN --base URL | web_pager
 *
 * Env:
 *   EDITOR           editor for the 'e' command (falls back to VISUAL, vi)
 *   WEB_FETCH        fetch command used by 'g N' (default: "wget -qO-")
 *   WEB_PLAIN        converter used by 'g N' (default: "web_plain")
 *   LINES            terminal height (default 24)
 *   TMPDIR           temp dir for 'e' (default /tmp)
 *
 * Pure ISO C, no POSIX headers. Compile with -DNO_TIME to drop <time.h>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NO_TIME
#include <time.h>
#endif

#define VERSION "2.0"

typedef struct { char *s; size_t len, cap; } Buf;

static Buf in;
static char **links;
static int nlinks, caplinks;
static char **lines;
static long nlin, cur;
static int page = 22;
static const char *self = "web_pager";

/* ---------- small helpers ---------- */

static int is_dg(int c) { return c >= '0' && c <= '9'; }
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n);
	if (!p) {
		fputs("web_pager: out of memory\n", stderr);
		exit(1);
	}
	return p;
}

static void bmem(Buf *b, const char *m, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		while (b->len + n + 1 > b->cap)
			b->cap = b->cap ? b->cap * 2 : 4096;
		b->s = xrealloc(b->s, b->cap);
	}
	memcpy(b->s + b->len, m, n);
	b->len += n;
	b->s[b->len] = 0;
}

static void bc(Buf *b, int c)
{
	char ch = (char)c;
	bmem(b, &ch, 1);
}

static void bs(Buf *b, const char *s)
{
	bmem(b, s, strlen(s));
}

static int ci_ncmp(const char *a, const char *b, size_t n)
{
	int x, y;
	for (; n; n--, a++, b++) {
		x = lc((unsigned char)*a);
		y = lc((unsigned char)*b);
		if (x != y)
			return x - y;
		if (!x)
			break;
	}
	return 0;
}

static const char *find_ci(const char *h, const char *nd)
{
	size_t n = strlen(nd);
	if (!n)
		return h;
	for (; *h; h++)
		if (ci_ncmp(h, nd, n) == 0)
			return h;
	return NULL;
}

static void sh_quote(Buf *b, const char *s)
{
	bc(b, '\'');
	for (; *s; s++) {
		if (*s == '\'')
			bs(b, "'\\''");
		else
			bc(b, *s);
	}
	bc(b, '\'');
}

/* ---------- input ---------- */

/*
 * Walk the lines of a candidate reference list starting at q. Each line
 * must look like "  12. URL". With store set, the URLs are saved.
 * Returns 1 if every line is valid.
 */
static int refs_pass(const char *q, int store)
{
	const char *e, *s;
	char *u;
	size_t n;

	while (*q) {
		e = strchr(q, '\n');
		if (!e)
			e = q + strlen(q);
		s = q;
		while (*s == ' ')
			s++;
		if (!is_dg((unsigned char)*s))
			return 0;
		while (is_dg((unsigned char)*s))
			s++;
		if (s[0] != '.' || s[1] != ' ' || s + 2 >= e)
			return 0;
		if (store) {
			n = (size_t)(e - (s + 2));
			u = xrealloc(NULL, n + 1);
			memcpy(u, s + 2, n);
			u[n] = 0;
			if (nlinks == caplinks) {
				caplinks = caplinks ? caplinks * 2 : 32;
				links = xrealloc(links,
				    (size_t)caplinks * sizeof *links);
			}
			links[nlinks++] = u;
		}
		q = *e ? e + 1 : e;
	}
	return 1;
}

/* cut a trailing "References:" list off the text and load it into links */
static void split_refs(void)
{
	char *p, *last = NULL;

	for (p = in.s; (p = strstr(p, "References:\n")) != NULL; p++)
		if (p == in.s || p[-1] == '\n')
			last = p;
	if (!last || !refs_pass(last + 12, 0))
		return;
	refs_pass(last + 12, 1);
	in.len = (size_t)(last - in.s);
	while (in.len && in.s[in.len - 1] == '\n')
		in.len--;
	in.s[in.len] = 0;
}

static void split_lines(void)
{
	char *p;
	long k = 0;

	if (!in.len)
		return;
	nlin = 1;
	for (p = in.s; *p; p++)
		if (*p == '\n')
			nlin++;
	lines = xrealloc(NULL, (size_t)nlin * sizeof *lines);
	p = in.s;
	lines[k++] = p;
	for (; *p; p++)
		if (*p == '\n') {
			*p = 0;
			lines[k++] = p + 1;
		}
}

/* first non-blank line, without leading '#' marks */
static const char *get_title(void)
{
	long i;
	const char *s;

	for (i = 0; i < nlin; i++) {
		s = lines[i];
		while (*s == '#' || *s == ' ')
			s++;
		if (*s)
			return s;
	}
	return "(untitled)";
}

/* ---------- output ---------- */

static void write_text(FILE *f)
{
	long i;
	int n;

	for (i = 0; i < nlin; i++) {
		fputs(lines[i], f);
		putc('\n', f);
	}
	if (nlinks) {
		fputs("\nReferences:\n", f);
		for (n = 0; n < nlinks; n++)
			fprintf(f, "%3d. %s\n", n + 1, links[n]);
	}
}

static void show(long start)
{
	long i, end;

	if (nlin == 0) {
		puts("(empty)");
		return;
	}
	if (start < 0)
		start = 0;
	end = start + page;
	if (end > nlin)
		end = nlin;
	for (i = start; i < end; i++)
		puts(lines[i]);
	cur = start;
	printf("-- %ld-%ld of %ld --\n", start + 1, end, nlin);
}

static void follow(int n)
{
	const char *f, *pl;
	Buf c;

	if (n < 1 || n > nlinks) {
		puts("No such link.");
		return;
	}
	f = getenv("WEB_FETCH");
	if (!f || !*f)
		f = "wget -qO-";
	pl = getenv("WEB_PLAIN");
	if (!pl || !*pl)
		pl = "web_plain";
	c.s = NULL;
	c.len = c.cap = 0;
	bs(&c, f);
	bc(&c, ' ');
	sh_quote(&c, links[n - 1]);
	bs(&c, " | ");
	bs(&c, pl);
	bs(&c, " --base ");
	sh_quote(&c, links[n - 1]);
	bs(&c, " | ");
	sh_quote(&c, self);
	if (system(c.s) == -1)
		perror("web_pager: system");
	free(c.s);
}

static void edit(void)
{
	char path[512], num[32];
	const char *td = getenv("TMPDIR"), *ed;
	FILE *f;
	Buf c;
	int k;

	if (!td || !*td || strlen(td) > 400)
		td = "/tmp";
	for (k = 0;; k++) {
#ifndef NO_TIME
		sprintf(num, "%lx-%d", (unsigned long)time(NULL), k);
#else
		sprintf(num, "%d", k);
#endif
		sprintf(path, "%s/web-%s.txt", td, num);
		f = fopen(path, "rb");
		if (!f)
			break;
		fclose(f);
	}
	f = fopen(path, "w");
	if (!f) {
		perror(path);
		return;
	}
	write_text(f);
	fclose(f);
	ed = getenv("EDITOR");
	if (!ed || !*ed)
		ed = getenv("VISUAL");
	if (!ed || !*ed)
		ed = "vi";
	c.s = NULL;
	c.len = c.cap = 0;
	bs(&c, ed);
	bc(&c, ' ');
	sh_quote(&c, path);
	bs(&c, " </dev/tty");
	if (system(c.s) == -1)
		perror("web_pager: system");
	free(c.s);
	remove(path);
}

static void help(void)
{
	puts("  <enter> n   next page          p   previous page");
	puts("  t           top                a   print everything");
	puts("  /text       search             l   list links");
	puts("  N           show URL of link N g N follow link N ($WEB_FETCH)");
	puts("  e           open text in $EDITOR");
	puts("  w file      write text to file");
	puts("  q           quit");
}

static char *arg(char *a)
{
	a++;
	while (*a == ' ' || *a == '\t')
		a++;
	return a;
}

static void interactive(void)
{
	FILE *tty = fopen("/dev/tty", "r"), *f;
	char buf[512], *a;
	static char pat[256];
	long i, j, found, start;
	int n;

	if (!tty) {
		write_text(stdout);
		return;
	}
	printf("web_pager: %s (%ld lines, %d links) - h for help\n",
	    get_title(), nlin, nlinks);
	show(0);
	for (;;) {
		fputs("& ", stdout);
		fflush(stdout);
		if (!fgets(buf, sizeof buf, tty))
			break;
		a = buf;
		while (*a == ' ' || *a == '\t')
			a++;
		a[strcspn(a, "\r\n")] = 0;

		if (is_dg((unsigned char)*a)) {
			n = atoi(a);
			if (n >= 1 && n <= nlinks)
				printf("[%d] %s\n", n, links[n - 1]);
			else
				puts("No such link.");
			continue;
		}
		switch (*a) {
		case 0:
		case 'n':
		case '+':
			if (cur + page >= nlin)
				puts("End. (p previous, q quit)");
			else
				show(cur + page);
			break;
		case 'p':
		case '-':
			if (cur <= 0)
				puts("Top.");
			else
				show(cur - page);
			break;
		case 't':
			show(0);
			break;
		case 'a':
			for (i = 0; i < nlin; i++)
				puts(lines[i]);
			cur = nlin;
			break;
		case '/':
			a = arg(a);
			if (*a) {
				strncpy(pat, a, sizeof pat - 1);
				pat[sizeof pat - 1] = 0;
			}
			if (!*pat) {
				puts("No pattern.");
				break;
			}
			if (nlin == 0)
				break;
			start = cur + page;
			found = -1;
			for (i = 0; i < nlin; i++) {
				j = (start + i) % nlin;
				if (find_ci(lines[j], pat)) {
					found = j;
					break;
				}
			}
			if (found < 0)
				puts("Not found.");
			else
				show(found);
			break;
		case 'l':
			if (!nlinks)
				puts("No links.");
			for (n = 0; n < nlinks; n++)
				printf("%3d. %s\n", n + 1, links[n]);
			break;
		case 'g':
			follow(atoi(arg(a)));
			break;
		case 'e':
		case 'v':
			edit();
			break;
		case 'w':
			a = arg(a);
			if (!*a) {
				puts("usage: w file");
				break;
			}
			f = fopen(a, "w");
			if (!f) {
				perror(a);
				break;
			}
			write_text(f);
			fclose(f);
			printf("wrote %s\n", a);
			break;
		case 'h':
		case '?':
			help();
			break;
		case 'q':
		case 'x':
			fclose(tty);
			return;
		default:
			puts("? (h for help)");
		}
	}
	fclose(tty);
}

/* ---------- main ---------- */

static void usage(void)
{
	fputs("usage: wget -qO- URL | web_plain | web_pager\n", stderr);
}

int main(int argc, char **argv)
{
	char tmp[4096];
	const char *e;
	size_t n;
	int i;

	self = argv[0];
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
			puts("web_pager " VERSION);
			return 0;
		}
		usage();
		return 1;
	}
	while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0)
		bmem(&in, tmp, n);
	if (!in.len) {
		fputs("web_pager: no input\n", stderr);
		return 1;
	}

	e = getenv("LINES");
	if (e && atoi(e) > 2)
		page = atoi(e) - 2;
	else
		page = 22;
	if (page < 5)
		page = 5;

	split_refs();
	split_lines();
	interactive();
	return 0;
}

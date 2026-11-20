/*
 * web - read HTML on stdin, show it as plain text. mailx-style pager.
 *
 *   cc -std=c89 -pedantic -Wall -O2 -o web web.c
 *
 *   wget -qO- 'http://example.com' | web
 *   wget -qO- 'http://example.com' | web --archive
 *   curl -fsSL 'https://example.com' | web
 *
 * No networking in here. Links are followed by piping through $WEB_FETCH
 * (default: "wget -qO-") into another web.
 *
 * Env:
 *   EDITOR           editor for the 'e' command (falls back to VISUAL, vi)
 *   WEB_ARCHIVE_DIR  where --archive writes (default: current dir)
 *   WEB_FETCH        fetch command used by 'g N'
 *   WEB_WIDTH        text width (default: min(terminal, 80))
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

typedef struct { char *s; size_t len, cap; } Buf;

static Buf out, title, base, rawbuf;
static char **links;
static int nlinks, caplinks;
static int trail = 2, pend, in_pre, cur_link;
static size_t link_mark;
static char **lines;
static long nlin, cur;
static int page = 22, width = 80;
static const char *self = "web";

/* ---------- small helpers ---------- */

static void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n);
	if (!p) {
		fputs("web: out of memory\n", stderr);
		exit(1);
	}
	return p;
}

static char *xstrdup(const char *s)
{
	char *p = xrealloc(NULL, strlen(s) + 1);
	strcpy(p, s);
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
		x = tolower((unsigned char)*a);
		y = tolower((unsigned char)*b);
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

/* ---------- entities ---------- */

static const struct { const char *n; const char *u; } ents[] = {
	{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""},
	{"apos", "'"}, {"nbsp", " "},
	{"rsquo", "\xe2\x80\x99"}, {"lsquo", "\xe2\x80\x98"},
	{"ldquo", "\xe2\x80\x9c"}, {"rdquo", "\xe2\x80\x9d"},
	{"mdash", "\xe2\x80\x94"}, {"ndash", "\xe2\x80\x93"},
	{"hellip", "\xe2\x80\xa6"}, {"middot", "\xc2\xb7"},
	{"copy", "\xc2\xa9"}, {"laquo", "\xc2\xab"}, {"raquo", "\xc2\xbb"},
	{"bull", "\xe2\x80\xa2"}, {"deg", "\xc2\xb0"}, {"times", "\xc3\x97"},
	{NULL, NULL}
};

static int utf8(unsigned long c, char *o)
{
	if (c < 0x80) {
		o[0] = (char)c;
		return 1;
	}
	if (c < 0x800) {
		o[0] = (char)(0xC0 | (c >> 6));
		o[1] = (char)(0x80 | (c & 0x3F));
		return 2;
	}
	if (c < 0x10000) {
		o[0] = (char)(0xE0 | (c >> 12));
		o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
		o[2] = (char)(0x80 | (c & 0x3F));
		return 3;
	}
	o[0] = (char)(0xF0 | (c >> 18));
	o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
	o[2] = (char)(0x80 | ((c >> 6) & 0x3F));
	o[3] = (char)(0x80 | (c & 0x3F));
	return 4;
}

/* p points at '&'. returns bytes consumed, 0 if not an entity. o >= 8 bytes */
static int entity(const char *p, char *o)
{
	const char *q = p + 1;
	char *end;
	unsigned long v;
	size_t n;
	int i;

	if (*q == '#') {
		q++;
		if (*q == 'x' || *q == 'X') {
			q++;
			v = strtoul(q, &end, 16);
		} else {
			v = strtoul(q, &end, 10);
		}
		if (end == q)
			return 0;
		if (*end == ';')
			end++;
		if (v == 0 || v > 0x10FFFF)
			return 0;
		if (v == 160)
			v = ' ';
		o[utf8(v, o)] = 0;
		return (int)(end - p);
	}
	for (i = 0; ents[i].n; i++) {
		n = strlen(ents[i].n);
		if (strncmp(q, ents[i].n, n) == 0 && q[n] == ';') {
			strcpy(o, ents[i].u);
			return (int)n + 2;
		}
	}
	return 0;
}

static void decode_range(Buf *b, const char *s, const char *e)
{
	char o[8];
	int n;

	while (s < e) {
		if (*s == '&') {
			n = entity(s, o);
			if (n && s + n <= e) {
				bs(b, o);
				s += n;
				continue;
			}
		}
		bc(b, *s++);
	}
}

/* ---------- output building ---------- */

static void emit(int c)
{
	bc(&out, c);
	trail = (c == '\n') ? trail + 1 : 0;
}

static void brk(int n)
{
	while (trail < n)
		emit('\n');
	pend = 0;
}

static void text(int c)
{
	if (in_pre) {
		if (c != '\r')
			emit(c);
		return;
	}
	if (isspace((unsigned char)c)) {
		pend = 1;
		return;
	}
	if (pend && trail == 0)
		emit(' ');
	pend = 0;
	emit(c);
}

static void texts(const char *s)
{
	for (; *s; s++)
		text(*s);
}

/* ---------- URLs ---------- */
static void dedot(char *u)
{
	char *a, *p, *r, *w, *s, *e, *tail;
	size_t len;

	a = strstr(u, "://");
	if (!a || !(p = strchr(a + 3, '/')))
		return;
	tail = xstrdup(p + strcspn(p, "?#"));
	p[strcspn(p, "?#")] = 0;
	r = w = p;
	while (*r) {
		s = r + 1;
		e = s + strcspn(s, "/");
		len = (size_t)(e - s);
		if (len == 1 && s[0] == '.') {
			if (!*e)
				*w++ = '/';
		} else if (len == 2 && s[0] == '.' && s[1] == '.') {
			if (w > p) {
				w--;
				while (w > p && *w != '/')
					w--;
			}
			if (!*e)
				*w++ = '/';
		} else {
			memmove(w, r, len + 1);
			w += len + 1;
		}
		r = e;
	}
	strcpy(w, tail);
	free(tail);
}

static char *resolve(const char *href)
{
	Buf r;
	const char *b = base.len ? base.s : "";
	const char *auth, *ps, *be, *ls, *q;
	size_t i;

	while (isspace((unsigned char)*href))
		href++;
	for (i = 0; isalnum((unsigned char)href[i]) || href[i] == '+' ||
	    href[i] == '-' || href[i] == '.'; i++)
		;
	if ((i > 0 && href[i] == ':') || !*b)
		return xstrdup(href);
	auth = strstr(b, "://");
	if (!auth)
		return xstrdup(href);
	auth += 3;
	ps = strchr(auth, '/');
	if (!ps)
		ps = b + strlen(b);

	r.s = NULL;
	r.len = r.cap = 0;
	if (href[0] == '/' && href[1] == '/') {
		bmem(&r, b, (size_t)(auth - 2 - b));
	} else if (href[0] == '/') {
		bmem(&r, b, (size_t)(ps - b));
	} else {
		be = b + strcspn(b, "?#");
		ls = NULL;
		for (q = ps; q < be; q++)
			if (*q == '/')
				ls = q;
		if (ls) {
			bmem(&r, b, (size_t)(ls + 1 - b));
		} else {
			bmem(&r, b, (size_t)(ps - b));
			bc(&r, '/');
		}
	}
	bs(&r, href);
	dedot(r.s);
	return r.s;
}

static int addlink(char *u)
{
	if (nlinks == caplinks) {
		caplinks = caplinks ? caplinks * 2 : 32;
		links = xrealloc(links, (size_t)caplinks * sizeof *links);
	}
	links[nlinks++] = u;
	return nlinks;
}

/* ---------- HTML parsing ---------- */

static int attr(const char *s, const char *e, const char *want, Buf *v)
{
	const char *n, *vs, *ve;
	size_t nl, wl = strlen(want);
	char q;

	while (s < e && !isspace((unsigned char)*s) && *s != '/')
		s++;
	while (s < e) {
		while (s < e && (isspace((unsigned char)*s) || *s == '/'))
			s++;
		n = s;
		while (s < e && !isspace((unsigned char)*s) && *s != '=' &&
		    *s != '/')
			s++;
		if (s == n) {
			s++;
			continue;
		}
		nl = (size_t)(s - n);
		while (s < e && isspace((unsigned char)*s))
			s++;
		vs = ve = NULL;
		if (s < e && *s == '=') {
			s++;
			while (s < e && isspace((unsigned char)*s))
				s++;
			if (s < e && (*s == '"' || *s == '\'')) {
				q = *s++;
				vs = s;
				while (s < e && *s != q)
					s++;
				ve = s;
				if (s < e)
					s++;
			} else {
				vs = s;
				while (s < e && !isspace((unsigned char)*s))
					s++;
				ve = s;
			}
		}
		if (nl == wl && ci_ncmp(n, want, nl) == 0) {
			if (vs)
				decode_range(v, vs, ve);
			return 1;
		}
	}
	return 0;
}

static const char *skip_to(const char *from, const char *nm)
{
	char ct[32];
	const char *c, *g;

	sprintf(ct, "</%s", nm);
	c = find_ci(from, ct);
	if (!c)
		return from + strlen(from);
	g = strchr(c, '>');
	return g ? g + 1 : c + strlen(c);
}

static int one_of(const char *nm, const char *list)
{
	char t[24];
	sprintf(t, " %s ", nm);
	return strstr(list, t) != NULL;
}

static void set_title(const char *s, const char *e)
{
	Buf tmp;
	size_t i;
	int sp = 0;

	tmp.s = NULL;
	tmp.len = tmp.cap = 0;
	bs(&tmp, "");
	decode_range(&tmp, s, e);
	title.len = 0;
	for (i = 0; i < tmp.len; i++) {
		if (isspace((unsigned char)tmp.s[i])) {
			sp = 1;
			continue;
		}
		if (sp && title.len)
			bc(&title, ' ');
		sp = 0;
		bc(&title, tmp.s[i]);
	}
	free(tmp.s);
}

static const char *tag(const char *p)
{
	char nm[16], t[16];
	const char *e, *q, *after, *c, *g;
	int closing = 0, n = 0, qt = 0, lv;
	Buf v;

	q = p + 1;
	if (*q == '/') {
		closing = 1;
		q++;
	}
	while (isalnum((unsigned char)*q) && n < 15)
		nm[n++] = (char)tolower((unsigned char)*q++);
	nm[n] = 0;
	for (e = p + 1; *e; e++) {
		if (qt) {
			if (*e == qt)
				qt = 0;
		} else if (*e == '"' || *e == '\'') {
			qt = *e;
		} else if (*e == '>') {
			break;
		}
	}
	after = *e ? e + 1 : e;

	if (one_of(nm, " script style noscript svg template ")) {
		if (closing)
			return after;
		return skip_to(after, nm);
	}
	if (!strcmp(nm, "title")) {
		if (closing)
			return after;
		c = find_ci(after, "</title");
		if (!c)
			c = after + strlen(after);
		set_title(after, c);
		g = strchr(c, '>');
		return g ? g + 1 : c + strlen(c);
	}
	if (nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6' && !nm[2]) {
		brk(2);
		if (!closing) {
			for (lv = nm[1] - '0'; lv > 0; lv--)
				emit('#');
			emit(' ');
		}
		return after;
	}
	if (one_of(nm, " p ul ol dl table blockquote figure article section "
	    "header footer main nav aside form address details fieldset "
	    "center ")) {
		brk(2);
		return after;
	}
	if (one_of(nm, " div tr dt dd figcaption summary caption thead "
	    "tbody tfoot ")) {
		brk(1);
		return after;
	}
	if (!strcmp(nm, "td") || !strcmp(nm, "th")) {
		pend = 1;
		return after;
	}
	if (!strcmp(nm, "li")) {
		brk(1);
		if (!closing) {
			emit('*');
			emit(' ');
		}
		return after;
	}
	if (!strcmp(nm, "br")) {
		brk(trail < 2 ? trail + 1 : 2);
		return after;
	}
	if (!strcmp(nm, "hr")) {
		brk(2);
		for (lv = 0; lv < 20; lv++)
			emit('-');
		brk(2);
		return after;
	}
	if (!strcmp(nm, "pre")) {
		brk(2);
		if (closing) {
			in_pre = 0;
		} else {
			in_pre = 1;
			if (*after == '\n')
				after++;
		}
		return after;
	}
	if (!strcmp(nm, "a")) {
		if (closing) {
			if (cur_link) {
				if (out.len > link_mark) {
					lv = 0;
					while (out.len && out.s[out.len - 1] == '\n') {
						out.s[--out.len] = 0;
						lv++;
					}
					trail = 0;
					sprintf(t, "[%d]", cur_link);
					for (q = t; *q; q++)
						emit(*q);
					if (lv)
						brk(lv);
				} else {
					free(links[--nlinks]);
				}
				cur_link = 0;
			}
			return after;
		}
		v.s = NULL;
		v.len = v.cap = 0;
		cur_link = 0;
		if (attr(p + 1, e, "href", &v) && v.len && v.s[0] != '#' &&
		    ci_ncmp(v.s, "javascript:", 11) != 0) {
			cur_link = addlink(resolve(v.s));
			link_mark = out.len;
		}
		free(v.s);
		return after;
	}
	if (!strcmp(nm, "img") && !closing) {
		v.s = NULL;
		v.len = v.cap = 0;
		if (attr(p + 1, e, "alt", &v) && v.len && !strchr(v.s, '/')) {
			texts("[img: ");
			texts(v.s);
			texts("]");
		}
		free(v.s);
		return after;
	}
	if (!strcmp(nm, "base") || !strcmp(nm, "link")) {
		Buf r;
		r.s = NULL;
		r.len = r.cap = 0;
		v.s = NULL;
		v.len = v.cap = 0;
		if (!strcmp(nm, "base")) {
			if (attr(p + 1, e, "href", &v) && v.len) {
				base.len = 0;
				bs(&base, v.s);
			}
		} else if (!base.len && attr(p + 1, e, "rel", &r) &&
		    find_ci(r.s ? r.s : "", "canonical") &&
		    attr(p + 1, e, "href", &v) && v.len) {
			bs(&base, v.s);
		}
		free(r.s);
		free(v.s);
		return after;
	}
	return after;
}

static void parse(const char *h)
{
	const char *p = h, *e;
	char o[8];
	int n;

	while (*p) {
		if (*p == '<') {
			if (!strncmp(p, "<!--", 4)) {
				e = strstr(p + 4, "-->");
				p = e ? e + 3 : p + strlen(p);
				continue;
			}
			if (isalpha((unsigned char)p[1]) || p[1] == '/') {
				p = tag(p);
				continue;
			}
			if (p[1] == '!' || p[1] == '?') {
				e = strchr(p, '>');
				p = e ? e + 1 : p + strlen(p);
				continue;
			}
		} else if (*p == '&') {
			n = entity(p, o);
			if (n) {
				texts(o);
				p += n;
				continue;
			}
		}
		text(*p++);
	}
}

static void render(const char *html)
{
	bs(&out, "");
	trail = 2;
	parse(html);
	while (out.len && (out.s[out.len - 1] == '\n' ||
	    out.s[out.len - 1] == ' '))
		out.s[--out.len] = 0;
}

/* ---------- wrapping ---------- */

static void wrap(void)
{
	Buf w;
	const char *s, *e, *t, *i, *ls;
	char *p;
	int col, blank = 0;
	long k;

	w.s = NULL;
	w.len = w.cap = 0;
	bs(&w, "");
	s = out.s;
	for (;;) {
		e = strchr(s, '\n');
		if (!e)
			e = s + strlen(s);
		t = e;
		while (t > s && (t[-1] == ' ' || t[-1] == '\t' ||
		    t[-1] == '\r'))
			t--;
		if (t == s) {
			if (blank++ == 0 && w.len)
				bc(&w, '\n');
		} else {
			blank = 0;
			for (;;) {
				i = s;
				ls = NULL;
				col = 0;
				while (i < t && col < width) {
					if (*i == ' ')
						ls = i;
					i++;
					col++;
					while (i < t &&
					    ((unsigned char)*i & 0xC0) == 0x80)
						i++;
				}
				if (i >= t)
					break;
				if (*i == ' ')
					ls = i;
				if (ls && ls > s) {
					bmem(&w, s, (size_t)(ls - s));
					bc(&w, '\n');
					s = ls + 1;
					while (s < t && *s == ' ')
						s++;
				} else {
					bmem(&w, s, (size_t)(i - s));
					bc(&w, '\n');
					s = i;
				}
			}
			bmem(&w, s, (size_t)(t - s));
			bc(&w, '\n');
		}
		if (!*e)
			break;
		s = e + 1;
	}

	nlin = 0;
	for (p = w.s; *p; p++)
		if (*p == '\n')
			nlin++;
	lines = xrealloc(lines, (size_t)(nlin + 1) * sizeof *lines);
	k = 0;
	p = w.s;
	while (k < nlin) {
		lines[k++] = p;
		p = strchr(p, '\n');
		*p++ = 0;
	}
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

static int archive(void)
{
	const char *dir = getenv("WEB_ARCHIVE_DIR");
	char stamp[32], num[16];
	time_t t = time(NULL);
	struct tm *tm;
	Buf sl, p;
	FILE *f;
	size_t i;
	int k;

	if (!dir || !*dir)
		dir = ".";
	else
		mkdir(dir, 0777);
	tm = localtime(&t);
	if (!tm || !strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tm))
		strcpy(stamp, "archive");

	sl.s = NULL;
	sl.len = sl.cap = 0;
	bs(&sl, "");
	for (i = 0; i < title.len && sl.len < 48; i++) {
		unsigned char c = (unsigned char)title.s[i];
		if (c < 128 && isalnum(c))
			bc(&sl, tolower(c));
		else if (sl.len && sl.s[sl.len - 1] != '-')
			bc(&sl, '-');
	}
	while (sl.len && sl.s[sl.len - 1] == '-')
		sl.s[--sl.len] = 0;

	p.s = NULL;
	p.len = p.cap = 0;
	for (k = 0;; k++) {
		p.len = 0;
		bs(&p, dir);
		bc(&p, '/');
		bs(&p, stamp);
		if (sl.len) {
			bc(&p, '-');
			bs(&p, sl.s);
		}
		if (k) {
			sprintf(num, "-%d", k);
			bs(&p, num);
		}
		bs(&p, ".html");
		f = fopen(p.s, "rb");
		if (!f)
			break;
		fclose(f);
	}
	f = fopen(p.s, "wb");
	if (!f) {
		perror(p.s);
		return 1;
	}
	if (fwrite(rawbuf.s, 1, rawbuf.len, f) != rawbuf.len ||
	    fclose(f) != 0) {
		perror(p.s);
		return 1;
	}
	puts(p.s);
	return 0;
}

static void termsize(int *cols, int *rows)
{
	struct winsize ws;
	const char *e;

	*cols = 80;
	*rows = 24;
	if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
		*cols = ws.ws_col;
		if (ws.ws_row > 0)
			*rows = ws.ws_row;
		return;
	}
	e = getenv("COLUMNS");
	if (e && atoi(e) > 0)
		*cols = atoi(e);
	e = getenv("LINES");
	if (e && atoi(e) > 0)
		*rows = atoi(e);
}

/* ---------- interactive ---------- */

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
	const char *f;
	Buf c;

	if (n < 1 || n > nlinks) {
		puts("No such link.");
		return;
	}
	f = getenv("WEB_FETCH");
	if (!f || !*f)
		f = "wget -qO-";
	c.s = NULL;
	c.len = c.cap = 0;
	bs(&c, f);
	bc(&c, ' ');
	sh_quote(&c, links[n - 1]);
	bs(&c, " | ");
	sh_quote(&c, self);
	bs(&c, " --base ");
	sh_quote(&c, links[n - 1]);
	if (system(c.s) == -1)
		perror("web: system");
	free(c.s);
}

static void edit(void)
{
	char path[512];
	const char *td = getenv("TMPDIR"), *ed;
	FILE *f;
	Buf c;
	int fd;

	if (!td || !*td || strlen(td) > 400)
		td = "/tmp";
	sprintf(path, "%s/web.XXXXXX", td);
	fd = mkstemp(path);
	if (fd < 0) {
		perror("web: mkstemp");
		return;
	}
	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(path);
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
		perror("web: system");
	free(c.s);
	unlink(path);
}

static void help(void)
{
	puts("  <enter> n   next page          p   previous page");
	puts("  t           top                a   print everything");
	puts("  /text       search             l   list links");
	puts("  N           show URL of link N g N follow link N ($WEB_FETCH)");
	puts("  e           open text in $EDITOR");
	puts("  w file      write text to file");
	puts("  s           archive raw HTML ($WEB_ARCHIVE_DIR or .)");
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
	printf("web: %s (%ld lines, %d links) - h for help\n",
	    title.len ? title.s : "(untitled)", nlin, nlinks);
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

		if (isdigit((unsigned char)*a)) {
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
		case 's':
			archive();
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
		fputs("usage: wget -qO- URL | web [--archive] [--dump]\n"
      		"       curl -fsSL URL | web [--archive] [--dump]\n", stderr);
}

int main(int argc, char **argv)
{
	int i, do_archive = 0, do_dump = 0, cols, rows;
	char tmp[4096];
	const char *e;
	size_t n;

	self = argv[0];
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--archive") || !strcmp(argv[i], "-a"))
			do_archive = 1;
		else if (!strcmp(argv[i], "--dump") || !strcmp(argv[i], "-d"))
			do_dump = 1;
		else if (!strcmp(argv[i], "--base") && i + 1 < argc)
			bs(&base, argv[++i]);
		else {
			usage();
			return 1;
		}
	}
	if (isatty(0)) {
		usage();
		return 1;
	}
	while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0)
		bmem(&rawbuf, tmp, n);
	if (!rawbuf.len) {
		fputs("web: no input\n", stderr);
		return 1;
	}

	render(rawbuf.s);
	if (do_archive)
		return archive();

	termsize(&cols, &rows);
	width = cols > 80 ? 80 : cols;
	e = getenv("WEB_WIDTH");
	if (e && atoi(e) > 10)
		width = atoi(e);
	if (width < 20)
		width = 20;
	page = rows - 2;
	if (page < 5)
		page = 5;
	wrap();

	if (do_dump || !isatty(1))
		write_text(stdout);
	else
		interactive();
	return 0;
}

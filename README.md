# web

A small terminal HTML reader in one C89 file. It reads HTML on standard input, turns it into plain text, and shows it in a mailx-style pager. It does no networking: pages are fetched by `wget`/`curl` and piped in.

## Build

```sh
cc -std=c89 -pedantic -Wall -Wextra -O2 -o web web.c
install -m 755 web /usr/local/bin/web
```

Optional: add `-DNO_TIME` to drop `<time.h>` entirely. Archive files are then named `web-title-slug.html` (no timestamp, `-1`, `-2`, ... on collisions) and the `e` temp file uses a counter instead of the clock.

```sh
cc -std=c89 -pedantic -Wall -Wextra -O2 -DNO_TIME -o web web.c
```

`web` must be on `$PATH` (or run by full path) for `g N` to work, because it re-runs itself to open links.

## Usage

```sh
wget -qO- 'http://example.com' | web          # interactive pager
curl -fsSL 'http://example.com' | web -d      # plain text to stdout
curl -fsSL 'http://example.com' | web -d | less
wget -qO- 'http://example.com' | web --archive
```

Interactive mode is the default. When stdout is not a terminal (pipe or file), pass `-d`, otherwise the pager will still try to start.

### Options

| Option | Meaning |
|---|---|
| `-d`, `--dump` | Print text plus the reference list and exit. |
| `-a`, `--archive` | Save the raw HTML to a file, print its path, do not display. |
| `--base URL` | Base URL for resolving relative links. Used internally by `g N`. |

### Pager commands

Prompt is `& `.

| Command | Action |
|---|---|
| `Enter`, `n`, `+` | Next page |
| `p`, `-` | Previous page |
| `t` | Top |
| `a` | Print everything |
| `/text` | Search (case-insensitive, starts after current page, wraps). `/` alone repeats the last search. |
| `l` | List all links |
| `N` | Show URL of link N |
| `g N` | Follow link N: runs `$WEB_FETCH 'URL' \| web --base 'URL'` |
| `e`, `v` | Open the text in your editor |
| `w file` | Write text and references to `file` |
| `s` | Archive the raw HTML |
| `h`, `?` | Help |
| `q`, `x` | Quit |

## Environment

| Variable | Default | Purpose |
|---|---|---|
| `WEB_FETCH` | `wget -qO-` | Command used by `g N` to fetch a link. |
| `WEB_WIDTH` | `min(COLUMNS, 80)` | Text width (minimum 20). |
| `WEB_ARCHIVE_DIR` | current directory | Where archives go. Must already exist. |
| `EDITOR` | `VISUAL`, then `vi` | Editor for `e`. |
| `COLUMNS`, `LINES` | 80, 24 | Terminal size. Shells often do not export these, so `export COLUMNS LINES` if the page height or width is wrong. |
| `TMPDIR` | `/tmp` | Temp file location for `e`. |

Archive files are named `YYYYMMDD-HHMMSS-title-slug.html`, with `-1`, `-2`, ... added if the name exists.

## Requirements

Build:
- A C89 compiler (`cc`, `gcc`, `clang`). Only ISO C headers are used: `stdio.h`, `stdlib.h`, `string.h`, and `time.h` (not needed with `-DNO_TIME`). No POSIX headers.

Run:
- A POSIX-style shell for `system()`, used by `g N` and `e`.
- `/dev/tty` for the interactive pager. Without it, `web` falls back to plain text output.
- `wget` (or whatever `WEB_FETCH` points to, e.g. `curl -fsSL`) only for following links.

## How it works

Everything is in `web.c`, in this order:

1. **Buffers and helpers.** A growable `Buf` type, allocation wrappers, ASCII character tests, case-insensitive search, shell quoting.
2. **Entities.** Decodes named entities (a table of common ones) and numeric `&#N;` / `&#xN;` to UTF-8.
3. **Output building.** `emit`, `brk` and `text` collapse whitespace, keep track of blank lines, and pass `<pre>` content through unchanged.
4. **URLs.** `resolve` turns relative links into absolute ones using `--base`, `<base href>` or `<link rel=canonical>`; `dedot` removes `.` and `..` path segments.
5. **HTML parsing.** `tag` handles each tag, `parse` walks the input. No DOM is built; the text is produced in a single pass.
6. **Dedupe.** A post-pass over the generated text.
7. **Wrapping.** Breaks lines at spaces to the chosen width, counting UTF-8 characters (not display width).
8. **Output.** `--dump`, `--archive`, and the interactive pager.

### What the parser does

- Headings become `#`..`######` lines, list items `* item`, `<hr>` a dashed line, table cells are joined with ` | `.
- Links get numbered `[N]` markers; URLs are listed under `References:`. The same URL always gets the same number. `#...` and `javascript:` links are ignored.
- Images with a useful alt text become `[img: alt]`. If an image is inside a link, the alt is only shown when the link has no other text. Alt texts that look like file paths are ignored.
- Skipped entirely: `script`, `style`, `noscript`, `svg`, `template`, `button`, `select`, `textarea`, `iframe`, `canvas`, `object`.
- Hidden elements are skipped with their content: `hidden`, `aria-hidden="true"`, inline `display:none` / `visibility:hidden`, and Bootstrap `d-none` / `d-lg-none` (unless a responsive `d-*-inline` etc. class re-shows it). This removes the duplicate mobile/desktop copies many sites ship.
- Duplicate lines are removed: a line identical to the previous non-blank line is dropped (`Advertising` / `Advertising `). `[img: X]` counts as equal to `X`. Lines inside `<pre>` are never touched. Empty `*` list items are removed.

## Limits

- No JavaScript, CSS layout, forms, frames or images. Only the inline hiding rules above are understood.
- Input is assumed to be UTF-8. No charset conversion.
- The whole page is read into memory.
- Deduplication only compares consecutive lines, so text repeated far apart (header and footer banners) stays.
- If two different links have identical consecutive text, the second line is dropped along with its link number.
- The temp file for `e` has a predictable name in `$TMPDIR`; do not use `e` on a shared multi-user `/tmp` if that matters to you.

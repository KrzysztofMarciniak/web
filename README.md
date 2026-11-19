# web

A small, lightweight terminal-based HTML reader written in C89. Reads HTML from standard input, converts it into readable plain text, and provides interactive navigation through the terminal.

### compilation:

```sh
cc -std=c89 -pedantic -Wall -O2 -o web web.c
```

### installation:

```sh
install -m 755 web /usr/local/bin/web
```

### usage:

```sh
wget -qO- 'http://example.com' | web
curl -fsSL 'http://example.com' | web
```

### navigation:

- `Enter` / `n` / `+` — next page
- `p` / `-` — previous page
- `t` — return to top
- `a` — display all text
- `/text` — search for text
- `l` — list links
- `N` — display URL of link N (number)
- `g N` — follow link N (number)
- `e` / `v` — open text in editor
- `w file` — write text to file
- `s` — archive original HTML
- `h` / `?` — show help
- `q` / `x` — quit

### options:

- `--dump`, `-d` — output text without interactive navigation
- `--archive`, `-a` — save original HTML instead of displaying it

### environment:

- `EDITOR` — editor used by the `e` command
- `VISUAL` — fallback editor
- `WEB_ARCHIVE_DIR` — directory for archived HTML (default: current directory)
- `WEB_FETCH` — command used to fetch followed links (default: `wget -qO-`)
- `WEB_WIDTH` — output text width (default: terminal width, maximum 80)

### dependencies:

- A C89-compatible compiler
- POSIX environment
- `wget` or `curl` for fetching pages

### notes:

- HTML is read from standard input.
- Networking is handled externally; `web` itself does not perform network requests.
- Links are displayed as numbered references.
- HTML is converted to plain text, with basic formatting preserved.
- Original HTML can be archived without conversion using `--archive`.

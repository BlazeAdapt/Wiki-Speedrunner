# wiki-speedrun

Build a queryable "shortest hop path between two Wikipedia articles" graph
from the raw enwiki SQL dumps, in C++, no MySQL required.

## Pipeline

```
enwiki-latest-page.sql.gz -----------------\
enwiki-latest-redirect.sql.gz (optional) ---+--> pages.bin / titles.blob
enwiki-latest-linktarget.sql.gz ------------+--> lt_to_dense.bin
enwiki-latest-pagelinks.sql.gz -------------+--> edges.bin
                                             \--> fwd/bwd CSR --> search
```

1. **build_pages** — parses `page.sql.gz`, keeps namespace-0 (article)
   pages, assigns each a dense id (0..N-1), writes `pages.bin` +
   `titles.blob` + sorted lookup indexes.
2. **build_redirects** *(optional but recommended)* — parses
   `redirect.sql.gz`, fills in each redirect page's `redirect_to` dense id
   so redirect chains collapse to their real target. If you skip this
   stage, links through redirect pages are simply dropped instead of
   collapsed — the graph still works, just with fewer edges.
3. **build_linktargets** — parses `linktarget.sql.gz`, resolves every
   `lt_id` to a dense article id (needed because post-2021 `pagelinks`
   dumps reference link targets indirectly through this table).
4. **build_pagelinks** — parses `pagelinks.sql.gz`, emits `edges.bin`: raw
   `(from_dense, to_dense)` pairs, with redirect collapsing and
   namespace/self-loop filtering already applied.
5. **build_csr** — turns `edges.bin` into forward and backward
   compressed-sparse-row adjacency arrays (`fwd_*`, `bwd_*`).
6. **search** — loads the graph and answers queries with classic
   bidirectional BFS (optimal shortest-hop path, since both sides expand
   depth-by-depth in lockstep).

## Build

```sh
sudo apt install zlib1g-dev   # if not already present
make
```

Produces `bin/build_pages`, `bin/build_redirects`, `bin/build_linktargets`,
`bin/build_pagelinks`, `bin/build_csr`, `bin/search`.

## Run

```sh
mkdir -p graph
./bin/build_pages        enwiki-latest-page.sql.gz        graph/
./bin/build_redirects     enwiki-latest-redirect.sql.gz     graph/   # optional
./bin/build_linktargets   enwiki-latest-linktarget.sql.gz   graph/
./bin/build_pagelinks     enwiki-latest-pagelinks.sql.gz    graph/
./bin/build_csr           graph/

./bin/search graph/                       # interactive
./bin/search graph/ "Cat" "Philosophy"    # one-shot
```

`enwiki-latest-redirect.sql.gz` isn't in your original download list — grab
it from the same dump directory (`https://dumps.wikimedia.org/enwiki/latest/`)
if you want redirects collapsed properly.

## Verifying the dump's column order

MediaWiki's schema has changed over the years. Before running, sanity check
the columns actually match what the code assumes:

```sh
zcat enwiki-latest-page.sql.gz | grep -m1 'CREATE TABLE'
```

The code assumes:
- `page`: `page_id, page_namespace, page_title, page_is_redirect, ...`
- `redirect`: `rd_from, rd_namespace, rd_title, rd_interwiki, rd_fragment`
- `linktarget`: `lt_id, lt_namespace, lt_title`
- `pagelinks` (post-2021 schema): `pl_from, pl_from_namespace, pl_target_id`

If your dump predates the 2021 pagelinks migration (columns
`pl_from, pl_namespace, pl_title, pl_from_namespace` with direct titles
instead of `pl_target_id`), `build_linktargets`/`build_pagelinks` need
small edits — ping me and I'll adjust them.

## Design notes / why it's fast

- **No MySQL import.** `include/sql_parser.hpp` is a small streaming
  tokenizer that reads gzip directly and pulls out `INSERT ... VALUES`
  tuples without building a full SQL AST — much faster than round-tripping
  through a database.
- **Dense integer ids everywhere.** Every article gets a compact `0..N-1`
  id (~6.9M for enwiki ns0), so the graph fits in flat `uint32_t` arrays
  instead of hash maps of strings.
- **CSR (compressed sparse row) adjacency**, built in both directions, so
  neither forward nor backward BFS ever has to scan an edge list — direct
  index into a `targets` array via `offsets[node]..offsets[node+1]`.
- **Bidirectional BFS.** Expanding from both start and goal in lockstep
  turns `O(branching^depth)` into roughly `O(branching^(depth/2))`, which
  is the difference between milliseconds and not finishing at Wikipedia's
  branching factor.
- **Redirects collapsed at build time**, not query time, so search doesn't
  need special-casing.

## Rough scale (full enwiki)

| stage | approx size |
|---|---|
| ns0 pages | ~6.9M |
| ns0→ns0 pagelinks edges | ~150–300M |
| `pages.bin` + `titles.blob` | a few hundred MB |
| CSR `targets` arrays (both directions) | ~1.2–2.4 GB combined |
| peak build RAM | a few GB |

A full build (all 5 stages) on a decent machine should take somewhere in
the tens-of-minutes range, dominated by decompression + parsing of
`pagelinks.sql.gz` (by far the largest file). `search` itself, once the
graph is loaded, answers most queries in low milliseconds.

## Tested

`test/` (not included in the tarball, but reproducible) contains a
hand-built synthetic dump exercising: multi-tuple/multi-statement INSERTs,
backslash-escaped strings, NULL fields, non-ns0 filtering, redirect
collapsing, and dropped self-loops/redirect-source edges — the full
pipeline runs clean against it and `search` returns correct shortest paths.

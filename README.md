# [Wiki-speedrunner](https://wiki-speedrunner.vercel.app)

Find the shortest chain of Wikipedia hyperlinks between any two articles — built from the raw enwiki dumps, no MySQL required, and fast enough to answer in milliseconds once it's running.

It's basically the engine behind that old "click through Wikipedia links to get from A to B" game, except it just tells you the answer.

## How it works

Wikipedia publishes its entire link graph as SQL dumps. This project parses those directly (no database import step), builds a compact graph out of them, and then answers "shortest path" queries with bidirectional BFS — searching from both the start and the destination at once, which is what makes queries fast even though Wikipedia's link graph is huge.

There's a small pipeline of tools that each do one step, and then either a command-line search tool or a web server that sits on top of the finished graph.

## Building the graph

You'll need three files from a [Wikipedia database dump](https://dumps.wikimedia.org/enwiki/latest/):

- `enwiki-latest-page.sql.gz`
- `enwiki-latest-linktarget.sql.gz`
- `enwiki-latest-pagelinks.sql.gz`

Build everything:

```sh
sudo apt install zlib1g-dev   # if not already installed
make
```

Then run the pipeline in order:

```sh
mkdir -p graph
./bin/build_pages        enwiki-latest-page.sql.gz        graph/
./bin/build_linktargets   enwiki-latest-linktarget.sql.gz   graph/
./bin/build_pagelinks     enwiki-latest-pagelinks.sql.gz    graph/
./bin/build_csr           graph/
```

Each step reads a dump and writes out a bit more of the finished graph into `graph/`. `build_csr` is the last one — after that, everything's ready to query.

This takes a while (mostly the `pagelinks` step, since it's the biggest file), and needs a few GB of RAM at peak. Once it's done, though, `graph/` is all you need going forward — you don't have to touch the dumps again.

## Trying it from the command line

```sh
./bin/search graph/ "Cat" "Napoleon"
```

or run it with no arguments for an interactive prompt. Good for a quick sanity check before setting up the website.

## Running it as a website

This is where it gets fun. `bin/server` loads the graph once when it starts up and then just keeps it sitting in memory — every search after that is answered straight out of RAM, no disk reads, no reloading. That's the whole trick to keeping it fast.

```sh
make server
./bin/server graph/ 8080 static/
```

Open `http://localhost:8080` and you've got a working site. `static/index.html` is a self-contained frontend — no build step, no framework, just talks to a small JSON API:

- `GET /api/search?start=Cat&goal=Dog` — returns the path, hop count, and timing
- `GET /api/suggest?q=Ca` — title autocomplete
- `GET /api/health` — quick "is it alive" check

### Putting it online

The setup I've been running: the graph and server live on a small always-on VM (an Oracle Cloud free-tier instance works well — it's genuinely free and has enough RAM for the full enwiki graph), kept running as a `systemd` service so it survives reboots and restarts itself if it ever crashes. The frontend is deployed separately to Vercel as a static site, and it just points at the VM's URL.

To expose the VM's server over HTTPS (needed since Vercel serves the frontend over HTTPS, and browsers won't let that page call a plain `http://` backend), I used [Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/):

```sh
cloudflared tunnel --url http://localhost:8080
```

It prints a public HTTPS URL that forwards straight to your local server — no port forwarding, no certificates to manage. Set that URL as the `API` constant near the top of `static/index.html`, then deploy:

```sh
cd static
vercel deploy --prod
```

And that's it — a real, working website, backed by a multi-gigabyte graph, running for free.

## A rough sense of scale

For context, the full English Wikipedia graph works out to something like:

- ~6.9 million articles
- somewhere around 150–300 million links between them
- a few GB of memory once it's all loaded

None of that is precise — it depends on which dump snapshot you use — but it gives you a sense of what kind of machine you'll want for the build step and for hosting.

## A couple of notes

- The `pages`/`linktarget`/`pagelinks` column layouts assumed here match the schema Wikipedia's been using since their ~2021 link-target migration. If you're working from a much older dump, some of the parsing logic may need small tweaks.
- `graph/` and the raw `.sql.gz` dumps aren't meant to be committed to version control — they're multi-gigabyte generated/source data, not code. Rebuild `graph/` from the steps above, or copy it directly between machines (`rsync` works well) rather than pushing it through git.

# SRT benchmark kit

`srt-bench` measures SRT's own transport stats at 10 / 20 / 40 / 50 Mbps, direct and through
`bmb-relay`. The pass bar for the remote link is **zero unrecovered drops at 40 Mbps for 10
minutes, tuned, direct and via the relay**. The winning settings become the Bridge's defaults,
and the results file is committed next to the design for the reviewer.

The tool is in `remote/`: `srt-bench` (a paced publisher or a requester) and `bmb-relay`
(pairs a publisher and its requesters by stream ID). Both build on libsrt, on Windows and on
Debian through the Docker image.

## What the matrix measures

For each of 10 / 20 / 40 / 50 Mbps:

- **default** options (libsrt buffers, latency 120 ms), and
- **tuned**: `--latency-rtt 4 --sndbuf 64MiB --rcvbuf 64MiB --fc 60000`,

over two paths:

- **direct**: the receiver runs on the VPS, this PC sends to it; and
- **via the relay**: both ends run from this PC through the relay on the VPS.

Every run logs one CSV row per second and one Markdown summary row; `run-matrix.ps1` keeps the
summary rows.

## Build

Windows (from the repository root):

```
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
build\release\srt-bench.exe --help
```

Linux / the VPS, as a Docker image:

```
docker build -f remote/docker/Dockerfile -t bmb-remote .
```

## Run the relay on the VPS

Copy `remote/docker/compose.yml`, `remote/docker/.env.example` and the image (or build it there)
to the VPS. Then:

```
cp .env.example .env        # set BMB_RELAY_TOKEN, optionally BMB_RELAY_PASSPHRASE
docker compose -f compose.yml up -d
```

Open UDP 7000 in the VPS firewall. The relay reads `BMB_RELAY_PORT`, `BMB_RELAY_PASSPHRASE`,
`BMB_RELAY_TOKEN`, `BMB_RELAY_MAX_MBPS` and `BMB_RELAY_LATENCY_MS`; the same names exist as
`--port`, `--passphrase`, `--token`, `--max-mbps` and `--latency-ms` flags.

Self-test on the VPS (relay + 10 Mbps through it, one command):

```
docker run --rm --entrypoint sh bmb-remote /usr/local/bin/selftest.sh
```

## Run the matrix from this PC

`-Host` must be an **IPv4 address**, not a hostname.

```
powershell -NoProfile -ExecutionPolicy Bypass -File remote/run-matrix.ps1 `
  -Host 203.0.113.10 -Port 7000 -Token <token> -Minutes 10
```

The script runs all 16 rows and writes `remote/bench-results/<yyyy-MM-dd-HHmm>.md`, printing its
path at the end.

For each **direct** row the receiver has to run on the VPS (the script cannot reach the VPS).
The script prints the exact command and waits for Enter:

```
docker run --rm --network host --entrypoint srt-bench bmb-remote \
  recv --listen --port 7001 --seconds 630
```

Start one receiver per direct row (it exits when that run's sender closes). If UDP 7001 is not
reachable from this PC, open it in the VPS firewall first. `-NoPrompt` runs the direct sends
without waiting, for when the receiver is already up.

Every sender and requester opens its own SRT connection to `-Port` on the VPS; the relay pairs
them by stream ID. The token must match `BMB_RELAY_TOKEN`; a passphrase, when set, must match on
all sides.

## Read the results

The last summary row of each run is in the results file. `result` is `PASS` when the run saw
`drop_recv == 0`, `seq_gaps == 0` and `snd_drop == 0`; `mbps avg` is within a few percent of the
target (the 32-byte payload header is included).

Before calling the benchmark done:

1. the 40 Mbps tuned rows (direct and via relay) must show `PASS` for 10 minutes;
2. commit `remote/bench-results/<file>.md` next to this plan (or paste it into the design's
   results section) and tell the reviewer.

If 40 Mbps cannot pass on the real path with any setting, stop and review the remote design
before the Bridge work: the fallback is the lower camera quality in the design, not re-encoding.
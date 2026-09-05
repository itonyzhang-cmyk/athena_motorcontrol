# Athena Bench WebUI

This local service exposes a small, fixed-action control panel for the
reviewed normal-firmware bench workflow. It is not a generic terminal, CAN
transmit console, or remote shell.

## Start

From the repository root:

```sh
python3 tools/athena_bench_webui.py
```

The service listens on all local interfaces at port `8788` and prints an
authenticated local URL and LAN URL. Open the URL printed by that exact
process; the random token is required for every API call and is not persisted
by the service.

To select a different port or provide a token yourself:

```sh
python3 tools/athena_bench_webui.py --port 8788 --token a-long-private-token
```

For an experimental image, set `ATHENA_BENCH_CONFIG` to that artifact's
explicit configuration file. This does not change `athena_bench_webui.json`:

```sh
ATHENA_BENCH_CONFIG=artifacts/athena_config_commit_async_v8_20260905/flash_config.json \
  python3 tools/athena_bench_webui.py --port 8788 --token a-long-private-token
```

Record the selected configuration path and the SHA shown by the WebUI before
any motion test. Never use an experimental config as the normal release
pointer.

The current UC12 bridge schedule defaults to a 20 ms MIT interval. Keep the
bridge's latest-value queueing and the firmware's approximately 100 ms
watchdog margin intact; a 5 ms host interval is not an accepted motion-test
baseline.

Only share the token URL with people permitted to operate the bench. The panel
does not use TLS, so it is intended for a trusted private LAN only.

## Actions

- Host tests and normal-image rebuild are offline actions.
- `flash-normal` is hash-locked to
  `bac3554fa8e230fc7dae72bfb3fbef1c5e17376fd8b3ccf0011d1f4d3d373367`.
  It requires the exact SHA in the UI and a physical-bench confirmation.
- `boot-normal` separately requires a physical-bench confirmation.
- `diag ping` sends only the normal image's compatibility PING. It does not
  run driver wake, injection, or motor-enable commands.
- CAN trace starts UC12 channel 0. Its only send button writes the fixed MIT
  test frame `t00187FFF7FF0000007FF` three times at 200 ms intervals; it
  contains no `0xFC` enable command. The pass condition is three bridge log
  lines `TRACE CAN RX t000#...` with no motor movement.

The service permits only these named actions. It intentionally has no endpoint
that accepts a shell command, arbitrary file path, or arbitrary CAN frame.

Do not run another UC12 client, SLCAN bridge, SavvyCAN, or `cat` on the bridge
PTY at the same time as an active panel action; UC12 access is exclusive.

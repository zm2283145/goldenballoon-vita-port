# Vita TCP profiler

The Vita profiling build records bounded, named CPU timing events and streams
the complete trace to a development computer. It is intended for comparing
ordinary and HD-texture runs without doing network work on the game/render
thread.

The stream includes these zones plus frame and draw-call counters:

- `game.frame.cpu`
- `vitagl.render_walk.cpu`
- `vitagl.replay_walk.cpu`
- `vitagl.texture_upload.cpu`
- `vitagl.swap_buffers.cpu`

Profiler data uses TCP port `18195`. DebugNet log text uses UDP port `18194`;
the two protocols and receivers are not interchangeable. This integration does
not start DebugNet, and CMake rejects enabling the profiler and debugger in the
same binary until they have one explicit shared network owner.

## Build

Use a fresh build directory when changing the receiver address. For a receiver
at `10.1.1.146`, a typical PowerShell configuration is:

```powershell
cmake -S . -B build-vita-profiler-tcp -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=C:/vitasdk/share/vita.toolchain.cmake `
  -DCMAKE_BUILD_TYPE=Release `
  -DMDKR_APP=OFF `
  -DMDKR_VITA_PROFILER=ON `
  -DMDKR_VITA_DEBUGGER=OFF `
  -DMDKR_VITAPROFILER_DIR=D:/Claude/VitaDebugger/profiler `
  -DMDKR_VITA_PROFILER_HOST_A=10 `
  -DMDKR_VITA_PROFILER_HOST_B=1 `
  -DMDKR_VITA_PROFILER_HOST_C=1 `
  -DMDKR_VITA_PROFILER_HOST_D=146 `
  -DMDKR_VITA_PROFILER_PORT=18195 `
  -DMDKR_VITA_PROFILER_CAPTURE_FRAMES=300
cmake --build build-vita-profiler-tcp --parallel
```

Set the four host octets to the development computer's address. The loopback
defaults are deliberately safe and will not reach another machine. Zero capture
frames means an unbounded capture that ends only during orderly application
shutdown; 300 is preferred for routine comparisons.

Before packaging, the Vita ELF should contain `vp_stream_writer_begin` and
`vp_vita_tcp_sink_connect`, and should not contain `uvdb_debugnet_start`:

```powershell
arm-vita-eabi-nm build-vita-profiler-tcp/mdkr64 | Select-String `
  'vp_stream_writer_begin|vp_vita_tcp_sink_connect|uvdb_debugnet_start'
```

## Capture

Allow inbound TCP `18195` in the computer firewall. Start the receiver before
launching the game:

```powershell
py -3 D:/Claude/VitaDebugger/profiler/tools/vitaprofiler_trace.py receive `
  diddy-hd.vptrace --bind 0.0.0.0 --port 18195 --source 10.1.1.93 `
  --accept-timeout 120 --idle-timeout 10 --capture-timeout 300 --force
```

The profiler waits for a connected Vita network interface, connects in its own
consumer thread, and then enables the nonblocking game hooks. At the requested
complete-frame limit it disables producers, drains the ring, sends a clean EOF,
and releases its socket and SceNet ownership while the game continues.

Inspect or export a completed capture with:

```powershell
py -3 D:/Claude/VitaDebugger/profiler/tools/vitaprofiler_trace.py view `
  diddy-hd.vptrace --events 40
py -3 D:/Claude/VitaDebugger/profiler/tools/vitaprofiler_trace.py chrome `
  diddy-hd.vptrace diddy-hd.chrome.json --force
```

## Failure evidence and shutdown safety

`ux0:data/goldenballoon/profiler_status.txt` records the current stage, first
error, endpoint, ring loss, writer state, and TCP state. Connection failure
disables profiling but does not block the game.

The consumer is the sole network owner. Shutdown disables producers, drains and
closes the writer, retries socket cleanup, and only then terminates SceNet. If a
socket cannot be proven closed, it intentionally leaves the network lifetime
for process teardown instead of terminating SceNet underneath a live resource.

PMU events are intentionally not part of this ordinary game build. The current
kernel PMU path requires separate forced-exit and restoration gates; portable
CPU zones are sufficient for the first HD-texture performance comparison and
cannot leave a PMU restoration lease behind when LiveArea closes the process.

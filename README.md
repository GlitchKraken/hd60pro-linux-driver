# mz0380 — Linux driver for Elgato Game Capture HD60 Pro

Experimental PCIe capture driver for the Elgato Game Capture **HD60 Pro**
family (YUAN MZ0380, PCI `12ab:0380` / `12ab:0381`). The card contains an ARM
SoC running its own Linux firmware. The host driver brings up the MST3367 HDMI
receiver, controls the card through a BAR0 mailbox, maps DMA buffers, and
exposes the result as a V4L2 capture device.

> **Current result:** persistent 1920x1080 High Profile H.264 capture works in
> OBS, including repeated userspace detach/attach, a host-owned NO SIGNAL
> picture, same-mode HDMI unplug/reconnect, and clean-IDR recovery. The
> all-frame scheduler is hardware-validated at approximately 60 encoded fps
> from a confirmed 1080p60 input, with one encoder spawn and a clean final
> stop.
>
> **Uncompressed capture works as of 2026-08-28 (M215/M217).** With
> `raw_deliver=1 post_mask=0x01` the node advertises `YU12` and delivers whole
> 1920x1080 I420 frames - no encoder in the delivery path. `post_skip` throttles
> the rate: the card's preview bitmap step is `post_skip+1`, and full-rate raw
> at 1080p60 is ~186 MB/s against a link this card negotiates as PCIe x1 Gen1.
>
> **HDMI OUT passthrough and HDMI audio capture work as of 2026-11-02.** The
> card's HDMI OUT port now carries the source's picture and sound to a monitor,
> and the source's audio is captured to the PC as an ALSA device, **HD60 Pro
> HDMI**, which OBS records alongside the video. Both are on by default and
> hardware-validated with a Nintendo Switch 2 at 1080p60 into OBS, with a
> 1440p monitor on HDMI OUT. See
> [HDMI OUT passthrough and audio](#hdmi-out-passthrough-and-audio).

This is reverse-engineered development code, not a mainline or production
driver. Read the [spawn-budget warning](#encoder-spawn-budget) before testing.

## What works today

- PCI probe, BAR mapping, mailbox commands, firmware handshake and MSI.
- MST3367 HDMI receiver initialization, signal detection and DV timings.
- A translating-IOMMU DMA path using four independently mapped buffers.
- Persistent 1920x1080 H.264 capture through V4L2 and OBS.
- Correct decoder boundaries: initial delivery and reconnect wait for a clean
  SPS/PPS-bearing IDR.
- One card-side encoder pipeline across ordinary OBS STREAMOFF/STREAMON cycles.
- Same-mode unplug/reconnect without a second `SET_VIC` encoder spawn.
- A two-fps, host-owned H.264 NO SIGNAL picture during a validated HDMI-unplug
  interval.
- Transient timing-change cancellation when the source returns to the running
  mode.
- Clean final pipeline stop at module removal.
- Legacy single-frame, planar I420 capture with the default raw profile.
- V4L2 compliance: 148/148 tests pass; the five warnings are the unsupported
  `DV_RX_POWER_PRESENT` indication.
- HDMI OUT passthrough, picture and sound, through the card's IT66121
  transmitter. Any monitor resolution works; it is sent the source's 1080p.
- HDMI audio capture: 2-channel, 16-bit, 48 kHz PCM as the ALSA capture device
  `HD60 Pro HDMI`, usable from OBS, PipeWire/PulseAudio and `arecord`.
- DKMS install that autoloads the module at boot.
- Builds validated with clang/lld and `W=1` against Linux 6.18 LTS, 7.2 and
  7.3.

Not implemented or not yet proved:

- Capturing source resolutions other than 1920x1080. This is about the
  source's output, not the monitor on HDMI OUT.
- Surround audio. Capture and passthrough are stereo only; set the source's
  audio output to stereo.
- Audio without video. The card captures audio inside the same pipeline as
  video, so the ALSA device only delivers while the V4L2 node is streaming.
- Re-running the HDMI OUT setup automatically after a monitor swap or a source
  mode change (`echo tx | sudo tee /proc/mz0380-hdmi` does it by hand).
- Native webcam-style compatibility with every camera application. The live
  V4L2 node currently advertises compressed H.264; applications that require
  raw YUYV/NV12 camera frames may not list or accept it.
- The zero-spawn placeholder path when the first STREAMON occurs with HDMI
  already absent. It is implemented and host-validated, but still needs its
  dedicated hardware run.
- A genuinely changed returning HDMI mode and its controlled replacement.
- Automatic HPD recovery for a connected-but-unlocked outage.
- Module signing, distribution packages and mainline submission.

## Supported cards

The PCI function must be vendor/device `12ab:0380` or `12ab:0381` with one of
the supported subsystem IDs:

| Subsystem | Variant |
|---|---|
| `1cfa:0003` | HD60 Pro Rev. 1 |
| `1cfa:0005` | HD60 Pro Rev. 2 (unreleased) |
| `1cfa:0006` | HD60 Pro Rev. 1 + Ryzen fix |
| `1cfa:0010` | HD60 Pro Rev. 3 (`DEV_0381`) |
| `12ab:05cf` | HD60 Pro prototype |

Confirm the hardware before building:

```bash
lspci -nn -d 12ab:0380
lspci -nn -d 12ab:0381
```

The preserved `sc0710-*` files target the different Elgato 4K60 Pro Mk.2.
They are excluded from the default build and do not compile on modern kernels.

## Requirements

### Hardware and platform

- A supported HD60 Pro in a PCIe slot.
- A 1920x1080 HDMI source. Use a source explicitly fixed at 60 Hz for the
  30-fps H.264 checkpoint or the raw-bank 60-Hz investigation.
- IOMMU/VT-d/AMD-Vi enabled in firmware and a **translating** Linux IOMMU
  domain. `iommu=off`, `iommu=pt`, and identity/pass-through domains cannot
  work with this card's high-32-bits-only outbound DMA target.
- A way to remove slot power completely. Once the card mailbox wedges, a
  reboot or soft power action may leave standby power present; use the PSU
  switch or unplug mains power.

The driver refuses to arm unreachable stream buffers when no translating
domain is available. Check the boot log and the driver's mappings:

```bash
sudo dmesg | grep -iE 'iommu|amd-vi|dmar'
sudo dmesg | grep -E 'mz0380.*mapped at IOVA|IOMMU domain|IOVA setup'
```

Successful setup prints mappings at 4-GiB-spaced IOVAs. If the driver reports
`no IOMMU domain` or `pass-through/identity`, enable the platform IOMMU and boot
without `iommu=off` or `iommu=pt`.

### Build and userspace tools

Required to build and load:

- the build tree/headers for the **running** kernel at
  `/lib/modules/$(uname -r)/build`;
- GNU Make and a kernel-compatible C toolchain;
- root access for module loading;
- the in-kernel V4L2/videobuf2 modules used by the loader.

The Makefile uses clang and `ld.lld` automatically when both are installed,
which is needed for kernels built with clang-only flags. Otherwise it uses the
kernel build system's default compiler.

Useful userspace packages:

- `v4l-utils` for `v4l2-ctl` and `v4l2-compliance`;
- OBS Studio for the validated live workflow;
- FFmpeg/ffprobe/ffplay for recording and inspecting elementary H.264;
- `pciutils` for `lspci`.

If Secure Boot enforces signed modules, an unsigned `mz0380.ko` will be rejected
with `Key was rejected by service`. Sign/enrol the module using your
distribution's normal process or disable enforcement before trying to load it.

## Firmware: do not upload anything

**The driver does not upload firmware. Do not restore or use the deleted
firmware-upload path.** The card boots its onboard flash image, and an earlier
host upload attempt broke the card's userspace.

### What is needed after a PC power cycle

The Elgato download is **not a runtime or build dependency**. You do not need
the Windows installer `.exe`, its `.sys` driver, an extracted
`MZ0380.HD.HEX`, or an extracted `tinyvenc` binary to build, load, or use this
Linux driver. The card contains its own ARM/Linux firmware and encoder binaries
in onboard flash and boots them by itself whenever the card receives power.
The host module only handshakes with that already-running firmware and then
controls capture through the PCIe mailbox.

A real mains/PSU power removal restarts the card firmware and clears a wedged
card-side encoder state. If the module is installed with
[`sudo make dkms-install`](#install), it loads by itself at the next boot and
nothing else is needed. Without an install, load it again from the build tree
with the recommended profile:

```bash
sudo env \
  VICFW=7 H264PROBE=1 POLLDRAIN=0 \
  WINSEQ=1 OP6=1 POSTMASK=0 FASTKILL=0 \
  H264DIVISOR=0 PERSIST=1 \
  ./mz0380-live.sh load
```

The loader builds `mz0380.ko` for the running kernel when matching kernel
headers are available, then inserts it and prints the `/dev/videoN` device.
Re-extraction from the Elgato installer is never part of this process. The
checked-in `mz0380-no-signal-data.h` already contains the generated host-side
NO SIGNAL H.264 frame; the local `tinyvenc5` dump mentioned by its generator is
needed only if a developer deliberately regenerates that file, not for a
normal build or at runtime.

The only requested file is an optional ASCII version sidecar:

```text
/lib/firmware/mz0380/MZ0380.FW.TXT
```

It contains the expected version in `MM.mm` form. A mismatch only warns; an
absent file skips the check. The tested card reports `01.11`:

```bash
sudo install -d /lib/firmware/mz0380
echo 01.11 | sudo tee /lib/firmware/mz0380/MZ0380.FW.TXT
```

`MZ0380.HD.HEX` is neither needed nor read. There is no `firmware_upload`
module parameter, and passing it makes `insmod` fail with an unknown parameter.

## Build

Build for the running kernel as an ordinary user:

```bash
make kernels
make
modinfo ./mz0380.ko | head
```

Other supported build operations:

```bash
make KVER=<installed-kernel-version>  # cross-build for one installed kernel
make all-kernels                     # save all builds under ko/
make objclean                        # clean top-level kbuild products
make clean                           # normal clean; preserves ko/
make distclean                       # clean and remove ko/
```

If `/lib/modules/$(uname -r)/build` is missing, no out-of-tree module can be
built for or loaded into that running kernel. Install matching headers, or
reboot into an installed kernel whose headers are present. A `.ko` built for a
different kernel will fail with `Invalid module format`; check its vermagic:

```bash
uname -r
modinfo -F vermagic ./mz0380.ko
```

## Install

The driver's compiled-in defaults are the validated 1080p60 capture
configuration, and `MODULE_DEVICE_TABLE` covers every known subsystem ID, so an
installed module autoloads when the card is present. No parameters are needed.

**Persistent install (recommended)** - rebuilds itself on every kernel upgrade:

```bash
sudo make dkms-install
sudo modprobe mz0380
```

**Single-kernel install** - quicker, but the next kernel upgrade leaves it
behind:

```bash
sudo make install
sudo modprobe mz0380
```

Either way `/etc/modprobe.d/mz0380.conf` is installed. It pulls in the
V4L2/ALSA dependencies, sets the two options where `mz0380-live.sh` differs
from the module defaults (`signal_poll_ms=4000 procfs_verbosity=2`), and
documents the other knobs worth knowing, commented out. To remove:
`sudo make dkms-uninstall` or `sudo make uninstall`.

Verify after the next boot:

```bash
lsmod | grep mz0380
v4l2-ctl --list-devices
arecord -l | grep -i hd60
```

Use a full shutdown and power-on rather than a reboot when you can: a warm
reboot may leave the card powered, carrying over its
[encoder spawn count](#encoder-spawn-budget).

To test a changed build once it is installed, `sudo rmmod mz0380` first, then
reinstall with `sudo make dkms-uninstall && sudo make dkms-install`, or load the
new build directly with `mz0380-live.sh`.

The development workflow below (`mz0380-live.sh`) is a different thing: it
insmods the module straight out of the build tree with explicit parameters, for
experiments. You do not need it to use the card.

## Recommended persistent H.264 workflow

The module's historical bare defaults intentionally preserve the old raw
single-frame baseline. For working live capture, use the exact validated H.264
profile below. The wrapper builds the module, runs a zero-spawn load/unload
smoke test, loads dependencies, inserts the driver, waits for the V4L2 node,
and leaves it loaded for OBS:

```bash
make

sudo env \
  VICFW=7 H264PROBE=1 POLLDRAIN=0 \
  WINSEQ=1 OP6=1 POSTMASK=0 FASTKILL=0 \
  H264DIVISOR=0 PERSIST=1 \
  ./mz0380-live.sh load
```

What the profile selections do:

| Loader variable | Module parameter | Purpose |
|---|---|---|
| `VICFW=7` | `vic_fw=7` | Select continuous tinyvenc7 instead of single-shot tinyvenc5. |
| `H264PROBE=1` | `h264_probe=1` | Allocate the independent Windows-style H.264 window-1 ring and advertise H.264. |
| `POLLDRAIN=0` | `poll_drain_ms=0` | Disable the legacy raw-frame polling path. |
| `WINSEQ=1` | `win_seq=1` | Use the Windows capture-start order. |
| `OP6=1` | `win_start_op6=1` | Send the additional start doorbell required by this path. |
| `POSTMASK=0` | `post_mask=0` | Avoid the mask that truncates DMA to 16 bytes. |
| `FASTKILL=0` | `vic_fast_kill=0` | Request orderly card-side encoder teardown. |
| `H264DIVISOR=0` | `h264_frame_divisor=0` | Select the hardware-validated all-frame bitmap: approximately 60 encoded fps from 60-Hz input. |
| `PERSIST=1` | `persistent_h264=1` | Keep one pipeline alive across userspace detach/attach. |

Several values in the command are also current parameter defaults, but spelling
out the complete validated profile prevents a future default change from
silently changing a hardware test.

The loader prints the actual `/dev/videoN`; do not assume it is
`/dev/video0`. Check the live state at any time:

```bash
sudo ./mz0380-live.sh status
cat /proc/mz0380-state
sudo ./mz0380-live.sh watch      # Ctrl-C stops watching, not the module
```

A healthy active state should show all of the following:

- `source: 1920x1080p @ 30 fps` or `@ 60 fps`;
- `pixelformat: H264` in `/proc/mz0380-state`;
- a running pipeline and attached VB2 consumer;
- increasing H.264 delivered counters;
- zero command timeouts;
- normally one `SET_VIC`/encoder spawn for the pipeline lifetime.

### Camera-application compatibility

On Windows, Elgato's driver presents the HD60 Pro as a camera source that can
be selected directly in camera-aware applications. Matching that experience
is a project requirement, and it must be implemented as native V4L2 format
negotiation rather than as a mandatory OBS/FFmpeg virtual-camera bridge.

The saved live DirectShow enumeration proves that both Windows capture pins
advertise YUY2, YV12, NV12, RGB24, RGB32, main H.264, and 960x540 substream
H.264. YUY2 is the current/default type there and is offered at 1920x1080 at
30, 50, and 59.94 fps. Static analysis of the Windows `.sys` now also confirms
that its camera filter has two video pins sharing 320 data-range descriptors.
Its pin-create callback records the application's bit depth and FourCC, keeps
raw streams in one slot group, and assigns H.264 and X264 to separate groups.
This selection happens when an application opens a pin, not when the Windows
driver loads.

The Linux driver currently registers a standard `/dev/videoN`, but it exposes
only one format chosen globally by reverse-engineering module parameters.
That is temporary scaffolding. Loading the module should only probe the PCI
device, initialize signal detection, and register its V4L2/ALSA interfaces. An
application must then select YUYV/NV12/YV12/H.264 through `VIDIOC_S_FMT`, with
the matching card DMA path configured only at STREAMON. H.264 must not be
forced merely because the module loaded.

Do not advertise a raw format before its continuous full-frame path works: the
bounded M203 raw-bank test received only 16-byte records, while the older
tinyvenc5 path produced one complete 3,110,400-byte planar I420 frame and then
stalled. The Windows raw delivery callback contains extensive format conversion
and scaling, including YV12/NV12 branches. Static analysis now identifies its
native source exactly: an eight-slot op02/op08 ring, with each slot containing
contiguous planar Y/U/V. At 1920x1080p60 (`fw=7`, VBI disabled), the active
write is I420-sized `0x2f7600` / 3,110,400 bytes: Y `0x1fa400`, then U and V
`0x7e900` each. Windows converts that native planar surface to YUY2 in its host
driver. This still does not prove that the card rotates full frames under the
Linux start sequence, so raw formats remain unadvertised until the raw-only
bounded discriminator produces repeated complete writes. A virtual-camera
decoder may be useful as an optional workaround, but it is not the intended
driver architecture. H.264 software decoding still does not belong in kernel
space.

With the validated all-frame H.264 schedule, 1080p60 produces approximately
60 H.264 access units per second. Divisor 2 remains an optional 30-fps fallback.

### OBS setup

1. Add **Video Capture Device (V4L2)** and select the `/dev/videoN` printed by
   the loader (`mz0380 H.264`).
2. Use the device's advertised 1920x1080 format and H.264 input.
3. Turn off **Deactivate when not showing**.
4. Turn off **Use Buffering** for lower preview latency.
5. Keep one source instance. During a bounded hardware discriminator, do not
   reopen/apply Properties or let OBS retry a failed source in a loop.

With `persistent_h264=1`, closing/reopening the ordinary same-mode OBS source
attaches to the existing pipeline rather than issuing another `SET_VIC`. A real
mode change queues one controlled encoder replacement at the next attachment.

For sound, also add **Audio Input Capture (PipeWire/PulseAudio)** and pick the
`HD60 Pro HDMI` device (PipeWire may list it as "HD60 Pro HDMI Analog Stereo").
To hear it, set **Edit → Advanced Audio Properties → Audio Monitoring** to
**Monitor and Output** for that source.

## HDMI OUT passthrough and audio

Both are on by default; there is nothing to load or configure.

**HDMI OUT.** Connect a monitor to the card's HDMI OUT port. The driver
programs the card's ITE IT66121 transmitter at probe, so the source's picture
and sound appear on the monitor as soon as the module loads, with or without
OBS running. The monitor can be any resolution - it receives the source's
1080p and scales it. Audio is 2-channel 48 kHz LPCM, so set the source's audio
output to stereo (on a Switch: *System Settings → TV Output → TV Sound →
Stereo*), and turn off HDR, which the card cannot carry.

**Audio capture.** The ALSA card `HD60 Pro HDMI` (`hw:HD60Pro`) delivers
16-bit stereo at 48 kHz. Audio flows only while video is streaming, because
the card captures both in one pipeline. A quick check while OBS or another
application has the video node open:

```bash
arecord -D hw:HD60Pro -f S16_LE -r 48000 -c 2 -d 5 test.wav && aplay test.wav
```

Neither feature starts the card's encoder, so neither costs encoder spawns.

**Diagnostics** through `/proc/mz0380-hdmi` (root only):

| Command | Effect |
|---|---|
| `echo tx > /proc/mz0380-hdmi` | Re-run the HDMI OUT setup, e.g. after swapping monitors. |
| `echo txstat > /proc/mz0380-hdmi` | Log the transmitter status: hot-plug, monitor detected, input video stable, audio CTS. |
| `echo 'txw REG VAL' > /proc/mz0380-hdmi` | Write one IT66121 register (`0x100`-`0x1ff` is bank 1). |
| `echo audio > /proc/mz0380-hdmi` | Log the audio event count, last period index and a sample of each period buffer. |

Results go to `dmesg`. Module parameters, all writable at runtime through
`/sys/module/mz0380/parameters/` except `audio_dma`. The `hdmitx*` ones take
effect at the next `tx`, `aic_input` at the next stream start:

| Parameter | Default | Meaning |
|---|---|---|
| `hdmitx` | `1` | Set up HDMI OUT at probe. |
| `hdmitx_hdmi` | `1` | HDMI mode with infoframes; `0` is DVI, with no audio. |
| `hdmitx_input` | `0x41` | Transmitter input format: YUV 4:2:2, separate syncs, input clock delay 1. Delay 2 (`0x42`) also works. |
| `hdmitx_audio` | `1` | Pass audio through HDMI OUT. |
| `hdmitx_mclk` | `1` | Transmitter audio MCLK ratio (256fs). |
| `audio_dma` | `1` | Register the audio capture buffers. Read at load. |
| `aic_input` | `0` | Audio input requested from the card. Only `0` starts capture. |

How these were found - the transmitter's input wiring by sweeping its
registers live, and the card's audio path by disassembling its firmware - is
recorded in the headers of `src/mz0380-hdmitx.c`, `src/mz0380-audio-dma.c`
and the corresponding commit messages.

### HDMI loss and NO SIGNAL behavior

The current implementation behaves as follows. The unplug/same-mode reconnect
path is hardware-validated; initial STREAMON with HDMI already absent and a
genuinely changed returning mode are still explicit validation items below.

- If STREAMON begins without HDMI lock, the driver immediately serves the
  embedded host H.264 NO SIGNAL IDR at two fps and sends no `SET_VIC` or
  pipeline-start command. A stable lock then starts the card pipeline once.
- If HDMI disappears during live capture, the card pipeline stays alive and
  DMA completions continue to be drained and acknowledged while the
  placeholder is presented.
- A same-mode reconnect keeps `SET_VIC` at one and returns to live video only
  at a clean SPS/PPS-bearing IDR.
- A genuinely changed mode keeps the placeholder active, queues a V4L2 source
  change, and performs one controlled replacement on the next attachment.
- Healthy H.264 delivery does not poll the receiver mailbox. After 1500 ms of
  producer silence, recovery starts and receiver checks run at 500-ms intervals.

Status exposes placeholder IDRs, cadence misses, recovery state, suppressed
live units and pending replacement state. `SIGMON=<ms>`, `NOSIGFPS=<1..10>`,
and `EXTRA="hotplug_stall_ms=<ms>"` are available for bounded tests; keep the
validated defaults for ordinary use.

### Capture H.264 without OBS

After loading the recommended profile, substitute the printed node below:

```bash
timeout 10s v4l2-ctl -d /dev/videoN \
  --stream-mmap=4 --stream-to=/tmp/mz0380.h264
ffprobe /tmp/mz0380.h264
ffplay -fflags nobuffer -flags low_delay /tmp/mz0380.h264
```

Finally close OBS or any other user of the node, then unload exactly once:

```bash
sudo ./mz0380-live.sh unload
```

The expected final log contains one successful `final pipeline stop` boundary.
The explicit unload also records the spawn tally and removes root-owned build
products that could obstruct a later non-root build.

## Encoder spawn budget

`SET_VIC` creates a new encoder process on the card. Historical testing wedges
the card's mailbox somewhere around **8–18 encoder spawns per real power
cycle**. The visible failure may look like loss of HDMI because the mailbox can
no longer bring up the receiver, even though PCI configuration and BAR reads
still look normal.

Persistent H.264 exists partly to avoid this: ordinary V4L2 detach/attach and
same-mode HDMI recovery reuse one pipeline. Still, inspect the tally before
experiments:

```bash
scripts/mz0380-spawns.sh
```

The tally is stored in `/run`, and loading scripts commit their per-module
count before unload. A warm reboot may clear `/run` without removing PCIe slot
power, so only reset the tally after a known full power removal:

```bash
sudo scripts/mz0380-spawns.sh reset
```

Do not fake video by repeatedly restarting the legacy one-frame stream. Do not
loop reloads, OBS source retries, or mode replacements. Once the tally is in
the historical 8–18 range, a further spawning run is a gamble.

## Card health check

This takes about two seconds, requires no HDMI source, and does not STREAMON or
spawn an encoder:

```bash
sudo modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc \
  v4l2-dv-timings snd-pcm
sudo insmod ./mz0380.ko
sudo dmesg | grep -aE 'CMD_INIT|handshake' | tail -3
sudo rmmod mz0380
```

Healthy:

```text
CMD_INIT answered on attempt 1 (status=0xdddddddd)
```

Wedged:

```text
CMD_INIT got no answer (-110)
the mailbox is deaf
```

If wedged, stop testing and remove mains power. Do not investigate EDID, the
HDMI source or PCI BAR values until the mailbox answers again.

## Legacy one-frame raw capture

A bare `insmod` uses `vic_fw=5`, `h264_probe=0` and the 20-ms poll-drain path.
It is useful as a known raw control, but it is not continuous video:

```bash
sudo modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc \
  v4l2-dv-timings snd-pcm
sudo insmod ./mz0380.ko
v4l2-ctl -d /dev/videoN --stream-mmap --stream-count=1 \
  --stream-to=/tmp/frame.i420
ffplay -f rawvideo -pixel_format yuv420p -video_size 1920x1080 \
  /tmp/frame.i420
sudo rmmod mz0380
```

The result is one correct 1920x1080 planar I420 frame (3,110,400 bytes). The
queue then ends with `-EIO`/`EPOLLERR` instead of blocking forever. Decoding it
as NV12 produces magenta/green interleave banding because NV12 has the same
byte count but a different chroma layout.

The validation harness reloads the module, waits for HDMI lock, captures and
scores this raw control, then unloads:

```bash
sudo scripts/mz0380-m55-real-capture.sh 1 45
```

Each raw STREAMON sends `SET_VIC`; use the harness sparingly and count spawns.
`make load`, `make load-streaming`, `make capture`, and `make capture-h264` are
legacy/raw helpers, not the recommended persistent H.264 workflow.

## Troubleshooting

### No `/dev/video*` node

Inspect the driver log first:

```bash
sudo dmesg | grep mz0380 | tail -80
lspci -nnk -d 12ab:
```

Common causes are an unsupported subsystem ID, missing V4L2 dependencies, a
firmware-handshake timeout, an IOMMU setup failure, module signature rejection,
or a module built for another kernel.

### HDMI is connected but status says unlocked

First distinguish a receiver/source problem from a deaf card:

```bash
sudo ./mz0380-live.sh status
sudo dmesg | grep -aE 'CMD_INIT|ret=-110|MST3367 signal' | tail -30
```

Any mailbox timeout points back to card health and slot power. With no timeout,
confirm the source is actually fixed at 1920x1080p30 or p60 and allow the
receiver to settle. Do not automate HPD pulses: they have only been proved not
to break healthy capture, not to shorten a connected-but-unlocked outage.

### OBS is black, frozen or laggy

- Verify `H264PROBE=1`, `VICFW=7` and `POLLDRAIN=0` were present in the load
  banner.
- Check that H.264 delivered counters increase and command timeouts remain zero.
- A 30-Hz source yielding about 15 encoded fps is the known divisor-2 limit.
- If NO SIGNAL is active, inspect `recovery`, `source`, suppressed units and
  pending replacement in `/proc/mz0380-state`.
- Remove duplicate OBS capture sources and disable buffering.
- Do not repeatedly reopen Properties while diagnosing an encoder failure.

### Module will not unload

Use the wrapper, which terminates users holding the V4L2 node:

```bash
sudo ./mz0380-live.sh unload
cat /sys/module/mz0380/initstate 2>/dev/null
```

If the state is `going`, the kernel module is stuck in its removal path and
another `rmmod` cannot repair it. Remove mains power before the next session.

### A root-run harness broke the next build

The explicit live-script unload normally removes root-owned generated files.
If a test was interrupted, inspect ownership, then run `make clean` with the
appropriate privileges before rebuilding as your normal user.

## Diagnostics

| Interface | Contents |
|---|---|
| `/proc/mz0380` | Devices, board identity, BAR map and V4L2 node. |
| `/proc/mz0380-state` | Signal, format, source rate, pipeline, H.264, placeholder, IRQ, timeout and spawn counters. |
| `/proc/mz0380-snapshot` | Targeted register snapshots in five profiles. |
| `/proc/mz0380-control` | SDK-grouped control surface. |
| `/proc/mz0380-experiment` | Bounded register probes and property writes. |

Useful commands:

```bash
cat /proc/mz0380-state
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoN --all
v4l2-ctl -d /dev/videoN --query-dv-timings
modinfo ./mz0380.ko
sudo dmesg | grep mz0380 | tail -100
```

Most module parameters are reverse-engineering controls, not user-facing
tuning. `modinfo ./mz0380.ko` documents their defaults and milestone references.
Use the validated profile unless a documented experiment explicitly requires a
different value.

## True 60-fps H.264 capture is validated

`h264_frame_divisor` is historically named: the SET_ENC_PARAMS byte is really
tinyvenc7's `skip` field. Value 0 selects a 128-bit bitmap which tinyvenc7
itself fills with one bit for every input frame. M205 validated this mode at a
confirmed 1920x1080p60 input: 4,339 frame events over approximately 73 seconds,
about 59--60 fps, with one SET_VIC, zero FIFO drops, zero command timeouts, and
a clean final STOP. The nine H.264 drops were two short intervals with no
queued userspace buffer, not an encoder cadence limit.

M206 repeated the all-frame path for approximately 279.7 seconds: 16,754 frame
events (about 59.9 fps), two userspace attachments on one persistent encoder
spawn, zero FIFO drops, and another clean final STOP.

Value 0 is now the module and recommended-profile default. Value 2 remains a
working 30-fps fallback, while value 1 is invalid because its modulo predicate
can never have remainder one. The all-frame path does not require opcode
`0x32`, the Elgato Windows package, or replacement card firmware.

The Windows-style independent `RAWBANKS=1` discriminator is closed for the
H.264-selected M203 start at confirmed 1920x1080p60. Each opcode-`0x02` buffer
received only 16 bytes and every independent opcode-`0x08` buffer remained
untouched. Static analysis later proved those are nevertheless the exact
buffers consumed by Windows' raw callback; what M203 lacked was a raw pin/base-
slot start, not another bank. Do not repeat the same H.264-selected experiment.
The next distinct test must disable H.264/op04, activate the raw base selection,
and look for repeated `0x2f7600` planar Y/U/V writes across the op02/op08 ring.

`raw_bank_probe` remains an opt-in historical diagnostic and is not part of
normal capture. It must be refactored and dual-kernel build-checked for the
raw-only start before spending one bounded hardware run.

## Remaining validation checklist

The primary persistent OBS lifecycle is hardware-validated. Remaining work is:

1. Start OBS with HDMI already absent and prove placeholder-only operation has
   `pipeline: stopped` and `SET_VIC sent: 0`.
2. Return with a genuinely different HDMI mode and validate the single
   controlled replacement on the next userspace attachment.
3. Test one manual HPD pulse during a real connected-but-unlocked outage before
   considering any automatic HPD recovery.
4. Close OBS, unload once, and retain the final STOP log.

Do not deliberately exhaust the spawn budget to test recovery from a deaf
card.

## Documentation map

| File | Purpose |
|---|---|
| `NEXT_SESSION_START.md` | Detailed current handoff, latest hardware evidence and exact next experiment. |
| `RE_FINDINGS.md` | Full reverse-engineering history, milestone by milestone. |
| `MZ0380_SDK_CONTROL_PATH.md` | Card-side SDK control path and command model. |
| `mz0380-live.sh` | Recommended live loader/status/watch/unload workflow. |
| `scripts/mz0380-spawns.sh` | Cross-reload encoder-spawn accounting. |
| `scripts/mz0380-m55-real-capture.sh` | Legacy raw control-capture harness. |

The handoff contains older, explicitly superseded investigation sections for
historical context. Prefer its topmost current section and this README over old
intermediate conclusions.

## Repository layout

```
mz0380-live.sh      the load/status/watch/unload workflow - start here
Makefile            builds mz0380.ko at the repo root
COPYING             GNU GPL version 2
src/                driver sources (mz0380-*.c/.h; legacy sc0710-* alongside)
scripts/            build helpers, milestone harnesses (mXX), analysis tools
docs/               captured run logs and RE artefacts
re-dump/            vendor disassembly and the card's own firmware tree
ko/                 per-kernel module copies from `make all-kernels`
```

Scripts in `scripts/` `cd` to the repo root before doing anything, so they can
be invoked from anywhere and still find `mz0380.ko`, `docs/` and each other:

```
sudo ./mz0380-live.sh load
sudo scripts/mz0380-m55-real-capture.sh 1 45
scripts/mz0380-spawns.sh
```

## License

**GPL-2.0-or-later**, and for a Linux kernel module that is not really a
choice. Recorded as an SPDX tag in every source file, with the full text in
`COPYING`.

GPLv3 is not available here, for three independent reasons:

1. **The kernel is GPLv2-only.** Linux does not carry the "or later" clause, and
   GPLv2 and GPLv3 are mutually incompatible. A loadable module is a derivative
   work of the kernel it links against, so a GPLv3 module could not be
   distributed for use with Linux.
2. **`MODULE_LICENSE()` has no GPLv3 ident.** The accepted strings in
   `include/linux/module.h` are all GPLv2-based, plus `"Proprietary"`. A
   GPLv3-only module would have to declare itself proprietary, which taints the
   kernel and makes the loader refuse to bind `EXPORT_SYMBOL_GPL` symbols - and
   videobuf2, which this driver is built on, exports GPL-only. The module would
   not load.
`MODULE_LICENSE("GPL")` already means "GPLv2 or later", so the built module is
consistent with the SPDX tags with no change needed.

Note that reasons 1 and 2 are properties of building a Linux kernel module and
hold no matter whose code is in the tree. Removing third-party code, as the
next section describes, does not make GPLv3 available.

## Provenance

This driver was written for the HD60 Pro (MZ0380). It is not a fork.

**sc0710 (Elgato 4K60 Pro Mk.2, Steven Toth) - not used.** The repository
directory is named `sc0710` for historical reasons and early revisions carried
that driver's sources locally as reading material, but they were never tracked
in git, never built by default, and never distributed. In 2026-08 they were
moved out of `src/` to a gitignored `reference/` directory and the `legacy`
build target was deleted. Three register-map sections (`BAR0` XDMA, `BAR0`
Xilinx AXI IIC, `BAR0` HDMI signal status) had been modelled on it; every
define in all three was unused, hardware disproved the model on all three
counts, and they were removed. **No code in `src/` derives from sc0710.**

**hdcapm (Startech USB2HDCAPM, Steven Toth / Michael Grzeschik, GPL-2) - used,
and credited.** `src/mz0380-mst3367.c` follows the structure of that driver's
`mst3367_init_setup()`. This is a genuine and deliberate dependency, and it is
kept because the alternative is worse, not because it could not be avoided:

- hdcapm reverse-engineered **the same Windows driver** this project does, for
  a board built on the same Vatics Mozart 395s + MST3367 combination.
- Every register value in the sequence was independently re-derived from our
  own disassembly of `e60MZ0380.X64.SYS` and agrees with theirs. See
  `docs/re-2026-07-05/mst3367-reference-from-gpl-driver.md`, which marks each
  confirmation inline.
- The values are therefore facts about how the MST3367 is brought up, not
  authored expression, and we hold an independent derivation of them.

Since the project is GPL-2.0-or-later regardless, the honest thing is to keep
the citation rather than paraphrase the code to obscure it.

**Mainline ite-it66121 (Linux DRM bridge driver, GPL-2) - used, and
credited.** `src/mz0380-hdmitx.c` follows that driver's IT66121 power-up,
mode-set, AFE, audio and mute sequences, adapted to reach the chip through the
card's I2C mailbox proxy. The board-specific input settings (separate syncs,
non-sequential IO mode, input clock delay) are not in mainline; they were found
by sweeping the registers on hardware, with register meanings from ITE's
public IT66121 datasheet and register list.

**Everything else** comes from this project's own reverse engineering of the
card's firmware image (`ep.ko`, `video_capture_mgr`, `tinyvenc5`/`tinyvenc7`,
and for audio `audio_capture_mgr`, `capture_app_infinite` and `vpl_dmac.ko`)
and the Windows driver, documented milestone by milestone in `RE_FINDINGS.md`
and, for HDMI OUT and audio, in the source headers and commit messages.

# RTL-SDR Blog V4 on macOS with Xcode

A small native C/Xcode example showing how to connect an **RTL-SDR Blog V4** directly to macOS, open it through `librtlsdr`, tune it, and receive I/Q samples without GNU Radio, Python, SDR++, or another SDR application.

The repository also contains the current experimental CW audio receiver in `CWMacTest/main.c`. The **RTL-SDR connection and continuous I/Q path are verified**. The CW/audio stage is still experimental and has not yet been RF-validated in this snapshot.

## What has been verified

- RTL-SDR Blog V4 detected over USB
- Rafael Micro R828D / V4 driver detected correctly
- native Xcode C project
- static linking to `librtlsdr.a`
- static linking to MacPorts `libusb-1.0.a`
- center-frequency and sample-rate control
- synchronous continuous I/Q reception with `rtlsdr_read_sync()`
- conversion of unsigned 8-bit interleaved I/Q samples to normalized values

A minimal, verified I/Q-only example is included in `examples/iq_stream.c`.

## Tested setup

- RTL-SDR Blog V4
- Intel Mac
- Xcode
- MacPorts
- `rtl-sdr-blog` driver source

Apple Silicon should also be possible if `librtlsdr` and `libusb` are built natively for arm64, but this repository snapshot was tested on Intel.

## 1. Install build dependencies with MacPorts

```bash
sudo port install cmake libusb pkgconfig git
```

## 2. Build the RTL-SDR Blog driver

The Xcode project defaults to `~/build/rtl-sdr-blog`:

```bash
mkdir -p ~/build
cd ~/build
git clone https://github.com/rtlsdrblog/rtl-sdr-blog.git
cd rtl-sdr-blog
mkdir -p build
cd build
cmake .. -DCMAKE_PREFIX_PATH=/opt/local
make LIBRARY_PATH=/opt/local/lib
```

Confirm that the static libraries exist:

```bash
ls -l ~/build/rtl-sdr-blog/build/src/librtlsdr.a
ls -l /opt/local/lib/libusb-1.0.a
```

## 3. Open the Xcode project

Open:

```text
CWMacTest.xcodeproj
```

The project uses two user-defined build settings:

```text
RTLSDR_ROOT     = $(HOME)/build/rtl-sdr-blog
MACPORTS_PREFIX = /opt/local
```

If your directories are different, change those two values in **Target → Build Settings**. There are no user-specific `/Users/<name>/...` paths in this cleaned project.

The project links the two dependencies statically:

```text
$(RTLSDR_ROOT)/build/src/librtlsdr.a
$(MACPORTS_PREFIX)/lib/libusb-1.0.a
```

and links the macOS frameworks required by static `libusb` plus the current audio experiment:

```text
IOKit
CoreFoundation
Security
AudioToolbox
```

Using static libraries avoids the `@rpath` and unsigned local `.dylib` problems that can otherwise appear when running directly from Xcode.

## 4. Expected RTL-SDR output

A successful start should contain output similar to:

```text
RTL-SDR devices found: 1
Device 0: Generic RTL2832U OEM
Found Rafael Micro R828D tuner
RTL-SDR Blog V4 Detected
RTL-SDR opened.
```

The verified I/Q example additionally prints the configured frequency/sample rate and a continuously updated relative digital power level.

## Source layout

```text
CWMacTest.xcodeproj/   Xcode project
CWMacTest/main.c       current experimental CW/audio receiver
examples/iq_stream.c  minimal verified RTL-SDR I/Q example
```

## Current CW experiment

`CWMacTest/main.c` currently contains:

- 2.4 MS/s RTL-SDR input
- 48 kHz audio target rate
- simple integrate-and-dump decimation
- 700 Hz CW pitch
- a simple biquad CW band-pass stage
- AudioQueue output on macOS

The DSP is intentionally still simple. A production-quality receiver should use a proper anti-alias FIR/polyphase decimator, better gain control, spectrum/waterfall support, and a tested audio/device-selection path.

## Notes on RTL-SDR Blog V4

Use the RTL-SDR Blog driver fork for V4 support:

https://github.com/rtlsdrblog/rtl-sdr-blog

That repository includes specific support for the R828D-based RTL-SDR Blog V4 and is licensed under GPL-2.0.

## License

No license has been selected for this example repository yet. Because the program links against GPL-2.0 `librtlsdr`, review the licensing implications before distributing binaries. If the intention is to publish this as an open-source example, GPL-2.0 is the simplest compatible choice.

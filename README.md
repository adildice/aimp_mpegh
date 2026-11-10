# aimp_mpegh

MPEG-H 3D Audio decoder plugin for [AIMP](https://www.aimp.ru) 5.x (Windows, 64-bit).

Plays MPEG-H 3D Audio tracks (`mha1` / `mhm1`) stored in MP4 / M4A files, for example
360 Reality Audio music. Files that are not MPEG-H are ignored, so AIMP keeps playing
them with its own decoders.

> Status: works in AIMP 5.x (64-bit). Decoding and seeking are also verified against
> Fraunhofer's reference decoder through a small test host (`test/test_host.cpp`).

## Install

1. Download `aimp_mpegh_plugin.zip` from [Releases](../../releases).
2. In AIMP open *Preferences → Plugins → Install*, pick the zip and restart AIMP.
3. *Preferences → Plugins* should now list "MPEG-H 3D Audio decoder".

If an MPEG-H file has a wrong extension (for example `.flac`), rename it to `.m4a`.

## Settings

In AIMP open *Preferences → Plugins*, select "MPEG-H 3D Audio decoder" and open its settings.
There you choose:

- **Output layout:** Stereo, 5.1, 7.1, 9.1.6 or Mono
- **Loudness target**
- **Dynamic range control**

Changes apply to the next track you start. Surround layouts need a sound device set up for
that many channels. Stereo, 5.1 and 7.1 are confirmed working in AIMP; `9.1.6` outputs 16
channels (L R C LFE Lb Rb Ls Rs Lw Rw Tfl Tfr Trl Trr Tml Tmr) and is untested in AIMP.

Your choices are saved automatically; there is no file you need to edit. (For the curious: they
live in `aimp_mpegh.ini`, usually in AIMP's profile folder, `%AppData%\AIMP`.)

## Limits

- MPEG-H Baseline / Low Complexity profile up to level 4.
- Output is 32-bit PCM at the stream's sample rate.
- No raw `.mhas` files, no DRM-protected streams, no cover art.

## Build (Linux, cross-compiling with mingw-w64)

```sh
sudo apt install g++-mingw-w64-x86-64-posix cmake
mkdir deps && cd deps
git clone https://github.com/Fraunhofer-IIS/mpeghdec.git
cmake -S mpeghdec -B build -DCMAKE_TOOLCHAIN_FILE=../mingw.cmake \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
cmake --build build -j8     # if ilo fails: apply ../ilo-mingw.patch in build/_deps/ilo-src, rebuild
cd ..
# put the AIMP SDK C++ headers (apiPlugin.h, apiDecoders.h, ...) into ./aimp_sdk
sh build.sh                 # -> out/aimp_mpegh.dll, out/test_host.exe
```

`test_host.exe aimp_mpegh.dll file.m4a out.wav` loads the DLL through the AIMP SDK interfaces,
decodes the file to WAV and runs seek checks.

## Licences

- The plugin's own code (`src/`, `test/`, build files) is MIT licensed, see [LICENSE](LICENSE).
- The released DLL statically links the Fraunhofer FDK MPEG-H Software
  ([mpeghdec](https://github.com/Fraunhofer-IIS/mpeghdec)),
  [mmtisobmff](https://github.com/Fraunhofer-IIS/mmtisobmff) and
  [ilo](https://github.com/Fraunhofer-IIS/ilo), © Fraunhofer-Gesellschaft, under their own
  licences in [third_party_licenses](third_party_licenses). The binary is a "Third-Party
  Modified Version of the Fraunhofer FDK MPEG-H Software": one ilo source file received extra
  `#include` lines (November 2026, see `ilo-mingw.patch`) so that it compiles with mingw-w64.
- The Fraunhofer licence grants **no patent licence**. Use the binary for personal,
  non-commercial purposes unless you hold the relevant MPEG-H patent licences.

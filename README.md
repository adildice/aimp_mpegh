# aimp_mpegh

MPEG-H 3D Audio decoder plugin for [AIMP](https://www.aimp.ru) 5.x (Windows, 64-bit and 32-bit).

Plays MPEG-H 3D Audio tracks (`mha1` / `mhm1`) stored in MP4 / M4A files, for example
360 Reality Audio music. With the **Headphones (3D)** output, the full 3D mix is rendered for
headphones in the style of 360 Reality Audio. Files that are not MPEG-H are ignored, so AIMP
keeps playing them with its own decoders.

> Status: works in AIMP 5.x (32-bit and 64-bit). Decoding and seeking are also verified against
> Fraunhofer's reference decoder through a small test host (`test/test_host.cpp`).

## Install

1. Download the `.aimppack` file (or the `.zip`, same contents) from the latest
   [release](../../releases/latest).
2. In AIMP open *Preferences → Plugins → Install*, pick the file and restart AIMP.
3. *Preferences → Plugins* should now list "MPEG-H 3D Audio decoder".

The package contains the 32-bit and the 64-bit plugin; AIMP picks the right one.

If an MPEG-H file has a wrong extension (for example `.flac`), rename it to `.m4a`.

## Settings

In AIMP open *Preferences → Plugins*, select "MPEG-H 3D Audio decoder" and open its settings.
There you choose:

- **Output layout:**
  - **Headphones (3D)** (default): the 3D mix (7.1.4, including height) is placed around your
    head for headphones. Works with any sound device.
  - **Stereo (2.0, speakers)**: plain two-speaker downmix.
  - 5.1, 7.1, 9.1.6 or Mono.
- **Room sound (3D):** how much room is added in Headphones (3D), from Off (dry) to Maximum.
- **Loudness target**
- **Dynamic range control**
- **Language** of the dialog: 44 languages; "Automatic" follows AIMP's interface language.
- **Read tags:** untick it if titles or artists look wrong, and AIMP reads the tags itself.

Changes apply to the next track you start. Surround layouts need a sound device set up for
that many channels. Stereo, 5.1 and 7.1 are confirmed working in AIMP; `9.1.6` outputs 16
channels (L R C LFE Lb Rb Ls Rs Lw Rw Tfl Tfr Trl Trr Tml Tmr) and is untested in AIMP.

Your choices are saved automatically; there is no file you need to edit. (For the curious: they
live in `aimp_mpegh.ini`, usually in AIMP's profile folder, `%AppData%\AIMP`.)

## Translations

The dialog texts are in [`Langs/`](Langs) (installed to `Plugins\aimp_mpegh\Langs`), one UTF-8
file per language. To fix a translation, edit the text after `=` and keep the keys on the left.
To add a language, copy `english.ini` under a new name and fill in `Name` (shown in the list)
and `AIMP` (AIMP's name for the language, used by "Automatic"). Lines left out appear in English.
Changes show the next time the settings dialog opens; no rebuild is needed.

The files are generated from [`tools/langs_data.py`](tools/langs_data.py) with
`python tools/gen_langs.py "C:\Program Files\AIMP\Langs"`. If you add a text to the dialog,
add it there and regenerate. Corrections from native speakers are welcome as pull requests.

## Limits

- MPEG-H Baseline / Low Complexity profile up to level 4.
- Output is 32-bit PCM at the stream's sample rate.
- Headphones (3D) uses a general head model, not Sony's own (unpublished) one, so it sounds
  close to, but not identical with, Sony's 360 Reality Audio on headphones.
- The dialog is not mirrored for right-to-left languages (Arabic, Hebrew, Persian).
- No raw `.mhas` files, no DRM-protected streams, no cover art.

## Build (Windows, Visual Studio 2022 Build Tools)

Needs the "Desktop development with C++" workload (MSVC, CMake and Ninja are included) and Git.

```bat
git clone https://github.com/Fraunhofer-IIS/mpeghdec.git C:\mhb\mpeghdec
rem put the AIMP SDK C++ headers (apiPlugin.h, apiDecoders.h, apiMUI.h, ...) into .\aimp_sdk
build_msvc.bat            &rem -> out\x64 and out\x86: aimp_mpegh.dll, test_host.exe
```

Use a short folder such as `C:\mhb` for mpeghdec: its build downloads ilo and mmtisobmff and
fails on long paths. `build_msvc.bat` builds the Fraunhofer libraries for both architectures
the first time, then the plugin. The 32-bit DLL exports `AIMPPluginGetHeader` without
decoration, as AIMP expects.

`test_host.exe aimp_mpegh.dll file.m4a out.wav` loads the DLL through the AIMP SDK interfaces,
decodes the file to WAV and runs seek checks. `test/test_binaural.cpp` checks the headphone
renderer on its own.

AIMP package layout:

```
aimp_mpegh/aimp_mpegh.dll        32-bit
aimp_mpegh/x64/aimp_mpegh.dll    64-bit
aimp_mpegh/Langs/*.ini           dialog translations, shared by both
aimp_mpegh/README.txt, LICENSE-*.txt
```

### Build (Linux, cross-compiling with mingw-w64)

Versions up to 1.1 were built this way. It is untested with the current sources.

```sh
sudo apt install g++-mingw-w64-x86-64-posix g++-mingw-w64-i686-posix cmake
mkdir deps && cd deps
git clone https://github.com/Fraunhofer-IIS/mpeghdec.git
# 64-bit libraries -> deps/build, 32-bit libraries -> deps/build32
cmake -S mpeghdec -B build   -DCMAKE_TOOLCHAIN_FILE=../mingw.cmake   -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
cmake -S mpeghdec -B build32 -DCMAKE_TOOLCHAIN_FILE=../mingw32.cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
cmake --build build -j8 && cmake --build build32 -j8
# if ilo fails to compile: apply ../ilo-mingw.patch in build/_deps/ilo-src (and build32/_deps/ilo-src), rebuild
cd ..
# put the AIMP SDK C++ headers (apiPlugin.h, apiDecoders.h, apiMUI.h, ...) into ./aimp_sdk
sh build.sh                 # -> out/x64 and out/x86: aimp_mpegh.dll, test_host.exe
```

## Licences

- The plugin's own code (`src/`, `test/`, `tools/`, `Langs/`, build files) is MIT licensed, see
  [LICENSE](LICENSE).
- The released DLL statically links the Fraunhofer FDK MPEG-H Software
  ([mpeghdec](https://github.com/Fraunhofer-IIS/mpeghdec)),
  [mmtisobmff](https://github.com/Fraunhofer-IIS/mmtisobmff) and
  [ilo](https://github.com/Fraunhofer-IIS/ilo), © Fraunhofer-Gesellschaft, under their own
  licences in [third_party_licenses](third_party_licenses). Since version 1.2 the release is
  built with Microsoft Visual C++ from the unmodified Fraunhofer sources. (Versions up to 1.1
  were a "Third-Party Modified Version of the Fraunhofer FDK MPEG-H Software": one ilo source
  file received extra `#include` lines so that it compiled with mingw-w64, see `ilo-mingw.patch`.)
- The Fraunhofer licence grants **no patent licence**. Use the binary for personal,
  non-commercial purposes unless you hold the relevant MPEG-H patent licences.

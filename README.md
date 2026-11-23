# Switchname Engine

**Switchname Engine** is a homebrew port of [Codename Engine](https://github.com/CodenameCrew/CodenameEngine) (a Friday Night Funkin' engine) for the **Nintendo Switch**. It runs Codename Engine mods, scripts and shaders on the console, using a renderer written for the Switch's own graphics API.

> [!IMPORTANT]
> Switchname Engine is a **sub-engine of Codename Engine**. Almost everything here is the work of the [Codename Crew](https://github.com/CodenameCrew) and the [Codename Engine contributors](https://github.com/CodenameCrew/CodenameEngine/graphs/contributors). This repository adds the Switch port on top. Please support the original project: [codename-engine.com](https://codename-engine.com/).

> [!NOTE]
> This is unofficial homebrew. It needs a Switch running custom firmware (Atmosphère). It is not affiliated with or endorsed by Nintendo, the Funkin' Crew or the Codename Crew.

---

## What the port adds

- **deko3d renderer ("dkgl")**: a GLES2-style OpenGL layer written directly on [deko3d](https://github.com/devkitPro/deko3d), the Switch's native graphics API, instead of Mesa/nouveau. Shader-heavy songs run much faster.
  - Mod GLSL shaders are compiled on the console with [UAM](https://github.com/devkitPro/uam), cached on the SD card, and only compiled once.
  - Very heavy shader effects render at a lower resolution that adjusts to the GPU's frame time, then get scaled back up.
  - Shader effects that draw nothing visible are skipped automatically.
  - Giant textures (6144 px or larger) are stored at half resolution, and memory is checked before large allocations, so big mods are less likely to crash.
- **Background character preloading**: characters swapped in by chart events load on a worker thread while the song plays, which cuts down on mid-song stutters.
- **Controller support**: full Joy-Con/Pro Controller bindings, an on-screen keyboard and a software cursor.
- **Video cutscenes** through FFmpeg (replaces hxvlc/libVLC).
- **Switch options**: screen-effect resolution, botplay, stick dead zone, and per-mod custom-menu toggles.
- **Compatibility fixes for older mods**: fallbacks for classes that moved between Codename versions, older Animate-atlas symbols, and more.
- **OpenGL fallback build**: `SwitchnameEngine-GL.nro`, using Mesa, in case the deko3d build has trouble with a mod.

## Installing

1. Download from the [latest release](https://github.com/Imnot-Ha3kin6/Switchname-Engine/releases/latest):
   - `SwitchnameEngine.nro` is the main build, using the native deko3d renderer. It's the fastest, especially with shaders. It shows up in the homebrew menu as **Switchname Engine**.
   - `SwitchnameEngine-GL.nro` is the OpenGL (Mesa) fallback build, for when a mod looks wrong on the main build. It shows up as **Switchname Engine (OpenGL)**.
2. Copy one or both to `sdmc:/switch/` on your SD card. They can be installed side by side and share the same mods folder.
3. Put your mods in `sdmc:/switch/CodenameEngine/mods/` and addons in `sdmc:/switch/CodenameEngine/addons/`.
4. Launch it from the homebrew menu in **full-memory mode**: hold **R** while opening a game, or use a forwarder. Applet mode (the Album) does not have enough memory.

Files the game creates on the SD card:

| Path | What it is |
| --- | --- |
| `sdmc:/switch/CodenameEngine/shadercache/` | compiled shaders. Safe to delete; they are rebuilt on the next run. |
| `sdmc:/switch/CodenameEngine/screenshots/` | in-game screenshots |
| `sdmc:/switch/cne-trace.log`, `sdmc:/switch/dkgl.log` | logs, useful for bug reports |

The first time a song uses a shader, compiling it causes a short hitch. After that, it loads from the cache.

## Building

The build runs on Windows with devkitPro. Linux and macOS should work with the same steps, but they have not been tested.

### 1. Tools

- [Haxe 4.3.x](https://haxe.org/download/) (developed with 4.3.7)
- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with devkitA64 and these packages:

  ```sh
  pacman -S switch-dev switch-portlibs deko3d switch-mesa switch-glad switch-ffmpeg switch-dav1d \
    switch-libopus switch-opusfile switch-mpg123 switch-libvorbisidec switch-libmodplug switch-libvorbis \
    switch-libogg switch-harfbuzz switch-freetype switch-libpng switch-libjpeg-turbo switch-libwebp \
    switch-zlib switch-bzip2
  ```
- Git, plus `bison`, `flex` and `make` from devkitPro's msys2. These are only needed to rebuild UAM.

### 2. Haxe libraries

First, install Codename Engine's normal libraries (see [building/README.md](building/README.md)):

```sh
haxelib --always run lime setup
building/setup-windows.bat
```

Then point the core libraries at the exact commits this port was made against, and apply the patches from [`patches/`](patches):

| Library | Repository @ commit | Patch |
| --- | --- | --- |
| lime | [CodenameCrew/cne-lime](https://github.com/CodenameCrew/cne-lime) @ `60c83466` | `lime.patch` (adds the `switch` target, its NRO makefile template and the rebuilt `tools.n`) |
| lime → `project/lib/sdl` | [FunkinCrew/SDL](https://github.com/FunkinCrew/SDL) @ `c329016c` | `lime-submodule-sdl.patch` (Switch audio fix) |
| lime → `project/lib/hashlink` | HaxeFoundation/hashlink @ `03110f7e` | `lime-submodule-hashlink.patch` |
| hxcpp | [Slushi-Github/hxcpp-nx](https://github.com/Slushi-Github/hxcpp-nx) @ `603063c6` | `hxcpp-nx.patch` |
| openfl | [CodenameCrew/cne-openfl](https://github.com/CodenameCrew/cne-openfl) @ `74f5f972` | `openfl.patch` |
| flixel | [CodenameCrew/cne-flixel](https://github.com/CodenameCrew/cne-flixel) @ `e05b0d0a` | `flixel.patch` |
| flixel-animate | [CodenameCrew/cne-flixel-animate](https://github.com/CodenameCrew/cne-flixel-animate) @ `49cd4753` | `flixel-animate.patch` |

`lime-nx.patch` is for reference only. The Switch platform code from [Slushi-Github/lime-nx](https://github.com/Slushi-Github/lime-nx) @ `e19e2d4e` is already built into the patched lime `tools.n`.

Example for one library:

```sh
git clone https://github.com/CodenameCrew/cne-openfl openfl-switch
cd openfl-switch
git checkout 74f5f972
git apply --whitespace=nowarn ../Switchname-Engine/patches/openfl.patch
haxelib dev openfl .
```

Do the same for the others. For lime, also run `git submodule update --init` before applying the submodule patches.

### 3. Native libraries

The NRO links against static Switch builds of lime (`liblime.a`), OpenAL Soft and SDL3, plus the deko3d renderer. The easy route is the **`switchname-prebuilt-libs.zip`** from the release. Extract it next to this repo:

```
prebuilt/
  lime/      liblime.a            -> copy into <lime>/ndll/Switch/
  openal/lib/libopenal.a          (OpenAL Soft 1.25.1, lime's openal submodule, SDL3 backend)
  sdl3/lib/libSDL3.a              (lime's SDL submodule with the patch above, static, Release)
  dkgl/libdkgl.a                  (deko3d renderer)
  uam/libuam.a                    (on-console shader compiler)
```

To build them yourself instead:

- **lime**: `haxelib run lime rebuild switch`
- **SDL3 / OpenAL**: configure lime's `project/lib/sdl` and `project/lib/openal` with CMake, using `-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake`. Use static libraries. For OpenAL, set `ALSOFT_BACKEND_SDL3=ON` with the other backends off, and `ALSOFT_EMBED_HRTF_DATA=ON`.
- **deko3d renderer**: see [deko3d/README.md](deko3d/README.md). Short version: `make -C deko3d/uam-switch -j8 && make -C deko3d/dkgl -j8`.

### 4. Build the NRO

Point the build at those libraries with these environment variables, then build with `-Dswitch_deko3d`:

```powershell
$env:DEVKITPRO  = "C:/devkitPro"
$env:DEVKITA64  = "C:/devkitPro/devkitA64"
$P = "C:/path/to/prebuilt"   # forward slashes
$env:CNE_OPENAL_DIR = "$P/openal"
$env:CNE_SDL3_DIR   = "$P/sdl3"
$env:CNE_GL_LIBS    = "-L$P/dkgl -ldkgl -L$P/uam -luam -ldeko3d"
haxelib run lime build switch -nocffi -Dswitch_deko3d
```

The output is `export/release-dk/switch/bin/SwitchnameEngine.nro`.

- If you leave out `-Dswitch_deko3d` and `CNE_GL_LIBS`, you get the Mesa/OpenGL build instead (`export/release/switch/bin/SwitchnameEngine-GL.nro`).
- If only native libraries changed, delete the `.elf` in the `bin` folder to force a relink.

## Project layout

| Path | What it is |
| --- | --- |
| `source/` | Codename Engine source with the Switch changes (marked `#if switch`) |
| `source-switch/` | Switch-only code: character preloader, keyboard, cursor, screenshots, watchdog, FFmpeg video player |
| `deko3d/dkgl/` | the deko3d OpenGL/EGL layer |
| `deko3d/uam-switch/`, `deko3d/uam/`, `deko3d/uam-gen/` | UAM built as a Switch library (the vendored UAM keeps its own license) |
| `patches/` | changes to lime, hxcpp, openfl, flixel and flixel-animate |

## Version

Switchname Engine's version is set in `Flags.SWITCHNAME_VERSION` (shown in the main menu) and in `CNE_APP_VERSION` in `project.xml` (shown in the homebrew menu). The engine still reports Codename Engine's own version to mods, so mod compatibility checks work as usual.

## License and credits

Codename Engine and this port are licensed under the [Apache License 2.0](LICENSE). This follows the Codename Engine usage guidelines for sub-engines: credit stays with the Codename Crew, and the in-game credits menu is unchanged.

- **Codename Engine**: the [Codename Crew](https://github.com/CodenameCrew) and [all contributors](https://github.com/CodenameCrew/CodenameEngine/graphs/contributors)
- **Friday Night Funkin'**: [ninjamuffin99](https://github.com/ninjamuffin99) and the [Funkin' Crew](https://github.com/FunkinCrew)
- **lime-nx / hxcpp-nx** (Haxe on the Switch): [Slushi-Github](https://github.com/Slushi-Github)
- **deko3d, UAM, libnx, devkitA64**: [devkitPro](https://github.com/devkitPro) and [fincs](https://github.com/fincs). UAM is based on Mesa (MIT) and keeps its license in `deko3d/uam/LICENSE`.
- **flixel-animate**: [MaybeMaru](https://github.com/MaybeMaru/flixel-animate)
- **HaxeFlixel, OpenFL, Lime, Haxe**: their respective teams
- **Switch port**: [Imnot-Ha3kin6](https://github.com/Imnot-Ha3kin6)

The original Codename Engine README is kept as [README-CodenameEngine.md](README-CodenameEngine.md).

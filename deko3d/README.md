# deko3d renderer (dkgl)

`dkgl` lets Lime/OpenFL draw on the Switch with **deko3d**, the console's native graphics API, instead of Mesa. It implements the OpenGL ES 2-class functions OpenFL uses, plus the EGL calls SDL makes. Lime loads GL through `eglGetProcAddress`, so the library only needs to export EGL. Any GL function it doesn't implement gets a stub that logs its name.

| Folder | What it is |
| --- | --- |
| `dkgl/` | the GL/EGL layer (`libdkgl.a`) |
| `uam-switch/` | builds [UAM](https://github.com/devkitPro/uam) (deko3d's GLSL compiler) as a Switch library (`libuam.a`) with a `uam_compile()` entry point |
| `uam/` | UAM at devkitPro/uam@`5a5afc2`, changed in `source/compiler_iface.{h,cpp}` so it can write DKSH to a `FILE*` (memory stream). It keeps its own license (`uam/LICENSE`). |
| `uam-gen/` | UAM's generated GLSL parser and lexer (bison/flex) and the `ir_expression_operation*.h` headers, so the build doesn't need bison, flex or Python |
| `uam-sources.txt` | UAM's source list, as used by `uam-switch/Makefile` |

## Building

From devkitPro's msys2 shell, with `DEVKITPRO` set:

```sh
make -C uam-switch -j8     # -> uam-switch/libuam.a
make -C dkgl -j8           # -> dkgl/libdkgl.a
```

Link the game with `-L<here>/dkgl -ldkgl -L<here>/uam-switch -luam -ldeko3d`. That is the `CNE_GL_LIBS` setting described in the main README.

## How it works

- **Shaders**: OpenFL/Codename GLSL (ES 1.00) is rewritten to GLSL 4.60:
  - inputs and outputs get explicit locations
  - loose uniforms are packed into one std140 block shared by the vertex and fragment stages
  - samplers get bindings
  - `gl_FragCoord` is corrected for render scale and offset
  - names that clash with built-ins are renamed

  The result is compiled with UAM and cached as `sdmc:/switch/CodenameEngine/shadercache/<hash>.dksh`.
- **Buffers**: every buffer keeps a CPU copy. Writes to a buffer the GPU is still reading go into that frame's ring memory, and a GPU copy brings them back at the start of the next frame.
- **Textures**:
  - Uploads go through the frame ring. Very large ones stream through a reusable 16 MB buffer.
  - Images 6144 px or larger are stored at half resolution. Shaders sample in 0..1 coordinates, so nothing else has to change.
  - Textures 4096 px or larger get automatic mipmaps when there is memory to spare.
- **Heavy fragment shaders** (more than 12 KB of shader code) are drawn in two passes: into an off-screen layer at reduced resolution, then composited with the original blending. The resolution follows GPU timestamps: it drops when the GPU takes over 15 ms per frame and goes back up below 11.5 ms. If an effect draws no visible pixels for 30 frames, it is skipped and re-checked every 15 frames.
- **Memory**: deko3d aborts the process when an allocation fails, so blocks of 4 MB or more are test-allocated first, and the layer fails softly if memory is short.
- Logs go to `sdmc:/switch/dkgl.log`.

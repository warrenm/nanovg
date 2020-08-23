**Fair warning: This project is a hard fork of the original NanoVG. It does not contribute upstream, it is not cross-platform, and it has no vestiges of the OpenGL API. Do not use this project unless you are only interested in a high-performance vector graphics rendering library for Metal on Apple platforms only.**

NanoVG
==========

NanoVG is small antialiased vector graphics rendering library ~for OpenGL~. It has lean API modeled after HTML5 canvas API. It is aimed to be a practical and fun toolset for building scalable user interfaces and visualizations.

## Screenshot

![screenshot of some text rendered with the sample program](/example/screenshot-01.png?raw=true)

Usage
=====

The NanoVG API is modeled loosely on HTML5 canvas API. If you know canvas, you'll be up to speed with NanoVG in no time.

## Creating drawing context

The drawing context is created using a platform-specific constructor function. If you're using the Metal backend, the context is created as follows:
```C
#define NANOVG_METAL_IMPLEMENTATION	// Use Metal implementation.
#include "nanovg_metal.h"
...
struct NVGcontext* vg = _vg = nvgCreateMetal(mtlDevice, mtlCommandQueue, NVG_ANTIALIAS | NVG_STENCIL_STROKES | NVG_DEBUG);;
```

The last parameter defines flags for creating the renderer.

- `NVG_ANTIALIAS` means that the renderer adjusts the geometry to include anti-aliasing. If you're using MSAA, you can omit this flags. 
- `NVG_STENCIL_STROKES` means that the render uses better quality rendering for (overlapping) strokes. The quality is mostly visible on wider strokes. If you want speed, you can omit this flag.

*NOTE:* The frame buffer you render to must have exactly one color attachment (of format `MTLPixelFormatBGRA8Unorm`) and a stencil attachment of format `MTLPixelFormatStencil8`.

## Drawing shapes with NanoVG

Drawing a simple shape using NanoVG consists of four steps: 1) begin a new shape, 2) define the path to draw, 3) set fill or stroke, 4) and finally fill or stroke the path.

```C
nvgBeginPath(vg);
nvgRect(vg, 100,100, 120,30);
nvgFillColor(vg, nvgRGBA(255,192,0,255));
nvgFill(vg);
```

Calling `nvgBeginPath()` will clear any existing paths and start drawing from blank slate. There are number of number of functions to define the path to draw, such as rectangle, rounded rectangle and ellipse, or you can use the common moveTo, lineTo, bezierTo and arcTo API to compose the paths step by step.

## Understanding Composite Paths

Because of the way the rendering backend is build in NanoVG, drawing a composite path, that is path consisting from multiple paths defining holes and fills, is a bit more involved. NanoVG uses even-odd filling rule and by default the paths are wound in counter clockwise order. Keep that in mind when drawing using the low level draw API. In order to wind one of the predefined shapes as a hole, you should call `nvgPathWinding(vg, NVG_HOLE)`, or `nvgPathWinding(vg, NVG_CW)` _after_ defining the path.

``` C
nvgBeginPath(vg);
nvgRect(vg, 100,100, 120,30);
nvgCircle(vg, 120,120, 5);
nvgPathWinding(vg, NVG_HOLE);	// Mark circle as a hole.
nvgFillColor(vg, nvgRGBA(255,192,0,255));
nvgFill(vg);
```

## Rendering is wrong, what to do?

- make sure you have created NanoVG context using one of the `nvgCreatexxx()` calls
- make sure you have initialized your Metal view with a *stencil buffer*
- make sure you have cleared the stencil buffer
- make sure you precede your `nvgBeginFrame()` call with a call to `nvgBeginFrameMetal(...)` to provide your per-frame rendering objects (command buffer and render command encoder).
- make sure all rendering calls happen between `nvgBeginFrame()` and `nvgEndFrame()`
- if the problem still persists, please report an issue!

## API Reference

See the header file [nanovg.h](/src/nanovg.h) for API reference.

## Ports

- [DX11 port](https://github.com/cmaughan/nanovg) by [Chris Maughan](https://github.com/cmaughan)
- An earlier [Metal port](https://github.com/ollix/MetalNanoVG) by [Olli Wang](https://github.com/olliwang)
- [bgfx port](https://github.com/bkaradzic/bgfx/tree/master/examples/20-nanovg) by [Branimir Karadžić](https://github.com/bkaradzic)

## Projects using NanoVG

- [Processing API simulation by vinjn](https://github.com/vinjn/island/blob/master/examples/01-processing/sketch2d.h)
- [NanoVG for .NET, C# P/Invoke binding](https://github.com/sbarisic/nanovg_dotnet)

## License
The library is licensed under [zlib license](LICENSE.txt)
Fonts used in examples:
- Roboto licensed under [Apache license](http://www.apache.org/licenses/LICENSE-2.0)
- Entypo licensed under CC BY-SA 4.0.
- Noto Emoji licensed under [SIL Open Font License, Version 1.1](http://scripts.sil.org/cms/scripts/page.php?site_id=nrsi&id=OFL)

## Discussions
[NanoVG mailing list](https://groups.google.com/forum/#!forum/nanovg)

## Links
Uses [stb_truetype](http://nothings.org) (or, optionally, [freetype](http://freetype.org)) for font rendering.
Uses [stb_image](http://nothings.org) for image loading.

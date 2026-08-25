# KCD2 async reprojection integration

KCD2 is the primary live-validation target for async reprojection.

## Camera acquisition (implemented, awaiting live validation)

`framegen/reproj/Kcd2Camera.{h,cpp}` detects `WHGame.dll`, scans only that module for the known `CCamera::UpdateFrustumPlanes` signatures, and installs a read-only Detours hook. The hook accepts only cameras embedded at `CView + 0xE8`, validated through MSVC RTTI (`.?AVCView@@`), thereby rejecting shadow/reflection/portal cameras. It publishes the rendered camera Matrix34 position/basis and vertical FOV through a seqlock. Packet capture uses this pose when normal upscaler camera callbacks are absent.

This is based on tkhquang's MIT-licensed KCD2Tools/TPVCamera reverse engineering. Preserve its license attribution if substantial source is copied; the OptiScaler implementation currently reproduces only the documented signatures, offsets, matrix layout, and RTTI gate.

Success criteria in telemetry: `camera=N/N`, `mode.rotation` or `mode.depth` rather than `camera=0/N mode.mv=N` during gameplay. Validate camera cuts, menus, dialogue, horseback, lockpicking, and loading transitions.

## Stable UI research

KCD2 uses Scaleform GFx (`Libs/UI/hud.gfx` plus DDS atlases). CryEngine submits Scaleform as a distinct late overlay stage, but normally draws it directly into the active final color target rather than retaining one reusable HUD texture.

Preferred approaches, in order:

1. **Existing hudless discovery:** inspect OptiScaler's detected HUDless resources with `DrawUIOverFG=true`. If KCD2 already leaves a pre-Scaleform final-color texture, use the existing `HudlessColor + UIColor` reprojection composition path.
2. **D3D12 command-list interception:** identify the transition/copy from final scene color to the swapchain immediately before Scaleform draws. Capture that pre-UI image as HudlessColor, then derive UIColor as the difference/composition between final and hudless. This is less invasive than hooking private Scaleform objects.
3. **CryEngine Scaleform hook:** signature-find `CScaleformPlayback::PushOutputTarget` or the UI playback submission, redirect to a transparent RGBA target (with compatible stencil for masks), then composite after every warped output. This is most exact but patch-sensitive.

A `.gfx` mod can reposition or hide elements but cannot by itself redirect the renderer to a separate UI texture. Any UI extraction must preserve premultiplied-alpha behavior, subtitles, interaction prompts, damage overlays, menus, and cursor.

## Overscan / guard-band research

The desired result is not merely a higher ordinary FOV. It is a guard-band render: preserve the original center pixel-to-angle scale, expand the camera frustum and render dimensions, warp that larger image, then crop to the monitor. For symmetric padding fraction `g`:

```
overscanWidth  = outputWidth  * (1 + 2g)
overscanHeight = outputHeight * (1 + 2g)
overscanVFov   = 2 * atan(tan(originalVFov / 2) * overscanHeight / outputHeight)
```

KCD2's camera hook exposes the live vertical FOV at `CCamera + 0x30`; TPVCamera also documents cull-edge fields at `+0x50/+0x58/+0x60/+0x68/+0x70`. Changing only FOV without matching render dimensions changes center magnification and is not a true guard band. Changing swapchain dimensions is also insufficient because KCD2 renders internally before upscaling.

Implementation investigation order:

1. Hook KCD2's dynamic-resolution/render-resolution selection and request the guard-band dimensions while reducing its quality scale so total pixel cost stays bounded.
2. Expand `CCamera` FOV and cull edges by the exact same guard-band factor before frustum construction.
3. Ensure projection, depth, motion vectors, jitter, TAA/DLSS inputs, and screen-space effects all use the expanded viewport.
4. Teach reprojection to output only the centered crop. HUD should remain output-resolution and be composited after cropping.

For roughly constant pixel cost, if each axis grows by `(1 + 2g)`, multiply the prior linear render scale by `1 / (1 + 2g)`. Example: 10% padding on every side grows each axis to 1.2x; reduce the prior linear scale to 0.833x, yielding approximately the original pixel count.

Risks: KCD2 may derive render resolution in several paths; wider FOV can expose LOD/culling/pop-in; screen-space effects may assume output bounds; DLSS jitter and MV scales must follow the guard-band render dimensions. Implement only after camera acquisition is validated.

## Sources

- https://github.com/tkhquang/KCD2Tools
- https://raw.githubusercontent.com/tkhquang/KCD2Tools/main/TPVCamera/src/hooks/camera_hook.cpp
- https://raw.githubusercontent.com/tkhquang/KCD2Tools/main/TPVCamera/src/aob_resolver.hpp
- https://www.cryengine.com/docs/static/engines/cryengine-5/categories/28704770/pages/29796988
- https://github.com/derplayer/CRYENGINE-5.6.7/blob/release/Code/CryEngine/RenderDll/Scaleform/ScaleformPlayback.cpp

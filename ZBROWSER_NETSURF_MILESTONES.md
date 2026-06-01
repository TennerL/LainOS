# ZBrowser NetSurf Milestones

This is the active browser integration roadmap. Work through it top-to-bottom unless a runtime crash or regression forces a detour.

## 1. Image Fidelity

- [done] Preserve alpha/transparency through the kernel image decode and NetSurf bitmap plot path.
- [done] Render common raster formats in page content: PNG, JPEG, GIF, BMP, ICO, and supported STB formats.
- [done] Add SVG handling or a clear placeholder/fallback path for unsupported SVG content.
- [done] Improve ICO selection so favicons/logos use the best embedded size instead of a rough fallback.
- [done] Use filtered scaling for NetSurf bitmaps so resized page images do not become nearest-neighbor pixel blocks.
- [done] Vendor a real SVG parser path (`libsvgtiny` plus XML parser support) and hand SVG content to NetSurf's vector redraw handler.
- Add WebP support or a predictable server-side fallback strategy for sites that prefer WebP assets.

## 2. Resource Loading

- Replace the current capped synchronous subresource fetch pass with a smarter queue.
- [done] Cache CSS/images/scripts by URL in the kernel NetSurf fetcher so reloads and redraws do not refetch everything.
- [done] Track fetch counts, cache hits, and cache size in browser status/debug output.
- [partial] Avoid wasting the fetch budget on low-value resources before critical CSS/images.

## 3. Font And Layout Fidelity

- Improve font metrics used by NetSurf layout: width, line-height, baseline, and fallback family handling.
- Fix oversized Wikipedia rendering by tuning viewport, default font size, and CSS unit conversions.
- Improve form/input sizing and text rendering inside controls.
- Add targeted regression pages for Wikipedia and the local smoke page.

## 4. Interaction

- Wire links, focused inputs, caret/text entry, selection, hover, and cursor state through the NetSurf frontend path.
- Keep URL bar edits and page interaction from forcing full-browser redraws.
- Add back/forward/reload/stop behavior around the NetSurf content lifecycle.

## 5. Performance

- Make scrolling redraw only newly exposed regions where possible.
- Cache scaled bitmaps for repeated draws and tiled backgrounds.
- Keep page preparation, subresource decode, and expensive layout off the foreground UI path.
- Add lightweight runtime counters for frame time, redraw pixels, fetch time, and decode time.

## 6. Browser Polish

- Improve loading/progress/status display.
- Add clear network/decode/layout error messages in the browser UI.
- Keep history/title/favicon state per page.
- Make the new `zbrowser_netsurf` path the default once stable.

## 7. Compatibility

- Use a small corpus of real pages for regressions: Wikipedia front page, simple article, image-heavy page, form page, and local smoke page.
- Add targeted compatibility fixes only after the NetSurf core path proves the missing behavior is frontend-side.

## 8. Self-Hosted NetSurf Build

- [done] Stage the first browser C build slice into lainfs: the guest-visible queue carries the browser/libdom seed units, while the host queue expands that to 488 module-linkable objects with libdom/parserutils/libcss discovery; the guest staged tree still avoids libcss source filenames that exceed lainfs limits.
- [done] Keep host checks green for browser C staging, queue probing, module linking, `make lainfs-smoke zcc-smoke`, and the broader `zbrowser-compile-smoke`.
- [done] Add a temporary browser compatibility provider so current Z browser manifests continue to link while the real NetSurf frontend/style provider is moved into place.
- [done] Guarantee the seeded live ramdisk appears at `R:` when no persistent system partition mounts, including the normal `./run.sh` ISO path.
- [done] Run the VM selfhost smoke under KVM from the live `R:` ramdisk. The guest boots headless, runs the smoke autoexec from `R:`, sees the staged browser C queue, self-installs `libc_smoke_module`, `clib_port_smoke_module`, `zbrowser_module`, and `zbrowser_netsurf`, and executes the selfhost project twice.
- [partial] Teach the headless QEMU smoke to expose `build/zbrowser-selfhost.data.img` as a mountable lainfs partition. The guest currently detects `sd0`, but partition discovery still does not report `sd0p1` under the ISO/AHCI smoke path, so host-side artifact checks are skipped for the live-ramdisk pass.
- [done] Update the user-facing `examples/autoexec` for the split prebuilt C engine object + `zbrowser_netsurf.Z` manifest so the NetSurf module can be copied from `R:` and installed during desktop startup.
- [done] Bring `browser_c_probe.Z` back into the in-OS compiler subset. The guest compiler now accepts the fuller staged-source probe, reads the 19 KB compile queue, and the selfhost smoke fails if `unsupported .Z syntax` reappears.
- [done] Link the first C browser engine object into `zbrowser_netsurf`: `make` now builds `build/browser-c-engine/zbrowser_engine_module.zo`, seeds it into the ramdisk as `R:/examples/zbrowser_engine_module.zo`, and `zinstall zbrowser_netsurf` consumes it as the installed helper object `zbrowser_netsurf_engine.zo` from inside the OS.
- [done] Replace the raw-byte C renderer with the first CSS-aware C engine pass. It strips HTML markup, skips script/style contents, parses simple `<style>` blocks and inline `style=` color/background declarations, and applies body/link/heading colors while preserving the prebuilt `.zo` module path.
- [done] Stabilize large-page browsing in the split C-engine module by removing idle tick redraws and bounding style/body scan work per render pass.
- [done] Trim the normal live `R:/examples` seed to a user-test manifest instead of the full examples tree. The broad seed remains available to selfhost smokes, while `./run.sh` boots with the files `autoexec` actually installs.
- [done] Teach the browser C queue linker path to avoid host stack-protector runtime dependencies and verify that the 185-unit staged queue module-links against kernel libc/runtime exports.
- [done] Add the first external stylesheet load path for `zbrowser_netsurf`: linked `<link rel=stylesheet href=...>` resources are fetched, appended as synthetic `<style>` blocks, counted in browser status as `css=N`, and handed to the C renderer's CSS parser.
- [done] Extend the interim C renderer from document-level theme colors to a small rule cache for simple `tag`, `.class`, `#id`, and `tag.class` color/background selectors. This proves fetched CSS can influence drawing, but it is still not real NetSurf layout or cascade.
- [done] Make the visible split C-engine renderer tolerate old HTML pages like FrogFind: declarations/malformed tags no longer corrupt the style stack, legacy `body` colors and `font color` are honored, `h3` headings render, and basic text/submit inputs draw as controls.
- Replace `examples/browser_compat_stub.Z` with the real NetSurf/WebStyle frontend provider and keep the compatibility exports as tests rather than production shims.
- [done] Begin replacing the CSS-aware text renderer with real linked parser calls behind the existing `zbrowser_engine_prepare` / `zbrowser_engine_draw` boundary. The staged queue now builds `zbrowser_engine_bridge.c` with `ZBROWSER_ENGINE_ENABLE_DOM`, creates a Hubbub parser, parses the HTML buffer into a libdom document, records the root element name, and module-links the expanded queue.
- [done] Link the first libcss parser slice next to the libdom document checkpoint. The host queue now stages libcss source for linking, parses inline `<style>` blocks with `css_stylesheet_create/append_data/data_done`, and module-links after raising the module object ceiling.
- [done] Link libcss selection next to the libdom/libcss parse checkpoint. The staged bridge now builds a `css_select_ctx`, appends parsed author sheets, matches selectors through libdom callbacks, computes the root style, and exposes root color/background/display through the engine ABI before the visible renderer swap.
- [done] Lift the runtime module loader/linker limits for the NetSurf-sized object set. `zbuild`/`zlink`/`zmod` now allow up to 1024 objects, the shell has larger object/image buffers, host/object conversion limits match, and `zmod target` can expand an installed multi-object `target.zbuild` manifest instead of loading only one `.zo`.
- [done] Generate and seed the first full staged `zbrowser_netsurf` package. The package validates a 489-object module link, installs short lainfs-safe object names (`zc001.zo`...`zc488.zo`), and `zbuild` can pull missing prebuilt objects from `R:/examples` while `autoexec` only copies the small manifest/source pair.
- [done] Keep the desktop usable after multi-object installs. Generated helper objects (`zcNNN.zo`) are hidden from the Start/Modules app catalog, and the NetSurf launcher now loads through the `zbrowser_netsurf.zbuild` manifest instead of the obsolete two-object command.
- [done] Remove the unsafe host callback table from the staged C bridge. The visible DOM/libcss bridge now draws through the imported kernel `draw_text_at_pixel` symbol directly, avoiding poisoned/static callback data when the 489-object NetSurf package is loaded from the guest module image.
- [done] Compile the staged browser C queue with incoming stack realignment. The linked NetSurf/libcss C exports now tolerate calls from Z module code that does not enter with the normal SysV 16-byte stack alignment, avoiding `movaps` general-protection traps in the full package.
- [partial] Swap the visible interim text renderer to the staged NetSurf DOM/libcss bridge. The bridge now exports the normal `zbrowser_engine_*` ABI from the multi-object package and walks libdom text nodes for first document paint.
- [partial] Expand computed-style traversal from root-only to document nodes before layout/paint replacement. The bridge keeps parsed author stylesheets alive, selects libcss style per element during traversal, skips hidden/script/style/head content, and now consumes color/background/display/visibility/font-size/margins/padding/text-align for a visibly more CSS-driven text-flow paint. This is still not full CSS because it does not build NetSurf HTML boxes or run NetSurf layout/redraw yet.
- [partial] Link and stage the NetSurf `content/handlers/html` box construction/layout/redraw path behind the existing browser C package. The selfhost queue now carries NetSurf CSS select/hints/internal plus HTML `box_construct`, `box_inspect`, `box_manipulate`, `box_normalise`, `box_special`, `box_textarea`, `font`, `form`, `forms`, `imagemap`, `object`, `layout`, `layout_flex`, `table`, `redraw_border`, and `redraw`, with frontend/content lifecycle hooks stubbed in the LainOS platform layer.
- [partial] Instantiate a real module-side `html_content`, feed the parsed DOM plus libcss selection context into NetSurf `dom_to_box`, run `layout_document`, and try `html_redraw` first through a LainOS plotter table. The old DOM text-flow painter remains as fallback while the frontend plotters, font metrics, object/content hooks, and runtime stability are hardened.
- Next full-CSS milestone: remove the fallback dependency by completing the NetSurf frontend shims: accurate font metrics, clipping/line/path/bitmap plotters, object content redraw, form widgets, and content lifecycle cleanup.
- Move from "source and queue visible inside the OS" to an in-OS C compile loop that emits at least one NetSurf object from the staged queue.

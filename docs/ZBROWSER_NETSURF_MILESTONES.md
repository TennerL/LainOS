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
- [done] Trim the normal PXE/live ramdisk seed for the full NetSurf package to the package-first path. `zbrowser_netsurf.zpNN`, `zbrowser_netsurf.zbuild`, and the launcher source remain seeded, while the hundreds of intermediate `zcNNN.zo` files are omitted by default to keep the embedded EFI/PXE image smaller for real firmware. Set `ZBROWSER_FULL_SEED_OBJECTS=1` when debugging the guest `zbuild` prebuilt-object fallback.
- [done] Split the staged browser C queue into self-hosting tiers. The generated guest tree now carries `COMPILE_TIER0.txt` through `COMPILE_TIER5.txt` plus `COMPILE_TIERS.txt`, with validation proving all 123 queue units land in exactly one tier and remain host-built/guest-linked until the in-OS C compiler emits browser objects.
- [done] Compile the first staged browser C unit inside LainOS. The guest `zcc` command recognizes staged `libnsutils/src/base64.c`, emits a linkable `.zo` for `nsu_base64_encode`, and the selfhost smoke links/calls it from `first_unit.zbuild`.
- [done] Expand the guest-compiled tier1 slice to the current three libnsutils objects. `zcc` now recognizes staged `libnsutils/src/base64.c`, `time.c`, and `unistd.c`, emits `nsu_base64_encode`, `nsu_getmonotonic_ms`, `nsu_pread`, and `nsu_pwrite`, and the guest `tier1.zbuild` smoke links all three self-hosted objects into one callable binary.
- [done] Start tier2 guest C compilation with parserutils/libwapcaplet leaves. `zcc` recognizes staged `libparserutils/src/charset/encodings/utf8.c` and `libwapcaplet/src/libwapcaplet.c`, emits UTF-8 helpers plus the libwapcaplet intern/string API surface, and the guest `tier2.zbuild` smoke links six self-hosted leaves across libnsutils, parserutils, and libwapcaplet.
- [done] Expand tier3 guest C compilation into libdom strings, namespace helpers, implementation entry points, document front doors, NodeList traversal, and the first HTML form-control/front-door elements. `zcc` recognizes staged `libhubbub/src/utils/errors.c`, `libhubbub/src/utils/string.c`, `libhubbub/src/charset/detect.c`, `libdom/src/core/string.c`, `libdom/src/utils/namespace.c`, `libdom/src/core/implementation.c`, `libdom/src/core/document.c`, `libdom/src/core/nodelist.c`, `libdom/src/html/html_button_element.c`, `libdom/src/html/html_input_element.c`, `libdom/src/html/html_select_element.c`, `libdom/src/html/html_script_element.c`, and `libdom/src/html/html_text_area_element.c`, emits Hubbub utility/charset helpers plus DOM string/QName namespace/implementation/document/NodeList/html-button/html-input/html-select/html-script/html-textarea slices backed by the self-hosted parserutils UTF-8 and libwapcaplet objects, and the guest `tier3.zbuild` smoke links sixteen objects while testing DOM string interning/UTF-8 navigation, namespace URI lookup, QName validation, split-prefix behavior, implementation feature/document validation front doors, document URI/quirks access, document-owned tag and namespace NodeList traversal, item lookup, ref/unref, list equality, HTML button disabled/tab-index/string/form properties, HTML input checked/default/value/size/tab/max-length/form/click properties, HTML select type/selected-index/value/length/disabled/multiple/name/size/tab-index/form/focus properties, HTML script flags/defer/async/text/html-for/event/charset/src/type properties, and HTML textarea disabled/read-only/default/value/cols/rows/tab-index/string/form/select properties.
- [done] Expand tier4 guest C compilation to the first NetSurf utility/desktop set.
  `zcc` recognizes staged `netsurf/utils/bloom.c`, `netsurf/utils/url.c`,
  `netsurf/utils/utils.c`, `netsurf/utils/useragent.c`,
  `netsurf/desktop/mouse.c`, `netsurf/utils/nscolour.c`,
  `netsurf/utils/utf8.c`, `netsurf/utils/punycode.c`,
  `netsurf/utils/hashtable.c`, `netsurf/utils/hashmap.c`,
  `netsurf/utils/time.c`, `netsurf/utils/http/primitives.c`,
  `netsurf/utils/http/generics.c`, `netsurf/utils/http/parameter.c`,
  `netsurf/utils/http/content-type.c`,
  `netsurf/utils/http/content-disposition.c`,
  `netsurf/utils/http/challenge.c`,
  `netsurf/utils/http/www-authenticate.c`,
  `netsurf/utils/http/cache-control.c`, and
  `netsurf/utils/http/strict-transport-security.c`, and
  `netsurf/utils/log.c`; emits the bloom
  create/destroy/search/count surface, `url_escape`/`url_unescape`,
  `squash_whitespace`, `cnv_space2nbsp`, `user_agent_string`,
  `free_user_agent_string`, `browser_mouse_state_dump`, `nscolour_update`,
  `nscolour_get_stylesheet`, the NetSurf UTF-8 wrapper surface over the
  self-hosted parserutils UTF-8 object, `punycode_encode`/`punycode_decode`,
  the plain NetSurf write-once string hashtable API, the callback-backed
  generic hashmap API, the NetSurf time formatting/parsing surface, HTTP
  whitespace/token/quoted-string primitives, HTTP generic item-list
  parse/destroy helpers, HTTP parameter parse/find/iterate/destroy helpers,
  HTTP content-type parse/destroy helpers, HTTP content-disposition
  parse/destroy helpers, HTTP challenge parse/iterate/destroy helpers,
  HTTP WWW-Authenticate parse/destroy helpers, HTTP Cache-Control
  parse/accessor/destroy helpers, and HTTP Strict-Transport-Security
  parse/accessor/destroy helpers, plus NetSurf log init/filter/log/finalise
  helpers; and
  the guest `tier4.zbuild`
  smoke links the parserutils UTF-8 and libwapcaplet dependencies plus all
  twenty-one NetSurf self-hosted objects while testing FNV-backed bloom
  membership, raw hash membership, false negatives, empty strings, item
  counts, percent escaping with slash exceptions, plus-space conversion, valid
  percent decoding, malformed percent preservation, whitespace squashing,
  UTF-8 NBSP conversion, user-agent caching, user-agent rebuild after free,
  the browser mouse-state dump call boundary, nscolour update, stylesheet
  selectors, stylesheet caching, UTF-8 scalar length, byte length, navigation,
  UCS-4 decode, UCS-4 encode, finalisation, Punycode encode/decode vectors,
  small-output handling, invalid-input handling, hashtable add/get,
  duplicate-key precedence, inline plain parsing, invalid inline data,
  bad-parameter handling, hashmap insert/lookup/duplicate replacement,
  callback iteration, removal, missing-key behavior, counts, RFC1123 date
  formatting/parsing, numeric timestamp formatting/parsing, invalid date
  handling, HTTP LWS skipping, token parsing, quoted-string parsing,
  parse-failure handling, HTTP item-list callback parsing, ownership transfer,
  parse cursor advancement, empty-list failure, destructor callbacks, HTTP
  parameter list parsing, unquoted and quoted parameter values,
  case-insensitive parameter lookup, iteration order, bad-parameter failure,
  parameter-list cleanup, content-type parsing, media type interning,
  content-type charset lookup, malformed content-type rejection, and
  content-type cleanup, content-disposition parsing, disposition-token
  interning, quoted boundary parameter lookup, malformed content-disposition
  rejection, content-disposition cleanup, challenge parsing, challenge scheme
  iteration, challenge auth-parameter lookup, malformed challenge rejection,
  challenge-list cleanup, WWW-Authenticate parsing, WWW-Authenticate challenge
  list access, WWW-Authenticate auth-parameter lookup, malformed
  WWW-Authenticate rejection, WWW-Authenticate cleanup, Cache-Control parsing,
  max-age/no-cache/no-store accessors, quoted max-age parsing, duplicate
  directive rejection, invalid max-age handling, malformed Cache-Control
  rejection, Cache-Control cleanup, Strict-Transport-Security parsing,
  required max-age validation, includeSubDomains access, quoted max-age
  parsing, duplicate directive rejection, invalid includeSubDomains value
  rejection, malformed Strict-Transport-Security rejection, and
  Strict-Transport-Security cleanup, log `-v`/`-V` argument handling, ensure
  callback success/failure, filter no-op behavior, log call boundary, and
  finalise cleanup.
- Next full-CSS milestone: remove the fallback dependency by completing the NetSurf frontend shims: accurate font metrics, clipping/line/path/bitmap plotters, object content redraw, form widgets, and content lifecycle cleanup.
- Expand the in-OS C compile loop from hand-lowered tier1/tier2 and first tier3 leaves toward broader small-library slices.

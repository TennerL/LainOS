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

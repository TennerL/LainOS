# Next Steps For Self-Hosting And NetSurf

This is the near-term work order for moving LainOS from a host-built but
guest-visible browser stack toward a genuinely self-hosted NetSurf build.

## 1. Refresh The Truth Baseline

Run the current host and guest smokes against the latest queue:

```bash
scripts/bootstrap-browser-deps.sh
scripts/zbrowser-compile-smoke.sh
scripts/zbrowser-c-queue-module-link-probe.sh
scripts/zbrowser-netsurf-full-package.sh
scripts/zbrowser-selfhost-smoke.sh
```

Goal: confirm the latest staged queue count, full package generation, and guest
smoke are all aligned. The existing serial log looked older than the current
object queue, so a clean baseline should be recorded before deeper work.

## 2. Make One NetSurf C Unit Compile Inside LainOS

Status: done for the first deliberately tiny browser C slice. The guest `zcc`
command currently supports staged `libnsutils/src/base64.c`, lowers it through a
small generated-Z implementation of `nsu_base64_encode`, saves a `.zo`, and
`ztest first_unit` links and calls it inside LainOS.

Follow-up status: the same path now also supports staged `libnsutils/src/time.c`
and `libnsutils/src/unistd.c`. The guest `tier1.zbuild` smoke links the
self-hosted base64, time, and unistd objects together and calls
`nsu_base64_encode`, `nsu_getmonotonic_ms`, `nsu_pread`, and `nsu_pwrite`.

Tier2 status: the current guest-compiled leaves are
`libparserutils/src/charset/encodings/utf8.c` and
`libwapcaplet/src/libwapcaplet.c`. The guest `tier2.zbuild` smoke links the
three libnsutils leaves plus parserutils UTF-8 helpers and libwapcaplet string
interning, exercising ASCII/multibyte length, navigation, UCS-4 conversion,
intern/reintern identity, substring, lowercase, equality, and hash calls.

Tier3 status: the first guest-compiled tier3 leaves are
`libhubbub/src/utils/errors.c`, `libhubbub/src/utils/string.c`,
`libhubbub/src/charset/detect.c`, `libdom/src/core/string.c`, and
`libdom/src/utils/namespace.c`, plus DOM implementation entry points in
`libdom/src/core/implementation.c`, the first document front doors in
`libdom/src/core/document.c`, and the first DOM traversal unit,
`libdom/src/core/nodelist.c`, plus the first HTML form-control front doors in
`libdom/src/html/html_button_element.c` and
`libdom/src/html/html_input_element.c`,
`libdom/src/html/html_select_element.c`, and
`libdom/src/html/html_script_element.c`, and
`libdom/src/html/html_text_area_element.c`. The guest
`tier3.zbuild` smoke links the self-hosted parserutils UTF-8/libwapcaplet
dependencies with these Hubbub/libdom objects and exercises error string
mapping, exact and case-insensitive string matching, content-type charset
parsing, charset fixups, BOM/meta/fallback charset detection, cdata and
interned DOM strings, LWC equality, UTF-8 scalar length/index/at operations,
namespace URI lookup, QName validation, split-prefix behavior, implementation
feature/document validation front doors, document URI/quirks access,
document-owned tag and namespace NodeList traversal, item lookup, ref/unref,
list equality, HTML button disabled/tab-index/string/form properties, and HTML
input checked/default/value/size/tab/max-length/form/click properties, HTML
select type/selected-index/value/length/disabled/multiple/name/size/tab-index/
form/focus properties, HTML script flags/defer/async/text/html-for/event/
charset/src/type properties, and HTML textarea disabled/read-only/default/value/
cols/rows/tab-index/string/form/select properties.

Tier4 status: the first guest-compiled NetSurf utility set is
`netsurf/utils/bloom.c`, `netsurf/utils/url.c`, `netsurf/utils/utils.c`, and
`netsurf/utils/useragent.c`, plus the first NetSurf desktop support leaf,
`netsurf/desktop/mouse.c`, the first UI colour stylesheet utility,
`netsurf/utils/nscolour.c`, the NetSurf UTF-8 utility wrapper
`netsurf/utils/utf8.c`, and the raw Punycode utility
`netsurf/utils/punycode.c`, plus the plain write-once string hashtable utility
`netsurf/utils/hashtable.c` and callback-backed generic hashmap utility
`netsurf/utils/hashmap.c`, the NetSurf time formatting/parsing utility
`netsurf/utils/time.c`, and the first HTTP parser primitive leaf
`netsurf/utils/http/primitives.c`, plus the HTTP generic item-list helper
`netsurf/utils/http/generics.c`, and the HTTP parameter parser helper
`netsurf/utils/http/parameter.c`, plus the HTTP content-type parser helper
`netsurf/utils/http/content-type.c`, the HTTP content-disposition parser helper
`netsurf/utils/http/content-disposition.c`, the HTTP challenge parser helper
`netsurf/utils/http/challenge.c`, and the HTTP WWW-Authenticate parser helper
`netsurf/utils/http/www-authenticate.c`, plus the HTTP Cache-Control parser
helper `netsurf/utils/http/cache-control.c`, plus the HTTP
Strict-Transport-Security parser helper
`netsurf/utils/http/strict-transport-security.c`, plus the NetSurf log helper
`netsurf/utils/log.c`. The guest `tier4.zbuild` smoke links
the self-hosted parserutils UTF-8 and libwapcaplet dependencies plus bloom,
URL, string utility, user-agent, mouse, nscolour, NetSurf UTF-8, Punycode,
hashtable, hashmap, time, HTTP primitives, HTTP generics, HTTP parameter, HTTP
content-type, HTTP content-disposition, HTTP challenge, HTTP
WWW-Authenticate, HTTP Cache-Control, and HTTP Strict-Transport-Security
objects plus the NetSurf log object into a twenty-four-object binary and exercises
create/destroy, string and raw-hash insert/search, empty strings, false-negative
checks, item counts, percent
escaping with slash exceptions, plus-space conversion, valid percent decoding,
malformed percent preservation, whitespace squashing, UTF-8 NBSP conversion,
user-agent caching, user-agent rebuild after free, and the browser mouse-state
dump call boundary, nscolour update, stylesheet generation, selector presence,
stylesheet caching, UTF-8 scalar length, byte length, navigation, UCS-4 decode,
UCS-4 encode, finalisation, Punycode encode/decode vectors, small-output
handling, invalid-input handling, hashtable add/get, duplicate-key precedence,
inline plain parsing, invalid inline data, bad-parameter handling, hashmap
insert/lookup/duplicate replacement, callback iteration, removal, missing-key
behavior, counts, RFC1123 date formatting/parsing, numeric timestamp
formatting/parsing, invalid date handling, HTTP LWS skipping, token parsing,
quoted-string parsing, parse-failure handling, HTTP item-list callback parsing,
ownership transfer, parse cursor advancement, empty-list failure, and
destructor callbacks, HTTP parameter list parsing, unquoted and quoted
parameter values, case-insensitive parameter lookup, iteration order,
bad-parameter failure, parameter-list cleanup, content-type parsing, media type
interning, content-type charset lookup, malformed content-type rejection, and
content-type cleanup, content-disposition parsing, disposition-token interning,
quoted boundary parameter lookup, malformed content-disposition rejection,
content-disposition cleanup, challenge parsing, scheme iteration,
auth-parameter lookup, malformed challenge rejection, challenge-list cleanup,
WWW-Authenticate parsing, challenge list access, auth-parameter lookup,
malformed WWW-Authenticate rejection, WWW-Authenticate cleanup, Cache-Control
parsing, max-age/no-cache/no-store accessors, quoted max-age parsing,
duplicate directive rejection, invalid max-age handling, malformed
Cache-Control rejection, Cache-Control cleanup, Strict-Transport-Security
parsing, required max-age validation,
includeSubDomains access, quoted max-age parsing, duplicate directive
rejection, invalid includeSubDomains value rejection, malformed
Strict-Transport-Security rejection, Strict-Transport-Security cleanup, log
`-v`/`-V` argument handling, ensure callback success/failure, filter no-op
behavior, log call boundary, and finalise cleanup. A focused guest
`http_chal.zbuild` smoke also
links the HTTP challenge object with its LWC/HTTP dependencies and exercises
challenge scheme iteration, auth-parameter lookup, malformed challenge
rejection, and challenge-list cleanup. A focused guest `http_wa.zbuild` smoke
links the HTTP WWW-Authenticate object with its LWC/HTTP dependencies and
exercises WWW-Authenticate challenge list access, auth-parameter lookup,
malformed-header rejection, and cleanup. A focused guest `http_cc.zbuild` smoke
links the HTTP Cache-Control object with its LWC/HTTP dependencies and
exercises max-age/no-cache/no-store accessors, quoted max-age parsing,
duplicate directive rejection, invalid max-age handling, malformed-header
rejection, and cleanup. A focused guest `http_sts.zbuild` smoke links the HTTP
Strict-Transport-Security object with its LWC/HTTP dependencies and exercises
required max-age parsing, includeSubDomains access, quoted max-age parsing,
missing/invalid/duplicate max-age rejection, invalid includeSubDomains value
rejection, malformed-header rejection, and cleanup. A focused guest
`log.zbuild` smoke links the NetSurf log object and exercises `-v`/`-V`
argument shifting, the ensure callback success/failure paths, filter no-ops,
the log call boundary, default init, and finalise cleanup.

Pick a tiny, low-dependency C file from the staged browser queue, probably a
small `libnsutils` or utility unit, and teach the in-OS toolchain to compile it
into an object-like artifact.

Specific tasks:

- Add a minimal in-OS `zcc` C frontend command or driver mode for staged C files.
- Start with a deliberately tiny C subset: includes, already-expanded macros if
  needed, simple functions, integer ops, and static data.
- Emit either `.zo` directly or an intermediate format that `zlink` can consume.
- Add a guest smoke that compiles one staged browser C file from
  `R:/browser_c/...`, installs it, and calls one exported function.

Acceptance test: inside LainOS, a staged NetSurf-related `.c` file becomes a
linkable object without host compilation.

## 3. Split The Browser Queue Into Self-Hosting Tiers

Status: done for the current 123-unit staged queue. `prepare-browser-selfhost-c-workspace.sh`
generates `COMPILE_TIER0.txt` through `COMPILE_TIER5.txt` plus
`COMPILE_TIERS.txt`, and `scripts/zbrowser-c-tier-smoke.sh` validates that the
tier manifests exactly match `COMPILE_UNITS.txt`.

Create a gradient instead of treating the browser queue as one huge all-or-
nothing step:

```text
tier0: libc/compiler smoke C files
tier1: libnsutils/simple utility files
tier2: libwapcaplet/parserutils leaf files
tier3: libcss/libdom leaf files
tier4: NetSurf utility/content files
tier5: html/layout/redraw files
```

Specific tasks:

- Generate `COMPILE_TIER0.txt`, `COMPILE_TIER1.txt`, etc.
- Add scripts that validate each tier independently.
- Track which tiers are host-built, guest-compiled, or guest-linked.

## 4. Replace The Production Compatibility Stub

Once one C unit compiles in-OS, clean up the browser module boundary.

Specific tasks:

- Move `examples/browser_compat_stub.Z` out of production manifests.
- Keep it only as a smoke/test compatibility provider.
- Make the real NetSurf/WebStyle frontend provider the normal
  `zbrowser_netsurf` dependency.
- Update `zbrowser_netsurf.zbuild` and autoexec copy/install paths.

Acceptance test: `zinstall zbrowser_netsurf` no longer relies on the stub for
normal operation.

## 5. Finish NetSurf Frontend Shims Before Removing Fallback Paint

Do not remove the fallback renderer yet. First harden the NetSurf path.

Specific tasks:

- Font metrics: width, baseline, line height, fallback families.
- Plotters: clipping, rectangles, lines, paths, bitmap blits.
- Bitmap object redraw for page images.
- Form controls enough for text inputs and buttons.
- Content lifecycle cleanup so reload/navigation does not leak or poison state.
- Runtime status text that says whether paint came from NetSurf redraw or the
  fallback path.

Acceptance test: the local smoke page and one simple real page render through
NetSurf redraw without fallback.

## 6. Promote Real Layout Page By Page

Use a small fixed corpus:

```text
local smoke page
FrogFind/simple old HTML
simple Wikipedia article
Wikipedia front page
image-heavy page
form page
```

For each page:

- Capture a screenshot.
- Check there is no crash or panic.
- Check text is visible and roughly laid out.
- Check CSS affects visible paint.
- Check scroll works.
- Track whether fallback was used.

## 7. Move More C Units To Guest Compilation

After the first in-OS C object works and the package remains stable, expand
guest C compilation in small steps:

```text
one C object [done]
5 leaf objects [done]
one small library slice
all tier1 [done for the current libnsutils tier1 slice]
all tier2 [done for the current parserutils/libwapcaplet tier2 slice]
first tier3 leaves [done for the current Hubbub charset + DOM string/namespace/implementation/document/NodeList/html-button/html-input/html-select/html-script/html-textarea slice]
first tier4 utility/desktop set [done for the current NetSurf utils/bloom.c + utils/url.c + utils/utils.c + utils/useragent.c + desktop/mouse.c + utils/nscolour.c + utils/utf8.c + utils/punycode.c + utils/hashtable.c + utils/hashmap.c + utils/time.c + utils/http/primitives.c + utils/http/generics.c + utils/http/parameter.c + utils/http/content-type.c + utils/http/content-disposition.c + utils/http/challenge.c + utils/http/www-authenticate.c + utils/http/cache-control.c + utils/http/strict-transport-security.c + utils/log.c slice]
```

Each tier should still allow host-prebuilt fallback objects, so the browser
remains runnable while the compiler grows.

## Concrete Next Commits

1. `Refresh zbrowser selfhost smoke baseline`
2. `[done] Add tiered browser C selfhost queue manifests`
3. `[done] Compile first staged browser C unit inside LainOS`

The third commit is the door opening: after that, every additional C unit is
real self-hosting progress rather than only integration work.

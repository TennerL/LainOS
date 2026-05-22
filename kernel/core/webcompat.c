#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "libc.h"
#include "webcompat.h"
#include "libwapcaplet/libwapcaplet.h"
#include "libcss/stylesheet.h"
#include "parserutils/charset/mibenum.h"
#include "parserutils/input/inputstream.h"

#define WEBCOMPAT_LWC_INTERN_A 1u
#define WEBCOMPAT_LWC_INTERN_B 2u
#define WEBCOMPAT_LWC_CASELESS 4u
#define WEBCOMPAT_LWC_DEDUP 8u
#define WEBCOMPAT_LWC_SUBSTRING 16u
#define WEBCOMPAT_LWC_TOLOWER 32u
#define WEBCOMPAT_LWC_EXPECTED 63u

static uint32_t webcompat_lwc_status;
static uint32_t webcompat_parserutils_status;
static uint32_t webcompat_css_parse_status;

static void webcompat_lwc_iter_noop(lwc_string *str, void *pw) {
    (void)str;
    (void)pw;
}

static void webcompat_lwc_unref(lwc_string **str) {
    if (str != 0 && *str != 0) {
        lwc_string_unref(*str);
        *str = 0;
    }
}

uint32_t webcompat_lwc_last_status(void) {
    return webcompat_lwc_status;
}

uint32_t webcompat_lwc_smoke(void) {
    lwc_string *mixed = 0;
    lwc_string *lower = 0;
    lwc_string *again = 0;
    lwc_string *sub = 0;
    lwc_string *tolower = 0;
    bool equal = false;
    bool same = false;
    uint32_t status = 0;

    if (lwc_intern_string("Nihonsaba", 9u, &mixed) == lwc_error_ok && mixed != 0) {
        status |= WEBCOMPAT_LWC_INTERN_A;
    }
    if (lwc_intern_string("nihonsaba", 9u, &lower) == lwc_error_ok && lower != 0) {
        status |= WEBCOMPAT_LWC_INTERN_B;
    }
    if (mixed != 0 && lower != 0 &&
        lwc_string_caseless_isequal(mixed, lower, &equal) == lwc_error_ok &&
        equal) {
        status |= WEBCOMPAT_LWC_CASELESS;
    }
    if (mixed != 0 &&
        lwc_intern_string("Nihonsaba", 9u, &again) == lwc_error_ok &&
        again != 0 &&
        lwc_string_isequal(mixed, again, &same) == lwc_error_ok &&
        same) {
        status |= WEBCOMPAT_LWC_DEDUP;
    }
    if (mixed != 0 &&
        lwc_intern_substring(mixed, 0u, 5u, &sub) == lwc_error_ok &&
        sub != 0 &&
        lwc_string_length(sub) == 5u &&
        memcmp(lwc_string_data(sub), "Nihon", 5u) == 0) {
        status |= WEBCOMPAT_LWC_SUBSTRING;
    }
    if (mixed != 0 &&
        lwc_string_tolower(mixed, &tolower) == lwc_error_ok &&
        tolower != 0 &&
        lwc_string_length(tolower) == 9u &&
        memcmp(lwc_string_data(tolower), "nihonsaba", 9u) == 0) {
        status |= WEBCOMPAT_LWC_TOLOWER;
    }

    webcompat_lwc_unref(&tolower);
    webcompat_lwc_unref(&sub);
    webcompat_lwc_unref(&again);
    webcompat_lwc_unref(&lower);
    webcompat_lwc_unref(&mixed);
    lwc_iterate_strings(webcompat_lwc_iter_noop, 0);

    webcompat_lwc_status = status;
    if (status == WEBCOMPAT_LWC_EXPECTED) {
        return 1;
    }
    return 0;
}

#define WEBCOMPAT_PU_MIB 1u
#define WEBCOMPAT_PU_CREATE 2u
#define WEBCOMPAT_PU_APPEND 4u
#define WEBCOMPAT_PU_PEEK_A 8u
#define WEBCOMPAT_PU_PEEK_B 16u
#define WEBCOMPAT_PU_CHARSET 32u
#define WEBCOMPAT_PU_EXPECTED 63u

uint32_t webcompat_pu_status(void) {
    return webcompat_parserutils_status;
}

uint32_t webcompat_pu_smoke(void) {
    parserutils_inputstream *stream = 0;
    const uint8_t *ptr = 0;
    const char *charset;
    size_t len = 0;
    uint32_t source = 0;
    uint32_t status = 0;

    if (parserutils_charset_mibenum_from_name("UTF-8", 5u) != 0) {
        status |= WEBCOMPAT_PU_MIB;
    }
    if (parserutils_inputstream_create("UTF-8", 0u, 0, &stream) == PARSERUTILS_OK &&
        stream != 0) {
        status |= WEBCOMPAT_PU_CREATE;
    }
    if (stream != 0 &&
        parserutils_inputstream_append(stream, (const uint8_t *)"abc", 3u) == PARSERUTILS_OK &&
        parserutils_inputstream_append(stream, 0, 0u) == PARSERUTILS_OK) {
        status |= WEBCOMPAT_PU_APPEND;
    }
    if (stream != 0 &&
        parserutils_inputstream_peek(stream, 0u, &ptr, &len) == PARSERUTILS_OK &&
        len == 1u &&
        ptr != 0 &&
        ptr[0] == 'a') {
        status |= WEBCOMPAT_PU_PEEK_A;
    }
    if (stream != 0) {
        parserutils_inputstream_advance(stream, 1u);
        ptr = 0;
        len = 0;
        if (parserutils_inputstream_peek(stream, 0u, &ptr, &len) == PARSERUTILS_OK &&
            len == 1u &&
            ptr != 0 &&
            ptr[0] == 'b') {
            status |= WEBCOMPAT_PU_PEEK_B;
        }
    }
    if (stream != 0) {
        charset = parserutils_inputstream_read_charset(stream, &source);
        if (charset != 0 && strcmp(charset, "UTF-8") == 0) {
            status |= WEBCOMPAT_PU_CHARSET;
        }
        parserutils_inputstream_destroy(stream);
    }

    webcompat_parserutils_status = status;
    if (status == WEBCOMPAT_PU_EXPECTED) {
        return 1;
    }
    return 0;
}

#define WEBCOMPAT_CSS_CREATE 1u
#define WEBCOMPAT_CSS_APPEND 2u
#define WEBCOMPAT_CSS_DONE 4u
#define WEBCOMPAT_CSS_SIZE 8u
#define WEBCOMPAT_CSS_URL 16u
#define WEBCOMPAT_CSS_DESTROY 32u
#define WEBCOMPAT_CSS_EXPECTED 63u

static css_error webcompat_css_resolve(void *pw,
                                       const char *base,
                                       lwc_string *rel,
                                       lwc_string **abs) {
    uint32_t *status = (uint32_t *)pw;
    lwc_error error;

    if (rel == 0 || abs == 0) {
        return CSS_BADPARM;
    }

    if (status != 0 &&
        base != 0 &&
        strcmp(base, "zbrowser:smoke") == 0 &&
        lwc_string_length(rel) == 8u &&
        memcmp(lwc_string_data(rel), "hero.png", 8u) == 0) {
        *status |= WEBCOMPAT_CSS_URL;
    }

    error = lwc_intern_string(lwc_string_data(rel), lwc_string_length(rel), abs);
    if (error != lwc_error_ok) {
        return error == lwc_error_oom ? CSS_NOMEM : CSS_INVALID;
    }
    return CSS_OK;
}

uint32_t webcompat_css_status(void) {
    return webcompat_css_parse_status;
}

uint32_t webcompat_css_smoke(void) {
    static const uint8_t css[] =
        "body{margin:0;color:#123456;text-align:center;background-image:url(hero.png)}"
        "a{display:block;color:red}";
    css_stylesheet_params params;
    css_stylesheet *sheet = 0;
    css_error error;
    size_t size = 0;
    uint32_t status = 0;

    memset(&params, 0, sizeof(params));
    params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    params.level = CSS_LEVEL_21;
    params.charset = "UTF-8";
    params.url = "zbrowser:smoke";
    params.title = "smoke";
    params.allow_quirks = false;
    params.inline_style = false;
    params.resolve = webcompat_css_resolve;
    params.resolve_pw = &status;

    error = css_stylesheet_create(&params, &sheet);
    if (error == CSS_OK && sheet != 0) {
        status |= WEBCOMPAT_CSS_CREATE;
    }
    if (sheet != 0) {
        error = css_stylesheet_append_data(sheet, css, sizeof(css) - 1u);
        if (error == CSS_OK || error == CSS_NEEDDATA) {
            status |= WEBCOMPAT_CSS_APPEND;
        }
    }
    if (sheet != 0 && css_stylesheet_data_done(sheet) == CSS_OK) {
        status |= WEBCOMPAT_CSS_DONE;
    }
    if (sheet != 0 && css_stylesheet_size(sheet, &size) == CSS_OK && size != 0u) {
        status |= WEBCOMPAT_CSS_SIZE;
    }
    if (sheet != 0 && css_stylesheet_destroy(sheet) == CSS_OK) {
        status |= WEBCOMPAT_CSS_DESTROY;
    }

    webcompat_css_parse_status = status;
    if (status == WEBCOMPAT_CSS_EXPECTED) {
        return 1;
    }
    return 0;
}

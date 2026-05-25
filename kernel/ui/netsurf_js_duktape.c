#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "duktape.h"

#include "utils/errors.h"
#include "javascript/content.h"
#include "javascript/js.h"
#include "utils/nsoption.h"
#include "nsutils/time.h"

struct jsheap {
    duk_context *ctx;
    uint64_t exec_start_ms;
    uint32_t timeout_ms;
};

struct jsthread {
    struct jsheap *heap;
    bool closed;
};

static void *netsurf_js_alloc(void *udata, duk_size_t size) {
    (void)udata;
    return malloc(size);
}

static void *netsurf_js_realloc(void *udata, void *ptr, duk_size_t size) {
    (void)udata;
    if (size == 0u) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

static void netsurf_js_free(void *udata, void *ptr) {
    (void)udata;
    free(ptr);
}

duk_bool_t dukky_check_timeout(void *udata) {
    struct jsheap *heap = udata;
    uint64_t now;

    if (heap == NULL || heap->exec_start_ms == 0u || heap->timeout_ms == 0u) {
        return 0;
    }
    if (nsu_getmonotonic_ms(&now) != NSUERROR_OK) {
        return 0;
    }
    return now - heap->exec_start_ms > heap->timeout_ms;
}

static void netsurf_js_install_browser_stubs(duk_context *ctx) {
    static const char bootstrap[] =
        "var window=this,self=this,top=this,parent=this;"
        "var location=location||{href:'about:blank'};"
        "var navigator=navigator||{userAgent:'NetSurf LainOS Duktape'};"
        "var console=console||{log:function(){},warn:function(){},error:function(){}};"
        "function __lainNode(){return {style:{},children:[],"
        "appendChild:function(n){this.children.push(n);return n;},"
        "removeChild:function(n){return n;},"
        "setAttribute:function(k,v){this[k]=String(v);},"
        "getAttribute:function(k){return this[k]||null;},"
        "addEventListener:function(){},removeEventListener:function(){},"
        "querySelector:function(){return null;},querySelectorAll:function(){return []}};}"
        "var document=document||{documentElement:__lainNode(),body:__lainNode(),"
        "createElement:function(){return __lainNode();},"
        "createTextNode:function(t){return {nodeValue:String(t||'')};},"
        "getElementById:function(){return null;},"
        "getElementsByTagName:function(){return []},"
        "querySelector:function(){return null;},querySelectorAll:function(){return []},"
        "addEventListener:function(){},removeEventListener:function(){}};"
        "function addEventListener(){} function removeEventListener(){}"
        "function setTimeout(fn){if(typeof fn==='function')fn();return 0;}"
        "function clearTimeout(){} function setInterval(){return 0;} function clearInterval(){}";

    duk_push_lstring(ctx, "lainos-js-bootstrap", sizeof("lainos-js-bootstrap") - 1u);
    if (duk_pcompile_lstring_filename(ctx,
                                      DUK_COMPILE_EVAL,
                                      bootstrap,
                                      sizeof(bootstrap) - 1u) == 0) {
        (void)duk_pcall(ctx, 0);
    }
    duk_pop(ctx);
}

void js_initialise(void) {
    javascript_init();
}

void js_finalise(void) {
}

nserror js_newheap(int timeout, jsheap **heap) {
    struct jsheap *ret;

    if (heap == NULL) {
        return NSERROR_BAD_PARAMETER;
    }
    *heap = NULL;
    ret = calloc(1, sizeof(*ret));
    if (ret == NULL) {
        return NSERROR_NOMEM;
    }
    ret->timeout_ms = timeout > 0 ? (uint32_t)timeout * 1000u : 3000u;
    ret->ctx = duk_create_heap(netsurf_js_alloc,
                               netsurf_js_realloc,
                               netsurf_js_free,
                               ret,
                               NULL);
    if (ret->ctx == NULL) {
        free(ret);
        return NSERROR_NOMEM;
    }
    netsurf_js_install_browser_stubs(ret->ctx);
    *heap = ret;
    return NSERROR_OK;
}

void js_destroyheap(jsheap *heap) {
    if (heap == NULL) {
        return;
    }
    if (heap->ctx != NULL) {
        duk_destroy_heap(heap->ctx);
    }
    free(heap);
}

nserror js_newthread(jsheap *heap, void *win_priv, void *doc_priv, jsthread **thread) {
    struct jsthread *ret;
    (void)win_priv;
    (void)doc_priv;

    if (heap == NULL || thread == NULL) {
        return NSERROR_BAD_PARAMETER;
    }
    *thread = NULL;
    ret = calloc(1, sizeof(*ret));
    if (ret == NULL) {
        return NSERROR_NOMEM;
    }
    ret->heap = heap;
    *thread = ret;
    return NSERROR_OK;
}

nserror js_closethread(jsthread *thread) {
    if (thread != NULL) {
        thread->closed = true;
    }
    return NSERROR_OK;
}

void js_destroythread(jsthread *thread) {
    free(thread);
}

bool js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen, const char *name) {
    duk_context *ctx;
    bool ok = false;

    if (thread == NULL || thread->heap == NULL || thread->closed ||
        txt == NULL || txtlen == 0u) {
        return false;
    }
    ctx = thread->heap->ctx;
    if (ctx == NULL) {
        return false;
    }

    duk_set_top(ctx, 0);
    if (nsu_getmonotonic_ms(&thread->heap->exec_start_ms) != NSUERROR_OK) {
        thread->heap->exec_start_ms = 1u;
    }
    duk_push_string(ctx, name != NULL ? name : "?javascript?");
    if (duk_pcompile_lstring_filename(ctx,
                                      DUK_COMPILE_EVAL,
                                      (const char *)txt,
                                      (duk_size_t)txtlen) == 0 &&
        duk_pcall(ctx, 0) == 0) {
        ok = true;
    }
    thread->heap->exec_start_ms = 0u;
    duk_set_top(ctx, 0);
    return ok;
}

bool js_fire_event(jsthread *thread,
                   const char *type,
                   struct dom_document *doc,
                   struct dom_node *target) {
    (void)thread;
    (void)type;
    (void)doc;
    (void)target;
    return true;
}

bool js_dom_event_add_listener(jsthread *thread,
                               struct dom_document *document,
                               struct dom_node *node,
                               struct dom_string *event_type_dom,
                               void *js_funcval) {
    (void)thread;
    (void)document;
    (void)node;
    (void)event_type_dom;
    (void)js_funcval;
    return true;
}

void js_handle_new_element(jsthread *thread, struct dom_element *node) {
    (void)thread;
    (void)node;
}

void js_event_cleanup(jsthread *thread, struct dom_event *evt) {
    (void)thread;
    (void)evt;
}

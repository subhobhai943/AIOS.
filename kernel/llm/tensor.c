#include "tensor.h"
#include "heap.h"      /* kmalloc/kfree */
#include "serial.h"    /* klog() */

/* kernel/llm/tensor.c — Phase 7.1: minimal tensor abstraction
 *
 * This file implements a tiny tensor helper layer used by the
 * LLM engine. It is deliberately small and freestanding:
 *  - no libc
 *  - all allocation uses kmalloc/kfree
 *  - shapes are limited to at most 4 dims
 */

/* Internal helper: compute product of dims[0..ndim-1]. Returns
 * 0 if any dim <= 0 or ndim is outside 1..4.
 */
static size_t tensor_numel_from_dims(const int32_t *dims, int32_t ndim) {
    if (!dims || ndim <= 0 || ndim > 4) return 0;
    size_t n = 1;
    for (int32_t i = 0; i < ndim; i++) {
        if (dims[i] <= 0) return 0;
        n *= (size_t)dims[i];
    }
    return n;
}

tensor_t *tensor_alloc(const int32_t *dims, int32_t ndim) {
    size_t numel = tensor_numel_from_dims(dims, ndim);
    if (numel == 0) return 0;

    tensor_t *t = (tensor_t *)kmalloc(sizeof(tensor_t));
    if (!t) return 0;

    t->data = (float *)kmalloc(numel * sizeof(float));
    if (!t->data) {
        kfree(t);
        return 0;
    }

    t->ndim  = ndim;
    t->numel = numel;
    for (int32_t i = 0; i < 4; i++) {
        t->dims[i] = (i < ndim) ? dims[i] : 1;
    }
    return t;
}

void tensor_free(tensor_t *t) {
    if (!t) return;
    if (t->data) kfree(t->data);
    kfree(t);
}

int tensor_reshape(tensor_t *t, const int32_t *new_dims, int32_t new_ndim) {
    if (!t) return -1;
    size_t new_numel = tensor_numel_from_dims(new_dims, new_ndim);
    if (new_numel == 0 || new_numel != t->numel) return -1;

    t->ndim = new_ndim;
    for (int32_t i = 0; i < 4; i++) {
        t->dims[i] = (i < new_ndim) ? new_dims[i] : 1;
    }
    return 0;
}

int tensor_slice(const tensor_t *t,
                 int32_t dim0_start,
                 int32_t dim0_count,
                 tensor_t *out_view) {
    if (!t || !out_view || t->ndim <= 0) return -1;
    int32_t dim0 = t->dims[0];
    if (dim0_start < 0 || dim0_count <= 0) return -1;
    if (dim0_start + dim0_count > dim0) return -1;

    /* Compute stride of one slice along dim0. */
    size_t stride = t->numel;
    if (dim0 > 0) stride /= (size_t)dim0;

    out_view->data = t->data + (size_t)dim0_start * stride;
    out_view->ndim = t->ndim;
    out_view->numel = (size_t)dim0_count * stride;

    /* Copy dims and update dim0. */
    for (int32_t i = 0; i < 4; i++) out_view->dims[i] = t->dims[i];
    out_view->dims[0] = dim0_count;
    return 0;
}

void tensor_print(const tensor_t *t) {
    if (!t) {
        klog("[tensor] (null)\n");
        return;
    }

    klog("[tensor] ndim=");
    char buf[4];
    /* Cheap decimal print for small ints (0..9). */
    int n = t->ndim;
    if (n < 0) n = 0;
    if (n > 9) n = 9;
    buf[0] = '0' + (char)n;
    buf[1] = '\0';
    klog(buf);
    klog(" dims=[");
    for (int32_t i = 0; i < t->ndim; i++) {
        int d = t->dims[i];
        if (d < 0) d = 0;
        if (d > 9999) d = 9999;
        int thousands = d / 1000;
        int hundreds  = (d / 100) % 10;
        int tens      = (d / 10) % 10;
        int ones      = d % 10;
        int started   = 0;
        if (thousands) { char c = '0' + (char)thousands; klog(&c); started = 1; }
        if (started || hundreds) { char c = '0' + (char)hundreds; klog(&c); started = 1; }
        if (started || tens) { char c = '0' + (char)tens; klog(&c); started = 1; }
        { char c = '0' + (char)ones; klog(&c); }
        if (i + 1 < t->ndim) klog(",");
    }
    klog("] numel=");

    /* Print numel approximately (only low 32 bits). */
    uint32_t n32 = (uint32_t)(t->numel & 0xFFFFFFFFu);
    char numbuf[16];
    int idx = 0;
    if (n32 == 0) {
        numbuf[idx++] = '0';
    } else {
        char tmp[10];
        int tlen = 0;
        while (n32 > 0 && tlen < 10) {
            tmp[tlen++] = (char)('0' + (n32 % 10));
            n32 /= 10;
        }
        while (tlen > 0) numbuf[idx++] = tmp[--tlen];
    }
    numbuf[idx] = '\0';
    klog(numbuf);

    /* Print first few elements. */
    klog(" values=[");
    size_t max_show = t->numel < 4 ? t->numel : 4;
    for (size_t i = 0; i < max_show; i++) {
        /* Very rough fixed-point: show x*1000 as integer. */
        float v = t->data[i];
        int sign = 0;
        if (v < 0.f) { sign = 1; v = -v; }
        int ival = (int)(v * 1000.f);
        if (sign) klog("-");
        char vbuf[16];
        int vlen = 0;
        if (ival == 0) {
            vbuf[vlen++] = '0';
        } else {
            char tmp[10];
            int tlen = 0;
            while (ival > 0 && tlen < 10) {
                tmp[tlen++] = (char)('0' + (ival % 10));
                ival /= 10;
            }
            while (tlen > 0) vbuf[vlen++] = tmp[--tlen];
        }
        vbuf[vlen] = '\0';
        klog(vbuf);
        if (i + 1 < max_show) klog(",");
    }
    if (t->numel > max_show) klog("...");
    klog("]\n");
}

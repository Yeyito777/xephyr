/* Xephyr DRI3 render-node plumbing.
 *
 * This is intentionally small and conservative: it only advertises DRI3 when a
 * render node can be opened. If anything fails, Xephyr remains on its normal
 * software/GLX path.
 */

#include "dix-config.h"

#ifdef DRI3

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>

#include "scrnintstr.h"
#include "dri3.h"
#include "ephyr.h"
#include "misyncshm.h"

typedef struct ephyr_dri3_pixmap_priv {
    struct gbm_bo *bo;
    void *map;
    void *map_data;
    void *copy;
    CARD16 width;
    CARD16 height;
    CARD8 depth;
    CARD8 bpp;
    CARD32 stride;
} ephyr_dri3_pixmap_priv_rec, *ephyr_dri3_pixmap_priv_ptr;

static DevPrivateKeyRec ephyr_dri3_pixmap_private_key;
static DestroyPixmapProcPtr ephyr_dri3_saved_destroy_pixmap;
static int ephyr_dri3_fd = -1;
static struct gbm_device *ephyr_dri3_gbm;

static inline ephyr_dri3_pixmap_priv_ptr
ephyr_dri3_pixmap_priv(PixmapPtr pixmap)
{
    if (!dixPrivateKeyRegistered(&ephyr_dri3_pixmap_private_key))
        return NULL;
    return dixLookupPrivate(&pixmap->devPrivates,
                            &ephyr_dri3_pixmap_private_key);
}

static uint32_t
ephyr_dri3_gbm_format(CARD8 depth)
{
    switch (depth) {
    case 15:
        return GBM_FORMAT_ARGB1555;
    case 16:
        return GBM_FORMAT_RGB565;
    case 24:
        return GBM_FORMAT_XRGB8888;
    case 30:
        return GBM_FORMAT_ARGB2101010;
    case 32:
    default:
        return GBM_FORMAT_ARGB8888;
    }
}

static Bool
ephyr_dri3_destroy_pixmap(PixmapPtr pixmap)
{
    ephyr_dri3_pixmap_priv_ptr priv = ephyr_dri3_pixmap_priv(pixmap);

    if (priv) {
        if (priv->bo && priv->map)
            gbm_bo_unmap(priv->bo, priv->map_data);
        if (priv->bo)
            gbm_bo_destroy(priv->bo);
        free(priv->copy);
        dixSetPrivate(&pixmap->devPrivates, &ephyr_dri3_pixmap_private_key, NULL);
        free(priv);
    }

    return ephyr_dri3_saved_destroy_pixmap(pixmap);
}

static const char *
ephyr_dri3_render_node(void)
{
    static char discovered[PATH_MAX];
    const char *env = getenv("EPHYR_DRI3_RENDER_NODE");
    int i;

    if (env && env[0]) {
        if (!strcmp(env, "0") || !strcmp(env, "off") ||
            !strcmp(env, "false") || !strcmp(env, "none"))
            return NULL;
        return env;
    }

    for (i = 128; i < 256; i++) {
        snprintf(discovered, sizeof(discovered), "/dev/dri/renderD%d", i);
        if (access(discovered, R_OK | W_OK) == 0)
            return discovered;
    }

    return NULL;
}

static int
ephyr_dri3_open_client(ClientPtr client, ScreenPtr screen,
                       RRProviderPtr provider, int *fdp)
{
    const char *node = ephyr_dri3_render_node();
    int fd;

    if (!node)
        return BadAlloc;

    fd = open(node, O_RDWR | O_CLOEXEC);

    if (fd < 0) {
        ErrorF("Xephyr DRI3: failed to open %s: %s\n", node, strerror(errno));
        return BadAlloc;
    }

    *fdp = fd;
    return Success;
}

static PixmapPtr
ephyr_dri3_pixmap_from_fds(ScreenPtr screen, CARD8 num_fds, const int *fds,
                           CARD16 width, CARD16 height,
                           const CARD32 *strides, const CARD32 *offsets,
                           CARD8 depth, CARD8 bpp, CARD64 modifier)
{
    /* Import the client's dma-buf into GBM, keep a CPU shadow buffer as the fb
     * pixmap storage, and refresh that shadow when Present copies the pixmap to
     * a window. This keeps client GL on the real GPU while preserving Xephyr's
     * normal software presentation path. If DRI3/GBM setup fails, this code is
     * not registered and clients naturally fall back to the regular llvmpipe
     * path.
     */
    struct gbm_bo *bo = NULL;
    PixmapPtr pixmap = NULL;
    ephyr_dri3_pixmap_priv_ptr priv = NULL;
    CARD32 copy_stride;
    int fd;

    if (!ephyr_dri3_gbm || num_fds < 1 || width == 0 || height == 0)
        return NULL;

    fd = dup(fds[0]);
    if (fd < 0)
        return NULL;

#ifdef GBM_BO_WITH_MODIFIERS
    if (modifier != DRM_FORMAT_MOD_INVALID) {
        struct gbm_import_fd_modifier_data data = { 0 };
        int i;

        data.width = width;
        data.height = height;
        data.format = ephyr_dri3_gbm_format(depth);
        data.num_fds = num_fds;
        data.modifier = modifier;
        for (i = 0; i < num_fds && i < GBM_MAX_PLANES; i++) {
            data.fds[i] = fds[i];
            data.strides[i] = strides[i];
            data.offsets[i] = offsets[i];
        }
        bo = gbm_bo_import(ephyr_dri3_gbm, GBM_BO_IMPORT_FD_MODIFIER,
                           &data, GBM_BO_USE_RENDERING);
    }
#endif

    if (!bo && num_fds == 1) {
        struct gbm_import_fd_data data = { 0 };

        data.fd = fd;
        data.width = width;
        data.height = height;
        data.stride = strides[0];
        data.format = ephyr_dri3_gbm_format(depth);
        bo = gbm_bo_import(ephyr_dri3_gbm, GBM_BO_IMPORT_FD,
                           &data, GBM_BO_USE_RENDERING);
    }
    close(fd);

    if (!bo)
        return NULL;

    pixmap = screen->CreatePixmap(screen, width, height, depth, 0);
    if (!pixmap)
        goto fail;

    priv = calloc(1, sizeof(*priv));
    if (!priv)
        goto fail;
    priv->bo = bo;
    priv->width = width;
    priv->height = height;
    priv->depth = depth;
    priv->bpp = bpp;
    priv->stride = strides[0];

    copy_stride = PixmapBytePad(width, depth);
    priv->copy = calloc(1, (size_t) copy_stride * height);
    if (!priv->copy)
        goto fail;

    if (!screen->ModifyPixmapHeader(pixmap, width, height, depth, bpp,
                                    copy_stride, priv->copy))
        goto fail;

    dixSetPrivate(&pixmap->devPrivates, &ephyr_dri3_pixmap_private_key, priv);
    return pixmap;

fail:
    if (priv) {
        if (priv->bo && priv->map)
            gbm_bo_unmap(priv->bo, priv->map_data);
        free(priv->copy);
        free(priv);
    }
    if (pixmap)
        screen->DestroyPixmap(pixmap);
    if (bo)
        gbm_bo_destroy(bo);
    return NULL;
}

Bool
ephyr_dri3_refresh_pixmap(PixmapPtr pixmap)
{
    ephyr_dri3_pixmap_priv_ptr priv = ephyr_dri3_pixmap_priv(pixmap);
    uint32_t map_stride = 0;
    CARD32 copy_stride;
    uint8_t *src, *dst;
    CARD16 y;

    if (!priv || !priv->bo || !priv->copy)
        return FALSE;

    if (priv->map) {
        gbm_bo_unmap(priv->bo, priv->map_data);
        priv->map = NULL;
        priv->map_data = NULL;
    }

    priv->map = gbm_bo_map(priv->bo, 0, 0, priv->width, priv->height,
                           GBM_BO_TRANSFER_READ,
                           &map_stride, &priv->map_data);
    if (!priv->map)
        return FALSE;

    copy_stride = pixmap->devKind;
    src = priv->map;
    dst = priv->copy;
    for (y = 0; y < priv->height; y++)
        memcpy(dst + (size_t) y * copy_stride,
               src + (size_t) y * map_stride,
               copy_stride < map_stride ? copy_stride : map_stride);

    gbm_bo_unmap(priv->bo, priv->map_data);
    priv->map = NULL;
    priv->map_data = NULL;
    return TRUE;
}

static int
ephyr_dri3_fds_from_pixmap(ScreenPtr screen, PixmapPtr pixmap, int *fds,
                           uint32_t *strides, uint32_t *offsets,
                           uint64_t *modifier)
{
    return 0;
}

static int
ephyr_dri3_get_formats(ScreenPtr screen, CARD32 *num_formats, CARD32 **formats)
{
    CARD32 *out = calloc(2, sizeof(CARD32));
    if (!out)
        return BadAlloc;

    out[0] = 0x34325258; /* DRM_FORMAT_XRGB8888 */
    out[1] = 0x34325241; /* DRM_FORMAT_ARGB8888 */
    *num_formats = 2;
    *formats = out;
    return Success;
}

static int
ephyr_dri3_get_modifiers(ScreenPtr screen, uint32_t format,
                         uint32_t *num_modifiers, uint64_t **modifiers)
{
    uint64_t *out = calloc(1, sizeof(uint64_t));
    if (!out)
        return BadAlloc;

    out[0] = DRM_FORMAT_MOD_LINEAR;
    *num_modifiers = 1;
    *modifiers = out;
    return Success;
}

static int
ephyr_dri3_get_drawable_modifiers(DrawablePtr draw, uint32_t format,
                                  uint32_t *num_modifiers,
                                  uint64_t **modifiers)
{
    return ephyr_dri3_get_modifiers(draw->pScreen, format, num_modifiers,
                                    modifiers);
}

static const dri3_screen_info_rec ephyr_dri3_info = {
    .version = 2,
    .open_client = ephyr_dri3_open_client,
    .pixmap_from_fds = ephyr_dri3_pixmap_from_fds,
    .fds_from_pixmap = ephyr_dri3_fds_from_pixmap,
    .get_formats = ephyr_dri3_get_formats,
    .get_modifiers = ephyr_dri3_get_modifiers,
    .get_drawable_modifiers = ephyr_dri3_get_drawable_modifiers,
};

Bool
ephyr_dri3_screen_init(ScreenPtr screen)
{
    if (!dixRegisterPrivateKey(&ephyr_dri3_pixmap_private_key,
                               PRIVATE_PIXMAP, 0))
        return FALSE;

    const char *node = ephyr_dri3_render_node();

    if (!node) {
        ErrorF("Xephyr DRI3: disabled; no usable render node found\n");
        return FALSE;
    }

    ephyr_dri3_fd = open(node, O_RDWR | O_CLOEXEC);
    if (ephyr_dri3_fd < 0) {
        ErrorF("Xephyr DRI3: disabled; cannot open %s: %s\n",
               node, strerror(errno));
        return FALSE;
    }

    ephyr_dri3_gbm = gbm_create_device(ephyr_dri3_fd);
    if (!ephyr_dri3_gbm) {
        ErrorF("Xephyr DRI3: disabled; cannot create GBM device\n");
        close(ephyr_dri3_fd);
        ephyr_dri3_fd = -1;
        return FALSE;
    }

    if (!miSyncShmScreenInit(screen)) {
        ErrorF("Xephyr DRI3: sync-shm fence init failed\n");
        return FALSE;
    }

    if (!dri3_screen_init(screen, &ephyr_dri3_info)) {
        ErrorF("Xephyr DRI3: dri3_screen_init failed\n");
        return FALSE;
    }

    if (!ephyr_dri3_saved_destroy_pixmap) {
        ephyr_dri3_saved_destroy_pixmap = screen->DestroyPixmap;
        screen->DestroyPixmap = ephyr_dri3_destroy_pixmap;
    }

    ErrorF("Xephyr DRI3: enabled using %s\n", node);
    return TRUE;
}

#else

#include "misc.h"
#include "scrnintstr.h"

Bool
ephyr_dri3_screen_init(ScreenPtr screen)
{
    return FALSE;
}

#endif

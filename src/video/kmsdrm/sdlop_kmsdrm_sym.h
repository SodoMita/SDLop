/* SDLop - libdrm + libgbm symbol list (X-macro; included multiple times with
 * different SDLOP_DRM_SYM / SDLOP_GBM_SYM definitions; deliberately no include guard).
 *
 * libdrm symbols are required for the KMSDRM driver to init at all.
 * libgbm symbols are loaded separately and are OPTIONAL - without libgbm the
 * driver still works with its dumb-buffer software scanout; only the GL path
 * (and the hardware cursor) are unavailable.
 */

#ifndef SDLOP_DRM_SYM
#define SDLOP_DRM_SYM(...)
#endif
#ifndef SDLOP_GBM_SYM
#define SDLOP_GBM_SYM(...)
#endif

/* --- DRM core / modesetting (required) -------------------------------- */
SDLOP_DRM_SYM(void, drmModeFreeResources, (drmModeResPtr ptr))
SDLOP_DRM_SYM(void, drmModeFreeFB, (drmModeFBPtr ptr))
SDLOP_DRM_SYM(void, drmModeFreeCrtc, (drmModeCrtcPtr ptr))
SDLOP_DRM_SYM(void, drmModeFreeConnector, (drmModeConnectorPtr ptr))
SDLOP_DRM_SYM(void, drmModeFreeEncoder, (drmModeEncoderPtr ptr))
SDLOP_DRM_SYM(void, drmModeFreeModeInfo, (drmModeModeInfoPtr ptr))
SDLOP_DRM_SYM(int, drmGetCap, (int fd, uint64_t capability, uint64_t *value))
SDLOP_DRM_SYM(int, drmSetMaster, (int fd))
SDLOP_DRM_SYM(int, drmDropMaster, (int fd))
SDLOP_DRM_SYM(int, drmAuthMagic, (int fd, drm_magic_t magic))
SDLOP_DRM_SYM(drmModeResPtr, drmModeGetResources, (int fd))
SDLOP_DRM_SYM(int, drmModeAddFB, (int fd, uint32_t width, uint32_t height, uint8_t depth,
                                  uint8_t bpp, uint32_t pitch, uint32_t bo_handle,
                                  uint32_t *buf_id))
SDLOP_DRM_SYM(int, drmModeAddFB2, (int fd, uint32_t width, uint32_t height,
                                   uint32_t pixel_format, const uint32_t bo_handles[4],
                                   const uint32_t pitches[4], const uint32_t offsets[4],
                                   uint32_t *buf_id, uint32_t flags))
SDLOP_DRM_SYM(int, drmModeAddFB2WithModifiers, (int fd, uint32_t width, uint32_t height,
                                                uint32_t pixel_format, const uint32_t bo_handles[4],
                                                const uint32_t pitches[4], const uint32_t offsets[4],
                                                const uint64_t modifier[4], uint32_t *buf_id, uint32_t flags))
SDLOP_DRM_SYM(const char *, drmModeGetConnectorTypeName, (uint32_t connector_type))
SDLOP_DRM_SYM(int, drmModeRmFB, (int fd, uint32_t bufferId))
SDLOP_DRM_SYM(drmModeFBPtr, drmModeGetFB, (int fd, uint32_t buf))
SDLOP_DRM_SYM(drmModeCrtcPtr, drmModeGetCrtc, (int fd, uint32_t crtcId))
SDLOP_DRM_SYM(int, drmModeSetCrtc, (int fd, uint32_t crtcId, uint32_t bufferId,
                                    uint32_t x, uint32_t y, uint32_t *connectors, int count,
                                    drmModeModeInfoPtr mode))
SDLOP_DRM_SYM(int, drmModeSetCursor, (int fd, uint32_t crtcId, uint32_t bo_handle,
                                      uint32_t width, uint32_t height))
SDLOP_DRM_SYM(int, drmModeSetCursor2, (int fd, uint32_t crtcId, uint32_t bo_handle,
                                       uint32_t width, uint32_t height,
                                       int32_t hot_x, int32_t hot_y))
SDLOP_DRM_SYM(int, drmModeMoveCursor, (int fd, uint32_t crtcId, int x, int y))
SDLOP_DRM_SYM(drmModeEncoderPtr, drmModeGetEncoder, (int fd, uint32_t encoder_id))
SDLOP_DRM_SYM(drmModeConnectorPtr, drmModeGetConnector, (int fd, uint32_t connector_id))
SDLOP_DRM_SYM(int, drmHandleEvent, (int fd, drmEventContextPtr evctx))
SDLOP_DRM_SYM(int, drmModePageFlip, (int fd, uint32_t crtc_id, uint32_t fb_id,
                                     uint32_t flags, void *user_data))
SDLOP_DRM_SYM(int, drmSetClientCap, (int fd, uint64_t capability, uint64_t value))
SDLOP_DRM_SYM(int, drmModeCreateDumbBuffer, (int fd, uint32_t width, uint32_t height,
                                             uint32_t bpp, uint32_t flags, uint32_t *handle,
                                             uint32_t *pitch, uint64_t *size))
SDLOP_DRM_SYM(int, drmModeMapDumbBuffer, (int fd, uint32_t handle, uint64_t *offset))
SDLOP_DRM_SYM(int, drmModeDestroyDumbBuffer, (int fd, uint32_t handle))

/* --- GBM (optional) -------------------------------------------------- */
SDLOP_GBM_SYM(int, gbm_device_is_format_supported, (struct gbm_device *gbm,
                                                   uint32_t format, uint32_t usage))
SDLOP_GBM_SYM(void, gbm_device_destroy, (struct gbm_device *gbm))
SDLOP_GBM_SYM(struct gbm_device *, gbm_create_device, (int fd))
SDLOP_GBM_SYM(unsigned int, gbm_bo_get_width, (struct gbm_bo *bo))
SDLOP_GBM_SYM(unsigned int, gbm_bo_get_height, (struct gbm_bo *bo))
SDLOP_GBM_SYM(uint32_t, gbm_bo_get_stride, (struct gbm_bo *bo))
SDLOP_GBM_SYM(uint32_t, gbm_bo_get_format, (struct gbm_bo *bo))
SDLOP_GBM_SYM(union gbm_bo_handle, gbm_bo_get_handle, (struct gbm_bo *bo))
SDLOP_GBM_SYM(int, gbm_bo_write, (struct gbm_bo *bo, const void *buf, size_t count))
SDLOP_GBM_SYM(struct gbm_device *, gbm_bo_get_device, (struct gbm_bo *bo))
SDLOP_GBM_SYM(void, gbm_bo_set_user_data, (struct gbm_bo *bo, void *data,
                                           void (*destroy_user_data)(struct gbm_bo *, void *)))
SDLOP_GBM_SYM(void *, gbm_bo_get_user_data, (struct gbm_bo *bo))
SDLOP_GBM_SYM(void, gbm_bo_destroy, (struct gbm_bo *bo))
SDLOP_GBM_SYM(struct gbm_bo *, gbm_bo_create, (struct gbm_device *gbm,
                                               uint32_t width, uint32_t height,
                                               uint32_t format, uint32_t usage))
SDLOP_GBM_SYM(struct gbm_surface *, gbm_surface_create, (struct gbm_device *gbm,
                                                         uint32_t width, uint32_t height,
                                                         uint32_t format, uint32_t flags))
SDLOP_GBM_SYM(void, gbm_surface_destroy, (struct gbm_surface *surf))
SDLOP_GBM_SYM(struct gbm_bo *, gbm_surface_lock_front_buffer, (struct gbm_surface *surf))
SDLOP_GBM_SYM(void, gbm_surface_release_buffer, (struct gbm_surface *surf, struct gbm_bo *bo))
SDLOP_GBM_SYM(uint64_t, gbm_bo_get_modifier, (struct gbm_bo *bo))
SDLOP_GBM_SYM(int, gbm_bo_get_plane_count, (struct gbm_bo *bo))
SDLOP_GBM_SYM(uint32_t, gbm_bo_get_offset, (struct gbm_bo *bo, int plane))
SDLOP_GBM_SYM(uint32_t, gbm_bo_get_stride_for_plane, (struct gbm_bo *bo, int plane))
SDLOP_GBM_SYM(union gbm_bo_handle, gbm_bo_get_handle_for_plane, (struct gbm_bo *bo, int plane))

#undef SDLOP_DRM_SYM
#undef SDLOP_GBM_SYM

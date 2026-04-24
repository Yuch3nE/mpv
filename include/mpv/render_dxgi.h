#ifndef MPV_CLIENT_API_RENDER_DXGI_H_
#define MPV_CLIENT_API_RENDER_DXGI_H_

#include <stdint.h>

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * For initializing the mpv DXGI state via MPV_RENDER_PARAM_DXGI_INIT_PARAMS.
 * The pointers are expected to point to ID3D11Device and IDXGISwapChain.
 */
typedef struct mpv_dxgi_init_params {
    void *device;
    void *swapchain;
} mpv_dxgi_init_params;

/**
 * State returned by MPV_RENDER_PARAM_DXGI_COLORSPACE_HINT.
 */
typedef enum mpv_dxgi_colorspace_hint_state {
    /**
     * No hint is available for the next frame.
     */
    MPV_DXGI_COLORSPACE_HINT_NONE = 0,
    /**
     * Apply the returned colorspace and HDR metadata to the DXGI swapchain.
     */
    MPV_DXGI_COLORSPACE_HINT_SET = 1,
    /**
     * Clear any previously applied colorspace/HDR override on the swapchain.
     */
    MPV_DXGI_COLORSPACE_HINT_CLEAR = 2,
} mpv_dxgi_colorspace_hint_state;

/**
 * Returned by MPV_RENDER_PARAM_DXGI_COLORSPACE_HINT.
 *
 * color_space uses the numeric value of DXGI_COLOR_SPACE_TYPE.
 * When state is MPV_DXGI_COLORSPACE_HINT_SET, the luminance and primary fields
 * contain the HDR metadata mpv derived for the next frame.
 */
typedef struct mpv_dxgi_colorspace_hint {
    enum mpv_dxgi_colorspace_hint_state state;
    uint32_t color_space;
    uint32_t bits_per_color;
    float primaries[4][2];
    float min_luma;
    float max_luma;
    float max_cll;
    float max_fall;
} mpv_dxgi_colorspace_hint;

#ifdef __cplusplus
}
#endif

#endif
#ifndef UWP_UTIL_H
#define UWP_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

void* uwp_get_window_reference(void);
void uwp_set_window_reference(void *window, int width, int height);
typedef long (__cdecl *mesa_uwp_swapchain_attach_callback)(void *opaque,
                                                           void *swapchain);
void mesa_uwp_set_swapchain_attach_callback(
   mesa_uwp_swapchain_attach_callback callback, void *opaque);
long mesa_uwp_attach_swapchain(void *swapchain);
int uwp_get_height(void);
int uwp_get_width(void);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Host-side buffer allocator for VCMD_RESOURCE_ALLOC_GPU.
 *
 * GPU path: allocates an exportable linear VkImage + dedicated VkDeviceMemory
 * on the render GPU (NVIDIA) and exports it as a dma_buf.  Buffers born on
 * the compositing GPU are the only ones KWin-on-NVIDIA can bind as
 * GL_TEXTURE_2D (foreign dmabufs bind only as EXTERNAL_OES).
 *
 * CPU path: memfd + udmabuf, for BO_USE_SW_* gralloc buffers that the guest
 * must mmap; still a real dma_buf so the GPU can sample it.
 */

#ifndef VTEST_GPU_ALLOC_H
#define VTEST_GPU_ALLOC_H

#include <stdbool.h>
#include <stdint.h>

int vtest_gpu_alloc_gpu(uint32_t width, uint32_t height, uint32_t drm_format,
                        uint32_t *out_stride, uint64_t *out_modifier,
                        uint64_t *out_size, int *out_fd);

int vtest_gpu_alloc_cpu(uint32_t width, uint32_t height, uint32_t drm_format,
                        uint32_t *out_stride, uint64_t *out_size, int *out_fd);

#endif /* VTEST_GPU_ALLOC_H */

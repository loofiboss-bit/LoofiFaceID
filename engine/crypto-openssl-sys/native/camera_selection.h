// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KFACEAUTH_CAMERA_SELECTION_H
#define KFACEAUTH_CAMERA_SELECTION_H
#include <stdint.h>
#include <string.h>
#include <time.h>
// clang-format off
#include <linux/videodev2.h>
// clang-format on
static inline int kfaceauth_camera_node_name(const char *name)
{
    if (strncmp(name, "video", 5) != 0 || name[5] == '\0')
        return 0;
    for (const char *p = name + 5; *p; ++p)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}
static inline int kfaceauth_camera_compatible(uint32_t caps, uint32_t format)
{
    return (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING) &&
           (format == V4L2_PIX_FMT_GREY || format == V4L2_PIX_FMT_YUYV);
}
static inline int kfaceauth_camera_unique(unsigned int compatible)
{
    return compatible == 1;
}
#endif

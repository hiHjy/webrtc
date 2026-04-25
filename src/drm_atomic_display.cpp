#include "drm_atomic_display.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace {

struct PlaneMatch {
    uint32_t plane_id = 0;
    bool found_primary = false;
};

std::string ErrnoMessage(const std::string &prefix) {
    std::ostringstream oss;
    oss << prefix << ": " << std::strerror(errno);
    return oss.str();
}

uint32_t GetPropertyId(int fd, uint32_t object_id, uint32_t object_type, const char *name) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, object_id, object_type);
    if (!props) {
        return 0;
    }

    uint32_t prop_id = 0;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop) {
            continue;
        }
        if (std::strcmp(prop->name, name) == 0) {
            prop_id = prop->prop_id;
            drmModeFreeProperty(prop);
            break;
        }
        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
    return prop_id;
}

bool GetPropertyValue(int fd,
                      uint32_t object_id,
                      uint32_t object_type,
                      const char *name,
                      uint64_t &value) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, object_id, object_type);
    if (!props) {
        return false;
    }

    bool found = false;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop) {
            continue;
        }
        if (std::strcmp(prop->name, name) == 0) {
            value = props->prop_values[i];
            found = true;
            drmModeFreeProperty(prop);
            break;
        }
        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
    return found;
}

bool PlaneSupportsFormat(const drmModePlane *plane, uint32_t format) {
    for (uint32_t i = 0; i < plane->count_formats; ++i) {
        if (plane->formats[i] == format) {
            return true;
        }
    }
    return false;
}

PlaneMatch FindPlaneForCrtc(int fd, drmModePlaneResPtr plane_res, uint32_t crtc_index) {
    PlaneMatch match;

    for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
        drmModePlanePtr plane = drmModeGetPlane(fd, plane_res->planes[i]);
        if (!plane) {
            continue;
        }

        const bool crtc_ok = (plane->possible_crtcs & (1u << crtc_index)) != 0;
        const bool format_ok = PlaneSupportsFormat(plane, DRM_FORMAT_NV12);
        if (!crtc_ok || !format_ok) {
            drmModeFreePlane(plane);
            continue;
        }

        uint64_t type = 0;
        if (GetPropertyValue(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, "type", type) &&
            type == DRM_PLANE_TYPE_PRIMARY) {
            match.plane_id = plane->plane_id;
            match.found_primary = true;
            drmModeFreePlane(plane);
            return match;
        }

        if (match.plane_id == 0) {
            match.plane_id = plane->plane_id;
        }

        drmModeFreePlane(plane);
    }

    return match;
}

} // namespace

DrmAtomicDisplay::DrmAtomicDisplay() = default;

DrmAtomicDisplay::~DrmAtomicDisplay() {
    Cleanup();
}

const std::string &DrmAtomicDisplay::LastError() const {
    return last_error_;
}

void DrmAtomicDisplay::HandlePageFlipEvent(int,
                                           unsigned int,
                                           unsigned int,
                                           unsigned int,
                                           void *data) {
    auto *display = static_cast<DrmAtomicDisplay *>(data);
    if (!display) {
        return;
    }

    display->page_flip_done_ = true;
    display->page_flip_pending_ = false;
}

void DrmAtomicDisplay::SetError(const std::string &message) {
    last_error_ = message;
    std::cerr << "[drm] " << message << std::endl;
}

bool DrmAtomicDisplay::OpenDevice() {
    const char *env_device = std::getenv("DRM_DEVICE");
    device_path_ = env_device && env_device[0] ? env_device : "/dev/dri/card0";

    fd_ = open(device_path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        SetError(ErrnoMessage("failed to open DRM device " + device_path_));
        return false;
    }

    if (drmSetClientCap(fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
        SetError(ErrnoMessage("failed to enable DRM universal planes"));
        return false;
    }

    if (drmSetClientCap(fd_, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        SetError(ErrnoMessage("failed to enable DRM atomic"));
        return false;
    }

    std::cout << "[drm] opened device " << device_path_ << std::endl;
    return true;
}

bool DrmAtomicDisplay::SetupKms() {
    drmModeResPtr res = drmModeGetResources(fd_);
    if (!res) {
        SetError(ErrnoMessage("drmModeGetResources failed"));
        return false;
    }

    drmModeConnectorPtr connector = nullptr;
    drmModeEncoderPtr encoder = nullptr;

    for (int i = 0; i < res->count_connectors; ++i) {
        connector = drmModeGetConnector(fd_, res->connectors[i]);
        if (!connector) {
            continue;
        }

        if (connector->connection != DRM_MODE_CONNECTED || connector->count_modes <= 0) {
            drmModeFreeConnector(connector);
            connector = nullptr;
            continue;
        }

        if (connector->encoder_id) {
            encoder = drmModeGetEncoder(fd_, connector->encoder_id);
        }

        if (encoder) {
            for (int crtc_i = 0; crtc_i < res->count_crtcs; ++crtc_i) {
                if (res->crtcs[crtc_i] == encoder->crtc_id) {
                    crtc_id_ = encoder->crtc_id;
                    crtc_index_ = static_cast<uint32_t>(crtc_i);
                    break;
                }
            }
        }

        if (!crtc_id_) {
            for (int enc_i = 0; enc_i < connector->count_encoders && !crtc_id_; ++enc_i) {
                drmModeEncoderPtr cand = drmModeGetEncoder(fd_, connector->encoders[enc_i]);
                if (!cand) {
                    continue;
                }
                for (int crtc_i = 0; crtc_i < res->count_crtcs; ++crtc_i) {
                    if (cand->possible_crtcs & (1 << crtc_i)) {
                        crtc_id_ = res->crtcs[crtc_i];
                        crtc_index_ = static_cast<uint32_t>(crtc_i);
                        break;
                    }
                }
                drmModeFreeEncoder(cand);
            }
        }

        if (crtc_id_) {
            connector_id_ = connector->connector_id;
            break;
        }

        if (encoder) {
            drmModeFreeEncoder(encoder);
            encoder = nullptr;
        }
        drmModeFreeConnector(connector);
        connector = nullptr;
    }

    if (!connector || !crtc_id_) {
        if (encoder) {
            drmModeFreeEncoder(encoder);
        }
        if (connector) {
            drmModeFreeConnector(connector);
        }
        drmModeFreeResources(res);
        SetError("failed to find connected connector/crtc");
        return false;
    }

    drmModeModeInfo mode = connector->modes[0];
    display_width_ = mode.hdisplay;
    display_height_ = mode.vdisplay;

    drmModePlaneResPtr plane_res = drmModeGetPlaneResources(fd_);
    if (!plane_res) {
        if (encoder) {
            drmModeFreeEncoder(encoder);
        }
        drmModeFreeConnector(connector);
        drmModeFreeResources(res);
        SetError(ErrnoMessage("drmModeGetPlaneResources failed"));
        return false;
    }

    PlaneMatch plane_match = FindPlaneForCrtc(fd_, plane_res, crtc_index_);
    plane_id_ = plane_match.plane_id;
    drmModeFreePlaneResources(plane_res);

    if (encoder) {
        drmModeFreeEncoder(encoder);
    }
    drmModeFreeConnector(connector);
    drmModeFreeResources(res);

    if (!plane_id_) {
        SetError("failed to find compatible DRM NV12 plane");
        return false;
    }

    conn_prop_crtc_id_ = GetPropertyId(fd_, connector_id_, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    crtc_prop_active_ = GetPropertyId(fd_, crtc_id_, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    crtc_prop_mode_id_ = GetPropertyId(fd_, crtc_id_, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    plane_prop_fb_id_ = GetPropertyId(fd_, plane_id_, DRM_MODE_OBJECT_PLANE, "FB_ID");
    plane_prop_crtc_id_ = GetPropertyId(fd_, plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    plane_prop_src_x_ = GetPropertyId(fd_, plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_X");
    plane_prop_src_y_ = GetPropertyId(fd_, plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    plane_prop_src_w_ = GetPropertyId(fd_, plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_W");
    plane_prop_src_h_ = GetPropertyId(fd_, plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_H");
    plane_prop_crtc_x_ = GetPropertyId(fd_, plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    plane_prop_crtc_y_ = GetPropertyId(fd_, plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    plane_prop_crtc_w_ = GetPropertyId(fd_, plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    plane_prop_crtc_h_ = GetPropertyId(fd_, plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_H");

    if (!conn_prop_crtc_id_ || !crtc_prop_active_ || !crtc_prop_mode_id_ || !plane_prop_fb_id_ ||
        !plane_prop_crtc_id_ || !plane_prop_src_x_ || !plane_prop_src_y_ || !plane_prop_src_w_ ||
        !plane_prop_src_h_ || !plane_prop_crtc_x_ || !plane_prop_crtc_y_ ||
        !plane_prop_crtc_w_ || !plane_prop_crtc_h_) {
        SetError("failed to resolve required DRM properties");
        return false;
    }

    if (drmModeCreatePropertyBlob(fd_, &mode, sizeof(mode), &mode_blob_id_) != 0) {
        SetError(ErrnoMessage("drmModeCreatePropertyBlob failed"));
        return false;
    }

    std::cout << "[drm] connector=" << connector_id_
              << " crtc=" << crtc_id_
              << " plane=" << plane_id_
              << " mode=" << display_width_ << "x" << display_height_
              << " video=" << video_width_ << "x" << video_height_
              << " stride=" << video_h_stride_ << "x" << video_v_stride_ << std::endl;
    return true;
}

bool DrmAtomicDisplay::Initialize(uint32_t width,
                                  uint32_t height,
                                  uint32_t h_stride,
                                  uint32_t v_stride) {
    if (initialized_) {
        if (video_width_ != width || video_height_ != height ||
            video_h_stride_ != h_stride || video_v_stride_ != v_stride) {
            SetError("video geometry changed after DRM init, reconfiguration is not implemented yet");
            return false;
        }
        return true;
    }

    video_width_ = width;
    video_height_ = height;
    video_h_stride_ = h_stride;
    video_v_stride_ = v_stride;

    if (!OpenDevice()) {
        return false;
    }
    if (!SetupKms()) {
        return false;
    }

    initialized_ = true;
    std::cout << "[drm] atomic dma-buf display initialized" << std::endl;
    return true;
}

bool DrmAtomicDisplay::ImportFrame(ImportedFrame &frame,
                                   int dma_fd,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t h_stride,
                                   uint32_t v_stride) {
    if (dma_fd < 0) {
        SetError("invalid dma-buf fd");
        return false;
    }

    frame.dma_fd = dma_fd;
    frame.width = width;
    frame.height = height;
    frame.h_stride = h_stride;
    frame.v_stride = v_stride;

    if (drmPrimeFDToHandle(fd_, dma_fd, &frame.handle) != 0) {
        SetError(ErrnoMessage("drmPrimeFDToHandle failed"));
        return false;
    }

    uint32_t handles[4] = {frame.handle, frame.handle, 0, 0};
    uint32_t pitches[4] = {h_stride, h_stride, 0, 0};
    uint32_t offsets[4] = {0, h_stride * v_stride, 0, 0};
    if (drmModeAddFB2(fd_, width, height, DRM_FORMAT_NV12, handles, pitches,
                      offsets, &frame.fb_id, 0) != 0) {
        ReleaseImportedFrame(frame);
        SetError(ErrnoMessage("drmModeAddFB2(NV12) failed"));
        return false;
    }

    return true;
}

void DrmAtomicDisplay::ReleaseImportedFrame(ImportedFrame &frame) {
    if (frame.fb_id) {
        drmModeRmFB(fd_, frame.fb_id);
        frame.fb_id = 0;
    }

    if (frame.handle) {
        struct drm_gem_close gem_close;
        std::memset(&gem_close, 0, sizeof(gem_close));
        gem_close.handle = frame.handle;
        drmIoctl(fd_, DRM_IOCTL_GEM_CLOSE, &gem_close);
        frame.handle = 0;
    }

    frame.dma_fd = -1;
    frame.width = 0;
    frame.height = 0;
    frame.h_stride = 0;
    frame.v_stride = 0;
}

bool DrmAtomicDisplay::CommitFrame(uint32_t fb_id, bool allow_modeset) {
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    if (!req) {
        SetError("drmModeAtomicAlloc failed");
        return false;
    }

    bool ok = true;
    auto add_prop = [&](uint32_t obj_id, uint32_t prop_id, uint64_t value) {
        if (drmModeAtomicAddProperty(req, obj_id, prop_id, value) < 0) {
            ok = false;
        }
    };

    add_prop(connector_id_, conn_prop_crtc_id_, crtc_id_);
    add_prop(crtc_id_, crtc_prop_active_, 1);
    add_prop(crtc_id_, crtc_prop_mode_id_, mode_blob_id_);

    add_prop(plane_id_, plane_prop_fb_id_, fb_id);
    add_prop(plane_id_, plane_prop_crtc_id_, crtc_id_);
    add_prop(plane_id_, plane_prop_src_x_, 0);
    add_prop(plane_id_, plane_prop_src_y_, 0);
    add_prop(plane_id_, plane_prop_src_w_, static_cast<uint64_t>(video_width_) << 16);
    add_prop(plane_id_, plane_prop_src_h_, static_cast<uint64_t>(video_height_) << 16);
    add_prop(plane_id_, plane_prop_crtc_x_, 0);
    add_prop(plane_id_, plane_prop_crtc_y_, 0);
    add_prop(plane_id_, plane_prop_crtc_w_, display_width_);
    add_prop(plane_id_, plane_prop_crtc_h_, display_height_);

    if (!ok) {
        drmModeAtomicFree(req);
        SetError("drmModeAtomicAddProperty failed");
        return false;
    }

    page_flip_done_ = false;
    page_flip_pending_ = true;
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
    if (allow_modeset) {
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    }

    if (drmModeAtomicCommit(fd_, req, flags, this) != 0) {
        page_flip_pending_ = false;
        drmModeAtomicFree(req);
        SetError(ErrnoMessage("drmModeAtomicCommit failed"));
        return false;
    }

    drmModeAtomicFree(req);
    return true;
}

bool DrmAtomicDisplay::WaitForPageFlip() {
    if (!page_flip_pending_) {
        return true;
    }

    drmEventContext ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = &DrmAtomicDisplay::HandlePageFlipEvent;

    while (!page_flip_done_) {
        struct pollfd pfd;
        std::memset(&pfd, 0, sizeof(pfd));
        pfd.fd = fd_;
        pfd.events = POLLIN;

        const int ret = poll(&pfd, 1, 3000);
        if (ret < 0) {
            SetError(ErrnoMessage("poll waiting for page flip failed"));
            return false;
        }
        if (ret == 0) {
            SetError("timeout waiting for DRM page flip event");
            return false;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            SetError("DRM fd reported error while waiting for page flip");
            return false;
        }
        if (drmHandleEvent(fd_, &ev) != 0) {
            SetError(ErrnoMessage("drmHandleEvent failed"));
            return false;
        }
    }

    return true;
}

bool DrmAtomicDisplay::PresentDmabuf(int dma_fd,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t h_stride,
                                     uint32_t v_stride) {
    if (!Initialize(width, height, h_stride, v_stride)) {
        return false;
    }

    if (!WaitForPageFlip()) {
        return false;
    }

    ImportedFrame next_frame;
    if (!ImportFrame(next_frame, dma_fd, width, height, h_stride, v_stride)) {
        return false;
    }

    if (!CommitFrame(next_frame.fb_id, !has_presented_frame_)) {
        ReleaseImportedFrame(next_frame);
        return false;
    }

    if (!WaitForPageFlip()) {
        ReleaseImportedFrame(next_frame);
        return false;
    }

    if (has_presented_frame_) {
        ReleaseImportedFrame(current_frame_);
    }

    current_frame_ = next_frame;
    has_presented_frame_ = true;
    return true;
}

void DrmAtomicDisplay::Cleanup() {
    if (page_flip_pending_) {
        WaitForPageFlip();
    }

    ReleaseImportedFrame(current_frame_);

    if (mode_blob_id_) {
        drmModeDestroyPropertyBlob(fd_, mode_blob_id_);
        mode_blob_id_ = 0;
    }

    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    initialized_ = false;
    has_presented_frame_ = false;
    page_flip_done_ = false;
    page_flip_pending_ = false;
    video_width_ = 0;
    video_height_ = 0;
    video_h_stride_ = 0;
    video_v_stride_ = 0;
}

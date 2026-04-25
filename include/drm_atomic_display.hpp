#ifndef DRM_ATOMIC_DISPLAY_HPP
#define DRM_ATOMIC_DISPLAY_HPP

#include <cstddef>
#include <cstdint>
#include <string>

class DrmAtomicDisplay {
public:
    DrmAtomicDisplay();
    ~DrmAtomicDisplay();

    bool PresentDmabuf(int dma_fd,
                       uint32_t width,
                       uint32_t height,
                       uint32_t h_stride,
                       uint32_t v_stride);

    const std::string &LastError() const;

private:
    struct ImportedFrame;
    static void HandlePageFlipEvent(int fd,
                                    unsigned int frame,
                                    unsigned int sec,
                                    unsigned int usec,
                                    void *data);

    bool Initialize(uint32_t width,
                    uint32_t height,
                    uint32_t h_stride,
                    uint32_t v_stride);
    bool OpenDevice();
    bool SetupKms();
    bool ImportFrame(ImportedFrame &frame,
                     int dma_fd,
                     uint32_t width,
                     uint32_t height,
                     uint32_t h_stride,
                     uint32_t v_stride);
    void ReleaseImportedFrame(ImportedFrame &frame);
    void Cleanup();

    bool CommitFrame(uint32_t fb_id, bool allow_modeset);
    bool WaitForPageFlip();

    void SetError(const std::string &message);

    int fd_ = -1;
    uint32_t video_width_ = 0;
    uint32_t video_height_ = 0;
    uint32_t video_h_stride_ = 0;
    uint32_t video_v_stride_ = 0;
    uint32_t connector_id_ = 0;
    uint32_t crtc_id_ = 0;
    uint32_t plane_id_ = 0;
    uint32_t crtc_index_ = 0;
    uint32_t mode_blob_id_ = 0;
    uint32_t conn_prop_crtc_id_ = 0;
    uint32_t crtc_prop_active_ = 0;
    uint32_t crtc_prop_mode_id_ = 0;
    uint32_t plane_prop_fb_id_ = 0;
    uint32_t plane_prop_crtc_id_ = 0;
    uint32_t plane_prop_src_x_ = 0;
    uint32_t plane_prop_src_y_ = 0;
    uint32_t plane_prop_src_w_ = 0;
    uint32_t plane_prop_src_h_ = 0;
    uint32_t plane_prop_crtc_x_ = 0;
    uint32_t plane_prop_crtc_y_ = 0;
    uint32_t plane_prop_crtc_w_ = 0;
    uint32_t plane_prop_crtc_h_ = 0;
    uint32_t display_width_ = 0;
    uint32_t display_height_ = 0;
    bool initialized_ = false;
    bool has_presented_frame_ = false;
    bool page_flip_pending_ = false;
    bool page_flip_done_ = false;
    std::string device_path_;
    std::string last_error_;

    struct ImportedFrame {
        int dma_fd = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t h_stride = 0;
        uint32_t v_stride = 0;
        uint32_t handle = 0;
        uint32_t fb_id = 0;
    };

    ImportedFrame current_frame_;
};

#endif

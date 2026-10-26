#ifndef UILOTTIE_H
#define UILOTTIE_H

#include "uiwidget.h"

#ifdef FW_LOTTIE

#include <atomic>
#include <future>

namespace rlottie {
    class Animation;
}

// Plays a Lottie (Bodymovin JSON) animation with rlottie.
//
// The playback clock runs in drawSelf, on the dispatcher thread, so a widget
// that is not drawn (hidden, out of its parent's area) costs nothing. Frames
// are rasterized on g_asyncDispatcher, at most one job per widget, into a few
// recycled RGBA images that are uploaded to one texture per widget
// (Texture::updatePixels: glTexSubImage2D while the size stays the same). The
// frame on screen may trail the playback clock by one frame.
//
// Every widget has its own rlottie::Animation (renderer state); widgets that
// load the same file share the parsed model through rlottie's model cache.
// @bindclass
class UILottie : public UIWidget
{
public:
    UILottie();
    ~UILottie() override;

    void drawSelf(Fw::DrawPane drawPane) override;

    // Loads a .json file from the resource tree ("/mods/x/anim.json"; a path
    // without a leading slash is taken from the root). A file that is missing
    // or does not parse is logged and keeps the animation that was loaded; the
    // file that is already loaded is not loaded again (reset() restarts it).
    // An empty source is the same as clearLottie().
    void setLottieSource(const std::string& source);
    std::string getLottieSource() { return m_source; }
    bool isLoaded() { return m_animation != nullptr; }
    // Drops the animation, its frames and its texture right away (destroy()
    // does it too).
    void clearLottie();

    void setPaused(bool paused);
    void play() { setPaused(false); }
    void pause() { setPaused(true); }
    bool isPaused() { return m_paused; }

    void setLoop(bool loop) { m_loop = loop; }
    bool isLoop() { return m_loop; }
    void setSpeed(double speed);
    double getSpeed() { return m_speed; }
    // false (default): the whole animation fits inside the widget, keeping its
    // aspect ratio. true: it fills the widget, keeping the aspect ratio, and
    // the parts outside are cut.
    void setCoverScaling(bool coverScaling) { if (coverScaling != m_coverScaling) { m_coverScaling = coverScaling; m_hasDisplayedFrame = false; } }

    // Frame numbers are 0 .. getTotalFrames() - 1. seek stays inside the
    // segment that is playing; reset goes back to its first frame.
    void seek(int frame);
    void reset();

    // Plays the [start, start + duration) range of a marker exported with the
    // animation (After Effects / LottieFiles markers), or an explicit range
    // [fromFrame, toFrame). The end of the range loops or ends like the whole
    // animation.
    void playSegment(const std::string& markerName);
    void playFrames(int fromFrame, int toFrame);
    std::vector<std::string> getMarkers();

    int getCurrentFrame() { return (int)m_currentFrame; }
    int getTotalFrames() { return (int)m_totalFrames; }
    double getFrameRate() { return m_frameRate; }
    double getDuration(); // seconds, whole animation
    double getElapsed();  // seconds, position of the current frame

protected:
    void onStyleApply(const std::string& styleName, const OTMLNodePtr& styleNode) override;
    void onReleaseResources() override { clearLottie(); }

private:
    struct RenderJob {
        std::shared_future<ImagePtr> result;
        size_t frame = 0;
        Size size;       // widget size it was made for
        Rect sourceRect; // part of the frame to draw
        std::shared_ptr<std::atomic<bool>> cancelled; // set when the result is no longer wanted
    };

    void loadFile(const std::string& path);
    void anchorClock();
    void advancePlayback();
    void updateRender();
    void computeLayout(Size& surfaceSize, Rect& sourceRect);
    ImagePtr acquireImage(const Size& size);
    void drawFrame();
    void callLuaFieldLater(const std::string& field);

    std::string m_source;
    std::shared_ptr<rlottie::Animation> m_animation;
    Size m_animationSize;

    TexturePtr m_frameTexture;
    std::vector<ImagePtr> m_imagePool;
    RenderJob m_job;               // m_job.result.valid() while a render is in flight
    size_t m_displayedFrame = 0;
    Size m_displayedSize;
    bool m_hasDisplayedFrame = false;
    Rect m_sourceRect;             // part of the frame that is drawn (cover crops)
    CoordsBuffer m_frameCoords;
    Rect m_frameCoordsRect;
    Rect m_frameCoordsSource;

    size_t m_totalFrames = 0;
    double m_frameRate = 60.0;
    size_t m_currentFrame = 0;
    size_t m_segmentStart = 0;     // inclusive
    size_t m_segmentEnd = 0;       // exclusive

    double m_startTime = 0.0;      // g_clock.millis() at which the segment's first frame would have been shown
    double m_speed = 1.0;

    bool m_paused = true;
    bool m_ended = false;
    bool m_loop = true;
    bool m_coverScaling = false;
};

#endif // FW_LOTTIE

#endif

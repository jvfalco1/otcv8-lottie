#include "uilottie.h"

#ifdef FW_LOTTIE

#include <framework/core/asyncdispatcher.h>
#include <framework/core/clock.h>
#include <framework/core/eventdispatcher.h>
#include <framework/core/resourcemanager.h>
#include <framework/graphics/drawqueue.h>
#include <framework/graphics/graphics.h>
#include <framework/graphics/image.h>
#include <framework/graphics/texture.h>
#include <framework/otml/otml.h>

// rlottie's public header only knows two modes on Windows: RLOTTIE_BUILD
// defined -> __declspec(dllexport), undefined -> __declspec(dllimport). The
// vcpkg port is a static library, so the dllimport markers make the linker look
// for __imp_ symbols that do not exist (LNK2001). The port's CMake target sets
// RLOTTIE_BUILD=0 for CMake consumers; the Visual Studio project (vcpkg
// MSBuild integration) does not, so it is set here. For an .exe the dllexport
// markers are harmless: nothing is exported from a static link.
#if defined(_WIN32) && !defined(RLOTTIE_BUILD)
#define RLOTTIE_BUILD 0
#endif
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4251) // the dllexport class has a private std::unique_ptr member
#endif
#include <rlottie.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace {
    // Largest side of a rendered frame. Bigger widgets are drawn from a frame
    // of this size, scaled up (a frame costs width * height * 4 bytes three or
    // four times over: pool, texture, upload).
    const int MaxFrameSide = 2048;

    // Recycled frame images per widget: one being rendered, one waiting for
    // the render thread, one spare.
    const size_t ImagePoolSize = 3;

    // rlottie writes premultiplied ARGB32 in native byte order (on little
    // endian: B, G, R, A in memory). Textures take straight RGBA.
    void argbPremultipliedToRgba(uint32_t* pixels, size_t count)
    {
        for (size_t i = 0; i < count; ++i) {
            const uint32_t v = pixels[i];
            const uint32_t a = (v >> 24) & 0xFFu;
            uint32_t r = (v >> 16) & 0xFFu;
            uint32_t g = (v >> 8) & 0xFFu;
            uint32_t b = v & 0xFFu;
            if (a == 0u) {
                r = g = b = 0u;
            } else if (a != 0xFFu) {
                r = std::min<uint32_t>(255u, (r * 255u + (a >> 1)) / a);
                g = std::min<uint32_t>(255u, (g * 255u + (a >> 1)) / a);
                b = std::min<uint32_t>(255u, (b * 255u + (a >> 1)) / a);
            }
            pixels[i] = r | (g << 8) | (b << 16) | (a << 24);
        }
    }
}

UILottie::UILottie()
{
}

UILottie::~UILottie()
{
}

void UILottie::clearLottie()
{
    // A render in flight holds its own references (animation and image); one
    // still waiting in the queue is skipped. Dropping the future never blocks.
    if (m_job.cancelled)
        m_job.cancelled->store(true, std::memory_order_relaxed);
    m_job = RenderJob();
    m_animation.reset();
    m_animationSize = Size();
    m_source.clear();
    m_frameTexture = nullptr;
    std::vector<ImagePtr>().swap(m_imagePool);
    m_hasDisplayedFrame = false;
    m_displayedSize = Size();
    m_sourceRect = Rect();
    m_frameCoordsRect = Rect();
    m_totalFrames = 0;
    m_frameRate = 60.0;
    m_currentFrame = 0;
    m_segmentStart = 0;
    m_segmentEnd = 0;
    m_ended = false;
}

void UILottie::setLottieSource(const std::string& source)
{
    if (source.empty()) {
        clearLottie();
        return;
    }

    std::string path = source;
    if (path[0] != '/')
        path = "/" + path;
    loadFile(path);
}

void UILottie::loadFile(const std::string& path)
{
    if (m_animation && path == m_source)
        return;

    if (!g_resources.fileExists(path)) {
        g_logger.error(stdext::format("UILottie: file '%s' does not exist", path));
        return;
    }

    std::string data;
    try {
        data = g_resources.readFileContents(path);
    } catch (const std::exception& e) {
        g_logger.error(stdext::format("UILottie: unable to read '%s': %s", path, e.what()));
        return;
    }
    if (data.empty()) {
        g_logger.error(stdext::format("UILottie: '%s' is empty", path));
        return;
    }
    // Only Lottie JSON reaches rlottie. It takes data starting with a zip
    // signature as a dotLottie archive and, when that archive is broken or has
    // a single entry, parses a null pointer (the client closes).
    const size_t firstChar = data.find_first_not_of(" \t\r\n");
    if (firstChar == std::string::npos || data[firstChar] != '{') {
        g_logger.error(stdext::format("UILottie: '%s' is not Lottie JSON (.lottie archives are not supported)", path));
        return;
    }

    // Parsed before anything is cleared, so a broken file keeps the animation
    // that was playing. The path is the key of rlottie's model cache (10
    // entries by default): widgets that load the same file share one parsed,
    // immutable model and get their own renderer.
    std::unique_ptr<rlottie::Animation> animation = rlottie::Animation::loadFromData(std::move(data), path, "", true);
    if (!animation || animation->totalFrame() == 0) {
        g_logger.error(stdext::format("UILottie: '%s' is not a valid Lottie animation", path));
        return;
    }
    size_t width = 0, height = 0;
    animation->size(width, height);
    if (width == 0 || height == 0) {
        g_logger.error(stdext::format("UILottie: '%s' has no size", path));
        return;
    }

    const bool wasPaused = m_paused;
    clearLottie();
    m_source = path;
    m_animation = std::move(animation);
    m_animationSize = Size((int)width, (int)height);
    m_totalFrames = m_animation->totalFrame();
    m_frameRate = m_animation->frameRate();
    if (!(m_frameRate > 0.0))
        m_frameRate = 60.0;
    m_segmentStart = 0;
    m_segmentEnd = m_totalFrames;
    m_paused = wasPaused;
    reset();

    callLuaField("onLoaded");
}

void UILottie::setPaused(bool paused)
{
    if (paused == m_paused)
        return;

    m_paused = paused;
    if (paused)
        return;
    if (m_ended || m_currentFrame < m_segmentStart || m_currentFrame >= m_segmentEnd) {
        m_ended = false;
        m_currentFrame = m_segmentStart;
    }
    anchorClock(); // resume at the frame where it was paused
}

void UILottie::setSpeed(double speed)
{
    m_speed = speed > 0.0 ? speed : 1.0;
    anchorClock(); // keep the current frame
}

void UILottie::seek(int frame)
{
    if (!m_animation || m_segmentEnd <= m_segmentStart)
        return;

    size_t target = (size_t)std::max(frame, 0);
    target = std::max(target, m_segmentStart);
    target = std::min(target, m_segmentEnd - 1);

    m_currentFrame = target;
    m_ended = false;
    anchorClock();
    callLuaFieldLater("onNewFrame");
}

void UILottie::reset()
{
    m_currentFrame = m_segmentStart;
    m_ended = false;
    anchorClock();
}

void UILottie::anchorClock()
{
    // the clock is placed so that "now" is the current frame
    const size_t framesIn = m_currentFrame > m_segmentStart ? m_currentFrame - m_segmentStart : 0;
    m_startTime = (double)g_clock.millis() - (framesIn * 1000.0 / m_frameRate) / m_speed;
}

void UILottie::playSegment(const std::string& markerName)
{
    if (!m_animation)
        return;

    // Markers are in composition frames; playback frames start at the
    // composition's first frame (frameAtPos(0)).
    const int firstFrame = (int)m_animation->frameAtPos(0);
    for (const auto& marker : m_animation->markers()) {
        if (std::get<0>(marker) == markerName) {
            playFrames(std::get<1>(marker) - firstFrame, std::get<2>(marker) - firstFrame);
            return;
        }
    }
    g_logger.error(stdext::format("UILottie: '%s' has no marker '%s'", m_source, markerName));
}

void UILottie::playFrames(int fromFrame, int toFrame)
{
    if (!m_animation || m_totalFrames == 0)
        return;

    size_t from = (size_t)std::max(fromFrame, 0);
    size_t to = (size_t)std::max(toFrame, 0);
    from = std::min(from, m_totalFrames - 1);
    to = std::min(to, m_totalFrames);
    if (to <= from)
        to = from + 1;

    m_segmentStart = from;
    m_segmentEnd = to;
    m_paused = false;
    reset();
}

std::vector<std::string> UILottie::getMarkers()
{
    std::vector<std::string> names;
    if (m_animation) {
        for (const auto& marker : m_animation->markers())
            names.push_back(std::get<0>(marker));
    }
    return names;
}

double UILottie::getDuration()
{
    return m_totalFrames > 0 ? m_totalFrames / m_frameRate : 0.0;
}

double UILottie::getElapsed()
{
    return m_totalFrames > 0 ? m_currentFrame / m_frameRate : 0.0;
}

void UILottie::callLuaFieldLater(const std::string& field)
{
    // Lua handlers never run inside drawSelf: one that destroys or creates
    // widgets would change the parent's child list while it is being drawn.
    if (!hasLuaField(field))
        return;
    std::weak_ptr<UILottie> weakSelf = static_self_cast<UILottie>();
    g_dispatcher.addEvent([weakSelf, field] {
        UILottiePtr self = weakSelf.lock();
        if (self && !self->isDestroyed())
            self->callLuaField(field);
    });
}

void UILottie::drawSelf(Fw::DrawPane drawPane)
{
    if (drawPane != Fw::ForegroundPane)
        return;

    if (m_backgroundColor.aF() > Fw::MIN_ALPHA)
        drawBackground(m_rect);

    if (m_animation) {
        advancePlayback();
        updateRender();
        drawFrame();
    }

    drawBorder(m_rect);
}

void UILottie::advancePlayback()
{
    if (m_paused || m_ended || m_segmentEnd <= m_segmentStart)
        return;

    const double segmentMs = (m_segmentEnd - m_segmentStart) * 1000.0 / m_frameRate;
    const double nowMs = (double)g_clock.millis();
    double elapsedMs = std::max(0.0, (nowMs - m_startTime) * m_speed);

    if (elapsedMs >= segmentMs) {
        if (m_loop) {
            elapsedMs = std::fmod(elapsedMs, segmentMs);
            m_startTime = nowMs - elapsedMs / m_speed;
        } else {
            m_currentFrame = m_segmentEnd - 1;
            m_ended = true;
            m_paused = true;
            callLuaFieldLater("onLottieEnd");
            return;
        }
    }

    size_t target = m_segmentStart + (size_t)(elapsedMs * m_frameRate / 1000.0);
    target = std::min(target, m_segmentEnd - 1);
    if (target != m_currentFrame) {
        m_currentFrame = target;
        callLuaFieldLater("onNewFrame");
    }
}

void UILottie::computeLayout(Size& surfaceSize, Rect& sourceRect)
{
    const Size widgetSize = m_rect.size();
    surfaceSize = widgetSize;
    if (m_coverScaling) {
        // a frame with the animation's aspect ratio that covers the widget
        const double scale = std::max(widgetSize.width() / (double)m_animationSize.width(),
                                      widgetSize.height() / (double)m_animationSize.height());
        surfaceSize = Size(std::max(1, (int)std::ceil(m_animationSize.width() * scale)),
                           std::max(1, (int)std::ceil(m_animationSize.height() * scale)));
    }

    double down = 1.0;
    const int maxSide = std::min(MaxFrameSide, g_graphics.getMaxTextureSize());
    if (surfaceSize.width() > maxSide || surfaceSize.height() > maxSide)
        down = std::min(maxSide / (double)surfaceSize.width(), maxSide / (double)surfaceSize.height());
    if (down < 1.0)
        surfaceSize = Size(std::max(1, (int)(surfaceSize.width() * down)), std::max(1, (int)(surfaceSize.height() * down)));

    if (m_coverScaling) {
        // the middle of the frame, with the widget's aspect ratio
        const Size visible(std::min(surfaceSize.width(), std::max(1, (int)std::lround(widgetSize.width() * down))),
                           std::min(surfaceSize.height(), std::max(1, (int)std::lround(widgetSize.height() * down))));
        sourceRect = Rect((surfaceSize.width() - visible.width()) / 2, (surfaceSize.height() - visible.height()) / 2, visible);
    } else {
        // rlottie centers the whole animation in the frame (keepAspectRatio)
        sourceRect = Rect(Point(0, 0), surfaceSize);
    }
}

ImagePtr UILottie::acquireImage(const Size& size)
{
    // Free = only the pool holds it: not being rendered, not waiting for the
    // render thread (Texture::update drops its reference after the upload).
    for (const ImagePtr& image : m_imagePool) {
        if (image.use_count() == 1 && image->getSize() == size) {
            std::atomic_thread_fence(std::memory_order_acquire);
            return image;
        }
    }
    m_imagePool.erase(std::remove_if(m_imagePool.begin(), m_imagePool.end(), [&size](const ImagePtr& image) {
        return image->getSize() != size && image.use_count() == 1;
    }), m_imagePool.end());

    ImagePtr image = std::make_shared<Image>(size, 4);
    if (m_imagePool.size() < ImagePoolSize)
        m_imagePool.push_back(image);
    return image;
}

void UILottie::updateRender()
{
    // pick up a finished frame
    if (m_job.result.valid() && m_job.result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        ImagePtr image;
        try {
            image = m_job.result.get();
        } catch (...) {
            // broken promise: the async dispatcher is shutting down
        }
        const RenderJob job = m_job;
        m_job = RenderJob();
        if (image) {
            if (!m_frameTexture) {
                m_frameTexture = std::make_shared<Texture>(image);
                m_frameTexture->setCanCache(false); // new pixels all the time: keep it out of the atlas
            } else {
                m_frameTexture->updatePixels(image);
            }
            m_sourceRect = job.sourceRect;
            m_displayedFrame = job.frame;
            m_displayedSize = job.size;
            m_hasDisplayedFrame = true;
        }
    }

    // one render in flight per widget; the next one starts when the frame on
    // screen is not the current one or was made for another size
    if (m_job.result.valid() || !m_rect.isValid())
        return;
    if (m_hasDisplayedFrame && m_displayedFrame == m_currentFrame && m_displayedSize == m_rect.size())
        return;

    Size surfaceSize;
    Rect sourceRect;
    computeLayout(surfaceSize, sourceRect);

    const ImagePtr image = acquireImage(surfaceSize);
    const std::shared_ptr<rlottie::Animation> animation = m_animation;
    const size_t frame = m_currentFrame;
    m_job.frame = frame;
    m_job.size = m_rect.size();
    m_job.sourceRect = sourceRect;
    m_job.cancelled = std::make_shared<std::atomic<bool>>(false);
    const std::shared_ptr<std::atomic<bool>> cancelled = m_job.cancelled;
    m_job.result = g_asyncDispatcher.schedule([animation, image, frame, cancelled]() -> ImagePtr {
        if (cancelled->load(std::memory_order_relaxed))
            return nullptr; // the widget was destroyed or changed file meanwhile
        const Size& size = image->getSize();
        const size_t count = (size_t)size.area();
        uint32_t* pixels = reinterpret_cast<uint32_t*>(image->getPixelData());
        std::fill(pixels, pixels + count, 0u);
        rlottie::Surface surface(pixels, (size_t)size.width(), (size_t)size.height(), (size_t)size.width() * 4u);
        animation->renderSync(frame, surface);
        argbPremultipliedToRgba(pixels, count);
        return image;
    });
}

void UILottie::drawFrame()
{
    if (!m_frameTexture || !m_rect.isValid() || !m_sourceRect.isValid())
        return;

    if (m_frameCoordsRect != m_rect || m_frameCoordsSource != m_sourceRect) {
        m_frameCoords.clear();
        m_frameCoords.addRect(m_rect, m_sourceRect);
        m_frameCoordsRect = m_rect;
        m_frameCoordsSource = m_sourceRect;
    }

    m_frameTexture->setSmooth(m_imageSmooth);
    if (!m_shader.empty())
        g_drawQueue->add(new DrawQueueItemImageWithShader(m_frameCoords, m_frameTexture, m_imageColor, m_shader));
    else
        g_drawQueue->addTextureCoords(m_frameCoords, m_frameTexture, m_imageColor);
}

void UILottie::onStyleApply(const std::string& styleName, const OTMLNodePtr& styleNode)
{
    UIWidget::onStyleApply(styleName, styleNode);

    for (const OTMLNodePtr& node : styleNode->children()) {
        if (node->tag() == "lottie-source")
            loadFile(stdext::resolve_path(node->value(), node->source()));
        else if (node->tag() == "lottie-loop")
            setLoop(node->value<bool>());
        else if (node->tag() == "lottie-speed")
            setSpeed(node->value<double>());
        else if (node->tag() == "lottie-cover")
            setCoverScaling(node->value<bool>());
        else if (node->tag() == "lottie-autoplay")
            setPaused(!node->value<bool>());
    }
}

#endif // FW_LOTTIE

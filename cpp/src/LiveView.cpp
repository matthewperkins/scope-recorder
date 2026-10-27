#include "LiveView.h"

#include <algorithm>

// Qt's GL headers must come before CUDA's GL interop header.
#include <cuda_gl_interop.h>

#include "Camera.h"
#include "GpuFrames.h"

namespace {

// Full-window quad from 4 vertices; `scale` letterboxes it to the image aspect ratio.
constexpr const char* kVertexShader = R"(#version 330 core
uniform vec2 scale;
out vec2 uv;
void main() {
    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    uv = vec2(corner.x, 1.0 - corner.y);  // image row 0 is the top
    gl_Position = vec4((corner * 2.0 - 1.0) * scale, 0.0, 1.0);
})";

constexpr const char* kFragmentShader = R"(#version 330 core
uniform sampler2D image;
in vec2 uv;
out vec4 color;
void main() { color = texture(image, uv); })";

}  // namespace

LiveView::LiveView(Camera* camera, QWidget* parent)
    : QOpenGLWidget(parent), camera_(camera)
{
    // Repaint whenever the camera publishes a frame (signal arrives queued from IC4's thread).
    connect(camera_, &Camera::frameReady, this, qOverload<>(&QWidget::update));
}

LiveView::~LiveView()
{
    makeCurrent();
    releaseTexture();
    if (stream_)
        cudaStreamDestroy(stream_);
    vao_.destroy();
    doneCurrent();
}

void LiveView::initializeGL()
{
    initializeOpenGLFunctions();
    program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    if (!program_.link())
        qCritical("Live view shader failed: %s", qPrintable(program_.log()));
    vao_.create();
    CUDA_CHECK(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking));
}

void LiveView::ensureTexture(int width, int height)
{
    if (texture_ && width == texWidth_ && height == texHeight_)
        return;
    releaseTexture();
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    CUDA_CHECK(cudaGraphicsGLRegisterImage(&cudaTexture_, texture_, GL_TEXTURE_2D,
                                           cudaGraphicsRegisterFlagsWriteDiscard));
    texWidth_ = width;
    texHeight_ = height;
}

void LiveView::releaseTexture()
{
    if (cudaTexture_)
        cudaGraphicsUnregisterResource(cudaTexture_);
    cudaTexture_ = nullptr;
    if (texture_)
        glDeleteTextures(1, &texture_);
    texture_ = 0;
    texWidth_ = texHeight_ = 0;
}

bool LiveView::uploadLatestFrame()
{
    const GpuFrame frame = camera_->latestFrame();
    if (frame.sequence == 0)
        return false;
    if (frame.sequence == shownSequence_)
        return true;  // nothing new; redraw the current texture

    ensureTexture(frame.width, frame.height);
    cudaArray_t array = nullptr;
    CUDA_CHECK(cudaGraphicsMapResources(1, &cudaTexture_, stream_));
    CUDA_CHECK(cudaGraphicsSubResourceGetMappedArray(&array, cudaTexture_, 0, 0));
    CUDA_CHECK(cudaStreamWaitEvent(stream_, frame.ready));
    CUDA_CHECK(cudaMemcpy2DToArrayAsync(array, 0, 0, frame.rgba, frame.pitch, size_t(frame.width) * sizeof(uchar4),
                                        frame.height, cudaMemcpyDeviceToDevice, stream_));
    CUDA_CHECK(cudaGraphicsUnmapResources(1, &cudaTexture_, stream_));
    CUDA_CHECK(cudaStreamSynchronize(stream_));  // finish before the camera can reuse this ring slot
    shownSequence_ = frame.sequence;
    ++framesShown_;
    return true;
}

void LiveView::paintGL()
{
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    bool haveImage = false;
    try {
        haveImage = uploadLatestFrame();
    } catch (const std::exception& e) {
        qCritical("Live view: %s", e.what());
    }
    if (!haveImage || !texture_)
        return;

    // Fit the image inside the widget, keeping its aspect ratio.
    const float widgetAspect = float(width()) / std::max(1, height());
    const float imageAspect = float(texWidth_) / texHeight_;
    const float sx = imageAspect < widgetAspect ? imageAspect / widgetAspect : 1.f;
    const float sy = imageAspect < widgetAspect ? 1.f : widgetAspect / imageAspect;

    program_.bind();
    program_.setUniformValue("scale", sx, sy);
    program_.setUniformValue("image", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    vao_.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    vao_.release();
    program_.release();
}

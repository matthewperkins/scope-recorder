#pragma once

#include <cstdint>

#include <QOpenGLBuffer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>

#include <cuda_runtime.h>

class Camera;
struct cudaGraphicsResource;

// Displays the camera's GPU frames. The debayered image is copied GPU-to-GPU into an
// OpenGL texture through CUDA-GL interop, so live view costs no CPU pixel work.
class LiveView : public QOpenGLWidget, protected QOpenGLExtraFunctions {
    Q_OBJECT
public:
    explicit LiveView(Camera* camera, QWidget* parent = nullptr);
    ~LiveView() override;

    uint64_t framesShown() const { return framesShown_; }

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    bool uploadLatestFrame();
    void ensureTexture(int width, int height);
    void releaseTexture();

    Camera* camera_;
    QOpenGLShaderProgram program_;
    QOpenGLVertexArrayObject vao_;
    GLuint texture_ = 0;
    int texWidth_ = 0, texHeight_ = 0;
    cudaGraphicsResource* cudaTexture_ = nullptr;
    cudaStream_t stream_ = nullptr;
    uint64_t shownSequence_ = 0;
    uint64_t framesShown_ = 0;
};

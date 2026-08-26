#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QFileDialog>
#include <QPainter>
#include <QDebug>
#include <QAudioFormat>
#include <chrono>


#define OUTSAMPLINGRATE 48000
//================ PacketQueue队列成员函数实现 =================
void PacketQueue::push(AVPacket *pkt)
{
    std::lock_guard<std::mutex> lk(mtx);
    queue.push(pkt);
    cond.notify_one();
}

AVPacket* PacketQueue::pop(int timeoutMs)
{
    std::unique_lock<std::mutex> lk(mtx);
    auto tp = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool ok = cond.wait_until(lk, tp, [this](){
        return !queue.empty() || quit;
    });
    if(!ok || queue.empty() || quit)
        return nullptr;

    AVPacket* p = queue.front();
    queue.pop();
    return p;
}

void PacketQueue::flush()
{
    std::lock_guard<std::mutex> lk(mtx);
    while(!queue.empty())
    {
        AVPacket* pkt = queue.front();
        queue.pop();
        av_packet_free(&pkt);
    }
    cond.notify_all();
}

int PacketQueue::size()
{
    std::lock_guard<std::mutex> lk(mtx);
    return static_cast<int>(queue.size());
}

//================ VideoGlWidget OpenGL渲染实现 =================
VideoGlWidget::VideoGlWidget(QWidget *parent)
    :QOpenGLWidget(parent)
{
    setUpdateBehavior(QOpenGLWidget::PartialUpdate);
}

VideoGlWidget::~VideoGlWidget()
{
    makeCurrent();
    delete m_texY;
    delete m_texU;
    delete m_texV;
    delete m_program;
    delete m_vbo;
    if(m_vao)
    {
        m_vao->destroy();
        delete m_vao;
    }
    doneCurrent();
}

/**
 * @brief submitYuvFrame 子线程调用：把 I420 紧凑拷贝到 CPU 缓冲（不做 RGB 转换）
 * YUV→RGB 由 paintGL 里的 GLSL 在 GPU 完成
 */
void VideoGlWidget::submitYuvFrame(int w, int h,
                                   const uint8_t *yData, int yStride,
                                   const uint8_t *uData, int uStride,
                                   const uint8_t *vData, int vStride)
{
    if(w <= 0 || h <= 0 || !yData || !uData || !vData)
        return;

    std::lock_guard<std::mutex> lk(m_yuvMtx);
    m_width = w;
    m_height = h;

    // 按像素宽紧凑存储，便于 GPU 按 width×height 上传（去掉 decoder stride 填充）
    m_yBuf.resize(w * h);
    m_uBuf.resize((w / 2) * (h / 2));
    m_vBuf.resize((w / 2) * (h / 2));

    for(int i = 0; i < h; ++i)
        memcpy(m_yBuf.data() + i * w, yData + i * yStride, size_t(w));
    for(int i = 0; i < h / 2; ++i)
    {
        memcpy(m_uBuf.data() + i * (w / 2), uData + i * uStride, size_t(w / 2));
        memcpy(m_vBuf.data() + i * (w / 2), vData + i * vStride, size_t(w / 2));
    }

    m_hasNewFrame = true;
}

void VideoGlWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0, 0, 0, 1.0f);

    // 顶点着色器：全屏矩形
    const char* vSrc = R"(
        #version 330 core
        layout(location=0) in vec2 pos;
        layout(location=1) in vec2 texCoord;
        out vec2 vTex;
        void main(){
            gl_Position = vec4(pos, 0.0, 1.0);
            vTex = texCoord;
        }
    )";
    // 片段着色器：I420 YUV420P → RGB（GPU 完成，CPU 不做 sws RGB 转换）
    const char* fSrc = R"(
        #version 330 core
        in vec2 vTex;
        uniform sampler2D texY;
        uniform sampler2D texU;
        uniform sampler2D texV;
        out vec4 fragColor;
        void main(){
            float y = texture(texY, vTex).r;
            float u = texture(texU, vTex).r - 0.5;
            float v = texture(texV, vTex).r - 0.5;
            float r = y + 1.402 * v;
            float g = y - 0.344136 * u - 0.714136 * v;
            float b = y + 1.772 * u;
            fragColor = vec4(r, g, b, 1.0);
        }
    )";

    m_program = new QOpenGLShaderProgram(this);
    if(!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vSrc))
        qWarning("Vertex shader error: %s", qPrintable(m_program->log()));
    if(!m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fSrc))
        qWarning("Fragment shader error: %s", qPrintable(m_program->log()));
    if(!m_program->link())
        qWarning("Shader link error: %s", qPrintable(m_program->log()));

    m_texY = new QOpenGLTexture(QOpenGLTexture::Target2D);
    m_texU = new QOpenGLTexture(QOpenGLTexture::Target2D);
    m_texV = new QOpenGLTexture(QOpenGLTexture::Target2D);
    for(auto t : {m_texY, m_texU, m_texV})
    {
        t->setFormat(QOpenGLTexture::R8_UNorm);
        t->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
        t->setWrapMode(QOpenGLTexture::ClampToEdge);
    }

    m_program->bind();
    m_program->setUniformValue("texY", 0);
    m_program->setUniformValue("texU", 1);
    m_program->setUniformValue("texV", 2);
    m_program->release();
}

void VideoGlWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    std::lock_guard<std::mutex> lk(m_yuvMtx);
    if(m_width <= 0 || m_height <= 0 || !m_program || !m_program->isLinked())
        return;

    // 有新帧才上传纹理；无新帧也继续用上一帧绘制（避免闪黑）
    if(m_hasNewFrame)
    {
        auto uploadPlane = [](QOpenGLTexture* tex, int w, int h, const void* data) {
            if(!tex->isCreated())
                tex->create();
            if(tex->width() != w || tex->height() != h || !tex->isStorageAllocated())
            {
                tex->destroy();
                tex->create();
                tex->setFormat(QOpenGLTexture::R8_UNorm);
                tex->setSize(w, h);
                tex->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
                tex->setWrapMode(QOpenGLTexture::ClampToEdge);
                tex->allocateStorage(QOpenGLTexture::Red, QOpenGLTexture::UInt8);
            }
            tex->setData(QOpenGLTexture::Red, QOpenGLTexture::UInt8, data);
        };

        uploadPlane(m_texY, m_width, m_height, m_yBuf.constData());
        uploadPlane(m_texU, m_width / 2, m_height / 2, m_uBuf.constData());
        uploadPlane(m_texV, m_width / 2, m_height / 2, m_vBuf.constData());
        m_hasNewFrame = false;
        m_texReady = true;
    }

    if(!m_texReady)
        return;

    m_program->bind();
    glActiveTexture(GL_TEXTURE0); m_texY->bind();
    glActiveTexture(GL_TEXTURE1); m_texU->bind();
    glActiveTexture(GL_TEXTURE2); m_texV->bind();

    // Core Profile 不能用客户端顶点数组，改用 VBO
    if(!m_vbo)
    {
        m_vbo = new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
        m_vbo->create();
        m_vbo->bind();
        static const float vert[] = {
            -1.0f, -1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 0.0f,
        };
        m_vbo->allocate(vert, sizeof(vert));
    }
    else
    {
        m_vbo->bind();
    }

    if(!m_vao)
    {
        m_vao = new QOpenGLVertexArrayObject(this);
        m_vao->create();
    }
    m_vao->bind();

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void*>(8));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    m_vao->release();
    m_vbo->release();
    m_texV->release();
    m_texU->release();
    m_texY->release();
    m_program->release();
}

void VideoGlWidget::resizeGL(int w, int h)
{
    glViewport(0,0,w,h);
}

//================ MainWindow实现 =================
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    resize(1000,700);

    // 创建OpenGL控件，占据上方视频区域（GPU 渲染）
    m_glWidget = new VideoGlWidget(ui->centralwidget);
    auto* lay = qobject_cast<QVBoxLayout*>(ui->centralwidget->layout());
    if(lay)
    {
        // 去掉占位弹簧，把拉伸空间全给 GL 控件
        if(ui->verticalSpacer)
        {
            lay->removeItem(ui->verticalSpacer);
            delete ui->verticalSpacer;
            ui->verticalSpacer = nullptr;
        }
        lay->insertWidget(0, m_glWidget, /*stretch*/ 1);
    }

    // update() 有多个重载，必须消歧义
    connect(this, &MainWindow::sigUpdateVideo, m_glWidget,
            static_cast<void (QWidget::*)()>(&QWidget::update));
    connect(this,&MainWindow::sigDurationChanged,this,&MainWindow::slotSetDuration);
    connect(this,&MainWindow::sigTimeChanged,this,&MainWindow::slotSetCurrentTime);
    connect(this,&MainWindow::sigPlayEnd,this,&MainWindow::slotPlayFinished);

    // 配置音频输出格式：44100Hz，双声道，16位小端s16le
    QAudioFormat fmt;
    fmt.setSampleRate(OUTSAMPLINGRATE);
    fmt.setChannelCount(2);
    fmt.setSampleSize(16);
    fmt.setCodec(QStringLiteral("audio/pcm"));
    fmt.setByteOrder(QAudioFormat::LittleEndian);
    fmt.setSampleType(QAudioFormat::SignedInt);
    m_audioOutput = new QAudioOutput(fmt,this);
    m_audioOutput->setBufferSize(OUTSAMPLINGRATE * 2 * 2 / 10);
    m_audioIODevice = m_audioOutput->start();
}

MainWindow::~MainWindow()
{
    closeMedia();
    delete ui;
}

qint64 MainWindow::ptsToMs(int64_t pts, AVRational timebase)
{
    return qint64(pts * av_q2d(timebase)*1000);
}

double MainWindow::getAudioClock()
{
    std::lock_guard<std::mutex> lk(m_clockMtx);
    if(!m_audioOutput)
        return m_audioClock;

    const double bytesPerSec = OUTSAMPLINGRATE * 2.0 * 2.0;
    const int bufferedBytes = qMax(0, m_audioOutput->bufferSize() - m_audioOutput->bytesFree());
    const double bufferedSec = bufferedBytes / bytesPerSec;
    return m_audioClock - bufferedSec;
}

void MainWindow::setAudioClock(double ptsSec)
{
    std::lock_guard<std::mutex> lk(m_clockMtx);
    m_audioClock = ptsSec;
}

bool MainWindow::openMedia(const QString &filePath)
{
    closeMedia();

    int ret = avformat_open_input(&m_fmtCtx, filePath.toUtf8().data(), nullptr, nullptr);
    if(ret <0){ qDebug()<<"avformat_open_input failed"; return false; }

    ret = avformat_find_stream_info(m_fmtCtx,nullptr);
    if(ret <0){ qDebug()<<"avformat_find_stream_info failed"; return false; }

    // --------初始化视频流--------
    m_videoStreamIdx = av_find_best_stream(m_fmtCtx,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
    if(m_videoStreamIdx >=0)
    {
        AVStream* vs = m_fmtCtx->streams[m_videoStreamIdx];
        const AVCodec* vcodec = avcodec_find_decoder(vs->codecpar->codec_id);
        m_videoCodecCtx = avcodec_alloc_context3(vcodec);
        avcodec_parameters_to_context(m_videoCodecCtx, vs->codecpar);
        avcodec_open2(m_videoCodecCtx, vcodec, nullptr);

        int w = m_videoCodecCtx->width;
        int h = m_videoCodecCtx->height;

        // 仅当不是 I420/J420 时，才用 sws 转到 YUV420P（仍不做 RGB；RGB 在 GPU shader）
        const AVPixelFormat pf = m_videoCodecCtx->pix_fmt;
        if(pf != AV_PIX_FMT_YUV420P && pf != AV_PIX_FMT_YUVJ420P)
        {
            m_swsCtx = sws_getContext(w, h, pf,
                                      w, h, AV_PIX_FMT_YUV420P,
                                      SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            qDebug() << "pixel format" << pf << "-> YUV420P (CPU format only, RGB on GPU)";
        }
    }

    // --------初始化音频流--------
    m_audioStreamIdx = av_find_best_stream(m_fmtCtx,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
    if(m_audioStreamIdx >=0)
    {
        AVStream* as = m_fmtCtx->streams[m_audioStreamIdx];
        const AVCodec* acodec = avcodec_find_decoder(as->codecpar->codec_id);
        m_audioCodecCtx = avcodec_alloc_context3(acodec);
        avcodec_parameters_to_context(m_audioCodecCtx,as->codecpar);
        avcodec_open2(m_audioCodecCtx,acodec,nullptr);

        AVChannelLayout outLayout;
        av_channel_layout_from_mask(&outLayout, AV_CH_LAYOUT_STEREO);
        swr_alloc_set_opts2(&m_swrCtx,
                            &outLayout, AV_SAMPLE_FMT_S16, OUTSAMPLINGRATE,
                            &m_audioCodecCtx->ch_layout, m_audioCodecCtx->sample_fmt, m_audioCodecCtx->sample_rate,
                            0, nullptr);
        av_channel_layout_uninit(&outLayout);
        swr_init(m_swrCtx);
    }

    if(m_fmtCtx->duration != AV_NOPTS_VALUE)
    {
        m_totalMs = qint64(m_fmtCtx->duration / (double)AV_TIME_BASE *1000);
        emit sigDurationChanged(m_totalMs);
    }

    m_threadQuit = false;
    m_isPaused = false;
    m_seekReq = false;
    m_seekSerial = 0;
    m_seekTargetSec = 0.0;
    m_flushVideoCodec = false;
    m_flushAudioCodec = false;
    m_sliderDragging = false;
    m_audioWriting = false;
    m_audioSuspend = false;
    m_forceShowFrame = false;
    m_lastUiTimeMs = -1;
    m_videoPktQueue.quit = false;
    m_audioPktQueue.quit = false;

    m_thDemux     = std::thread(&MainWindow::demuxWork,this);
    m_thVideoDec  = std::thread(&MainWindow::videoDecodeWork,this);
    m_thAudioDec  = std::thread(&MainWindow::audioDecodeWork,this);

    return true;
}

void MainWindow::closeMedia()
{
    m_threadQuit = true;
    m_videoPktQueue.quit = true;
    m_audioPktQueue.quit = true;
    m_videoPktQueue.cond.notify_all();
    m_audioPktQueue.cond.notify_all();

    if(m_thDemux.joinable())     m_thDemux.join();
    if(m_thVideoDec.joinable())  m_thVideoDec.join();
    if(m_thAudioDec.joinable())  m_thAudioDec.join();

    m_videoPktQueue.flush();
    m_audioPktQueue.flush();

    if(m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if(m_swrCtx) swr_free(&m_swrCtx);
    if(m_videoCodecCtx) avcodec_free_context(&m_videoCodecCtx);
    if(m_audioCodecCtx) avcodec_free_context(&m_audioCodecCtx);
    if(m_fmtCtx) avformat_close_input(&m_fmtCtx);

    m_videoStreamIdx = -1;
    m_audioStreamIdx = -1;
    m_totalMs =0;
    m_audioClock =0;
    m_lastUiTimeMs = -1;

    emit sigDurationChanged(0);
    emit sigTimeChanged(0);
}

void MainWindow::demuxWork()
{
    AVPacket* pkt = av_packet_alloc();
    while(!m_threadQuit)
    {
        if(m_seekReq.exchange(false))
        {
            const qint64 ms = m_seekMs.load();
            const int64_t tsUs = ms * 1000;
            int ret = av_seek_frame(m_fmtCtx, -1, tsUs, AVSEEK_FLAG_BACKWARD);
            if(ret < 0)
            {
                if(m_videoStreamIdx >= 0)
                {
                    const AVRational msBase{1, 1000};
                    const int64_t ts = av_rescale_q(ms, msBase,
                                                   m_fmtCtx->streams[m_videoStreamIdx]->time_base);
                    av_seek_frame(m_fmtCtx, m_videoStreamIdx, ts, AVSEEK_FLAG_BACKWARD);
                }
            }

            m_videoPktQueue.flush();
            m_audioPktQueue.flush();
            m_flushVideoCodec = true;
            m_flushAudioCodec = true;
            m_seekTargetSec = ms / 1000.0;
            setAudioClock(ms / 1000.0);
            m_forceShowFrame = true;
            m_audioSuspend = false;
            continue;
        }

        if(m_isPaused)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        while(!m_threadQuit && !m_seekReq
              && (m_videoPktQueue.size() + m_audioPktQueue.size()) > 50)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if(m_threadQuit || m_seekReq)
            continue;

        int ret = av_read_frame(m_fmtCtx, pkt);
        if(ret < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if(pkt->stream_index == m_videoStreamIdx)
            m_videoPktQueue.push(av_packet_clone(pkt));
        else if(pkt->stream_index == m_audioStreamIdx)
            m_audioPktQueue.push(av_packet_clone(pkt));
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

void MainWindow::videoDecodeWork()
{
    AVFrame* frame = av_frame_alloc();
    // 临时帧：当原始格式不是I420P，sws转换输出到此
    AVFrame* frameI420 = av_frame_alloc();

    while(!m_threadQuit)
    {
        if(m_flushVideoCodec.exchange(false) && m_videoCodecCtx)
        {
            avcodec_flush_buffers(m_videoCodecCtx);
            m_forceShowFrame = true;
        }

        if(m_isPaused)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        AVPacket* pkt = m_videoPktQueue.pop(100);
        if(!pkt) continue;

        avcodec_send_packet(m_videoCodecCtx, pkt);
        av_packet_free(&pkt);

        while(true)
        {
            int ret = avcodec_receive_frame(m_videoCodecCtx, frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if(ret <0) break;

            int64_t vts = frame->pts;
            if(vts == AV_NOPTS_VALUE)
                vts = frame->best_effort_timestamp;
            if(vts == AV_NOPTS_VALUE)
                continue;
            double videoPtsSec = vts * av_q2d(m_fmtCtx->streams[m_videoStreamIdx]->time_base);
            const int serial = m_seekSerial.load();
            const double seekTarget = m_seekTargetSec.load();

            if(videoPtsSec + 0.05 < seekTarget)
                continue;

            bool forceShow = m_forceShowFrame.load();
            if(forceShow)
            {
                setAudioClock(videoPtsSec);
            }
            else
            {
                double audioNow = getAudioClock();
                double diff = videoPtsSec - audioNow;

                if(diff > 0.025)
                {
                    long long waitUs = static_cast<long long>(qMin(diff, 0.04) * 1000000);
                    while(waitUs > 0 && !m_threadQuit && m_seekSerial.load() == serial)
                    {
                        const long long step = qMin<long long>(waitUs, 2000);
                        std::this_thread::sleep_for(std::chrono::microseconds(step));
                        waitUs -= step;
                        diff = videoPtsSec - getAudioClock();
                        if(diff <= 0.025)
                            break;
                        waitUs = static_cast<long long>(qMin(diff, 0.04) * 1000000);
                    }
                    if(m_seekSerial.load() != serial)
                        continue;
                }
                else if(diff < -0.3)
                {
                    continue;
                }
            }

            const uint8_t* y,*u,*v;
            int ys,us,vs;
            // 非 I420 时仅做平面格式转换（仍是 YUV）；YUV→RGB 在 GPU
            if(m_swsCtx != nullptr)
            {
                if(!frameI420->data[0]
                    || frameI420->width != frame->width
                    || frameI420->height != frame->height)
                {
                    av_frame_unref(frameI420);
                    frameI420->format = AV_PIX_FMT_YUV420P;
                    frameI420->width = frame->width;
                    frameI420->height = frame->height;
                    if(av_frame_get_buffer(frameI420, 32) < 0)
                        continue;
                }
                sws_scale(m_swsCtx, frame->data, frame->linesize, 0, frame->height,
                          frameI420->data, frameI420->linesize);
                y = frameI420->data[0]; ys = frameI420->linesize[0];
                u = frameI420->data[1]; us = frameI420->linesize[1];
                v = frameI420->data[2]; vs = frameI420->linesize[2];
            }
            else
            {
                y = frame->data[0]; ys = frame->linesize[0];
                u = frame->data[1]; us = frame->linesize[1];
                v = frame->data[2]; vs = frame->linesize[2];
            }

            // 提交I420帧给GL控件，子线程只做内存拷贝
            m_glWidget->submitYuvFrame(
                m_videoCodecCtx->width,
                m_videoCodecCtx->height,
                y, ys,
                u, us,
                v, vs
            );

            emit sigUpdateVideo();
            if(forceShow)
            {
                m_forceShowFrame = false;
                emit sigUpdateVideo();
            }

            const qint64 curMs = qint64(videoPtsSec * 1000);
            if(forceShow || curMs - m_lastUiTimeMs >= 200 || curMs < m_lastUiTimeMs)
            {
                m_lastUiTimeMs = curMs;
                emit sigTimeChanged(curMs);
            }
        }
    }
    av_frame_free(&frame);
    av_frame_free(&frameI420);
}

void MainWindow::audioDecodeWork()
{
    AVFrame* frame = av_frame_alloc();
    while(!m_threadQuit)
    {
        if(m_flushAudioCodec.exchange(false) && m_audioCodecCtx)
            avcodec_flush_buffers(m_audioCodecCtx);

        if(m_isPaused)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        AVPacket* pkt = m_audioPktQueue.pop(100);
        if(!pkt) continue;

        avcodec_send_packet(m_audioCodecCtx,pkt);
        av_packet_free(&pkt);

        while(true)
        {
            int ret = avcodec_receive_frame(m_audioCodecCtx, frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if(ret <0) break;

            int64_t ats = frame->pts;
            if(ats == AV_NOPTS_VALUE)
                ats = frame->best_effort_timestamp;
            double audioPtsSec = 0.0;
            double audioDurSec = frame->nb_samples / qMax(1.0, double(frame->sample_rate));
            if(ats != AV_NOPTS_VALUE)
                audioPtsSec = ats * av_q2d(m_fmtCtx->streams[m_audioStreamIdx]->time_base);
            const int serial = m_seekSerial.load();

            if(ats != AV_NOPTS_VALUE && audioPtsSec + 0.05 < m_seekTargetSec.load())
                continue;

            uint8_t* outData[2]{nullptr,nullptr};
            int outSamples = swr_get_out_samples(m_swrCtx, frame->nb_samples);
            av_samples_alloc(outData,nullptr,2,outSamples,AV_SAMPLE_FMT_S16,0);

            int conv = swr_convert(m_swrCtx, outData, outSamples,
                                   (const uint8_t**)frame->data, frame->nb_samples);
            if(conv>0 && m_audioIODevice)
            {
                int bytes = av_samples_get_buffer_size(nullptr,2,conv,AV_SAMPLE_FMT_S16,1);
                const char* p = reinterpret_cast<const char*>(outData[0]);

                if(m_audioSuspend.load() || m_seekReq.load() || m_seekSerial.load() != serial)
                {
                    av_freep(&outData[0]);
                    break;
                }

                m_audioWriting = true;
                bool aborted = false;
                while(bytes > 0 && !m_threadQuit)
                {
                    if(m_audioSuspend.load() || m_seekSerial.load() != serial || m_seekReq.load())
                    {
                        aborted = true;
                        break;
                    }
                    const int freeBytes = m_audioOutput->bytesFree();
                    if(freeBytes <= 0)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }
                    const int n = qMin(bytes, freeBytes);
                    const qint64 written = m_audioIODevice->write(p, n);
                    if(written <= 0)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }
                    p += written;
                    bytes -= static_cast<int>(written);
                }
                m_audioWriting = false;

                if(!aborted && ats != AV_NOPTS_VALUE && m_seekSerial.load() == serial && !m_audioSuspend.load())
                    setAudioClock(audioPtsSec + audioDurSec);
            }
            av_freep(&outData[0]);

            if(m_flushAudioCodec.load() || m_seekReq.load() || m_seekSerial.load() != serial)
                break;
        }
    }
    av_frame_free(&frame);
}

void MainWindow::seekTo(qint64 ms)
{
    if(!m_fmtCtx) return;

    ms = qBound(0LL, ms, qMax(0LL, m_totalMs));

    m_audioSuspend = true;
    m_seekSerial.fetch_add(1);
    m_forceShowFrame = true;
    m_seekMs = ms;
    m_seekTargetSec = ms / 1000.0;
    setAudioClock(ms / 1000.0);
    m_seekReq = true;
}

//====================UI槽函数====================
void MainWindow::slotUpdateVideo()
{
    // 信号直接绑定m_glWidget->update，本槽保留占位
}

void MainWindow::slotSetDuration(qint64 total)
{
    ui->sliderProgress->setRange(0,static_cast<int>(total));
}

void MainWindow::slotSetCurrentTime(qint64 cur)
{
    if(m_sliderDragging)
        return;

    const bool blocked = ui->sliderProgress->blockSignals(true);
    ui->sliderProgress->setValue(static_cast<int>(cur));
    ui->sliderProgress->blockSignals(blocked);
    ui->labelTime->setText(QString("%1 / %2")
                           .arg(cur/1000)
                           .arg(m_totalMs/1000));
}

void MainWindow::slotPlayFinished()
{

}

void MainWindow::on_btnOpen_clicked()
{
    QString f = QFileDialog::getOpenFileName(this,
                                             QStringLiteral("Open Media"),
                                             QString(),
                                             QStringLiteral("Video (*.mp4 *.mkv *.avi *.mov)"));
    if(f.isEmpty()) return;
    openMedia(f);
}

void MainWindow::on_btnPlay_clicked()
{
    m_isPaused = false;
    m_audioOutput->setVolume(1.0);
}

void MainWindow::on_btnPause_clicked()
{
    m_isPaused = true;
}

void MainWindow::on_btnStop_clicked()
{
    closeMedia();
}

void MainWindow::on_sliderProgress_sliderPressed()
{
    m_sliderDragging = true;
}

void MainWindow::on_sliderProgress_sliderMoved(int value)
{
    ui->labelTime->setText(QString("%1 / %2")
                           .arg(value/1000)
                           .arg(m_totalMs/1000));
}

void MainWindow::on_sliderProgress_sliderReleased()
{
    seekTo(ui->sliderProgress->value());
    m_sliderDragging = false;
}

//#ifndef MAINWINDOW_H
//#define MAINWINDOW_H

//#include <QMainWindow>
//#include <QImage>
//#include <QAudioOutput>
//#include <QIODevice>

//#include <thread>         // C++标准线程 std::thread
//#include <mutex>          // 互斥锁
//#include <condition_variable> // 条件变量，队列阻塞等待
//#include <queue>          // 数据包存储队列
//#include <atomic>         // std::atomic

//QT_BEGIN_NAMESPACE
//namespace Ui { class MainWindow; }
//QT_END_NAMESPACE

//// FFmpeg是C库，必须extern "C"防止C++名字改编导致链接失败
//extern "C"
//{
//#include <libavcodec/avcodec.h>
//#include <libavformat/avformat.h>
//#include <libavutil/imgutils.h>
//#include <libavutil/time.h>
//#include <libswscale/swscale.h>
//#include <libswresample/swresample.h>
//}

///**
// * @brief PacketQueue 线程安全AVPacket数据包队列
// * 解复用线程生产包；解码线程消费包
// * mutex保护多线程读写；condition_variable实现阻塞pop
// */
//struct PacketQueue
//{
//    std::queue<AVPacket*> queue;       // 存放压缩数据包AVPacket
//    std::mutex mtx;                    // 互斥锁，保护队列读写
//    std::condition_variable cond;      // 条件变量：队列为空时阻塞消费者线程
//    bool quit{false};                  // 退出标记，通知pop立刻返回

//    // 入队：压入一个数据包
//    void push(AVPacket* pkt);
//    // 出队：超时等待，ms超时；返回nullptr代表无数据或退出
//    AVPacket* pop(int timeoutMs);
//    // 清空队列，释放所有packet内存
//    void flush();
//    // 获取队列当前元素个数
//    int size();
//};

//class MainWindow : public QMainWindow
//{
//    Q_OBJECT
//public:
//    MainWindow(QWidget *parent = nullptr);
//    ~MainWindow() override;

//protected:
//    // Qt窗口绘图事件，把RGB视频图像绘制到窗口
//    void paintEvent(QPaintEvent *event) override;

//signals:
//    // --------子线程发送信号，通知UI主线程更新界面（子线程严禁直接操作UI）--------
//    void sigUpdateVideo();         // 有新的视频帧，需要刷新画面
//    void sigDurationChanged(qint64 totalMs); // 媒体总时长，毫秒
//    void sigTimeChanged(qint64 currentMs);   // 当前播放时间，毫秒
//    void sigPlayEnd();             // 视频播放完毕

//private slots:
//    // UI按钮槽函数
//    void on_btnOpen_clicked();
//    void on_btnPlay_clicked();
//    void on_btnPause_clicked();
//    void on_btnStop_clicked();
//    // 进度条：拖动中只预览时间，松开后再 seek（避免卡死）
//    void on_sliderProgress_sliderPressed();
//    void on_sliderProgress_sliderMoved(int value);
//    void on_sliderProgress_sliderReleased();

//    // 信号对应的UI处理槽
//    void slotUpdateVideo();
//    void slotSetDuration(qint64 total);
//    void slotSetCurrentTime(qint64 cur);
//    void slotPlayFinished();

//private:
//    Ui::MainWindow *ui;

//    // ========= FFmpeg全局上下文 =========
//    AVFormatContext*        m_fmtCtx{nullptr};        // 封装解复用上下文，打开视频文件

//    // 视频相关
//    AVCodecContext*         m_videoCodecCtx{nullptr}; // 视频解码器上下文
//    int                     m_videoStreamIdx{-1};     // 视频流下标
//    SwsContext*             m_swsCtx{nullptr};         // YUV转RGB转换器

//    // 音频相关
//    AVCodecContext*         m_audioCodecCtx{nullptr}; // 音频解码器上下文
//    int                     m_audioStreamIdx{-1};     // 音频流下标
//    SwrContext*             m_swrCtx{nullptr};        // 音频重采样转换器

//    // ========= 线程安全数据包队列 =========
//    PacketQueue             m_videoPktQueue;    // 视频压缩包队列
//    PacketQueue             m_audioPktQueue;    // 音频压缩包队列

//    // ========= 三个C++标准线程 =========
//    std::thread             m_thDemux;     // 解复用线程：av_read_frame读包，分发到两个队列
//    std::thread             m_thVideoDec;  // 视频解码线程：取包解码、音画同步、转RGB、发信号刷新UI
//    std::thread             m_thAudioDec; // 音频解码线程：解码PCM，重采样，音频输出，维护主时钟

//    std::atomic<bool>       m_threadQuit{false}; // 全部线程退出标记，原子变量多线程安全
//    std::atomic<bool>       m_isPaused{false};    // 暂停标记，原子变量

//    // seek：UI只投递请求，由解复用线程执行（禁止UI线程直接 av_seek_frame）
//    std::atomic<bool>       m_seekReq{false};
//    std::atomic<qint64>     m_seekMs{0};
//    std::atomic<double>     m_seekTargetSec{0.0}; // seek 目标秒，用于丢掉关键帧之前的旧画面
//    std::atomic<int>        m_seekSerial{0};      // 每次seek递增，打断音频写入/视频等待
//    std::atomic<bool>       m_flushVideoCodec{false};
//    std::atomic<bool>       m_flushAudioCodec{false};
//    std::atomic<bool>       m_sliderDragging{false};
//    std::atomic<bool>       m_audioWriting{false};  // 音频线程是否正在 write
//    std::atomic<bool>       m_audioSuspend{false};  // seek 期间禁止碰声卡（替代不安全的 reset）
//    std::atomic<bool>       m_forceShowFrame{false}; // seek 后强制显示下一帧并多刷一次

//    // ========= 音画同步：【音频作为主时钟】=========
//    double                  m_audioClock{0.0};   // 最近写入PCM末尾对应的pts(秒)
//    std::mutex              m_clockMtx;            // 保护音频时钟多线程读写

//    // ========= 画面双缓冲（解码线程写 / UI线程读，避免抢同一张图卡顿）=========
//    QImage                  m_frameDisplay;
//    QImage                  m_frameBack;
//    std::mutex              m_frameMtx;
//    std::atomic<qint64>     m_lastUiTimeMs{-1};   // 节流进度条刷新

//    // ========= Qt音频输出 =========
//    QAudioOutput*           m_audioOutput{nullptr};
//    QIODevice*              m_audioIODevice{nullptr};

//    qint64                  m_totalMs{0};         // 媒体总时长，毫秒

//    // 函数声明
//    bool openMedia(const QString& filePath); // 打开媒体文件，初始化ffmpeg、启动3个线程
//    void closeMedia();                       // 关闭媒体，停止线程，释放全部ffmpeg资源

//    // 三个线程入口函数
//    void demuxWork();
//    void videoDecodeWork();
//    void audioDecodeWork();

//    // seek跳转：参数ms 毫秒（仅投递请求）
//    void seekTo(qint64 ms);

//    // 获取经过音频输出偏移校正后的真实音频时钟（主时钟）
//    double getAudioClock();
//    // 设置音频基准时钟
//    void setAudioClock(double ptsSec);

//    // 工具函数：pts + timebase → 转换成毫秒
//    qint64 ptsToMs(int64_t pts, AVRational timebase);
//};
//#endif // MAINWINDOW_H
#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#include <QMainWindow>
#include <QImage>
#include <QAudioOutput>
#include <QIODevice>
#include <thread>         // C++标准线程 std::thread
#include <mutex>          // 互斥锁
#include <condition_variable> // 条件变量，队列阻塞等待
#include <queue>          // 数据包存储队列
#include <atomic>         // std::atomic

// ============【新增】OpenGL渲染头文件 ============
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

// FFmpeg是C库，必须extern "C"防止C++名字改编导致链接失败
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

/**
 * @brief PacketQueue 线程安全AVPacket数据包队列
 * 解复用线程生产包；解码线程消费包
 * mutex保护多线程读写；condition_variable实现阻塞pop
 */
struct PacketQueue
{
    std::queue<AVPacket*> queue;       // 存放压缩数据包AVPacket
    std::mutex mtx;                    // 互斥锁，保护队列读写
    std::condition_variable cond;      // 条件变量：队列为空时阻塞消费者线程
    bool quit{false};                  // 退出标记，通知pop立刻返回
    // 入队：压入一个数据包
    void push(AVPacket* pkt);
    // 出队：超时等待，ms超时；返回nullptr代表无数据或退出
    AVPacket* pop(int timeoutMs);
    // 清空队列，释放所有packet内存
    void flush();
    // 获取队列当前元素个数
    int size();
};

/**
 * @brief VideoGlWidget OpenGL YUV420P(I420)渲染控件
 * 【重要约束】所有OpenGL API只能GUI主线程执行；
 * 子线程只调用 submitYuvFrame，仅做内存拷贝，禁止调用任何GL接口。
 * 功能：接收子线程提交的I420原始YUV，GLSL着色器GPU完成YUV→RGB转换并绘制窗口。
 */
class VideoGlWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit VideoGlWidget(QWidget *parent = nullptr);
    ~VideoGlWidget() override;

    /**
     * @brief submitYuvFrame 子线程安全入口，提交一帧I420(YUV420P)
     * 内部加锁，仅内存拷贝，不调用OpenGL函数
     * @param w 视频像素宽
     * @param h 视频像素高
     * @param yData Y平面原始数据指针
     * @param yStride Y平面行字节跨度(stride，含对齐填充)
     * @param uData U平面原始数据指针
     * @param uStride U平面行字节跨度
     * @param vData V平面原始数据指针
     * @param vStride V平面行字节跨度
     */
    void submitYuvFrame(int w, int h,
                        const uint8_t* yData, int yStride,
                        const uint8_t* uData, int uStride,
                        const uint8_t* vData, int vStride);

protected:
    void initializeGL() override;   // GL初始化，编译shader，GUI主线程
    void paintGL() override;        // 上传纹理、执行渲染，GUI主线程
    void resizeGL(int w, int h) override; // 窗口大小改变，更新视口，GUI主线程

private:
    std::mutex m_yuvMtx;            // 保护CPU侧YUV缓冲：子线程写 / GL主线程读

    // CPU内存缓冲区，拷贝AVFrame的I420数据到此，避免直接引用AVFrame栈内存
    int m_width{0};
    int m_height{0};
    QByteArray m_yBuf;
    QByteArray m_uBuf;
    QByteArray m_vBuf;

    // GPU渲染资源，仅GUI主线程访问
    QOpenGLShaderProgram* m_program{nullptr};
    QOpenGLTexture* m_texY{nullptr};
    QOpenGLTexture* m_texU{nullptr};
    QOpenGLTexture* m_texV{nullptr};
    QOpenGLBuffer* m_vbo{nullptr};
    QOpenGLVertexArrayObject* m_vao{nullptr};

    bool m_hasNewFrame{false};      // 存在尚未上传的新 YUV 帧
    bool m_texReady{false};         // 纹理已上传过，可继续绘制上一帧
};


class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

// =========【改动】删除旧QPainter绘图事件，现在交给VideoGlWidget做OpenGL渲染 =========
//protected:
//    // Qt窗口绘图事件，把RGB视频图像绘制到窗口
//    void paintEvent(QPaintEvent *event) override;

signals:
    // --------子线程发送信号，通知UI主线程更新界面（子线程严禁直接操作UI）--------
    void sigUpdateVideo();         // 有新的视频帧，触发OpenGL控件update()
    void sigDurationChanged(qint64 totalMs); // 媒体总时长，毫秒
    void sigTimeChanged(qint64 currentMs);   // 当前播放时间，毫秒
    void sigPlayEnd();             // 视频播放完毕

private slots:
    // UI按钮槽函数
    void on_btnOpen_clicked();
    void on_btnPlay_clicked();
    void on_btnPause_clicked();
    void on_btnStop_clicked();
    // 进度条：拖动中只预览时间，松开后再 seek（避免卡死）
    void on_sliderProgress_sliderPressed();
    void on_sliderProgress_sliderMoved(int value);
    void on_sliderProgress_sliderReleased();

    // 信号对应的UI处理槽
    void slotUpdateVideo();
    void slotSetDuration(qint64 total);
    void slotSetCurrentTime(qint64 cur);
    void slotPlayFinished();

private:
    Ui::MainWindow *ui;

    // =========【新增】OpenGL视频渲染控件，替代原来QImage+QPainter绘图 =========
    VideoGlWidget* m_glWidget{nullptr};

    // ========= FFmpeg全局上下文 =========
    AVFormatContext*        m_fmtCtx{nullptr};        // 封装解复用上下文，打开视频文件
    // 视频相关
    AVCodecContext*         m_videoCodecCtx{nullptr}; // 视频解码器上下文
    int                     m_videoStreamIdx{-1};     // 视频流下标

    // 【保留】SwsContext：不再做YUV→RGB；仅用于把其它YUV像素格式转为I420(YUV420P)给OpenGL着色器
    SwsContext*             m_swsCtx{nullptr};

    // =========【删除旧的RGB双缓冲QImage整套】=========
    // QImage                  m_frameDisplay;
    // QImage                  m_frameBack;
    // std::mutex              m_frameMtx;

    // 音频相关
    AVCodecContext*         m_audioCodecCtx{nullptr}; // 音频解码器上下文
    int                     m_audioStreamIdx{-1};     // 音频流下标
    SwrContext*             m_swrCtx{nullptr};        // 音频重采样转换器

    // ========= 线程安全数据包队列 =========
    PacketQueue             m_videoPktQueue;    // 视频压缩包队列
    PacketQueue             m_audioPktQueue;    // 音频压缩包队列

    // ========= 三个C++标准线程 =========
    std::thread             m_thDemux;     // 解复用线程：av_read_frame读包，分发到两个队列
    std::thread             m_thVideoDec;  // 视频解码线程：取包解码、音画同步，提交YUV给GL控件
    std::thread             m_thAudioDec; // 音频解码线程：解码PCM，重采样，音频输出，维护主时钟

    std::atomic<bool>       m_threadQuit{false}; // 全部线程退出标记，原子变量多线程安全
    std::atomic<bool>       m_isPaused{false};    // 暂停标记，原子变量

    // seek：UI只投递请求，由解复用线程执行（禁止UI线程直接 av_seek_frame）
    std::atomic<bool>       m_seekReq{false};
    std::atomic<qint64>     m_seekMs{0};
    std::atomic<double>     m_seekTargetSec{0.0}; // seek 目标秒，用于丢掉关键帧之前的旧画面
    std::atomic<int>        m_seekSerial{0};      // 每次seek递增，打断音频写入/视频等待
    std::atomic<bool>       m_flushVideoCodec{false};
    std::atomic<bool>       m_flushAudioCodec{false};
    std::atomic<bool>       m_sliderDragging{false};
    std::atomic<bool>       m_audioWriting{false};  // 音频线程是否正在 write
    std::atomic<bool>       m_audioSuspend{false};  // seek 期间禁止碰声卡（替代不安全的 reset）
    std::atomic<bool>       m_forceShowFrame{false}; // seek 后强制显示下一帧并多刷一次

    // ========= 音画同步：【音频作为主时钟】=========
    double                  m_audioClock{0.0};   // 最近写入PCM末尾对应的pts(秒)
    std::mutex              m_clockMtx;            // 保护音频时钟多线程读写

    std::atomic<qint64>     m_lastUiTimeMs{-1};   // 节流进度条刷新

    // ========= Qt音频输出 =========
    QAudioOutput*           m_audioOutput{nullptr};
    QIODevice*              m_audioIODevice{nullptr};
    qint64                  m_totalMs{0};         // 媒体总时长，毫秒

    // 函数声明
    bool openMedia(const QString& filePath); // 打开媒体文件，初始化ffmpeg、启动3个线程
    void closeMedia();                       // 关闭媒体，停止线程，释放全部ffmpeg资源

    // 三个线程入口函数
    void demuxWork();
    void videoDecodeWork();
    void audioDecodeWork();

    // seek跳转：参数ms 毫秒（仅投递请求）
    void seekTo(qint64 ms);

    // 获取经过音频输出偏移校正后的真实音频时钟（主时钟）
    double getAudioClock();
    // 设置音频基准时钟
    void setAudioClock(double ptsSec);
    // 工具函数：pts + timebase → 转换成毫秒
    qint64 ptsToMs(int64_t pts, AVRational timebase);
};
#endif // MAINWINDOW_H

#ifndef SIMULATOR_H
#define SIMULATOR_H

#include <QObject>
#include <QTcpServer>
#include <QTimer>
#include <QElapsedTimer>
#include <QHostAddress>

// 波形模式：模拟不同光照场景
enum class WaveMode {
    Constant = 0,   // 恒值 2048
    Sine,           // 正弦，10 秒周期（昼夜光照）
    Step,           // 每 5 秒在 500/3500 间跳变（开关灯）
    Noise           // 正弦叠加 ±100 随机抖动
};

// 坏帧注入模式：考察上位机解析鲁棒性
enum class FaultMode {
    None = 0,       // 正常
    HalfPacket,     // 半包：一帧拆两次写，间隔 50ms
    Sticky,         // 粘包：攒 3 帧一次性写出
    Garbage,        // 每帧前混入 5 字节垃圾
    EmptyValue      // 发 "$DATA,#"（空数值）
};

/*
 * 模拟器核心。只负责"连接管理 + 产生字节流"，不碰任何 UI，
 * 因此可以脱离窗口做单元测试（tests/test_simulator.cpp）。
 * 协议对齐 STM32 9-2 工程：$DATA,<0~4095>#\r\n
 */
class SimulatorCore : public QObject
{
    Q_OBJECT
public:
    explicit SimulatorCore(QObject *parent = nullptr);

    // 默认只监听 LocalHost：本工具就是给本机上位机用的，也免去防火墙弹窗
    void start(quint16 port, int intervalMs,
               const QHostAddress &host = QHostAddress::LocalHost);
    void stop();
    bool isRunning() const { return m_running; }

    void setWaveform(WaveMode mode) { m_wave = mode; }
    void setFaultMode(FaultMode mode) { m_fault = mode; }

    // 帧格式与采样值是纯函数，直接公开给单元测试
    static QByteArray makeFrame(qint64 value);
    qint64 nextSample();

    int clientCount() const { return m_clients.size(); }
    qint64 framesSent() const { return m_framesSent; }     // 有效帧数
    qint64 badFramesSent() const { return m_badFramesSent; } // 注入的坏帧数

signals:
    void clientCountChanged(int n);
    void frameSent(const QByteArray &frame);   // 每次发帧（含坏帧）都会发
    void statusMessage(const QString &msg);

private slots:
    void onNewConnection();
    void onClientDisconnected();

private:
    void broadcast();
    void sendRaw(const QByteArray &data);

    QTcpServer *m_server = nullptr;
    QList<QTcpSocket *> m_clients;
    QTimer m_timer;
    QElapsedTimer m_clock;          // 波形时间基准
    bool m_running = false;

    WaveMode m_wave = WaveMode::Sine;
    FaultMode m_fault = FaultMode::None;
    QByteArray m_stickyBuf;         // Sticky 模式的攒帧缓冲
    qint64 m_framesSent = 0;
    qint64 m_badFramesSent = 0;
};

#endif // SIMULATOR_H

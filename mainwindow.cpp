#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTime>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_core(new SimulatorCore(this))
{
    ui->setupUi(this);          // Qt Creator 模板自带的菜单栏/状态栏壳
    buildUi();

    connect(m_core, &SimulatorCore::statusMessage, this, &MainWindow::onStatus);
    connect(m_core, &SimulatorCore::clientCountChanged, this, [this](int n) {
        m_lblClients->setText(QString::number(n));
        m_log->addItem(QStringLiteral("[%1] 客户端数: %2")
                           .arg(QTime::currentTime().toString("hh:mm:ss"), QString::number(n)));
        m_log->scrollToBottom();
    });
    connect(m_core, &SimulatorCore::frameSent, this, [this](const QByteArray &frame) {
        // 高频路径只刷计数，不写日志，避免 10ms 压测时列表撑爆
        m_lblFrames->setText(QString::number(m_core->framesSent()));
        m_lblBad->setText(QString::number(m_core->badFramesSent()));
        if (m_core->badFramesSent() > 0 && m_lblBad->property("lastLogged").toLongLong()
                != m_core->badFramesSent()) {
            m_lblBad->setProperty("lastLogged", m_core->badFramesSent());
            m_log->addItem(QStringLiteral("[%1] 注入坏帧: %2")
                               .arg(QTime::currentTime().toString("hh:mm:ss"),
                                    QString(frame.left(24))));
            m_log->scrollToBottom();
        }
    });
}

MainWindow::~MainWindow()
{
    m_core->stop();
    delete ui;
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);

    auto *form = new QFormLayout;
    m_port = new QSpinBox;
    m_port->setObjectName("spinPort");
    m_port->setRange(1024, 65535);
    m_port->setValue(9000);

    m_interval = new QSpinBox;
    m_interval->setObjectName("spinInterval");
    m_interval->setRange(10, 5000);
    m_interval->setValue(200);
    m_interval->setSuffix(QStringLiteral(" ms/帧"));

    m_wave = new QComboBox;
    m_wave->setObjectName("comboWave");
    m_wave->addItem(QStringLiteral("恒值 2048"),       static_cast<int>(WaveMode::Constant));
    m_wave->addItem(QStringLiteral("正弦（昼夜光照）"), static_cast<int>(WaveMode::Sine));
    m_wave->addItem(QStringLiteral("阶跃（开关灯）"),   static_cast<int>(WaveMode::Step));
    m_wave->addItem(QStringLiteral("正弦+噪声"),       static_cast<int>(WaveMode::Noise));

    m_fault = new QComboBox;
    m_fault->setObjectName("comboFault");
    m_fault->addItem(QStringLiteral("无（正常发帧）"),     static_cast<int>(FaultMode::None));
    m_fault->addItem(QStringLiteral("半包（一帧拆两次）"), static_cast<int>(FaultMode::HalfPacket));
    m_fault->addItem(QStringLiteral("粘包（3帧并一包）"), static_cast<int>(FaultMode::Sticky));
    m_fault->addItem(QStringLiteral("垃圾前缀（5字节）"), static_cast<int>(FaultMode::Garbage));
    m_fault->addItem(QStringLiteral("空值帧 $DATA,#"),    static_cast<int>(FaultMode::EmptyValue));

    form->addRow(QStringLiteral("监听端口"), m_port);
    form->addRow(QStringLiteral("发送间隔"), m_interval);
    form->addRow(QStringLiteral("波形模式"), m_wave);
    form->addRow(QStringLiteral("坏帧注入"), m_fault);
    root->addLayout(form);

    auto *btns = new QHBoxLayout;
    m_btnStart = new QPushButton(QStringLiteral("启动"));
    m_btnStart->setObjectName("btnStart");
    m_btnStop = new QPushButton(QStringLiteral("停止"));
    m_btnStop->setObjectName("btnStop");
    m_btnStop->setEnabled(false);
    btns->addWidget(m_btnStart);
    btns->addWidget(m_btnStop);
    root->addLayout(btns);

    auto *stats = new QHBoxLayout;
    m_lblClients = new QLabel("0");
    m_lblClients->setObjectName("lblClients");
    m_lblFrames = new QLabel("0");
    m_lblFrames->setObjectName("lblFrames");
    m_lblBad = new QLabel("0");
    m_lblBad->setObjectName("lblBad");
    stats->addWidget(new QLabel(QStringLiteral("客户端:")));
    stats->addWidget(m_lblClients);
    stats->addStretch();
    stats->addWidget(new QLabel(QStringLiteral("已发帧:")));
    stats->addWidget(m_lblFrames);
    stats->addStretch();
    stats->addWidget(new QLabel(QStringLiteral("坏帧:")));
    stats->addWidget(m_lblBad);
    root->addLayout(stats);

    m_log = new QListWidget;
    m_log->setObjectName("listLog");
    root->addWidget(m_log, /*stretch*/ 1);

    setCentralWidget(central);
    setWindowTitle(QStringLiteral("AOSimulator — STM32 串口帧模拟器"));
    resize(460, 560);

    // 初始化时就把下拉框当前选中项同步给核心，
    // 否则界面显示「恒值」而核心仍用默认正弦（activated 只在用户操作时触发）
    m_core->setWaveform(static_cast<WaveMode>(m_wave->currentData().toInt()));
    m_core->setFaultMode(static_cast<FaultMode>(m_fault->currentData().toInt()));

    connect(m_btnStart, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(m_btnStop, &QPushButton::clicked, this, &MainWindow::onStop);
    connect(m_wave, &QComboBox::activated, this, [this](int idx) {
        m_core->setWaveform(static_cast<WaveMode>(m_wave->itemData(idx).toInt()));
    });
    connect(m_fault, &QComboBox::activated, this, [this](int idx) {
        m_core->setFaultMode(static_cast<FaultMode>(m_fault->itemData(idx).toInt()));
    });
}

void MainWindow::onStart()
{
    m_core->start(static_cast<quint16>(m_port->value()), m_interval->value());
    if (!m_core->isRunning())
        return;     // 监听失败，错误信息已进状态栏
    m_port->setEnabled(false);
    m_interval->setEnabled(false);
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);
}

void MainWindow::onStop()
{
    m_core->stop();
    m_port->setEnabled(true);
    m_interval->setEnabled(true);
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
}

void MainWindow::onStatus(const QString &msg)
{
    statusBar()->showMessage(msg);
    m_log->addItem(QStringLiteral("[%1] %2")
                       .arg(QTime::currentTime().toString("hh:mm:ss"), msg));
    m_log->scrollToBottom();
}

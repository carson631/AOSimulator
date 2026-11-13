#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "simulator.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class QSpinBox;
class QComboBox;
class QPushButton;
class QLabel;
class QListWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // 供单元测试访问核心与控件（objectName 见构造函数）
    SimulatorCore *core() const { return m_core; }

    // 用界面当前参数直接启动监听（--autostart 走这里，便于脚本化验证）
    void startWithDefaults() { onStart(); }

private slots:
    void onStart();
    void onStop();
    void onStatus(const QString &msg);

private:
    void buildUi();

    Ui::MainWindow *ui;
    SimulatorCore *m_core;

    QSpinBox *m_port = nullptr;
    QSpinBox *m_interval = nullptr;
    QComboBox *m_wave = nullptr;
    QComboBox *m_fault = nullptr;
    QPushButton *m_btnStart = nullptr;
    QPushButton *m_btnStop = nullptr;
    QLabel *m_lblClients = nullptr;
    QLabel *m_lblFrames = nullptr;
    QLabel *m_lblBad = nullptr;
    QListWidget *m_log = nullptr;
};
#endif // MAINWINDOW_H

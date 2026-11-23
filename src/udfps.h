#ifndef UDFPS_H
#define UDFPS_H

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

/*
 * Under-display fingerprint support for the OnePlus 6T (fajita), doing what
 * SystemUI does on OxygenOS/LineageOS.
 *
 * While enrolling/authenticating, the Goodix HAL arms the touch IC's "touch
 * hold" mode and reports finger down/up over the sensor as ACQUIRED_VENDOR
 * with vendor code 0/1. On finger down the HAL waits ~680 ms for the display
 * to become "UI ready" before capturing. That happens when fingerprint HBM is
 * on and the press is notified: the kernel commits a frame with
 * fingerprint_pressed set and sends GF_NET_EVENT_UI_READY to the Goodix
 * driver.
 *
 * With the display off the touch IC still reports presses, so the display is
 * woken through mce first, and the press is only notified once the display is
 * on and the lockscreen has had time to draw its white circle over the
 * sensor. Otherwise the HAL captures in the dark.
 */
class Udfps : public QObject
{
    Q_OBJECT
public:
    explicit Udfps(QObject *parent = nullptr);
    bool enabled() const { return m_enabled; }
    bool pressed() const { return m_enabled && m_pressed; }

public slots:
    void fingerDown();
    void fingerUp();

private slots:
    void displayStatusChanged(const QString &status);
    void notifyPress();

private:
    void setPress(bool on);
    void scheduleNotify();

    bool m_enabled = false;
    bool m_displayOn = true;
    bool m_pressed = false;
    bool m_waking = false;
    QElapsedTimer m_downTime;
    QTimer m_notifyTimer;
};

#endif // UDFPS_H

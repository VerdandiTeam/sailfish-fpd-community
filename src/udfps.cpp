#include "udfps.h"
#include "util/property_store.h"

#include <QDebug>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QFile>

#include <algorithm>

#define FP_HBM   "/sys/class/drm/card0-DSI-1/op_friginer_print_hbm"
#define FP_PRESS "/sys/class/drm/card0-DSI-1/notify_fppress"

#define MCE_SERVICE        "com.nokia.mce"
#define MCE_REQUEST_PATH   "/com/nokia/mce/request"
#define MCE_REQUEST_IF     "com.nokia.mce.request"
#define MCE_SIGNAL_PATH    "/com/nokia/mce/signal"
#define MCE_SIGNAL_IF      "com.nokia.mce.signal"

// After the display comes on, wait this long for the lockscreen to draw the
// circle and the panel to settle before letting the HAL capture. 300 ms was
// needed with a white circle (else NOT_LIVE_FINGER / PREPROCESS_FAILED);
// with the green circle 150, 50 and 0 ms all unlocked on the first press
// (2026-11-23), so capture as soon as mce reports the display on. Raise it
// via /etc/sailfish-fpd-community/udfps-settle-ms if captures start failing.
static const int SETTLE_WANTED_MS = 0;
// ...but at least this long...
static const int SETTLE_MIN_MS = 100;
// ...and never later than this after finger down: the HAL stops waiting for
// UI ready ~680 ms after finger down and captures in the dark.
static const int UI_DEADLINE_MS = 600;

static void writeNode(const char *path, const char *value)
{
    QFile f(QString::fromLatin1(path));
    if (!f.open(QIODevice::WriteOnly) || f.write(value) < 0) {
        qWarning() << "udfps: writing" << value << "to" << path << "failed:" << f.errorString();
    }
}

static bool isFajita()
{
    util::AndroidPropertyStore store;
    for (const char *key : {"ro.product.device", "ro.product.vendor.device", "ro.build.product"}) {
        // get() returns raw getprop output, including the trailing newline
        const QString v = QString::fromStdString(store.get(key)).trimmed();
        if (v == "fajita" || v == "OnePlus6T" || v == "OnePlus6TSingle")
            return true;
    }
    return false;
}

Udfps::Udfps(QObject *parent) : QObject(parent)
{
    m_enabled = isFajita() && QFile::exists(FP_HBM) && QFile::exists(FP_PRESS);
    qInfo() << "udfps: under-display fingerprint support" << (m_enabled ? "enabled" : "disabled");
    if (!m_enabled)
        return;

    m_notifyTimer.setSingleShot(true);
    connect(&m_notifyTimer, &QTimer::timeout, this, &Udfps::notifyPress);

    QDBusConnection bus = QDBusConnection::systemBus();
    bus.connect(MCE_SERVICE, MCE_SIGNAL_PATH, MCE_SIGNAL_IF, "display_status_ind",
                this, SLOT(displayStatusChanged(QString)));

    QDBusMessage call = QDBusMessage::createMethodCall(MCE_SERVICE, MCE_REQUEST_PATH,
                                                       MCE_REQUEST_IF, "get_display_status");
    QDBusReply<QString> reply = bus.call(call, QDBus::Block, 2000);
    if (reply.isValid())
        m_displayOn = reply.value() != "off";
    else
        qWarning() << "udfps: get_display_status failed:" << reply.error().message();
}

void Udfps::setPress(bool on)
{
    if (on) {
        writeNode(FP_HBM, "1");
        writeNode(FP_PRESS, "1");
    } else {
        writeNode(FP_PRESS, "0");
        writeNode(FP_HBM, "0");
    }
}

void Udfps::fingerDown()
{
    if (!m_enabled || m_pressed)
        return;
    m_pressed = true;
    m_downTime.start();

    if (m_displayOn) {
        setPress(true);
        return;
    }

    // Don't notify the press while the panel is off: the first frame after
    // power up would commit it and the HAL would capture before HBM is on
    // and before the circle is drawn.
    if (!m_waking) {
        qInfo() << "udfps: finger down with display off, waking display";
        m_waking = true;
        QDBusMessage call = QDBusMessage::createMethodCall(MCE_SERVICE, MCE_REQUEST_PATH,
                                                           MCE_REQUEST_IF, "req_display_state_on");
        call.setAutoStartService(false);
        QDBusConnection::systemBus().asyncCall(call);
    }
}

void Udfps::fingerUp()
{
    if (!m_enabled || !m_pressed)
        return;
    m_pressed = false;
    m_waking = false; // retry the wake on the next press if it didn't happen
    m_notifyTimer.stop();
    setPress(false);
}

void Udfps::displayStatusChanged(const QString &status)
{
    bool on = status != "off";
    if (on == m_displayOn)
        return;
    m_displayOn = on;

    if (!on) {
        // Display went off: drop any pending press state.
        m_notifyTimer.stop();
        if (m_pressed) {
            m_pressed = false;
            setPress(false);
        }
        return;
    }

    if (m_waking) {
        m_waking = false;
        if (m_pressed)
            scheduleNotify();
    }
}

// Optional override of SETTLE_WANTED_MS for tuning, re-read on every
// screen-off press: echo 150 > /etc/sailfish-fpd-community/udfps-settle-ms
static int settleWantedMs()
{
    QFile f(QStringLiteral("/etc/sailfish-fpd-community/udfps-settle-ms"));
    if (f.open(QIODevice::ReadOnly)) {
        bool ok = false;
        int ms = QString::fromLatin1(f.readAll()).trimmed().toInt(&ok);
        if (ok && ms >= 0 && ms <= UI_DEADLINE_MS)
            return ms;
    }
    return SETTLE_WANTED_MS;
}

void Udfps::scheduleNotify()
{
    int sinceDown = int(m_downTime.elapsed());
    int wanted = settleWantedMs();
    int settle = std::min(wanted, std::max(std::min(SETTLE_MIN_MS, wanted), UI_DEADLINE_MS - sinceDown));
    qInfo() << "udfps: display on" << sinceDown << "ms after finger down, notifying press in" << settle << "ms";
    m_notifyTimer.start(settle);
}

void Udfps::notifyPress()
{
    if (m_pressed && m_displayOn)
        setPress(true);
}

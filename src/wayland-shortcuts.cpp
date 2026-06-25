/*
obs-breadcrumbs
Copyright (C) 2026 Christian Nachtigall <christian@nachtigall.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "wayland-shortcuts.hpp"
#include "breadcrumbs.hpp"

#include <plugin-support.h>
#include <util/base.h> // LOG_INFO / LOG_WARNING

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QList>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVariantMap>

#include <memory>

namespace {

constexpr const char *kPortalService = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char *kShortcutsIface = "org.freedesktop.portal.GlobalShortcuts";
constexpr const char *kRequestIface = "org.freedesktop.portal.Request";
constexpr const char *kRegistryIface = "org.freedesktop.host.portal.Registry";

// App id declared to the portal; becomes the "<app-id>:slotN" prefix the
// compositor uses (e.g. in `hyprctl globalshortcuts`). The portal validates
// this against an installed .desktop file, so we use OBS's own id (OBS itself
// registers no global shortcuts, so these names are unambiguously ours).
constexpr const char *kAppId = "com.obsproject.Studio";

// We use a *private* D-Bus connection rather than the shared sessionBus():
// OBS already associated the shared connection with its own (empty) app id,
// which makes the portal reject our Register and refuse the session. A fresh
// connection lets us register our own app id.
constexpr const char *kConnName = "obs-breadcrumbs-portal";

// One entry of the BindShortcuts `a(sa{sv})` argument: (id, {description, ...}).
struct PortalShortcut {
	QString id;
	QVariantMap props;
};

QDBusArgument &operator<<(QDBusArgument &arg, const PortalShortcut &s)
{
	arg.beginStructure();
	arg << s.id << s.props;
	arg.endStructure();
	return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalShortcut &s)
{
	arg.beginStructure();
	arg >> s.id >> s.props;
	arg.endStructure();
	return arg;
}

} // namespace

Q_DECLARE_METATYPE(PortalShortcut)
Q_DECLARE_METATYPE(QList<PortalShortcut>)

namespace {

// Drives the portal handshake: CreateSession -> (Response) -> BindShortcuts ->
// (Response), then routes Activated signals to the matching breadcrumb slot.
//
// Request.Response signals are matched by interface+member only (no path/sender
// filter): they are unicast to our connection, and QtDBus's path-based hook
// matching does not catch them. Because the handshake is strictly sequential, a
// small phase flag tells the CreateSession response from the BindShortcuts one.
class BreadcrumbsPortal : public QObject {
	Q_OBJECT

	enum class Phase { Create, Bind, Done };

public:
	BreadcrumbsPortal()
	{
		bus_ = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QString::fromUtf8(kConnName));
		if (!bus_.isConnected()) {
			obs_log(LOG_WARNING, "wayland: no session D-Bus; global shortcuts unavailable");
			return;
		}

		bus_.connect(QString(), QString(), kRequestIface, QStringLiteral("Response"), this,
			     SLOT(onResponse(uint, QVariantMap)));
		// Match by interface+member only (no path), same as Response — QtDBus's
		// path-filtered matching does not catch these portal signals.
		bus_.connect(QString(), QString(), kShortcutsIface, QStringLiteral("Activated"), this,
			     SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));

		// Unsandboxed apps must declare an app id before the portal will create
		// a GlobalShortcuts session ("An app id is required" otherwise).
		QDBusMessage reg = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kRegistryIface,
								  QStringLiteral("Register"));
		reg << QString::fromUtf8(kAppId) << QVariantMap();
		QDBusMessage regReply = bus_.call(reg, QDBus::Block, 3000);
		if (regReply.type() == QDBusMessage::ErrorMessage)
			obs_log(LOG_WARNING, "wayland: app-id registration: %s (continuing anyway)",
				qUtf8Printable(regReply.errorMessage()));

		createSession();
	}

	~BreadcrumbsPortal() override { QDBusConnection::disconnectFromBus(QString::fromUtf8(kConnName)); }

private:
	void createSession()
	{
		QVariantMap options;
		options[QStringLiteral("handle_token")] = QStringLiteral("bccreate");
		options[QStringLiteral("session_handle_token")] = QStringLiteral("breadcrumbs");

		QDBusMessage msg = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kShortcutsIface,
								  QStringLiteral("CreateSession"));
		msg << options;

		auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(msg), this);
		connect(watcher, &QDBusPendingCallWatcher::finished, this, &BreadcrumbsPortal::onMethodReturn);
		obs_log(LOG_INFO, "wayland: creating global-shortcuts session");
	}

	void bindShortcuts()
	{
		QList<PortalShortcut> shortcuts;
		std::array<std::string, BREADCRUMBS_SLOTS> categories = breadcrumbs_get_categories();
		for (size_t i = 0; i < BREADCRUMBS_SLOTS; i++) {
			QString desc = QStringLiteral("Breadcrumb slot %1").arg(static_cast<int>(i + 1));
			if (!categories[i].empty())
				desc += QStringLiteral(" (%1)").arg(QString::fromStdString(categories[i]));
			PortalShortcut s;
			s.id = QStringLiteral("breadcrumbs-slot%1").arg(static_cast<int>(i + 1));
			s.props[QStringLiteral("description")] = desc;
			shortcuts.append(s);
		}

		QVariantMap options;
		options[QStringLiteral("handle_token")] = QStringLiteral("bcbind");

		QDBusMessage msg = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kShortcutsIface,
								  QStringLiteral("BindShortcuts"));
		msg << QVariant::fromValue(QDBusObjectPath(sessionPath_)) << QVariant::fromValue(shortcuts) << QString()
		    << options;

		auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(msg), this);
		connect(watcher, &QDBusPendingCallWatcher::finished, this, &BreadcrumbsPortal::onMethodReturn);
		obs_log(LOG_INFO, "wayland: binding %d shortcuts", static_cast<int>(BREADCRUMBS_SLOTS));
	}

private slots:
	// Surfaces D-Bus errors from the CreateSession / BindShortcuts method calls
	// (the useful results arrive later via the Response signal).
	void onMethodReturn(QDBusPendingCallWatcher *watcher)
	{
		QDBusPendingReply<QDBusObjectPath> reply = *watcher;
		watcher->deleteLater();
		if (reply.isError())
			obs_log(LOG_WARNING, "wayland: portal call failed: %s",
				qUtf8Printable(reply.error().message()));
	}

	void onResponse(uint response, const QVariantMap &results)
	{
		if (phase_ == Phase::Create) {
			phase_ = Phase::Bind;
			if (response != 0) {
				obs_log(LOG_WARNING, "wayland: shortcuts session denied (response %u)", response);
				return;
			}
			QVariant handle = results.value(QStringLiteral("session_handle"));
			sessionPath_ = handle.canConvert<QDBusObjectPath>() ? handle.value<QDBusObjectPath>().path()
									    : handle.toString();
			if (sessionPath_.isEmpty()) {
				obs_log(LOG_WARNING, "wayland: session created but no session_handle returned");
				return;
			}
			bindShortcuts();
		} else if (phase_ == Phase::Bind) {
			phase_ = Phase::Done;
			if (response != 0) {
				obs_log(LOG_WARNING, "wayland: BindShortcuts rejected (response %u)", response);
				return;
			}
			obs_log(LOG_INFO, "wayland: global shortcuts registered "
					  "(bind them in your compositor; see `hyprctl globalshortcuts`)");
		}
	}

	void onActivated(const QDBusObjectPath &session, const QString &shortcutId, qulonglong, const QVariantMap &)
	{
		if (session.path() != sessionPath_)
			return;
		const QString prefix = QStringLiteral("breadcrumbs-slot");
		if (!shortcutId.startsWith(prefix))
			return;

		bool ok = false;
		int n = shortcutId.mid(prefix.size()).toInt(&ok); // "breadcrumbs-slot3" -> 3
		if (!ok || n < 1 || n > static_cast<int>(BREADCRUMBS_SLOTS))
			return;

		breadcrumbs_trigger_slot(static_cast<size_t>(n - 1));
	}

private:
	QDBusConnection bus_ = QDBusConnection::sessionBus();
	QString sessionPath_;
	Phase phase_ = Phase::Create;
};

std::unique_ptr<BreadcrumbsPortal> g_portal;

bool running_on_wayland()
{
	if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland"), Qt::CaseInsensitive))
		return true;
	return qgetenv("XDG_SESSION_TYPE") == "wayland";
}

} // namespace

void breadcrumbs_wayland_init()
{
	if (g_portal)
		return;
	if (!running_on_wayland())
		return; // X11/other: OBS's own hotkeys work; nothing to do.

	qDBusRegisterMetaType<PortalShortcut>();
	qDBusRegisterMetaType<QList<PortalShortcut>>();

	// Create on the GUI thread so the private D-Bus connection integrates with
	// the running Qt event loop.
	QMetaObject::invokeMethod(
		qApp,
		[]() {
			if (!g_portal)
				g_portal = std::make_unique<BreadcrumbsPortal>();
		},
		Qt::QueuedConnection);
}

void breadcrumbs_wayland_shutdown()
{
	if (QThread::currentThread() == qApp->thread()) {
		g_portal.reset();
	} else {
		QMetaObject::invokeMethod(qApp, []() { g_portal.reset(); }, Qt::BlockingQueuedConnection);
	}
}

#include "wayland-shortcuts.moc"

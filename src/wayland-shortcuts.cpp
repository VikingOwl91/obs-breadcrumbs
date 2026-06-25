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

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QGuiApplication>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <memory>

namespace {

constexpr const char *kPortalService = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char *kShortcutsIface = "org.freedesktop.portal.GlobalShortcuts";
constexpr const char *kRequestIface = "org.freedesktop.portal.Request";

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

// Drives the portal handshake: CreateSession -> (Response) -> BindShortcuts,
// and routes Activated signals to the matching breadcrumb slot.
class BreadcrumbsPortal : public QObject {
	Q_OBJECT

public:
	BreadcrumbsPortal()
	{
		auto bus = QDBusConnection::sessionBus();
		if (!bus.isConnected()) {
			obs_log(LOG_WARNING, "wayland: no session D-Bus; global shortcuts unavailable");
			return;
		}

		// Predict the portal's per-request/session object paths from our unique
		// bus name so we can subscribe before the calls land (avoids a race).
		QString token = bus.baseService();      // e.g. ":1.42"
		token = token.mid(1).replace('.', '_'); // "1_42"
		sessionPath_ = QStringLiteral("/org/freedesktop/portal/desktop/session/%1/breadcrumbs").arg(token);
		QString createReq = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/bccreate").arg(token);
		QString bindReq = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/bcbind").arg(token);

		bus.connect(kPortalService, createReq, kRequestIface, QStringLiteral("Response"), this,
			    SLOT(onCreateResponse(uint, QVariantMap)));
		bus.connect(kPortalService, bindReq, kRequestIface, QStringLiteral("Response"), this,
			    SLOT(onBindResponse(uint, QVariantMap)));
		bus.connect(kPortalService, kPortalPath, kShortcutsIface, QStringLiteral("Activated"), this,
			    SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));

		QVariantMap options;
		options[QStringLiteral("handle_token")] = QStringLiteral("bccreate");
		options[QStringLiteral("session_handle_token")] = QStringLiteral("breadcrumbs");

		QDBusMessage msg = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kShortcutsIface,
								  QStringLiteral("CreateSession"));
		msg << options;
		bus.asyncCall(msg);
		obs_log(LOG_INFO, "wayland: requesting global-shortcuts session");
	}

private slots:
	void onCreateResponse(uint response, const QVariantMap &)
	{
		if (response != 0) {
			obs_log(LOG_WARNING, "wayland: shortcuts session denied (response %u)", response);
			return;
		}

		QList<PortalShortcut> shortcuts;
		std::array<std::string, BREADCRUMBS_SLOTS> categories = breadcrumbs_get_categories();
		for (size_t i = 0; i < BREADCRUMBS_SLOTS; i++) {
			QString desc = QStringLiteral("Breadcrumb slot %1").arg(static_cast<int>(i + 1));
			if (!categories[i].empty())
				desc += QStringLiteral(" (%1)").arg(QString::fromStdString(categories[i]));
			PortalShortcut s;
			s.id = QStringLiteral("slot%1").arg(static_cast<int>(i + 1));
			s.props[QStringLiteral("description")] = desc;
			shortcuts.append(s);
		}

		QVariantMap options;
		options[QStringLiteral("handle_token")] = QStringLiteral("bcbind");

		QDBusMessage msg = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kShortcutsIface,
								  QStringLiteral("BindShortcuts"));
		msg << QVariant::fromValue(QDBusObjectPath(sessionPath_)) << QVariant::fromValue(shortcuts) << QString()
		    << options;
		QDBusConnection::sessionBus().asyncCall(msg);
		obs_log(LOG_INFO, "wayland: binding %d global shortcuts", static_cast<int>(BREADCRUMBS_SLOTS));
	}

	void onBindResponse(uint response, const QVariantMap &)
	{
		if (response != 0) {
			obs_log(LOG_WARNING, "wayland: BindShortcuts failed (response %u)", response);
			return;
		}
		obs_log(LOG_INFO, "wayland: global shortcuts registered "
				  "(bind them in your compositor; see `hyprctl globalshortcuts`)");
	}

	void onActivated(const QDBusObjectPath &session, const QString &shortcutId, qulonglong, const QVariantMap &)
	{
		if (session.path() != sessionPath_)
			return;
		if (!shortcutId.startsWith(QStringLiteral("slot")))
			return;

		bool ok = false;
		int n = shortcutId.mid(4).toInt(&ok); // "slot3" -> 3
		if (!ok || n < 1 || n > static_cast<int>(BREADCRUMBS_SLOTS))
			return;

		breadcrumbs_trigger_slot(static_cast<size_t>(n - 1));
	}

private:
	QString sessionPath_;
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

	g_portal = std::make_unique<BreadcrumbsPortal>();
}

void breadcrumbs_wayland_shutdown()
{
	g_portal.reset();
}

#include "wayland-shortcuts.moc"

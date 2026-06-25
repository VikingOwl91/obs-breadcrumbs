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

#include "breadcrumbs-config.hpp"
#include "breadcrumbs.hpp"

#include <obs-frontend-api.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QString>
#include <QVBoxLayout>

#include <array>
#include <string>

void breadcrumbs_open_config_dialog()
{
	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());

	QDialog dialog(parent);
	dialog.setWindowTitle(QStringLiteral("Breadcrumbs — Category Names"));
	dialog.setMinimumWidth(360);

	auto *layout = new QVBoxLayout(&dialog);

	const QString introText =
		QStringLiteral("Name each marker category below.\n"
			       "Bind a key to each one in Settings → Hotkeys (search \"Breadcrumb\").\n"
			       "Empty names fall back to \"Marker N\".");
	auto *intro = new QLabel(introText, &dialog);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	auto *form = new QFormLayout();
	layout->addLayout(form);

	std::array<std::string, BREADCRUMBS_SLOTS> categories = breadcrumbs_get_categories();
	QLineEdit *edits[BREADCRUMBS_SLOTS];
	for (size_t i = 0; i < BREADCRUMBS_SLOTS; i++) {
		edits[i] = new QLineEdit(QString::fromStdString(categories[i]), &dialog);
		edits[i]->setMaxLength(64);
		form->addRow(QStringLiteral("Slot %1").arg(static_cast<int>(i + 1)), edits[i]);
	}

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	layout->addWidget(buttons);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	if (dialog.exec() != QDialog::Accepted)
		return;

	for (size_t i = 0; i < BREADCRUMBS_SLOTS; i++)
		categories[i] = edits[i]->text().trimmed().toStdString();
	breadcrumbs_set_categories(categories);
}

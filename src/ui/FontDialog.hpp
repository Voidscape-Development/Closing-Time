/*
Closing Time
Copyright (C) 2026 Voidscape Development <Eiondailey@live.com>

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

#pragma once

#include <QDialog>

#include "model/CreditsModel.hpp"

class QCheckBox;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace closingtime {

/*
 * What this roll does about its fonts, in one window.
 *
 * One row per family the roll is actually set in, saying where that family stands on this machine
 * and what will happen to it on one that does not have it: carried with the roll, standing in for
 * something else, or neither -- which is the one case that still renders as a surprise, and is
 * therefore the one the row calls out.
 *
 * The window edits the designer's copy of the document directly and reports through
 * documentChanged(), the same way the style library manager does: undo, the section list and the
 * preview belong to the designer and nothing here should be reaching into them.
 */
class FontDialog : public QDialog {
	Q_OBJECT

public:
	explicit FontDialog(Document *document, QWidget *parent = nullptr);

signals:
	/* Raised either side of each change, so the designer can open an undo step around it. */
	void documentAboutToChange();
	void documentChanged();

private:
	void refreshRows();
	void updateSummary();
	void setBundling(bool enabled);
	void substituteChanged(const QString &family, const QString &substitute);
	void refreshBundleNow();

	Document *document = nullptr;

	QTreeWidget *table = nullptr;
	QCheckBox *bundleCheck = nullptr;
	QLabel *summaryLabel = nullptr;
	QLabel *noteLabel = nullptr;

	/* Set while the rows are being rebuilt, so a combo box being filled is not read as a choice. */
	bool populating = false;
};

} // namespace closingtime

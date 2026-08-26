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

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace closingtime {

/*
 * The style library manager: two lists side by side, with the traffic between them in the middle.
 *
 * On the left, the presets belonging to the roll being designed. On the right, the machine-wide
 * library. Publishing sends a style rightward and links the document's copy to it; linking brings
 * one leftward as a preset bound to the library. That shape is the point of the window -- the two
 * places a style can live are visible at once, and which of them a given style is in is a fact on
 * screen rather than something to remember.
 *
 * Opened from the Tools menu with no document, it is the right-hand list alone: the library is a
 * thing a machine has whether or not a designer is open, and renaming or deleting from it should
 * not require finding a source to open first.
 *
 * The library holds two collections -- text styles and the panels drawn behind them -- and a picker
 * at the top says which the lists are showing. One pair of lists rather than two, because every
 * operation between them means exactly the same thing for either kind: publish, link, copy, rename,
 * delete. Two pairs would be the same five buttons twice and a window half as tall for each.
 */
class StyleLibraryDialog : public QDialog {
	Q_OBJECT

public:
	/*
	 * `document` may be null, which opens the library on its own. When it is not, the dialog
	 * edits the caller's copy directly and reports through documentChanged() -- the designer owns
	 * undo, the section list and the preview, none of which this window should be reaching into.
	 */
	explicit StyleLibraryDialog(Document *document, QWidget *parent = nullptr);

signals:
	/*
	 * Raised before each change to the document, so the designer can open an undo step around
	 * it, and again after: `before` separates the two.
	 */
	void documentAboutToChange();
	void documentChanged();

private:
	/* Which of the library's two collections the lists are showing. */
	bool showingBackgrounds() const;

	void refreshLists();
	void publishSelected();
	void linkSelected();
	void copySelected();
	void renameSelected();
	void deleteSelected();
	void importLibrary();
	void exportLibrary();
	void updateButtons();

	/* The name selected in each list, or empty. */
	QString selectedDocumentPreset() const;
	QString selectedLibraryPreset() const;

	Document *document = nullptr;

	QComboBox *kindBox = nullptr;
	QListWidget *documentList = nullptr;
	QListWidget *libraryList = nullptr;
	QPushButton *publishButton = nullptr;
	QPushButton *linkButton = nullptr;
	QPushButton *copyButton = nullptr;
	QPushButton *renameButton = nullptr;
	QPushButton *deleteButton = nullptr;
	QLabel *pathLabel = nullptr;
};

} // namespace closingtime

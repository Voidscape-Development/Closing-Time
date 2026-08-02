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

#include <obs.hpp>

#include <QDialog>

#include "model/CreditsModel.hpp"
#include "render/StripRenderer.hpp"

class QDialogButtonBox;
class QLabel;
class QListWidget;
class QScrollArea;
class QTimer;

namespace closingtime {

class PreviewWidget;
class SectionEditor;

/*
 * Opens the designer for `source`, raising the existing window if one is already open for
 * it. Must be called from the UI thread.
 */
void openDesignerFor(obs_source_t *source);

/* Closes any designer window bound to `source`. Safe to call for sources with none. */
void closeDesignerFor(obs_source_t *source);

/*
 * The credit roll designer: a section list on the left, the editor for the selected
 * section in the middle, and a live preview of the whole strip on the right. It works on
 * its own copy of the document and only writes back to the source on Apply or OK.
 */
class DesignerDialog : public QDialog {
	Q_OBJECT

public:
	DesignerDialog(obs_source_t *source, QWidget *parent = nullptr);
	~DesignerDialog() override;

	/* True once the bound source has been destroyed and the window should go away. */
	bool sourceIsGone() const;

private:
	void loadFromSource();
	void writeToSource();

	void refreshSectionList(int selectRow);
	void onSelectionChanged();
	void onSectionEdited();

	void duplicateSection();
	void removeSection();
	void moveSection(int delta);

	void importJson();
	void exportJson();

	/* Re-renders the preview strip and updates the duration readout. */
	void refreshPreview();
	void schedulePreviewRefresh();

	/* Commits the editor's current state into `document` at `currentIndex`. */
	void commitCurrentSection();

	OBSWeakSource weakSource;

	Document document;
	int currentIndex = -1;

	/* Private to this dialog: the source keeps its own cache on the same thread. */
	LogoCache logos;

	QListWidget *sectionList = nullptr;
	SectionEditor *editor = nullptr;
	QScrollArea *editorScroll = nullptr;
	PreviewWidget *preview = nullptr;
	QLabel *durationLabel = nullptr;
	QDialogButtonBox *buttons = nullptr;
	/*
	 * Renders are debounced so that typing into a text field does not re-rasterise the
	 * entire strip on every keystroke.
	 */
	QTimer *refreshTimer = nullptr;
};

} // namespace closingtime

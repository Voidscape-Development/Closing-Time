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

#include <QPushButton>
#include <QWidget>

#include "model/CreditsModel.hpp"

class QSpinBox;
class QTableWidget;

namespace closingtime {

/*
 * A button that shows the colour it holds and opens a colour dialog when pressed. The style
 * editor picks four separate colours -- fill, outline, shadow and each gradient stop -- so
 * the swatch, the dialog and the "did it actually change" test live here once.
 */
class ColourButton : public QPushButton {
	Q_OBJECT

public:
	explicit ColourButton(QWidget *parent = nullptr);

	QColor colour() const { return current; }
	void setColour(const QColor &colour);

	/* Shown in the colour dialog's title bar. */
	void setDialogTitle(const QString &title) { dialogTitle = title; }

signals:
	void colourChanged();

private:
	void pick();
	void refresh();

	QColor current = QColor(255, 255, 255);
	QString dialogTitle;
};

/* Paints the gradient a style would be filled with, as the style would actually map it. */
class GradientPreview : public QWidget {
	Q_OBJECT

public:
	explicit GradientPreview(QWidget *parent = nullptr);

	void setSpec(TextFill fill, const GradientSpec &spec);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	TextFill fill = TextFill::LinearGradient;
	GradientSpec spec;
};

/*
 * Angle and colour stops for one gradient, with a preview of the result. Stops are edited in
 * place in a small table; the list is kept sorted by the renderer rather than by the table,
 * so dragging a stop past its neighbour does not renumber the rows under the user's cursor.
 */
class GradientEditor : public QWidget {
	Q_OBJECT

public:
	explicit GradientEditor(QWidget *parent = nullptr);

	void setGradient(const GradientSpec &spec);
	GradientSpec gradient() const { return current; }

	/* Radial gradients have no axis, so the angle row hides for them. */
	void setFill(TextFill fill);

signals:
	void changed();

protected:
	/*
	 * Selects the row a cell widget belongs to when that widget takes focus. Clicking a
	 * spin box or a swatch inside a cell never reaches the table itself, so without this
	 * there is no way to select the row the Remove button acts on.
	 */
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void rebuildTable();
	void readTable();
	void sortStops();
	void addStop();
	void removeSelectedStop();
	void notifyEdited();

	QSpinBox *angle = nullptr;
	QWidget *angleRow = nullptr;
	QTableWidget *table = nullptr;
	QPushButton *removeButton = nullptr;
	GradientPreview *preview = nullptr;

	GradientSpec current;
	TextFill fill = TextFill::LinearGradient;
	bool loading = false;
};

} // namespace closingtime

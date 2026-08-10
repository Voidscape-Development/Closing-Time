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

#include <QColor>
#include <QWidget>

#include "render/StripRenderer.hpp"

namespace closingtime {

/*
 * Draws the rendered strip at a scale that fits the canvas across the widget, with the canvas
 * itself framed at the top of the pane and the roll below it dimmed as still to come. This is
 * the same Strip the source uploads to the GPU, so what the designer shows is what the roll
 * will look like.
 */
class PreviewWidget : public QWidget {
	Q_OBJECT

public:
	explicit PreviewWidget(QWidget *parent = nullptr);

	void setStrip(const Strip &strip, int canvasWidth, int canvasHeight, const QColor &background);

	/* Scrolls so that `stripY` (in strip pixels) sits at the top of the visible area. */
	void scrollToStripY(int stripY);

	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	/* Width the canvas is drawn at: the widget's, less the surround kept either side of it. */
	int canvasScreenWidth() const;
	qreal scaleFactor() const;
	int maxScroll() const;

	Strip strip;
	int canvasWidth = 1920;
	int canvasHeight = 1080;
	QColor background = QColor(0, 0, 0, 0);
	int scroll = 0;
};

} // namespace closingtime

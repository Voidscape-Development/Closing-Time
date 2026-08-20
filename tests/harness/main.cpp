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

#include <QGuiApplication>

#include <cstdio>

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"
#include "harness/Probe.hpp"

using namespace closingtime::test;

int main(int argc, char **argv)
{
	/*
	 * Offscreen before the application is constructed, so the harness runs on a build machine
	 * with no display. Set here rather than left to the caller because forgetting it fails as
	 * an abort during static init, which reads as the tests being broken.
	 */
	qputenv("QT_QPA_PLATFORM", "offscreen");

	/*
	 * QGuiApplication rather than QCoreApplication: everything here goes through QPainter and
	 * real font metrics, which need the platform's font database.
	 */
	QGuiApplication app(argc, argv);

	Options options;
	QString error;
	if (!parseOptions(argc, argv, &options, &error)) {
		if (!error.isEmpty()) {
			std::fprintf(stderr, "%s\n", error.toUtf8().constData());
			return 2;
		}
		return 0;
	}

	if (options.listOnly) {
		const int code = run(options);
		std::printf("\nscenes:\n");
		for (const Scene &scene : scenes())
			std::printf("  %-24s %s\n", scene.name.toUtf8().constData(),
				    scene.description.toUtf8().constData());
		return code;
	}

	const int code = run(options);

	/*
	 * Scenes are rendered after the suites, so a run that asked for artifacts gets pictures
	 * even when something failed -- which is exactly the run where they are wanted.
	 */
	if (!artifactDir().isEmpty()) {
		for (const Scene &scene : scenes())
			saveArtifact(scene.name, scene.document);
	}

	return code;
}

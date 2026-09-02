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

#include "harness/Harness.hpp"

#include <QDir>
#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace closingtime::test {

namespace {

struct Suite {
	QString name;
	QString description;
	SuiteBody body;
};

/*
 * Function-local statics rather than file-scope objects, because suites register from other
 * translation units before main runs and the initialization order between them is not defined.
 */
QVector<Suite> &suites()
{
	static QVector<Suite> registered;
	return registered;
}

QStringList &contextStack()
{
	static QStringList stack;
	return stack;
}

struct RunState {
	QString suite;
	QString artifactDir;
	bool verbose = false;
	int checks = 0;
	int failures = 0;
	/* Failures raised by the suite currently running, so its own line can report them. */
	int suiteFailures = 0;
	/*
	 * A sweep that is wrong is usually wrong thousands of times over, and a wall of identical
	 * failures buries everything after it. Only the first few of each run are printed in full.
	 */
	int printed = 0;
};

RunState &state()
{
	static RunState current;
	return current;
}

constexpr int kMaxPrintedFailures = 40;

QString describeLocation()
{
	QStringList parts;
	if (!state().suite.isEmpty())
		parts.append(state().suite);
	parts.append(contextStack());
	return parts.join(QStringLiteral(" / "));
}

void record(bool ok, const QString &what)
{
	RunState &run = state();
	++run.checks;

	if (ok)
		return;

	++run.failures;
	++run.suiteFailures;

	if (run.printed == kMaxPrintedFailures) {
		++run.printed;
		std::fprintf(stderr, "  ... further failures suppressed; fix these first\n");
		return;
	}
	if (run.printed > kMaxPrintedFailures)
		return;

	++run.printed;

	const QString where = describeLocation();
	if (where.isEmpty())
		std::fprintf(stderr, "  FAIL  %s\n", what.toUtf8().constData());
	else
		std::fprintf(stderr, "  FAIL  [%s] %s\n", where.toUtf8().constData(), what.toUtf8().constData());
}

} // namespace

Context::Context(const QString &label)
{
	contextStack().append(label);
}

Context::~Context()
{
	if (!contextStack().isEmpty())
		contextStack().removeLast();
}

void registerSuite(const char *name, const char *description, SuiteBody body)
{
	suites().append(Suite{QString::fromUtf8(name), QString::fromUtf8(description), body});
}

void check(bool ok, const QString &what)
{
	record(ok, what);
}

void checkEq(long long actual, long long expected, const QString &what)
{
	record(actual == expected, QStringLiteral("%1 (got %2, wanted %3)").arg(what).arg(actual).arg(expected));
}

void checkEq(const QString &actual, const QString &expected, const QString &what)
{
	record(actual == expected, QStringLiteral("%1 (got \"%2\", wanted \"%3\")").arg(what, actual, expected));
}

void checkNear(double actual, double expected, double tolerance, const QString &what)
{
	record(std::abs(actual - expected) <= tolerance, QStringLiteral("%1 (got %2, wanted %3 +/- %4)")
								 .arg(what)
								 .arg(actual, 0, 'f', 2)
								 .arg(expected, 0, 'f', 2)
								 .arg(tolerance, 0, 'f', 2));
}

void fail(const QString &what)
{
	record(false, what);
}

QString artifactDir()
{
	return state().artifactDir;
}

int checkCount()
{
	return state().checks;
}

int failureCount()
{
	return state().failures;
}

bool parseOptions(int argc, char **argv, Options *options, QString *error)
{
	const auto needsValue = [&](int i, const char *flag) {
		if (i + 1 >= argc) {
			*error = QStringLiteral("%1 needs a value").arg(QString::fromUtf8(flag));
			return false;
		}
		return true;
	};

	for (int i = 1; i < argc; ++i) {
		const QString arg = QString::fromUtf8(argv[i]);

		if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
			std::printf("closing-time-tests -- offscreen checks for the model and the renderer\n\n"
				    "  --filter <text>     run only the suites whose name contains <text>\n"
				    "  --artifacts <dir>   write a PNG of every scene, and of anything a\n"
				    "                      suite saves, into <dir>\n"
				    "  --list              list the suites and the scenes, run nothing\n"
				    "  --verbose           print every suite, not only the failing ones\n");
			return false;
		}
		if (arg == QLatin1String("--list")) {
			options->listOnly = true;
		} else if (arg == QLatin1String("--verbose") || arg == QLatin1String("-v")) {
			options->verbose = true;
		} else if (arg == QLatin1String("--filter")) {
			if (!needsValue(i, "--filter"))
				return false;
			options->filter = QString::fromUtf8(argv[++i]);
		} else if (arg == QLatin1String("--artifacts")) {
			if (!needsValue(i, "--artifacts"))
				return false;
			options->artifactDir = QString::fromUtf8(argv[++i]);
		} else {
			*error = QStringLiteral("unknown argument \"%1\" (try --help)").arg(arg);
			return false;
		}
	}

	return true;
}

int run(const Options &options)
{
	RunState &current = state();
	current.artifactDir = options.artifactDir;
	current.verbose = options.verbose;

	if (!options.artifactDir.isEmpty() && !QDir().mkpath(options.artifactDir)) {
		std::fprintf(stderr, "could not create the artifact directory \"%s\"\n",
			     options.artifactDir.toUtf8().constData());
		return 2;
	}

	/* Registration order follows link order, which is not something to write assertions
	 * against; sorting makes the report stable from one build to the next. */
	QVector<Suite> ordered = suites();
	std::sort(ordered.begin(), ordered.end(), [](const Suite &a, const Suite &b) { return a.name < b.name; });

	if (options.listOnly) {
		std::printf("suites:\n");
		for (const Suite &suite : ordered)
			std::printf("  %-24s %s\n", suite.name.toUtf8().constData(),
				    suite.description.toUtf8().constData());
		return 0;
	}

	QElapsedTimer timer;
	timer.start();

	int ran = 0;
	int skipped = 0;

	for (const Suite &suite : ordered) {
		if (!options.filter.isEmpty() && !suite.name.contains(options.filter, Qt::CaseInsensitive)) {
			++skipped;
			continue;
		}

		current.suite = suite.name;
		current.suiteFailures = 0;
		const int before = current.checks;

		suite.body();

		const int checksHere = current.checks - before;
		if (current.suiteFailures > 0 || options.verbose) {
			std::fprintf(
				stderr, "%-6s %-24s %d checks%s\n", current.suiteFailures > 0 ? "FAILED" : "ok",
				suite.name.toUtf8().constData(), checksHere,
				current.suiteFailures > 0
					? QStringLiteral(", %1 failed").arg(current.suiteFailures).toUtf8().constData()
					: "");
		}

		current.suite.clear();
		++ran;
	}

	std::fprintf(stderr, "\n%d suites, %d checks, %d failures in %lld ms", ran, current.checks, current.failures,
		     static_cast<long long>(timer.elapsed()));
	if (skipped > 0)
		std::fprintf(stderr, " (%d suites skipped by --filter)", skipped);
	std::fprintf(stderr, "\n");

	if (!options.artifactDir.isEmpty())
		std::fprintf(stderr, "artifacts written to %s\n", options.artifactDir.toUtf8().constData());

	return current.failures == 0 ? 0 : 1;
}

} // namespace closingtime::test

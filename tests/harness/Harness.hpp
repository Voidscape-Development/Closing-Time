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

#include <QString>
#include <QStringList>
#include <QVector>

/*
 * The check framework.
 *
 * Every assertion records and returns rather than aborting, so one run reports everything that is
 * wrong instead of the first thing. That matters more here than in most test suites: a layout
 * change usually breaks several measurements at once, and the *set* of them is what says which
 * part of the layout moved.
 */

namespace closingtime::test {

/* --- suites ------------------------------------------------------------------------------ */

using SuiteBody = void (*)();

void registerSuite(const char *name, const char *description, SuiteBody body);

struct SuiteRegistrar {
	SuiteRegistrar(const char *name, const char *description, SuiteBody body)
	{
		registerSuite(name, description, body);
	}
};

/*
 * Declares a suite and registers it before main runs.
 *
 *   CT_SUITE(logo_rows, "How a logo is divided from the text beside it")
 *   {
 *           ...
 *   }
 *
 * The name is what `--filter` matches on, so keep it short and greppable.
 */
#define CT_SUITE(name, description)                                                             \
	static void ct_suite_##name();                                                          \
	static const ::closingtime::test::SuiteRegistrar ct_registrar_##name(#name, description, \
									    &ct_suite_##name);  \
	static void ct_suite_##name()

/* --- context ----------------------------------------------------------------------------- */

/*
 * A breadcrumb pushed for as long as it is in scope, prefixed onto every failure raised under it.
 * Sweeps use it so one failing combination out of thousands says which combination it was, without
 * every check in the body having to carry the label itself.
 */
class Context {
public:
	explicit Context(const QString &label);
	~Context();

	Context(const Context &) = delete;
	Context &operator=(const Context &) = delete;
};

/* --- assertions -------------------------------------------------------------------------- */

void check(bool ok, const QString &what);
void checkEq(long long actual, long long expected, const QString &what);
void checkEq(const QString &actual, const QString &expected, const QString &what);
/* For anything measured in fractional pixels, where an exact compare would be a coin toss. */
void checkNear(double actual, double expected, double tolerance, const QString &what);

/*
 * Records a failure directly. For the cases where the condition is not one expression -- a sweep
 * counting escapes, a loop that found the wrong thing -- and the message is doing the work.
 */
void fail(const QString &what);

/* --- running ----------------------------------------------------------------------------- */

struct Options {
	/* Substring matched against suite names; empty runs them all. */
	QString filter;
	/* Where `--artifacts` writes its PNGs; empty writes none. */
	QString artifactDir;
	bool listOnly = false;
	bool verbose = false;
};

/* Parses argv. Returns false when the arguments were bad or `--help` was asked for. */
bool parseOptions(int argc, char **argv, Options *options, QString *error);

/* Runs the registered suites. Returns the process exit code: 0 when everything passed. */
int run(const Options &options);

/*
 * Where the current run is writing PNGs, or empty when it is not writing any. Suites use this to
 * decide whether a render is worth saving -- see Probe::saveArtifact.
 */
QString artifactDir();

/* Number of checks and failures so far, for anything that wants to report its own totals. */
int checkCount();
int failureCount();

} // namespace closingtime::test

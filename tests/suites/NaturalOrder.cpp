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

#include "util/NaturalOrder.hpp"

using namespace closingtime;
using namespace closingtime::test;

CT_SUITE(natural_order, "Scene and source names sorted the way a person would")
{
	QStringList names = {QStringLiteral("Scene 10"),        QStringLiteral("scene 2"),
			     QStringLiteral("Scene 1"),         QStringLiteral("Background"),
			     QStringLiteral("Scene 2b"),        QStringLiteral("Scene 02"),
			     QStringLiteral("Camera 3 - Wide"), QStringLiteral("Camera 12"),
			     QStringLiteral("camera 3")};
	naturalSort(names);

	const QStringList expected = {QStringLiteral("Background"),      QStringLiteral("camera 3"),
				      QStringLiteral("Camera 3 - Wide"), QStringLiteral("Camera 12"),
				      QStringLiteral("Scene 1"),         QStringLiteral("Scene 02"),
				      QStringLiteral("scene 2"),         QStringLiteral("Scene 2b"),
				      QStringLiteral("Scene 10")};
	checkEq(names.join(QStringLiteral(" | ")), expected.join(QStringLiteral(" | ")),
		"numbers by value, letters without regard to case, a prefix first");

	check(naturalLess(QStringLiteral("Scene 9"), QStringLiteral("Scene 10")), "9 comes before 10");
	check(!naturalLess(QStringLiteral("Scene 10"), QStringLiteral("Scene 9")), "and 10 does not come before 9");
	check(naturalLess(QStringLiteral("a"), QStringLiteral("B")), "case does not decide the order of letters");
	check(naturalLess(QStringLiteral("Item 99999999999999999999"), QStringLiteral("Item 100000000000000000000")),
	      "numbers too long for any integer type still compare by value");
	check(!naturalLess(QStringLiteral("Same"), QStringLiteral("Same")), "a name is not before itself");
	check(naturalLess(QStringLiteral("Scene"), QStringLiteral("scene")) !=
		      naturalLess(QStringLiteral("scene"), QStringLiteral("Scene")),
	      "names differing only in case still have a fixed order");
}

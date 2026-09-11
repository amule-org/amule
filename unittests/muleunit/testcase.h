//
// MuleUnit: A minimalistic C++ Unit testing framework based on EasyUnit.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2004-2011 Barthelemy Dagenais ( barthelemy@prologique.com )
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#ifndef TESTCASE_H
#define TESTCASE_H

#include <wx/string.h>
#include <list>

namespace muleunit
{

class Test;

typedef std::list<Test *> TestList;

/**
 * A TestCase is a collection of unit tests (instances of Test), always named by the first parameter
 * of a Test declaration.
 */
class TestCase
{
public:
	/**
	 * Main TestCase constructor. @param name TestCase name
	 */
	TestCase(const wxString &name);

	virtual ~TestCase();

	/**
	 * Add a Test to the Test list. Used by TestRegistry. @param test Test instance to add.
	 */
	void addTest(Test *test);

	/**
	 * Get the Test list. @return Test list
	 */
	const TestList &getTests() const;

	/**
	 * Execute all Tests in this TestCase's list, returning false if there were failures.
	 */
	bool run();

	/**
	 * Get the Test list size, i.e. the number of Tests in this TestCase.
	 */
	int getTestsCount() const;

	/**
	 * @return The total number of failures reported by all Tests, 0 if no test ran or none
	 * failed.
	 */
	int getFailuresCount() const;

	/**
	 * @return The total number of successes reported by all Tests, 0 if no test ran or none
	 * succeeded.
	 */
	int getSuccessesCount() const;

	/**
	 * Get the TestCase name: the first parameter of the Test declaration. For a test declared
	 * as TEST(TESTCASE1, TEST1) the TestCase name is "TESTCASE1".
	 *
	 * @return The name of the TestCase
	 */
	const wxString &getName() const;

protected:
	int m_failuresCount;
	int m_successesCount;
	TestList m_tests;
	wxString m_name;

private:
	void runTests(Test *test);
};

} // namespace muleunit
#endif // TESTCASE_H

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

#ifndef TEST_H
#define TEST_H

#include <exception>
#include <memory>

#include <wx/string.h>
#include <list>
#include <string>
#include <wx/wxcrt.h>

/**
 * MuleUnit namespace, holding all muleunit classes.
 */
namespace muleunit
{

class TestCase;
class BTList;

/** Returns the size of a static array. */
template <typename T, size_t N> inline size_t ArraySize(T (&)[N])
{
	return N;
}

/** Print wide-char strings. */
inline void Print(const wxString &str)
{
	wxPuts(str.c_str());
}

/** This exception is raised if an ASSERT fails. */
struct CTestFailureException : public std::exception
{
	/** Constructor, takes a snapshot of the current context, and adds the given information. */
	CTestFailureException(const wxString &msg, const wxString &file, long lineNumber);

	/** Prints the context backtrace for the location where the exception was thrown. */
	void PrintBT() const;

	virtual const char *what() const noexcept;

private:
	//! Pointer to struct containing a snapshot of the contexts
	//! taken at the time the exception was created.
	std::shared_ptr<struct BTList> m_bt;

	//! The message passed in the constructor.
	std::string m_message;
};

/** This exception is raised if an wxASSERT fails. */
struct CAssertFailureException : public CTestFailureException
{
public:
	CAssertFailureException(const wxString &msg, const wxString &file, long lineNumber)
	: CTestFailureException(msg, file, lineNumber)
	{
	}
};

/**
 * Produces informative backtraces.
 *
 * Specify a "context" for a given scope with the CONTEXT macro, which adds a description to the
 * current list of contexts; at destruction, when the scope is exited, the context is removed from
 * the queue.
 *
 * The resulting "backtrace" is printed by CTestFailureException::PrintBT().
 */
class CContext
{
public:
	/** Adds a context with the specified information and description. */
	CContext(const wxString &file, int line, const wxString &desc);

	/** Removes the context added by the constructor. */
	~CContext();
};

//! Used to join the CContext instance name with the line-number.
//! This is done to prevent shadowing.
#define DO_CONTEXT(x, y, z) x y##z

//! Specifies the context of the current scope.
#define CONTEXT(x) CContext wxCONCAT(context, __LINE__)(__FILE__, __LINE__, x)

/// Disables assertions while it is in scope.
class CAssertOff
{
public:
	CAssertOff();
	~CAssertOff();
};

/// Converts basic types to strings.
template <typename TYPE> wxString StringFrom(const TYPE &value)
{
	return wxString() << value;
}

inline wxString StringFrom(unsigned long long value)
{
	return wxString::Format("%" wxLongLongFmtSpec "u", value);
}

inline wxString StringFrom(signed long long value)
{
	return wxString::Format("%" wxLongLongFmtSpec "i", value);
}

/**
 * Test class containing all macros to do unit testing.
 *
 * A test object represents a test that will be executed. Once it has run, it reports all failures
 * in the testPartResult linked list. A failure occurs when a test's condition is false.
 */
class Test
{
public:
	/**
	 * Main Test constructor. Creates a test that registers itself with TestRegistry and with
	 * its test case.
	 * @param testCaseName Name of the test case this test belongs to
	 * @param testName Name of this test
	 */
	Test(const wxString &testCaseName, const wxString &testName);

	/**
	 * Main Test destructor. Deletes the testPartResult linked list, which is why the user
	 * should only report a test result through the macros muleunit provides.
	 */
	virtual ~Test();

	/// Fixtures that will be called after run().
	virtual void tearDown();

	/// Fixtures that will be called before run().
	virtual void setUp();

	/**
	 * Test code goes in this method. run() is called by the Test's TestCase, so subclasses of
	 * Test should override it.
	 */
	virtual void run();

	/**
	 * Get the name of the TestCase this test belongs to: the first parameter of the test
	 * declaration. For a test declared as TEST(TESTCASE1, TEST1) this returns "TESTCASE1".
	 *
	 * @return The TestCase name of this test
	 */
	const wxString &getTestCaseName() const;

	/**
	 * Get the name of this test: the second parameter of the test declaration. For a test
	 * declared as TEST(TESTCASE1, TEST1) this returns "TEST1".
	 *
	 * @return The name of this test.
	 */
	const wxString &getTestName() const;

	template <typename A, typename B>
	static void DoAssertEquals(const wxString &file, unsigned line, const A &a, const B &b)
	{
		if (!(a == b)) {
			wxString message = "Expected '" + StringFrom(a) + "' but got '" + StringFrom(b) + "'";

			throw CTestFailureException(message, file.c_str(), line);
		}
	}

protected:
	wxString m_testCaseName;
	wxString m_testName;
	TestCase *m_testCase;
};

#define THROW_TEST_FAILURE(message) throw CTestFailureException(message, __FILE__, __LINE__)

/**
 * Asserts that a condition is true; a failure is generated if it is not.
 * @param condition Condition to fulfill for the assertion to pass
 * @param message Message that will be displayed if this assertion fails
 */
#define ASSERT_TRUE_M(condition, message) \
	{ \
		if (!(condition)) { \
			THROW_TEST_FAILURE(message); \
		} \
	}

/// Same as ASSERT_TRUE, but without an explicit message.
#define ASSERT_TRUE(condition) ASSERT_TRUE_M(condition, wxString("Not true: ") + #condition);

/// Same as ASSERT_TRUE, but without an explicit message and the condition must be false.
#define ASSERT_FALSE(condition) ASSERT_TRUE_M(!(condition), wxString("Not false: ") + #condition);

/**
 * Asserts that the two parameters are equal; operator == must be defined. A failure is generated if
 * they are not.
 * @param expected Expected value
 * @param actual Actual value to be compared
 * @param message Message that will be displayed if this assertion fails
 */
#define ASSERT_EQUALS_M(expected, actual, message) \
	{ \
		if (!(expected == actual)) { \
			THROW_TEST_FAILURE(message); \
		} \
	}

/// Same as ASSERT_EQUALS_M, but without an explicit message.
#define ASSERT_EQUALS(expected, actual) Test::DoAssertEquals(__FILE__, __LINE__, expected, actual)

/**
 * Makes a test fail with the given message. @param text Failure message
 */
#define FAIL_M(text) THROW_TEST_FAILURE(text)

/// Same as FAIL_M, but without an explicit message.
#define FAIL() FAIL_M("Test failed.")

/// Requires that an exception of a certain type is raised.
#define ASSERT_RAISES_M(type, call, message) \
	try { \
		{ \
			call; \
		} \
		THROW_TEST_FAILURE(message); \
	} catch (const type &) { \
	} catch (const std::exception &e) { \
		THROW_TEST_FAILURE(wxString::FromAscii(e.what())); \
	}

/// Same as ASSERT_RAISES, but without an explicit message.
#define ASSERT_RAISES(type, call) ASSERT_RAISES_M(type, (call), "Exception of type " #type " not raised.")

/**
 * Define a test in a TestCase using test fixtures. Put the test code between brackets after this
 * macro.
 *
 * Only use this if test fixtures were declared earlier, in this order: DECLARE, SETUP, TEARDOWN.
 * @param testCaseName TestCase the test belongs to. The same name as DECLARE, SETUP and TEARDOWN.
 * @param testName Unique test name.
 * @param testDisplayName Displayed when running the test.
 */
#define TEST_M(testCaseName, testName, testDisplayName) \
	class testCaseName##testName##Test : public testCaseName##Declare##Test \
	{ \
	public: \
		testCaseName##testName##Test() \
		: testCaseName##Declare##Test(#testCaseName, testDisplayName) \
		{ \
		} \
\
		void run(); \
	} testCaseName##testName##Instance; \
\
	void testCaseName##testName##Test::run()

/**
 * Define a test in a TestCase using test fixtures. Put the test code between brackets after this
 * macro.
 *
 * Only use this if test fixtures were declared earlier, in this order: DECLARE, SETUP, TEARDOWN.
 * @param testCaseName TestCase the test belongs to. The same name as DECLARE, SETUP and TEARDOWN.
 * @param testName Unique test name.
 */
#define TEST(testCaseName, testName) TEST_M(testCaseName, testName, #testName)

/**
 * Location to declare variables and objects: the members accessible by TESTF, SETUP and TEARDOWN.
 *
 * Do not use brackets after this macro, and do not initialize any members here.
 *
 * @param testCaseName TestCase name of the fixtures
 * @see END_DECLARE for more information.
 */
#define DECLARE(testCaseName) \
	class testCaseName##Declare##Test : public Test \
	{ \
	public: \
		testCaseName##Declare##Test(const wxString &testCaseName, const wxString &testName) \
		: Test(testCaseName, testName) \
		{ \
		} \
		virtual void run() = 0;

/**
 * Ending macro, used after declaring members with DECLARE.
 */
#define END_DECLARE \
	} \
	;

/// Creates a fixture with no setup/teardown or member variables.
#define DECLARE_SIMPLE(testCaseName) \
	DECLARE(testCaseName) \
	END_DECLARE;

} // namespace muleunit
#endif // TEST_H

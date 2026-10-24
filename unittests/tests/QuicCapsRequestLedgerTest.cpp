#include <muleunit/test.h>

#include <QuicCapsRequestLedger.h>

#include <array>

using namespace muleunit;
DECLARE_SIMPLE(QuicCapsRequestLedger)

namespace
{
std::array<uint8_t, 16> Bytes(uint8_t value)
{
	std::array<uint8_t, 16> result{};
	result.fill(value);
	return result;
}
} // namespace

TEST(QuicCapsRequestLedger, ExactFieldsAndConsumeOnce)
{
	CQuicCapsRequestLedger ledger(2);
	ASSERT_TRUE(ledger.Add(Bytes(1), 0x01020304, 4662, Bytes(9), 100));
	ASSERT_TRUE(!ledger.Consume(Bytes(2), 0x01020304, 4662, Bytes(9), 101));
	ASSERT_TRUE(!ledger.Consume(Bytes(1), 0x01020304, 4662, Bytes(8), 101));
	ASSERT_TRUE(ledger.Consume(Bytes(1), 0x01020304, 4662, Bytes(9), 101));
	ASSERT_TRUE(!ledger.Consume(Bytes(1), 0x01020304, 4662, Bytes(9), 101));
}

TEST(QuicCapsRequestLedger, DuplicateAndExpiry)
{
	CQuicCapsRequestLedger ledger(2);
	ASSERT_TRUE(ledger.Add(Bytes(1), 1, 2, Bytes(3), 0));
	ASSERT_TRUE(!ledger.Add(Bytes(1), 1, 2, Bytes(3), 1));
	ASSERT_EQUALS(1, ledger.Size(1));
	ASSERT_TRUE(!ledger.Consume(Bytes(1), 1, 2, Bytes(3), 30000));
	ASSERT_EQUALS(0, ledger.Size(30000));
}

TEST(QuicCapsRequestLedger, BoundedOldestEvictionAndZeroCapacity)
{
	CQuicCapsRequestLedger ledger(2);
	ASSERT_TRUE(ledger.Add(Bytes(1), 1, 1, Bytes(1), 0));
	ASSERT_TRUE(ledger.Add(Bytes(2), 2, 2, Bytes(2), 0));
	ASSERT_TRUE(ledger.Add(Bytes(3), 3, 3, Bytes(3), 0));
	ASSERT_TRUE(!ledger.Consume(Bytes(1), 1, 1, Bytes(1), 1));
	ASSERT_TRUE(ledger.Consume(Bytes(2), 2, 2, Bytes(2), 1));
	CQuicCapsRequestLedger empty(0);
	ASSERT_TRUE(!empty.Add(Bytes(1), 1, 1, Bytes(1), 0));
	ASSERT_EQUALS(0, empty.Size(0));
}

//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//

#include <muleunit/test.h>

#include "NatRendezvousPolicy.h"

#include <string>

using namespace muleunit;

DECLARE_SIMPLE(NatRendezvousPolicy)

namespace
{
PeerAddressing::UdpEndpoint Endpoint(const char *address, uint16_t port)
{
	return { CNetworkAddress::FromString(address), port };
}
} // namespace

TEST(NatRendezvousPolicy, AcceptsOnlyMatchingObservedEndpoint)
{
	const auto claimed = Endpoint("192.0.2.7", 4672);
	const auto observed = Endpoint("192.0.2.7", 4672);
	const auto accepted = NatRendezvous::AcceptObservedEndpoint(claimed, observed);

	ASSERT_TRUE(accepted.has_value());
	ASSERT_TRUE(observed.address == accepted->address);
	ASSERT_EQUALS(observed.port, accepted->port);
}

TEST(NatRendezvousPolicy, RejectsMismatchedEndpoint)
{
	const auto claimed = Endpoint("192.0.2.7", 4672);
	const auto observed = Endpoint("192.0.2.8", 4672);
	ASSERT_FALSE(NatRendezvous::AcceptObservedEndpoint(claimed, observed).has_value());
	ASSERT_FALSE(NatRendezvous::AcceptObservedEndpoint(claimed, Endpoint("192.0.2.7", 4673)).has_value());
}

TEST(NatRendezvousPolicy, ThrottlesRequesterAndExpiresBudget)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto requester = CNetworkAddress::FromString("192.0.2.7");

	ASSERT_TRUE(limiter.Admit(requester, 1000));
	ASSERT_FALSE(limiter.Admit(requester, 1001));
	ASSERT_TRUE(limiter.Admit(requester, 1000 + NatRendezvous::kRequestThrottleMs));
}

TEST(NatRendezvousPolicy, LimiterTakesItsOwnWindowAndCap)
{
	NatRendezvous::CRequesterLimiter limiter(500, 2);
	const auto first = CNetworkAddress::FromString("192.0.2.7");

	ASSERT_TRUE(limiter.Admit(first, 1000));
	ASSERT_FALSE(limiter.Admit(first, 1499));
	ASSERT_TRUE(limiter.Admit(first, 1500));
	// A third scope evicts the oldest, which then starts a new window.
	ASSERT_TRUE(limiter.Admit(CNetworkAddress::FromString("192.0.2.8"), 1501));
	ASSERT_TRUE(limiter.Admit(CNetworkAddress::FromString("192.0.2.9"), 1502));
	ASSERT_TRUE(limiter.Admit(first, 1503));
}

TEST(NatRendezvousPolicy, RejectsUnusableRequester)
{
	NatRendezvous::CRequesterLimiter limiter;
	ASSERT_FALSE(limiter.Admit(CNetworkAddress(), 1000));
	ASSERT_FALSE(limiter.Admit(CNetworkAddress::FromString("0.0.0.0"), 1000));
	ASSERT_FALSE(limiter.Admit(CNetworkAddress::FromString("::"), 1000));
	ASSERT_FALSE(limiter.Admit(CNetworkAddress::FromString("::ffff:0.0.0.0"), 1000));
}

TEST(NatRendezvousPolicy, ReanchorsWindowOnClockRollback)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto requester = CNetworkAddress::FromString("192.0.2.7");

	ASSERT_TRUE(limiter.Admit(requester, 2000));
	ASSERT_TRUE(limiter.Admit(requester, 1000));
	ASSERT_FALSE(limiter.Admit(requester, 1001));
	ASSERT_FALSE(limiter.Admit(requester, 1000 + NatRendezvous::kRequestThrottleMs - 1));
	ASSERT_TRUE(limiter.Admit(requester, 1000 + NatRendezvous::kRequestThrottleMs));
}

TEST(NatRendezvousPolicy, EvictsOldestScopeToAdmit257thRequester)
{
	NatRendezvous::CRequesterLimiter limiter;
	// Insert the oldest entry last so eviction cannot rely on insertion order.
	for (std::size_t i = 1; i < NatRendezvous::kMaxTrackedRequesters; ++i) {
		const auto address = "192.0.2." + std::to_string(i);
		ASSERT_TRUE(limiter.Admit(CNetworkAddress::FromString(address.c_str()), 1001));
	}
	const auto oldest = CNetworkAddress::FromString("192.0.2.0");
	const auto newest = CNetworkAddress::FromString("192.0.3.1");
	ASSERT_TRUE(limiter.Admit(oldest, 1000));
	ASSERT_TRUE(limiter.Admit(newest, 1002));
	ASSERT_FALSE(limiter.Admit(newest, 1003));
	ASSERT_FALSE(limiter.Admit(CNetworkAddress::FromString("192.0.2.1"), 1003));
	ASSERT_TRUE(limiter.Admit(oldest, 1003));
}

TEST(NatRendezvousPolicy, FreesExpiredSlotBeforeEvictingLiveScope)
{
	NatRendezvous::CRequesterLimiter limiter;
	for (std::size_t i = 0; i < NatRendezvous::kMaxTrackedRequesters; ++i) {
		const auto address = "192.0.2." + std::to_string(i);
		ASSERT_TRUE(
			limiter.Admit(CNetworkAddress::FromString(address.c_str()), i == 0 ? 1000 : 1001));
	}
	const auto now = 1000 + NatRendezvous::kRequestThrottleMs;
	ASSERT_TRUE(limiter.Admit(CNetworkAddress::FromString("192.0.3.1"), now));
	for (std::size_t i = 1; i < NatRendezvous::kMaxTrackedRequesters; ++i) {
		const auto address = "192.0.2." + std::to_string(i);
		ASSERT_FALSE(limiter.Admit(CNetworkAddress::FromString(address.c_str()), now));
	}
}

TEST(NatRendezvousPolicy, SharesBudgetAcrossMappedAndPlainIPv4)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto plain = CNetworkAddress::FromString("192.0.2.7");
	const auto mapped = CNetworkAddress::FromString("::ffff:192.0.2.7");

	ASSERT_TRUE(limiter.Admit(plain, 1000));
	ASSERT_FALSE(limiter.Admit(mapped, 1001));
	ASSERT_TRUE(limiter.Admit(mapped, 1000 + NatRendezvous::kRequestThrottleMs));
	ASSERT_FALSE(limiter.Admit(plain, 1001 + NatRendezvous::kRequestThrottleMs));
}

TEST(NatRendezvousPolicy, SharesBudgetAcrossAnIPv6Slash64)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto first = CNetworkAddress::FromString("2001:db8:1:2::1");
	const auto second = CNetworkAddress::FromString("2001:db8:1:2::2");

	ASSERT_TRUE(limiter.Admit(first, 1000));
	ASSERT_FALSE(limiter.Admit(second, 1001));
}

TEST(NatRendezvousPolicy, KeepsAdjacentIPv6PrefixesIndependent)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto first = CNetworkAddress::FromString("2001:db8:1:2::1");
	const auto second = CNetworkAddress::FromString("2001:db8:1:3::1");

	ASSERT_TRUE(limiter.Admit(first, 1000));
	ASSERT_TRUE(limiter.Admit(second, 1001));
}

namespace
{
using Requests = NatRendezvous::CKadRendezvousRequests;
using Action = NatRendezvous::SKadRendezvousAction;

constexpr std::uint32_t kTargetIP = 0x0A71000C;
constexpr std::uint16_t kTargetPort = 4672;

NatRendezvous::KadRendezvousHash HashOf(std::uint8_t value)
{
	NatRendezvous::KadRendezvousHash hash{};
	hash.fill(value);
	return hash;
}

NatRendezvous::SKadRendezvousTarget Target(
	std::uint8_t hash, std::uint32_t ip = kTargetIP, std::uint16_t port = kTargetPort)
{
	return { ip, port, HashOf(hash), 0x0B71000C, 5000 };
}

std::size_t CountOf(const std::vector<Action> &actions, Action::EKind kind)
{
	return static_cast<std::size_t>(std::count_if(actions.begin(),
		actions.end(),
		[kind](const Action &action) { return action.kind == kind; }));
}
} // namespace

TEST(NatRendezvousPolicy, RequesterSendsThreeTimesFiveSecondsApart)
{
	Requests requests;
	ASSERT_TRUE(requests.Start(Target(1), 1000));

	const auto first = requests.TakeDue(1000);
	ASSERT_EQUALS(1u, CountOf(first, Action::Send));
	ASSERT_TRUE(first.front().target.hash == HashOf(1));
	ASSERT_EQUALS(0u, CountOf(requests.TakeDue(1000), Action::Send));
	ASSERT_EQUALS(0u, CountOf(requests.TakeDue(5999), Action::Send));
	ASSERT_EQUALS(1u, CountOf(requests.TakeDue(6000), Action::Send));
	ASSERT_EQUALS(1u, CountOf(requests.TakeDue(11000), Action::Send));
	ASSERT_EQUALS(0u, CountOf(requests.TakeDue(16000), Action::Send));
}

TEST(NatRendezvousPolicy, RequesterDialsTheKadEndpointOnceWithoutAPunch)
{
	Requests requests;
	ASSERT_TRUE(requests.Start(Target(1), 1000));
	requests.TakeDue(1000);

	ASSERT_EQUALS(0u,
		CountOf(requests.TakeDue(1000 + NatRendezvous::kRequesterFallbackDialMs - 1),
			Action::FallbackDial));
	const auto due = requests.TakeDue(1000 + NatRendezvous::kRequesterFallbackDialMs);
	ASSERT_EQUALS(1u, CountOf(due, Action::FallbackDial));
	ASSERT_EQUALS(0u, CountOf(requests.TakeDue(9000), Action::FallbackDial));
}

TEST(NatRendezvousPolicy, RequesterDialsTheFirstPunchAndStopsSending)
{
	Requests requests;
	ASSERT_TRUE(requests.Start(Target(1), 1000));
	requests.TakeDue(1000);

	// The punch comes from the target's mapping, which need not be its Kad port.
	const auto punched = requests.TakePunch(kTargetIP, 40000, 2000);
	ASSERT_TRUE(punched.has_value());
	ASSERT_TRUE(punched->hash == HashOf(1));
	ASSERT_FALSE(requests.TakePunch(kTargetIP, 40000, 2100).has_value());

	const auto later = requests.TakeDue(20000);
	ASSERT_EQUALS(0u, CountOf(later, Action::FallbackDial));
	ASSERT_EQUALS(0u, CountOf(later, Action::Send));
}

TEST(NatRendezvousPolicy, RequesterPunchFromAnotherPortNeedsALoneAttemptAtTheAddress)
{
	Requests requests;
	ASSERT_TRUE(requests.Start(Target(1), 1000));
	ASSERT_TRUE(requests.Start(Target(2, kTargetIP, 4700), 1000));

	ASSERT_FALSE(requests.TakePunch(kTargetIP, 40000, 2000).has_value());
	const auto exact = requests.TakePunch(kTargetIP, 4700, 2000);
	ASSERT_TRUE(exact.has_value());
	ASSERT_TRUE(exact->hash == HashOf(2));
	ASSERT_FALSE(requests.TakePunch(0x0C71000C, kTargetPort, 2000).has_value());
}

TEST(NatRendezvousPolicy, RequesterAttemptExpiresAndCoolsDown)
{
	Requests requests;
	ASSERT_TRUE(requests.Start(Target(1), 1000));
	ASSERT_FALSE(requests.Start(Target(1), 1001));
	requests.TakeDue(1000);

	ASSERT_EQUALS(0u,
		CountOf(requests.TakeDue(1000 + NatRendezvous::kRequesterAttemptMs - 1), Action::Expired));
	const auto expired = requests.TakeDue(1000 + NatRendezvous::kRequesterAttemptMs);
	ASSERT_EQUALS(1u, CountOf(expired, Action::Expired));
	ASSERT_FALSE(requests.TakePunch(kTargetIP, kTargetPort, 1000 + NatRendezvous::kRequesterAttemptMs)
			     .has_value());

	const std::uint64_t expiredAt = 1000 + NatRendezvous::kRequesterAttemptMs;
	ASSERT_FALSE(requests.Start(Target(1), expiredAt + NatRendezvous::kRequesterCooldownMs - 1));
	ASSERT_TRUE(requests.Start(Target(1), expiredAt + NatRendezvous::kRequesterCooldownMs));
}

TEST(NatRendezvousPolicy, RequesterHelloEndsTheAttemptWithoutACooldown)
{
	Requests requests;
	ASSERT_TRUE(requests.Start(Target(1), 1000));
	ASSERT_FALSE(requests.Complete(kTargetIP, HashOf(2)));
	ASSERT_TRUE(requests.Complete(kTargetIP, HashOf(1)));
	ASSERT_FALSE(requests.Complete(kTargetIP, HashOf(1)));

	ASSERT_EQUALS(0u, requests.TakeDue(60000).size());
	ASSERT_TRUE(requests.Start(Target(1), 60000));
}

TEST(NatRendezvousPolicy, RequesterFileNotFoundMeansNoRendezvousForGood)
{
	Requests requests;
	ASSERT_FALSE(requests.NoRendezvous(kTargetIP, kTargetPort).has_value());
	ASSERT_TRUE(requests.Start(Target(1), 1000));

	const auto ended = requests.NoRendezvous(kTargetIP, kTargetPort);
	ASSERT_TRUE(ended.has_value());
	ASSERT_TRUE(ended->hash == HashOf(1));
	ASSERT_FALSE(requests.NoRendezvous(kTargetIP, kTargetPort).has_value());
	ASSERT_EQUALS(0u, requests.TakeDue(2000).size());

	ASSERT_FALSE(requests.Start(Target(1), 1000 + 24ull * 3600 * 1000));
	ASSERT_TRUE(requests.Start(Target(2), 2000));
}

TEST(NatRendezvousPolicy, RequesterAttemptsAreBounded)
{
	Requests requests;
	for (std::size_t i = 0; i < NatRendezvous::kMaxRequesterAttempts; ++i) {
		ASSERT_TRUE(requests.Start(
			Target(static_cast<std::uint8_t>(i + 1), kTargetIP + static_cast<std::uint32_t>(i)),
			1000));
	}
	ASSERT_FALSE(requests.Start(Target(0xF0, 0x0D71000C), 1000));
	ASSERT_TRUE(requests.Complete(kTargetIP, HashOf(1)));
	ASSERT_TRUE(requests.Start(Target(0xF0, 0x0D71000C), 1000));
}

TEST(NatRendezvousPolicy, RequesterDoesNotDialAgainTheEndpointTheFallbackDialled)
{
	Requests requests;
	ASSERT_TRUE(requests.Start(Target(1), 1000));
	const auto due = requests.TakeDue(1000 + NatRendezvous::kRequesterFallbackDialMs);
	ASSERT_EQUALS(1u, CountOf(due, Action::FallbackDial));

	// The punch shows the endpoint the fallback already dialled: no second dial, and no more sends.
	ASSERT_FALSE(requests.TakePunch(kTargetIP, kTargetPort, 4500).has_value());
	ASSERT_EQUALS(0u, CountOf(requests.TakeDue(20000), Action::Send));
	// A punch from another port is a mapping the fallback did not reach: that one is dialled, once.
	const auto moved = requests.TakePunch(kTargetIP, 40000, 21000);
	ASSERT_TRUE(moved.has_value());
	ASSERT_TRUE(moved->hash == HashOf(1));
	ASSERT_FALSE(requests.TakePunch(kTargetIP, 40001, 21100).has_value());
}

TEST(NatRendezvousPolicy, RequesterDialsThePunchFromTheKadPortBeforeTheFallback)
{
	Requests requests;
	ASSERT_TRUE(requests.Start(Target(1), 1000));
	requests.TakeDue(1000);
	ASSERT_TRUE(requests.TakePunch(kTargetIP, kTargetPort, 2000).has_value());
	ASSERT_EQUALS(0u, CountOf(requests.TakeDue(20000), Action::FallbackDial));
}

namespace
{
NatRendezvous::KadRendezvousHash HashOfIndex(std::size_t index)
{
	NatRendezvous::KadRendezvousHash hash{};
	hash.fill(0xEE);
	hash[0] = static_cast<std::uint8_t>(index);
	hash[1] = static_cast<std::uint8_t>(index >> 8);
	return hash;
}
} // namespace

TEST(NatRendezvousPolicy, RequesterCooldownsDoNotPushOutANoRendezvousMark)
{
	Requests requests;
	NatRendezvous::SKadRendezvousTarget silent{
		kTargetIP, kTargetPort, HashOfIndex(0xFFFF), 0x0B71000C, 5000
	};
	ASSERT_TRUE(requests.Start(silent, 1000));
	ASSERT_TRUE(requests.NoRendezvous(kTargetIP, kTargetPort).has_value());

	// More timed-out attempts than there is room for marks, each with a cooldown of its own.
	for (std::size_t i = 0; i < NatRendezvous::kMaxRequesterMarks + 1; ++i) {
		NatRendezvous::SKadRendezvousTarget target{ 0x0C000000u + static_cast<std::uint32_t>(i),
			kTargetPort,
			HashOfIndex(i),
			0x0B71000C,
			5000 };
		ASSERT_TRUE(requests.Start(target, 1000));
		ASSERT_EQUALS(1u,
			CountOf(requests.TakeDue(1000 + NatRendezvous::kRequesterAttemptMs),
				Action::Expired));
	}
	ASSERT_FALSE(requests.Start(silent, 1000 + NatRendezvous::kRequesterAttemptMs));
}

// A known source at the attempt's endpoint answers OP_FILENOTFOUND: it is the target itself only when
// it carries the attempt's hash, as a source that connected before does.
TEST(NatRendezvousPolicy, RequesterFileNotFoundFromAKnownSourceIsTheTargetsOnlyWithItsHash)
{
	Requests requests;
	ASSERT_TRUE(requests.Start(Target(1), 1000));
	ASSERT_FALSE(requests.NoRendezvous(kTargetIP, kTargetPort, HashOf(2)).has_value());
	ASSERT_EQUALS(1u, requests.Size());

	const auto ended = requests.NoRendezvous(kTargetIP, kTargetPort, HashOf(1));
	ASSERT_TRUE(ended.has_value());
	ASSERT_TRUE(ended->hash == HashOf(1));
	ASSERT_EQUALS(0u, requests.Size());
	ASSERT_FALSE(requests.Start(Target(1), 2000));
}

// A restarted source has a new port while nodes still hand out its older records, in any order.
TEST(NatRendezvousPolicy, RequesterKeepsTheEndpointsEarlierRecordsNamed)
{
	using Endpoint = NatRendezvous::SKadEndpoint;
	std::vector<Endpoint> former;
	const Endpoint fresh{ kTargetIP, 53927 };
	const Endpoint stale{ kTargetIP, 54310 };
	NatRendezvous::NoteKadEndpointChange(former, Endpoint{}, fresh);
	ASSERT_TRUE(former.empty());

	// The current record first, a stale one last: the current endpoint is kept as a former one.
	NatRendezvous::NoteKadEndpointChange(former, fresh, stale);
	ASSERT_EQUALS(1u, former.size());
	ASSERT_TRUE(former[0] == fresh);
	NatRendezvous::NoteKadEndpointChange(former, stale, stale);
	ASSERT_EQUALS(1u, former.size());
	// An endpoint that comes back leaves the list.
	NatRendezvous::NoteKadEndpointChange(former, stale, fresh);
	ASSERT_EQUALS(1u, former.size());
	ASSERT_TRUE(former[0] == stale);

	Endpoint current = fresh;
	for (std::uint16_t port = 1; port <= 5; ++port) {
		const Endpoint next{ kTargetIP, port };
		NatRendezvous::NoteKadEndpointChange(former, current, next);
		current = next;
	}
	ASSERT_EQUALS(NatRendezvous::kMaxFormerKadEndpoints, former.size());
	ASSERT_TRUE(former[0] == (Endpoint{ kTargetIP, 4 }));
}

TEST(NatRendezvousPolicy, RequesterTakesAPunchFromAFormerEndpoint)
{
	Requests requests;
	NatRendezvous::SKadRendezvousTarget target = Target(1);
	// Only an earlier record named the address the source punches from.
	target.former.push_back({ 0x0C71000C, 53927 });
	ASSERT_TRUE(requests.Start(target, 1000));

	const auto punch = requests.TakePunch(0x0C71000C, 53927, 2000);
	ASSERT_TRUE(punch.has_value());
	ASSERT_TRUE(punch->hash == HashOf(1));
	ASSERT_TRUE(requests.Complete(0x0C71000C, HashOf(1)));
	ASSERT_EQUALS(0u, requests.Size());
}

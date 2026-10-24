// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include "UploadBuffering.h"
#include <limits>
#include <array>
#include <thread>
#include <vector>

using namespace muleunit;
DECLARE_SIMPLE(UploadBuffering)

namespace
{
constexpr uint32_t block = 184320;
}

TEST(UploadBuffering, TargetFollowsTimeWithoutTheOldRateCliff)
{
	ASSERT_EQUALS(block + 1, UploadBufferTarget(0, block));
	ASSERT_EQUALS(block + 1, UploadBufferTarget(1024, block));
	ASSERT_EQUALS(2u * 100 * 1024 + 1, UploadBufferTarget(100 * 1024, block));
	ASSERT_EQUALS(2u * 512 * 1024 + 1, UploadBufferTarget(512 * 1024, block));
	ASSERT_EQUALS(UploadBufferTarget(75 * 1024, block), UploadBufferTarget(75 * 1024 + 1, block));
	ASSERT_EQUALS(4u * 1024 * 1024, UploadBufferTarget(std::numeric_limits<uint32_t>::max(), block));
}

TEST(UploadBuffering, TinyRequestsHaveAFixedChargeAndInvalidSizesCannotOverflow)
{
	UploadBufferBudget budget;
	auto client = std::make_shared<UploadBufferBudget::Account>();
	ASSERT_TRUE(budget.Reserve(client, 0) == nullptr);
	ASSERT_TRUE(budget.Reserve(client, std::numeric_limits<uint32_t>::max()) == nullptr);
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
	auto read = budget.Reserve(client, 1);
	ASSERT_TRUE(read != nullptr);
	ASSERT_TRUE(budget.Used() >= 4096);
	read.reset();
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
	ASSERT_EQUALS(uint64_t{ 0 }, client->used.load());
}

TEST(UploadBuffering, OneClientCannotExhaustOtherClientsBudget)
{
	UploadBufferBudget budget;
	auto client = std::make_shared<UploadBufferBudget::Account>();
	std::vector<UploadBufferBudget::Lease> pending;
	for (unsigned i = 0; i < 64; ++i) {
		auto lease = budget.Reserve(client, 3 * block);
		if (!lease) {
			break;
		}
		pending.push_back(std::move(lease));
	}
	ASSERT_FALSE(pending.empty());
	ASSERT_TRUE(pending.size() < 64);
	ASSERT_TRUE(client->used <= UploadBufferBudget::kClientLimit);
	auto other = std::make_shared<UploadBufferBudget::Account>();
	ASSERT_TRUE(budget.Reserve(other, block) != nullptr);
	pending.clear();
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
}

TEST(UploadBuffering, GlobalLimitCountsAllClientsAndFailedAdmissionRollsBack)
{
	UploadBufferBudget budget;
	std::vector<UploadBufferBudget::Lease> pending;
	auto next = std::make_shared<UploadBufferBudget::Account>();
	for (unsigned i = 0; i < 256; ++i) {
		auto lease = budget.Reserve(next, 3 * block);
		if (!lease) {
			break;
		}
		pending.push_back(std::move(lease));
		next = std::make_shared<UploadBufferBudget::Account>();
	}
	ASSERT_TRUE(pending.size() > 4);
	ASSERT_TRUE(pending.size() < 256);
	ASSERT_TRUE(budget.Used() <= UploadBufferBudget::kGlobalLimit);
	ASSERT_EQUALS(uint64_t{ 0 }, next->used.load());
	pending.pop_back();
	ASSERT_TRUE(budget.Reserve(next, 3 * block) != nullptr);
	pending.clear();
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
}

TEST(UploadBuffering, ChargeFollowsReadAndAllPacketsIncludingCurrentPacket)
{
	UploadBufferBudget budget;
	auto client = std::make_shared<UploadBufferBudget::Account>();
	auto read = budget.Reserve(client, block);
	const auto charge = budget.Used();
	auto firstPacket = read;
	auto lastPacket = read;
	read.reset();
	ASSERT_EQUALS(charge, budget.Used());
	firstPacket.reset();
	ASSERT_EQUALS(charge, budget.Used());
	// Truncating queued packets must keep the current packet's reservation.
	auto currentPacket = std::move(lastPacket);
	ASSERT_EQUALS(charge, budget.Used());
	currentPacket.reset();
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
}

TEST(UploadBuffering, OutstandingPacketsCanOutliveTheCoordinator)
{
	auto client = std::make_shared<UploadBufferBudget::Account>();
	UploadBufferBudget::Lease packet;
	{
		UploadBufferBudget budget;
		packet = budget.Reserve(client, block);
	}
	ASSERT_TRUE(client->used > 0);
	packet.reset();
	ASSERT_EQUALS(uint64_t{ 0 }, client->used.load());
}

TEST(UploadBuffering, AdmissionsAndSenderReleasesCanRunConcurrently)
{
	UploadBufferBudget budget;
	auto client = std::make_shared<UploadBufferBudget::Account>();
	std::atomic<bool> exceeded{ false };
	std::array<std::thread, 8> threads;
	for (auto &thread : threads) {
		thread = std::thread([&] {
			for (unsigned i = 0; i < 500; ++i) {
				auto lease = budget.Reserve(client, 3 * block);
				if (client->used > UploadBufferBudget::kClientLimit ||
					budget.Used() > UploadBufferBudget::kGlobalLimit) {
					exceeded = true;
				}
				std::this_thread::yield();
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	ASSERT_FALSE(exceeded.load());
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
	ASSERT_EQUALS(uint64_t{ 0 }, client->used.load());
}

TEST(UploadBuffering, DenialReasonDistinguishesClientAndGlobalPressure)
{
	UploadBufferBudget budget;
	auto client = std::make_shared<UploadBufferBudget::Account>();
	UploadBufferBudget::Denial denial;
	client->used = UploadBufferBudget::kClientLimit;
	ASSERT_TRUE(budget.Reserve(client, block, &denial) == nullptr);
	ASSERT_TRUE(denial == UploadBufferBudget::Denial::Client);
	client->used = 0;
	std::vector<UploadBufferBudget::Lease> pending;
	for (unsigned i = 0; i < 256; ++i) {
		auto other = std::make_shared<UploadBufferBudget::Account>();
		auto lease = budget.Reserve(other, 3 * block, &denial);
		if (!lease) {
			break;
		}
		pending.push_back(std::move(lease));
	}
	ASSERT_TRUE(denial == UploadBufferBudget::Denial::Global);
	ASSERT_EQUALS(uint64_t{ 0 }, client->used.load());
	pending.clear();
	ASSERT_TRUE(budget.Reserve(client, block, &denial) != nullptr);
	ASSERT_TRUE(denial == UploadBufferBudget::Denial::None);
}

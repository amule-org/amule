//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#ifndef UDPOBFUSCATIONPOLICY_H
#define UDPOBFUSCATIONPOLICY_H

#include <cstdint>

enum class EUdpSendForm
{
	Plain,
	Obfuscated,
	Drop
};

/**
 * How a queued UDP packet leaves. Client obfuscation needs our public IP, so an ordinary packet falls
 * back to plain without one. A packet that must be obfuscated (one carrying our user hash) is dropped
 * instead: the public IP can be lost between queueing and sending.
 */
inline EUdpSendForm DecideUdpSendForm(bool encrypt, bool requireObfuscation, bool kad, std::uint32_t publicIP)
{
	if (encrypt && (publicIP != 0 || kad)) {
		return EUdpSendForm::Obfuscated;
	}
	return requireObfuscation ? EUdpSendForm::Drop : EUdpSendForm::Plain;
}

#endif // UDPOBFUSCATIONPOLICY_H

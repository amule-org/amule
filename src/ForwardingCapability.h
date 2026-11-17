#pragma once

// Deliberately not included by production code: CPreferences::CanForwardPorts()
// provides the runtime capability combination, and this header only pins the
// boolean contract in tests so a future change to that combination is caught.
// See ForwardingCapabilityTest and the PCP review notes.
namespace forwarding
{
struct Capability
{
	bool upnp;
	bool pcp;

	bool CanForward() const { return upnp || pcp; }
};

inline bool CanForward(bool upnpAvailable, bool pcpAvailable)
{
	return Capability{ upnpAvailable, pcpAvailable }.CanForward();
}
} // namespace forwarding

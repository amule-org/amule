//
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Marcelo Roberto Jimenez ( phoenix@amule.org )
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include "config.h" // Needed for ENABLE_UPNP

#ifdef ENABLE_UPNP

// check for broken Debian-hacked libUPnP
#include <upnp.h>
#ifdef STRING_H // defined in UpnpString.h Yes, I would have liked UPNPSTRING_H much better.
#define BROKEN_DEBIAN_LIBUPNP
#endif

/* Fix for broken UPNP_VERSION macro */
#if UPNP_VERSION_MAJOR == 17

#undef UPNP_VERSION_MAJOR
#undef UPNP_VERSION_MINOR
#undef UPNP_VERSION_PATCH
#undef UPNP_VERSION

#define UPNP_VERSION_MAJOR 1
#define UPNP_VERSION_MINOR 14
#define UPNP_VERSION_PATCH 18
#define UPNP_VERSION ((UPNP_VERSION_MAJOR * 10000) + (UPNP_VERSION_MINOR * 100) + (UPNP_VERSION_PATCH))

#endif /* UPNP_MAJOR_VERSION == 17 */

#include "UPnPBase.h"
#include "IPv6PinholePolicy.h"  // Needed for ShouldRenewIPv6Pinhole
#include "UPnPLocationPolicy.h" // Needed for IsIPv6Location

#include <algorithm> // For transform()
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include "CCtypeAsciiScope.h" // Needed to pin LC_CTYPE = C around the
			      // ASCII-only tolower() comparisons below
			      // (Turkish-i: tolower('I') = U+0131 under
			      // tr_TR.UTF-8, breaks UPnP direction /
			      // device-type matching).

#ifdef BROKEN_DEBIAN_LIBUPNP
#define GET_UPNP_STRING(a) UpnpString_get_String(a)
#else
#define GET_UPNP_STRING(a) (a)
#endif

std::string stdEmptyString;

const char s_argument[] = "argument";
const char s_argumentList[] = "argumentList";
const char s_action[] = "action";
const char s_actionList[] = "actionList";
const char s_allowedValue[] = "allowedValue";
const char s_allowedValueList[] = "allowedValueList";
const char s_stateVariable[] = "stateVariable";
const char s_serviceStateTable[] = "serviceStateTable";
const char s_service[] = "service";
const char s_serviceList[] = "serviceList";
const char s_device[] = "device";
const char s_deviceList[] = "deviceList";

/// Case-insensitive std::string comparison.
static bool stdStringIsEqualCI(const std::string &s1, const std::string &s2)
{
	// Pin LC_CTYPE = C so tolower() is ASCII-deterministic regardless of the user's locale
	// (tr_TR turns 'I' into U+0131, breaking this comparison's intended semantics).
	CCtypeAsciiScope scope;
	std::string ns1(s1);
	std::string ns2(s2);
	std::transform(ns1.begin(), ns1.end(), ns1.begin(), tolower);
	std::transform(ns2.begin(), ns2.end(), ns2.begin(), tolower);
	return ns1 == ns2;
}

static bool stdStringStartsWithCI(const std::string &s, const std::string &prefix)
{
	if (s.size() < prefix.size()) {
		return false;
	}
	// Same reason as stdStringIsEqualCI: tolower() must be ASCII here.
	CCtypeAsciiScope scope;
	std::string head = s.substr(0, prefix.size());
	std::string p = prefix;
	std::transform(head.begin(), head.end(), head.begin(), tolower);
	std::transform(p.begin(), p.end(), p.begin(), tolower);
	return head == p;
}

CUPnPPortMapping::CUPnPPortMapping(
	int port, const std::string &protocol, bool enabled, const std::string &description)
: m_port()
, m_protocol(protocol)
, m_enabled(enabled ? "1" : "0")
, m_description(description)
, m_key()
{
	std::ostringstream oss;
	oss << port;
	m_port = oss.str();
	m_key = m_protocol + m_port;
}

namespace UPnP
{

static const std::string ROOT_DEVICE("upnp:rootdevice");

namespace Device
{
static const std::string IGW("urn:schemas-upnp-org:device:InternetGatewayDevice:1");
static const std::string WAN("urn:schemas-upnp-org:device:WANDevice:1");
static const std::string WAN_Connection("urn:schemas-upnp-org:device:WANConnectionDevice:1");
static const std::string LAN("urn:schemas-upnp-org:device:LANDevice:1");
} // namespace Device

namespace Service
{
static const std::string Layer3_Forwarding("urn:schemas-upnp-org:service:Layer3Forwarding:1");
static const std::string WAN_Common_Interface_Config(
	"urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1");
static const std::string WAN_IP_Connection("urn:schemas-upnp-org:service:WANIPConnection:1");
static const std::string WAN_PPP_Connection("urn:schemas-upnp-org:service:WANPPPConnection:1");
static const std::string WAN_IPv6_Firewall_Control("urn:schemas-upnp-org:service:WANIPv6FirewallControl:1");
} // namespace Service

static bool IsIPv6FirewallService(const std::string &type);
static bool IsWANMappingService(const std::string &type);

// Decide whether an SSDP NT/ST is something amule wants to learn about. The whitelist is the IGW
// family plus upnp:rootdevice, which is opaque from SSDP alone (description.xml has to be fetched
// to classify it). Anything else is dropped before that fetch, which keeps the libupnp ThreadPool
// queue from filling on busy LANs and stops spurious "Error retrieving device description" lines
// for devices we have no use for.
static bool IsWANRelatedDeviceType(const std::string &type)
{
	if (type.empty()) {
		// Malformed announcement; let the existing parser deal with it
		// so we don't change behaviour for the pathological case.
		return true;
	}
	if (stdStringIsEqualCI(type, ROOT_DEVICE)) {
		return true;
	}
	// Versioned UPnP URNs: prefix-match so future :2/:3 revisions don't
	// need code changes.
	static const std::string prefixes[] = {
		"urn:schemas-upnp-org:device:InternetGatewayDevice:",
		"urn:schemas-upnp-org:device:WANDevice:",
		"urn:schemas-upnp-org:device:WANConnectionDevice:",
		"urn:schemas-upnp-org:device:LANDevice:",
		"urn:schemas-upnp-org:service:Layer3Forwarding:",
		"urn:schemas-upnp-org:service:WANCommonInterfaceConfig:",
	};
	for (const std::string &prefix : prefixes) {
		if (stdStringStartsWithCI(type, prefix)) {
			return true;
		}
	}
	return IsWANMappingService(type) || IsIPv6FirewallService(type);
}

// Case-insensitive match of a device/service type URN against a fully-versioned reference URN,
// ignoring the trailing ":<version>". IGD:2 gateways advertise their embedded devices and services
// with a ":2" suffix, so classifying by an exact ":1" string would skip them even though libupnp's
// version-tolerant M-SEARCH still reaches the device. The fully-versioned constants stay (they
// double as the M-SEARCH target, which must carry a version) and the tail is stripped only for
// classification. The reference's final ':' is kept in the prefix so "WANIPConnection:" cannot
// match "WANIPConnectionFoo:1".
static bool TypeMatchesIgnoringVersion(const std::string &type, const std::string &reference)
{
	std::string::size_type lastColon = reference.find_last_of(':');
	if (lastColon == std::string::npos) {
		return stdStringIsEqualCI(type, reference);
	}
	return stdStringStartsWithCI(type, reference.substr(0, lastColon + 1));
}

static bool IsIPv6FirewallService(const std::string &type)
{
	return TypeMatchesIgnoringVersion(type, Service::WAN_IPv6_Firewall_Control);
}

static bool IsWANMappingService(const std::string &type)
{
	return TypeMatchesIgnoringVersion(type, Service::WAN_IP_Connection) ||
	       TypeMatchesIgnoringVersion(type, Service::WAN_PPP_Connection);
}

static std::string ProcessErrorMessage(
	const std::string &message, int errorCode, const DOMString errorString, IXML_Document *doc)
{
	std::ostringstream msg;
	if (errorString == NULL || *errorString == 0) {
		errorString = "Not available";
	}
	if (errorCode > 0) {
		msg << "Error: " << message << ": Error code :'";
		if (doc) {
			CUPnPError e(doc);
			msg << e.getErrorCode() << "', Error description :'" << e.getErrorDescription()
			    << "'.";
		} else {
			msg << errorCode << "', Error description :'" << errorString << "'.";
		}
		AddDebugLogLineN(logUPnP, msg);
	} else {
		msg << "Error: " << message << ": UPnP SDK error: " << UpnpGetErrorMessage(errorCode) << " ("
		    << errorCode << ").";
		AddDebugLogLineN(logUPnP, msg);
	}

	return msg.str();
}

static void ProcessActionResponse(IXML_Document *RespDoc, const std::string &actionName)
{
	std::ostringstream msg;
	msg << "Response: ";
	IXML_Element *root = IXML::Document::GetRootElement(RespDoc);
	IXML_Element *child = IXML::Element::GetFirstChild(root);
	if (child) {
		while (child) {
			const DOMString childTag = IXML::Element::GetTag(child);
			std::string childValue = IXML::Element::GetTextValue(child);
			msg << "\n    " << childTag << "='" << childValue << "'";
			child = IXML::Element::GetNextSibling(child);
		}
	} else {
		msg << "\n    Empty response for action '" << actionName << "'.";
	}
	AddDebugLogLineN(logUPnP, msg);
}

// UpnpResolveURL() strcpy()s into an unsized buffer. libupnp allocates the result as
// strlen(base) + strlen(rel) + 2: an added '/' and the NUL.
static int ResolveURL(const std::string &base, const std::string &rel, std::string &absURL)
{
	std::vector<char> buf(base.length() + rel.length() + 2);
	int errcode = UpnpResolveURL(base.c_str(), rel.c_str(), &buf[0]);
	if (errcode == UPNP_E_SUCCESS) {
		absURL = &buf[0];
	}
	return errcode;
}

namespace http = boost::beast::http;

struct HttpControlURL
{
	std::string authority;
	std::string host;
	std::string port;
	std::string target;
};

static bool ParseHttpControlURL(const std::string &url, HttpControlURL &parsed)
{
	const std::string scheme("http://");
	if (url.compare(0, scheme.length(), scheme) != 0) {
		return false;
	}

	const std::string::size_type authorityStart = scheme.length();
	std::string::size_type authorityEnd = url.find_first_of("/?#", authorityStart);
	if (authorityEnd == std::string::npos) {
		authorityEnd = url.length();
	}
	const std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);
	if (authority.empty() || authority.find('@') != std::string::npos) {
		return false;
	}
	parsed.authority = authority;
	parsed.port = "80";

	if (authority[0] == '[') {
		const std::string::size_type closingBracket = authority.find(']');
		if (closingBracket == std::string::npos || closingBracket == 1) {
			return false;
		}
		parsed.host = authority.substr(1, closingBracket - 1);
		if (closingBracket + 1 < authority.length()) {
			if (authority[closingBracket + 1] != ':' ||
				closingBracket + 2 == authority.length()) {
				return false;
			}
			parsed.port = authority.substr(closingBracket + 2);
		}
		const std::string::size_type encodedZone = parsed.host.find("%25");
		if (encodedZone != std::string::npos) {
			parsed.host.replace(encodedZone, 3, "%");
		}
	} else {
		const std::string::size_type colon = authority.find(':');
		if (colon != std::string::npos) {
			if (authority.find(':', colon + 1) != std::string::npos || colon == 0 ||
				colon + 1 == authority.length()) {
				return false;
			}
			parsed.host = authority.substr(0, colon);
			parsed.port = authority.substr(colon + 1);
		} else {
			parsed.host = authority;
		}
	}

	if (parsed.host.empty()) {
		return false;
	}
	parsed.target = authorityEnd == url.length() ? "/" : url.substr(authorityEnd);
	if (parsed.target[0] == '?') {
		parsed.target.insert(0, "/");
	}
	const std::string::size_type fragment = parsed.target.find('#');
	if (fragment != std::string::npos) {
		parsed.target.erase(fragment);
	}
	return !parsed.target.empty() && parsed.target[0] == '/';
}

static int SendBoundSoapAction(const std::string &url,
	const std::string &sourceAddress,
	const std::string &serviceType,
	const std::string &actionName,
	IXML_Document *actionDocument,
	IXML_Document **responseDocument)
{
	if (responseDocument == nullptr) {
		return UPNP_E_INVALID_PARAM;
	}
	*responseDocument = nullptr;

	HttpControlURL controlURL;
	if (!ParseHttpControlURL(url, controlURL)) {
		return UPNP_E_INVALID_PARAM;
	}

	boost::system::error_code error;
	const boost::asio::ip::address_v6 localAddress =
		boost::asio::ip::make_address_v6(sourceAddress, error);
	if (error) {
		return UPNP_E_SOCKET_BIND;
	}

	boost::asio::io_context context;
	boost::asio::ip::tcp::resolver resolver(context);
	const boost::asio::ip::tcp::resolver::results_type endpoints =
		resolver.resolve(controlURL.host, controlURL.port, error);
	if (error) {
		return UPNP_E_SOCKET_CONNECT;
	}

	boost::asio::ip::tcp::endpoint remoteEndpoint;
	bool hasIPv6Endpoint = false;
	for (const auto &endpoint : endpoints) {
		if (endpoint.endpoint().address().is_v6()) {
			remoteEndpoint = endpoint.endpoint();
			hasIPv6Endpoint = true;
			break;
		}
	}
	if (!hasIPv6Endpoint) {
		return UPNP_E_SOCKET_CONNECT;
	}

	boost::beast::tcp_stream stream(context);
	stream.socket().open(boost::asio::ip::tcp::v6(), error);
	if (error) {
		return UPNP_E_SOCKET_ERROR;
	}
	stream.socket().bind(boost::asio::ip::tcp::endpoint(localAddress, 0), error);
	if (error) {
		return UPNP_E_SOCKET_BIND;
	}

	DOMString actionXML = ixmlPrintNode(reinterpret_cast<IXML_Node *>(actionDocument));
	if (actionXML == nullptr) {
		return UPNP_E_SOCKET_READ;
	}
	const std::string soapBody =
		std::string("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" ") +
		"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>" + actionXML +
		"</s:Body></s:Envelope>";
	ixmlFreeDOMString(actionXML);

	http::request<http::string_body> request(http::verb::post, controlURL.target, 11);
	request.set(http::field::host, controlURL.authority);
	request.set(http::field::content_type, "text/xml; charset=\"utf-8\"");
	request.set("SOAPAction", "\"" + serviceType + "#" + actionName + "\"");
	request.keep_alive(false);
	request.body() = soapBody;
	request.prepare_payload();

	boost::beast::flat_buffer buffer;
	http::response_parser<http::string_body> responseParser;
	responseParser.body_limit(1024 * 1024);
	http::response<http::string_body> response;
	boost::asio::steady_timer timeout(context);
	timeout.expires_after(std::chrono::seconds(30));
	int transportError = UPNP_E_SOCKET_CONNECT;
	timeout.async_wait([&](const boost::system::error_code &timerError) {
		if (!timerError) {
			boost::system::error_code ignored;
			stream.socket().cancel(ignored);
		}
	});
	stream.async_connect(remoteEndpoint, [&](const boost::system::error_code &connectError) {
		if (connectError) {
			timeout.cancel();
			return;
		}
		transportError = UPNP_E_SOCKET_WRITE;
		http::async_write(
			stream, request, [&](const boost::system::error_code &writeError, std::size_t) {
				if (writeError) {
					timeout.cancel();
					return;
				}
				transportError = UPNP_E_SOCKET_READ;
				http::async_read(stream,
					buffer,
					responseParser,
					[&](const boost::system::error_code &readError, std::size_t) {
						if (!readError) {
							response = responseParser.release();
							transportError = UPNP_E_SUCCESS;
						}
						timeout.cancel();
					});
			});
	});
	context.run();
	if (transportError != UPNP_E_SUCCESS) {
		return transportError;
	}

	const int status = static_cast<int>(response.result_int());
	if (!response.body().empty()) {
		const int parseError = ixmlParseBufferEx(response.body().c_str(), responseDocument);
		if (parseError != IXML_SUCCESS) {
			IXML::Document::Free(*responseDocument);
			*responseDocument = nullptr;
			return status == 200 ? UPNP_E_SOCKET_READ : status;
		}
	}
	if (status != 200) {
		// Hand back the UPnPError element alone, as UpnpSendAction() does: CUPnPError reads
		// errorCode from the root.
		IXML_Document *error = nullptr;
		IXML_NodeList *nodes = nullptr;
		if (*responseDocument != nullptr) {
			nodes = ixmlDocument_getElementsByTagName(*responseDocument, "UPnPError");
		}
		IXML_Node *node = nodes != nullptr ? ixmlNodeList_item(nodes, 0) : nullptr;
		DOMString errorXML = node != nullptr ? ixmlPrintNode(node) : nullptr;
		if (errorXML != nullptr && ixmlParseBufferEx(errorXML, &error) != IXML_SUCCESS) {
			error = nullptr;
		}
		ixmlFreeDOMString(errorXML);
		ixmlNodeList_free(nodes);
		IXML::Document::Free(*responseDocument);
		*responseDocument = error;
		return status;
	}
	return *responseDocument == nullptr ? UPNP_E_SOCKET_READ : UPNP_E_SUCCESS;
}

} /* namespace UPnP */

namespace IXML
{

/*! \brief Returns the root node of a given document. */
IXML_Element *Document::GetRootElement(IXML_Document *doc)
{
	return reinterpret_cast<IXML_Element *>(ixmlNode_getFirstChild(&doc->n));
}

/*!
 * \brief Frees the given document.
 *
 * \note Nodes extracted via any other interface function become invalid after this call unless
 * explicitly cloned.
 */
inline void Document::Free(IXML_Document *doc)
{
	ixmlDocument_free(doc);
}

namespace Element
{

/*! \brief Returns the first child of a given element. */
IXML_Element *GetFirstChild(IXML_Element *parent)
{
	return reinterpret_cast<IXML_Element *>(ixmlNode_getFirstChild(&parent->n));
}

/*! \brief Returns the next sibling of a given child. */
IXML_Element *GetNextSibling(IXML_Element *child)
{
	return reinterpret_cast<IXML_Element *>(ixmlNode_getNextSibling(&child->n));
}

/*! \brief Returns the element tag (name). */
const DOMString GetTag(IXML_Element *element)
{
	return ixmlNode_getNodeName(&element->n);
}

/*! \brief Returns the TEXT node value of the current node. */
const std::string GetTextValue(IXML_Element *element)
{
	if (!element) {
		return stdEmptyString;
	}
	IXML_Node *text = ixmlNode_getFirstChild(&element->n);
	const DOMString s = ixmlNode_getNodeValue(text);
	std::string ret;
	if (s) {
		ret = s;
	}

	return ret;
}

/*! \brief Returns the TEXT node value of the first child matching tag. */
const std::string GetChildValueByTag(IXML_Element *element, const DOMString tag)
{
	return GetTextValue(GetFirstChildByTag(element, tag));
}

/*! \brief Returns the first child element matching the requested tag, or NULL. */
IXML_Element *GetFirstChildByTag(IXML_Element *element, const DOMString tag)
{
	if (!element || !tag) {
		return NULL;
	}

	IXML_Node *child = ixmlNode_getFirstChild(&element->n);
	const DOMString childTag = ixmlNode_getNodeName(child);
	while (child && childTag && strcmp(tag, childTag)) {
		child = ixmlNode_getNextSibling(child);
		childTag = ixmlNode_getNodeName(child);
	}

	return reinterpret_cast<IXML_Element *>(child);
}

/*!
 * \brief Returns the next sibling element matching the requested tag. Use with the return value
 * of GetFirstChildByTag().
 */
IXML_Element *GetNextSiblingByTag(IXML_Element *element, const DOMString tag)
{
	if (!element || !tag) {
		return NULL;
	}

	IXML_Node *child = &element->n;
	const DOMString childTag = NULL;
	do {
		child = ixmlNode_getNextSibling(child);
		childTag = ixmlNode_getNodeName(child);
	} while (child && childTag && strcmp(tag, childTag));

	return reinterpret_cast<IXML_Element *>(child);
}

const std::string GetAttributeByTag(IXML_Element *element, const DOMString tag)
{
	IXML_NamedNodeMap *NamedNodeMap = ixmlNode_getAttributes(&element->n);
	IXML_Node *attribute = ixmlNamedNodeMap_getNamedItem(NamedNodeMap, tag);
	const DOMString s = ixmlNode_getNodeValue(attribute);
	std::string ret;
	if (s) {
		ret = s;
	}
	ixmlNamedNodeMap_free(NamedNodeMap);

	return ret;
}

} /* namespace Element */

} /* namespace IXML */

CUPnPError::CUPnPError(IXML_Document *errorDoc)
: m_root(IXML::Document::GetRootElement(errorDoc))
, m_ErrorCode(IXML::Element::GetChildValueByTag(m_root, "errorCode"))
, m_ErrorDescription(IXML::Element::GetChildValueByTag(m_root, "errorDescription"))
{
}

CUPnPArgument::CUPnPArgument(const CUPnPControlPoint &WXUNUSED(upnpControlPoint),
	IXML_Element *argument,
	const std::string &WXUNUSED(SCPDURL))
: m_name(IXML::Element::GetChildValueByTag(argument, "name"))
, m_direction(IXML::Element::GetChildValueByTag(argument, "direction"))
, m_retval(IXML::Element::GetFirstChildByTag(argument, "retval"))
, m_relatedStateVariable(IXML::Element::GetChildValueByTag(argument, "relatedStateVariable"))
{
	std::ostringstream msg;
	msg << "\n    Argument:" << "\n        name: " << m_name << "\n        direction: " << m_direction
	    << "\n        retval: " << m_retval
	    << "\n        relatedStateVariable: " << m_relatedStateVariable;
	AddDebugLogLineN(logUPnP, msg);
}

CUPnPAction::CUPnPAction(
	const CUPnPControlPoint &upnpControlPoint, IXML_Element *action, const std::string &SCPDURL)
: m_ArgumentList(upnpControlPoint, action, SCPDURL)
, m_name(IXML::Element::GetChildValueByTag(action, "name"))
{
	std::ostringstream msg;
	msg << "\n    Action:" << "\n        name: " << m_name;
	AddDebugLogLineN(logUPnP, msg);
}

CUPnPAllowedValue::CUPnPAllowedValue(const CUPnPControlPoint &WXUNUSED(upnpControlPoint),
	IXML_Element *allowedValue,
	const std::string &WXUNUSED(SCPDURL))
: m_allowedValue(IXML::Element::GetTextValue(allowedValue))
{
	std::ostringstream msg;
	msg << "\n    AllowedValue:" << "\n        allowedValue: " << m_allowedValue;
	AddDebugLogLineN(logUPnP, msg);
}

CUPnPStateVariable::CUPnPStateVariable(
	const CUPnPControlPoint &upnpControlPoint, IXML_Element *stateVariable, const std::string &SCPDURL)
: m_AllowedValueList(upnpControlPoint, stateVariable, SCPDURL)
, m_name(IXML::Element::GetChildValueByTag(stateVariable, "name"))
, m_dataType(IXML::Element::GetChildValueByTag(stateVariable, "dataType"))
, m_defaultValue(IXML::Element::GetChildValueByTag(stateVariable, "defaultValue"))
, m_sendEvents(IXML::Element::GetAttributeByTag(stateVariable, "sendEvents"))
{
	std::ostringstream msg;
	msg << "\n    StateVariable:" << "\n        name: " << m_name << "\n        dataType: " << m_dataType
	    << "\n        defaultValue: " << m_defaultValue << "\n        sendEvents: " << m_sendEvents;
	AddDebugLogLineN(logUPnP, msg);
}

CUPnPSCPD::CUPnPSCPD(
	const CUPnPControlPoint &upnpControlPoint, IXML_Element *scpd, const std::string &SCPDURL)
: m_ActionList(upnpControlPoint, scpd, SCPDURL)
, m_ServiceStateTable(upnpControlPoint, scpd, SCPDURL)
, m_SCPDURL(SCPDURL)
{
}

CUPnPArgumentValue::CUPnPArgumentValue()
: m_argument()
, m_value()
{
}

CUPnPArgumentValue::CUPnPArgumentValue(const std::string &argument, const std::string &value)
: m_argument(argument)
, m_value(value)
{
}

CUPnPService::CUPnPService(
	const CUPnPControlPoint &upnpControlPoint, IXML_Element *service, const std::string &URLBase)
: m_UPnPControlPoint(upnpControlPoint)
, m_serviceType(IXML::Element::GetChildValueByTag(service, "serviceType"))
, m_serviceId(IXML::Element::GetChildValueByTag(service, "serviceId"))
, m_SCPDURL(IXML::Element::GetChildValueByTag(service, "SCPDURL"))
, m_controlURL(IXML::Element::GetChildValueByTag(service, "controlURL"))
, m_eventSubURL(IXML::Element::GetChildValueByTag(service, "eventSubURL"))
, m_timeout(1801)
, m_SCPD(nullptr)
{
	std::ostringstream msg;
	int errcode;

	memset(m_SID, 0, sizeof(Upnp_SID));

	errcode = UPnP::ResolveURL(URLBase, m_SCPDURL, m_absSCPDURL);
	if (errcode != UPNP_E_SUCCESS) {
		msg << "Error generating scpdURL from " << "|" << URLBase << "|" << m_SCPDURL << "|.";
		AddDebugLogLineN(logUPnP, msg);
	}

	errcode = UPnP::ResolveURL(URLBase, m_controlURL, m_absControlURL);
	if (errcode != UPNP_E_SUCCESS) {
		msg << "Error generating controlURL from " << "|" << URLBase << "|" << m_controlURL << "|.";
		AddDebugLogLineN(logUPnP, msg);
	}

	errcode = UPnP::ResolveURL(URLBase, m_eventSubURL, m_absEventSubURL);
	if (errcode != UPNP_E_SUCCESS) {
		msg << "Error generating eventURL from " << "|" << URLBase << "|" << m_eventSubURL << "|.";
		AddDebugLogLineN(logUPnP, msg);
	}

	msg << "\n    Service:" << "\n        serviceType: " << m_serviceType
	    << "\n        serviceId: " << m_serviceId << "\n        SCPDURL: " << m_SCPDURL
	    << "\n        absSCPDURL: " << m_absSCPDURL << "\n        controlURL: " << m_controlURL
	    << "\n        absControlURL: " << m_absControlURL << "\n        eventSubURL: " << m_eventSubURL
	    << "\n        absEventSubURL: " << m_absEventSubURL;
	AddDebugLogLineN(logUPnP, msg);

	// Keep WAN mappings on IPv4. Register the IPv6 firewall service from both families so a gateway
	// that refuses IPv6 pinhole actions can use its IPv4 control URL as a fallback.
	const bool isIPv6ControlURL = UPnP::IsIPv6Location(m_absControlURL);
	const bool isIPv6Firewall = UPnP::IsIPv6FirewallService(m_serviceType);
	const bool isWANMapping = UPnP::IsWANMappingService(m_serviceType);
	if ((isIPv6ControlURL && isIPv6Firewall) || (!isIPv6ControlURL && (isIPv6Firewall || isWANMapping))) {
#if 0
	    m_serviceType == UPnP::Service::WAN_PPP_Connection ||
	    m_serviceType == UPnP::Service::WAN_Common_Interface_Config ||
	    m_serviceType == UPnP::Service::Layer3_Forwarding) {
#endif
#if 0
		if (!upnpControlPoint.WanServiceDetected()) {
			// This condition can be used to suspend the parse
			// of the XML tree.
#endif
		msg.str("");
		msg << "WAN Service Detected: '" << m_serviceType << "'.";
		AddDebugLogLineN(logUPnP, msg);
		const_cast<CUPnPControlPoint &>(upnpControlPoint).Subscribe(*this);
#if 0
		} else {
			msg.str("");
			msg << "WAN service detected again: '" <<
				m_serviceType <<
				"'. Will only use the first instance.";
			AddDebugLogLineC(logUPnP, msg);
		}
#endif
	} else {
		msg.str("");
		msg << "Uninteresting service detected: '" << m_serviceType << "'. Ignoring.";
		AddDebugLogLineN(logUPnP, msg);
	}
}

CUPnPService::~CUPnPService()
{
	const_cast<CUPnPControlPoint &>(m_UPnPControlPoint).ForgetService(this);
}

bool CUPnPService::Execute(const std::string &ActionName,
	const std::vector<CUPnPArgumentValue> &ArgValue,
	const char *resultArg,
	std::string *result,
	const std::string *sourceAddress) const
{
	std::ostringstream msg;
	if (m_SCPD.get() == nullptr) {
		msg << "Service without SCPD Document, cannot execute action '" << ActionName
		    << "' for service '" << GetServiceType() << "'.";
		AddDebugLogLineN(logUPnP, msg);
		return false;
	}
	std::ostringstream msgAction("Sending action ");
	ActionList::const_iterator itAction = m_SCPD->GetActionList().find(ActionName);
	if (itAction == m_SCPD->GetActionList().end()) {
		msg << "Invalid action name '" << ActionName << "' for service '" << GetServiceType() << "'.";
		AddDebugLogLineN(logUPnP, msg);
		return false;
	}
	msgAction << ActionName << "(";
	bool firstTime = true;
	const CUPnPAction &action = *(itAction->second);
	for (unsigned int i = 0; i < ArgValue.size(); ++i) {
		ArgumentList::const_iterator itArg = action.GetArgumentList().find(ArgValue[i].GetArgument());
		if (itArg == action.GetArgumentList().end()) {
			msg << "Invalid argument name '" << ArgValue[i].GetArgument() << "' for action '"
			    << action.GetName() << "' for service '" << GetServiceType() << "'.";
			AddDebugLogLineN(logUPnP, msg);
			return false;
		}
		const CUPnPArgument &argument = *(itArg->second);
		// Direction is "in" or "out" -- ASCII-only. Pin LC_CTYPE = C so
		// tolower() doesn't fold 'I' to U+0131 under tr_TR.
		CCtypeAsciiScope direction_scope;
		if (tolower(argument.GetDirection()[0]) != 'i' ||
			tolower(argument.GetDirection()[1]) != 'n') {
			msg << "Invalid direction for argument '" << ArgValue[i].GetArgument()
			    << "' for action '" << action.GetName() << "' for service '" << GetServiceType()
			    << "'.";
			AddDebugLogLineN(logUPnP, msg);
			return false;
		}
		const std::string relatedStateVariableName = argument.GetRelatedStateVariable();
		if (!relatedStateVariableName.empty()) {
			ServiceStateTable::const_iterator itSVT =
				m_SCPD->GetServiceStateTable().find(relatedStateVariableName);
			if (itSVT == m_SCPD->GetServiceStateTable().end()) {
				msg << "Inconsistent Service State Table, did not find '"
				    << relatedStateVariableName << "' for argument '" << argument.GetName()
				    << "' for action '" << action.GetName() << "' for service '"
				    << GetServiceType() << "'.";
				AddDebugLogLineN(logUPnP, msg);
				return false;
			}
			const CUPnPStateVariable &stateVariable = *(itSVT->second);
			if (!stateVariable.GetAllowedValueList().empty() &&
				stateVariable.GetAllowedValueList().find(ArgValue[i].GetValue()) ==
					stateVariable.GetAllowedValueList().end()) {
				msg << "Value not allowed '" << ArgValue[i].GetValue()
				    << "' for state variable '" << relatedStateVariableName
				    << "' for argument '" << argument.GetName() << "' for action '"
				    << action.GetName() << "' for service '" << GetServiceType() << "'.";
				AddDebugLogLineN(logUPnP, msg);
				return false;
			}
		}
		if (firstTime) {
			firstTime = false;
		} else {
			msgAction << ", ";
		}
		msgAction << ArgValue[i].GetArgument() << "='" << ArgValue[i].GetValue() << "'";
	}
	msgAction << ")";
	AddDebugLogLineN(logUPnP, msgAction);
	IXML_Document *ActionDoc = NULL;
	if (!ArgValue.empty()) {
		for (unsigned int i = 0; i < ArgValue.size(); ++i) {
			int ret = UpnpAddToAction(&ActionDoc,
				action.GetName().c_str(),
				GetServiceType().c_str(),
				ArgValue[i].GetArgument().c_str(),
				ArgValue[i].GetValue().c_str());
			if (ret != UPNP_E_SUCCESS) {
				UPnP::ProcessErrorMessage("UpnpAddToAction", ret, NULL, NULL);
				return false;
			}
		}
	} else {
		ActionDoc = UpnpMakeAction(action.GetName().c_str(), GetServiceType().c_str(), 0, NULL);
		if (!ActionDoc) {
			msg << "Error: UpnpMakeAction returned NULL.";
			AddDebugLogLineN(logUPnP, msg);
			return false;
		}
	}
#if 0
	UpnpSendActionAsync(
		m_UPnPControlPoint.GetUPnPClientHandle(),
		GetAbsControlURL().c_str(),
		GetServiceType().c_str(),
		NULL, ActionDoc,
		static_cast<Upnp_FunPtr>(&CUPnPControlPoint::Callback),
		NULL);
	return true;
#endif

	IXML_Document *RespDoc = NULL;
	const int ret = sourceAddress == nullptr ? UpnpSendAction(m_UPnPControlPoint.GetUPnPClientHandle(),
							   GetAbsControlURL().c_str(),
							   GetServiceType().c_str(),
							   nullptr,
							   ActionDoc,
							   &RespDoc)
						 : UPnP::SendBoundSoapAction(GetAbsControlURL(),
							   *sourceAddress,
							   GetServiceType(),
							   ActionName,
							   ActionDoc,
							   &RespDoc);
	if (ret != UPNP_E_SUCCESS) {
		UPnP::ProcessErrorMessage(sourceAddress == nullptr ? "UpnpSendAction" : "Bound SOAP action",
			ret,
			nullptr,
			RespDoc);
		IXML::Document::Free(ActionDoc);
		IXML::Document::Free(RespDoc);
		return false;
	}
	IXML::Document::Free(ActionDoc);

	UPnP::ProcessActionResponse(RespDoc, action.GetName());

	if (resultArg != nullptr && result != nullptr) {
		// SOAP response, so the value is found by tag name rather than by position: gateways
		// order their output arguments as they please.
		IXML_NodeList *nodes = ixmlDocument_getElementsByTagName(RespDoc, resultArg);
		if (nodes != nullptr) {
			IXML_Node *node = ixmlNodeList_item(nodes, 0);
			IXML_Node *text = (node != nullptr) ? ixmlNode_getFirstChild(node) : nullptr;
			const char *value = (text != nullptr) ? ixmlNode_getNodeValue(text) : nullptr;
			if (value != nullptr) {
				*result = value;
			}
			ixmlNodeList_free(nodes);
		}
	}

	IXML::Document::Free(RespDoc);

	return true;
}

CUPnPDevice::CUPnPDevice(
	const CUPnPControlPoint &upnpControlPoint, IXML_Element *device, const std::string &URLBase)
: m_DeviceList(upnpControlPoint, device, URLBase)
, m_ServiceList(upnpControlPoint, device, URLBase)
, m_deviceType(IXML::Element::GetChildValueByTag(device, "deviceType"))
, m_friendlyName(IXML::Element::GetChildValueByTag(device, "friendlyName"))
, m_manufacturer(IXML::Element::GetChildValueByTag(device, "manufacturer"))
, m_manufacturerURL(IXML::Element::GetChildValueByTag(device, "manufacturerURL"))
, m_modelDescription(IXML::Element::GetChildValueByTag(device, "modelDescription"))
, m_modelName(IXML::Element::GetChildValueByTag(device, "modelName"))
, m_modelNumber(IXML::Element::GetChildValueByTag(device, "modelNumber"))
, m_modelURL(IXML::Element::GetChildValueByTag(device, "modelURL"))
, m_serialNumber(IXML::Element::GetChildValueByTag(device, "serialNumber"))
, m_UDN(IXML::Element::GetChildValueByTag(device, "UDN"))
, m_UPC(IXML::Element::GetChildValueByTag(device, "UPC"))
, m_presentationURL(IXML::Element::GetChildValueByTag(device, "presentationURL"))
{
	std::ostringstream msg;
	std::string presURL;
	int errcode = UPnP::ResolveURL(URLBase, m_presentationURL, presURL);
	if (errcode != UPNP_E_SUCCESS) {
		msg << "Error generating presentationURL from " << "|" << URLBase << "|" << m_presentationURL
		    << "|.";
		AddDebugLogLineN(logUPnP, msg);
	} else {
		m_presentationURL = presURL;
	}

	msg.str("");
	msg << "\n    Device: " << "\n        friendlyName: " << m_friendlyName
	    << "\n        deviceType: " << m_deviceType << "\n        manufacturer: " << m_manufacturer
	    << "\n        manufacturerURL: " << m_manufacturerURL
	    << "\n        modelDescription: " << m_modelDescription << "\n        modelName: " << m_modelName
	    << "\n        modelNumber: " << m_modelNumber << "\n        modelURL: " << m_modelURL
	    << "\n        serialNumber: " << m_serialNumber << "\n        UDN: " << m_UDN
	    << "\n        UPC: " << m_UPC << "\n        presentationURL: " << m_presentationURL;
	AddDebugLogLineN(logUPnP, msg);
}

CUPnPRootDevice::CUPnPRootDevice(const CUPnPControlPoint &upnpControlPoint,
	IXML_Element *rootDevice,
	const std::string &OriginalURLBase,
	const std::string &FixedURLBase,
	const char *location,
	int expires)
: CUPnPDevice(upnpControlPoint, rootDevice, FixedURLBase)
, m_URLBase(OriginalURLBase)
, m_location(location)
, m_expires(expires)
{
	std::ostringstream msg;
	msg << "\n    Root Device: " << "\n        URLBase: " << m_URLBase
	    << "\n        Fixed URLBase: " << FixedURLBase << "\n        location: " << m_location
	    << "\n        expires: " << m_expires;
	AddDebugLogLineN(logUPnP, msg);
}

CUPnPControlPoint *CUPnPControlPoint::s_CtrlPoint = NULL;

static const int SEARCH_MX_SECONDS = 3;
// libupnp's timeout for one request.
static const int REGISTRATION_GRACE_SECONDS = 30;

CUPnPControlPoint::CUPnPControlPoint(unsigned short udpPort)
: m_UPnPClientHandle()
, m_RootDeviceMap()
, m_ServiceMap()
, m_ActivePortMappingsMap()
, m_RootDeviceListMutex()
, m_IGWDeviceDetected(false)
, m_ServiceMapMutex()
, m_gatewayMapped(false)
, m_registering(0)
{
	s_CtrlPoint = this;
	std::ostringstream msg;

	// Declare those here to avoid
	// "jump to label ‘error’ [-fpermissive] crosses initialization
	// of ‘char* ipAddress’"
	unsigned short port;
	char *ipAddress;

	int ret;
	ret = UpnpInit2(0, udpPort);
	if (ret != UPNP_E_SUCCESS) {
		msg << "error(UpnpInit2): Error code ";
		goto error;
	}
	port = UpnpGetServerPort();
	ipAddress = UpnpGetServerIpAddress();
	msg << "bound to " << ipAddress << ":" << port << ".";
	AddDebugLogLineN(logUPnP, msg);
	msg.str("");

	// Raise the SDK's incoming content-length ceiling above libupnp's 16 KB default. Some
	// gateways serve an SCPD/description XML slightly larger than that, which makes the
	// blocking UpnpDownloadXmlDoc in Subscribe() fail with UPNP_E_OUTOF_BOUNDS -- the service
	// never registers, no port mapping happens, and the user is stuck on a LowID (seen on ZTE
	// and Sagemcom routers). 1 MB clears any real router descriptor while still bounding what a
	// hostile LAN device can push into a blocking fetch. Must run after UpnpInit2 succeeds: the
	// call is a no-op until the SDK is initialised.
	ret = UpnpSetMaxContentLength(1024 * 1024);
	if (ret != UPNP_E_SUCCESS) {
		msg << "warning(UpnpSetMaxContentLength): could not raise content-length limit, error code "
		    << ret << "; keeping libupnp default.";
		AddDebugLogLineN(logUPnP, msg);
		msg.str("");
	}

	ret = UpnpRegisterClient(static_cast<Upnp_FunPtr>(&CUPnPControlPoint::Callback),
		&m_UPnPClientHandle,
		&m_UPnPClientHandle);
	if (ret != UPNP_E_SUCCESS) {
		msg << "error(UpnpRegisterClient): Error registering callback: ";
		goto error;
	}

	// Search for the InternetGatewayDevice specifically rather than upnp:rootdevice. Searching
	// rootdevice makes every UPnP speaker on the LAN answer the M-SEARCH, and our SEARCH_RESULT
	// handler then fetches description.xml from each one synchronously on a libupnp worker
	// thread; on a busy LAN that saturates the mini-server thread pool until ThreadPoolAdd hits
	// its cap and drops jobs. Targeting the IGW type means only gateways answer, which is all
	// port mapping needs, and a gateway that appears later is still caught by its periodic IGW-
	// typed ADVERTISEMENT_ALIVE.
	m_searchDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(SEARCH_MX_SECONDS + 1);
	ret = UpnpSearchAsync(m_UPnPClientHandle, SEARCH_MX_SECONDS, UPnP::Device::IGW.c_str(), nullptr);
	if (ret != UPNP_E_SUCCESS) {
		msg << "error(UpnpSearchAsync): Error sending search request: ";
		goto error;
	}

	// No wait here: Subscribe() maps a service whenever it registers, and DiscoveryPending() tells
	// callers when that has happened.
	return;

error:
	UpnpFinish();
	msg << ret << ": " << UpnpGetErrorMessage(ret) << ".";
	throw CUPnPException(msg);
}

CUPnPControlPoint::~CUPnPControlPoint()
{
	// Stop libupnp first: UpnpFinish() waits for discovery callbacks still in flight, which
	// would otherwise reach the devices deleted below.
	UpnpUnRegisterClient(m_UPnPClientHandle);
	UpnpFinish();

	CUPnPMutexLocker lock(m_RootDeviceListMutex);
	for (auto &entry : m_RootDeviceMap) {
		delete entry.second;
	}
	m_RootDeviceMap.clear();
}

bool CUPnPControlPoint::DiscoveryPending() const
{
	if (m_gatewayMapped) {
		return false;
	}
	// Not UPNP_DISCOVERY_SEARCH_TIMEOUT: it does not wait for answers still being processed, and
	// libupnp drops it when its thread pool is full.
	const auto now = std::chrono::steady_clock::now();
	return now < m_searchDeadline ||
	       (m_registering > 0 &&
		       now < m_searchDeadline + std::chrono::seconds(REGISTRATION_GRACE_SECONDS));
}

bool CUPnPControlPoint::HasWANMappingService() const
{
	for (const auto &entry : m_ServiceMap) {
		if (UPnP::IsWANMappingService(entry.second->GetServiceType())) {
			return true;
		}
	}
	return false;
}

bool CUPnPControlPoint::AddPortMappings(const std::vector<CUPnPPortMapping> &upnpPortMapping)
{
	CUPnPMutexLocker lock(m_ServiceMapMutex);
	std::ostringstream msg;
	const bool hasWANMappingService = HasWANMappingService();
	if (!hasWANMappingService) {
		msg << "CUPnPControlPoint::AddPortMappings: "
		       "WAN Service not detected yet. Will map when one registers.";
		AddDebugLogLineC(logUPnP, msg);
	}

	bool ok = hasWANMappingService;
	for (const auto &mapping : upnpPortMapping) {
		if (mapping.getEnabled() == "1") {
			m_ActivePortMappingsMap[mapping.getKey()] = mapping;
			// Send every mapping, even after one fails.
			ok = PrivateAddPortMapping(mapping) && ok;
		}
	}

	msg.str("");
	msg << "CUPnPControlPoint::AddPortMappings: "
	       "m_ActivePortMappingsMap.size() == "
	    << m_ActivePortMappingsMap.size();
	AddDebugLogLineN(logUPnP, msg);

	return ok;
}

void CUPnPControlPoint::RefreshPortMappings()
{
	for (PortMappingMap::iterator it = m_ActivePortMappingsMap.begin();
		it != m_ActivePortMappingsMap.end();
		++it) {
		PrivateAddPortMapping(it->second);
	}
}

bool CUPnPControlPoint::ExecuteOnAnyService(
	const std::string &actionName, const std::vector<CUPnPArgumentValue> &argval)
{
	// Any WAN service on any gateway accepting counts as success, except the IPv6 firewall
	// service, whose SCPD does not implement IPv4 port-mapping actions.
	bool ok = false;
	for (const auto &entry : m_ServiceMap) {
		if (UPnP::IsIPv6FirewallService(entry.second->GetServiceType())) {
			continue;
		}
		ok = entry.second->Execute(actionName, argval) || ok;
	}
	return ok;
}

//! SOAP arguments are strings, and this file builds its own without a formatting library.
template <typename T> static std::string Numeric(T value)
{
	std::ostringstream text;
	text << value;
	return text.str();
}

std::string CUPnPControlPoint::PinholeKey(int protocol, unsigned short port)
{
	std::ostringstream key;
	key << protocol << ":" << static_cast<unsigned int>(port);
	return key.str();
}

bool CUPnPControlPoint::IPv6FirewallDetected() const
{
	CUPnPMutexLocker lock(m_ServiceMapMutex);
	for (const auto &entry : m_ServiceMap) {
		if (UPnP::IsIPv6FirewallService(entry.second->GetServiceType())) {
			return true;
		}
	}
	return false;
}

bool CUPnPControlPoint::ExecuteOnIPv6FirewallService(const std::string &actionName,
	const std::vector<CUPnPArgumentValue> &argval,
	const std::string &sourceAddress,
	const char *resultArg,
	std::string *result)
{
	for (int family = 0; family < 2; ++family) {
		for (const auto &entry : m_ServiceMap) {
			if (!UPnP::IsIPv6FirewallService(entry.second->GetServiceType())) {
				continue;
			}
			const bool isIPv6ControlURL = UPnP::IsIPv6Location(entry.second->GetAbsControlURL());
			if (isIPv6ControlURL != (family == 0)) {
				continue;
			}
			std::string value;
			const std::string *boundSource = isIPv6ControlURL ? &sourceAddress : nullptr;
			if (entry.second->Execute(actionName, argval, resultArg, &value, boundSource)) {
				if (result != nullptr) {
					*result = value;
				}
				return true;
			}
		}
	}
	return false;
}

bool CUPnPControlPoint::SendAddPinhole(SIPv6Pinhole &pinhole)
{
	std::vector<CUPnPArgumentValue> argval(6);
	argval[0].SetArgument("RemoteHost");
	argval[0].SetValue("");
	argval[1].SetArgument("RemotePort");
	argval[1].SetValue("0");
	argval[2].SetArgument("InternalClient");
	argval[2].SetValue(pinhole.client);
	argval[3].SetArgument("InternalPort");
	argval[3].SetValue(Numeric(pinhole.port));
	argval[4].SetArgument("Protocol");
	argval[4].SetValue(Numeric(pinhole.protocol));
	argval[5].SetArgument("LeaseTime");
	argval[5].SetValue(Numeric(pinhole.lease));

	std::string uniqueId;
	if (!ExecuteOnIPv6FirewallService("AddPinhole", argval, pinhole.client, "UniqueID", &uniqueId) ||
		uniqueId.empty()) {
		pinhole.uniqueId.clear();
		const auto retryDelay = std::chrono::seconds(pinhole.lease / 2);
		pinhole.expiry = std::chrono::steady_clock::now() + retryDelay;
		return false;
	}
	pinhole.uniqueId = uniqueId;
	++m_IPv6PinholeOpenings;
	m_IPv6PinholeHeld = true;
	pinhole.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(pinhole.lease);
	std::ostringstream msg;
	msg << "IPv6 pinhole for " << pinhole.client << " (protocol " << pinhole.protocol
	    << ") got unique id " << uniqueId;
	AddDebugLogLineN(logUPnP, msg);
	return true;
}

bool CUPnPControlPoint::SendUpdatePinhole(SIPv6Pinhole &pinhole)
{
	// IGD2 names this argument NewLeaseTime: an argument the SCPD does not list is refused before
	// it ever reaches the gateway.
	std::vector<CUPnPArgumentValue> argval(2);
	argval[0].SetArgument("UniqueID");
	argval[0].SetValue(pinhole.uniqueId);
	argval[1].SetArgument("NewLeaseTime");
	argval[1].SetValue(Numeric(pinhole.lease));

	if (!ExecuteOnIPv6FirewallService("UpdatePinhole", argval, pinhole.client, nullptr, nullptr)) {
		return false;
	}
	pinhole.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(pinhole.lease);
	return true;
}

void CUPnPControlPoint::SendDeletePinhole(const SIPv6Pinhole &pinhole)
{
	if (pinhole.uniqueId.empty()) {
		return;
	}
	std::vector<CUPnPArgumentValue> argval(1);
	argval[0].SetArgument("UniqueID");
	argval[0].SetValue(pinhole.uniqueId);
	ExecuteOnIPv6FirewallService("DeletePinhole", argval, pinhole.client, nullptr, nullptr);
}

void CUPnPControlPoint::AddIPv6Pinhole(const std::string &internalClient,
	unsigned short internalPort,
	int protocol,
	unsigned int leaseSeconds)
{
	CUPnPMutexLocker lock(m_ServiceMapMutex);
	const std::string key = PinholeKey(protocol, internalPort);
	SIPv6Pinhole &pinhole = m_IPv6Pinholes[key];
	// Recorded before it is sent: if the firewall service has not registered yet, this entry is
	// what the registration path replays.
	pinhole.client = internalClient;
	pinhole.port = internalPort;
	pinhole.protocol = protocol;
	pinhole.lease = leaseSeconds;
	SendAddPinhole(pinhole);
}

void CUPnPControlPoint::SetIPv6PinholeTarget(
	const std::string &client, unsigned short port, unsigned int leaseSeconds)
{
	CUPnPMutexLocker lock(m_IPv6PinholeTargetMutex);
	m_IPv6PinholeTarget = SIPv6PinholeTarget{ client, port, leaseSeconds, false };
}

void CUPnPControlPoint::ReleaseIPv6Pinholes()
{
	CUPnPMutexLocker lock(m_IPv6PinholeTargetMutex);
	m_IPv6PinholeTarget = SIPv6PinholeTarget{ std::string(), 0, 0, true };
}

void CUPnPControlPoint::NoteIPv6PinholesHeld()
{
	bool held = false;
	for (const auto &entry : m_IPv6Pinholes) {
		held = held || !entry.second.uniqueId.empty();
	}
	m_IPv6PinholeHeld = held;
}

void CUPnPControlPoint::ApplyIPv6PinholeTarget(const SIPv6PinholeTarget &target)
{
	const int tcp = 6;
	if (target.release) {
		// A handover, not a close: on a gateway that keeps one entry for both protocols, a
		// DeletePinhole would remove the PCP pinhole too. A separate IGD2 entry ends with its lease.
		if (!m_IPv6Pinholes.empty()) {
			AddDebugLogLineN(logUPnP, "IPv6 pinhole handed over to PCP without DeletePinhole");
		}
		m_IPv6Pinholes.clear();
		NoteIPv6PinholesHeld();
		return;
	}
	const std::string wanted = target.client.empty() ? std::string() : PinholeKey(tcp, target.port);
	for (auto it = m_IPv6Pinholes.begin(); it != m_IPv6Pinholes.end();) {
		if (it->first == wanted) {
			++it;
		} else {
			SendDeletePinhole(it->second);
			it = m_IPv6Pinholes.erase(it);
		}
	}
	if (!wanted.empty() && m_IPv6Pinholes.find(wanted) == m_IPv6Pinholes.end()) {
		SIPv6Pinhole &pinhole = m_IPv6Pinholes[wanted];
		pinhole.client = target.client;
		pinhole.port = target.port;
		pinhole.protocol = tcp;
		pinhole.lease = target.lease;
		SendAddPinhole(pinhole);
	}
}

void CUPnPControlPoint::DeleteIPv6Pinholes()
{
	{
		CUPnPMutexLocker targetLock(m_IPv6PinholeTargetMutex);
		m_IPv6PinholeTarget.reset();
	}
	CUPnPMutexLocker lock(m_ServiceMapMutex);
	for (auto &entry : m_IPv6Pinholes) {
		SendDeletePinhole(entry.second);
	}
	// Forgotten whether or not the gateway acknowledged: the leases expire by themselves, and a
	// kept id would only let a later renewal claim a lease that no longer exists.
	m_IPv6Pinholes.clear();
	m_IPv6PinholeHeld = false;
}

void CUPnPControlPoint::RenewExpiringIPv6Pinholes(const std::string &currentClient)
{
	CUPnPMutexLocker lock(m_ServiceMapMutex);
	std::optional<SIPv6PinholeTarget> target;
	{
		// Read under the service map lock: DeleteIPv6Pinholes() clears the target before it takes
		// that lock, so a pass cannot reopen what it deletes.
		CUPnPMutexLocker targetLock(m_IPv6PinholeTargetMutex);
		target = m_IPv6PinholeTarget;
	}
	if (target.has_value()) {
		ApplyIPv6PinholeTarget(*target);
	}
	// The target is newer than the address this pass was queued with.
	const std::string &client = target.has_value() ? target->client : currentClient;
	const auto now = std::chrono::steady_clock::now();
	for (auto &entry : m_IPv6Pinholes) {
		SIPv6Pinhole &pinhole = entry.second;
		// A prefix change leaves the pinhole naming an address we no longer hold: replace it
		// rather than renew it.
		if (!client.empty() && IsIPv6PinholeStale(client, pinhole.client)) {
			SendDeletePinhole(pinhole);
			pinhole.uniqueId.clear();
			pinhole.client = client;
			SendAddPinhole(pinhole);
			continue;
		}
		if (pinhole.uniqueId.empty()) {
			if (now >= pinhole.expiry) {
				SendAddPinhole(pinhole);
			}
			continue;
		}
		const auto left = std::chrono::duration_cast<std::chrono::seconds>(pinhole.expiry - now);
		// Half the granted lease is the margin: room to retry on the next tick if this attempt
		// fails, without renewing so eagerly that a gateway sees a flood of updates.
		const unsigned int margin = pinhole.lease / 2;
		const unsigned int remaining =
			(left.count() > 0) ? static_cast<unsigned int>(left.count()) : 0;
		if (!ShouldRenewIPv6Pinhole(remaining, margin)) {
			continue;
		}
		// A renewal that the gateway cannot grant - a rebooted router has forgotten the UniqueID -
		// falls back to asking again, instead of retrying an update forever.
		if (!SendUpdatePinhole(pinhole)) {
			pinhole.uniqueId.clear();
			SendAddPinhole(pinhole);
		}
	}
}

static std::vector<CUPnPArgumentValue> AddPortMappingArguments(const CUPnPPortMapping &upnpPortMapping)
{
	std::vector<CUPnPArgumentValue> argval(8);

	argval[0].SetArgument("NewRemoteHost");
	argval[0].SetValue("");
	argval[1].SetArgument("NewExternalPort");
	argval[1].SetValue(upnpPortMapping.getPort());
	argval[2].SetArgument("NewProtocol");
	argval[2].SetValue(upnpPortMapping.getProtocol());
	argval[3].SetArgument("NewInternalPort");
	argval[3].SetValue(upnpPortMapping.getPort());
	argval[4].SetArgument("NewInternalClient");
	argval[4].SetValue(UpnpGetServerIpAddress());
	argval[5].SetArgument("NewEnabled");
	argval[5].SetValue("1");
	argval[6].SetArgument("NewPortMappingDescription");
	argval[6].SetValue(upnpPortMapping.getDescription());
	argval[7].SetArgument("NewLeaseDuration");
	argval[7].SetValue("0");

	return argval;
}

bool CUPnPControlPoint::PrivateAddPortMapping(const CUPnPPortMapping &upnpPortMapping)
{
	return ExecuteOnAnyService("AddPortMapping", AddPortMappingArguments(upnpPortMapping));
}

bool CUPnPControlPoint::DeletePortMappings(const std::vector<CUPnPPortMapping> &upnpPortMapping)
{
	CUPnPMutexLocker lock(m_ServiceMapMutex);
	std::ostringstream msg;
	bool ok = HasWANMappingService();
	for (const auto &mapping : upnpPortMapping) {
		if (mapping.getEnabled() == "1") {
			PortMappingMap::iterator it = m_ActivePortMappingsMap.find(mapping.getKey());
			if (it != m_ActivePortMappingsMap.end()) {
				m_ActivePortMappingsMap.erase(it);
			} else {
				msg << "UPnP Error: "
				       "CUPnPControlPoint::DeletePortMapping: "
				       "Mapping was not found in the active "
				       "mapping map.";
				AddDebugLogLineC(logUPnP, msg);
			}

			ok = PrivateDeletePortMapping(mapping) && ok;
		}
	}

	msg.str("");
	msg << "CUPnPControlPoint::DeletePortMappings: "
	       "m_ActivePortMappingsMap.size() == "
	    << m_ActivePortMappingsMap.size();
	AddDebugLogLineN(logUPnP, msg);

	return ok;
}

bool CUPnPControlPoint::PrivateDeletePortMapping(const CUPnPPortMapping &upnpPortMapping)
{
	std::string actionName("DeletePortMapping");
	std::vector<CUPnPArgumentValue> argval(3);

	argval[0].SetArgument("NewRemoteHost");
	argval[0].SetValue("");
	argval[1].SetArgument("NewExternalPort");
	argval[1].SetValue(upnpPortMapping.getPort());
	argval[2].SetArgument("NewProtocol");
	argval[2].SetValue(upnpPortMapping.getProtocol());

	return ExecuteOnAnyService(actionName, argval);
}

#if UPNP_VERSION >= 11800
int CUPnPControlPoint::Callback(Upnp_EventType_e EventType, void *Event, void * /*Cookie*/)
#elif UPNP_VERSION >= 11430
int CUPnPControlPoint::Callback(Upnp_EventType_e EventType, const void *Event, void * /*Cookie*/)
#elif UPNP_VERSION >= 11426
int CUPnPControlPoint::Callback(Upnp_EventType_e EventType, void *Event, void * /*Cookie*/)
#elif UPNP_VERSION >= 10800
int CUPnPControlPoint::Callback(Upnp_EventType_e EventType, const void *Event, void * /*Cookie*/)
#else
int CUPnPControlPoint::Callback(Upnp_EventType EventType, void *Event, void * /*Cookie*/)
#endif
{
	std::ostringstream msg;
	std::ostringstream msg2;
	// Somehow, this is unreliable: UPNP_DISCOVERY_ADVERTISEMENT_ALIVE events
	// happen with a wrong cookie, so the cookie cannot be trusted here.
	CUPnPControlPoint *upnpCP = CUPnPControlPoint::s_CtrlPoint;

	switch (EventType) {
	case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE: {
		// Drop unsolicited NOTIFY announcements whose NT is not a device or service we care
		// about. Without this filter amule fetches description.xml from every UPnP speaker
		// on the LAN, which burns libupnp's ThreadPool budget and produces "Error
		// retrieving device description" noise for endpoints amule has no business poking.
#if UPNP_VERSION >= 10800
		UpnpDiscovery *d_filter_event = (UpnpDiscovery *)Event;
		const char *deviceType = UpnpDiscovery_get_DeviceType_cstr(d_filter_event);
#else
		struct Upnp_Discovery *d_filter_event = (struct Upnp_Discovery *)Event;
		const char *deviceType = d_filter_event->DeviceType;
#endif
		if (!UPnP::IsWANRelatedDeviceType(deviceType ? deviceType : "")) {
			break;
		}
		msg << "error(UPNP_DISCOVERY_ADVERTISEMENT_ALIVE): ";
		msg2 << "UPNP_DISCOVERY_ADVERTISEMENT_ALIVE: ";
		goto upnpDiscovery;
	}
	case UPNP_DISCOVERY_SEARCH_RESULT: {
		msg << "error(UPNP_DISCOVERY_SEARCH_RESULT): ";
		msg2 << "UPNP_DISCOVERY_SEARCH_RESULT: ";
	upnpDiscovery:
#if UPNP_VERSION >= 10800
		UpnpDiscovery *d_event = (UpnpDiscovery *)Event;
#else
		struct Upnp_Discovery *d_event = (struct Upnp_Discovery *)Event;
#endif
		IXML_Document *doc = NULL;
#if UPNP_VERSION >= 10800
		int errCode = UpnpDiscovery_get_ErrCode(d_event);
		if (errCode != UPNP_E_SUCCESS) {
			msg << UpnpGetErrorMessage(errCode) << ".";
#else
		if (d_event->ErrCode != UPNP_E_SUCCESS) {
			msg << UpnpGetErrorMessage(d_event->ErrCode) << ".";
#endif
			AddDebugLogLineN(logUPnP, msg);
		}
		// Get the XML tree device description in doc
#if UPNP_VERSION >= 10800
		const char *location = UpnpDiscovery_get_Location_cstr(d_event);
#else
		const char *location = d_event->Location;
#endif
		// Passive ALIVE announcements arrive in dense bursts (one per service class, every
		// few seconds per device), and when the announcing device's HTTP server is
		// unreachable each fetch blocks a libupnp worker for the full TCP-connect timeout.
		// Suppress repeat fetches against a recently-failed URL on ALIVE only:
		// SEARCH_RESULT is our own active poll.
		if (EventType == UPNP_DISCOVERY_ADVERTISEMENT_ALIVE &&
			upnpCP->ShouldSkipAdvertisementFetch(location ? location : "")) {
			break;
		}
		++upnpCP->m_registering;
		int ret = UpnpDownloadXmlDoc(location, &doc);
		if (ret != UPNP_E_SUCCESS) {
			if (EventType == UPNP_DISCOVERY_ADVERTISEMENT_ALIVE) {
				upnpCP->RecordAdvertisementFetchResult(location ? location : "", false);
			}
			msg << "Error retrieving device description from " << (location ? location : "")
			    << ": " << UpnpGetErrorMessage(ret) << "(" << ret << ").";
			AddDebugLogLineN(logUPnP, msg);
		} else {
			if (EventType == UPNP_DISCOVERY_ADVERTISEMENT_ALIVE) {
				upnpCP->RecordAdvertisementFetchResult(location ? location : "", true);
			}
			msg2 << "Retrieving device description from " << (location ? location : "") << ".";
			AddDebugLogLineN(logUPnP, msg2);
		}
		if (doc) {
			IXML_Element *root = IXML::Document::GetRootElement(doc);
			const std::string urlBase = IXML::Element::GetChildValueByTag(root, "URLBase");
			IXML_Element *rootDevice = IXML::Element::GetFirstChildByTag(root, "device");
			std::string devType(IXML::Element::GetChildValueByTag(rootDevice, "deviceType"));
			// Only add device if it is an InternetGatewayDevice
			// (any version -- IGD:2 advertises its root as ":2").
			if (UPnP::TypeMatchesIgnoringVersion(devType, UPnP::Device::IGW)) {
				// Do not block entry on this condition: there may be more than one
				// device, and the first to arrive may not be the one we want.
				upnpCP->SetIGWDeviceDetected(true);
				// Log it if not UPNP_DISCOVERY_ADVERTISEMENT_ALIVE,
				// we don't want to spam our logs.
				if (EventType != UPNP_DISCOVERY_ADVERTISEMENT_ALIVE) {
					msg.str("Internet Gateway Device Detected.");
					AddDebugLogLineC(logUPnP, msg);
				}
#if UPNP_VERSION >= 10800
				int expires = UpnpDiscovery_get_Expires(d_event);
				upnpCP->AddRootDevice(rootDevice, urlBase, location, expires);
#else
				upnpCP->AddRootDevice(
					rootDevice, urlBase, d_event->Location, d_event->Expires);
#endif
			}
			IXML::Document::Free(doc);
		}
		--upnpCP->m_registering;
		break;
	}
	case UPNP_DISCOVERY_SEARCH_TIMEOUT: {
		// Search timeout
		msg << "UPNP_DISCOVERY_SEARCH_TIMEOUT.";
		AddDebugLogLineN(logUPnP, msg);

		break;
	}
	case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE: {
		// UPnP Device Removed
#if UPNP_VERSION >= 10800
		UpnpDiscovery *dab_event = (UpnpDiscovery *)Event;
		int errCode = UpnpDiscovery_get_ErrCode(dab_event);
		if (errCode != UPNP_E_SUCCESS) {
#else
		struct Upnp_Discovery *dab_event = (struct Upnp_Discovery *)Event;
		if (dab_event->ErrCode != UPNP_E_SUCCESS) {
#endif
			msg << "error(UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE): " <<
#if UPNP_VERSION >= 10800
				UpnpGetErrorMessage(errCode) <<
#else
				UpnpGetErrorMessage(dab_event->ErrCode) <<
#endif
				".";
			AddDebugLogLineN(logUPnP, msg);
		}
#if UPNP_VERSION >= 10800
		std::string devType = UpnpDiscovery_get_DeviceType_cstr(dab_event);
#else
		std::string devType = dab_event->DeviceType;
#endif
		// Check for an InternetGatewayDevice and removes it from the list

		{
			// devType is a UPnP URN ("urn:schemas-upnp-org:device:..."), so ASCII-only.
			// Pin LC_CTYPE = C around the tolower transform so 'I' stays 'i' under
			// tr_TR.
			CCtypeAsciiScope devtype_scope;
			std::transform(devType.begin(), devType.end(), devType.begin(), tolower);
		}

		if (UPnP::TypeMatchesIgnoringVersion(devType, UPnP::Device::IGW)) {
#if UPNP_VERSION >= 10800
			const char *deviceID = UpnpDiscovery_get_DeviceID_cstr(dab_event);
			upnpCP->RemoveRootDevice(deviceID);
#else
			upnpCP->RemoveRootDevice(dab_event->DeviceId);
#endif
		}
		break;
	}
	case UPNP_EVENT_RECEIVED: {
		// Event received
#if UPNP_VERSION >= 10800
		UpnpEvent *e_event = (UpnpEvent *)Event;
		int eventKey = UpnpEvent_get_EventKey(e_event);
		IXML_Document *changedVariables = UpnpEvent_get_ChangedVariables(e_event);
		const std::string sid = UpnpEvent_get_SID_cstr(e_event);
#else
		struct Upnp_Event *e_event = (struct Upnp_Event *)Event;
		const std::string Sid = e_event->Sid;
#endif
		// Parses the event
#if UPNP_VERSION >= 10800
		upnpCP->OnEventReceived(sid, eventKey, changedVariables);
#else
		upnpCP->OnEventReceived(Sid, e_event->EventKey, e_event->ChangedVariables);
#endif
		break;
	}
	case UPNP_EVENT_SUBSCRIBE_COMPLETE:
		msg << "error(UPNP_EVENT_SUBSCRIBE_COMPLETE): ";
		goto upnpEventRenewalComplete;
	case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
		msg << "error(UPNP_EVENT_UNSUBSCRIBE_COMPLETE): ";
		goto upnpEventRenewalComplete;
	case UPNP_EVENT_RENEWAL_COMPLETE: {
		msg << "error(UPNP_EVENT_RENEWAL_COMPLETE): ";
	upnpEventRenewalComplete:
#if UPNP_VERSION >= 10800
		UpnpEventSubscribe *es_event = (UpnpEventSubscribe *)Event;
		int errCode = UpnpEventSubscribe_get_ErrCode(es_event);
		if (errCode != UPNP_E_SUCCESS) {
#else
		struct Upnp_Event_Subscribe *es_event = (struct Upnp_Event_Subscribe *)Event;
		if (es_event->ErrCode != UPNP_E_SUCCESS) {
#endif
			msg << "Error in Event Subscribe Callback";
#if UPNP_VERSION >= 10800
			UPnP::ProcessErrorMessage(msg.str(), errCode, NULL, NULL);
#else
			UPnP::ProcessErrorMessage(msg.str(), es_event->ErrCode, NULL, NULL);
#endif
		} else {
#if 0
#if UPNP_VERSION >= 10800

			const UpnpString *publisherUrl =
				UpnpEventSubscribe_get_PublisherUrl(es_event);
			const char *sid = UpnpEvent_get_SID_cstr(es_event);
			int timeOut = UpnpEvent_get_TimeOut(es_event);
			TvCtrlPointHandleSubscribeUpdate(
				publisherUrl, sid, timeOut);
#else
			TvCtrlPointHandleSubscribeUpdate(
				GET_UPNP_STRING(es_event->PublisherUrl),
				es_event->Sid,
				es_event->TimeOut );
#endif
#endif
		}
		break;
	}
	case UPNP_EVENT_AUTORENEWAL_FAILED:
		msg << "error(UPNP_EVENT_AUTORENEWAL_FAILED): ";
		msg2 << "UPNP_EVENT_AUTORENEWAL_FAILED: ";
		goto upnpEventSubscriptionExpired;
	case UPNP_EVENT_SUBSCRIPTION_EXPIRED: {
		msg << "error(UPNP_EVENT_SUBSCRIPTION_EXPIRED): ";
		msg2 << "UPNP_EVENT_SUBSCRIPTION_EXPIRED: ";
	upnpEventSubscriptionExpired:
#if UPNP_VERSION >= 10800
		UpnpEventSubscribe *es_event = (UpnpEventSubscribe *)Event;
#else
		struct Upnp_Event_Subscribe *es_event = (struct Upnp_Event_Subscribe *)Event;
#endif
		Upnp_SID newSID;
		memset(newSID, 0, sizeof(Upnp_SID));
		int TimeOut = 1801;
#if UPNP_VERSION >= 10800
		const char *publisherUrl = UpnpEventSubscribe_get_PublisherUrl_cstr(es_event);
#endif
		int ret = UpnpSubscribe(upnpCP->m_UPnPClientHandle,
#if UPNP_VERSION >= 10800
			publisherUrl,
#else
			GET_UPNP_STRING(es_event->PublisherUrl),
#endif
			&TimeOut,
			newSID);
		if (ret != UPNP_E_SUCCESS) {
			msg << "Error Subscribing to EventURL";
#if UPNP_VERSION >= 10800
			int errCode = UpnpEventSubscribe_get_ErrCode(es_event);
#endif
			UPnP::ProcessErrorMessage(
#if UPNP_VERSION >= 10800
				msg.str(), errCode, NULL, NULL);
#else
				msg.str(), es_event->ErrCode, NULL, NULL);
#endif
		} else {
			CUPnPMutexLocker lock(upnpCP->m_ServiceMapMutex);
			ServiceMap::iterator it =
#if UPNP_VERSION >= 10800
				upnpCP->m_ServiceMap.find(publisherUrl);
#else
				upnpCP->m_ServiceMap.find(GET_UPNP_STRING(es_event->PublisherUrl));
#endif
			if (it != upnpCP->m_ServiceMap.end()) {
				CUPnPService &service = *(it->second);
				service.SetTimeout(TimeOut);
				service.SetSID(newSID);
				msg2 << "Re-subscribed to EventURL '" <<
#if UPNP_VERSION >= 10800
					publisherUrl <<
#else
					GET_UPNP_STRING(es_event->PublisherUrl) <<
#endif
					"' with SID == '" << newSID << "'.";
				AddDebugLogLineC(logUPnP, msg2);
				// In principle we should test whether the service is the same, but
				// there is only one service here.
				upnpCP->RefreshPortMappings();
			} else {
				msg << "Error: did not find service " << newSID << " in the service map.";
				AddDebugLogLineN(logUPnP, msg);
			}
		}
		break;
	}
	case UPNP_CONTROL_ACTION_COMPLETE: {
		// This is here if we choose to do this asynchronously
#if UPNP_VERSION >= 10800
		UpnpActionComplete *a_event = (UpnpActionComplete *)Event;
		int errCode = UpnpActionComplete_get_ErrCode(a_event);
		IXML_Document *actionResult = UpnpActionComplete_get_ActionResult(a_event);
		if (errCode != UPNP_E_SUCCESS) {
#else
		struct Upnp_Action_Complete *a_event = (struct Upnp_Action_Complete *)Event;
		if (a_event->ErrCode != UPNP_E_SUCCESS) {
#endif
			UPnP::ProcessErrorMessage("UpnpSendActionAsync",
#if UPNP_VERSION >= 10800
				errCode,
				NULL,
				actionResult);
#else
				a_event->ErrCode,
				NULL,
				a_event->ActionResult);
#endif
		} else {
			UPnP::ProcessActionResponse(
#if UPNP_VERSION >= 10800
				actionResult,
#else
				a_event->ActionResult,
#endif
				"<UpnpSendActionAsync>");
		}
		/* Nothing to process here, just print the results. Service state table updates are handled
		 * by events. */
		break;
	}
	case UPNP_CONTROL_GET_VAR_COMPLETE: {
		msg << "error(UPNP_CONTROL_GET_VAR_COMPLETE): ";
#if UPNP_VERSION >= 10800
		UpnpStateVarComplete *sv_event = (UpnpStateVarComplete *)Event;
		int errCode = UpnpStateVarComplete_get_ErrCode(sv_event);
		if (errCode != UPNP_E_SUCCESS) {
#else
		struct Upnp_State_Var_Complete *sv_event = (struct Upnp_State_Var_Complete *)Event;
		if (sv_event->ErrCode != UPNP_E_SUCCESS) {
#endif
			msg << "m_UpnpGetServiceVarStatusAsync";
			UPnP::ProcessErrorMessage(
#if UPNP_VERSION >= 10800
				msg.str(), errCode, NULL, NULL);
#else
				msg.str(), sv_event->ErrCode, NULL, NULL);
#endif
		} else {
#if 0
			// Warning: UpnpGetServiceVarStatus and UpnpGetServiceVarStatusAsync are
			// deprecated by the UPnP forum.
#if UPNP_VERSION >= 10800
			const char *ctrlUrl =
				UpnpStateVarComplete_get_CtrlUrl(sv_event);
			const char *stateVarName =
				UpnpStateVarComplete_get_StateVarName(sv_event);
			const DOMString currentVal =
				UpnpStateVarComplete_get_CurrentVal(sv_event);
			TvCtrlPointHandleGetVar(
				ctrlUrl, stateVarName, currentVal);
#else
			TvCtrlPointHandleGetVar(
				sv_event->CtrlUrl,
				sv_event->StateVarName,
				sv_event->CurrentVal );
#endif
#endif
		}
		break;
	}
	// ignore these cases, since this is not a device
	case UPNP_CONTROL_GET_VAR_REQUEST:
		msg << "error(UPNP_CONTROL_GET_VAR_REQUEST): ";
		goto eventSubscriptionRequest;
	case UPNP_CONTROL_ACTION_REQUEST:
		msg << "error(UPNP_CONTROL_ACTION_REQUEST): ";
		goto eventSubscriptionRequest;
	case UPNP_EVENT_SUBSCRIPTION_REQUEST:
		msg << "error(UPNP_EVENT_SUBSCRIPTION_REQUEST): ";
	eventSubscriptionRequest:
		msg << "This is not a UPnP Device, this is a UPnP Control Point, event ignored.";
		AddDebugLogLineC(logUPnP, msg);
		break;
	default:
		fprintf(stderr, "Callback: default... Unknown event:'%d', not good.\n", EventType);
		msg << "error(UPnP::Callback): Event not handled:'" << EventType << "'.";
		fprintf(stderr, "%s\n", msg.str().c_str());
		AddDebugLogLineC(logUPnP, msg);
		// Better not throw in the callback. Who would catch it?
		break;
	}

	return 0;
}

void CUPnPControlPoint::OnEventReceived(
	const std::string &Sid, int EventKey, IXML_Document *ChangedVariablesDoc)
{
	std::ostringstream msg;
	msg << "UPNP_EVENT_RECEIVED:" << "\n    SID: " << Sid << "\n    Key: " << EventKey
	    << "\n    Property list:";
	IXML_Element *root = IXML::Document::GetRootElement(ChangedVariablesDoc);
	IXML_Element *child = IXML::Element::GetFirstChild(root);
	if (child) {
		while (child) {
			IXML_Element *child2 = IXML::Element::GetFirstChild(child);
			const DOMString childTag = IXML::Element::GetTag(child2);
			std::string childValue = IXML::Element::GetTextValue(child2);
			msg << "\n        " << childTag << "='" << childValue << "'";
			child = IXML::Element::GetNextSibling(child);
		}
	} else {
		msg << "\n    Empty property list.";
	}
	AddDebugLogLineC(logUPnP, msg);
}

void CUPnPControlPoint::AddRootDevice(
	IXML_Element *rootDevice, const std::string &urlBase, const char *location, int expires)
{
	CUPnPMutexLocker lock(m_RootDeviceListMutex);

	const std::string &OriginalURLBase(urlBase);
	std::string FixedURLBase(OriginalURLBase.empty() ? location : OriginalURLBase);

	std::string UDN(IXML::Element::GetChildValueByTag(rootDevice, "UDN"));
	const std::pair<std::string, bool> key(UDN, UPnP::IsIPv6Location(location ? location : ""));
	RootDeviceMap::iterator it = m_RootDeviceMap.find(key);
	if (it != m_RootDeviceMap.end()) {
		it->second->SetExpires(expires);
	} else {
		CUPnPRootDevice *upnpRootDevice = new CUPnPRootDevice(
			*this, rootDevice, OriginalURLBase, FixedURLBase, location, expires);
		m_RootDeviceMap[key] = upnpRootDevice;
	}
	// Not on a device alone: some switches answer as a gateway and list no services.
	if (WanServiceDetected()) {
		m_gatewayMapped = true;
	}
}

void CUPnPControlPoint::RemoveRootDevice(const char *udn)
{
	CUPnPMutexLocker lock(m_RootDeviceListMutex);

	const std::string UDN(udn);
	for (RootDeviceMap::iterator it = m_RootDeviceMap.begin(); it != m_RootDeviceMap.end();) {
		if (it->first.first == UDN) {
			delete it->second;
			it = m_RootDeviceMap.erase(it);
		} else {
			++it;
		}
	}
}

void CUPnPControlPoint::ForgetService(const CUPnPService *service)
{
	CUPnPMutexLocker lock(m_ServiceMapMutex);
	for (ServiceMap::iterator it = m_ServiceMap.begin(); it != m_ServiceMap.end();) {
		if (it->second == service) {
			it = m_ServiceMap.erase(it);
		} else {
			++it;
		}
	}
}

bool CUPnPControlPoint::ShouldSkipAdvertisementFetch(const std::string &location)
{
	CUPnPMutexLocker lock(m_failedFetchCacheMutex);
	std::map<std::string, time_t>::iterator it = m_failedFetchCache.find(location);
	if (it == m_failedFetchCache.end()) {
		return false;
	}
	if (time(NULL) - it->second >= FAILED_FETCH_TTL_SECS) {
		// TTL expired -- erase and allow one retry, which will refresh
		// the entry on failure or clear it on success.
		m_failedFetchCache.erase(it);
		return false;
	}
	return true;
}

void CUPnPControlPoint::RecordAdvertisementFetchResult(const std::string &location, bool /*success*/)
{
	CUPnPMutexLocker lock(m_failedFetchCacheMutex);
	// Rate-limit successes the same way as failures. Erasing on success re-issued a blocking
	// UpnpDownloadXmlDoc on every subsequent ALIVE announcement for an already-classified
	// location, and those arrive in dense repeating bursts, so on a busy LAN they alone can
	// keep libupnp's thread pool saturated. Recording the attempt time regardless of outcome
	// bounds ALIVE-driven downloads to one per location per FAILED_FETCH_TTL_SECS, and nothing
	// is lost: a gateway found on the first fetch is already registered, and later ALIVEs only
	// refresh the expiry, which the TTL covers.
	m_failedFetchCache[location] = time(nullptr);
}

void CUPnPControlPoint::Subscribe(CUPnPService &service)
{
	std::ostringstream msg;

	IXML_Document *scpdDoc = NULL;
	int errcode = UpnpDownloadXmlDoc(service.GetAbsSCPDURL().c_str(), &scpdDoc);
	if (errcode == UPNP_E_SUCCESS) {
		IXML_Element *scpdRoot = IXML::Document::GetRootElement(scpdDoc);
		CUPnPSCPD *scpd = new CUPnPSCPD(*this, scpdRoot, service.GetAbsSCPDURL());
		service.SetSCPD(scpd);
		IXML::Document::Free(scpdDoc);
		{
			// In one step, so each mapping reaches each service once, whichever thread comes
			// first. Held across the actions so that a delete cannot run before them and leave
			// the mapping on the gateway.
			CUPnPMutexLocker lock(m_ServiceMapMutex);
			m_ServiceMap[service.GetAbsEventSubURL()] = &service;
			if (!UPnP::IsIPv6FirewallService(service.GetServiceType())) {
				for (const auto &entry : m_ActivePortMappingsMap) {
					service.Execute(
						"AddPortMapping", AddPortMappingArguments(entry.second));
				}
			} else {
				// Requests recorded before this service registered are replayed here: this is
				// the point where they can finally be sent.
				for (auto &entry : m_IPv6Pinholes) {
					SendDeletePinhole(entry.second);
					SendAddPinhole(entry.second);
				}
			}
		}
		msg << "Successfully retrieved SCPD Document for service " << service.GetServiceType()
		    << ", absEventSubURL: " << service.GetAbsEventSubURL() << ".";
		AddDebugLogLineN(logUPnP, msg);
		msg.str("");

		// Now try to subscribe to this service. Without a successful subscription we are
		// not notified about events, but the service may still be usable.
		errcode = UpnpSubscribe(m_UPnPClientHandle,
			service.GetAbsEventSubURL().c_str(),
			service.GetTimeoutAddr(),
			service.GetSID());
		if (errcode == UPNP_E_SUCCESS) {
			msg << "Successfully subscribed to service " << service.GetServiceType()
			    << ", absEventSubURL: " << service.GetAbsEventSubURL() << ".";
			AddDebugLogLineC(logUPnP, msg);
		} else {
			msg << "Error subscribing to service " << service.GetServiceType()
			    << ", absEventSubURL: " << service.GetAbsEventSubURL()
			    << ", error: " << UpnpGetErrorMessage(errcode) << ".";
			goto error;
		}
	} else {
		msg << "Error getting SCPD Document from " << service.GetAbsSCPDURL() << ".";
		AddDebugLogLineN(logUPnP, msg);
	}

	return;

error:
	AddDebugLogLineN(logUPnP, msg);
}

void CUPnPControlPoint::Unsubscribe(CUPnPService &service)
{
	{
		CUPnPMutexLocker lock(m_ServiceMapMutex);
		ServiceMap::iterator it = m_ServiceMap.find(service.GetAbsEventSubURL());
		if (it == m_ServiceMap.end()) {
			return;
		}
		m_ServiceMap.erase(it);
	}
	UpnpUnSubscribe(m_UPnPClientHandle, service.GetSID());
}

#endif /* ENABLE_UPNP */

//
// This file is part of the aMule Project.
//
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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
//

#include "PrefsSchema.h"

#include <cstring>
#include <type_traits>

namespace webapi
{

namespace
{

// Enum value tables. Order is the wire order: an entry's index is the integer
// the daemon stores, so entries may be appended but never reordered.
const char *const kProxyTypes[] = { "socks5", "socks4", "http", "socks4a", nullptr };
const char *const kSharedFilesVisibility[] = { "everybody", "friends", "nobody", nullptr };
const char *const kIp2CountrySources[] = { "dbip", "maxmind", "custom", nullptr };

// Everything from here to the matching `clang-format on` is a data table, not
// code, and is kept one row per field so it reads as a specification.
// ColumnLimit is 110 and the longest row is ~184 characters, so the formatter
// would otherwise break every row across seven lines -- which costs the
// property this table exists for: renaming or adding a preference stays a
// one-line diff a reviewer can take in at a glance.
//
// clang-format off

// Address-of-member accessor with a compile-time guard that the row's declared
// PrefType matches the member's real C++ type. Getting the two out of step is
// the one way a table row could be quietly wrong, so it is a build error.
#define PREF_MEMBER(memb, cpp_type) \
	[](PreferencesSnapshot &p) -> void * { \
		static_assert(std::is_same<decltype(p.memb), cpp_type>::value, \
			"PrefsSchema: declared PrefType does not match the type of " #memb); \
		return &p.memb; \
	}

#define PREF_BOOL(cat, key, tag, enc, inv, acc, memb) \
	{cat, key, tag, PrefType::Bool, enc, inv, acc, 0u, nullptr, nullptr, PREF_MEMBER(memb, bool), 0}

// Same as PREF_BOOL but the tag is read out of a different EC group than the
// one this field's JSON category maps to.
#define PREF_BOOL_INGROUP(cat, key, tag, enc, acc, memb, group) \
	{cat, key, tag, PrefType::Bool, enc, false, acc, 0u, nullptr, nullptr, PREF_MEMBER(memb, bool), group}

// Capability-gated: PATCH answers 409 when the named sibling bool is false.
#define PREF_BOOL_GATED(cat, key, tag, enc, inv, acc, memb, gate) \
	{cat, key, tag, PrefType::Bool, enc, inv, acc, 0u, nullptr, gate, PREF_MEMBER(memb, bool), 0}

#define PREF_U16(cat, key, tag, maxv, acc, memb) \
	{cat, key, tag, PrefType::Uint16, PrefEnc::Value, false, acc, maxv, nullptr, nullptr, PREF_MEMBER(memb, std::uint16_t), 0}

#define PREF_U32(cat, key, tag, maxv, acc, memb) \
	{cat, key, tag, PrefType::Uint32, PrefEnc::Value, false, acc, maxv, nullptr, nullptr, PREF_MEMBER(memb, std::uint32_t), 0}

// Same as PREF_U32 but the API value is the EC value divided by `scale`.
#define PREF_U32_SCALED(cat, key, tag, maxv, acc, memb, scale) \
	{cat, key, tag, PrefType::Uint32, PrefEnc::Value, false, acc, maxv, nullptr, nullptr, PREF_MEMBER(memb, std::uint32_t), 0, scale}

// Numeric rows whose real domain is narrower than their type. `minv`/`maxv` are
// both inclusive and both rejected with a 400; `stepv` is the granularity the
// core stores at, 0 when the row is not quantised. See the notes on
// PrefField::min and ::step for why this is declared rather than clamped.
#define PREF_U32_DOMAIN(cat, key, tag, minv, maxv, stepv, acc, memb) \
	{cat, key, tag, PrefType::Uint32, PrefEnc::Value, false, acc, maxv, nullptr, nullptr, \
		PREF_MEMBER(memb, std::uint32_t), 0, 0, minv, stepv}

#define PREF_U16_DOMAIN(cat, key, tag, minv, maxv, acc, memb) \
	{cat, key, tag, PrefType::Uint16, PrefEnc::Value, false, acc, maxv, nullptr, nullptr, \
		PREF_MEMBER(memb, std::uint16_t), 0, 0, minv, 0}

// Scaled and bounded: bounds are in the API's unit, applied before scaling.
#define PREF_U32_SCALED_DOMAIN(cat, key, tag, minv, maxv, acc, memb, scale) \
	{cat, key, tag, PrefType::Uint32, PrefEnc::Value, false, acc, maxv, nullptr, nullptr, \
		PREF_MEMBER(memb, std::uint32_t), 0, scale, minv, 0}

#define PREF_STR(cat, key, tag, acc, memb) \
	{cat, key, tag, PrefType::String, PrefEnc::Value, false, acc, 0u, nullptr, nullptr, PREF_MEMBER(memb, std::string), 0}

#define PREF_STRARR(cat, key, tag, acc, memb) \
	{cat, key, tag, PrefType::StringArray, PrefEnc::Value, false, acc, 0u, nullptr, nullptr, PREF_MEMBER(memb, std::vector<std::string>), 0}

#define PREF_ENUM(cat, key, tag, names, acc, memb) \
	{cat, key, tag, PrefType::Enum, PrefEnc::Value, false, acc, 0u, names, nullptr, PREF_MEMBER(memb, std::string), 0}

#define PREF_MD4(cat, key, tag, acc, memb) \
	{cat, key, tag, PrefType::Md4Hex, PrefEnc::Value, false, acc, 0u, nullptr, nullptr, PREF_MEMBER(memb, std::string), 0}

// Write-only rows have no backing member: nothing is ever read back for them.
#define PREF_PASSWD_PLAIN(cat, key, tag) \
	{cat, key, tag, PrefType::String, PrefEnc::Value, false, PrefAccess::WriteOnly, 0u, nullptr, nullptr, nullptr, 0}

#define PREF_PASSWD_HASHED(cat, key, tag) \
	{cat, key, tag, PrefType::Md4Hex, PrefEnc::Value, false, PrefAccess::WriteOnly, 0u, nullptr, nullptr, nullptr, 0}

// Rejected rows carry no usable tag; they exist so the PATCH walker can answer
// with a pointer to the endpoint that does own the field.
#define PREF_REJECT(cat, key) \
	{cat, key, 0, PrefType::String, PrefEnc::Value, false, PrefAccess::Rejected, 0u, nullptr, nullptr, nullptr, 0}

const PrefField kSchema[] = {
	// [general]
	// Value, not Presence: the core emits EC_TAG_GENERAL_CHECK_NEW_VERSION
	// unconditionally as a value tag, so presence-decoding it pinned the
	// answer to true and a read-modify-write of any other preference
	// silently re-enabled version checking.
	PREF_BOOL("general", "version_check_enabled", EC_TAG_GENERAL_CHECK_NEW_VERSION, PrefEnc::Value, false, PrefAccess::ReadWrite, version_check_enabled),
	PREF_STR("general", "daemon_host_name", EC_TAG_USER_HOST, PrefAccess::ReadOnly, daemon_host_name),
	PREF_STR("general", "nickname", EC_TAG_USER_NICK, PrefAccess::ReadWrite, nickname),
	PREF_MD4("general", "user_hash", EC_TAG_USER_HASH, PrefAccess::ReadOnly, user_hash),

	// [connection]
	PREF_BOOL("connection", "autoconnect", EC_TAG_CONN_AUTOCONNECT, PrefEnc::Presence, false, PrefAccess::ReadWrite, autoconnect),
	PREF_STR("connection", "bind_address", EC_TAG_CONN_BIND_ADDRESS, PrefAccess::ReadWrite, bind_address),
	PREF_STR("connection", "bind_interface", EC_TAG_CONN_BIND_INTERFACE, PrefAccess::ReadWrite, bind_interface),
	PREF_BOOL("connection", "extended_udp_port_enabled", EC_TAG_CONN_UDP_DISABLE, PrefEnc::Presence, true, PrefAccess::ReadWrite, extended_udp_port_enabled),
	PREF_U32("connection", "max_connection_count", EC_TAG_CONN_MAX_CONN, 65535u, PrefAccess::ReadWrite, max_connection_count),
	PREF_U32("connection", "max_download_kibibytes_per_second", EC_TAG_CONN_MAX_DL, 1000000000u, PrefAccess::ReadWrite, max_download_kibibytes_per_second),
	PREF_U32("connection", "max_sources_per_file_count", EC_TAG_CONN_MAX_FILE_SOURCES, 65535u, PrefAccess::ReadWrite, max_sources_per_file_count),
	PREF_U32("connection", "max_upload_kibibytes_per_second", EC_TAG_CONN_MAX_UL, 1000000000u, PrefAccess::ReadWrite, max_upload_kibibytes_per_second),
	PREF_BOOL("connection", "ed2k_enabled", EC_TAG_NETWORK_ED2K, PrefEnc::Presence, false, PrefAccess::ReadWrite, ed2k_enabled),
	PREF_BOOL("connection", "kad_enabled", EC_TAG_NETWORK_KADEMLIA, PrefEnc::Presence, false, PrefAccess::ReadWrite, kad_enabled),
	PREF_BOOL("connection", "proxy_auth", EC_TAG_PROXY_AUTH, PrefEnc::Value, false, PrefAccess::ReadWrite, proxy_auth),
	PREF_BOOL("connection", "proxy_enabled", EC_TAG_PROXY_ENABLE, PrefEnc::Value, false, PrefAccess::ReadWrite, proxy_enabled),
	PREF_STR("connection", "proxy_host", EC_TAG_PROXY_HOST, PrefAccess::ReadWrite, proxy_host),
	PREF_U16("connection", "proxy_port", EC_TAG_PROXY_PORT, 65535u, PrefAccess::ReadWrite, proxy_port),
	PREF_ENUM("connection", "proxy_type", EC_TAG_PROXY_TYPE, kProxyTypes, PrefAccess::ReadWrite, proxy_type),
	PREF_STR("connection", "proxy_user", EC_TAG_PROXY_USER, PrefAccess::ReadWrite, proxy_user),
	PREF_BOOL("connection", "reconnect_on_connection_loss", EC_TAG_CONN_RECONNECT, PrefEnc::Presence, false, PrefAccess::ReadWrite, reconnect_on_connection_loss),
	// 65532, not 65535: SetPort() substitutes DEFAULT_TCP_PORT when val + 3
	// exceeds 65535, because the server UDP socket is TCP+3. A 65534 here was a
	// 200 that left the daemon listening on 4662.
	PREF_U16_DOMAIN("connection", "tcp_port", EC_TAG_CONN_TCP_PORT, 1u, 65532u, PrefAccess::ReadWrite, tcp_port),
	PREF_U16("connection", "udp_port", EC_TAG_CONN_UDP_PORT, 65535u, PrefAccess::ReadWrite, udp_port),
	// Floor of 1: SetSlotAllocation clamps anything lower up to 1, so
	// without the bound a 0 answered 200 and read back as 1.
	PREF_U32_DOMAIN("connection", "upload_slot_min_kibibytes_per_second", EC_TAG_CONN_SLOT_ALLOCATION, 1u, 65535u, 0u, PrefAccess::ReadWrite, upload_slot_min_kibibytes_per_second),
	PREF_BOOL_INGROUP("connection", "upnp_supported", EC_TAG_GENERAL_UPNP_AVAILABLE, PrefEnc::Value, PrefAccess::ReadOnly, upnp_supported, EC_TAG_PREFS_GENERAL),
	PREF_BOOL("connection", "upnp_enabled", EC_TAG_CONN_UPNP_ENABLED, PrefEnc::Value, false, PrefAccess::ReadWrite, upnp_enabled),
	PREF_U16("connection", "upnp_control_point_port", EC_TAG_CONN_UPNP_TCP_PORT, 65535u, PrefAccess::ReadWrite, upnp_control_point_port),
	PREF_PASSWD_PLAIN("connection", "proxy_password", EC_TAG_PROXY_PASSWORD),

	// [directories]
	PREF_BOOL("directories", "rescan_on_startup", EC_TAG_DIRECTORIES_AUTO_RESCAN, PrefEnc::Presence, false, PrefAccess::ReadWrite, directories.rescan_on_startup),
	PREF_STR("directories", "exclude_patterns", EC_TAG_DIRECTORIES_EXCLUDE_PATTERNS, PrefAccess::ReadWrite, directories.exclude_patterns),
	PREF_BOOL("directories", "exclude_patterns_use_regex", EC_TAG_DIRECTORIES_EXCLUDE_REGEX, PrefEnc::Value, false, PrefAccess::ReadWrite, directories.exclude_patterns_use_regex),
	PREF_BOOL("directories", "follow_symlinks", EC_TAG_DIRECTORIES_FOLLOW_SYMLINKS, PrefEnc::Presence, false, PrefAccess::ReadWrite, directories.follow_symlinks),
	PREF_STR("directories", "incoming_path", EC_TAG_DIRECTORIES_INCOMING, PrefAccess::ReadWrite, directories.incoming_path),
	PREF_BOOL("directories", "share_hidden", EC_TAG_DIRECTORIES_SHARE_HIDDEN, PrefEnc::Presence, false, PrefAccess::ReadWrite, directories.share_hidden),
	PREF_STRARR("directories", "shared_paths", EC_TAG_DIRECTORIES_SHARED, PrefAccess::ReadWrite, directories.shared_paths),
	PREF_STR("directories", "temp_path", EC_TAG_DIRECTORIES_TEMP, PrefAccess::ReadWrite, directories.temp_path),

	// [files]
	PREF_BOOL("files", "add_new_downloads_paused", EC_TAG_FILES_NEW_PAUSED, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.add_new_downloads_paused),
	PREF_BOOL("files", "trust_unverified_aich_hashes", EC_TAG_FILES_AICH_TRUST, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.trust_unverified_aich_hashes),
	PREF_BOOL("files", "create_sparse_files", EC_TAG_FILES_CREATE_NORMAL, PrefEnc::Presence, true, PrefAccess::ReadWrite, files.create_sparse_files),
	PREF_BOOL("files", "endgame_mode_enabled", EC_TAG_FILES_ENDGAME, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.endgame_mode_enabled),
	PREF_STR("files", "ffprobe_path", EC_TAG_FILES_MEDIA_FFPROBE_PATH, PrefAccess::ReadWrite, files.ffprobe_path),
	PREF_BOOL("files", "ich_enabled", EC_TAG_FILES_ICH_ENABLED, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.ich_enabled),
	PREF_BOOL("files", "media_metadata_enabled", EC_TAG_FILES_MEDIA_METADATA_ENABLED, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.media_metadata_enabled),
	PREF_U32("files", "min_free_space_mebibytes", EC_TAG_FILES_MIN_FREE_SPACE, 0xFFFFFFFFu, PrefAccess::ReadWrite, files.min_free_space_mebibytes),
	PREF_BOOL_GATED("files", "mmap_enabled", EC_TAG_FILES_MMAP_ENABLED, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.mmap_enabled, "mmap_supported"),
	PREF_BOOL("files", "mmap_supported", EC_TAG_FILES_MMAP_SUPPORTED, PrefEnc::Presence, false, PrefAccess::ReadOnly, files.mmap_supported),
	PREF_BOOL("files", "new_downloads_auto_priority", EC_TAG_FILES_NEW_AUTO_DL_PRIO, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.new_downloads_auto_priority),
	PREF_BOOL("files", "new_shared_files_auto_priority", EC_TAG_FILES_NEW_AUTO_UL_PRIO, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.new_shared_files_auto_priority),
	PREF_BOOL("files", "preallocate_full_file_size", EC_TAG_FILES_ALLOC_FULL_SIZE, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.preallocate_full_file_size),
	PREF_BOOL("files", "prioritize_first_last_chunks", EC_TAG_FILES_PREVIEW_PRIO, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.prioritize_first_last_chunks),
	PREF_BOOL("files", "save_sources_for_rare_files", EC_TAG_FILES_SAVE_SOURCES, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.save_sources_for_rare_files),
	PREF_BOOL("files", "on_finished_start_next_alphabetically", EC_TAG_FILES_START_NEXT_ALPHA, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.on_finished_start_next_alphabetically),
	PREF_BOOL("files", "on_finished_start_next_paused", EC_TAG_FILES_START_NEXT_PAUSED, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.on_finished_start_next_paused),
	PREF_BOOL("files", "on_finished_start_next_in_same_category", EC_TAG_FILES_RESUME_SAME_CAT, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.on_finished_start_next_in_same_category),
	PREF_BOOL("files", "stop_on_low_disk_space", EC_TAG_FILES_CHECK_FREE_SPACE, PrefEnc::Presence, false, PrefAccess::ReadWrite, files.stop_on_low_disk_space),

	// [servers]
	PREF_BOOL("servers", "update_list_at_startup", EC_TAG_SERVERS_AUTO_UPDATE, PrefEnc::Presence, false, PrefAccess::ReadWrite, servers.update_list_at_startup),
	PREF_BOOL("servers", "autoconnect_static_servers_only", EC_TAG_SERVERS_AUTOCONN_STATIC_ONLY, PrefEnc::Presence, false, PrefAccess::ReadWrite, servers.autoconnect_static_servers_only),
	PREF_U32("servers", "dead_server_retry_count", EC_TAG_SERVERS_DEAD_SERVER_RETRIES, 65535u, PrefAccess::ReadWrite, servers.dead_server_retry_count),
	PREF_BOOL("servers", "manual_servers_high_priority", EC_TAG_SERVERS_MANUAL_HIGH_PRIO, PrefEnc::Presence, false, PrefAccess::ReadWrite, servers.manual_servers_high_priority),
	PREF_BOOL("servers", "remove_dead_servers", EC_TAG_SERVERS_REMOVE_DEAD, PrefEnc::Presence, false, PrefAccess::ReadWrite, servers.remove_dead_servers),
	PREF_BOOL("servers", "safe_server_connect_enabled", EC_TAG_SERVERS_SAFE_SERVER_CONNECT, PrefEnc::Presence, false, PrefAccess::ReadWrite, servers.safe_server_connect_enabled),
	PREF_BOOL("servers", "smart_lowid_check_enabled", EC_TAG_SERVERS_SMART_ID_CHECK, PrefEnc::Presence, false, PrefAccess::ReadWrite, servers.smart_lowid_check_enabled),
	PREF_BOOL("servers", "update_list_from_client", EC_TAG_SERVERS_ADD_FROM_CLIENT, PrefEnc::Presence, false, PrefAccess::ReadWrite, servers.update_list_from_client),
	PREF_BOOL("servers", "update_list_from_server", EC_TAG_SERVERS_ADD_FROM_SERVER, PrefEnc::Presence, false, PrefAccess::ReadWrite, servers.update_list_from_server),
	PREF_STR("servers", "update_url", EC_TAG_SERVERS_UPDATE_URL, PrefAccess::ReadWrite, servers.update_url),
	PREF_BOOL("servers", "server_priority_system_enabled", EC_TAG_SERVERS_USE_SCORE_SYSTEM, PrefEnc::Presence, false, PrefAccess::ReadWrite, servers.server_priority_system_enabled),

	// [security]
	PREF_BOOL("security", "ipfilter_auto_update", EC_TAG_IPFILTER_AUTO_UPDATE, PrefEnc::Presence, false, PrefAccess::ReadWrite, security.ipfilter_auto_update),
	PREF_U32("security", "ipfilter_min_access_level", EC_TAG_IPFILTER_LEVEL, 255u, PrefAccess::ReadWrite, security.ipfilter_min_access_level),
	PREF_BOOL("security", "ipfilter_clients_enabled", EC_TAG_IPFILTER_CLIENTS, PrefEnc::Presence, false, PrefAccess::ReadWrite, security.ipfilter_clients_enabled),
	PREF_BOOL("security", "ipfilter_include_lan_ips", EC_TAG_IPFILTER_FILTER_LAN, PrefEnc::Presence, false, PrefAccess::ReadWrite, security.ipfilter_include_lan_ips),
	PREF_BOOL("security", "ipfilter_servers_enabled", EC_TAG_IPFILTER_SERVERS, PrefEnc::Presence, false, PrefAccess::ReadWrite, security.ipfilter_servers_enabled),
	PREF_STR("security", "ipfilter_update_url", EC_TAG_IPFILTER_UPDATE_URL, PrefAccess::ReadWrite, security.ipfilter_update_url),
	PREF_BOOL("security", "protocol_obfuscation_enabled", EC_TAG_SECURITY_OBFUSCATION_SUPPORTED, PrefEnc::Presence, false, PrefAccess::ReadWrite, security.protocol_obfuscation_enabled),
	PREF_BOOL("security", "obfuscation_requested", EC_TAG_SECURITY_OBFUSCATION_REQUESTED, PrefEnc::Presence, false, PrefAccess::ReadWrite, security.obfuscation_requested),
	PREF_BOOL("security", "obfuscation_required", EC_TAG_SECURITY_OBFUSCATION_REQUIRED, PrefEnc::Presence, false, PrefAccess::ReadWrite, security.obfuscation_required),
	PREF_BOOL("security", "reject_spoofed_source_ips", EC_TAG_IPFILTER_PARANOID, PrefEnc::Presence, false, PrefAccess::ReadWrite, security.reject_spoofed_source_ips),
	PREF_ENUM("security", "shared_files_visibility", EC_TAG_SECURITY_CAN_SEE_SHARES, kSharedFilesVisibility, PrefAccess::ReadWrite, security.shared_files_visibility),
	PREF_BOOL("security", "secure_identification_enabled", EC_TAG_SECURITY_USE_SECIDENT, PrefEnc::Presence, false, PrefAccess::ReadWrite, security.secure_identification_enabled),
	PREF_BOOL("security", "system_ipfilter_enabled", EC_TAG_IPFILTER_SYSTEM, PrefEnc::Presence, false, PrefAccess::ReadWrite, security.system_ipfilter_enabled),

	// [message_filter]
	PREF_BOOL("message_filter", "accept_from_friends_only", EC_TAG_MSGFILTER_FRIENDS, PrefEnc::Presence, false, PrefAccess::ReadWrite, message_filter.accept_from_friends_only),
	PREF_BOOL("message_filter", "accept_from_known_clients_only", EC_TAG_MSGFILTER_SECURE, PrefEnc::Presence, false, PrefAccess::ReadWrite, message_filter.accept_from_known_clients_only),
	PREF_BOOL("message_filter", "filter_by_keyword", EC_TAG_MSGFILTER_BY_KEYWORD, PrefEnc::Presence, false, PrefAccess::ReadWrite, message_filter.filter_by_keyword),
	PREF_STR("message_filter", "comment_keywords", EC_TAG_MSGFILTER_COMMENT_KEYWORDS, PrefAccess::ReadWrite, message_filter.comment_keywords),
	PREF_BOOL("message_filter", "enabled", EC_TAG_MSGFILTER_ENABLED, PrefEnc::Presence, false, PrefAccess::ReadWrite, message_filter.enabled),
	PREF_BOOL("message_filter", "filter_all_messages", EC_TAG_MSGFILTER_ALL, PrefEnc::Presence, false, PrefAccess::ReadWrite, message_filter.filter_all_messages),
	PREF_BOOL("message_filter", "filter_comments", EC_TAG_MSGFILTER_FILTER_COMMENTS, PrefEnc::Presence, false, PrefAccess::ReadWrite, message_filter.filter_comments),
	PREF_STR("message_filter", "keywords", EC_TAG_MSGFILTER_KEYWORDS, PrefAccess::ReadWrite, message_filter.keywords),
	PREF_BOOL("message_filter", "log_filtered_messages", EC_TAG_MSGFILTER_SHOW_IN_LOG, PrefEnc::Presence, false, PrefAccess::ReadWrite, message_filter.log_filtered_messages),

	// [remote_controls.webserver]
	PREF_BOOL("remote_controls.webserver", "enabled", EC_TAG_WEBSERVER_AUTORUN, PrefEnc::Presence, false, PrefAccess::ReadWrite, remote_controls.webserver.enabled),
	// Its partner `remote_controls.webserver.guest_password` deliberately has
	// NO row here, and that is not an oversight -- neither access level fits.
	// The two share one EC tag (EC_TAG_WEBSERVER_GUEST carries the enable
	// bool with the password hash as a child), so a WriteOnly row would send
	// the generic loop at the tag a second time behind the bespoke packing;
	// and Bespoke rows are emitted on GET, which a password must never be.
	// It is therefore applied only by the hand-written branch in Api.cpp and
	// documented in REFERENCE.md, which is where a client can find it.
	PREF_BOOL("remote_controls.webserver", "guest_enabled", EC_TAG_WEBSERVER_GUEST, PrefEnc::Presence, false, PrefAccess::Bespoke, remote_controls.webserver.guest_enabled),
	PREF_U32("remote_controls.webserver", "port", EC_TAG_WEBSERVER_PORT, 65535u, PrefAccess::ReadWrite, remote_controls.webserver.port),
	PREF_U32("remote_controls.webserver", "refresh_seconds", EC_TAG_WEBSERVER_REFRESH, 0xFFFFFFFFu, PrefAccess::ReadWrite, remote_controls.webserver.refresh_seconds),
	PREF_STR("remote_controls.webserver", "template_name", EC_TAG_WEBSERVER_TEMPLATE, PrefAccess::ReadWrite, remote_controls.webserver.template_name),
	PREF_BOOL("remote_controls.webserver", "gzip_enabled", EC_TAG_WEBSERVER_USEGZIP, PrefEnc::Presence, false, PrefAccess::ReadWrite, remote_controls.webserver.gzip_enabled),
	PREF_PASSWD_HASHED("remote_controls.webserver", "password", EC_TAG_PASSWD_HASH),

	// [remote_controls.amuleapi]
	PREF_STR("remote_controls.amuleapi", "bind_address", EC_TAG_AMULEAPI_BIND, PrefAccess::ReadWrite, remote_controls.amuleapi.bind_address),
	PREF_BOOL("remote_controls.amuleapi", "enabled", EC_TAG_AMULEAPI_AUTORUN, PrefEnc::Presence, false, PrefAccess::ReadWrite, remote_controls.amuleapi.enabled),
	PREF_U32("remote_controls.amuleapi", "port", EC_TAG_AMULEAPI_PORT, 65535u, PrefAccess::ReadWrite, remote_controls.amuleapi.port),
	PREF_REJECT("remote_controls.amuleapi", "password"),
	PREF_REJECT("remote_controls.amuleapi", "guest_password"),
	PREF_REJECT("remote_controls.amuleapi", "guest_enabled"),

	// [online_signature]
	PREF_STR("online_signature", "directory", EC_TAG_ONLINESIG_DIRECTORY, PrefAccess::ReadWrite, online_signature.directory),
	PREF_BOOL("online_signature", "enabled", EC_TAG_ONLINESIG_ENABLED, PrefEnc::Presence, false, PrefAccess::ReadWrite, online_signature.enabled),
	// 65535, not the uint32 ceiling: CPreferences::SetOSUpdate takes a uint16
	// (Preferences.h), so anything larger wraps on the way in -- 86400 (daily)
	// became 20864 and the PATCH still reported success. Capping here turns a
	// silent rewrite into the 400 every other numeric preference answers.
	PREF_U32("online_signature", "update_frequency_seconds", EC_TAG_ONLINESIG_UPDATE, 65535u, PrefAccess::ReadWrite, online_signature.update_frequency_seconds),

	// [advanced] (EC group: CORETWEAKS)
	// s_iFileBufferSize is a uint8 holding val/15000, so the domain is 255
	// blocks of 15000 bytes and nothing between them.
	PREF_U32_DOMAIN("advanced", "file_buffer_bytes", EC_TAG_CORETW_FILEBUFFER, 0u, 3825000u, 15000u, PrefAccess::ReadWrite, advanced.file_buffer_bytes),
	// LoadAllItems() clamps this to 5..50 on the next start, so anything else
	// was a value GET reported until the daemon was restarted.
	PREF_U32_DOMAIN("advanced", "kad_max_concurrent_source_search_count", EC_TAG_CORETW_KAD_MAX_SEARCHES, 5u, 50u, 0u, PrefAccess::ReadWrite, advanced.kad_max_concurrent_source_search_count),
	PREF_U32_SCALED_DOMAIN("advanced", "kad_source_reask_minutes", EC_TAG_CORETW_KAD_REASK_MS, 30u, 60u, PrefAccess::ReadWrite, advanced.kad_source_reask_minutes, 60000u),
	// s_MaxConperFive is a uint16; 70000 wrapped to 4464.
	PREF_U32_DOMAIN("advanced", "max_new_connections_per_5_seconds", EC_TAG_CORETW_MAX_CONN_PER_FIVE, 0u, 65535u, 0u, PrefAccess::ReadWrite, advanced.max_new_connections_per_5_seconds),
	// s_iQueueSize is a uint8 holding val/100: 255 hundreds, nothing between.
	PREF_U32_DOMAIN("advanced", "max_upload_queue_client_count", EC_TAG_CORETW_UL_QUEUE, 0u, 25500u, 100u, PrefAccess::ReadWrite, advanced.max_upload_queue_client_count),
	PREF_U32_SCALED("advanced", "server_keepalive_timeout_minutes", EC_TAG_CORETW_SRV_KEEPALIVE_TIMEOUT, 71582u, PrefAccess::ReadWrite, advanced.server_keepalive_timeout_minutes, 60000u),
	// The 15-minute floor is load-bearing: the UDP reask goes out at
	// getter - 20s, and below ~10 min peers auto-ban for reask spam.
	PREF_U32_SCALED_DOMAIN("advanced", "source_reask_minutes", EC_TAG_CORETW_SOURCE_REASK_MS, 15u, 60u, PrefAccess::ReadWrite, advanced.source_reask_minutes, 60000u),
	PREF_BOOL("advanced", "verbose_logging", EC_TAG_CORETW_VERBOSE, PrefEnc::Presence, false, PrefAccess::ReadWrite, advanced.verbose_logging),

	// [kad] (EC group: KADEMLIA)
	PREF_STR("kad", "update_url", EC_TAG_KADEMLIA_UPDATE_URL, PrefAccess::ReadWrite, kad.update_url),

	// [geoip] (EC group: IP2COUNTRY)
	PREF_BOOL("geoip", "auto_update", EC_TAG_IP2COUNTRY_AUTO_UPDATE, PrefEnc::Value, false, PrefAccess::ReadWrite, geoip.auto_update),
	PREF_STR("geoip", "custom_update_url", EC_TAG_IP2COUNTRY_CUSTOM_URL, PrefAccess::ReadWrite, geoip.custom_update_url),
	PREF_BOOL("geoip", "db_loaded", EC_TAG_IP2COUNTRY_DB_LOADED, PrefEnc::Value, false, PrefAccess::ReadOnly, geoip.db_loaded),
	PREF_STR("geoip", "db_path", EC_TAG_IP2COUNTRY_DB_PATH, PrefAccess::ReadOnly, geoip.db_path),
	PREF_BOOL("geoip", "download_in_progress", EC_TAG_IP2COUNTRY_DOWNLOADING, PrefEnc::Value, false, PrefAccess::ReadOnly, geoip.download_in_progress),
	PREF_BOOL("geoip", "enabled", EC_TAG_IP2COUNTRY_ENABLED, PrefEnc::Value, false, PrefAccess::ReadWrite, geoip.enabled),
	PREF_STR("geoip", "last_update_status", EC_TAG_IP2COUNTRY_LAST_RESULT, PrefAccess::ReadOnly, geoip.last_update_status),
	PREF_STR("geoip", "loaded_source", EC_TAG_IP2COUNTRY_LOADED_SOURCE, PrefAccess::ReadOnly, geoip.loaded_source),
	PREF_STR("geoip", "maxmind_license", EC_TAG_IP2COUNTRY_MAXMIND_LICENSE, PrefAccess::ReadWrite, geoip.maxmind_license),
	PREF_ENUM("geoip", "source", EC_TAG_IP2COUNTRY_SOURCE, kIp2CountrySources, PrefAccess::ReadWrite, geoip.source),
	PREF_BOOL("geoip", "supported", EC_TAG_IP2COUNTRY_SUPPORTED, PrefEnc::Value, false, PrefAccess::ReadOnly, geoip.supported),
	// `update_now` was a write-only boolean here; it is POST /geoip/update
	// now (#1189), an action rather than a setting. The row stays as a
	// Rejected one so a client still sending it is told where it went.
	PREF_REJECT("geoip", "update_now"),
};

const PrefCategory kCategories[] = {
	{"general", EC_TAG_PREFS_GENERAL},
	{"connection", EC_TAG_PREFS_CONNECTIONS},
	{"directories", EC_TAG_PREFS_DIRECTORIES},
	{"files", EC_TAG_PREFS_FILES},
	{"servers", EC_TAG_PREFS_SERVERS},
	{"security", EC_TAG_PREFS_SECURITY},
	{"message_filter", EC_TAG_PREFS_MESSAGEFILTER},
	// Both nested remote-control sub-objects pack into the one EC group.
	{"remote_controls.webserver", EC_TAG_PREFS_REMOTECTRL},
	{"remote_controls.amuleapi", EC_TAG_PREFS_REMOTECTRL},
	{"online_signature", EC_TAG_PREFS_ONLINESIG},
	// `advanced`, not `core_tweaks`: there is no such amule.conf section,
	// "core" means nothing to an API consumer, verbose_logging is not a
	// tweak, and the desktop tab holding these settings is called Advanced.
	{"advanced", EC_TAG_PREFS_CORETWEAKS},
	// Everything else in the API says kad.
	{"kad", EC_TAG_PREFS_KADEMLIA},
	// The rest of the API says GeoIP (country_code, /flags/{code}.png).
	{"geoip", EC_TAG_PREFS_IP2COUNTRY},
};

// clang-format on

} // namespace

const PrefField *PrefSchema()
{
	return kSchema;
}

std::size_t PrefSchemaSize()
{
	return sizeof(kSchema) / sizeof(kSchema[0]);
}

const PrefCategory *PrefCategories()
{
	return kCategories;
}

std::size_t PrefCategoryCount()
{
	return sizeof(kCategories) / sizeof(kCategories[0]);
}

ec_tagname_t PrefGroupTagFor(const char *category)
{
	for (std::size_t i = 0; i < PrefCategoryCount(); ++i) {
		if (std::strcmp(kCategories[i].name, category) == 0)
			return kCategories[i].group_tag;
	}
	return 0;
}

} // namespace webapi

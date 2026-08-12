//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef CLIENTCREDITS_H
#define CLIENTCREDITS_H

#include "MD4Hash.h" // Needed for CMD4Hash

#define MAXPUBKEYSIZE 80

#define CRYPT_CIP_REMOTECLIENT 10
#define CRYPT_CIP_LOCALCLIENT 20
#define CRYPT_CIP_NONECLIENT 30

class CreditStruct
{
public:
	CreditStruct();

	CMD4Hash key;
	uint64 uploaded;   // uploaded TO him
	uint64 downloaded; // downloaded from him
	uint32 nLastSeen;  // uint32 stores seconds on disk, shall be good until Y2106
	uint16 nReserved3;
	uint8 nKeySize;
	uint8_t abySecureIdent[MAXPUBKEYSIZE];
};

/**
 * What we remember about a peer beyond its credits, for the clients history.
 *
 * Deliberately separate from CreditStruct: that one mirrors the fixed 119-byte
 * on-disk record that aMule has always written and that eMule also reads, and
 * it must stay exactly as it is. This travels in a trailer appended after those
 * records, which older readers never look at (they consume `count` records and
 * stop), so adding fields here cannot cost anyone their credit history.
 *
 * Every field is a last-known value, captured while the peer was connected.
 * Nothing here is authoritative -- a peer can change name, address or client
 * between sessions -- so it is for describing who someone was, not for
 * identifying them. The hash remains the identity.
 */
class ClientMetaStruct
{
public:
	wxString name;    //!< last nickname seen, capped on write
	uint32 firstSeen; //!< first time we ever saw this peer
	uint32 sessions;  //!< how many times we have seen it since
	uint32 lastIP;    //!< last known address, also feeds GeoIP at display time
	uint16 lastPort;
	uint16 kadPort;
	uint32 version;    //!< numeric client version, rendered as a string
	uint8 clientSoft;  //!< eMule / aMule / MLDonkey / ...
	uint8 sourceFrom;  //!< server, kad, passive, incoming
	uint8 obfuscation; //!< obfuscation support/status

	ClientMetaStruct()
	: firstSeen(0)
	, sessions(0)
	, lastIP(0)
	, lastPort(0)
	, kadPort(0)
	, version(0)
	, clientSoft(0)
	, sourceFrom(0)
	, obfuscation(0)
	{
	}

	//! Nothing worth persisting until we have actually met the peer once.
	bool IsPopulated() const { return firstSeen != 0; }
};

enum EIdentState
{
	IS_NOTAVAILABLE,
	IS_IDNEEDED,
	IS_IDENTIFIED,
	IS_IDFAILED,
	IS_IDBADGUY
};

class CClientCredits
{

public:
	CClientCredits(CreditStruct *in_credits);
	CClientCredits(const CMD4Hash &key);
	~CClientCredits();

	const CMD4Hash &GetKey() const { return m_pCredits->key; }
	const uint8_t *GetSecureIdent() const { return m_abyPublicKey; }
	uint8 GetSecIDKeyLen() const { return m_nPublicKeyLen; }
	const CreditStruct *GetDataStruct() const { return m_pCredits; }
	void ClearWaitStartTime();
	void AddDownloaded(uint32 bytes, uint32 dwForIP, bool cryptoavail);
	void AddUploaded(uint32 bytes, uint32 dwForIP, bool cryptoavail);
	uint64 GetUploadedTotal() const;
	uint64 GetDownloadedTotal() const;
	float GetScoreRatio(uint32 dwForIP, bool cryptoavail);
	void SetLastSeen();
	bool SetSecureIdent(const uint8_t *pachIdent,
		uint8 nIdentLen); // Public key cannot change, use only if there is not public key yet
	uint32 m_dwCryptRndChallengeFor;
	uint32 m_dwCryptRndChallengeFrom;
	EIdentState GetCurrentIdentState(uint32 dwForIP) const; // can be != m_identState
	uint64 GetSecureWaitStartTime(uint32 dwForIP);
	void SetSecWaitStartTime(uint32 dwForIP);
	void Verified(uint32 dwForIP);
	EIdentState GetIdentState() const { return m_identState; }
	void SetIdentState(EIdentState state) { m_identState = state; }

	const ClientMetaStruct &GetMeta() const { return m_meta; }
	bool HasMeta() const { return m_meta.IsPopulated(); }
	//! Load-time restore from the clients.met trailer.
	void SetMeta(const ClientMetaStruct &meta) { m_meta = meta; }
	/**
	 * Record what a connected peer looks like right now.
	 *
	 * `countSession` is the caller's answer to "is this a new sighting?" --
	 * the handshake can be processed more than once for one connection, and
	 * a session count that grows per packet would say nothing.
	 */
	void UpdateMeta(const wxString &name,
		uint32 ip,
		uint16 port,
		uint16 kadPort,
		uint32 version,
		uint8 clientSoft,
		uint8 sourceFrom,
		uint8 obfuscation,
		bool countSession);

private:
	ClientMetaStruct m_meta;
	EIdentState m_identState;
	void InitalizeIdent();
	CreditStruct *m_pCredits;
	uint8_t m_abyPublicKey[80]; // even keys which are not verified will be stored here, and - if verified
				    // - copied into the struct
	uint8 m_nPublicKeyLen;
	uint32 m_dwIdentIP;
	uint64 m_dwSecureWaitTime;
	uint64 m_dwUnSecureWaitTime;
	uint32 m_dwWaitTimeIP; // client IP assigned to the waittime
};

#endif // CLIENTCREDITS_H
// File_checked_for_headers

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

#include "ClientCreditsList.h" // Interface declarations

#include <protocol/ed2k/Constants.h>
#include <common/Macros.h>
#include <common/DataFileVersion.h>
#include <common/FileFunctions.h> // Needed for GetFileSize

#include "GetTickCount.h"  // Needed for GetTickCount64
#include "Preferences.h"   // Needed for thePrefs
#include "ClientCredits.h" // Needed for CClientCredits
#include "amule.h"         // Needed for theApp
#include "CFile.h"         // Needed for CFile
#include "Logger.h"        // Needed for Add(Debug)LogLine
#include "CryptoPP_Inc.h"  // Needed for Crypto functions

#define CLIENTS_MET_FILENAME "clients.met"
#define CLIENTS_MET_BAK_FILENAME "clients.met.bak"
#define CRYPTKEY_FILENAME "cryptkey.dat"

CClientCreditsList::CClientCreditsList()
{
	m_nLastSaved = ::GetTickCount64();
	LoadList();

	InitalizeCrypting();
}

CClientCreditsList::~CClientCreditsList()
{
	DeleteContents(m_mapClients);
	delete static_cast<CryptoPP::RSASSA_PKCS1v15_SHA_Signer *>(m_pSignkey);
}

namespace
{
/**
 * The clients.met metadata trailer.
 *
 * Appended after the fixed 119-byte credit records rather than folded into
 * them, and the file version stays at CREDITFILE_VERSION on purpose. Bumping
 * that version is what would break compatibility, not adding data: an older
 * aMule meeting a version it does not know logs "Creditfile is outdated and
 * will be replaced" and returns without loading a single record, and the next
 * save then overwrites the file -- every credit gone, on nothing worse than
 * running a previous build once. The same byte is also the format shared with
 * the eMule lineage.
 *
 * The loader reads exactly `count` records and stops; it never checks the
 * file length and never looks further. So anything after the last record is
 * invisible to every reader that predates this, and they keep loading the
 * credits they always did. The cost is that such a reader's *save* drops the
 * trailer, losing the metadata but never the credits.
 *
 * The magic makes presence unambiguous, since absence is the normal state for
 * a file last written by an older build.
 */
const char kMetaMagic[8] = { 'A', 'M', 'U', 'L', 'E', 'M', 'D', '1' };
const uint8 kMetaVersion = 1;
//! Names are attacker-supplied; cap what we store rather than what we read.
//! Counted in characters, because that is what wxString::Left() takes -- a
//! multi-byte name can therefore occupy more than this many bytes on disk,
//! which the uint16 length field has ample room for. The cap is storage
//! sanity, not a bound the format depends on.
const size_t kMetaMaxNameChars = 64;
} // namespace

void CClientCreditsList::LoadList()
{
	CFile file;
	CPath fileName = CPath(thePrefs::GetConfigDir() + CLIENTS_MET_FILENAME);

	if (!fileName.FileExists()) {
		return;
	}

	try {
		file.Open(fileName, CFile::read);

		if (file.ReadUInt8() != CREDITFILE_VERSION) {
			AddDebugLogLineC(logCredits, "Creditfile is outdated and will be replaced");
			file.Close();
			return;
		}

		// everything is ok, lets see if the backup exist...
		CPath bakFileName = CPath(thePrefs::GetConfigDir() + CLIENTS_MET_BAK_FILENAME);

		bool bCreateBackup = TRUE;
		if (bakFileName.FileExists()) {
			// Ok, the backup exist, get the size
			CFile hBakFile(bakFileName);
			if (hBakFile.GetLength() > file.GetLength()) {
				// the size of the backup was larger then the
				// org. file, something is wrong here, don't
				// overwrite old backup..
				bCreateBackup = FALSE;
			}
			// else: backup is smaller or the same size as org.
			// file, proceed with copying of file
		}

		// else: the backup doesn't exist, create it
		if (bCreateBackup) {
			file.Close(); // close the file before copying
			// Small same-directory config backup (clients.met ->
			// clients.met.bak); the plain wxCopyFile path is the right
			// fit, not the buffered CFile::CloneFile used for large,
			// possibly cross-filesystem data-file copies.
			if (!CPath::BackupFile(fileName, wxT(".bak"))) {
				AddDebugLogLineC(
					logCredits, CFormat("Could not create backup file '%s'") % fileName);
			}
			// reopen file
			if (!file.Open(fileName, CFile::read)) {
				AddDebugLogLineC(logCredits, "Failed to load creditfile");
				return;
			}

			file.Seek(1);
		}

		uint32 count = file.ReadUInt32();

		const uint32 dwExpired = time(NULL) - 12960000; // today - 150 day
		uint32 cDeleted = 0;
		for (uint32 i = 0; i < count; i++) {
			CreditStruct *newcstruct = new CreditStruct();

			newcstruct->key = file.ReadHash();
			newcstruct->uploaded = file.ReadUInt32();
			newcstruct->downloaded = file.ReadUInt32();
			newcstruct->nLastSeen = file.ReadUInt32();
			newcstruct->uploaded += static_cast<uint64>(file.ReadUInt32()) << 32;
			newcstruct->downloaded += static_cast<uint64>(file.ReadUInt32()) << 32;
			newcstruct->nReserved3 = file.ReadUInt16();
			newcstruct->nKeySize = file.ReadUInt8();
			file.Read(newcstruct->abySecureIdent, MAXPUBKEYSIZE);

			if (newcstruct->nKeySize > MAXPUBKEYSIZE) {
				// Oh dear, this is bad mojo, the file is most likely corrupt
				// We can no longer assume that any of the clients in the file are valid
				// and will have to discard it.
				delete newcstruct;

				DeleteContents(m_mapClients);

				AddDebugLogLineC(
					logCredits, "WARNING: Corruptions found while reading Creditfile!");
				return;
			}

			if (newcstruct->nLastSeen < dwExpired) {
				cDeleted++;
				delete newcstruct;
				continue;
			}

			CClientCredits *newcredits = new CClientCredits(newcstruct);
			m_mapClients[newcredits->GetKey()] = newcredits;
		}

		AddLogLineN(CFormat(wxPLURAL("Creditfile loaded, %u client is known",
				    "Creditfile loaded, %u clients are known",
				    count - cDeleted)) %
			    (count - cDeleted));

		if (cDeleted) {
			AddLogLineN(CFormat(wxPLURAL(" - Credits expired for %u client!",
					    " - Credits expired for %u clients!",
					    cDeleted)) %
				    cDeleted);
		}

		LoadMetaTrailer(file);
	} catch (const CSafeIOException &e) {
		AddDebugLogLineC(logCredits, "IO error while loading clients.met file: " + e.what());
	}
}

void CClientCreditsList::LoadMetaTrailer(CFile &file)
{
	// Optional by construction: a file written by any older build simply ends
	// after the last record. Everything here is best-effort -- the credits are
	// already loaded and must survive whatever this finds, so a trailer that is
	// absent, truncated or unrecognised is dropped rather than treated as a
	// corrupt file.
	try {
		const uint64 remaining = file.GetLength() - file.GetPosition();
		if (remaining < sizeof(kMetaMagic) + 1 + 4) {
			return;
		}

		char magic[sizeof(kMetaMagic)];
		file.Read(magic, sizeof(magic));
		if (memcmp(magic, kMetaMagic, sizeof(magic)) != 0) {
			AddDebugLogLineN(logCredits, "Trailing data in clients.met is not a metadata block");
			return;
		}
		const uint8 version = file.ReadUInt8();
		if (version != kMetaVersion) {
			// A newer aMule wrote it. Same reasoning as the file version: do
			// not guess at a layout we do not know, just leave the metadata
			// behind. The credits are unaffected either way.
			AddDebugLogLineN(logCredits,
				CFormat("clients.met metadata is version %u, expected %u -- ignoring") %
					version % kMetaVersion);
			return;
		}

		const uint32 entries = file.ReadUInt32();
		uint32 restored = 0;
		for (uint32 i = 0; i < entries; i++) {
			const CMD4Hash key = file.ReadHash();
			ClientMetaStruct meta;
			meta.firstSeen = file.ReadUInt32();
			meta.sessions = file.ReadUInt32();
			meta.lastIP = file.ReadUInt32();
			meta.lastPort = file.ReadUInt16();
			meta.kadPort = file.ReadUInt16();
			meta.version = file.ReadUInt32();
			meta.clientSoft = file.ReadUInt8();
			meta.sourceFrom = file.ReadUInt8();
			meta.obfuscation = file.ReadUInt8();
			meta.name = file.ReadString(true, sizeof(uint16));

			// Entries whose credit record is gone -- expired by the 150-day
			// prune above, or dropped by a save from a build that did not know
			// about them -- describe a client we no longer track.
			ClientMap::iterator it = m_mapClients.find(key);
			if (it != m_mapClients.end()) {
				it->second->SetMeta(meta);
				restored++;
			}
		}
		AddDebugLogLineN(
			logCredits, CFormat("Restored metadata for %u of %u clients") % restored % entries);
	} catch (const CSafeIOException &e) {
		// Truncated or malformed: the credits are already in hand, so this is
		// the end of what we can use, not a failure.
		AddDebugLogLineN(logCredits, "Could not read clients.met metadata: " + e.what());
	}
}

void CClientCreditsList::SaveList()
{
	AddDebugLogLineN(logCredits, "Saved Credit list");
	m_nLastSaved = ::GetTickCount64();

	wxString name(thePrefs::GetConfigDir() + CLIENTS_MET_FILENAME);
	CFile file;

	if (!file.Create(name, true)) {
		AddDebugLogLineC(logCredits, "Failed to create creditfile");
		return;
	}

	if (file.Open(name, CFile::write)) {
		try {
			uint32 count = 0;

			file.WriteUInt8(CREDITFILE_VERSION);
			// Temporary place-holder for number of structs
			file.WriteUInt32(0);

			ClientMap::iterator it = m_mapClients.begin();
			for (; it != m_mapClients.end(); ++it) {
				CClientCredits *cur_credit = it->second;

				if (cur_credit->GetUploadedTotal() || cur_credit->GetDownloadedTotal()) {
					const CreditStruct *const cstruct = cur_credit->GetDataStruct();
					file.WriteHash(cstruct->key);
					file.WriteUInt32(static_cast<uint32>(cstruct->uploaded));
					file.WriteUInt32(static_cast<uint32>(cstruct->downloaded));
					file.WriteUInt32(cstruct->nLastSeen);
					file.WriteUInt32(static_cast<uint32>(cstruct->uploaded >> 32));
					file.WriteUInt32(static_cast<uint32>(cstruct->downloaded >> 32));
					file.WriteUInt16(cstruct->nReserved3);
					file.WriteUInt8(cstruct->nKeySize);
					// Doesn't matter if this saves garbage, will be fixed on load.
					file.Write(cstruct->abySecureIdent, MAXPUBKEYSIZE);
					count++;
				}
			}

			// Write the actual number of structs
			file.Seek(1);
			file.WriteUInt32(count);

			// Append the metadata after the records, where older readers
			// never look. Seek to the end explicitly: the count fixup above
			// left the position at byte 5.
			file.Seek(0, wxFromEnd);
			SaveMetaTrailer(file);
		} catch (const CIOFailureException &e) {
			AddDebugLogLineC(logCredits, "IO failure while saving clients.met: " + e.what());
		}
	} else {
		AddDebugLogLineC(logCredits, "Failed to open existing creditfile!");
	}
}

void CClientCreditsList::SaveMetaTrailer(CFile &file)
{
	// Only clients we have actually met since this existed carry metadata, so
	// on a file inherited from an older build this writes nothing at all and
	// the result stays byte-identical to what that build produced.
	uint32 entries = 0;
	for (const auto &entry : m_mapClients) {
		const CClientCredits *cur = entry.second;
		// Mirrors the record loop's own filter: a client with no traffic is
		// not written above, so metadata for it would have no record to
		// attach to on the next load.
		if (cur->HasMeta() && (cur->GetUploadedTotal() || cur->GetDownloadedTotal())) {
			entries++;
		}
	}
	if (entries == 0) {
		return;
	}

	try {
		file.Write(kMetaMagic, sizeof(kMetaMagic));
		file.WriteUInt8(kMetaVersion);
		file.WriteUInt32(entries);

		for (const auto &entry : m_mapClients) {
			CClientCredits *cur = entry.second;
			if (!cur->HasMeta() || !(cur->GetUploadedTotal() || cur->GetDownloadedTotal())) {
				continue;
			}
			const ClientMetaStruct &meta = cur->GetMeta();
			file.WriteHash(cur->GetKey());
			file.WriteUInt32(meta.firstSeen);
			file.WriteUInt32(meta.sessions);
			file.WriteUInt32(meta.lastIP);
			file.WriteUInt16(meta.lastPort);
			file.WriteUInt16(meta.kadPort);
			file.WriteUInt32(meta.version);
			file.WriteUInt8(meta.clientSoft);
			file.WriteUInt8(meta.sourceFrom);
			file.WriteUInt8(meta.obfuscation);
			// Two length bytes because CFileDataIO supports 0, 2 or 4 and
			// rejects anything else -- a 1-byte prefix throws
			// "Invalid length for string-length field" from inside the
			// write, which the handler below would then swallow, leaving a
			// trailer truncated mid-entry. The cap is enforced here, not by
			// the field width.
			file.WriteString(meta.name.Left(kMetaMaxNameChars), utf8strRaw, sizeof(uint16));
		}
	} catch (const CIOFailureException &e) {
		// The records are already on disk and intact; losing the trailer costs
		// the metadata only, which is why it is written last.
		AddDebugLogLineC(logCredits, "IO failure while saving clients.met metadata: " + e.what());
	}
}

void CClientCreditsList::GetAllCredits(std::vector<CClientCredits *> &result) const
{
	result.clear();
	result.reserve(m_mapClients.size());
	for (const auto &entry : m_mapClients) {
		result.push_back(entry.second);
	}
}

CClientCredits *CClientCreditsList::GetCredit(const CMD4Hash &key)
{
	CClientCredits *result;

	ClientMap::iterator it = m_mapClients.find(key);

	if (it == m_mapClients.end()) {
		result = new CClientCredits(key);
		m_mapClients[result->GetKey()] = result;
	} else {
		result = it->second;
	}

	result->SetLastSeen();

	return result;
}

void CClientCreditsList::Process()
{
	if (::GetTickCount64() - m_nLastSaved > MIN2MS(13))
		SaveList();
}

bool CClientCreditsList::CreateKeyPair()
{
	try {
		CryptoPP::AutoSeededX917RNG<CryptoPP::DES_EDE3> rng;
		CryptoPP::InvertibleRSAFunction privkey;
		privkey.Initialize(rng, RSAKEYSIZE);

		// Nothing we can do against this filename2char :/
		wxCharBuffer filename = filename2char(thePrefs::GetConfigDir() + CRYPTKEY_FILENAME);
		CryptoPP::FileSink *fileSink = new CryptoPP::FileSink(filename);
		CryptoPP::Base64Encoder *privkeysink = new CryptoPP::Base64Encoder(fileSink);
		privkey.DEREncode(*privkeysink);
		privkeysink->MessageEnd();

		// Do not delete these pointers or it will blow in your face.
		// cryptopp semantics is giving ownership of these objects.
		//
		// delete privkeysink;
		// delete fileSink;

		AddDebugLogLineN(logCredits, "Created new RSA keypair");
	} catch (const CryptoPP::Exception &e) {
		AddDebugLogLineC(logCredits,
			wxString("Failed to create new RSA keypair: ") + wxString(char2unicode(e.what())));
		wxFAIL;
		return false;
	}

	return true;
}

void CClientCreditsList::InitalizeCrypting()
{
	m_nMyPublicKeyLen = 0;
	memset(m_abyMyPublicKey, 0, 80); // not really needed; better for debugging tho
	m_pSignkey = NULL;

	if (!thePrefs::IsSecureIdentEnabled()) {
		return;
	}

	try {
		// check if keyfile is there
		if (wxFileExists(thePrefs::GetConfigDir() + CRYPTKEY_FILENAME)) {
			off_t keySize = CPath::GetFileSize(thePrefs::GetConfigDir() + CRYPTKEY_FILENAME);

			if (keySize == wxInvalidOffset) {
				AddDebugLogLineC(logCredits,
					"Cannot access 'cryptkey.dat', please check permissions.");
				return;
			} else if (keySize == 0) {
				AddDebugLogLineC(logCredits, "'cryptkey.dat' is empty, recreating keypair.");
				CreateKeyPair();
			}
		} else {
			AddLogLineN(_("No 'cryptkey.dat' file found, creating."));
			CreateKeyPair();
		}

		// load private key
		CryptoPP::FileSource filesource(filename2char(thePrefs::GetConfigDir() + CRYPTKEY_FILENAME),
			true,
			new CryptoPP::Base64Decoder);
		m_pSignkey = new CryptoPP::RSASSA_PKCS1v15_SHA_Signer(filesource);
		// calculate and store public key
		CryptoPP::RSASSA_PKCS1v15_SHA_Verifier pubkey(
			*static_cast<CryptoPP::RSASSA_PKCS1v15_SHA_Signer *>(m_pSignkey));
		CryptoPP::ArraySink asink(m_abyMyPublicKey, 80);
		pubkey.GetMaterial().Save(asink);
		m_nMyPublicKeyLen = asink.TotalPutLength();
		asink.MessageEnd();
	} catch (const CryptoPP::Exception &e) {
		delete static_cast<CryptoPP::RSASSA_PKCS1v15_SHA_Signer *>(m_pSignkey);
		m_pSignkey = NULL;

		AddDebugLogLineC(logCredits,
			wxString("Error while initializing encryption keys: ") +
				wxString(char2unicode(e.what())));
	}
}

uint8 CClientCreditsList::CreateSignature(CClientCredits *pTarget,
	uint8_t *pachOutput,
	uint8 nMaxSize,
	uint32 ChallengeIP,
	uint8 byChaIPKind,
	void *sigkey)
{
	CryptoPP::RSASSA_PKCS1v15_SHA_Signer *signer =
		static_cast<CryptoPP::RSASSA_PKCS1v15_SHA_Signer *>(sigkey);
	// signer param is used for debug only
	if (signer == NULL)
		signer = static_cast<CryptoPP::RSASSA_PKCS1v15_SHA_Signer *>(m_pSignkey);

	// create a signature of the public key from pTarget
	wxASSERT(pTarget);
	wxASSERT(pachOutput);

	if (!CryptoAvailable()) {
		return 0;
	}

	try {
		CryptoPP::SecByteBlock sbbSignature(signer->SignatureLength());
		CryptoPP::AutoSeededX917RNG<CryptoPP::DES_EDE3> rng;
		uint8_t abyBuffer[MAXPUBKEYSIZE + 9];
		uint32 keylen = pTarget->GetSecIDKeyLen();
		memcpy(abyBuffer, pTarget->GetSecureIdent(), keylen);
		// 4 additional bytes random data send from this client
		uint32 challenge = pTarget->m_dwCryptRndChallengeFrom;
		wxASSERT(challenge != 0);
		PokeUInt32(abyBuffer + keylen, challenge);

		uint16 ChIpLen = 0;
		if (byChaIPKind != 0) {
			ChIpLen = 5;
			PokeUInt32(abyBuffer + keylen + 4, ChallengeIP);
			PokeUInt8(abyBuffer + keylen + 4 + 4, byChaIPKind);
		}
		signer->SignMessage(rng, abyBuffer, keylen + 4 + ChIpLen, sbbSignature.begin());
		CryptoPP::ArraySink asink(pachOutput, nMaxSize);
		asink.Put(sbbSignature.begin(), sbbSignature.size());

		return asink.TotalPutLength();
	} catch (const CryptoPP::Exception &e) {
		AddDebugLogLineC(logCredits,
			wxString("Error while creating signature: ") + wxString(char2unicode(e.what())));
		wxFAIL;

		return 0;
	}
}

bool CClientCreditsList::VerifyIdent(CClientCredits *pTarget,
	const uint8_t *pachSignature,
	uint8 nInputSize,
	uint32 dwForIP,
	uint8 byChaIPKind)
{
	wxASSERT(pTarget);
	wxASSERT(pachSignature);
	if (!CryptoAvailable()) {
		pTarget->SetIdentState(IS_NOTAVAILABLE);
		return false;
	}
	bool bResult;
	try {
		CryptoPP::StringSource ss_Pubkey(
			(uint8_t *)pTarget->GetSecureIdent(), pTarget->GetSecIDKeyLen(), true, 0);
		CryptoPP::RSASSA_PKCS1v15_SHA_Verifier pubkey(ss_Pubkey);
		// 4 additional bytes random data send from this client +5 bytes v2
		uint8_t abyBuffer[MAXPUBKEYSIZE + 9];
		memcpy(abyBuffer, m_abyMyPublicKey, m_nMyPublicKeyLen);
		uint32 challenge = pTarget->m_dwCryptRndChallengeFor;
		wxASSERT(challenge != 0);
		PokeUInt32(abyBuffer + m_nMyPublicKeyLen, challenge);

		// v2 security improvements (not supported by 29b, not used as default by 29c)
		uint8 nChIpSize = 0;
		if (byChaIPKind != 0) {
			nChIpSize = 5;
			uint32 ChallengeIP = 0;
			switch (byChaIPKind) {
			case CRYPT_CIP_LOCALCLIENT:
				ChallengeIP = dwForIP;
				break;
			case CRYPT_CIP_REMOTECLIENT:
				// Ignore local ip...
				if (!theApp->GetPublicIP(true)) {
					if (::IsLowID(theApp->GetED2KID())) {
						AddDebugLogLineN(logCredits,
							"Warning: Maybe SecureHash Ident fails because "
							"LocalIP is unknown");
						// Fallback to local ip...
						ChallengeIP = theApp->GetPublicIP();
					} else {
						ChallengeIP = theApp->GetED2KID();
					}
				} else {
					ChallengeIP = theApp->GetPublicIP();
				}
				break;
			case CRYPT_CIP_NONECLIENT: // maybe not supported in future versions
				ChallengeIP = 0;
				break;
			}
			PokeUInt32(abyBuffer + m_nMyPublicKeyLen + 4, ChallengeIP);
			PokeUInt8(abyBuffer + m_nMyPublicKeyLen + 4 + 4, byChaIPKind);
		}
		// v2 end

		bResult = pubkey.VerifyMessage(
			abyBuffer, m_nMyPublicKeyLen + 4 + nChIpSize, pachSignature, nInputSize);
	} catch (const CryptoPP::Exception &e) {
		AddDebugLogLineC(logCredits,
			wxString("Error while verifying identity: ") + wxString(char2unicode(e.what())));
		bResult = false;
	}

	if (!bResult) {
		if (pTarget->GetIdentState() == IS_IDNEEDED)
			pTarget->SetIdentState(IS_IDFAILED);
	} else {
		pTarget->Verified(dwForIP);
	}

	return bResult;
}

bool CClientCreditsList::CryptoAvailable() const
{
	return m_nMyPublicKeyLen > 0 && m_pSignkey != NULL;
}

#ifdef _DEBUG
bool CClientCreditsList::Debug_CheckCrypting()
{
	// create random key
	CryptoPP::AutoSeededX917RNG<CryptoPP::DES_EDE3> rng;

	CryptoPP::RSASSA_PKCS1v15_SHA_Signer priv(rng, 384);
	CryptoPP::RSASSA_PKCS1v15_SHA_Verifier pub(priv);

	uint8_t abyPublicKey[80];
	CryptoPP::ArraySink asink(abyPublicKey, 80);
	pub.DEREncode(asink);
	int8 PublicKeyLen = asink.TotalPutLength();
	asink.MessageEnd();
	uint32 challenge = rand();
	// create fake client which pretends to be this emule
	CreditStruct *newcstruct = new CreditStruct();
	CClientCredits newcredits(newcstruct);
	newcredits.SetSecureIdent(m_abyMyPublicKey, m_nMyPublicKeyLen);
	newcredits.m_dwCryptRndChallengeFrom = challenge;
	// create signature with fake priv key
	uint8_t pachSignature[200];
	memset(pachSignature, 0, 200);
	uint8 sigsize = CreateSignature(&newcredits, pachSignature, 200, 0, false, &priv);

	// next fake client uses the random created public key
	CreditStruct *newcstruct2 = new CreditStruct();
	CClientCredits newcredits2(newcstruct2);
	newcredits2.m_dwCryptRndChallengeFor = challenge;

	// if you uncomment one of the following lines the check has to fail
	// abyPublicKey[5] = 34;
	// m_abyMyPublicKey[5] = 22;
	// pachSignature[5] = 232;

	newcredits2.SetSecureIdent(abyPublicKey, PublicKeyLen);

	// now verify this signature - if it's true everything is fine
	return VerifyIdent(&newcredits2, pachSignature, sigsize, 0, 0);
}
#endif
// File_checked_for_headers

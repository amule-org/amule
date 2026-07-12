if (BUILD_MONOLITHIC OR BUILD_DAEMON)
	set (CORE_SOURCES
		kademlia/kademlia/Kademlia.cpp
		kademlia/kademlia/Prefs.cpp
		kademlia/kademlia/Search.cpp
		kademlia/kademlia/UDPFirewallTester.cpp
		kademlia/net/KademliaUDPListener.cpp
		kademlia/net/PacketTracking.cpp
		kademlia/routing/Contact.cpp
		kademlia/routing/RoutingZone.cpp
		amule.cpp
		BaseClient.cpp
		ClientCreditsList.cpp
		ClientList.cpp
		ClientTCPSocket.cpp
		ClientUDPSocket.cpp
		CorruptionBlackBox.cpp
		DownloadBandwidthThrottler.cpp
		DownloadClient.cpp
		DownloadQueue.cpp
		ECFullResponseCache.cpp
		ECSpecialCoreTags.cpp
		EMSocket.cpp
		EncryptedStreamSocket.cpp
		EncryptedDatagramSocket.cpp
		ExternalConn.cpp
		FirstRunWizard.cpp
		FriendList.cpp
		IPFilter.cpp
		KnownFileList.cpp
		ListenSocket.cpp
		MuleUDPSocket.cpp
		SearchFile.cpp
		SearchList.cpp
		ServerConnect.cpp
		ServerList.cpp
		ServerSocket.cpp
		ServerUDPSocket.cpp
		SHAHashSet.cpp
		SharedDirWatcher.cpp
		SharedFileList.cpp
		UploadBandwidthThrottler.cpp
		UploadClient.cpp
		UploadDiskIOThread.cpp
		UploadQueue.cpp
		PartFileWriteThread.cpp
		PartFileHashThread.cpp
		MediaProbeThread.cpp
		ThreadTasks.cpp
	)
endif()

if (BUILD_MONOLITHIC OR BUILD_REMOTEGUI)
	set (GUI_SOURCES
		# wxArtProvider subclass + the C TU it pulls icon bytes
		# from. ${AMULE_ICON_DATA_C} resolves to either the build-
		# generated copy (Python3 found at configure → regenerated
		# from src/icons/*.png by src/icons/embed_icons.py) or the
		# checked-in fallback (Python3 missing → use the file as
		# committed). See src/CMakeLists.txt for the resolution.
		CamuleArtProvider.cpp
		${AMULE_ICON_DATA_C}
		AddFriend.cpp
		amule-gui.cpp
		AppImageIntegration.cpp
		amuleDlg.cpp
		AboutDialog.cpp
		VersionCheck.cpp
		CatDialog.cpp
		ChatSelector.cpp
		ChatWnd.cpp
		ClientDetailDialog.cpp
		CommentDialog.cpp
		CommentDialogLst.cpp
		DirectoryTreeCtrl.cpp
		DownloadListCtrl.cpp
		FileDetailDialog.cpp
		FriendListCtrl.cpp
		GenericClientListCtrl.cpp
		KadDlg.cpp
		MuleTrayIcon.cpp
		OScopeCtrl.cpp
		PrefsUnifiedDlg.cpp
		SearchDlg.cpp
		SearchListCtrl.cpp
		ServerListCtrl.cpp
		ServerWnd.cpp
		SharedDirsApplyTask.cpp
		SharedFilePeersListCtrl.cpp
		SharedFilesCtrl.cpp
		SharedFilesWnd.cpp
		SourceListCtrl.cpp
		StatisticsDlg.cpp
		TransferWnd.cpp
	)

	if (APPLE)
		# Obj-C++ helper for AppKit access (NSApp activation policy
		# toggle for "minimize to tray" — drops the Dock icon while
		# the main window is hidden so no Dock thumbnail is left).
		list (APPEND GUI_SOURCES MacAppHelper.mm)
	endif()
endif()

if (BUILD_MONOLITHIC OR BUILD_DAEMON OR BUILD_REMOTEGUI)
	set (COMMON_SOURCES
		amuleAppCommon.cpp
		AppImageEnv.cpp
		AutostartManager.cpp
		ProtocolHandlerManager.cpp
		$<$<BOOL:${APPLE}>:ProtocolHandlerManager_mac.mm>
		ClientRef.cpp
		ECSpecialMuleTags.cpp
		GetTickCount.cpp
		GuiEvents.cpp
		HTTPDownload.cpp
		InstanceLock.cpp
		KnownFile.cpp
		Logger.cpp
		MediaProbe.cpp
		PartFile.cpp
		Preferences.cpp
		Proxy.cpp
		Server.cpp
		Statistics.cpp
		StatTree.cpp
		UserEvents.cpp
	)
endif()

# IP2Country.cpp compiles unconditionally: when ENABLE_IP2COUNTRY is off its
# #else branch provides no-op stubs, so core code (CamuleApp owns the resolver
# for the #439/#440 EC tags) can reference CIP2Country without sprinkling
# ENABLE_IP2COUNTRY guards over every call site + the dtor. Only the MaxMind DB
# reader — and its libmaxminddb link (see src/CMakeLists.txt) — is gated on the
# feature; the stub needs neither.
set (IP2COUNTRY IP2Country.cpp)
if (ENABLE_IP2COUNTRY)
	list (APPEND IP2COUNTRY geoip/MaxMindDBDatabase.cpp)
endif()

if (ENABLE_UPNP)
	set (UPNP_SOURCES ${CMAKE_SOURCE_DIR}/src/UPnPBase.cpp)
endif()

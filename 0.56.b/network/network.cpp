#include "network.h"
#include "send.h"
#include "receive.h"

#ifndef _WIN32
	extern LinuxEv g_NetworkEvent;
#endif

NetworkIn g_NetworkIn;
NetworkOut g_NetworkOut;

//
// Packet logging
//
#if _PACKETDUMP || _DUMPSUPPORT

void xRecordPacketData(CClient* client, const BYTE* data, int length, LPCTSTR heading)
{
#ifdef _DUMPSUPPORT
	if (client->GetAccount() != NULL && strnicmp(client->GetAccount()->GetName(), (LPCTSTR) g_Cfg.m_sDumpAccPackets, strlen( client->GetAccount()->GetName())))
		return;
#else
	if (!(g_Cfg.m_wDebugFlags & DEBUGF_PACKETS))
		return;
#endif

	Packet packet(data, length);
	xRecordPacket(client, &packet, heading);
}

void xRecordPacket(CClient* client, Packet* packet, LPCTSTR heading)
{
#ifdef _DUMPSUPPORT
	if (client->GetAccount() != NULL && strnicmp(client->GetAccount()->GetName(), (LPCTSTR) g_Cfg.m_sDumpAccPackets, strlen( client->GetAccount()->GetName())))
		return;
#else
	if (!(g_Cfg.m_wDebugFlags & DEBUGF_PACKETS))
		return;
#endif

	TCHAR* dump = packet->dump();

#ifdef _DEBUG
	// write to console
	g_Log.EventDebug("%s %s\n", heading, dump);
#endif

	// build file name
	TCHAR fname[64];
	strcpy(fname, "packets_");
	if (client->GetAccount())
		strcat(fname, client->GetAccount()->GetName());
	else
	{
		strcat(fname, "(");
		strcat(fname, client->GetPeerStr());
		strcat(fname, ")");
	}

	strcat(fname, ".log");

	CGString sFullFileName = CGFile::GetMergedFileName(g_Log.GetLogDir(), fname);
	
	// write to file
	CFileText out;
	if (out.Open(sFullFileName, OF_READWRITE|OF_TEXT))
	{
		out.Printf("%s %s\n\n", heading, dump);
		out.Close();
	}
}
#endif


/***************************************************************************
 *
 *
 *	class NetState				Network status (client info, etc)
 *
 *
 ***************************************************************************/

NetState::NetState(long id) : m_id(id), m_client(NULL), m_clientType(CLIENTTYPE_2D), m_clientVersion(0), m_reportedVersion(0), m_useAsync(false), m_packetExceptions(0)
{
#ifdef NETWORK_MULTITHREADED
	m_queueLock.setMutex(&m_queueMutex);
	m_closeLock.setMutex(&m_closeMutex);
#endif

	clear();
}

NetState::~NetState(void)
{
}

void NetState::clear(void)
{
	if (m_client != NULL)
	{
		g_Serv.StatDec(SERV_STAT_CLIENTS);
		g_Log.Event(LOGM_CLIENTS_LOG, "%x:Client disconnected [Total:%d] ('%s')\n",
			m_id, g_Serv.StatGet(SERV_STAT_CLIENTS), m_peerAddress.GetAddrStr());

		if (m_socket.IsOpen() && g_Cfg.m_fUseAsyncNetwork)
		{
#ifndef _WIN32
			g_NetworkEvent.unregisterClient(m_client);
#else
			m_socket.ClearAsync();
#endif
		}

		//	record the client reference to the garbage collection to be deleted on it's time
		g_World.m_ObjDelete.InsertHead(m_client);
	}

#ifdef NETWORK_MULTITHREADED
	m_isReady = false;
#endif

	m_isClosed = true;
	m_socket.Close();
	m_client = NULL;

#ifdef NETWORK_MULTITHREADED
	m_queueLock.doLock();
#endif

	for (int i = 0; i < PacketSend::PRI_QTY; i++)
	{
		while (m_queue[i].size())
		{
			delete m_queue[i].front();
			m_queue[i].pop();
		}

#ifdef NETWORK_MULTITHREADED
		while (m_holdingQueue[i].size())
		{
			delete m_holdingQueue[i].front();
			m_holdingQueue[i].pop();
		}
#endif
	}
	
#ifdef NETWORK_MULTITHREADED
	m_queueLock.doUnlock();
#endif

	while (m_asyncQueue.size())
	{
		delete m_asyncQueue.front();
		m_asyncQueue.pop();
	}

	m_sequence = 0;
	m_seeded = false;
	m_newseed = false;
	m_seed = 0;
	m_clientVersion = m_reportedVersion = 0;
	m_clientType = CLIENTTYPE_2D;
	m_isSendingAsync = false;
	m_useAsync = false;
	m_packetExceptions = 0;
}

void NetState::init(SOCKET socket, CSocketAddress addr)
{
	m_peerAddress = addr;

	if (m_client != NULL)
	{
		//	record the client reference to the garbage collection to be deleted on it's time
		g_World.m_ObjDelete.InsertHead(m_client);
	}

	clear();

	g_Serv.StatInc(SERV_STAT_CLIENTS);
	m_socket.SetSocket(socket);
	m_client = new CClient(this);
	m_socket.SetNonBlocking();

	// disable NAGLE algorythm for data compression/coalescing.
	// Send as fast as we can. we handle packing ourselves.
	BOOL nbool = true;
	m_socket.SetSockOpt(TCP_NODELAY, &nbool, sizeof(BOOL), IPPROTO_TCP);

	setAsyncMode();
	
#ifndef _WIN32
	if (g_Cfg.m_fUseAsyncNetwork)
		g_NetworkEvent.registerClient(m_client, LinuxEv::Write);
#endif

	m_isClosed = false;
#ifdef NETWORK_MULTITHREADED
	m_isReady = true;
#endif
}

bool NetState::isValid(const CClient* client) const
{
#ifdef NETWORK_MULTITHREADED
	if (m_isReady == false)
		return false;
#endif
	if (client == NULL)
		return m_socket.IsOpen() && !m_isClosed;
	else
		return m_socket.IsOpen() && m_client == client;
}

void NetState::markClosed(void)
{
	m_isClosed = true;
}

void NetState::setAsyncMode(void)
{
	// is async mode enabled?
	if ( !g_Cfg.m_fUseAsyncNetwork )
		m_useAsync = false;

	// if the version mod flag is not set, always use async mode
	else if ( g_Cfg.m_fUseAsyncNetwork != 2 )
		m_useAsync = true;

	// http clients do not want to be using async networking unless they have keep-alive set
	else if (getClient() != NULL && getClient()->GetConnectType() == CONNECT_HTTP)
		m_useAsync = false;

	// only use async with clients newer than 4.0.0
	// - normally the client version is unknown for the first 1 or 2 packets, so all clients will begin
	//   without async networking (but should switch over as soon as it has been determined)
	// - a minor issue with this is that for clients without encryption we cannot determine their version
	//   until after they have fully logged into the game server and sent a client version packet.
	else if (isClientVersion(0x400000) || isClientKR())
		m_useAsync = true;
	else
		m_useAsync = false;
}

bool NetState::hasPendingData(void) const
{
	// check if state is even valid
	if (m_socket.IsOpen() == false)
		return false;

	// check packet queues (only count high priority+ for closed states)
	for (int i = isClosed() ? NETWORK_DISCONNECTPRI : PacketSend::PRI_IDLE; i < PacketSend::PRI_QTY; i++)
	{
		if (m_queue[i].empty() == false)
			return true;
		
#ifdef NETWORK_MULTITHREADED
		if (m_holdingQueue[i].empty() == false)
			return true;
#endif
	}

	// check async data
	if (isAsyncMode() && m_asyncQueue.empty() == false)
		return true;

	return false;
}

bool NetState::canReceive(PacketSend* packet) const
{
	if (m_socket.IsOpen() == false || packet == NULL)
		return false;

	if (isClosed() && packet->getPriority() < NETWORK_DISCONNECTPRI)
		return false;

	if (packet->getTarget()->m_client == NULL)
		return false;

	return true;
}


/***************************************************************************
 *
 *
 *	class ClientIterator		Works as client iterator getting the clients
 *
 *
 ***************************************************************************/

ClientIterator::ClientIterator(const NetworkIn* network)
{
	m_id = -1;
	m_network = (network == NULL? &g_NetworkIn : network);
	m_max = m_network->m_stateCount;
}

ClientIterator::~ClientIterator(void)
{
	m_network = NULL;
}

CClient* ClientIterator::next()
{
	while (++m_id < m_max)
	{
		NetState* state = m_network->m_states[m_id];
		if ( state->isValid() && state->m_client != NULL && state->isValid(state->m_client) )
			return state->m_client;
	}

	return NULL;
}


/***************************************************************************
 *
 *
 *	class NetworkIn::HistoryIP	Simple interface for IP history maintainese
 *
 *
 ***************************************************************************/

void NetworkIn::HistoryIP::update(void)
{
	m_ttl = NETHISTORY_TTL;
}

bool NetworkIn::HistoryIP::checkPing(void)
{
	update();

	return ( m_blocked || ( m_pings++ >= NETHISTORY_MAXPINGS ));
}

void NetworkIn::HistoryIP::setBlocked(bool isBlocked, int timeout)
{
	if (isBlocked == true)
	{
		CScriptTriggerArgs args(m_ip.GetAddrStr());
		args.m_iN1 = timeout;
		g_Serv.r_Call("f_onserver_blockip", &g_Serv, &args);
		timeout = args.m_iN1;
	}

	m_blocked = isBlocked;

	if (isBlocked && timeout >= 0)
		m_blockExpire = CServTime::GetCurrentTime() + timeout;
	else
		m_blockExpire.Init();
}


/***************************************************************************
 *
 *
 *	class NetworkIn::HistoryIP	Simple interface for IP history maintainese
 *
 *
 ***************************************************************************/

NetworkIn::NetworkIn(void) : AbstractThread("NetworkIn", IThread::RealTime)
{
	m_buffer = NULL;
	m_decryptBuffer = NULL;
	m_states = NULL;
	m_stateCount = 0;
}

NetworkIn::~NetworkIn(void)
{
	if (m_buffer != NULL)
	{
		delete[] m_buffer;
		m_buffer = NULL;
	}

	if (m_decryptBuffer != NULL)
	{
		delete[] m_decryptBuffer;
		m_decryptBuffer = NULL;
	}

	for (long l = 0; l < NETWORK_PACKETCOUNT; l++)
	{
		if (m_handlers[l] != NULL)
		{
			delete m_handlers[l];
			m_handlers[l] = NULL;
		}

		if (m_extended[l] != NULL)
		{
			delete m_extended[l];
			m_extended[l] = NULL;
		}

		if (m_encoded[l] != NULL)
		{
			delete m_encoded[l];
			m_encoded[l] = NULL;
		}
	}

	if (m_states != NULL)
	{
		for (long l = 0; l < m_stateCount; l++)
		{
			delete m_states[l];
			m_states[l] = NULL;
		}

		delete[] m_states;
		m_states = NULL;
	}
}

void NetworkIn::onStart(void)
{
	m_lastGivenSlot = -1;
	memset(m_handlers, 0, sizeof(m_handlers));
	memset(m_extended, 0, sizeof(m_extended));
	memset(m_encoded, 0, sizeof(m_encoded));

	m_buffer = new BYTE[NETWORK_BUFFERSIZE];
	m_decryptBuffer = new BYTE[NETWORK_BUFFERSIZE];

#ifdef DEBUGPACKETS
	g_Log.Debug("Registering packets...\n");
#endif

	// standard packets
	registerPacket(XCMD_Create, new PacketCreate());							// create character
	registerPacket(XCMD_Walk, new PacketMovementReq());							// movement request
	registerPacket(XCMD_Talk, new PacketSpeakReq());							// speak
	registerPacket(XCMD_Attack, new PacketAttackReq());							// begin attack
	registerPacket(XCMD_DClick, new PacketDoubleClick());						// double click object
	registerPacket(XCMD_ItemPickupReq, new PacketItemPickupReq());				// pick up item
	registerPacket(XCMD_ItemDropReq, new PacketItemDropReq());					// drop item
	registerPacket(XCMD_Click, new PacketSingleClick());						// single click object
	registerPacket(XCMD_ExtCmd, new PacketTextCommand());						// extended text command
	registerPacket(XCMD_ItemEquipReq, new PacketItemEquipReq());				// equip item
	registerPacket(XCMD_WalkAck, new PacketResynchronize());					//
	registerPacket(XCMD_DeathMenu, new PacketDeathStatus());					//
	registerPacket(XCMD_CharStatReq, new PacketCharStatusReq());				// status request
	registerPacket(XCMD_Skill, new PacketSkillLockChange());					// change skill lock
	registerPacket(XCMD_VendorBuy, new PacketVendorBuyReq());					// buy items from vendor
	registerPacket(XCMD_MapEdit, new PacketMapEdit());							// edit map pins
	registerPacket(XCMD_CharPlay, new PacketCharPlay());						// select character
	registerPacket(XCMD_BookPage, new PacketBookPageEdit());					// edit book content
	registerPacket(XCMD_Options, new PacketUnknown(-1));						// unused options packet
	registerPacket(XCMD_Target, new PacketTarget());							// target an object
	registerPacket(XCMD_SecureTrade, new PacketSecureTradeReq());				// secure trade action
	registerPacket(XCMD_BBoard, new PacketBulletinBoardReq());					// bulletin board action
	registerPacket(XCMD_War, new PacketWarModeReq());							// toggle war mode
	registerPacket(XCMD_Ping, new PacketPingReq());								// ping request
	registerPacket(XCMD_CharName, new PacketCharRename());						// change character name
	registerPacket(XCMD_MenuChoice, new PacketMenuChoice());					// select menu item
	registerPacket(XCMD_ServersReq, new PacketServersReq());					// request server list
	registerPacket(XCMD_CharDelete, new PacketCharDelete());					// delete character
	registerPacket(XCMD_CreateNew, new PacketCreateNew());						// create character
	registerPacket(XCMD_CharListReq, new PacketCharListReq());					// request character list
	registerPacket(XCMD_BookOpen, new PacketBookHeaderEdit());					// edit book
	registerPacket(XCMD_DyeVat, new PacketDyeObject());							// colour selection dialog
	registerPacket(XCMD_AllNames3D, new PacketAllNamesReq());					// all names macro (ctrl+shift)
	registerPacket(XCMD_Prompt, new PacketPromptResponse());					// prompt response
	registerPacket(XCMD_HelpPage, new PacketHelpPageReq());						// GM help page request
	registerPacket(XCMD_VendorSell, new PacketVendorSellReq());					// sell items to vendor
	registerPacket(XCMD_ServerSelect, new PacketServerSelect());				// select server
	registerPacket(XCMD_Spy, new PacketSystemInfo());							//
	registerPacket(XCMD_Scroll, new PacketUnknown(5));							// scroll closed
	registerPacket(XCMD_TipReq, new PacketTipReq());							//
	registerPacket(XCMD_GumpInpValRet, new PacketGumpValueInputResponse());		// text input dialog
	registerPacket(XCMD_TalkUNICODE, new PacketSpeakReqUNICODE());				// speech (unicode)
	registerPacket(XCMD_GumpDialogRet, new PacketGumpDialogRet());				// dialog response (button press)
	registerPacket(XCMD_ChatText, new PacketChatCommand());						// chat command
	registerPacket(XCMD_Chat, new PacketChatButton());							// chat button
	registerPacket(XCMD_ToolTipReq, new PacketToolTipReq());					// popup help request
	registerPacket(XCMD_CharProfile, new PacketProfileReq());					// profile read/write request
	registerPacket(XCMD_MailMsg, new PacketMailMessage());						//
	registerPacket(XCMD_ClientVersion, new PacketClientVersion());				// client version
	registerPacket(XCMD_ExtData, new PacketExtendedCommand());					//
	registerPacket(XCMD_PromptUNICODE, new PacketPromptResponseUnicode());		// prompt response (unicode)
	registerPacket(XCMD_ViewRange, new PacketViewRange());						//
	registerPacket(XCMD_ConfigFile, new PacketUnknown(-1));						//
	registerPacket(XCMD_LogoutStatus, new PacketLogout());						//
	registerPacket(XCMD_AOSTooltip, new PacketAOSTooltipReq());					//
	registerPacket(XCMD_ExtAosData, new PacketEncodedCommand());				//
	registerPacket(XCMD_Spy2, new PacketHardwareInfo());						// client hardware info
	registerPacket(XCMD_BugReport, new PacketBugReport());						// bug report
	registerPacket(XCMD_KRClientType, new PacketClientType());					// report client type (2d/kr/sa)
	registerPacket(XCMD_HighlightUIRemove, new PacketRemoveUIHighlight());		//
	registerPacket(XCMD_UseHotbar, new PacketUseHotbar());						//
	registerPacket(XCMD_MacroEquipItem, new PacketEquipItemMacro());			//
	registerPacket(XCMD_MacroUnEquipItem, new PacketUnEquipItemMacro());		//
	registerPacket(XCMD_WalkNew, new PacketMovementReqNew());					// movement request (SA)
	registerPacket(XCMD_WalkUnknown, new PacketUnknown(9));						//
	registerPacket(XCMD_CrashReport, new PacketCrashReport());					//

	// extended packets (0xBF)
	registerExtended(EXTDATA_ScreenSize, new PacketScreenSize());				// client screen size
	registerExtended(EXTDATA_Party_Msg, new PacketPartyMessage());				// party command
	registerExtended(EXTDATA_Arrow_Click, new PacketArrowClick());				// click quest arrow
	registerExtended(EXTDATA_Wrestle_DisArm, new PacketWrestleDisarm());		// wrestling disarm macro
	registerExtended(EXTDATA_Wrestle_Stun, new PacketWrestleStun());			// wrestling stun macro
	registerExtended(EXTDATA_Lang, new PacketLanguage());						// client language
	registerExtended(EXTDATA_StatusClose, new PacketStatusClose());				// status window closed
	registerExtended(EXTDATA_Yawn, new PacketAnimationReq());					// play animation
	registerExtended(EXTDATA_Unk15, new PacketClientInfo());					// client information
	registerExtended(EXTDATA_OldAOSTooltipInfo, new PacketAOSTooltipReq());		//
	registerExtended(EXTDATA_Popup_Request, new PacketPopupReq());				// request popup menu
	registerExtended(EXTDATA_Popup_Select, new PacketPopupSelect());			// select popup option
	registerExtended(EXTDATA_Stats_Change, new PacketChangeStatLock());			// change stat lock
	registerExtended(EXTDATA_NewSpellSelect, new PacketSpellSelect());			//
	registerExtended(EXTDATA_HouseDesignDet, new PacketHouseDesignReq());		// house design request
	registerExtended(EXTDATA_AntiCheat, new PacketAntiCheat());					// anti-cheat / unknown
	registerExtended(EXTDATA_BandageMacro, new PacketBandageMacro());			//
	registerExtended(EXTDATA_GargoyleFly, new PacketGargoyleFly());				//

	// encoded packets (0xD7)
	registerEncoded(EXTAOS_HcBackup, new PacketHouseDesignBackup());			// house design - backup
	registerEncoded(EXTAOS_HcRestore, new PacketHouseDesignRestore());			// house design - restore
	registerEncoded(EXTAOS_HcCommit, new PacketHouseDesignCommit());			// house design - commit
	registerEncoded(EXTAOS_HcDestroyItem, new PacketHouseDesignDestroyItem());	// house design - remove item
	registerEncoded(EXTAOS_HcPlaceItem, new PacketHouseDesignPlaceItem());		// house design - place item
	registerEncoded(EXTAOS_HcExit, new PacketHouseDesignExit());				// house design - exit designer
	registerEncoded(EXTAOS_HcPlaceStair, new PacketHouseDesignPlaceStair());	// house design - place stairs
	registerEncoded(EXTAOS_HcSynch, new PacketHouseDesignSync());				// house design - synchronise
	registerEncoded(EXTAOS_HcClear, new PacketHouseDesignClear());				// house design - clear
	registerEncoded(EXTAOS_HcSwitch, new PacketHouseDesignSwitch());			// house design - change floor
	registerEncoded(EXTAOS_HcPlaceRoof, new PacketHouseDesignPlaceRoof());		// house design - place roof
	registerEncoded(EXTAOS_HcDestroyRoof, new PacketHouseDesignDestroyRoof());	// house design - remove roof
	registerEncoded(EXTAOS_SpecialMove, new PacketSpecialMove());				//
	registerEncoded(EXTAOS_HcRevert, new PacketHouseDesignRevert());			// house design - revert
	registerEncoded(EXTAOS_EquipLastWeapon, new PacketEquipLastWeapon());		//
	registerEncoded(EXTAOS_GuildButton, new PacketGuildButton());				// guild button press
	registerEncoded(EXTAOS_QuestButton, new PacketQuestButton());				// quest button press

	m_states = new NetState*[g_Cfg.m_iClientsMax];
	for (long l = 0; l < g_Cfg.m_iClientsMax; l++)
		m_states[l] = new NetState(l);
	m_stateCount = g_Cfg.m_iClientsMax;
}

void NetworkIn::tick(void)
{
	EXC_TRY("NetworkIn");
	if (g_Serv.m_iExitFlag || g_Serv.m_iModeCode != SERVMODE_Run)
		return;

	// periodically check ip history
	static char iPeriodic = 0;
	if (iPeriodic == 0)
	{
		EXC_SET("periodic");
		periodic();
	}

	if (++iPeriodic > 20)
		iPeriodic = 0;

	// check for incoming data
	EXC_SET("select");
	fd_set readfds;
	int ret = checkForData(&readfds);
	if (ret <= 0)
		return;

	EXC_SET("new connection");
	if (FD_ISSET(g_Serv.m_SocketMain.GetSocket(), &readfds))
		acceptConnection();

	EXC_SET("messages");
	BYTE* buffer = m_buffer;
	for (long i = 0; i < m_stateCount; i++)
	{
		NetState* client = m_states[i];
		if (client->m_client == NULL || client->isClosed())
			continue;

		if (!FD_ISSET(client->m_socket.GetSocket(), &readfds))
		{
			if (client->m_client->GetConnectType() != CONNECT_TELNET)
			{
				// check for timeout
				int iLastEventDiff = -g_World.GetTimeDiff( client->m_client->m_timeLastEvent );
				if ( g_Cfg.m_iDeadSocketTime && iLastEventDiff > g_Cfg.m_iDeadSocketTime )
				{
					g_Log.EventError("%x:Frozen client connection disconnected.\n", client->m_id);
					client->markClosed();
				}
			}
			continue;
		}

		// receive data
		int received = client->m_socket.Receive(buffer, NETWORK_BUFFERSIZE, 0);
		if (received <= 0)
		{
			client->markClosed();
			continue;
		}

		if (client->m_client->GetConnectType() == CONNECT_UNK)
		{
			if (client->m_seeded == false)
			{
				if (received >= 4) // login connection
				{
					if ( memcmp(buffer, "GET /", 5) == 0 || memcmp(buffer, "POST /", 6) == 0 ) // HTTP
					{
						if ( g_Cfg.m_fUseHTTP != 2 )
						{
							client->markClosed();
							continue;
						}

						client->m_client->SetConnectType(CONNECT_HTTP);
						if ( !client->m_client->OnRxWebPageRequest(buffer, received) )
						{
							client->markClosed();
							continue;
						}

						continue;
					}

					DWORD seed(0);
					int iSeedLen(0);
					if (client->m_newseed || (buffer[0] == XCMD_NewSeed && received >= NETWORK_SEEDLEN_NEW))
					{
						CEvent* pEvent = (CEvent*)buffer;

						if (client->m_newseed)
						{
							// we already received the 0xEF on its own, so move the pointer
							// back 1 byte to align it
							iSeedLen = NETWORK_SEEDLEN_NEW - 1;
							pEvent = (CEvent *)(((BYTE*)pEvent) - 1);
						}
						else
						{
							iSeedLen = NETWORK_SEEDLEN_NEW;
							client->m_newseed = true;
						}

						DEBUG_WARN(("New Login Handshake Detected. Client Version: %d.%d.%d.%d\n", (DWORD)pEvent->NewSeed.m_Version_Maj, 
									 (DWORD)pEvent->NewSeed.m_Version_Min, (DWORD)pEvent->NewSeed.m_Version_Rev, 
									 (DWORD)pEvent->NewSeed.m_Version_Pat));

						client->m_reportedVersion = CCrypt::GetVerFromVersion(pEvent->NewSeed.m_Version_Maj, pEvent->NewSeed.m_Version_Min, pEvent->NewSeed.m_Version_Rev, pEvent->NewSeed.m_Version_Pat);
						seed = (DWORD) pEvent->NewSeed.m_Seed;
					}
					else
					{
						// assume it's a normal client log in
						seed = ( buffer[0] << 24 ) | ( buffer[1] << 16 ) | ( buffer[2] << 8 ) | buffer[3];
						iSeedLen = NETWORK_SEEDLEN_OLD;
					}

					if ( !seed )
					{
						g_Log.EventError("Invalid client %d detected, disconnecting.", client->id());
						client->markClosed();
						continue;
					}

					client->m_seeded = true;
					client->m_seed = seed;
					buffer += iSeedLen;
					received -= iSeedLen;
				}
				else
				{
					if (*buffer == XCMD_NewSeed)
					{
						// Game client
						client->m_newseed = true;
						continue;
					}

					if (client->m_client->OnRxPing(buffer, received) == false)
						client->markClosed();

					continue;
				}
			}

			if (!received)
			{
				if (client->m_seed == 0xFFFFFFFF)
				{
					// UOKR Client opens connection with 255.255.255.255
					DEBUG_WARN(("UOKR Client Detected.\n"));
					client->m_client->SetConnectType(CONNECT_CRYPT);
					client->m_clientType = CLIENTTYPE_KR;
					new PacketKREncryption(client->getClient());
				}
				continue;
			}

			if (received < 5)
			{
				if (client->m_client->OnRxPing(buffer, received) == false)
					client->markClosed();

				continue;
			}

			// log in the client
			client->m_client->SetConnectType(CONNECT_CRYPT);
			client->m_client->xProcessClientSetup((CEvent*)buffer, received);
			continue;
		}

		client->m_client->m_timeLastEvent = CServTime::GetCurrentTime();

		// first data on a new connection - find out what should come next
		if ( client->m_client->m_Crypt.IsInit() == false )
		{
			CEvent evt;
			memcpy(&evt, buffer, received);

			switch ( client->m_client->GetConnectType() )
			{
				case CONNECT_CRYPT:
					if (received >= 5)
					{
						if (*buffer == XCMD_EncryptionReply && client->isClientKR())
						{
							// receiving response to 0xe3 packet
							int iEncKrLen = evt.EncryptionReply.m_len;
							if (received < iEncKrLen)
								break;
							else if (received == iEncKrLen)
								continue;

							received -= iEncKrLen;
							client->m_client->xProcessClientSetup( (CEvent*)(evt.m_Raw + iEncKrLen), received);
							break;
						}
						else
						{
							client->m_client->xProcessClientSetup(&evt, received);
						}
					}
					else
					{
						// not enough data to be a real client
						client->m_client->SetConnectType(CONNECT_UNK);
						if (client->m_client->OnRxPing(buffer, received) == false)
						{
							client->markClosed();
							continue;
						}
					}
					break;
					
				case CONNECT_HTTP:
					if ( !client->m_client->OnRxWebPageRequest(evt.m_Raw, received) )	
					{
						client->markClosed();
						continue;
					}
					break;
					
				case CONNECT_TELNET:
					if ( !client->m_client->OnRxConsole(evt.m_Raw, received) )
					{
						client->markClosed();
						continue;
					}
					break;
					
				default:
					g_Log.Event(LOGM_CLIENTS_LOG,"%x:Junk messages with no crypt\n", client->m_id);
					client->markClosed();
					continue;
			}

			continue;
		}

		// decrypt the client data and add it to queue
		client->m_client->m_Crypt.Decrypt(m_decryptBuffer, buffer, received);
		Packet* packet = new Packet(m_decryptBuffer, received);

		xRecordPacket(client->m_client, packet, "client->server");

		// process the message
		EXC_TRYSUB("ProcessMessage");

		long len = packet->getLength();
		long offset = 0;
		while (len > 0 && !client->isClosed())
		{
			long packetID = packet->getData()[offset];
			Packet* handler = m_handlers[packetID];

			//	Packet filtering - check if any function triggering is installed
			//		allow skipping the packet which we do not wish to get
			if (client->m_client->xPacketFilter((CEvent*)(packet->getData() + offset), packet->getLength() - offset))
				break;

			if (handler != NULL)
			{
				long packetLength = handler->checkLength(client, packet);
#ifdef DEBUGPACKETS
				g_Log.Debug("Checking length: counted %d.\n", packetLength);
#endif
				//	fall back and delete the packet
				if (packetLength <= 0)
					break;

				len -= packetLength;
				offset += packetLength;

				handler->seek();
				for (int i = 0; i < packetLength; i++)
				{
					BYTE next = packet->readByte();
					handler->writeByte(next);
				}

				handler->resize(packetLength);
				handler->seek(1);
				handler->onReceive(client);
			}
			else
			{
				g_Log.EventWarn("%x:Unknown game packet (0x%x) received.\n", client->id(), packetID);

				len -= 1;
				offset += 1;
				packet->skip(1);
			}
		}

		EXC_CATCHSUB("Network");
		EXC_DEBUGSUB_START;
		g_Log.EventDebug("Parsing %s", packet->dump());

		client->m_packetExceptions++;
		if (client->m_packetExceptions && client->m_client != NULL)
		{
			g_Log.EventWarn("Disconnecting client from account '%s' since it is causing exceptions problems\n", client->m_client->GetAccount() ? client->m_client->GetAccount()->GetName() : "");
			client->m_client->addKick(&g_Serv, false);
		}

		client->m_client->addKick(&g_Serv, false);
		EXC_DEBUGSUB_END;
		delete packet;
	}

	EXC_CATCH;
	EXC_DEBUG_START;
	
	EXC_DEBUG_END;
}

void NetworkIn::waitForClose(void)
{
	terminate();
	return;
}

int NetworkIn::checkForData(fd_set* storage)
{
//	Berkeley sockets needs nfds to be updated that while in Windows that's ignored
#ifdef _WIN32
#define ADDTOSELECT(_x_)	FD_SET(_x_, storage)
#else
#define ADDTOSELECT(_x_)	{ FD_SET(_x_, storage); if ( _x_ > nfds ) nfds = _x_; }
#endif

	int nfds = 0;
		
	FD_ZERO(storage);

#ifndef _WIN32
	if ( !g_Cfg.m_fUseAsyncNetwork )
#endif
		ADDTOSELECT(g_Serv.m_SocketMain.GetSocket());

	for ( long l = 0; l < m_stateCount; l++ )
	{
		NetState* state = m_states[l];
		if ( !state->m_socket.IsOpen() )
			continue;

		if ( state->isClosed() )
		{
#ifdef NETWORK_MULTITHREADED
			if (state->m_closeLock.doTryLock())
#endif
			{
#ifdef DEBUGPACKETS
				g_Log.Debug("Client '%x' is gonna being cleared since marked to close.\n",
					state->id());
#endif
				g_NetworkOut.flush(state->getClient());

				if (state->hasPendingData() == false)
					state->clear();

#ifdef NETWORK_MULTITHREADED
				state->m_closeLock.doUnlock();
#endif
			}
			continue;
		}
		ADDTOSELECT(state->m_socket.GetSocket());
	}

	timeval Timeout;	// time to wait for data.
	Timeout.tv_sec=0;
	Timeout.tv_usec=100;	// micro seconds = 1/1000000
	return select(nfds+1, storage, NULL, NULL, &Timeout);
#undef ADDTOSELECT
}


#ifdef _WIN32
#define CLOSESOCKET(_x_)	{ shutdown(_x_, 2); closesocket(_x_); }
#else
#define CLOSESOCKET(_x_)	{ shutdown(_x_, 2); close(_x_); }
#endif

void NetworkIn::acceptConnection(void)
{
	EXC_TRY("acceptConnection");
	CSocketAddress client_addr;

	EXC_SET("accept");
	SOCKET h = g_Serv.m_SocketMain.Accept(client_addr);
	if (( h >= 0 ) && ( h != INVALID_SOCKET ))
	{
		EXC_SET("ip history");
		HistoryIP* ip = &getHistoryForIP(client_addr);
		long maxIp = g_Cfg.m_iConnectingMaxIP;
		long climaxIp = g_Cfg.m_iClientsMaxIP;

#ifdef DEBUGPACKETS 
		g_Log.Debug("Incoming connection from '%s' [blocked=%d, ttl=%d, pings=%d, connecting=%d, connected=%d]\n",
			ip->m_ip.GetAddrStr(), ip->m_blocked, ip->m_ttl, ip->m_pings, ip->m_connecting, ip->m_connected);
#endif
		//	ip is blocked
		if ( ip->checkPing() ||
			// or too much connect tries from this ip
			( maxIp && ( ip->m_connecting > maxIp )) ||
			// or too much clients from this ip
			( climaxIp && ( ip->m_connected > climaxIp ))
			)
		{
			EXC_SET("rejecting");
#ifdef DEBUGPACKETS 
			g_Log.Debug("Closing incoming connection [max ip=%d, clients max ip=%d).\n",
				maxIp, climaxIp);
#endif

			CLOSESOCKET(h);

			if (ip->m_blocked)
				g_Log.Event(LOGM_CLIENTS_LOG|LOGL_ERROR, "Connection from %s rejected. (Blocked IP)\n", (LPCTSTR)client_addr.GetAddrStr());
			else if ( maxIp && ip->m_connecting > maxIp )
				g_Log.Event(LOGM_CLIENTS_LOG|LOGL_ERROR, "Connection from %s rejected. (CONNECTINGMAXIP reached)\n", (LPCTSTR)client_addr.GetAddrStr());
			else if ( climaxIp && ip->m_connected > climaxIp )
				g_Log.Event(LOGM_CLIENTS_LOG|LOGL_ERROR, "Connection from %s rejected. (CLIENTMAXIP reached)\n", (LPCTSTR)client_addr.GetAddrStr());
			else
				g_Log.Event(LOGM_CLIENTS_LOG|LOGL_ERROR, "Connection from %s rejected.\n", (LPCTSTR)client_addr.GetAddrStr());
		}
		else
		{
			EXC_SET("detecting slot");
			long slot = getStateSlot();
			if ( slot == -1 )			// we do not have enough empty slots for clients
			{
#ifdef DEBUGPACKETS 
				g_Log.Debug("Unable to allocate new slot for client, too many clients already.\n");
#endif
				CLOSESOCKET(h);
			}
			else
			{
				m_states[slot]->init(h, client_addr);
			}
		}
	}
	EXC_CATCH;
}

void NetworkIn::registerPacket(int packetId, Packet* handler)
{
	if (packetId >= 0 && packetId < NETWORK_PACKETCOUNT)
		m_handlers[packetId] = handler;
}

void NetworkIn::registerExtended(int packetId, Packet* handler)
{
	if (packetId >= 0 && packetId < NETWORK_PACKETCOUNT)
		m_extended[packetId] = handler;
}

void NetworkIn::registerEncoded(int packetId, Packet* handler)
{
	if (packetId >= 0 && packetId < NETWORK_PACKETCOUNT)
		m_encoded[packetId] = handler;
}

Packet* NetworkIn::getHandler(int packetId) const
{
	return m_handlers[packetId];
}

Packet* NetworkIn::getExtendedHandler(int packetId) const
{
	return m_extended[packetId];
}

Packet* NetworkIn::getEncodedHandler(int packetId) const
{
	return m_encoded[packetId];
}

NetworkIn::HistoryIP &NetworkIn::getHistoryForIP(CSocketAddressIP ip)
{
	vector<HistoryIP>::iterator it;

	for ( it = m_ips.begin(); it != m_ips.end(); it++ )
	{
		if ( (*it).m_ip == ip )
			return *it;
	}

	HistoryIP hist;
	memset(&hist, 0, sizeof(hist));
	hist.m_ip = ip;
	hist.update();

	m_ips.push_back(hist);
	return getHistoryForIP(ip);
}

NetworkIn::HistoryIP &NetworkIn::getHistoryForIP(char* ip)
{
	CSocketAddressIP	me(ip);

	return getHistoryForIP(me);
}

long NetworkIn::getStateSlot(long startFrom)
{
	if ( startFrom == -1 )
		startFrom = m_lastGivenSlot + 1;

	//	give ordered slot number, each time incrementing by 1 for easier log view
	for ( long l = startFrom; l < m_stateCount; l++ )
	{
		if ( !m_states[l]->isValid() )
		{
			return ( m_lastGivenSlot = l );
		}
	}

	//	we did not find empty slots till the end, try rescan from beginning
	if ( startFrom > 0 )
		return getStateSlot(0);

	//	no empty slots exists, arbitrary too many clients
	return -1;
}

void NetworkIn::periodic(void)
{
	CClient* client;
	long connecting = 0;
	long connectingMax = g_Cfg.m_iConnectingMax;
	long decaystart = 0;
	long decayttl = 0;

	EXC_TRY("periodic");
	// check if max connecting limit is obeyed
	if (connectingMax > 0)
	{
		EXC_SET("limiting connecting clients");
		ClientIterator clients(this);
		while ((client = clients.next()) != NULL)
		{
			if (client->IsConnecting())
			{
				if (++connecting > connectingMax)
				{
#ifdef DEBUGPACKETS
					g_Log.EventDebug("Closing client '%x' since '%d' connecting overlaps '%d'\n", client->m_net->id(), connecting, connectingMax);
#endif
					client->m_net->markClosed();
				}
			}
			if (connecting > connectingMax)
			{
				g_Log.EventWarn("%d clients in connect mode (max %d), closing %d\n", connecting, connectingMax, connecting - connectingMax);
			}
		}
	}

	// tick the ip history, remove some from the list
	EXC_SET("ticking history");
	for (int i = 0; i < m_ips.size(); i++)
	{
		HistoryIP* ip = &m_ips[i];
		if (ip->m_blocked)
		{
			// blocked IPs don't decay, but check if the ban has expired
			if (ip->m_blockExpire.IsTimeValid() && CServTime::GetCurrentTime() > ip->m_blockExpire)
				ip->setBlocked(false);
		}
		else
		{
			ip->m_ttl--;

			// decay pings history with time
			if (ip->m_pings > decaystart && ip->m_ttl < decayttl)
				ip->m_pings--;
		}
	}

	// clear old ip history
	for ( vector<HistoryIP>::iterator it = m_ips.begin(); it != m_ips.end(); it++ )
	{
		if (it->m_ttl >= 0)
			continue;

		m_ips.erase(it);
		break;
	}

	// resize m_states to account for m_iClientsMax changes
	long max = g_Cfg.m_iClientsMax;
	if (max > m_stateCount)
	{
		EXC_SET("increasing network state size");
		// reallocate state buffer to accomodate additional clients
		long prevCount = m_stateCount;
		NetState** prevStates = m_states;

		NetState** newStates = new NetState*[max];
		memcpy(newStates, prevStates, m_stateCount * sizeof(NetState*));
		
		m_states = newStates;
		m_stateCount = max;

		// destroy previous states
		delete[] prevStates;
	}
	else if (max < m_stateCount)
	{
		EXC_SET("decreasing network state size");
		// delete excess states but leave array intact
		for (long l = m_stateCount; l < max; l++)
		{
			delete m_states[l];
			m_states[l] = NULL;
		}

		m_stateCount = max;
	}

	EXC_CATCH;
}


/***************************************************************************
 *
 *
 *	class NetworkOut			Networking thread used for queued sending outgoing packets
 *
 *
 ***************************************************************************/

NetworkOut::NetworkOut(void) : AbstractThread("NetworkOut", IThread::Highest)
{
	m_encryptBuffer = new BYTE[MAX_BUFFER];
}

NetworkOut::~NetworkOut(void)
{
	if (m_encryptBuffer != NULL)
	{
		delete[] m_encryptBuffer;
		m_encryptBuffer = NULL;
	}
}

void NetworkOut::tick(void)
{
	if (g_Serv.m_iExitFlag || g_Serv.m_iModeCode != SERVMODE_Run)
		return;

	EXC_TRY("NetworkOut");

	static char iCount = 0;
	iCount++;

#ifndef NETWORK_MULTITHREADED

	// process queues faster in single-threaded mode
	EXC_SET("highest");
	proceedQueue(PacketSend::PRI_HIGHEST);

	EXC_SET("high");
	proceedQueue(PacketSend::PRI_HIGH);

	EXC_SET("normal");
	proceedQueue(PacketSend::PRI_NORMAL);

	EXC_SET("low");
	if ((iCount % 2) == 1)
		proceedQueue(PacketSend::PRI_LOW);

	EXC_SET("idle");
	if ((iCount % 4) == 3)
		proceedQueue(PacketSend::PRI_IDLE);
#else

	// throttle rate of sending in multi-threaded mode
	EXC_SET("highest");
	proceedQueue(PacketSend::PRI_HIGHEST);

	EXC_SET("high");
	if ((iCount % 2) == 1)
		proceedQueue(PacketSend::PRI_HIGH);

	EXC_SET("normal");
	if ((iCount % 4) == 3)
		proceedQueue(PacketSend::PRI_NORMAL);

	EXC_SET("low");
	if ((iCount % 8) == 7)
		proceedQueue(PacketSend::PRI_LOW);

	EXC_SET("idle");
	if ((iCount % 16) == 15)
		proceedQueue(PacketSend::PRI_IDLE);
#endif

	EXC_CATCH;
}

void NetworkOut::waitForClose(void)
{
	terminate();
	return;
}

void NetworkOut::schedule(PacketSend* packet)
{
	scheduleOnce(packet->clone());
}

void NetworkOut::scheduleOnce(PacketSend* packet)
{
	NetState* state = packet->m_target;

	// don't bother queuing packets for invalid sockets
	if (state->isValid() == false)
	{
		delete packet;
		return;
	}

	long priority = packet->getPriority();

#ifdef NETWORK_MAXQUEUESIZE
	// limit by number of packets to be in queue
	if (priority > PacketSend::PRI_IDLE)
	{
		long maxClientPackets = NETWORK_MAXQUEUESIZE;
		if (maxClientPackets > 0)
		{
#ifdef NETWORK_MULTITHREADED
			if (state->m_holdingQueue[priority].size() >= maxClientPackets)
#else
			if (state->m_queue[priority].size() >= maxClientPackets)
#endif
			{
#ifdef DEBUGPACKETS
				g_Log.Debug("%x:Packet decreased priority due to overal amount %d overlapping %d.\n",
					state->id(), state->m_queue[priority].size(), maxClientPackets);
#endif
				packet->m_priority = priority-1;
				scheduleOnce(packet);
				return;
			}
		}
	}
#endif

#ifdef NETWORK_MULTITHREADED
	state->m_holdingQueue[priority].push(packet);
#else
	state->m_queue[priority].push(packet);
#endif
}

void NetworkOut::flush(CClient* client)
{
	for (int priority = 0; priority < PacketSend::PRI_QTY; priority++)
		proceedQueue(client, priority);

	proceedQueueAsync(client);
}

#ifdef NETWORK_MULTITHREADED
void NetworkOut::pushHoldingQueues(void)
{
	ClientIterator clients;

	NetState* state;
	while (CClient* client = clients.next())
	{
		state = client->GetNetState();
		ASSERT(state != NULL);

		bool lockAcquired(false);

		for (long i = PacketSend::PRI_HIGHEST; i >= PacketSend::PRI_IDLE; i--)
		{
			if (state->m_holdingQueue[i].empty())
				continue;

			if (lockAcquired == false)
			{
				state->m_queueLock.doLock();
				lockAcquired = true;
			}

			while (state->m_holdingQueue[i].size())
			{
				PacketSend* packet = state->m_holdingQueue[i].front();
				state->m_holdingQueue[i].pop();

				state->m_queue[i].push(packet);
			}
		}

		if (lockAcquired)
			state->m_queueLock.doUnlock();
	}
}
#endif

void NetworkOut::proceedQueue(long priority)
{
	ClientIterator clients;
	while (CClient* client = clients.next())
	{
		proceedQueue(client, priority);
		proceedQueueAsync(client);
	}
}

void NetworkOut::proceedQueue(CClient* client, long priority)
{
	long maxClientPackets = NETWORK_MAXPACKETS;
	long maxClientLength = NETWORK_MAXPACKETLEN;
	CServTime time = CServTime::GetCurrentTime();

	NetState* state = client->GetNetState();
	ASSERT(state != NULL);

	long packets = state->m_queue[priority].size();
	long length = 0;

	if (packets <= 0)
		return;

	if (packets > maxClientPackets)
		packets = maxClientPackets;

#ifdef NETWORK_MULTITHREADED
	state->m_closeLock.doLock();
	state->m_queueLock.doLock();
#endif

	// send N packets from the queue
	for (int i = 0; i < packets; i++)
	{
		PacketSend* packet = state->m_queue[priority].front();

		// remove early to prevent exceptions
		state->m_queue[priority].pop();

		if (state->canReceive(packet) == false || packet->onSend(client) == false)
		{
			if (packet == NULL)
				break;

			// don't count this towards the limit
			delete packet;

			// allow an extra packet to be processed, but take care not to axceed the queue size
			if (((packets + 1) - i) < state->m_queue[priority].size())
				packets++;

			continue;
		}

		EXC_TRY("proceedQueue");
		length += packet->getLength();

		EXC_SET("sending");
		if (sendPacket(client, packet) == true)
			client->m_timeLastSend = time;
		else
			state->markClosed();

		EXC_SET("check length");
		if (length > maxClientLength)
		{
#ifdef DEBUGPACKETS
			g_Log.Debug("%x:Packets sending stopped at %d packet due to overall length %d overlapping %d.\n",
				state->id(), i, length, maxClientLength);
#endif
			break;
		}

		EXC_CATCH;
		EXC_DEBUG_START;
		g_Log.EventDebug("id='%x', pri='%d', packet '%d' of '%d' to send, length '%d' of '%d'\n",
			state->id(), priority, i, packets, length, maxClientLength);
		EXC_DEBUG_END;
	}

#ifdef NETWORK_MULTITHREADED
	state->m_queueLock.doUnlock();
	state->m_closeLock.doUnlock();
#endif
}

void NetworkOut::proceedQueueAsync(CClient* client)
{
	NetState* state = client->GetNetState();
	ASSERT(state != NULL);

#ifdef NETWORK_MULTITHREADED
	state->m_closeLock.doLock();
#endif

	if (state->isAsyncMode() == false || state->m_asyncQueue.empty() || state->m_isSendingAsync)
	{
#ifdef NETWORK_MULTITHREADED
		state->m_closeLock.doUnlock();
#endif
		return;
	}

	// get next packet
	PacketSend* packet = NULL;
	
	while (state->m_asyncQueue.empty() == false)
	{
		packet = state->m_asyncQueue.front();
		state->m_asyncQueue.pop();

		if (state->canReceive(packet) == false || packet->onSend(client) == false)
		{
			if (packet != NULL)
			{
				delete packet;
				packet = NULL;
			}

			continue;
		}

		break;
	}

	// start sending
	if (packet != NULL)
	{
		if (sendPacketNow(client, packet) == false)
			state->markClosed();
	}

#ifdef NETWORK_MULTITHREADED
	state->m_closeLock.doUnlock();
#endif
}

void NetworkOut::onAsyncSendComplete(CClient* client)
{
	NetState* state = client->GetNetState();
	ASSERT(state != NULL);

	state->m_isSendingAsync = false;
	proceedQueueAsync(client);
}

bool NetworkOut::sendPacket(CClient* client, PacketSend* packet)
{
	NetState* state = client->GetNetState();
	ASSERT(state != NULL);

	// only allow high-priority packets to be sent to queued for closed sockets
	if (state->canReceive(packet) == false)
	{
		delete packet;
		return false;
	}

	if (state->isAsyncMode())
	{
		state->m_asyncQueue.push(packet);
		return true;
	}

	return sendPacketNow(client, packet);
}

#ifdef _WIN32

void CALLBACK SendCompleted(DWORD dwError, DWORD cbTransferred, LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags)
{
	if (dwError == WSAEFAULT)
		return;

	CClient* client = reinterpret_cast<CClient *>(lpOverlapped->hEvent);
	if (client != NULL)
		g_NetworkOut.onAsyncSendComplete(client);
}

#endif

bool NetworkOut::sendPacketNow(CClient* client, PacketSend* packet)
{
	NetState* state = client->GetNetState();
	ASSERT(state != NULL);

	EXC_TRY("proceedQueue");

	xRecordPacket(client, packet, "server->client");

	EXC_SET("send trigger");
	if (packet->onSend(client))
	{
		BYTE* sendBuffer = NULL;
		int sendBufferLength = 0;

		int ret = 0;
		if (state->m_client == NULL)
		{
#ifdef DEBUGPACKETS
			g_Log.Debug("%x:Sending packet to closed client?\n", state->id());
#endif

			sendBuffer = packet->getData();
			sendBufferLength = packet->getLength();
		}
		else
		{
			bool compressed = false;

			// game packets required packet encrypting
			if (client->GetConnectType() == CONNECT_GAME)
			{
				EXC_SET("compress and encrypt");

				// compress
				ret = client->xCompress(m_encryptBuffer, packet->getData(), packet->getLength());
				compressed = true;

				// encrypt
				if (client->m_Crypt.GetEncryptionType() == ENC_TFISH)
					client->m_Crypt.Encrypt(m_encryptBuffer, m_encryptBuffer, ret);
			}

			// select a buffer to send
			if (compressed == true)
			{
				sendBuffer = m_encryptBuffer;
				sendBufferLength = ret;
			}
			else
			{
				sendBuffer = packet->getData();
				sendBufferLength = packet->getLength();
			}
		}

		// send the data
		EXC_SET("sending");
#ifdef _WIN32
		if (state->isAsyncMode())
		{
			ZeroMemory(&state->m_overlapped, sizeof(WSAOVERLAPPED));
			state->m_overlapped.hEvent = client;
			state->m_bufferWSA.buf = (CHAR*)sendBuffer;
			state->m_bufferWSA.len = sendBufferLength;

			DWORD bytesSent;
			if (state->m_socket.SendAsync(&state->m_bufferWSA, 1, &bytesSent, 0, &state->m_overlapped, SendCompleted) != 0)
				ret = 0;
			else
				ret = bytesSent;
		}
		else
#endif
		{
			ret = state->m_socket.Send(sendBuffer, sendBufferLength);
		}

		// check for error
		if (ret <= 0)
		{
			EXC_SET("error parse");
			int errCode = CGSocket::GetLastError();

#ifdef _WIN32
			if (state->isAsyncMode() && errCode == WSA_IO_PENDING)
			{
				// safe to ignore this
				g_Serv.m_Profile.Count(PROFILE_DATA_TX, sendBufferLength);
			}
			else if (state->isAsyncMode() == false && errCode == WSAEWOULDBLOCK)
			{
				// re-queue the packet and try again later
				scheduleOnce(packet);
				return true;
			}
			else if (errCode == WSAECONNRESET || errCode == WSAECONNABORTED)
			{
				delete packet;
				return false;
			}
			else
#endif
			{
				if (state->isClosed() == false)
					g_Log.EventWarn("%x:TX Error %d\n", state->id(), errCode);

#ifndef _WIN32
				delete packet;
				return false;
#endif
			}
		}
		else
		{
			g_Serv.m_Profile.Count(PROFILE_DATA_TX, ret);
		}

		EXC_SET("sent trigger");
		packet->onSent(client);
	}

	delete packet;
	return true;

	EXC_CATCH;
	EXC_DEBUG_START;
	g_Log.EventDebug("id='%x', packet '%d', length '%d'\n",
		state->id(), *packet->getData(), packet->getLength());
	EXC_DEBUG_END;
	return false;
}
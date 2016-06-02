/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_SERVERBROWSER_H
#define ENGINE_CLIENT_SERVERBROWSER_H

#include <base/tl/sorted_array.h>
#include <engine/serverbrowser.h>
#include <engine/client/db_sqlite3.h>

class CServerBrowser : public IServerBrowser
{
public:
	class CServerEntry
	{
	public:
		NETADDR m_Addr;
		int64 m_RequestTime;
		bool m_Is64;
		int m_GotInfo;
		CServerInfo m_Info;

		CServerEntry *m_pNextIp; // ip hashed list

		CServerEntry *m_pPrevReq; // request list
		CServerEntry *m_pNextReq;
	};

	class CDDNetCountry
	{
	public:
		enum
		{
			MAX_SERVERS = 1024
		};

		char m_aName[256];
		int m_FlagID;
		NETADDR m_aServers[MAX_SERVERS];
		char m_aTypes[MAX_SERVERS][32];
		int m_NumServers;

		void Reset() { m_NumServers = 0; m_FlagID = -1; m_aName[0] = '\0'; };
		/*void Add(NETADDR Addr, char* pType) {
			if (m_NumServers < MAX_SERVERS)
			{
				m_aServers[m_NumServers] = Addr;
				str_copy(m_aTypes[m_NumServers], pType, sizeof(m_aTypes[0]));
				m_NumServers++;
			}
		};*/
	};

	struct RecentServer
	{
		RecentServer() { m_ID = -1; mem_zero(&m_Addr, sizeof(NETADDR)); }
		RecentServer(NETADDR addr, int id) : m_Addr(addr), m_ID(id) { }
		NETADDR m_Addr;
		int m_ID;
		char m_LastJoined[20];
		bool operator<(const RecentServer& other) const { return this->m_ID > other.m_ID; }
		bool operator==(const RecentServer& other) const {
			if(mem_comp(&this->m_Addr, &other, sizeof(NETADDR)) == 0)
				return true;
			return false;
		}
	};

	enum
	{
		MAX_FAVORITES=2048,
		//MAX_RECENT=2048,
		MAX_DDNET_COUNTRIES=16,
		MAX_DDNET_TYPES=32,
	};

	CServerBrowser();

	// interface functions
	void Refresh(int Type, int NoReload=false);
	void AbortRefresh() { m_pFirstReqServer = 0; m_NumRequests = 0; } // dunno if something needs to be cleaned up here...?
	void SaveCache();
	bool LoadCache();
	bool CacheExists() const { return m_CacheExists; }
	bool IsRefreshing() const;
	bool IsRefreshingMasters() const;
	int LoadingProgression() const;
	int UpgradeProgression() const;

	int NumServers() const { return m_NumServers; }

	int NumSortedServers() const { return m_NumSortedServers; }
	const CServerInfo *SortedGet(int Index) const;
	const CServerInfo *Get(int Index) const;

	bool IsFavorite(const NETADDR &Addr) const;
	void AddFavorite(const NETADDR &Addr);
	void AddRecent(const NETADDR& Addr);
	void RemoveFavorite(const NETADDR &Addr);

	void LoadDDNet();
	int NumDDNetCountries() { return m_NumDDNetCountries; }
	int GetDDNetCountryFlag(int Index) { return m_aDDNetCountries[Index].m_FlagID; }
	const char *GetDDNetCountryName(int Index) { return m_aDDNetCountries[Index].m_aName; }

	int NumDDNetTypes() { return m_NumDDNetTypes; };
	const char *GetDDNetType(int Index) { return m_aDDNetTypes[Index]; }

	void DDNetFilterAdd(char *pFilter, const char *pName);
	void DDNetFilterRem(char *pFilter, const char *pName);
	bool DDNetFiltered(char *pFilter, const char *pName);
	void DDNetCountryFilterClean();
	void DDNetTypeFilterClean();

	//
	void Update(bool ForceResort);
	void Upgrade(); // only re-request all the infos
	void Set(const NETADDR &Addr, int Type, int Token, const CServerInfo *pInfo);
	void Request(const NETADDR &Addr) const;

	void SetBaseInfo(class CNetClient *pClient, const char *pNetVersion);

	void RequestImpl64(const NETADDR &Addr, CServerEntry *pEntry) const;
	void QueueRequest(CServerEntry *pEntry);
	CServerEntry *Find(const NETADDR &Addr);
	int GetCurrentType() { return m_ServerlistType; }

private:

	CNetClient *m_pNetClient;
	IMasterServer *m_pMasterServer;
	class IConsole *m_pConsole;
	class IFriends *m_pFriends;
	char m_aNetVersion[128];

	CHeap m_ServerlistHeap;
	CServerEntry **m_ppServerlist;
	int *m_pSortedServerlist;

	NETADDR m_aFavoriteServers[MAX_FAVORITES];
	int m_NumFavoriteServers;

	CSql *m_pRecentDB;
	sorted_array<RecentServer> m_aRecentServers;

	CDDNetCountry m_aDDNetCountries[MAX_DDNET_COUNTRIES];
	int m_NumDDNetCountries;

	char m_aDDNetTypes[MAX_DDNET_TYPES][32];
	int m_NumDDNetTypes;

	CServerEntry *m_aServerlistIp[256]; // ip hash list

	CServerEntry *m_pFirstReqServer; // request list
	CServerEntry *m_pLastReqServer;
	int m_NumRequests;
	int m_MasterServerCount;

	// used instead of g_Config.BrMaxRequests to get more servers
	int m_CurrentMaxRequests;

	int m_LastPacketTick;

	int m_NeedRefresh;
	bool m_NeedUpgrade;
	bool m_CacheExists;

	int m_NumSortedServers;
	int m_NumSortedServersCapacity;
	int m_NumServers;
	float m_UpgradeProgression;
	int m_NumServerCapacity;

	int m_Sorthash;
	char m_aFilterString[64];
	char m_aFilterGametypeString[128];

	// the token is to keep server refresh separated from each other
	int m_CurrentToken;

	int m_ServerlistType;
	int64 m_BroadcastTime;

	// sorting criteria
	bool SortCompareName(int Index1, int Index2) const;
	bool SortCompareMap(int Index1, int Index2) const;
	bool SortComparePing(int Index1, int Index2) const;
	bool SortCompareGametype(int Index1, int Index2) const;
	bool SortCompareNumPlayers(int Index1, int Index2) const;
	bool SortCompareNumClients(int Index1, int Index2) const;

	//
	void Filter();
	void Sort();
	int SortHash() const;

	CServerEntry *Add(const NETADDR &Addr);

	void RemoveRequest(CServerEntry *pEntry);

	void RequestImpl(const NETADDR &Addr, CServerEntry *pEntry) const;

	void SetInfo(CServerEntry *pEntry, const CServerInfo &Info);

	static void ConfigSaveCallback(IConfig *pConfig, void *pUserData);
};

class CQueryRecent : public CQuery
{
	sorted_array<CServerBrowser::RecentServer> *m_paRecentList;

public:
	CQueryRecent() { m_paRecentList = 0; }
	CQueryRecent(sorted_array<CServerBrowser::RecentServer> *paRecentList) : m_paRecentList(paRecentList) { }
	void OnData();
};

#endif

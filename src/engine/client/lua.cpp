#include <base/system.h>
#include <engine/storage.h>

#include "luabinding.h"
#include "lua.h"


IClient * CLua::m_pClient = 0; 
IGameClient * CLua::m_pGameClient = 0;
CGameClient * CLua::m_pCGameClient = 0;

using namespace luabridge;

CLua::CLua()
{
	CLuaBinding::m_pUiContainer = new CLuaBinding::UiContainer;
}

CLua::~CLua()
{
	m_pLuaFiles.delete_all();
	m_pLuaFiles.clear();
}

void CLua::Init(IClient * pClient, IStorage * pStorage)
{
	m_pClient = pClient;
	m_pStorage = pStorage;
	LoadFolder();
}

void CLua::SetGameClient(IGameClient *pGameClient)
{
	CLua::m_pGameClient = pGameClient;
	CLua::m_pCGameClient = (CGameClient*)pGameClient;
}

void CLua::AddUserscript(const char *pFilename)
{
	if(!pFilename || pFilename[0] == '\0' || str_length(pFilename) <= 4 || str_comp_nocase(&pFilename[str_length(pFilename)]-4, ".lua"))
		return;

	std::string file = pFilename;
	dbg_msg("Lua", "adding script '%s' to list", file.c_str());
	m_pLuaFiles.add(new CLuaFile(this, file));
}

void CLua::LoadFolder()
{
	//char FullDir[256];
	//str_format(FullDir, sizeof(FullDir), "lua");

	dbg_msg("Lua", "Loading Folder");
	CLua::LuaLoadHelper * pParams = new CLua::LuaLoadHelper;
	pParams->pLua = this;
	pParams->pString = "lua";

	m_pStorage->ListDirectory(IStorage::TYPE_ALL, "lua", LoadFolderCallback, pParams);

	delete pParams;
}

int CLua::LoadFolderCallback(const char *pName, int IsDir, int DirType, void *pUser)
{
	if(pName[0] == '.')
		return 0;

	LuaLoadHelper *pParams = (LuaLoadHelper *)pUser;

	CLua *pSelf = pParams->pLua;
	const char *pFullDir = pParams->pString;

	char File[64];
	str_format(File, sizeof(File), "%s/%s", pFullDir, pName);
	dbg_msg("Lua", "-> Found File %s", File);

	pSelf->AddUserscript(File);
	return 0;
}

int CLua::Panic(lua_State *L)
{
    dbg_break();
    return 0;
}

int CLua::ErrorFunc(lua_State *L)
{
	dbg_msg("Lua", "Lua Script Error! :");
    lua_getglobal(L, "pLUA");
    CLua *pSelf = (CLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    int depth = 0;
    int frameskip = 1;
    lua_Debug frame;

    if (lua_tostring(L, -1) == 0)
	{
		//dbg_msg("Lua", "PANOS");
        return 0;
	}
	
    //dbg_msg("Lua", pSelf->m_aFilename);
    dbg_msg("Lua", lua_tostring(L, -1));
    dbg_msg("Lua", "Backtrace:");

    while(lua_getstack(L, depth, &frame) == 1)
    {
        depth++;
        lua_getinfo(L, "nlSf", &frame);
        /* check for functions that just report errors. these frames just confuses more then they help */
        if(frameskip && str_comp(frame.short_src, "[C]") == 0 && frame.currentline == -1)
            continue;
        frameskip = 0;
        /* print stack frame */
        dbg_msg("Lua", "%s(%d): %s %s", frame.short_src, frame.currentline, frame.name, frame.namewhat);
    }
    lua_pop(L, 1); // remove error message
    lua_gc(L, LUA_GCCOLLECT, 0);
    return 0;
}
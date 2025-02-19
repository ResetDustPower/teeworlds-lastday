
#include <base/system.h>
#include <engine/shared/config.h>
#include <game/server/gamecontext.h>

#include <string>
#include <pqxx/pqxx>

#include "postgresql.h"

using namespace pqxx;

static LOCK SQLLock = 0;
CGameContext *m_pGameServer;
CGameContext *GameServer() { return m_pGameServer; }
static const char* m_Database;
static const char* m_UserName;
static const char* m_Password;
static const char* m_IP;
static int m_Port;

CPostgresql::CPostgresql(CGameContext *pGameServer)
{
	if(SQLLock == 0)
		SQLLock = lock_create();

	m_pGameServer = pGameServer;
		
	// set Database info
	m_Database = g_Config.m_SvSqlDatabase;
	m_UserName = g_Config.m_SvSqlUser;
	m_Password = g_Config.m_SvSqlPass;
	m_IP = g_Config.m_SvSqlIP;
	m_Port = g_Config.m_SvSqlPort;
}

static void InitThread(void *pUser)
{
    try 
	{
		// Create connection
		char aBuf[256];
        str_format(aBuf, sizeof(aBuf), "dbname = %s user = %s password = %s \
	  hostaddr = %s port = %d", m_Database,
			 m_UserName, m_Password, m_IP, m_Port);
        connection Connection(aBuf);
		
		if (Connection.is_open()) 
		{
			// create tables
			work Work(Connection);
			char Buf[2048];
			str_format(Buf, sizeof(Buf),
			"CREATE TABLE IF NOT EXISTS ld_PlayerAccount "
			"(UserID  serial 	  NOT NULL, "
			"Username VARCHAR(%d) NOT NULL, "
			"Nickname VARCHAR(%d) NOT NULL, "
			"Password VARCHAR(%d) NOT NULL, "
			"Language VARCHAR(16) NOT NULL DEFAULT 'en', "
			"Level BIGINT DEFAULT 0);", MAX_ACCOUNTS_NAME_LENTH, MAX_NAME_LENGTH, MAX_ACCOUNTS_PASSWORD_LENTH);
			Work.exec(Buf);

			str_format(Buf, sizeof(Buf),
			"CREATE TABLE IF NOT EXISTS ld_PlayerItem "
			"(ID serial NOT NULL, "
			"ItemName VARCHAR(128) NOT NULL, "
			"ItemNum BIGINT DEFAULT 0, "
			"OwnerID INT DEFAULT 0);");
			Work.exec(Buf);

			Work.commit();

			dbg_msg("Postgresql", "Created tables");
			
		}
		Connection.disconnect();
	} 
	catch (const std::exception &e)
	{
		dbg_msg("Postgresql", "ERROR: SQL connection failed when init(%s)", e.what());
	}
}

void CPostgresql::Init()
{
	void *init_thread = thread_init(InitThread, nullptr);
	thread_detach(init_thread);
}

static void Register(void *pUser)
{
	lock_wait(SQLLock);

	CTempAccountsData *pData = (CTempAccountsData *)pUser;
	try 
	{
		// Create connection
		char aBuf[256];
        str_format(aBuf, sizeof(aBuf), "dbname = %s user = %s password = %s \
	  hostaddr = %s port = %d", m_Database,
			 m_UserName, m_Password, m_IP, m_Port);
        connection Connection(aBuf);
		
		if (Connection.is_open()) 
		{
			// create tables
			char Buf[2048];
			str_format(Buf, sizeof(Buf), "SELECT * FROM ld_PlayerAccount WHERE Username='%s';", pData->Name);
			work Work(Connection);

			int ClientID = pData->ClientID;
				
			/* Execute SQL query */
			result Result(Work.exec(Buf));
			if(Result.size())
			{
				GameServer()->SendChatTarget(ClientID, _("This account already exists!"));
			}else
			{
				str_format(Buf, sizeof(Buf), "INSERT INTO ld_PlayerAccount(Username, Nickname, Password, Language) VALUES ('%s', '%s', '%s', '%s');", 
					pData->Name, GameServer()->Server()->ClientName(ClientID), pData->Password, GameServer()->m_apPlayers[pData->ClientID]->GetLanguage());
				
				Work.exec(Buf);
				Work.commit();
				GameServer()->SendChatTarget_Locazition(ClientID, "You're register now!");
				
				GameServer()->Postgresql()->CreateLoginThread(pData->Name, pData->Password, ClientID);
			}
			Connection.disconnect();
		}
	} 
	catch (const std::exception &e)
	{
		dbg_msg("Postgresql", "ERROR: SQL connection failed when register (%s)", e.what());
		m_pGameServer->SendChatTarget_Locazition(pData->ClientID, "Register failed!Please ask your server hoster!");
	}

	delete pData;
	lock_unlock(SQLLock);
}

void CPostgresql::CreateRegisterThread(const char *pUserName, const char *pPassword, int ClientID)
{
	CTempAccountsData *Temp = new CTempAccountsData();
	str_copy(Temp->Name, pUserName);
	str_copy(Temp->Password, pPassword);
	Temp->ClientID = ClientID;
	
	void *register_thread = thread_init(Register, Temp);
	thread_detach(register_thread);
}

static void Login(void *pUser)
{
	lock_wait(SQLLock);

	CTempAccountsData *pData = (CTempAccountsData *) pUser;
	try 
	{
		// Create connection
		char aBuf[256];
        str_format(aBuf, sizeof(aBuf), "dbname = %s user = %s password = %s \
	  hostaddr = %s port = %d", m_Database,
			 m_UserName, m_Password, m_IP, m_Port);
        connection Connection(aBuf);
		
		if (Connection.is_open()) 
		{
			// create tables
			char Buf[2048];
			str_format(Buf, sizeof(Buf), "SELECT * FROM ld_PlayerAccount WHERE Username='%s';", pData->Name);
			work Work(Connection);
				
			/* Execute SQL query */
			result Result(Work.exec(Buf));
			Work.commit();
			if(Result.size())
			{
				const char *pNickName = Result.begin()["Nickname"].as<std::string>().c_str();
				if(str_comp(pNickName, GameServer()->Server()->ClientName(pData->ClientID)) == 0)
				{
					if(str_comp(Result.begin()["Password"].as<std::string>().c_str(), pData->Password) == 0)
					{
						GameServer()->m_apPlayers[pData->ClientID]->Login(Result.begin()["UserID"].as<int>());
						GameServer()->m_apPlayers[pData->ClientID]->SetLanguage(Result.begin()["Language"].as<std::string>().c_str());
						GameServer()->SendChatTarget_Locazition(pData->ClientID, "You're login now!");
						
						GameServer()->Postgresql()->CreateSyncItemThread(pData->ClientID);
					}else
					{
						GameServer()->SendChatTarget_Locazition(pData->ClientID, "Wrong password!");
					}
				}else
				{
					GameServer()->SendChatTarget_Locazition(pData->ClientID, "Wrong player nickname!");
				}
			}else
			{
				GameServer()->SendChatTarget_Locazition(pData->ClientID, "This account doesn't exists!");
			}
			Connection.disconnect();
		}
	} 
	catch (const std::exception &e)
	{
		dbg_msg("Postgresql", "ERROR: SQL connection failed when login (%s)", e.what());
		m_pGameServer->SendChatTarget_Locazition(pData->ClientID, "Login failed!Please ask your server hoster!");
	}

	delete pData;
	lock_unlock(SQLLock);
}

void CPostgresql::CreateLoginThread(const char *pUserName, const char *pPassword, int ClientID)
{
	CTempAccountsData *Temp = new CTempAccountsData();
	str_copy(Temp->Name, pUserName);
	str_copy(Temp->Password, pPassword);
	Temp->ClientID = ClientID;
	
	void *login_thread = thread_init(Login, Temp);
	thread_detach(login_thread);
}

static void UpdateItem(void *pUser)
{
	lock_wait(SQLLock);

	CTempItemData *pData = (CTempItemData *)pUser;
	if(pData->UserID > 0)
	{
		try 
		{
			// Create connection
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "dbname = %s user = %s password = %s \
		hostaddr = %s port = %d", m_Database,
				m_UserName, m_Password, m_IP, m_Port);
			connection Connection(aBuf);
			
			if (Connection.is_open()) 
			{
				// create tables
				char Buf[2048];
				str_format(Buf, sizeof(Buf), "SELECT * FROM ld_PlayerItem WHERE OwnerID='%d' AND ItemName='%s';", pData->UserID, pData->Name);
				work Work(Connection);
					
				/* Execute SQL query */
				result Result(Work.exec(Buf));
				if(Result.size())
				{
					str_format(Buf, sizeof(Buf), "UPDATE ld_PlayerItem SET ItemNum=%d WHERE ID='%d';", 
						pData->Num, Result.begin()["ID"].as<int>());
					
					Work.exec(Buf);
					
				}else
				{
					str_format(Buf, sizeof(Buf), "INSERT INTO ld_PlayerItem(ItemName, ItemNum, OwnerID) VALUES ('%s', '%d', '%d');", 
						pData->Name, pData->Num, pData->UserID);
					
					Work.exec(Buf);
				}
				Work.commit();
				Connection.disconnect();
			}
		} 
		catch (const std::exception &e)
		{
			dbg_msg("Postgresql", "ERROR: SQL connection failed when update item (%s)", e.what());
		}
	}

	delete pData;
	lock_unlock(SQLLock);
}

void CPostgresql::CreateUpdateItemThread(int ClientID, const char *pItemName, int Num)
{
	CTempItemData *Temp = new CTempItemData();
	str_copy(Temp->Name, pItemName);
	Temp->Num = Num;
	Temp->UserID = GameServer()->m_apPlayers[ClientID] ? GameServer()->m_apPlayers[ClientID]->GetUserID() : -1;
	Temp->ClientID = ClientID;
	
	void *update_thread = thread_init(UpdateItem, Temp);
	thread_detach(update_thread);
}

static void SyncItem(void *pUser)
{
	lock_wait(SQLLock);

	CTempItemData *pData = (CTempItemData *)pUser;
	if(pData->UserID > 0)
	{
		try 
		{
			// Create connection
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "dbname = %s user = %s password = %s \
		hostaddr = %s port = %d", m_Database,
				m_UserName, m_Password, m_IP, m_Port);
			connection Connection(aBuf);
			
			if (Connection.is_open()) 
			{
				// create tables
				char Buf[2048];
				str_format(Buf, sizeof(Buf), "SELECT * FROM ld_PlayerItem WHERE OwnerID='%d';", pData->UserID);
				work Work(Connection);
					
				/* Execute SQL query */
				result Result(Work.exec(Buf));
				Work.commit();

				if(Result.size())
				{
					for(result::const_iterator i = Result.begin(); i != Result.end(); ++i)
					{
						GameServer()->Item()->SetInvItemNum(i["ItemName"].as<std::string>().c_str(), i["ItemNum"].as<int>(), pData->ClientID, false);
					}
				}
				Connection.disconnect();
			}
		} 
		catch (const std::exception &e)
		{
			dbg_msg("Postgresql", "ERROR: SQL connection failed when sync item (%s)", e.what());
		}
	}

	delete pData;
	lock_unlock(SQLLock);
}

void CPostgresql::CreateSyncItemThread(int ClientID)
{
	CTempItemData *Temp = new CTempItemData();
	Temp->UserID = GameServer()->m_apPlayers[ClientID] ? GameServer()->m_apPlayers[ClientID]->GetUserID() : -1;
	Temp->ClientID = ClientID;
	
	void *sync_thread = thread_init(SyncItem, Temp);
	thread_detach(sync_thread);
}

static void ClearItem(void *pUser)
{
	lock_wait(SQLLock);

	CTempItemData *pData = (CTempItemData *)pUser;
	
	if(pData->UserID > 0)
	{
		try 
		{
			// Create connection
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "dbname = %s user = %s password = %s \
		hostaddr = %s port = %d", m_Database,
				m_UserName, m_Password, m_IP, m_Port);
			connection Connection(aBuf);

			if (Connection.is_open()) 
			{
				// create tables
				char Buf[2048];
				str_format(Buf, sizeof(Buf), "DELETE FROM ld_PlayerItem WHERE OwnerID='%d';", pData->UserID);
				work Work(Connection);
				
				Work.exec(Buf);
				Work.commit();
				Connection.disconnect();
			}
		} 
		catch (const std::exception &e)
		{
			dbg_msg("Postgresql", "ERROR: SQL connection failed when clear item (%s)", e.what());
		}
	}

	delete pData;
	lock_unlock(SQLLock);
}

void CPostgresql::CreateClearItemThread(int ClientID)
{
	CTempItemData *Temp = new CTempItemData();
	Temp->UserID = GameServer()->m_apPlayers[ClientID] ? GameServer()->m_apPlayers[ClientID]->GetUserID() : -1;
	Temp->ClientID = ClientID;
	
	void *clear_thread = thread_init(ClearItem, Temp);
	thread_detach(clear_thread);
}
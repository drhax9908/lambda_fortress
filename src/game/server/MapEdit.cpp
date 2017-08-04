//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: The system for handling Lambda Fortress's MapEdit.
//
// Heavily inspired by Synergy's MapEdit and completely based on the Commentary System.
//
// (Blxibon)
//
// $NoKeywords: $
//=============================================================================//

#include "cbase.h"

#include "filesystem.h"

#ifndef CLIENT_DLL
#include <KeyValues.h>
#include "utldict.h"
#include "isaverestore.h"
#include "eventqueue.h"
#include "saverestore_utlvector.h"
#include "ai_basenpc.h"
#include "triggers.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#ifndef CLIENT_DLL

#define MAPEDIT_SPAWNED_SEMAPHORE		"mapedit_semaphore"
#define MAPEDIT_DEFAULT_FILE		"maps/%s_auto.txt"

ConVar mapedit_enabled("mapedit_enabled", "1", FCVAR_ARCHIVE, "Is automatic MapEdit enabled?");
ConVar mapedit_stack("mapedit_stack", "1", FCVAR_ARCHIVE, "If multiple MapEdit scripts are loaded, should they stack or replace each other?");
ConVar mapedit_debug("mapedit_debug", "0", FCVAR_NONE, "Should MapEdit give debug messages?");

// Convar restoration save/restore
#define MAX_MODIFIED_CONVAR_STRING		128
#define modifiedconvars_t mapedit_modifiedconvars_t
struct mapedit_modifiedconvars_t 
{
	DECLARE_SIMPLE_DATADESC();

	char pszConvar[MAX_MODIFIED_CONVAR_STRING];
	char pszCurrentValue[MAX_MODIFIED_CONVAR_STRING];
	char pszOrgValue[MAX_MODIFIED_CONVAR_STRING];
};

void DebugMsg(const tchar *pMsg, ...)
{
	if (mapedit_debug.GetBool() == true)
	{
		Msg(pMsg);
	}
}

// Used in some debug messages.
const char *DebugGetSecondaryName(const char *pName, CBaseEntity *pEntity)
{
	if (!pName || !pEntity)
		return NULL;

	const char *sSecondaryName = "<null>";
	if (!Q_stricmp(pName, STRING(pEntity->GetEntityName())))
	{
		// Use classname, if unavailable use entity index
		if (pEntity->GetClassname())
			sSecondaryName = pEntity->GetClassname();
		else
			sSecondaryName = UTIL_VarArgs("index: %i", pEntity->entindex());
	}
	else if (!Q_stricmp(pName, pEntity->GetClassname()))
	{
		// Use entity name, if unavailable use entity index
		if (pEntity->GetEntityName() != NULL_STRING)
			sSecondaryName = STRING(pEntity->GetEntityName());
		else
			sSecondaryName = UTIL_VarArgs("index: %i", pEntity->entindex());
	}
	else if (atoi(pName) && UTIL_EntityByIndex(atoi(pName)))
	{
		if (pEntity->GetClassname())
			sSecondaryName = pEntity->GetClassname();
		else
			sSecondaryName = UTIL_VarArgs("index: %i", pEntity->entindex());
	}

	return sSecondaryName;
}

//bool g_bMapEditAvailable;
bool g_bMapEditLoaded = false;
bool IsMapEditLoaded( void )
{
	return g_bMapEditLoaded;
}

bool IsAutoMapEditAllowed(void)
{
	return mapedit_enabled.GetBool();
}

void CV_GlobalChange_MapEdit( IConVar *var, const char *pOldString, float flOldValue );

//-----------------------------------------------------------------------------
// Purpose: Game system to kickstart the director's commentary
//-----------------------------------------------------------------------------
class CMapEdit : public CAutoGameSystemPerFrame
{
public:
	DECLARE_DATADESC();

	//CMapEdit() : CAutoGameSystemPerFrame( "CMapEdit" )
	//{
	//	m_iCommentaryNodeCount = 0;
	//}

	virtual void LevelInitPreEntity()
	{
		m_bMapEditConvarsChanging = false;

		Msg("MapEdit: ANY TIME NOW!\n");

		CalculateAvailableState();
	}

	void CalculateAvailableState( void )
	{
		///*
		// Set the available cvar if we can find commentary data for this level
		char szFullName[512];
		Q_snprintf(szFullName,sizeof(szFullName), "maps/%s_auto.txt", STRING( gpGlobals->mapname) );
		if ( filesystem->FileExists( szFullName ) )
		{
			bool bAllowed = IsAutoMapEditAllowed();
			Msg("MapEdit: EDIT AVAILABLE%s\n", bAllowed ? "!" : ", BUT NOT ALLOWED!");
			g_bMapEditLoaded = bAllowed;
		}
		else
		{
			Warning("MapEdit: EDIT UNAVAILABLE!\n");
			g_bMapEditLoaded = false;
		}
		//*/
	}

	virtual void LevelShutdownPreEntity()
	{
		ShutDownMapEdit();
	}

	void ParseEntKVBlock( CBaseEntity *pNode, KeyValues *pkvNode )
	{
		KeyValues *pkvNodeData = pkvNode->GetFirstSubKey();
		while ( pkvNodeData )
		{
			// Handle the connections block
			if ( !Q_strcmp(pkvNodeData->GetName(), "connections") )
			{
				ParseEntKVBlock( pNode, pkvNodeData );
			}
			else
			{ 
				#define COMMENTARY_STRING_LENGTH_MAX		1024

				const char *pszValue = pkvNodeData->GetString();
				Assert( Q_strlen(pszValue) < COMMENTARY_STRING_LENGTH_MAX );
				if ( Q_strnchr(pszValue, '^', COMMENTARY_STRING_LENGTH_MAX) )
				{
					// We want to support quotes in our strings so that we can specify multiple parameters in
					// an output inside our commentary files. We convert ^s to "s here.
					char szTmp[COMMENTARY_STRING_LENGTH_MAX];
					Q_strncpy( szTmp, pszValue, COMMENTARY_STRING_LENGTH_MAX );
					int len = Q_strlen( szTmp );
					for ( int i = 0; i < len; i++ )
					{
						if ( szTmp[i] == '^' )
						{
							szTmp[i] = '"';
						}
					}

					pNode->KeyValue( pkvNodeData->GetName(), szTmp );
				}
				else
				{
					pNode->KeyValue( pkvNodeData->GetName(), pszValue );
				}
			}

			pkvNodeData = pkvNodeData->GetNextKey();
		}
	}

	virtual void LevelInitPostEntity( void )
	{
		if ( !IsMapEditLoaded() )
		{
			Warning("MapEdit: LevelInitPostEntity says unloaded!\n");
			return;
		}

		// Don't spawn entities when loading a savegame
		if ( gpGlobals->eLoadType == MapLoad_LoadGame || gpGlobals->eLoadType == MapLoad_Background )
		{
			Warning("MapEdit: Game loaded as save or background!\n");
			return;
		}

		Msg("MapEdit: LevelInitPostEntity successful\n");
		m_bMapEditLoadedMidGame = false;
		InitMapEdit();

		//IGameEvent *event = gameeventmanager->CreateEvent( "playing_mapedit" );
		//gameeventmanager->FireEventClientSide( event );
	}

	bool MapEditConvarsChanging( void )
	{
		return m_bMapEditConvarsChanging;
	}

	void SetMapEditConvarsChanging( bool bChanging )
	{
		m_bMapEditConvarsChanging = bChanging;
	}

	void ConvarChanged( IConVar *pConVar, const char *pOldString, float flOldValue )
	{
		ConVarRef var( pConVar );

		// A convar has been changed by a commentary node. We need to store
		// the old state. If the engine shuts down, we need to restore any
		// convars that the commentary changed to their previous values.
		for ( int i = 0; i < m_ModifiedConvars.Count(); i++ )
		{
			// If we find it, just update the current value
			if ( !Q_strncmp( var.GetName(), m_ModifiedConvars[i].pszConvar, MAX_MODIFIED_CONVAR_STRING ) )
			{
				Q_strncpy( m_ModifiedConvars[i].pszCurrentValue, var.GetString(), MAX_MODIFIED_CONVAR_STRING );
				//Msg("    Updating Convar %s: value %s (org %s)\n", m_ModifiedConvars[i].pszConvar, m_ModifiedConvars[i].pszCurrentValue, m_ModifiedConvars[i].pszOrgValue );
				return;
			}
		}

		// We didn't find it in our list, so add it
		modifiedconvars_t newConvar;
		Q_strncpy( newConvar.pszConvar, var.GetName(), MAX_MODIFIED_CONVAR_STRING );
		Q_strncpy( newConvar.pszCurrentValue, var.GetString(), MAX_MODIFIED_CONVAR_STRING );
		Q_strncpy( newConvar.pszOrgValue, pOldString, MAX_MODIFIED_CONVAR_STRING );
		m_ModifiedConvars.AddToTail( newConvar );

		/*
		Msg(" Commentary changed '%s' to '%s' (was '%s')\n", var->GetName(), var->GetString(), pOldString );
		Msg(" Convars stored: %d\n", m_ModifiedConvars.Count() );
		for ( int i = 0; i < m_ModifiedConvars.Count(); i++ )
		{
			Msg("    Convar %d: %s, value %s (org %s)\n", i, m_ModifiedConvars[i].pszConvar, m_ModifiedConvars[i].pszCurrentValue, m_ModifiedConvars[i].pszOrgValue );
		}
		*/
	}

	CBaseEntity *FindMapEditEntity( CBaseEntity *pStartEntity, const char *szName, const char *szValue = NULL )
	{
		CBaseEntity *pEntity = NULL;
		DebugMsg("MapEdit Find Debug: Starting Search, Name: %s, Value: %s\n", szName, szValue);

		// First, find by targetname/classname
		pEntity = gEntList.FindEntityGeneric(pStartEntity, szName);

		if (!pEntity)
		{
			DebugMsg("MapEdit Find Debug: \"%s\" not found as targetname or classname\n", szName);

			if (!Q_stricmp(szName, "find_changelevel") && szValue)
			{
				// Find trigger_changelevel(s) with matching map name
				DebugMsg("MapEdit Find Debug: \"%s\" is find_changelevel, searching trigger_changelevel\n", szValue);
				pEntity = gEntList.FindEntityByClassname(pStartEntity, "trigger_changelevel");
				while (pEntity)
				{
					CChangeLevel *pChangeLevel = dynamic_cast<CChangeLevel*>(pEntity);
					if (!pChangeLevel)
					{
						pEntity = gEntList.FindEntityByClassname(pEntity, "trigger_changelevel");
						continue;
					}

					if (pChangeLevel->GetMapName() && !Q_stricmp(pChangeLevel->GetMapName(), szValue))
					{
						DebugMsg("MapEdit Find Debug: \"%s\" found as mapname in trigger_changelevel\n", szValue);
						return pEntity;
					}
					else if (pChangeLevel->GetLandmark() && !Q_stricmp(pChangeLevel->GetMapName(), szValue))
					{
						DebugMsg("MapEdit Find Debug: \"%s\" found as landmark in trigger_changelevel\n", szValue);
						return pEntity;
					}

					pEntity = gEntList.FindEntityByClassname(pEntity, "trigger_changelevel");
				}
			}

			if (!Q_stricmp(szName, "origin") && szValue)
			{
				// Find entities at this origin
				Vector vecOrigin = Vector(*szValue);
				if (vecOrigin.IsValid())
				{
					DebugMsg("MapEdit Find Debug: \"%s\" is valid vector\n", szValue);
					//gEntList.FindEntityInSphere(pStartEntity, vecOrigin, 1);

					// This might seem a bit gross, especially since it's not in its own EntList function,
					// but it ensures we don't get the wrong entity with FindEntityInSphere or add an otherwise useless function.
					const CEntInfo *pInfo = pStartEntity ? gEntList.GetEntInfoPtr(pStartEntity->GetRefEHandle())->m_pNext : gEntList.FirstEntInfo();
					for (; pInfo; pInfo = pInfo->m_pNext)
					{
						CBaseEntity *ent = (CBaseEntity *)pInfo->m_pEntity;
						if (!ent)
						{
							DevWarning("NULL entity in global entity list!\n");
							continue;
						}

						if (!ent->edict())
							continue;

						if (ent->GetLocalOrigin() == vecOrigin)
							return ent;
					}
				}
			}

			// Try the entity index
			int iEntIndex = atoi(szName);
			if (!pStartEntity && iEntIndex && UTIL_EntityByIndex(iEntIndex))
			{
				DebugMsg("MapEdit Find Debug: \"%s\" is valid index\n", szName);
				return CBaseEntity::Instance(iEntIndex);
			}

			DebugMsg("MapEdit Find Debug: \"%s\" not found\n", szName);
			return NULL;
		}

		DebugMsg("MapEdit Find Debug: \"%s\" found as targetname or classname\n", szName);
		return pEntity;
	}

	void InitMapEdit( const char* pFile = MAPEDIT_DEFAULT_FILE )
	{
		// Install the global cvar callback
		cvar->InstallGlobalChangeCallback( CV_GlobalChange_MapEdit );


		// If we find the commentary semaphore, the commentary entities already exist.
		// This occurs when you transition back to a map that has saved commentary nodes in it.
		if ( gEntList.FindEntityByName( NULL, MAPEDIT_SPAWNED_SEMAPHORE ) )
			return;

		// Spawn the commentary semaphore entity
		CBaseEntity *pSemaphore = CreateEntityByName( "info_target" );
		pSemaphore->SetName( MAKE_STRING(MAPEDIT_SPAWNED_SEMAPHORE) );

		bool oldLock = engine->LockNetworkStringTables( false );

		SpawnMapEdit(pFile);

		engine->LockNetworkStringTables( oldLock );
	}

	void SpawnMapEdit(const char *pFile = NULL)
	{
		// Find the commentary file
		char szFullName[512];
		if (pFile == NULL)
		{
			DebugMsg("MapEdit Debug: NULL file, loading default\n");
			Q_snprintf(szFullName,sizeof(szFullName), "maps/%s_auto.txt", STRING( gpGlobals->mapname ));
		}
		else
		{
			DebugMsg("MapEdit Debug: File not NULL, loading %s\n", pFile);
			Q_snprintf(szFullName,sizeof(szFullName), pFile);
		}
		KeyValues *pkvFile = new KeyValues( "MapEdit" );
		if ( pkvFile->LoadFromFile( filesystem, szFullName, "MOD" ) )
		{
			Msg( "MapEdit: Loading MapEdit data from %s. \n", szFullName );

			// Load each commentary block, and spawn the entities
			KeyValues *pkvNode = pkvFile->GetFirstSubKey();
			while ( pkvNode )
			{
				// Starts as name of pkvNode (e.g. "create")
				const char *pNodeName = pkvNode->GetName();

				// Skip the trackinfo
				if ( !Q_strncmp( pNodeName, "trackinfo", 9 ) )
				{
					pkvNode = pkvNode->GetNextKey();
					continue;
				}

				if (!Q_stricmp( pNodeName, "create" ) || !Q_stricmp( pNodeName, "entity" ))
				{
					KeyValues *pClassname = pkvNode->GetFirstSubKey();
					while (pClassname)
					{
						// Turns into the classname of the entity to create
						pNodeName = pClassname->GetName();

						// Spawns the entity
						CBaseEntity *pNode = CreateEntityByName(pNodeName);
						if (pNode)
						{
							ParseEntKVBlock(pNode, pClassname);
							DispatchSpawn(pNode);

							EHANDLE hHandle;
							hHandle = pNode;
							m_hSpawnedEntities.AddToTail(hHandle);
							DebugMsg("MapEdit Debug: Spawned entity %s\n", pNodeName);
						}
						else
						{
							Warning("MapEdit: Failed to spawn mapedit entity, type: '%s'\n", pNodeName);
						}

						// Gets the next key
						pClassname = pClassname->GetNextKey();
					}
					pClassname->deleteThis();
				}

				if (!Q_stricmp( pNodeName, "edit" ))
				{
					KeyValues *pName = pkvNode->GetFirstSubKey();
					while (pName)
					{
						// Turns into the name of the class to edit
						pNodeName = pName->GetName();

						CBaseEntity *pNode = NULL;

						pNode = FindMapEditEntity(NULL, pNodeName, pName->GetString());
						/*
						// First, try finding it by targetname/classname
						pNode = gEntList.FindEntityGeneric(NULL, pNodeName);

						if (!pNode)
						{
							// If null, try the entity index
							int iEntIndex = atoi(pNodeName);
							if (iEntIndex && UTIL_EntityByIndex(iEntIndex))
							{
								pNode = CBaseEntity::Instance(iEntIndex);
							}
						}
						*/

						if (pNode)
						{
							while (pNode)
							{
								if (mapedit_debug.GetBool() == true)
								{
									// Using this instead of DebugMsg to save resources from DebugGetSecondaryName
									Msg("MapEdit Debug: Editing %s (%s)\n", pNodeName, DebugGetSecondaryName(pNodeName, pNode));
								}

								ParseEntKVBlock(pNode, pName);
								pNode = FindMapEditEntity(pNode, pNodeName, pName->GetString());
								//pNode = gEntList.FindEntityGeneric(pNode, pNodeName);
							}
						}

						pName = pName->GetNextKey();
					}
					pName->deleteThis();
				}

				if (!Q_stricmp( pNodeName, "delete" ))
				{
					KeyValues *pName = pkvNode->GetFirstSubKey();
					while (pName)
					{
						pNodeName = pName->GetName();

						CBaseEntity *pNode = NULL;

						pNode = FindMapEditEntity(NULL, pNodeName, pName->GetString());
						/*
						// First, try finding it by targetname/classname
						pNode = gEntList.FindEntityGeneric(NULL, pNodeName);

						if (!pNode)
						{
							// If null, try the entity index
							int iEntIndex = pName->GetInt();
							if (iEntIndex)
							{
								pNode = CBaseEntity::Instance(iEntIndex);
							}
						}
						*/

						if (pNode)
						{
							while (pNode)
							{
								if (mapedit_debug.GetBool() == true)
								{
									// Using this instead of DebugMsg to save resources from DebugGetSecondaryName
									Msg("MapEdit Debug: Deleting %s (%s)\n", pNodeName, DebugGetSecondaryName(pNodeName, pNode));
								}
								UTIL_Remove(pNode);
								//pNode = gEntList.FindEntityGeneric(pNode, pNodeName);
								pNode = FindMapEditEntity(pNode, pNodeName, pName->GetString());
							}
						}

						pName = pName->GetNextKey();
					}
					pName->deleteThis();
				}
				
				if (!Q_stricmp( pNodeName, "console" ))
				{
					KeyValues *pkvNodeData = pkvNode->GetFirstSubKey();
					const char *pKey;
					const char *pValue;
					while (pkvNodeData)
					{
						SetMapEditConvarsChanging(true);

						pKey = pkvNodeData->GetName();
						pValue = pkvNodeData->GetString();

						engine->ServerCommand(UTIL_VarArgs("%s %s", pKey, pValue));
						engine->ServerCommand("mapedit_cvarsnotchanging\n");

						pkvNodeData = pkvNodeData->GetNextKey();
					}
					pkvNodeData->deleteThis();
				}
				
				pkvNode = pkvNode->GetNextKey();
			}

			pkvNode->deleteThis();

			// Then activate all the entities
			for ( int i = 0; i < m_hSpawnedEntities.Count(); i++ )
			{
				m_hSpawnedEntities[i]->Activate();
			}
		}
		else
		{
			Msg( "MapEdit: Could not find mapedit data file '%s'. \n", szFullName );
		}

		pkvFile->deleteThis();
	}

	void ShutDownMapEdit( void )
	{
		// Destroy all the entities created by commentary
		for ( int i = m_hSpawnedEntities.Count()-1; i >= 0; i-- )
		{
			if ( m_hSpawnedEntities[i] )
			{
				UTIL_Remove( m_hSpawnedEntities[i] );
			}
		}
		m_hSpawnedEntities.Purge();

		// Remove the semaphore
		CBaseEntity *pSemaphore = gEntList.FindEntityByName( NULL, MAPEDIT_SPAWNED_SEMAPHORE );
		if ( pSemaphore )
		{
			UTIL_Remove( pSemaphore );
		}

		// Remove our global convar callback
		cvar->RemoveGlobalChangeCallback( CV_GlobalChange_MapEdit );

		// Reset any convars that have been changed by the commentary
		for ( int i = 0; i < m_ModifiedConvars.Count(); i++ )
		{
			ConVar *pConVar = (ConVar *)cvar->FindVar( m_ModifiedConvars[i].pszConvar );
			if ( pConVar )
			{
				pConVar->SetValue( m_ModifiedConvars[i].pszOrgValue );
			}
		}
		m_ModifiedConvars.Purge();
	}

	void SetMapEdit( bool bMapEdit, const char *pFile = NULL )
	{
		//g_bMapEditLoaded = bMapEdit;
		//CalculateAvailableState();

		// If we're turning on commentary, create all the entities.
		if ( bMapEdit )
		{
			if (filesystem->FileExists(pFile) || pFile == NULL)
			{
				g_bMapEditLoaded = true;
				m_bMapEditLoadedMidGame = true;
				InitMapEdit(pFile);
			}
			else
			{
				Warning("MapEdit: No such file \"%s\"!\n", pFile);
			}
		}
		else
		{
			ShutDownMapEdit();
		}
	}

	void OnRestore( void )
	{
		cvar->RemoveGlobalChangeCallback( CV_GlobalChange_MapEdit );

		if ( !IsMapEditLoaded() )
			return;

		// Set any convars that have already been changed by the commentary before the save
		for ( int i = 0; i < m_ModifiedConvars.Count(); i++ )
		{
			ConVar *pConVar = (ConVar *)cvar->FindVar( m_ModifiedConvars[i].pszConvar );
			if ( pConVar )
			{
				//Msg("    Restoring Convar %s: value %s (org %s)\n", m_ModifiedConvars[i].pszConvar, m_ModifiedConvars[i].pszCurrentValue, m_ModifiedConvars[i].pszOrgValue );
				pConVar->SetValue( m_ModifiedConvars[i].pszCurrentValue );
			}
		}

		// Install the global cvar callback
		cvar->InstallGlobalChangeCallback( CV_GlobalChange_MapEdit );
	}

	bool MapEditLoadedMidGame( void ) 
	{
		return m_bMapEditLoadedMidGame;
	}

private:
	bool	m_bMapEditConvarsChanging;
	bool	m_bMapEditLoadedMidGame;

	CUtlVector< modifiedconvars_t > m_ModifiedConvars;
	CUtlVector<EHANDLE>				m_hSpawnedEntities;
};

CMapEdit	g_MapEdit;

BEGIN_DATADESC_NO_BASE( CMapEdit )
	//int m_afPlayersLastButtons;			DON'T SAVE
	//bool m_bCommentaryConvarsChanging;	DON'T SAVE
	//int m_iClearPressedButtons;			DON'T SAVE

	DEFINE_FIELD( m_bMapEditLoadedMidGame, FIELD_BOOLEAN ),

	DEFINE_UTLVECTOR( m_ModifiedConvars, FIELD_EMBEDDED ),
	DEFINE_UTLVECTOR( m_hSpawnedEntities, FIELD_EHANDLE ),
END_DATADESC()

BEGIN_SIMPLE_DATADESC( modifiedconvars_t )
	DEFINE_ARRAY( pszConvar, FIELD_CHARACTER, MAX_MODIFIED_CONVAR_STRING ),
	DEFINE_ARRAY( pszCurrentValue, FIELD_CHARACTER, MAX_MODIFIED_CONVAR_STRING ),
	DEFINE_ARRAY( pszOrgValue, FIELD_CHARACTER, MAX_MODIFIED_CONVAR_STRING ),
END_DATADESC()

//-----------------------------------------------------------------------------
// Purpose: We need to revert back any convar changes that are made by the
//			commentary system during commentary. This code stores convar changes
//			made by the commentary system, and reverts them when finished.
//-----------------------------------------------------------------------------
void CV_GlobalChange_MapEdit( IConVar *var, const char *pOldString, float flOldValue )
{
	if ( !g_MapEdit.MapEditConvarsChanging() )
	{
		// A convar has changed, but not due to mapedit. Ignore it.
		return;
	}

	g_MapEdit.ConvarChanged( var, pOldString, flOldValue );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CC_MapEditNotChanging( void )
{
	g_MapEdit.SetMapEditConvarsChanging( false );
}
static ConCommand mapedit_cvarsnotchanging( "mapedit_cvarsnotchanging", CC_MapEditNotChanging, 0 );

//-----------------------------------------------------------------------------
// Purpose: MapEdit specific logic_auto replacement.
//			Fires outputs based upon how MapEdit has been activated.
//-----------------------------------------------------------------------------
class CMapEditAuto : public CBaseEntity
{
	DECLARE_CLASS( CMapEditAuto, CBaseEntity );
public:
	DECLARE_DATADESC();

	void Spawn(void);
	void Think(void);

private:
	// fired if loaded due to new map
	COutputEvent m_OnMapEditNewGame;

	// fired if loaded in the middle of a map
	COutputEvent m_OnMapEditMidGame;
};

LINK_ENTITY_TO_CLASS(mapedit_auto, CMapEditAuto);

BEGIN_DATADESC( CMapEditAuto )
	// Outputs
	DEFINE_OUTPUT(m_OnMapEditNewGame, "OnMapEditNewGame"),
	DEFINE_OUTPUT(m_OnMapEditMidGame, "OnMapEditMidGame"),
END_DATADESC()

//------------------------------------------------------------------------------
// Purpose : Fire my outputs here if I fire on map reload
//------------------------------------------------------------------------------
void CMapEditAuto::Spawn(void)
{
	BaseClass::Spawn();
	SetNextThink( gpGlobals->curtime + 0.1 );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMapEditAuto::Think(void)
{
	if ( g_MapEdit.MapEditLoadedMidGame() )
	{
		m_OnMapEditMidGame.FireOutput(NULL, this);
	}
	else
	{
		m_OnMapEditNewGame.FireOutput(NULL, this);
	}
}

//------------------------------------------------------------------------------
// Purpose : 
// Input   :
// Output  :
//------------------------------------------------------------------------------
void CC_MapEdit_Load( const CCommand& args )
{
	const char *sFile = args[1] ? args[1] : NULL;

	if (!filesystem->FileExists(sFile))
	{
		Warning("MapEdit: No such file \"%s\"!\n", sFile);
		return;
	}
	
	if (IsMapEditLoaded())
	{
		if (mapedit_stack.GetBool() == 1)
		{
			g_MapEdit.SpawnMapEdit(sFile);
			return;
		}
		else
		{
			g_MapEdit.SetMapEdit(false);
		}
	}

	g_MapEdit.SetMapEdit(true, sFile);
}
static ConCommand mapedit_load("mapedit_load", CC_MapEdit_Load, "Forces mapedit to load a specific file. If there is no input value, it will load the map's default file.", FCVAR_CHEAT);

//------------------------------------------------------------------------------
// Purpose : Unloads all MapEdit entities. Does not undo modifications or deletions.
// Input   :
// Output  :
//------------------------------------------------------------------------------
void CC_MapEdit_Unload( const CCommand& args )
{
	g_MapEdit.SetMapEdit(false);
}
static ConCommand mapedit_unload("mapedit_unload", CC_MapEdit_Unload, "Forces mapedit to unload.", FCVAR_CHEAT);

//------------------------------------------------------------------------------
// Purpose : 
// Input   :
// Output  :
//------------------------------------------------------------------------------
void CC_MapEdit_Autoload( const CCommand& args )
{
	g_MapEdit.ShutDownMapEdit();

	g_MapEdit.LevelInitPreEntity();
}
static ConCommand mapedit_autoload("mapedit_autoload", CC_MapEdit_Autoload, "This command is supposed to be used automatically for when the round restarts.", FCVAR_HIDDEN);

#else

//------------------------------------------------------------------------------
// Purpose : Prints MapEdit data from a specific file to the console.
// Input   : The file to print
// Output  : The file's data
//------------------------------------------------------------------------------
void CC_MapEdit_Print( const CCommand& args )
{
	const char *szFullName = args[1];
	if (szFullName && filesystem->FileExists(szFullName))
	{
		KeyValues *pkvFile = new KeyValues( "MapEdit" );
		if ( pkvFile->LoadFromFile( filesystem, szFullName, "MOD" ) )
		{
			Msg( "MapEdit: Printing MapEdit data from %s. \n", szFullName );

			// Load each commentary block, and spawn the entities
			KeyValues *pkvNode = pkvFile->GetFirstSubKey();
			while ( pkvNode )
			{
				// Get node name
				const char *pNodeName = pkvNode->GetName();
				Msg("- Section Name: %s\n", pNodeName);

				// Skip the trackinfo
				if ( !Q_strncmp( pNodeName, "trackinfo", 9 ) )
				{
					pkvNode = pkvNode->GetNextKey();
					continue;
				}

				KeyValues *pClassname = pkvNode->GetFirstSubKey();
				while (pClassname)
				{
					// Use the classname instead
					pNodeName = pClassname->GetName();

					Msg("     %s\n", pNodeName);
					for ( KeyValues *sub = pClassname->GetFirstSubKey(); sub; sub = sub->GetNextKey() )
					{
						if (!Q_strcmp(sub->GetName(), "connections"))
						{
							Msg("-         connections\n");
							for ( KeyValues *sub2 = sub->GetFirstSubKey(); sub2; sub2 = sub2->GetNextKey() )
							{
								Msg("-              \"%s\", \"%s\"\n", sub2->GetName(), sub2->GetString());
							}
							continue;
						}
						Msg("-         %s, %s\n", sub->GetName(), sub->GetString());
					}

					pClassname = pClassname->GetNextKey();
				}
				
				pkvNode = pkvNode->GetNextKey();
			}

			pkvNode->deleteThis();
		}
	}
}
static ConCommand mapedit_print("mapedit_print", CC_MapEdit_Print, "Prints a mapedit file in the console.");

#endif

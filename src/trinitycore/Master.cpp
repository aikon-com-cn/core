/*
 * Copyright (C) 2005-2008 MaNGOS <http://www.mangosproject.org/>
 *
 * Copyright (C) 2008 Trinity <http://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/** \file
    \ingroup Trinityd
*/

#include <ace/OS_NS_signal.h>

#include "WorldSocketMgr.h"
#include "Common.h"
#include "Master.h"
#include "WorldSocket.h"
#include "WorldRunnable.h"
#include "World.h"
#include "Log.h"
#include "Timer.h"
#include "Policies/SingletonImp.h"
#include "SystemConfig.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "CliRunnable.h"
#include "ScriptCalls.h"
#include "Util.h"
#include "InstanceSaveMgr.h"

#ifdef WIN32
#include "ServiceWin32.h"
#else
#include "vmap/BIH.h"
#include "PosixDaemon.h"
#endif

extern RunModes runMode;

/// \todo Warning disabling not useful under VC++2005. Can somebody say on which compiler it is useful?
#pragma warning(disable:4305)

INSTANTIATE_SINGLETON_1( Master );

volatile uint32 Master::m_masterLoopCounter = 0;
#ifndef WIN32
volatile bool BIH::possibleFreeze = false;
#endif

class FreezeDetectorRunnable : public ACE_Based::Runnable
{
public:
    FreezeDetectorRunnable() { _delaytime = 0; }
    uint32 m_loops, m_lastchange;
    uint32 w_loops, w_lastchange;
    uint32 _delaytime;
    uint32 maxVmapCalcTime;
    void SetDelayTime(uint32 t) { _delaytime = t; }
    void run(void)
    {
        if(!_delaytime)
            return;
        sLog.outString("Starting up anti-freeze thread (%u seconds max stuck time)...",_delaytime/1000);
        m_loops = 0;
        w_loops = 0;
        m_lastchange = 0;
        w_lastchange = 0;
        #ifndef WIN32
        maxVmapCalcTime = sConfig.GetIntDefault("MaxVmapCalcTime", 30);
        maxVmapCalcTime *= 1000;
        #endif
        while(!World::IsStopped())
        {
            ACE_Based::Thread::Sleep(1000);
            uint32 curtime = WorldTimer::getMSTime();
            //DEBUG_LOG("anti-freeze: time=%u, counters=[%u; %u]",curtime,Master::m_masterLoopCounter,World::m_worldLoopCounter);

            // There is no Master anymore
            // TODO: clear the rest of the code
//            // normal work
//            if(m_loops != Master::m_masterLoopCounter)
//            {
//                m_lastchange = curtime;
//                m_loops = Master::m_masterLoopCounter;
//            }
//            // possible freeze
//            else if(WorldTimer::getMSTimeDiff(m_lastchange,curtime) > _delaytime)
//            {
//                sLog.outError("Main/Sockets Thread hangs, kicking out server!");
//                *((uint32 volatile*)NULL) = 0;                       // bang crash
//            }

            // normal work
            if(w_loops != World::m_worldLoopCounter)
            {
                w_lastchange = curtime;
                w_loops = World::m_worldLoopCounter;
                #ifndef WIN32
                BIH::possibleFreeze = false;
                #endif
            }
            // possible freeze
            else
            {
                if(WorldTimer::getMSTimeDiff(w_lastchange,curtime) > _delaytime)
                {
                    sLog.outError("World Thread hangs, kicking out server!");
                    *((uint32 volatile*)NULL) = 0;                       // bang crash
                }
                #ifndef WIN32
                else
                {
                    if (WorldTimer::getMSTimeDiff(w_lastchange,curtime) > maxVmapCalcTime && !BIH::possibleFreeze)
                    {
                        sLog.outError("Freeze Detector: World Thread hangs, trying prevent VMAP Freeze.");
                        BIH::possibleFreeze = true;
                    }
                }
                #endif
            }
        }
        sLog.outString("Anti-freeze thread exiting without problems.");
    }
};

Master::Master()
{
}

Master::~Master()
{
}

/// Main function
int Master::Run()
{
    sLog.outString( "%s (core-daemon)", _FULLVERSION );
    sLog.outString( "<Ctrl-C> to stop.\n" );

    sLog.outTitle( " ______                       __");
    sLog.outTitle( "/\\__  _\\       __          __/\\ \\__");
    sLog.outTitle( "\\/_/\\ \\/ _ __ /\\_\\    ___ /\\_\\ \\ ,_\\  __  __");
    sLog.outTitle( "   \\ \\ \\/\\`'__\\/\\ \\ /' _ `\\/\\ \\ \\ \\/ /\\ \\/\\ \\");
    sLog.outTitle( "    \\ \\ \\ \\ \\/ \\ \\ \\/\\ \\/\\ \\ \\ \\ \\ \\_\\ \\ \\_\\ \\");
    sLog.outTitle( "     \\ \\_\\ \\_\\  \\ \\_\\ \\_\\ \\_\\ \\_\\ \\__\\\\/`____ \\");
    sLog.outTitle( "      \\/_/\\/_/   \\/_/\\/_/\\/_/\\/_/\\/__/ `/___/> \\");
    sLog.outTitle( "                                 C O R E  /\\___/");
    sLog.outTitle( "http://TrinityCore.org                    \\/__/\n");

    /// worldd PID file creation
    std::string pidfile = sConfig.GetStringDefault("PidFile", "");
    if(!pidfile.empty())
    {
        uint32 pid = CreatePIDFile(pidfile);
        if( !pid )
        {
            sLog.outError( "Cannot create PID file %s.\n", pidfile.c_str() );
            return 1;
        }

        sLog.outString( "Daemon PID: %u\n", pid );
    }

#ifndef WIN32
    detachDaemon();
#endif

    ///- Start the databases
    if (!_StartDB())
        return 1;

    ///- Initialize the World
    sWorld.SetInitialWorldSettings();
    //server loaded successfully => enable async DB requests
    //this is done to forbid any async transactions during server startup!
    CharacterDatabase.AllowAsyncTransactions();
    WorldDatabase.AllowAsyncTransactions();
    LoginDatabase.AllowAsyncTransactions();

    ///- Catch termination signals
    _HookSignals();

    ///- Launch WorldRunnable thread
    ACE_Based::Thread t(new WorldRunnable);
    t.setPriority(ACE_Based::Highest);

    // set server online
    LoginDatabase.PExecute("UPDATE realmlist SET color = 0, population = 0 WHERE id = '%d'",realmID);

    // console should be disabled in service/daemon mode
    if (sConfig.GetBoolDefault("Console.Enable", true) && (runMode == MODE_NORMAL))
    {
        ///- Launch CliRunnable thread
        ACE_Based::Thread td1(new CliRunnable);
    }

    ///- Handle affinity for multiple processors and process priority on Windows
    #ifdef WIN32
    {
        HANDLE hProcess = GetCurrentProcess();

        uint32 Aff = sConfig.GetIntDefault("UseProcessors", 0);
        if(Aff > 0)
        {
            ULONG_PTR appAff;
            ULONG_PTR sysAff;

            if(GetProcessAffinityMask(hProcess,&appAff,&sysAff))
            {
                ULONG_PTR curAff = Aff & appAff;            // remove non accessible processors

                if(!curAff )
                {
                    sLog.outError("Processors marked in UseProcessors bitmask (hex) %x not accessible for Trinityd. Accessible processors bitmask (hex): %x",Aff,appAff);
                }
                else
                {
                    if(SetProcessAffinityMask(hProcess,curAff))
                        sLog.outString("Using processors (bitmask, hex): %x", curAff);
                    else
                        sLog.outError("Can't set used processors (hex): %x",curAff);
                }
            }
            sLog.outString();
        }

        bool Prio = sConfig.GetBoolDefault("ProcessPriority", false);

//        if(Prio && (m_ServiceStatus == -1)/* need set to default process priority class in service mode*/)
        if(Prio)
        {
            if(SetPriorityClass(hProcess,HIGH_PRIORITY_CLASS))
                sLog.outString("TrinityCore process priority class set to HIGH");
            else
                sLog.outError("ERROR: Can't set Trinityd process priority class.");
            sLog.outString();
        }
    }
    #endif

    uint32 realCurrTime, realPrevTime;
    realCurrTime = realPrevTime = WorldTimer::getMSTime();

    uint32 socketSelecttime = sWorld.getConfig(CONFIG_SOCKET_SELECTTIME);

    //server has started up successfully => enable async DB requests
    LoginDatabase.AllowAsyncTransactions();

    // maximum counter for next ping
    uint32 numLoops = (sConfig.GetIntDefault( "MaxPingTime", 30 ) * (MINUTE * 1000000 / socketSelecttime));
    uint32 loopCounter = 0;

    ///- Start up freeze catcher thread
    uint32 freeze_delay = sConfig.GetIntDefault("MaxCoreStuckTime", 0);
    if(freeze_delay)
    {
        FreezeDetectorRunnable *fdr = new FreezeDetectorRunnable();
        fdr->SetDelayTime(freeze_delay*1000);
        ACE_Based::Thread t(fdr);
        t.setPriority(ACE_Based::Highest);
    }

#ifdef ANTICHEAT_SOCK
    AntiCheatRunnable *acr = new AntiCheatRunnable();
    if (acr)
        ACE_Based::Thread t2(acr);
#endif

    ///- Launch the world listener socket
    uint16 wsport = sWorld.getConfig (CONFIG_PORT_WORLD);
    std::string bind_ip = sConfig.GetStringDefault ("BindIP", "0.0.0.0");

    if (sWorldSocketMgr->StartNetwork (wsport, bind_ip.c_str ()) == -1)
    {
        sLog.outError ("Failed to start network");
        World::StopNow(ERROR_EXIT_CODE);
        // go down and shutdown the server
    }

    sWorldSocketMgr->Wait ();

    // set server offline
    LoginDatabase.DirectPExecute("UPDATE realmlist SET color = 2 WHERE id = '%d'",realmID);

    ///- Remove signal handling before leaving
    _UnhookSignals();

    // when the main thread closes the singletons get unloaded
    // since worldrunnable uses them, it will crash if unloaded after master
    t.wait();

    ///- Clean database before leaving
    clearOnlineAccounts();

    sLog.outString( "Halting process..." );

    #ifdef WIN32
    if (sConfig.GetBoolDefault("Console.Enable", true))
    {
        // this only way to terminate CLI thread exist at Win32 (alt. way exist only in Windows Vista API)
        //_exit(1);
        // send keyboard input to safely unblock the CLI thread
        INPUT_RECORD b[5];
        HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
        b[0].EventType = KEY_EVENT;
        b[0].Event.KeyEvent.bKeyDown = TRUE;
        b[0].Event.KeyEvent.uChar.AsciiChar = 'X';
        b[0].Event.KeyEvent.wVirtualKeyCode = 'X';
        b[0].Event.KeyEvent.wRepeatCount = 1;

        b[1].EventType = KEY_EVENT;
        b[1].Event.KeyEvent.bKeyDown = FALSE;
        b[1].Event.KeyEvent.uChar.AsciiChar = 'X';
        b[1].Event.KeyEvent.wVirtualKeyCode = 'X';
        b[1].Event.KeyEvent.wRepeatCount = 1;

        b[2].EventType = KEY_EVENT;
        b[2].Event.KeyEvent.bKeyDown = TRUE;
        b[2].Event.KeyEvent.dwControlKeyState = 0;
        b[2].Event.KeyEvent.uChar.AsciiChar = '\r';
        b[2].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        b[2].Event.KeyEvent.wRepeatCount = 1;
        b[2].Event.KeyEvent.wVirtualScanCode = 0x1c;

        b[3].EventType = KEY_EVENT;
        b[3].Event.KeyEvent.bKeyDown = FALSE;
        b[3].Event.KeyEvent.dwControlKeyState = 0;
        b[3].Event.KeyEvent.uChar.AsciiChar = '\r';
        b[3].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        b[3].Event.KeyEvent.wVirtualScanCode = 0x1c;
        b[3].Event.KeyEvent.wRepeatCount = 1;
        DWORD numb;
        BOOL ret = WriteConsoleInput(hStdIn, b, 4, &numb);
    }
    #endif

    // for some unknown reason, unloading scripts here and not in worldrunnable
    // fixes a memory leak related to detaching threads from the module
    UnloadScriptingModule();

    sInstanceSaveManager.UnbindBeforeDelete();

    ///- Wait for delay threads to end
    CharacterDatabase.HaltDelayThread();
    WorldDatabase.HaltDelayThread();
    LoginDatabase.HaltDelayThread();

    // Exit the process with specified return value
    return World::GetExitCode();
}

/// Initialize connection to the databases
bool Master::_StartDB()
{
    ///- Get world database info from configuration file
    std::string dbstring;
    dbstring = sConfig.GetStringDefault("WorldDatabaseInfo", "");
    if(dbstring.empty())
    {
        sLog.outError("Database not specified in configuration file");
        return false;
    }
    int nConnections = sConfig.GetIntDefault("WorldDatabaseConnections", 1);
    sLog.outString("World Database: %s, total connections: %i", dbstring.c_str(), nConnections + 1);

    ///- Initialise the world database
    if(!WorldDatabase.Initialize(dbstring.c_str(), nConnections))
    {
        sLog.outError("Cannot connect to world database %s",dbstring.c_str());
        return false;
    }

    dbstring = sConfig.GetStringDefault("CharacterDatabaseInfo", "");
    if(dbstring.empty())
    {
        sLog.outError("Character Database not specified in configuration file");
        return false;
    }
    nConnections = sConfig.GetIntDefault("CharacterDatabaseConnections", 1);
    sLog.outDetail("Character Database: %s", dbstring.c_str());

    ///- Initialise the Character database
    if(!CharacterDatabase.Initialize(dbstring.c_str(), nConnections))
    {
        sLog.outString("Character Database: %s, total connections: %i", dbstring.c_str(), nConnections + 1);
        return false;
    }

    ///- Get login database info from configuration file
    dbstring = sConfig.GetStringDefault("LoginDatabaseInfo", "");
    if(dbstring.empty())
    {
        sLog.outError("Login database not specified in configuration file");
        return false;
    }
    nConnections = sConfig.GetIntDefault("LoginDatabaseConnections", 1);
    ///- Initialise the login database
    sLog.outString("Login Database: %s, total connections: %i", dbstring.c_str(), nConnections + 1);
    if(!LoginDatabase.Initialize(dbstring.c_str(), nConnections))
    {
        sLog.outError("Cannot connect to login database %s",dbstring.c_str());
        return false;
    }

    ///- Get the realm Id from the configuration file
    realmID = sConfig.GetIntDefault("RealmID", 0);
    if(!realmID)
    {
        sLog.outError("Realm ID not defined in configuration file");
        return false;
    }
    sLog.outString("Realm running as realm ID %d", realmID);

    ///- Clean the database before starting
    clearOnlineAccounts();

    ///- Insert version info into DB
    WorldDatabase.PExecute("UPDATE `version` SET `core_version` = '%s', `core_revision` = '%s'", _FULLVERSION, _REVISION);

    sWorld.LoadDBVersion();

    sLog.outString("Using %s", sWorld.GetDBVersion());
    return true;
}

/// Clear 'online' status for all accounts with characters in this realm
void Master::clearOnlineAccounts()
{
    // Cleanup online status for characters hosted at current realm
    /// \todo Only accounts with characters logged on *this* realm should have online status reset. Move the online column from 'account' to 'realmcharacters'?
    LoginDatabase.PExecute(
        "UPDATE account SET online = 0 WHERE online > 0 "
        "AND id IN (SELECT acctid FROM realmcharacters WHERE realmid = '%d')",realmID);


    CharacterDatabase.Execute("UPDATE characters SET online = 0 WHERE online<>0");
}

/// Handle termination signals
void Master::_OnSignal(int s)
{
    switch (s)
    {
        case SIGINT:
            World::StopNow(RESTART_EXIT_CODE);
            break;
        case SIGTERM:
            // 3 - LANG_SYSTEMMESSAGE
            sWorld.SendWorldText(3, "Server is going to perform daily database backups. We are back online in approx ~30 min.");
            sWorld.ShutdownServ(600, 0 ,SHUTDOWN_EXIT_CODE);
            break;
        #ifdef _WIN32
        case SIGBREAK:
            World::StopNow(SHUTDOWN_EXIT_CODE);
            break;
        #endif
    }

    signal(s, _OnSignal);
}

/// Define hook '_OnSignal' for all termination signals
void Master::_HookSignals()
{
    signal(SIGINT, _OnSignal);
    signal(SIGTERM, _OnSignal);
    #ifdef _WIN32
    signal(SIGBREAK, _OnSignal);
    #endif
}

/// Unhook the signals before leaving
void Master::_UnhookSignals()
{
    signal(SIGINT, 0);
    signal(SIGTERM, 0);
    #ifdef _WIN32
    signal(SIGBREAK, 0);
    #endif
}


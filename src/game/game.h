#ifndef __GAME_H__
#define __GAME_H__

#include "cube.h"

#define DMF 16.0f
#define DNF 100.0f
#define DVELF 1.0f

enum
{
    NOTUSED = ET_EMPTY,
    LIGHT = ET_LIGHT,
    MAPMODEL = ET_MAPMODEL,
    PLAYERSTART = ET_PLAYERSTART,
    ENVMAP = ET_ENVMAP,
    PARTICLES = ET_PARTICLES,
    MAPSOUND = ET_SOUND,
    SPOTLIGHT = ET_SPOTLIGHT,
    DECAL = ET_DECAL,
    MAXENTTYPES,

    I_FIRST = 0,
    I_LAST = -1
};

struct gameentity : extentity {};

enum
{
    M_EDIT = 1<<0,
    M_LOCAL = 1<<1
};

static struct gamemodeinfo
{
    const char *name, *prettyname;
    int flags;
    const char *info;
} gamemodes[] =
{
    { "edit", "Edit", M_EDIT, "Cooperative map editing." }
};

#define STARTGAMEMODE 0
#define NUMGAMEMODES ((int)(sizeof(gamemodes)/sizeof(gamemodes[0])))
#define m_valid(mode) ((mode) >= STARTGAMEMODE && (mode) < STARTGAMEMODE + NUMGAMEMODES)
#define m_edit (m_valid(game::gamemode) && (gamemodes[game::gamemode - STARTGAMEMODE].flags&M_EDIT))
#define m_mp(mode) (m_valid(mode) && !(gamemodes[(mode) - STARTGAMEMODE].flags&M_LOCAL))

enum { MM_OPEN = 0, MM_PRIVATE, MM_PASSWORD, MM_INVALID = -1 };
static const char * const mastermodes[] = { "open", "private", "password" };

enum
{
    N_CONNECT = 0, N_SERVINFO, N_WELCOME, N_INITCLIENT, N_POS, N_TEXT, N_SOUND, N_CDIS,
    N_MAPCHANGE, N_MAPVOTE, N_PING, N_PONG, N_CLIENTPING, N_SERVMSG,
    N_EDITMODE, N_EDITENT, N_EDITF, N_EDITT, N_EDITM, N_FLIP, N_COPY, N_PASTE, N_ROTATE, N_REPLACE, N_DELCUBE, N_CALCLIGHT, N_REMIP, N_EDITVSLOT, N_UNDO, N_REDO, N_NEWMAP, N_GETMAP, N_SENDMAP, N_CLIPBOARD, N_EDITVAR,
    NUMMSG
};

static const int msgsizes[] =
{
    N_CONNECT, 0, N_SERVINFO, 0, N_WELCOME, 1, N_INITCLIENT, 0, N_POS, 0, N_TEXT, 0, N_SOUND, 2, N_CDIS, 2,
    N_MAPCHANGE, 0, N_MAPVOTE, 0, N_PING, 2, N_PONG, 2, N_CLIENTPING, 2, N_SERVMSG, 0,
    N_EDITMODE, 2, N_EDITENT, 11, N_EDITF, 16, N_EDITT, 16, N_EDITM, 16, N_FLIP, 14, N_COPY, 14, N_PASTE, 14, N_ROTATE, 15, N_REPLACE, 17, N_DELCUBE, 14, N_CALCLIGHT, 1, N_REMIP, 1, N_EDITVSLOT, 16, N_UNDO, 0, N_REDO, 0, N_NEWMAP, 2, N_GETMAP, 1, N_SENDMAP, 0, N_CLIPBOARD, 0, N_EDITVAR, 0,
    -1
};

#define TESSERACT_SERVER_PORT 42000
#define TESSERACT_LANINFO_PORT 41998
#define TESSERACT_MASTER_PORT 41999
#define PROTOCOL_VERSION 3

struct gameent : dynent
{
    int clientnum, privilege, ping;
    editinfo *edit;
    string name;

    gameent() : clientnum(-1), privilege(0), ping(0), edit(NULL)
    {
        type = ENT_PLAYER;
        state = editstate = CS_ALIVE;
        maxspeed = 120.0f;
        name[0] = '\0';
    }

    ~gameent()
    {
        freeeditinfo(edit);
    }
};

namespace entities
{
    extern vector<extentity *> ents;
    extern void preloadentities();
    extern void renderentities();
}

namespace game
{
    extern int gamemode;
    extern string clientmap;
    extern bool connected, remote;
    extern gameent *player1;
    extern vector<gameent *> players, clients;

    extern void changemap(const char *name, int mode);
    extern bool addmsg(int type, const char *fmt = NULL, ...);
    extern void c2sinfo(bool force = false);
}

namespace server
{
    extern int msgsizelookup(int msg);
}

#endif

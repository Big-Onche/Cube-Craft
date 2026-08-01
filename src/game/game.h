#ifndef __GAME_H__
#define __GAME_H__

#include "cube.h"

#define DMF 16.0f
#define DNF 100.0f
#define DVELF 1.0f
#define GAMEUNITSPERMETER 16.0f

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
    M_LOCAL = 1<<1,
    M_CREATIVE = 1<<2,
    M_SURVIVAL = 1<<3
};

static struct gamemodeinfo
{
    const char *name, *prettyname;
    int flags;
    const char *info;
} gamemodes[] =
{
    { "creative", "Creative", M_CREATIVE, "Build freely with fixed-size voxel blocks." },
    { "edit", "Edit", M_EDIT, "Cooperative map editing." },
    { "survival", "Survival", M_SURVIVAL, "Gather resources and break blocks by hand." }
};

#define STARTGAMEMODE 0
#define NUMGAMEMODES ((int)(sizeof(gamemodes)/sizeof(gamemodes[0])))
#define m_valid(mode) ((mode) >= STARTGAMEMODE && (mode) < STARTGAMEMODE + NUMGAMEMODES)
#define m_edit (m_valid(game::gamemode) && (gamemodes[game::gamemode - STARTGAMEMODE].flags&M_EDIT))
#define m_creative (m_valid(game::gamemode) && (gamemodes[game::gamemode - STARTGAMEMODE].flags&M_CREATIVE))
#define m_survival (m_valid(game::gamemode) && (gamemodes[game::gamemode - STARTGAMEMODE].flags&M_SURVIVAL))
#define m_mp(mode) (m_valid(mode) && !(gamemodes[(mode) - STARTGAMEMODE].flags&M_LOCAL))

enum { MM_OPEN = 0, MM_PRIVATE, MM_PASSWORD, MM_INVALID = -1 };
static const char * const mastermodes[] = { "open", "private", "password" };
enum { PRIV_NONE = 0, PRIV_ADMIN = 3 };

enum
{
    N_CONNECT = 0, N_SERVINFO, N_WELCOME, N_INITCLIENT, N_POS, N_TEXT, N_SOUND, N_CDIS,
    N_MAPCHANGE, N_MAPVOTE, N_PING, N_PONG, N_CLIENTPING, N_SERVMSG,
    N_EDITMODE, N_EDITENT, N_EDITF, N_EDITT, N_EDITM, N_FLIP, N_COPY, N_PASTE, N_ROTATE, N_REPLACE, N_DELCUBE, N_CALCLIGHT, N_REMIP, N_EDITVSLOT, N_UNDO, N_REDO, N_NEWMAP, N_GETMAP, N_SENDMAP, N_CLIPBOARD, N_EDITVAR, N_EDITSCATTER,
    N_EDITAUTHOR, N_WORLDSTATE, N_WORLDREADY, N_WORLDSYNC, N_WORLDTIME,
    N_SETPRIVILEGE, N_SETMASTER, N_SERVERCOMMAND,
    N_SERVERIDENTITY, N_IDENTITYLOGIN, N_IDENTITYREGISTER, N_IDENTITYCHALLENGE,
    N_IDENTITYRESPONSE, N_IDENTITYSUCCESS, N_IDENTITYFAILURE, N_IDENTITYREVOKED,
    N_INVENTORYSTATE, N_INVENTORYACTION, N_WORLDACTION, N_WORLDAUTH, N_ACTIONRESULT,
    N_BREAKSTATE,
    NUMMSG
};

enum
{
    INVENTORY_ACTION_SWAP = 0,
    INVENTORY_ACTION_SELECT
};

enum
{
    WORLD_ACTION_PLACE_CUBE = 0,
    WORLD_ACTION_PLACE_SCATTER,
    WORLD_ACTION_BREAK_CUBE_START,
    WORLD_ACTION_BREAK_SCATTER_START,
    WORLD_ACTION_BREAK_UPDATE,
    WORLD_ACTION_BREAK_CANCEL,
    WORLD_ACTION_BREAK_COMPLETE,
    WORLD_ACTION_PLACE_ITEM
};

enum
{
    BREAK_STATE_START = 0,
    BREAK_STATE_UPDATE,
    BREAK_STATE_COMPLETE,
    BREAK_STATE_CANCEL
};

enum
{
    ACTION_RESULT_REJECTED = 0,
    ACTION_RESULT_ACCEPTED,
    ACTION_RESULT_CORRECTED
};

static const int msgsizes[] =
{
    N_CONNECT, 0, N_SERVINFO, 0, N_WELCOME, 1, N_INITCLIENT, 0, N_POS, 0, N_TEXT, 0, N_SOUND, 2, N_CDIS, 2,
    N_MAPCHANGE, 0, N_MAPVOTE, 0, N_PING, 2, N_PONG, 2, N_CLIENTPING, 2, N_SERVMSG, 0,
    N_EDITMODE, 2, N_EDITENT, 11, N_EDITF, 16, N_EDITT, 16, N_EDITM, 16,
    N_FLIP, 14, N_COPY, 14, N_PASTE, 14, N_ROTATE, 15, N_REPLACE, 17,
    N_DELCUBE, 14, N_CALCLIGHT, 1, N_REMIP, 1, N_EDITVSLOT, 16,
    N_UNDO, 0, N_REDO, 0, N_NEWMAP, 2, N_GETMAP, 1, N_SENDMAP, 0,
    N_CLIPBOARD, 0, N_EDITVAR, 0, N_EDITSCATTER, 16, N_EDITAUTHOR, 4,
    N_WORLDSTATE, 9, N_WORLDREADY, 2, N_WORLDSYNC, 2, N_WORLDTIME, 3,
    N_SETPRIVILEGE, 3, N_SETMASTER, 0, N_SERVERCOMMAND, 0,
    N_SERVERIDENTITY, 0, N_IDENTITYLOGIN, 0, N_IDENTITYREGISTER, 0, N_IDENTITYCHALLENGE, 0,
    N_IDENTITYRESPONSE, 0, N_IDENTITYSUCCESS, 0, N_IDENTITYFAILURE, 0, N_IDENTITYREVOKED, 0,
    N_INVENTORYSTATE, 0, N_INVENTORYACTION, 5, N_WORLDACTION, 9, N_WORLDAUTH, 7,
    N_ACTIONRESULT, 0, N_BREAKSTATE, 10,
    -1
};

#define TESSERACT_SERVER_PORT 42000
#define TESSERACT_LANINFO_PORT 41998
#define TESSERACT_MASTER_PORT 41999
#define PROTOCOL_VERSION 11

struct gameent : dynent
{
    int clientnum, privilege, ping, lastupdate, plag;
    editinfo *edit;
    float deltayaw, deltapitch, deltaroll, newyaw, newpitch, newroll;
    float renderbodyyaw, rendercrouch, renderstridephase, renderattackreleasepitch;
    int smoothmillis, renderbodyyawmillis, rendercrouchmillis, renderstridemillis, selectedcreative,
        renderattackmillis, renderattackreleasemillis, renderplacemillis;
    bool renderattacking, renderplacetoggle, renderactioninitialized;
    string name;

    gameent() : clientnum(-1), privilege(0), ping(0), lastupdate(0), plag(0), edit(NULL),
                deltayaw(0), deltapitch(0), deltaroll(0), newyaw(0), newpitch(0), newroll(0),
                renderbodyyaw(0), rendercrouch(0), renderstridephase(0), renderattackreleasepitch(0),
                smoothmillis(-1), renderbodyyawmillis(-1), rendercrouchmillis(-1),
                renderstridemillis(-1), selectedcreative(-1), renderattackmillis(0),
                renderattackreleasemillis(-1000),
                renderplacemillis(-1000), renderattacking(false), renderplacetoggle(false),
                renderactioninitialized(false)
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
    enum { CREATIVE_ARM_CYCLE = 300 };

    struct networkedit
    {
        int type, author, args[6];
        uint revision, requestid;
        selinfo selection;
        vector<uchar> extra;

        networkedit() : type(-1), author(-1), revision(0), requestid(0)
        {
            memset(args, 0, sizeof(args));
        }
    };

    extern int gamemode;
    extern string clientmap;
    extern bool connected, remote;
    extern gameent *player1;
    extern vector<gameent *> players, clients;

    extern void changemap(const char *name, int mode);
    extern bool addmsg(int type, const char *fmt = NULL, ...);
    extern void c2sinfo(bool force = false);
    extern void beginlocalworld();
    extern bool islocalworld();
    extern bool waitforserveredit();
    extern void requestworldcommand(const char *command);
    extern float horizontalmeterspersecond(const physent *d);
    extern float playerarmactionpitch(const gameent *d);
    extern float creativearmwave(int elapsed);
    extern int selectedcreativeblock();
    extern void receiveserversettings(int breakmillis, int scatterbreakmillis);
    extern void receiveinventory(const int *items, const int *counts, int slots, int selected);
    extern void receiveactionresult(uint requestid, int result, const char *reason);
    extern void receivebreakstate(int actor, uint requestid, int phase, int action, const ivec &target, int orient, int stage);
    extern int smoothmove, smoothdist;
    extern vector<networkedit *> pendingnetworkedits;
    extern void processnetworkedits();

#ifndef STANDALONE
    extern void preloadplayermodels();
    extern bool heldtorchemitterposition(gameent *d, vec &position);
    extern void resetclientreceive();
    extern bool pendingnetworkworld, pendingnetworkreset, pendingnetworkfrozen,
                pendingnetworkrestoreposition;
    extern int pendingnetworkseed, pendingnetworktime;
    extern vec pendingnetworkposition;

    namespace environment
    {
        extern void reset();
        extern void update();
        extern void synctime(int millis, bool frozen);
        extern int gettimemillis();
        extern bool istimefrozen();
    }
#endif
}

namespace server
{
    extern int msgsizelookup(int msg);
}

#endif

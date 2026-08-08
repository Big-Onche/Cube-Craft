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

struct gamemodeinfo
{
    const char *name, *prettyname;
    int flags;
    const char *info;
};
extern const gamemodeinfo gamemodes[3];

#define STARTGAMEMODE 0
#define NUMGAMEMODES 3
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
    N_INVENTORYSTATE, N_INVENTORYACTION, N_CRAFTSTATE, N_CRAFTACTION, N_WORLDACTION, N_WORLDAUTH, N_ACTIONRESULT,
    N_BREAKSTATE, N_DROPSETTINGS, N_DROPSPAWN, N_DROPDELETE, N_DROPPICKUP,
    NUMMSG
};

enum
{
    INVENTORY_ACTION_SWAP = 0,
    INVENTORY_ACTION_SELECT,
    INVENTORY_ACTION_CLICK
};

enum
{
    INVENTORY_CLICK_LEFT = 0,
    INVENTORY_CLICK_RIGHT
};

enum
{
    CRAFT_ACTION_OPEN_PLAYER = 0,
    CRAFT_ACTION_OPEN_TABLE,
    CRAFT_ACTION_INVENTORY_TO_GRID,
    CRAFT_ACTION_GRID_TO_INVENTORY,
    CRAFT_ACTION_GRID_SWAP,
    CRAFT_ACTION_TAKE_OUTPUT,
    CRAFT_ACTION_CLICK_GRID,
    CRAFT_ACTION_TAKE_OUTPUT_CURSOR
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
    N_WORLDSTATE, 12, N_WORLDREADY, 2, N_WORLDSYNC, 2, N_WORLDTIME, 3,
    N_SETPRIVILEGE, 3, N_SETMASTER, 0, N_SERVERCOMMAND, 0,
    N_SERVERIDENTITY, 0, N_IDENTITYLOGIN, 0, N_IDENTITYREGISTER, 0, N_IDENTITYCHALLENGE, 0,
    N_IDENTITYRESPONSE, 0, N_IDENTITYSUCCESS, 0, N_IDENTITYFAILURE, 0, N_IDENTITYREVOKED, 0,
    N_INVENTORYSTATE, 0, N_INVENTORYACTION, 5, N_CRAFTSTATE, 0, N_CRAFTACTION, 7, N_WORLDACTION, 9, N_WORLDAUTH, 7,
    N_ACTIONRESULT, 0, N_BREAKSTATE, 10, N_DROPSETTINGS, 6, N_DROPSPAWN, 10, N_DROPDELETE, 3, N_DROPPICKUP, 6,
    -1
};

#define TESSERACT_SERVER_PORT 42000
#define TESSERACT_LANINFO_PORT 41998
#define TESSERACT_MASTER_PORT 41999
#define PROTOCOL_VERSION 16

static inline bool inventoryslotclick(int &cursoritem, int &cursorcount, int &slotitem, int &slotcount, int button)
{
    if(button != INVENTORY_CLICK_LEFT && button != INVENTORY_CLICK_RIGHT) return false;
    if(cursorcount <= 0) { cursoritem = -1; cursorcount = 0; }
    if(slotcount <= 0) { slotitem = -1; slotcount = 0; }
    if(button == INVENTORY_CLICK_LEFT)
    {
        if(cursorcount <= 0)
        {
            if(slotcount <= 0) return false;
            swap(cursoritem, slotitem);
            swap(cursorcount, slotcount);
            return true;
        }
        if(slotcount <= 0)
        {
            swap(cursoritem, slotitem);
            swap(cursorcount, slotcount);
            return true;
        }
        if(cursoritem != slotitem)
        {
            swap(cursoritem, slotitem);
            swap(cursorcount, slotcount);
            return true;
        }
        const int moved = min(cursorcount, max(getinventoryitemmaxstack(slotitem), 1) - slotcount);
        if(moved <= 0) return false;
        slotcount += moved;
        cursorcount -= moved;
    }
    else if(cursorcount <= 0)
    {
        if(slotcount <= 0) return false;
        const int moved = (slotcount + 1) / 2;
        cursoritem = slotitem;
        cursorcount = moved;
        slotcount -= moved;
    }
    else
    {
        if(slotcount > 0 && (slotitem != cursoritem || slotcount >= max(getinventoryitemmaxstack(slotitem), 1))) return false;
        if(slotcount <= 0) slotitem = cursoritem;
        ++slotcount;
        --cursorcount;
    }
    if(cursorcount <= 0) { cursoritem = -1; cursorcount = 0; }
    if(slotcount <= 0) { slotitem = -1; slotcount = 0; }
    return true;
}

static inline uint worlddrophash(uint value)
{
    value ^= value >> 16;
    value *= 0x7FEB352DU;
    value ^= value >> 15;
    value *= 0x846CA68BU;
    return value ^ (value >> 16);
}

static inline bool worlddroproll(int source, uint requestid, int objectitem, int dropindex, int mincount, int maxcount, float chance, int &quantity)
{
    const uint seed = worlddrophash(requestid ^ uint(source + 1) * 0x9E3779B9U ^ uint(objectitem + 1) * 0x85EBCA6BU ^ uint(dropindex + 1));
    if(float(seed & 0xFFFFFFU) / 16777216.0f >= clamp(chance, 0.0f, 1.0f)) return false;
    quantity = mincount >= maxcount ? mincount : mincount + int(worlddrophash(seed ^ 0xC2B2AE35U) % uint(maxcount - mincount + 1));
    return quantity > 0;
}

struct worlddrop
{
    uint id, sourcerequestid, pickuprequestid;
    int source, item, count, owner, created, pickupmillis, picker;
    bool confirmed, picking, removed, pickupblocked;
    vec o, pickupfrom;

    worlddrop() : id(0), sourcerequestid(0), pickuprequestid(0), source(-1), item(-1), count(0), owner(-1), created(0), pickupmillis(0),
                  picker(-1), confirmed(false), picking(false), removed(false), pickupblocked(false), o(0, 0, 0), pickupfrom(0, 0, 0)
    {
    }
};

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
    extern void receiveserversettings(int breakmillis, int scatterbreakmillis, int waterupdates, int waterdistance, int waterspeed);
    extern void receivedropsettings(int personal, int timeout, int maximum, int maxdistance, int requireconfirmation);
    extern void receivedropspawn(uint id, int source, uint sourcerequestid, int item, int count, int owner, const vec &o);
    extern void receivedropdelete(uint id, int picker);
    extern void resetworlddrops();
    extern const vector<worlddrop *> &getworlddrops();
    extern int getdynamicentsmaxdistance();
    extern void receiveinventory(const int *items, const int *counts, int slots, int selected, int cursoritem, int cursorcount);
    extern void receivecraftstate(const int *items, const int *counts, int slots, int gridsize, int stationitem,
                                  int recipe, int outputitem, int outputcount);
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

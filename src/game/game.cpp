#include "game.h"

#ifndef STANDALONE
extern int mainmenu;
extern int initing;
#endif

namespace game
{
    int gamemode = STARTGAMEMODE;
    string clientmap = "";
    bool connected = false, remote = false, gamepaused = false;
    static bool localworldactive = false;
    int sessionid = 0, mastermode = MM_OPEN;
    gameent *player1 = NULL;
    vector<gameent *> players, clients;
    vector<uchar> messages;

    float horizontalmeterspersecond(const physent *d)
    {
        if(!d) return 0.0f;
        float movescale = d->inwater && d->state != CS_EDITING && d->state != CS_SPECTATOR ? 0.5f : 1.0f;
        float x = d->vel.x * movescale + d->falling.x,
              y = d->vel.y * movescale + d->falling.y;
        return sqrtf(x*x + y*y) / GAMEUNITSPERMETER;
    }

    static string connectpass = "", servdesc = "";
    static int authoritativeauthor = -1;
    static uint authoritativerevision = 0, synchronizedrevision = 0;
#ifndef STANDALONE
    static bool pendingnetworkworld = false, pendingnetworkreset = false,
                pendingnetworkfrozen = false, pendingnetworkrestoreposition = false;
    static int pendingnetworkseed = 0, pendingnetworktime = 0;
    static vec pendingnetworkposition;
    static void updatesurvivalbreaking();
#endif

    struct networkedit
    {
        int type, author, args[3];
        uint revision;
        selinfo selection;
        vector<uchar> extra;

        networkedit() : type(-1), author(-1), revision(0)
        {
            memset(args, 0, sizeof(args));
        }
    };

    static vector<networkedit *> pendingnetworkedits;

    static void putsel(packetbuf &p, const selinfo &sel)
    {
        putint(p, sel.o.x); putint(p, sel.o.y); putint(p, sel.o.z);
        putint(p, sel.s.x); putint(p, sel.s.y); putint(p, sel.s.z);
        putint(p, sel.grid); putint(p, sel.orient);
        putint(p, sel.cx); putint(p, sel.cxs); putint(p, sel.cy); putint(p, sel.cys);
        putint(p, sel.corner);
    }

    static void getsel(packetbuf &p, selinfo &sel)
    {
        sel.o.x = getint(p); sel.o.y = getint(p); sel.o.z = getint(p);
        sel.s.x = getint(p); sel.s.y = getint(p); sel.s.z = getint(p);
        sel.grid = getint(p); sel.orient = getint(p);
        sel.cx = getint(p); sel.cxs = getint(p); sel.cy = getint(p); sel.cys = getint(p);
        sel.corner = getint(p);
    }

    #ifndef STANDALONE
    static void putvslot(packetbuf &p, int index)
    {
        vector<uchar> buf;
        packvslot(buf, index);
        if(buf.length()) p.put(buf.getbuf(), buf.length());
    }

    static void putvslot(packetbuf &p, const VSlot *vs)
    {
        vector<uchar> buf;
        packvslot(buf, vs);
        if(buf.length()) p.put(buf.getbuf(), buf.length());
    }
    #endif

    bool addmsg(int type, const char *fmt, ...)
    {
#ifdef STANDALONE
        return false;
#else
        if(!fmt) fmt = "";
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, type);
        va_list args;
        va_start(args, fmt);
        while(*fmt) switch(*fmt++)
        {
            case 'r': break;
            case 'c':
            {
                (void)va_arg(args, gameent *);
                break;
            }
            case 'i':
            {
                int n = isdigit(*fmt) ? *fmt++ - '0' : 1;
                loopi(n) putint(p, va_arg(args, int));
                break;
            }
            case 'f':
            {
                int n = isdigit(*fmt) ? *fmt++ - '0' : 1;
                loopi(n) putfloat(p, (float)va_arg(args, double));
                break;
            }
            case 's':
                sendstring(va_arg(args, const char *), p);
                break;
        }
        va_end(args);
        sendclientpacket(p.finalize(), 1);
        return true;
#endif
    }

    bool waitforserveredit()
    {
#ifdef STANDALONE
        return false;
#else
        return !localworldactive && (remote || (connected && isconnected(false, true)));
#endif
    }

    bool islocalworld()
    {
        return localworldactive;
    }

    void requestworldcommand(const char *command)
    {
        if(!waitforserveredit())
        {
            conoutf(CON_ERROR, "server command is only available in multiplayer");
            return;
        }
        addmsg(N_SERVERCOMMAND, "rs", command ? command : "");
    }

    void parseoptions(vector<const char *> &args)
    {
        loopv(args) conoutf(CON_ERROR, "unknown command-line option: %s", args[i]);
    }

    const char *gameident() { return "CubeCraft"; }

#ifndef STANDALONE
    const char *gameconfig() { return "config/game.cfg"; }
    const char *savedconfig() { return "config/saved.cfg"; }
    const char *restoreconfig() { return "config/restore.cfg"; }
    const char *defaultconfig() { return "config/default.cfg"; }
    const char *autoexec() { return "config/autoexec.cfg"; }
    const char *savedservers() { return "config/servers.cfg"; }
    void loadconfigs()
    {
        execute("if (|| (=s (getbind F2) []) (=s (getbind F2) [togglevar debughud])) [bind F2 [toggleui debughud]]");
        loopi(7)
        {
            defformatstring(command,
                "if (|| (=s (getbind %d) []) (=s (getbind %d) [creativeselect %d])) [bind %d [creativehotbarselect %d]]",
                i + 1, i + 1, i, i + 1, i);
            execute(command);
        }
        execute("if (=s (getbind 8) []) [bind 8 [creativehotbarselect 7]]");
        execute("if (|| (=s (getbind 9) []) (=s (getbind 9) [if (allowthirdperson) [togglevar thirdperson]])) "
                "[bind 9 [creativehotbarselect 8]]");
        execute("if (=s (getbind F5) []) [bindvar F5 [thirdperson] [allowthirdperson]]");
    }

    void initclient()
    {
        player1 = new gameent;
        copystring(player1->name, "camera");
        players.add(player1);
    }

    void resetgamestate() {}
    static void removeclient(int cn)
    {
        if(!clients.inrange(cn) || !clients[cn]) return;
        gameent *d = clients[cn];
        clients[cn] = NULL;
        players.removeobj(d);
        delete d;
        cleardynentcache();
    }

    static void clearclients()
    {
        loopv(clients) if(clients[i]) delete clients[i];
        clients.setsize(0);
        players.setsize(0);
        if(player1) players.add(player1);
        cleardynentcache();
    }

    static gameent *newclient(int cn)
    {
        if(cn < 0 || cn > max(0xFF, MAXCLIENTS))
        {
            neterr("clientnum", false);
            return NULL;
        }
        if(player1 && cn == player1->clientnum) return player1;
        while(clients.length() <= cn) clients.add(NULL);
        gameent *&d = clients[cn];
        if(!d)
        {
            d = new gameent;
            d->clientnum = cn;
            copystring(d->name, "player");
            players.add(d);
            cleardynentcache();
        }
        return d;
    }

    static int lastpositionsend = -1000;
    static string sentname = "";

    void gamedisconnect(bool cleanup)
    {
        connected = remote = false;
        localworldactive = false;
        pendingnetworkworld = pendingnetworkreset = pendingnetworkrestoreposition = false;
        pendingnetworkedits.deletecontents();
        authoritativeauthor = -1;
        authoritativerevision = synchronizedrevision = 0;
        clearclients();
        if(player1)
        {
            player1->clientnum = -1;
            player1->privilege = PRIV_NONE;
        }
#ifndef STANDALONE
        if(editmode) toggleedit(true);
#endif
        lastpositionsend = -1000;
        sentname[0] = '\0';
    }
    void connectattempt(const char *name, const char *password, const ENetAddress &address) { copystring(connectpass, password ? password : ""); }
    void connectfail() {}

    void gameconnect(bool _remote)
    {
        // Explicitly connecting starts an authoritative multiplayer session.
        // Saved procedural worlds remain offline until this point.
        localworldactive = false;
        remote = _remote;
        if(remote) addmsg(N_CONNECT, "rs", connectpass);
        else connected = true;
    }

    void beginlocalworld()
    {
#ifndef STANDALONE
        // A listen server owns a separate seed and journal. Leaving it connected
        // would make its N_WORLDSTATE replace the saved world after loading.
        if(isconnected(false, false)) disconnect(false, false);
        if(isconnected(false, true)) server::localdisconnect(false);
#endif
        connected = remote = false;
        localworldactive = true;
#ifndef STANDALONE
        pendingnetworkworld = pendingnetworkreset = pendingnetworkrestoreposition = false;
        pendingnetworkedits.deletecontents();
        authoritativeauthor = -1;
        authoritativerevision = synchronizedrevision = 0;
#endif
        if(player1)
        {
            player1->clientnum = -1;
            player1->privilege = PRIV_ADMIN;
        }
    }

    bool allowedittoggle()
    {
        // Always permit leaving edit mode, including after privilege is lost.
        if(editmode) return true;
        if(player1 && player1->privilege >= PRIV_ADMIN) return true;
        conoutf(CON_ERROR, "full edit mode requires admin privilege");
        return false;
    }

    void edittoggled(bool on)
    {
        addmsg(N_EDITMODE, "ri", on ? 1 : 0);
    }

    void writeclientinfo(stream *f)
    {
        if(player1) f->printf("name %s\n", escapestring(player1->name));
    }

    void toserver(char *text)
    {
        conoutf("%s", text);
        addmsg(N_TEXT, "rs", text);
    }

    void changemap(const char *name, int mode)
    {
        gamemode = m_valid(mode) ? mode : STARTGAMEMODE;
#ifndef STANDALONE
        if(!localworldactive && !remote && !isconnected()) localconnect();
#endif
        if(editmode) toggleedit();
        if(name && name[0]) load_world(name);
        else emptymap(0, true, NULL);
    }

    void changemap(const char *name) { changemap(name, STARTGAMEMODE); }
    bool validgamemode(int mode) { return m_valid(mode); }
    void forceedit(const char *name) { if(name && name[0]) copystring(clientmap, name); }
    bool ispaused() { return gamepaused; }
    int scaletime(int t) { return t*100; }
    bool allowmouselook() { return true; }

    VARP(smoothmove, 0, 75, 100);
    VARP(smoothdist, 0, 32, 64);

    static void predictplayer(gameent *d)
    {
        d->o = d->newpos;
        d->yaw = d->newyaw;
        d->pitch = d->newpitch;
        d->roll = d->newroll;
        moveplayer(d, 1, false);
        d->newpos = d->o;

        float k = 1.0f - float(lastmillis - d->smoothmillis)/smoothmove;
        if(k <= 0) return;
        d->o.add(vec(d->deltapos).mul(k));
        d->yaw += d->deltayaw*k;
        if(d->yaw < 0) d->yaw += 360;
        else if(d->yaw >= 360) d->yaw -= 360;
        d->pitch += d->deltapitch*k;
        d->roll += d->deltaroll*k;
    }

    static void otherplayers()
    {
        loopv(players)
        {
            gameent *d = players[i];
            if(d == player1) continue;

            int lagtime = (totalmillis ? totalmillis : 1) - d->lastupdate;
            if(!lagtime) continue;
            if(lagtime > 1000 && d->state == CS_ALIVE)
            {
                d->state = CS_LAGGED;
                continue;
            }
            if(d->state == CS_ALIVE || d->state == CS_EDITING)
            {
                if(smoothmove && d->smoothmillis > 0) predictplayer(d);
                else moveplayer(d, 1, false);
            }
        }
    }

    static bool applynetworkedit(networkedit &edit)
    {
        selinfo sel = edit.selection;
        worldselectiontolocal(sel);
        if(!sel.validate() || !worldselectionready(sel)) return false;

        setworldeditauthor(edit.author);
        setworldeditrevision(edit.revision);
        switch(edit.type)
        {
            case N_EDITF: mpeditface(edit.args[0], edit.args[1], sel, false); break;
            case N_EDITT:
            {
                ucharbuf extra(edit.extra.getbuf(), edit.extra.length());
                mpedittex(edit.args[0], edit.args[1], sel, extra);
                break;
            }
            case N_EDITM: mpeditmat(edit.args[0], edit.args[1], sel, false); break;
            case N_FLIP: mpflip(sel, false); break;
            case N_ROTATE: mprotate(edit.args[0], sel, false); break;
            case N_REPLACE:
            {
                ucharbuf extra(edit.extra.getbuf(), edit.extra.length());
                mpreplacetex(edit.args[0], edit.args[1], edit.args[2] > 0, sel, extra);
                break;
            }
            case N_DELCUBE: mpdelcube(sel, false); break;
            case N_EDITSCATTER:
                return editworldscatter(edit.args[0], sel.o, sel.orient,
                                        edit.args[1] != 0);
            case N_EDITVSLOT:
            {
                ucharbuf extra(edit.extra.getbuf(), edit.extra.length());
                mpeditvslot(edit.args[0], edit.args[1], sel, extra);
                break;
            }
            default: return true;
        }
        return true;
    }

    static void processnetworkedits()
    {
        if(pendingnetworkworld) return;
        for(int i = 0; i < pendingnetworkedits.length();)
        {
            networkedit *edit = pendingnetworkedits[i];
            if(!applynetworkedit(*edit))
            {
                ++i;
                continue;
            }
            delete edit;
            pendingnetworkedits.remove(i);
        }
    }

    void updateworld()
    {
#ifndef STANDALONE
        if(pendingnetworkworld)
        {
            const int seed = pendingnetworkseed, timemillis = pendingnetworktime;
            const bool frozen = pendingnetworkfrozen,
                       restoreposition = pendingnetworkrestoreposition;
            const vec savedposition = pendingnetworkposition;
            pendingnetworkreset = pendingnetworkrestoreposition = false;

            // Keep the server lighting active for the entire network-world load.
            // startmap() sees pendingnetworkworld and preserves this authoritative
            // time instead of briefly installing the local default lighting.
            environment::synctime(timemillis, frozen);
            startnetworkworld(seed);
            if(restoreposition && player1)
            {
                vec restored = savedposition;
                worldpositiontolocal(restored);
                player1->o = restored;
                player1->resetinterp();
                updateworldchunks(true);
            }
            environment::synctime(timemillis, frozen);
            pendingnetworkworld = false;
            addmsg(N_WORLDREADY, "ri", 0);
        }
        environment::update();
#endif
        updateworldchunks();
        processnetworkedits();
        physicsframe();
        otherplayers();
        if(player1)
        {
            crouchplayer(player1, 10, true);
            moveplayer(player1, 10, true);
            updateworldchunks();
        }
#ifndef STANDALONE
        updatesurvivalbreaking();
#endif
        gets2c();
        c2sinfo();
    }

    void physicstrigger(physent *d, bool local, int floorlevel, int waterlevel, int material) {}
    void bounced(physent *d, const vec &surface) {}

    void edittrigger(const selinfo &sel, int op, int arg1, int arg2, int arg3, const VSlot *vs)
    {
        if(remote && op == EDIT_COPY) return;
        if(!connected && !remote && !isconnected(false, true)) return;
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_EDITF + op);
        switch(op)
        {
            case EDIT_CALCLIGHT:
            case EDIT_REMIP:
                break;

            case EDIT_UNDO:
            case EDIT_REDO:
            {
                uchar *outbuf = NULL;
                int inlen = 0, outlen = 0;
                if(packundo(op, inlen, outbuf, outlen))
                {
                    putint(p, inlen);
                    putint(p, outlen);
                    if(outlen > 0) p.put(outbuf, outlen);
                    delete[] outbuf;
                }
                break;
            }

            default:
            {
                selinfo networksel = sel;
                if(waitforserveredit()) worldselectiontoabsolute(networksel);
                putsel(p, networksel);
                switch(op)
                {
                    case EDIT_FACE: case EDIT_MAT:
                        putint(p, arg1); putint(p, arg2);
                        break;
                    case EDIT_ROTATE:
                        putint(p, arg1);
                        break;
                    case EDIT_TEX:
                    {
                        int tex1 = shouldpacktex(arg1);
                        putint(p, tex1 ? tex1 : arg1); putint(p, arg2);
                        p.pad(2);
                        int offset = p.length();
                        if(tex1) putvslot(p, arg1);
                        *(ushort *)&p.buf[offset-2] = lilswap(ushort(p.length() - offset));
                        break;
                    }
                    case EDIT_REPLACE:
                    {
                        int tex1 = shouldpacktex(arg1), tex2 = shouldpacktex(arg2);
                        putint(p, tex1 ? tex1 : arg1); putint(p, tex2 ? tex2 : arg2); putint(p, arg3);
                        p.pad(2);
                        int offset = p.length();
                        if(tex1) putvslot(p, arg1);
                        if(tex2) putvslot(p, arg2);
                        *(ushort *)&p.buf[offset-2] = lilswap(ushort(p.length() - offset));
                        break;
                    }
                    case EDIT_VSLOT:
                        putint(p, arg1); putint(p, arg2);
                        p.pad(2);
                        {
                            int offset = p.length();
                            putvslot(p, vs);
                            *(ushort *)&p.buf[offset-2] = lilswap(ushort(p.length() - offset));
                        }
                        break;
                }
                break;
            }
        }
        sendclientpacket(p.finalize(), 1);
    }

    static void scatteredittrigger(int type, const ivec &support,
                                   int orient, bool place)
    {
        if(!waitforserveredit())
        {
            editworldscatter(type, support, orient, place);
            return;
        }
        selinfo sel;
        sel.o = support;
        sel.s = ivec(1, 1, 1);
        sel.grid = 16;
        sel.orient = orient;
        sel.cx = sel.cy = sel.corner = 0;
        sel.cxs = sel.cys = 2;
        worldselectiontoabsolute(sel);
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_EDITSCATTER);
        putsel(p, sel);
        putint(p, type);
        putint(p, place ? 1 : 0);
        sendclientpacket(p.finalize(), 1);
    }

    void vartrigger(ident *id)
    {
        if(!id || (!connected && !remote && !isconnected(false, true))) return;
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_EDITVAR);
        putint(p, id->type);
        sendstring(id->name, p);
        switch(id->type)
        {
            case ID_VAR: putint(p, *id->storage.i); break;
            case ID_FVAR: putfloat(p, *id->storage.f); break;
            case ID_SVAR: sendstring(*id->storage.s, p); break;
        }
        sendclientpacket(p.finalize(), 1);
    }

    void dynentcollide(physent *d, physent *o, const vec &dir) {}
    const char *getclientmap() { return clientmap; }
    int findclientnum(const char *name)
    {
        if(!name || !name[0]) return -1;
        char *end = NULL;
        long numeric = strtol(name, &end, 10);
        if(end != name && !*end) return int(numeric);
        loopv(players) if(players[i] && !cubecasecmp(players[i]->name, name))
            return players[i]->clientnum;
        loopv(clients) if(clients[i] && !cubecasecmp(clients[i]->name, name))
            return clients[i]->clientnum;
        return -1;
    }
    const char *getmapinfo() { return NULL; }
    const char *getscreenshotinfo() { return clientmap; }
    void suicide(physent *d) {}
    void newmap(int size)
    {
#ifndef STANDALONE
        if(!initing)
        {
            connected = true;
            mainmenu = 0;
            if(!editmode) toggleedit(true);
        }
#endif
        if(isconnected(false, true)) addmsg(N_NEWMAP, "ri", size);
    }

    void startmap(const char *name)
    {
        copystring(clientmap, name ? name : "");
#ifndef STANDALONE
        if(pendingnetworkworld) environment::synctime(pendingnetworktime, pendingnetworkfrozen);
        else environment::reset();
        if(!initing)
        {
            if(!localworldactive && !remote && !isconnected()) localconnect();
            mainmenu = 0;
        }
#endif
        findplayerspawn(player1, -1, 0);
        if(player1)
        {
            player1->renderbodyyawmillis = -1;
            player1->rendercrouchmillis = -1;
            player1->renderstridemillis = -1;
            player1->renderattacking = false;
            player1->renderattackreleasemillis = -1000;
            player1->renderplacemillis = -1000;
            player1->renderactioninitialized = true;
        }
    }
    void preload()
    {
        entities::preloadentities();
#ifndef STANDALONE
        preloadplayermodels();
#endif
    }
    float abovegameplayhud(int w, int h) { return 1.0f; }

    enum
    {
        CREATIVE_GRID = 16,
        CREATIVE_REACH = CREATIVE_GRID * 8
    };

    enum
    {
        CREATIVE_HOTBAR_SLOTS = 9
    };

    static int creativehotbar[CREATIVE_HOTBAR_SLOTS] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    static int creativehotbarslot = 0;
    static int survivalitems[SURVIVAL_USABLE_SLOTS] =
    {
        -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1
    };
    static int survivalcounts[SURVIVAL_USABLE_SLOTS] = { 0 };

    enum
    {
        CREATIVE_ARM_CYCLE = 300,
        CREATIVE_ARM_RELEASE = 120,
        SURVIVAL_BREAK_MILLIS = 5000,
        SURVIVAL_SCATTER_BREAK_MILLIS = 250,
        SURVIVAL_BREAK_STAGES = 8,
        SURVIVAL_STACK_SIZE = 64
    };

    static const float CREATIVE_ARM_PITCH = 70.0f;

    static int clampcreativehotbarslot()
    {
        creativehotbarslot = clamp(creativehotbarslot, 0, CREATIVE_HOTBAR_SLOTS - 1);
        return creativehotbarslot;
    }

    int selectedcreativeblock()
    {
        const int slot = clampcreativehotbarslot(),
                  item = m_survival ? survivalitems[slot] : creativehotbar[slot],
                  count = numworldcubes() + numworldscatters();
        if(m_survival && survivalcounts[slot] <= 0) return -1;
        return item >= 0 && item < count ? item : -1;
    }

    void resetsurvivalinventory()
    {
        loopi(SURVIVAL_USABLE_SLOTS)
        {
            survivalitems[i] = -1;
            survivalcounts[i] = 0;
        }
        creativehotbarslot = 0;
    }

    void loadsurvivalinventory(const int *items, const int *counts, int slots)
    {
        resetsurvivalinventory();
        loopi(min(slots, int(SURVIVAL_USABLE_SLOTS)))
        {
            if(items[i] < 0 || counts[i] <= 0) continue;
            survivalitems[i] = items[i];
            survivalcounts[i] = clamp(counts[i], 1, int(SURVIVAL_STACK_SIZE));
        }
    }

    void savesurvivalinventory(stream *f)
    {
        if(!f) return;
        f->printf("game_mode %d\n", gamemode);
        loopi(SURVIVAL_USABLE_SLOTS) if(survivalitems[i] >= 0 && survivalcounts[i] > 0)
            f->printf("inventory %d %d %d\n", i, survivalitems[i], survivalcounts[i]);
    }

    static bool addsurvivalitem(int item)
    {
        if(item < 0 || item >= numworldcubes() + numworldscatters()) return false;
        loopi(SURVIVAL_USABLE_SLOTS)
        {
            if(survivalitems[i] != item || survivalcounts[i] >= SURVIVAL_STACK_SIZE) continue;
            ++survivalcounts[i];
            return true;
        }
        loopi(SURVIVAL_USABLE_SLOTS) if(survivalitems[i] < 0 || survivalcounts[i] <= 0)
        {
            survivalitems[i] = item;
            survivalcounts[i] = 1;
            return true;
        }
        return false;
    }

    static void consumesurvivalitem()
    {
        const int slot = clampcreativehotbarslot();
        if(survivalcounts[slot] <= 0) return;
        if(--survivalcounts[slot] <= 0)
        {
            survivalitems[slot] = -1;
            survivalcounts[slot] = 0;
        }
    }

    static bool creativeenabled()
    {
        return m_creative && !editmode && player1 && player1->state == CS_ALIVE;
    }

    static bool survivalenabled()
    {
        return m_survival && !editmode && player1 && player1->state == CS_ALIVE;
    }

    static bool buildenabled()
    {
        return (m_creative || m_survival) && !editmode && player1 && player1->state == CS_ALIVE;
    }

    static float creativearmwave(int elapsed)
    {
        float progress = clamp(elapsed / float(CREATIVE_ARM_CYCLE), 0.0f, 1.0f);
        return (0.5f - 0.5f * cosf(progress * 2.0f * PI)) * CREATIVE_ARM_PITCH;
    }

    float playerarmactionpitch(const gameent *d)
    {
        if(!d || (d == player1 && !buildenabled())) return -1.0f;

        if(d->renderattacking)
        {
            int elapsed = max(lastmillis - d->renderattackmillis, 0) % CREATIVE_ARM_CYCLE;
            return creativearmwave(elapsed);
        }

        int elapsed = lastmillis - d->renderattackreleasemillis;
        if(elapsed >= 0 && elapsed < CREATIVE_ARM_RELEASE)
            return d->renderattackreleasepitch * (1.0f - elapsed / float(CREATIVE_ARM_RELEASE));

        elapsed = lastmillis - d->renderplacemillis;
        return elapsed >= 0 && elapsed < CREATIVE_ARM_CYCLE ? creativearmwave(elapsed) : -1.0f;
    }

    static bool creativehit(selinfo &hit)
    {
        if(!buildenabled()) return false;

        const vec origin = camera1 ? camera1->o : player1->o;
        vec hitpos;
        float dist = raycubepos(origin, camdir, hitpos, CREATIVE_REACH,
                                RAY_CLIPMAT | RAY_SKIPFIRST, CREATIVE_GRID);
        if(dist >= CREATIVE_REACH) return false;

        // Step just through the hit surface so flooring selects the occupied cell.
        vec inside = vec(camdir).mul(dist + 0.05f).add(origin);
        if(!insideworld(inside)) return false;

        hit.o = ivec(inside).mask(~(CREATIVE_GRID - 1));
        hit.s = ivec(1, 1, 1);
        hit.grid = CREATIVE_GRID;
        hit.cx = hit.cy = hit.corner = 0;
        hit.cxs = hit.cys = 2;

        float boxdist = 0;
        if(!rayboxintersect(vec(hit.o), vec(CREATIVE_GRID), origin, camdir, boxdist, hit.orient))
            return false;
        return hit.validate();
    }

    enum
    {
        CREATIVE_TARGET_NONE = 0,
        CREATIVE_TARGET_CUBE,
        CREATIVE_TARGET_SCATTER
    };

    struct creativetarget
    {
        int type, entity;
        selinfo cube;
        vec center, radius;

        creativetarget() : type(CREATIVE_TARGET_NONE), entity(-1), center(0, 0, 0), radius(0, 0, 0) {}
    };

    static bool findcreativetarget(creativetarget &target)
    {
        if(!buildenabled()) return false;

        const vec origin = camera1 ? camera1->o : player1->o;
        int orient = -1, entity = -1;
        rayent(origin, camdir, CREATIVE_REACH, RAY_CLIPMAT | RAY_ENTS | RAY_SKIPFIRST,
               CREATIVE_GRID, orient, entity);
        if(entity >= 0 && isworldscatterentity(entity) &&
           getworldscatterentitybox(entity, target.center, target.radius))
        {
            target.type = CREATIVE_TARGET_SCATTER;
            target.entity = entity;
            return true;
        }

        if(!creativehit(target.cube)) return false;
        target.type = CREATIVE_TARGET_CUBE;
        target.center = vec(target.cube.o).add(CREATIVE_GRID * 0.5f);
        target.radius = vec(CREATIVE_GRID * 0.5f);
        return true;
    }

    static ivec creativeplacecell(const selinfo &hit)
    {
        ivec target = hit.o;
        int d = hit.orient >> 1;
        target[d] += (hit.orient & 1) ? CREATIVE_GRID : -CREATIVE_GRID;
        return target;
    }

    static bool creativeplayeroverlap(const ivec &cell)
    {
        if(!player1) return false;
        const float bx1 = cell.x, by1 = cell.y, bz1 = cell.z,
                    bx2 = cell.x + CREATIVE_GRID, by2 = cell.y + CREATIVE_GRID,
                    bz2 = cell.z + CREATIVE_GRID,
                    px1 = player1->o.x - player1->xradius,
                    py1 = player1->o.y - player1->yradius,
                    pz1 = player1->o.z - player1->eyeheight,
                    px2 = player1->o.x + player1->xradius,
                    py2 = player1->o.y + player1->yradius,
                    pz2 = player1->o.z + player1->aboveeye;
        return bx1 < px2 && bx2 > px1 && by1 < py2 && by2 > py1 && bz1 < pz2 && bz2 > pz1;
    }

    static void creativeplace()
    {
        selinfo hit;
        if(!creativehit(hit)) return;

        const int selected = selectedcreativeblock(),
                  cubecount = numworldcubes();
        if(selected < 0) return;
        if(selected >= cubecount)
        {
            const int type = selected - cubecount;
            if(isworldtorch(type))
            {
                if(hit.orient == WORLD_ORIENT_BOTTOM) return;
            }
            else if(hit.orient != WORLD_ORIENT_TOP) return;
            scatteredittrigger(type, hit.o, hit.orient, true);
            if(m_survival) consumesurvivalitem();
            player1->renderplacemillis = lastmillis;
            player1->renderplacetoggle = !player1->renderplacetoggle;
            return;
        }
        if(cubecount <= 0) return;

        ivec target = creativeplacecell(hit);
        if(!insideworld(target) || !insideworld(ivec(target).add(CREATIVE_GRID - 1)) ||
           creativeplayeroverlap(target) || worldtorchincell(target))
            return;

        // Extrude exactly one 16-unit voxel, then deliberately paint every face.
        selinfo placed = hit;
        placed.o = target;
        mpeditface(-1, 1, hit, true);
        mpedittex(getworldcubeslot(selected), 1, placed, true);
        if(m_survival) consumesurvivalitem();
        player1->renderplacemillis = lastmillis;
        player1->renderplacetoggle = !player1->renderplacetoggle;
    }

    static void creativeremove()
    {
        creativetarget target;
        if(!findcreativetarget(target)) return;
        if(target.type == CREATIVE_TARGET_SCATTER)
        {
            int type, mountorient;
            ivec support;
            if(getworldscatterentityedit(target.entity, type, support, mountorient))
                scatteredittrigger(type, support, mountorient, false);
            return;
        }
        mpdelcube(target.cube, true);
    }

#ifndef STANDALONE
    static bool survivalbreakactive = false;
    static creativetarget survivalbreaktarget;
    static int survivalbreakstart = 0;

    static bool samesurvivaltarget(const creativetarget &a,
                                   const creativetarget &b)
    {
        if(a.type != b.type) return false;
        if(a.type == CREATIVE_TARGET_SCATTER) return a.entity == b.entity;
        return a.type == CREATIVE_TARGET_CUBE &&
               a.cube.o == b.cube.o && a.cube.grid == b.cube.grid;
    }

    static int survivalblockitem(const creativetarget &target)
    {
        return getworldcubeindexat(
            ivec(target.cube.o).add(target.cube.grid / 2),
            target.cube.orient);
    }

    static void updatesurvivalbreaking()
    {
        if(!survivalenabled() || !player1->renderattacking)
        {
            survivalbreakactive = false;
            clearbreakstain();
            return;
        }

        creativetarget target;
        if(!findcreativetarget(target))
        {
            survivalbreakactive = false;
            clearbreakstain();
            return;
        }
        if(!survivalbreakactive || !samesurvivaltarget(target, survivalbreaktarget))
        {
            survivalbreaktarget = target;
            survivalbreakstart = lastmillis;
            survivalbreakactive = true;
            if(target.type == CREATIVE_TARGET_CUBE) setbreakstain(target.cube.o, target.cube.grid, 0);
            else clearbreakstain();
            return;
        }
        const int breakmillis = target.type == CREATIVE_TARGET_SCATTER
                              ? SURVIVAL_SCATTER_BREAK_MILLIS
                              : SURVIVAL_BREAK_MILLIS;
        const int elapsed = max(lastmillis - survivalbreakstart, 0);
        if(target.type == CREATIVE_TARGET_CUBE)
        {
            const int stage = clamp(elapsed, 0, SURVIVAL_BREAK_MILLIS - 1) * SURVIVAL_BREAK_STAGES / SURVIVAL_BREAK_MILLIS;
            setbreakstain(target.cube.o, target.cube.grid, stage);
        }
        else clearbreakstain();
        if(elapsed < breakmillis) return;

        int item = -1;
        bool broken = false;
        clearbreakstain();
        if(survivalbreaktarget.type == CREATIVE_TARGET_SCATTER)
        {
            int type, mountorient;
            ivec support;
            if(getworldscatterentityedit(survivalbreaktarget.entity, type, support, mountorient))
            {
                item = numworldcubes() + type;
                scatteredittrigger(type, support, mountorient, false);
                broken = true;
            }
        }
        else
        {
            item = survivalblockitem(survivalbreaktarget);
            mpdelcube(survivalbreaktarget.cube, true);
            broken = true;
        }
        if(broken && !addsurvivalitem(item)) conoutf(CON_WARN, "inventory is full; the broken block was not collected");
        survivalbreakactive = false;
    }
#endif

    void rendercreativetarget()
    {
#ifndef STANDALONE
        if(survivalenabled())
        {
            creativetarget target;
            if(!findcreativetarget(target)) return;
            renderboundingbox(target.center, target.radius);
            return;
        }

        creativetarget target;
        if(!findcreativetarget(target)) return;

        renderboundingbox(target.center, target.radius);
#endif
    }

    ICOMMAND(creativeattack, "D", (int *down),
    {
        if(*down)
        {
            if(player1 && !player1->renderattacking)
            {
                player1->renderattacking = buildenabled();
                player1->renderattackmillis = lastmillis;
                player1->renderattackreleasemillis = -1000;
                if(creativeenabled()) creativeremove();
#ifndef STANDALONE
                else if(survivalenabled()) updatesurvivalbreaking();
#endif
            }
        }
        else if(player1 && player1->renderattacking)
        {
            int elapsed = max(lastmillis - player1->renderattackmillis, 0) % CREATIVE_ARM_CYCLE;
            player1->renderattackreleasepitch = creativearmwave(elapsed);
            player1->renderattackreleasemillis = lastmillis;
            player1->renderattacking = false;
#ifndef STANDALONE
            survivalbreakactive = false;
            clearbreakstain();
#endif
        }
    });
    ICOMMAND(creativeplaceblock, "D", (int *down), { if(*down) creativeplace(); });
    ICOMMAND(creativeselect, "i", (int *index),
    {
        int count = numworldcubes() + numworldscatters();
        creativehotbar[clampcreativehotbarslot()] = *index >= 0 && *index < count ? *index : -1;
    });
    ICOMMAND(creativecycle, "i", (int *dir),
    {
        creativehotbarslot = (clampcreativehotbarslot() - *dir) % CREATIVE_HOTBAR_SLOTS;
        if(creativehotbarslot < 0) creativehotbarslot += CREATIVE_HOTBAR_SLOTS;
    });
    ICOMMAND(creativehotbarselect, "i", (int *slot),
    {
        creativehotbarslot = clamp(*slot, 0, CREATIVE_HOTBAR_SLOTS - 1);
    });
    ICOMMAND(creativehotbarassign, "ii", (int *slot, int *item),
    {
        const int count = numworldcubes() + numworldscatters();
        if(*slot >= 0 && *slot < CREATIVE_HOTBAR_SLOTS)
            creativehotbar[*slot] = *item >= 0 && *item < count ? *item : -1;
    });
    ICOMMAND(creativehotbarswap, "ii", (int *from, int *to),
    {
        if(*from >= 0 && *from < CREATIVE_HOTBAR_SLOTS && *to >= 0 && *to < CREATIVE_HOTBAR_SLOTS)
            swap(creativehotbar[*from], creativehotbar[*to]);
    });
    ICOMMAND(getcreativeblock, "", (), intret(selectedcreativeblock()));
    ICOMMAND(getcreativehotbarslot, "i", (int *slot),
    {
        intret(*slot >= 0 && *slot < CREATIVE_HOTBAR_SLOTS ? creativehotbar[*slot] : -1);
    });
    ICOMMAND(getcreativehotbarselected, "", (), intret(clampcreativehotbarslot()));
    ICOMMAND(creativeactive, "", (), intret(creativeenabled() ? 1 : 0));
    ICOMMAND(survivalactive, "", (), intret(survivalenabled() ? 1 : 0));
    ICOMMAND(getsurvivalinventoryitem, "i", (int *slot),
    {
        intret(*slot >= 0 && *slot < SURVIVAL_USABLE_SLOTS && survivalcounts[*slot] > 0
             ? survivalitems[*slot] : -1);
    });
    ICOMMAND(getsurvivalinventorycount, "i", (int *slot),
    {
        intret(*slot >= 0 && *slot < SURVIVAL_USABLE_SLOTS ? survivalcounts[*slot] : 0);
    });
    ICOMMAND(survivalinventoryswap, "ii", (int *from, int *to),
    {
        if(*from >= 0 && *from < SURVIVAL_USABLE_SLOTS &&
           *to >= 0 && *to < SURVIVAL_USABLE_SLOTS)
        {
            swap(survivalitems[*from], survivalitems[*to]);
            swap(survivalcounts[*from], survivalcounts[*to]);
        }
    });
    ICOMMAND(creativeblockcount, "", (), intret(numworldcubes() + numworldscatters()));
    ICOMMAND(creativecubecount, "", (), intret(numworldcubes()));
    ICOMMAND(creativeblockslot, "i", (int *index), intret(*index < numworldcubes() ? getworldcubeslot(*index) : getworldcubeslot(0)));
    ICOMMAND(creativeblockname, "i", (int *index),
    {
        if(*index < numworldcubes()) result(getworldcubename(*index));
        else result(getworldscattername(*index - numworldcubes()));
    });
    ICOMMAND(creativeblockmodel, "i", (int *index), result(getworldscattermodel(*index - numworldcubes())));
    ICOMMAND(creativeblockicon, "i", (int *index), result(*index < numworldcubes() ? getworldcubetexture(*index) : getworldscattericon(*index - numworldcubes())));

    void gameplayhud(int w, int h) {}
    bool canjump() { return true; }
    bool cancrouch() { return true; }
    bool allowmove(physent *d) { return true; }
    dynent *iterdynents(int i) { return players.inrange(i) ? players[i] : NULL; }
    int numdynents() { return players.length(); }

    int numanims() { return ANIM_GAMESPECIFIC; }
    void findanims(const char *pattern, vector<int> &anims) {}
    void writegamedata(vector<char> &extras) {}
    void readgamedata(vector<char> &extras) {}
    float clipconsole(float w, float h) { return 0; }
    const char *defaultcrosshair(int index) { return "media/interface/crosshair/default.png"; }
    int selectcrosshair(vec &col) { return 0; }
    void setupcamera() {}
    bool allowthirdperson(bool msg) { return true; }
    bool detachcamera() { return false; }
    bool collidecamera() { return false; }

    static bool heldtorchflame(gameent *d, vec &flame)
    {
        return heldtorchemitterposition(d, flame);
    }

    static vec heldtorchparticleorigin;
    static int heldtorchparticlemillis = -1;
    FVARP(hudparticlemovementoffset, 0.0f, 0.25f, 2.0f);
    static vec previoushudparticleorigin, hudparticlemovement;
    static int previoushudparticlemillis = -1, hudparticlemovementmillis = -1;

    void adddynlights()
    {
        addworldtorchlights();
        loopv(players)
        {
            vec flame;
            if(heldtorchflame(players[i], flame))
                adddynlight(flame, 14.0f * CREATIVE_GRID, vec(1.0f, 0.58f, 0.24f), 0, 0, 0, 0, vec(0, 0, 0), players[i]);
        }
    }

    void addparticles()
    {
        addworldtorchparticles();
        heldtorchparticlemillis = -1;
        bool hudtorch = false;
        loopv(players)
        {
            gameent *d = players[i];
            vec flame;
            if(!heldtorchflame(d, flame)) continue;
            if(d == player1 && !isthirdperson())
            {
                hudtorch = true;
                heldtorchparticleorigin = flame;
                heldtorchparticlemillis = lastmillis;
                regular_particle_hud_flame(PART_HUD_FLAME, flame, 0.07f, 0.7f, 0xFF8628, 1, 2.4f, 9.2f, 220.0f, -100, player1);
                regular_particle_hud_flame(PART_HUD_SMOKE, flame, 0.09f, 1.1f, 0xAA8C4E, 1, 3.0f, 4.0f, 1100.0f, -250, player1);
            }
            else
            {
                regular_particle_flame(PART_FLAME, flame, 0.35f, 0.7f, 0xFF8628, 1, 2.4f, 35.0f, 220.0f, -10);
                regular_particle_flame(PART_SMOKE, flame, 0.45f, 1.1f, 0xAA8C4E, 1, 3.0f, 16.0f, 1100.0f, -25);
            }
        }
        if(!hudtorch)
        {
            if(player1) removetrackedparticles(player1);
            previoushudparticlemillis = hudparticlemovementmillis = -1;
            hudparticlemovement = vec(0, 0, 0);
        }
    }

    static void updatehudparticlemovement(physent *owner, const vec &emitter)
    {
        if(hudparticlemovementmillis == totalmillis) return;

        hudparticlemovement = vec(0, 0, 0);
        if(previoushudparticlemillis >= 0)
        {
            const int elapsed = totalmillis - previoushudparticlemillis;
            if(elapsed > 0 && elapsed <= 250 && hudparticlemovementoffset > 0)
            {
                vec velocity(emitter);
                velocity.sub(previoushudparticleorigin).mul(1000.0f/elapsed);
                const float speed = velocity.magnitude();
                if(speed > 1e-4f)
                {
                    const float movement = clamp(speed / max(owner->maxspeed, 1.0f), 0.0f, 1.0f);
                    velocity.div(speed);
                    hudparticlemovement = vec(-velocity.dot(camright), -velocity.dot(camdir), -velocity.dot(camup)).mul(hudparticlemovementoffset * movement);
                }
            }
        }
        previoushudparticleorigin = emitter;
        previoushudparticlemillis = hudparticlemovementmillis = totalmillis;
    }

    void particletrack(physent *owner, vec &o, vec &d) {}

    void hudparticletrack(physent *owner, vec &o, vec &d, int age)
    {
        if(!owner || owner != player1 || heldtorchparticlemillis != lastmillis) return;
        vec emitter;
        if(heldtorchemitterposition(player1, emitter)) heldtorchparticleorigin = emitter;
        updatehudparticlemovement(owner, heldtorchparticleorigin);
        o.madd(hudparticlemovement, age/500.0f);
        const vec localorigin(o), localvelocity(d);
        o = vec(heldtorchparticleorigin).madd(camright, localorigin.x).madd(camdir, localorigin.y).madd(camup, localorigin.z);
        d = vec(camright).mul(localvelocity.x).madd(camdir, localvelocity.y).madd(camup, localvelocity.z);
    }
    void dynlighttrack(physent *owner, vec &o, vec &hud) {}
    int maxsoundradius(int n) { return 500; }
    // The procedural world is unbounded while the engine minimap assumes one
    // stable, finite octree. The runtime octree is only a moving chunk window,
    // so a finite-map texture is neither valid nor safe during world rebuilds.
    bool needminimap() { return false; }

    static void sendposition(gameent *d, packetbuf &q)
    {
        putint(q, N_POS);
        putuint(q, d->clientnum);

        vec feet = d->feetpos();
        if(waitforserveredit()) worldpositiontoabsolute(feet);
        ivec o = ivec(feet.mul(DMF));
        putint(q, o.x);
        putint(q, o.y);
        putint(q, o.z);

        // 3 bits physics state, 2 bits movement, and 2 bits strafing.
        uchar physstate = d->physstate | ((d->move&3)<<4) | ((d->strafe&3)<<6);
        q.put(physstate);

        uint vel = min(int(d->vel.magnitude()*DVELF), 0xFFFF),
             fall = min(int(d->falling.magnitude()*DVELF), 0xFFFF);

        // Extended movement data in the low byte; the high bits carry the
        // selected creative item plus one, leaving zero to mean no held item.
        uint flags = 0;
        if(d->crouching) flags |= 1<<0;
        if(d->renderattacking) flags |= 1<<1;
        if(d->renderplacetoggle) flags |= 1<<2;
        const int selected = selectedcreativeblock();
        if(buildenabled() && selected >= 0) flags |= uint(selected + 1)<<8;
        if(vel > 0xFF) flags |= 1<<3;
        if(fall > 0)
        {
            flags |= 1<<4;
            if(fall > 0xFF) flags |= 1<<5;
            if(d->falling.x || d->falling.y || d->falling.z > 0) flags |= 1<<6;
        }
        if((lookupmaterial(d->feetpos())&MATF_CLIP) == MAT_GAMECLIP) flags |= 1<<7;
        putuint(q, flags);

        uint dir = (d->yaw < 0 ? 360 + int(d->yaw)%360 : int(d->yaw)%360)
                 + clamp(int(d->pitch + 90), 0, 180)*360;
        q.put(dir&0xFF);
        q.put((dir>>8)&0xFF);
        q.put(clamp(int(d->roll + 90), 0, 180));

        q.put(vel&0xFF);
        if(flags&(1<<3)) q.put((vel>>8)&0xFF);
        float velyaw, velpitch;
        vectoyawpitch(d->vel, velyaw, velpitch);
        uint veldir = (velyaw < 0 ? 360 + int(velyaw)%360 : int(velyaw)%360)
                    + clamp(int(velpitch + 90), 0, 180)*360;
        q.put(veldir&0xFF);
        q.put((veldir>>8)&0xFF);

        if(flags&(1<<4))
        {
            q.put(fall&0xFF);
            if(flags&(1<<5)) q.put((fall>>8)&0xFF);
            if(flags&(1<<6))
            {
                float fallyaw, fallpitch;
                vectoyawpitch(d->falling, fallyaw, fallpitch);
                uint falldir = (fallyaw < 0 ? 360 + int(fallyaw)%360 : int(fallyaw)%360)
                              + clamp(int(fallpitch + 90), 0, 180)*360;
                q.put(falldir&0xFF);
                q.put((falldir>>8)&0xFF);
            }
        }
    }

    static void updateremotepos(gameent *d)
    {
        const float r = player1->radius + d->radius,
                    dx = player1->o.x - d->o.x,
                    dy = player1->o.y - d->o.y,
                    dz = player1->o.z - d->o.z,
                    rz = player1->aboveeye + d->eyeheight,
                    fx = fabs(dx), fy = fabs(dy), fz = fabs(dz);
        if(fx < r && fy < r && fz < rz && player1->state != CS_SPECTATOR && d->state != CS_DEAD)
        {
            if(fx < fy) d->o.y += dy < 0 ? r - fy : -(r - fy);
            else d->o.x += dx < 0 ? r - fx : -(r - fx);
        }

        int now = totalmillis ? totalmillis : 1,
            lagtime = now - d->lastupdate;
        if(lagtime)
        {
            if(d->state != CS_SPAWNING && d->lastupdate) d->plag = (d->plag*5 + lagtime)/6;
            d->lastupdate = now;
        }
    }

    void c2sinfo(bool force)
    {
        if(!connected || !player1 || player1->clientnum < 0) return;

        if(strcmp(sentname, player1->name))
        {
            addmsg(N_INITCLIENT, "s", player1->name);
            copystring(sentname, player1->name);
        }

        if(!force && totalmillis - lastpositionsend < 33) return;
        lastpositionsend = totalmillis;
        {
            // packetbuf inspects its ENet packet when it leaves scope. Release
            // builds can transmit and free an unreliable packet immediately
            // during flushclient(), so relinquish the stack wrapper first.
            packetbuf p(100);
            sendposition(player1, p);
            sendclientpacket(p.finalize(), 0);
        }
        flushclient();
    }

    void parsepacketclient(int chan, packetbuf &p)
    {
        if(chan == 0)
        {
            while(p.remaining())
            {
                int type = getint(p);
                if(type != N_POS)
                {
                    p.pad(p.remaining());
                    break;
                }

                int cn = getuint(p);
                vec pos;
                // Packet reads have side effects, so preserve the wire order
                // explicitly. A vec(getint(), getint(), getint()) expression
                // may be evaluated right-to-left by optimized compilers.
                loopk(3) pos[k] = getint(p)/DMF;
                int physstate = p.get();
                uint flags = getuint(p);
                vec vel, falling;
                int dir = p.get();
                dir |= p.get()<<8;
                float yaw = dir%360, pitch = clamp(dir/360, 0, 180) - 90,
                      roll = clamp(int(p.get()), 0, 180) - 90;
                int mag = p.get();
                if(flags&(1<<3)) mag |= p.get()<<8;
                dir = p.get();
                dir |= p.get()<<8;
                vecfromyawpitch(dir%360, clamp(dir/360, 0, 180) - 90, 1, 0, vel);
                vel.mul(mag/DVELF);
                if(flags&(1<<4))
                {
                    mag = p.get();
                    if(flags&(1<<5)) mag |= p.get()<<8;
                    if(flags&(1<<6))
                    {
                        dir = p.get();
                        dir |= p.get()<<8;
                        vecfromyawpitch(dir%360, clamp(dir/360, 0, 180) - 90, 1, 0, falling);
                    }
                    else falling = vec(0, 0, -1);
                    falling.mul(mag/DVELF);
                }
                else falling = vec(0, 0, 0);
                if(p.overread()) return;
                if(waitforserveredit()) worldpositiontolocal(pos);

                gameent *d = clients.inrange(cn) ? clients[cn] : NULL;
                if(!d || d == player1) continue;

                float oldyaw = d->yaw, oldpitch = d->pitch, oldroll = d->roll;
                vec oldpos(d->o);
                d->yaw = yaw;
                d->pitch = pitch;
                d->roll = roll;
                d->move = (physstate>>4)&2 ? -1 : (physstate>>4)&1;
                d->strafe = (physstate>>6)&2 ? -1 : (physstate>>6)&1;
                d->crouching = flags&(1<<0) ? -1 : 0;
                bool attacking = (flags&(1<<1)) != 0;
                bool placetoggle = (flags&(1<<2)) != 0;
                const uint helditem = flags>>8;
                d->selectedcreative = helditem ? int(helditem - 1) : -1;
                if(!d->renderactioninitialized)
                {
                    d->renderattacking = attacking;
                    d->renderplacetoggle = placetoggle;
                    if(attacking)
                    {
                        d->renderattackmillis = lastmillis;
                        d->renderattackreleasemillis = -1000;
                    }
                    d->renderactioninitialized = true;
                }
                else
                {
                    if(attacking != d->renderattacking)
                    {
                        if(attacking)
                        {
                            d->renderattackmillis = lastmillis;
                            d->renderattackreleasemillis = -1000;
                        }
                        else
                        {
                            int elapsed = max(lastmillis - d->renderattackmillis, 0) % CREATIVE_ARM_CYCLE;
                            d->renderattackreleasepitch = creativearmwave(elapsed);
                            d->renderattackreleasemillis = lastmillis;
                        }
                        d->renderattacking = attacking;
                    }
                    if(placetoggle != d->renderplacetoggle)
                    {
                        d->renderplacetoggle = placetoggle;
                        d->renderplacemillis = lastmillis;
                    }
                }
                d->o = pos;
                d->o.z += d->eyeheight;
                d->vel = vel;
                d->falling = falling;
                d->physstate = physstate&7;
                updatephysstate(d);
                updateremotepos(d);

                if(smoothmove && d->smoothmillis >= 0 && oldpos.dist(d->o) < smoothdist)
                {
                    d->newpos = d->o;
                    d->newyaw = d->yaw;
                    d->newpitch = d->pitch;
                    d->newroll = d->roll;
                    d->o = oldpos;
                    d->yaw = oldyaw;
                    d->pitch = oldpitch;
                    d->roll = oldroll;
                    (d->deltapos = oldpos).sub(d->newpos);
                    d->deltayaw = oldyaw - d->newyaw;
                    if(d->deltayaw > 180) d->deltayaw -= 360;
                    else if(d->deltayaw < -180) d->deltayaw += 360;
                    d->deltapitch = oldpitch - d->newpitch;
                    d->deltaroll = oldroll - d->newroll;
                    d->smoothmillis = lastmillis;
                }
                else d->smoothmillis = 0;
                if(d->state == CS_LAGGED || d->state == CS_SPAWNING) d->state = CS_ALIVE;
            }
            return;
        }

        if(chan == 2)
        {
            int type = getint(p);
            if(type == N_SENDMAP)
            {
                defformatstring(mname, "getmap_%d", lastmillis);
                defformatstring(fname, "media/map/%s.ogz", mname);
                stream *map = openrawfile(path(fname), "wb");
                if(map)
                {
                    ucharbuf b = p.subbuf(p.remaining());
                    map->write(b.buf, b.maxlen);
                    delete map;
                    load_world(mname, clientmap[0] ? clientmap : NULL);
                    remove(findfile(fname, "rb"));
                }
            }
            return;
        }

        while(p.remaining())
        {
            int type = getint(p);
            switch(type)
            {
            case N_SERVINFO:
            {
                int cn = getint(p), prot = getint(p);
                if(prot != PROTOCOL_VERSION)
                {
                    conoutf(CON_ERROR, "protocol mismatch: client %d, server %d", PROTOCOL_VERSION, prot);
                    disconnect();
                    return;
                }
                sessionid = getint(p);
                if(player1) player1->clientnum = cn;
                getint(p);
                getstring(servdesc, p, sizeof(servdesc));
                string unused;
                getstring(unused, p, sizeof(unused));
                break;
            }
            case N_WELCOME:
                connected = true;
                notifywelcome();
                break;
            case N_INITCLIENT:
            {
                int cn = getint(p);
                string name;
                getstring(name, p, sizeof(name));
                gameent *d = newclient(cn);
                if(d) filtertext(d->name, name, false, false, MAXSTRLEN);
                break;
            }
            case N_CDIS:
                removeclient(getint(p));
                break;
            case N_MAPCHANGE:
            {
                string name;
                getstring(name, p, sizeof(name));
                int mode = getint(p);
                getint(p);
                gamemode = m_valid(mode) ? mode : STARTGAMEMODE;
                copystring(clientmap, name);
                if(name[0]) load_world(name);
                break;
            }
            case N_SERVMSG:
            {
                string text;
                getstring(text, p, sizeof(text));
                conoutf("%s", text);
                break;
            }
            case N_EDITAUTHOR:
                authoritativeauthor = getint(p);
                authoritativerevision = uint(getint(p));
                break;
            case N_WORLDSTATE:
            {
                pendingnetworkseed = getint(p);
                synchronizedrevision = uint(getint(p));
                pendingnetworktime = getint(p);
                pendingnetworkfrozen = getint(p) != 0;
                pendingnetworkreset = getint(p) != 0;
                pendingnetworkrestoreposition = pendingnetworkreset && player1;
                if(pendingnetworkrestoreposition)
                {
                    pendingnetworkposition = player1->o;
                    worldpositiontoabsolute(pendingnetworkposition);
                }
                pendingnetworkedits.deletecontents();
                authoritativeauthor = -1;
                authoritativerevision = 0;
                pendingnetworkworld = true;
                break;
            }
            case N_WORLDSYNC:
                synchronizedrevision = uint(getint(p));
                processnetworkedits();
                break;
            case N_WORLDTIME:
            {
                // Packet reads mutate p. Read in wire order instead of relying
                // on function-argument evaluation order in optimized builds.
                const int timemillis = getint(p);
                const bool frozen = getint(p) != 0;
                if(pendingnetworkworld)
                {
                    pendingnetworktime = timemillis;
                    pendingnetworkfrozen = frozen;
                }
                else environment::synctime(timemillis, frozen);
                break;
            }
            case N_EDITMODE:
            {
                // The server uses this only to cancel an unauthorized local
                // toggle. Never let a server packet force a client into edit.
                const bool enabled = getint(p) != 0;
                if(!enabled && editmode) toggleedit(true);
                break;
            }
            case N_SETPRIVILEGE:
            {
                int cn = getint(p), privilege = getint(p);
                gameent *d = newclient(cn);
                if(d)
                {
                    d->privilege = privilege;
                    if(d == player1 && privilege < PRIV_ADMIN && editmode)
                        toggleedit(true);
                }
                break;
            }
            case N_EDITENT:
            {
                int i = getint(p);
                float x = getint(p)/DMF, y = getint(p)/DMF, z = getint(p)/DMF;
                int type = getint(p), attr1 = getint(p), attr2 = getint(p), attr3 = getint(p), attr4 = getint(p), attr5 = getint(p);
                mpeditent(i, vec(x, y, z), type, attr1, attr2, attr3, attr4, attr5, false);
                break;
            }
            case N_CLIPBOARD:
            {
                int cn = getint(p), unpacklen = getint(p), packlen = getint(p);
                ucharbuf q = p.subbuf(max(packlen, 0));
                if(player1 && cn == player1->clientnum) unpackeditinfo(player1->edit, q.buf, q.maxlen, unpacklen);
                break;
            }
            case N_UNDO:
            case N_REDO:
            {
                getint(p);
                int unpacklen = getint(p), packlen = getint(p);
                ucharbuf q = p.subbuf(max(packlen, 0));
                unpackundo(q.buf, q.maxlen, unpacklen);
                break;
            }
            case N_EDITF:
            case N_EDITT:
            case N_EDITM:
            case N_FLIP:
            case N_COPY:
            case N_PASTE:
            case N_ROTATE:
            case N_REPLACE:
            case N_DELCUBE:
            case N_EDITVSLOT:
            case N_EDITSCATTER:
            {
                networkedit *edit = new networkedit;
                edit->type = type;
                edit->author = authoritativeauthor;
                edit->revision = authoritativerevision;
                getsel(p, edit->selection);
                switch(type)
                {
                    case N_EDITF:
                        edit->args[0] = getint(p);
                        edit->args[1] = getint(p);
                        break;
                    case N_EDITT:
                    {
                        edit->args[0] = getint(p);
                        edit->args[1] = getint(p);
                        if(p.remaining() < 2) { delete edit; return; }
                        int extra = lilswap(*(const ushort *)p.pad(2));
                        if(p.remaining() < extra) { delete edit; return; }
                        ucharbuf ebuf = p.subbuf(extra);
                        edit->extra.put(ebuf.buf, ebuf.maxlen);
                        break;
                    }
                    case N_EDITM:
                        edit->args[0] = getint(p);
                        edit->args[1] = getint(p);
                        break;
                    case N_FLIP: break;
                    case N_COPY:
                    case N_PASTE:
                        delete edit;
                        edit = NULL;
                        break;
                    case N_ROTATE: edit->args[0] = getint(p); break;
                    case N_REPLACE:
                    {
                        edit->args[0] = getint(p);
                        edit->args[1] = getint(p);
                        edit->args[2] = getint(p);
                        if(p.remaining() < 2) { delete edit; return; }
                        int extra = lilswap(*(const ushort *)p.pad(2));
                        if(p.remaining() < extra) { delete edit; return; }
                        ucharbuf ebuf = p.subbuf(extra);
                        edit->extra.put(ebuf.buf, ebuf.maxlen);
                        break;
                    }
                    case N_DELCUBE: break;
                    case N_EDITSCATTER:
                        edit->args[0] = getint(p);
                        edit->args[1] = getint(p);
                        break;
                    case N_EDITVSLOT:
                    {
                        edit->args[0] = getint(p);
                        edit->args[1] = getint(p);
                        if(p.remaining() < 2) { delete edit; return; }
                        int extra = lilswap(*(const ushort *)p.pad(2));
                        if(p.remaining() < extra) { delete edit; return; }
                        ucharbuf ebuf = p.subbuf(extra);
                        edit->extra.put(ebuf.buf, ebuf.maxlen);
                        break;
                    }
                }
                if(edit)
                {
                    pendingnetworkedits.add(edit);
                    processnetworkedits();
                }
                authoritativeauthor = -1;
                authoritativerevision = 0;
                break;
            }
            case N_CALCLIGHT:
                mpcalclight(false);
                break;
            case N_REMIP:
                mpremip(false);
                break;
            case N_NEWMAP:
            {
                int size = getint(p);
                if(size >= 0) emptymap(size, true, NULL);
                else enlargemap(true);
                break;
            }
            default:
                p.pad(p.remaining());
                break;
            }
        }
    }

    ICOMMAND(name, "sN", (char *s, int *numargs),
    {
        if(*numargs > 0 && player1) filtertext(player1->name, s, false, false, MAXSTRLEN);
        else result(player1 ? player1->name : "camera");
    });
    ICOMMAND(getname, "", (), result(player1 ? player1->name : "camera"));
    ICOMMAND(getclientnum, "s", (char *name), intret(player1 ? player1->clientnum : -1));
    ICOMMAND(getclientcolorname, "i", (int *cn), result(player1 ? player1->name : "camera"));
    ICOMMAND(getclientfrags, "i", (int *cn), intret(0));
    ICOMMAND(getclientflags, "i", (int *cn), intret(0));
    ICOMMAND(getclientdeaths, "i", (int *cn), intret(0));
    ICOMMAND(getclientteam, "i", (int *cn), intret(0));
    ICOMMAND(getclientmodel, "i", (int *cn), intret(-1));
    ICOMMAND(getclientcolor, "i", (int *cn), intret(0xFFFFFF));
    ICOMMAND(ismaster, "i", (int *cn),
    {
        gameent *d = clients.inrange(*cn) ? clients[*cn] : NULL;
        if(player1 && player1->clientnum == *cn) d = player1;
        intret(d && d->privilege >= PRIV_ADMIN ? 1 : 0);
    });
    ICOMMAND(isadmin, "i", (int *cn),
    {
        gameent *d = clients.inrange(*cn) ? clients[*cn] : NULL;
        if(player1 && player1->clientnum == *cn) d = player1;
        intret(d && d->privilege >= PRIV_ADMIN ? 1 : 0);
    });
    ICOMMAND(setmaster, "ss", (char *password, char *who),
    {
        if(who[0])
        {
            conoutf(CON_ERROR, "delegating admin is not supported; each admin must authenticate");
            return;
        }
        addmsg(N_SETMASTER, "rs", password);
    });
    ICOMMAND(isai, "ii", (int *cn, int *type), intret(0));
    ICOMMAND(isspectator, "i", (int *cn), intret(0));
    ICOMMAND(isdead, "i", (int *cn), intret(0));
    ICOMMAND(getmastermode, "", (), intret(mastermode));
    ICOMMAND(getmastermodename, "i", (int *mm), result((*mm >= 0 && *mm < 3) ? mastermodes[*mm] : ""));
    ICOMMAND(getmode, "", (), intret(gamemode));
    ICOMMAND(getmodeprettyname, "i", (int *mode), result(m_valid(*mode) ? gamemodes[*mode - STARTGAMEMODE].prettyname : ""));
    ICOMMAND(mode, "iN", (int *mode, int *numargs),
    {
        if(*numargs > 0)
        {
            if(m_valid(*mode))
            {
                gamemode = *mode;
                intret(1);
            }
            else intret(0);
        }
        else intret(gamemode);
    });
    ICOMMAND(map, "sN", (char *name, int *numargs),
    {
        if(*numargs > 0 && name[0]) changemap(name, gamemode);
        else if(clientmap[0]) changemap(clientmap, gamemode);
        else emptymap(0, true, NULL);
    });
    ICOMMAND(m_timed, "i", (int *mode), intret(0));
    ICOMMANDN(m_edit, _icmd_m_edit_cmd, "i", (int *mode),
              intret(m_valid(*mode) && (gamemodes[*mode - STARTGAMEMODE].flags&M_EDIT) ? 1 : 0));
    ICOMMANDN(m_creative, _icmd_m_creative_cmd, "i", (int *mode),
              intret(m_valid(*mode) && (gamemodes[*mode - STARTGAMEMODE].flags&M_CREATIVE) ? 1 : 0));
    ICOMMANDN(m_survival, _icmd_m_survival_cmd, "i", (int *mode),
              intret(m_valid(*mode) && (gamemodes[*mode - STARTGAMEMODE].flags&M_SURVIVAL) ? 1 : 0));
    ICOMMANDN(m_ctf, _icmd_m_ctf_cmd, "i", (int *mode), intret(0));
    ICOMMANDN(m_teammode, _icmd_m_teammode_cmd, "i", (int *mode), intret(0));
    ICOMMAND(getfollow, "", (), intret(-1));
    ICOMMAND(nextfollow, "i", (int *dir), {});
    VARP(specmode, 0, 0, 2);
    ICOMMAND(spectator, "is", (int *val, char *who), {});
    ICOMMAND(team, "sN", (char *s, int *numargs), { if(*numargs < 0) result(""); });
    ICOMMAND(sayteam, "C", (char *text), toserver(text));
    ICOMMAND(shoot, "D", (int *down), {});
    ICOMMAND(melee, "D", (int *down), {});
    ICOMMAND(taunt, "", (), {});
    ICOMMAND(allowthirdperson, "b", (int *msg), intret(1));
    ICOMMAND(getdebugplayerspeed, "", (),
    {
        defformatstring(speed, "%.2f", horizontalmeterspersecond(player1));
        result(speed);
    });
    ICOMMAND(getplayercolor, "ii", (int *model, int *team), intret(0xFFFFFF));
    ICOMMAND(showscores, "D", (int *down), {});
    ICOMMAND(refreshscoreboard, "", (), {});
    ICOMMAND(loopscoreboard, "rie", (ident *id, int *team, uint *body),
    {
        if(!player1) return;
        identstack stack;
        loopiter(id, stack, player1->clientnum);
        execute(body);
        loopend(id, stack);
    });
    ICOMMAND(getteamscore, "i", (int *team), intret(0));
    ICOMMAND(scoreboardpj, "i", (int *cn), intret(0));
    ICOMMAND(scoreboardping, "i", (int *cn), intret(player1 ? player1->ping : 0));
    ICOMMAND(scoreboardstatus, "i", (int *cn), intret(0xFFFFFF));
    ICOMMAND(scoreboardmultiplayer, "", (), intret(multiplayer(false)));
#endif
}

namespace server
{
    enum
    {
        SERVER_DAY_MILLIS = 20 * 60 * 1000,
        SERVER_START_MILLIS = 8 * SERVER_DAY_MILLIS / 24,
        SERVER_JOURNAL_VERSION = 1
    };

    SVAR(serverpass, "");
    SVAR(adminpass, "");
    SVAR(serverworld, "multiplayer");
    SVAR(serverdesc, "Cube-Craft authoritative server");
    SVAR(servermotd, "");
    VAR(serverworldseed, 0, 1337, INT_MAX);

    struct clientinfo
    {
        int clientnum, privilege, lastpositionmillis;
        bool connected, local, worldready, hasposition;
        string name;
        vector<uchar> position;
        vec o;
        ENetPacket *getmap;

        clientinfo() : clientnum(-1), privilege(PRIV_NONE), lastpositionmillis(0),
                       connected(false), local(false),
                       worldready(false), hasposition(false), o(0, 0, 0), getmap(NULL)
        {
            name[0] = '\0';
        }
    };

    struct serveredit
    {
        uint revision, timestamp;
        int author, type;
        bool active, hasselection;
        selinfo selection;
        vector<uchar> payload;

        serveredit() : revision(0), timestamp(0), author(-1), type(-1),
                       active(true), hasselection(false) {}
    };

    vector<clientinfo *> clients;
    vector<serveredit *> worldhistory, worldredostack;
    string smapname = "";
    stream *mapdata = NULL;
    int gamemode = STARTGAMEMODE;
    uint worldeditrevision = 0;
    int worldclockmillis = SERVER_START_MILLIS, lastworldtimesync = 0;
    bool worldtimefrozen = false, serverworldready = true, journalinitialized = false;

    static void journalput32(vector<uchar> &out, uint value)
    {
        value = lilswap(value);
        out.put((uchar *)&value, sizeof(value));
    }

    static uint journalchecksum(const uchar *data, int length)
    {
        uint hash = 2166136261U;
        loopi(length) { hash ^= data[i]; hash *= 16777619U; }
        return hash;
    }

    static bool journalread32(ucharbuf &p, uint &value)
    {
        if(p.remaining() < 4) return false;
        memcpy(&value, p.pad(4), 4);
        value = lilswap(value);
        return true;
    }

    static bool readselection(ucharbuf &p, selinfo &sel)
    {
        sel.o.x = getint(p); sel.o.y = getint(p); sel.o.z = getint(p);
        sel.s.x = getint(p); sel.s.y = getint(p); sel.s.z = getint(p);
        sel.grid = getint(p); sel.orient = getint(p);
        sel.cx = getint(p); sel.cxs = getint(p); sel.cy = getint(p); sel.cys = getint(p);
        sel.corner = getint(p);
        return !p.overread();
    }

    static bool editselectiontype(int type)
    {
        return type == N_EDITF || type == N_EDITT || type == N_EDITM ||
               type == N_FLIP || type == N_ROTATE || type == N_REPLACE ||
               type == N_DELCUBE || type == N_EDITVSLOT ||
               type == N_EDITSCATTER;
    }

    static void updateservereditmetadata(serveredit &edit)
    {
        edit.hasselection = false;
        if(!editselectiontype(edit.type)) return;
        ucharbuf p(edit.payload.getbuf(), edit.payload.length());
        if(readselection(p, edit.selection)) edit.hasselection = true;
    }

    static void serverjournalname(char *name, size_t len)
    {
        string safe;
        int n = 0;
        for(const char *s = serverworld; *s && n < int(sizeof(safe)) - 1; ++s)
            if(iscubealnum(*s) || *s == '_' || *s == '-') safe[n++] = *s;
        safe[n] = '\0';
        if(!safe[0]) copystring(safe, "multiplayer");
        snprintf(name, len, "media/map/%s/server.diff", safe);
        path(name);
    }

    static bool writeserverjournalheader(stream &file)
    {
        return file.write("CCJ1", 4) == 4 &&
               file.putlil<uint>(SERVER_JOURNAL_VERSION) &&
               file.putlil<uint>(PROTOCOL_VERSION) &&
               file.putlil<uint>(uint(serverworldseed)) &&
               file.putlil<uint>(worldeditrevision);
    }

    static bool writeserveredit(stream &file, const serveredit &edit)
    {
        vector<uchar> body;
        journalput32(body, edit.revision);
        journalput32(body, edit.timestamp);
        journalput32(body, uint(edit.author));
        journalput32(body, uint(edit.type));
        journalput32(body, edit.active ? 1U : 0U);
        journalput32(body, uint(edit.payload.length()));
        body.put(edit.payload.getbuf(), edit.payload.length());
        return file.write("OP01", 4) == 4 &&
               file.putlil<uint>(uint(body.length())) &&
               file.putlil<uint>(journalchecksum(body.getbuf(), body.length())) &&
               file.write(body.getbuf(), body.length()) == size_t(body.length());
    }

    static bool rewriteserverjournal()
    {
        string filename;
        serverjournalname(filename, sizeof(filename));
        stream *file = openrawfile(filename, "wb");
        if(!file) return false;
        bool ok = writeserverjournalheader(*file);
        loopv(worldhistory) if(ok) ok = writeserveredit(*file, *worldhistory[i]);
        delete file;
        if(!ok) conoutf(CON_ERROR, "could not write authoritative world journal %s", filename);
        return ok;
    }

    static bool appendserveredit(const serveredit &edit)
    {
        string filename;
        serverjournalname(filename, sizeof(filename));
        stream *file = openrawfile(filename, "ab");
        if(!file) return false;
        bool ok = writeserveredit(*file, edit);
        delete file;
        return ok;
    }

    static void loadserverjournal()
    {
        worldhistory.deletecontents();
        worldredostack.deletecontents();
        worldeditrevision = 0;
        serverworldready = true;

        string filename;
        serverjournalname(filename, sizeof(filename));
        stream *file = openrawfile(filename, "rb");
        if(!file)
        {
            if(!rewriteserverjournal()) serverworldready = false;
            return;
        }

        char magic[4];
        uint version = 0, protocol = 0, seed = 0, headerrevision = 0;
        if(file->read(magic, 4) != 4 || memcmp(magic, "CCJ1", 4) ||
           (version = file->getlil<uint>()) != SERVER_JOURNAL_VERSION ||
           (protocol = file->getlil<uint>()) != PROTOCOL_VERSION ||
           (seed = file->getlil<uint>()) != uint(serverworldseed))
        {
            conoutf(CON_ERROR, "authoritative journal %s is incompatible (version %u, protocol %u, seed %u; configured seed %d)",
                    filename, version, protocol, seed, serverworldseed);
            serverworldready = false;
            delete file;
            return;
        }
        headerrevision = file->getlil<uint>();
        worldeditrevision = headerrevision;

        bool recovered = false;
        while(!file->end())
        {
            if(file->read(magic, 4) != 4) break;
            uint length = file->getlil<uint>(), checksum = file->getlil<uint>();
            if(memcmp(magic, "OP01", 4) || length < 24 || length > uint(MAXTRANS + 64))
            {
                recovered = true;
                break;
            }
            vector<uchar> body;
            // vector::setsize() only shrinks an existing allocation. Using it
            // here left getbuf() unallocated in release builds, so every valid
            // first record looked like a corrupt tail after a restart.
            uchar *bodybuf = body.pad(length);
            if(file->read(bodybuf, length) != length ||
               journalchecksum(body.getbuf(), body.length()) != checksum)
            {
                recovered = true;
                break;
            }
            ucharbuf p(body.getbuf(), body.length());
            uint revision, timestamp, author, type, active, payloadlen;
            if(!journalread32(p, revision) || !journalread32(p, timestamp) ||
               !journalread32(p, author) || !journalread32(p, type) ||
               !journalread32(p, active) || !journalread32(p, payloadlen) ||
               payloadlen != uint(p.remaining()))
            {
                recovered = true;
                break;
            }
            serveredit *edit = new serveredit;
            edit->revision = revision;
            edit->timestamp = timestamp;
            edit->author = int(author);
            edit->type = int(type);
            edit->active = active != 0;
            edit->payload.put(p.pad(payloadlen), payloadlen);
            updateservereditmetadata(*edit);
            worldhistory.add(edit);
            worldeditrevision = max(worldeditrevision, revision);
        }
        delete file;
        if(recovered)
        {
            conoutf(CON_WARN, "authoritative journal had a corrupt tail; recovered %d valid revisions",
                    worldhistory.length());
            // Remove an actually incomplete tail before future appends;
            // otherwise every later record would remain hidden behind it.
            if(!rewriteserverjournal()) serverworldready = false;
        }
        conoutf("loaded %d authoritative world revisions for seed %d",
                worldhistory.length(), serverworldseed);
    }

    static bool ensureserverworld()
    {
        if(!journalinitialized)
        {
            journalinitialized = true;
            loadserverjournal();
        }
        return serverworldready;
    }

    clientinfo *getinfo(int n)
    {
        return clients.inrange(n) ? clients[n] : NULL;
    }

    void *newclientinfo() { return new clientinfo; }
    void deleteclientinfo(void *info)
    {
        clientinfo *ci = (clientinfo *)info;
        if(ci && clients.inrange(ci->clientnum) && clients[ci->clientnum] == ci)
            clients[ci->clientnum] = NULL;
        delete ci;
    }
    static void sendprivilege(int cn, int subject, int privilege)
    {
        sendf(cn, 1, "ri3", N_SETPRIVILEGE, subject, privilege);
    }

    static void sendworldtime(int cn = -1)
    {
        sendf(cn, 1, "ri3", N_WORLDTIME, worldclockmillis, worldtimefrozen ? 1 : 0);
    }

    static void sendserveredit(int cn, const serveredit &edit)
    {
        packetbuf q(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(q, N_EDITAUTHOR);
        putint(q, edit.author);
        putint(q, int(edit.revision));
        putint(q, edit.type);
        q.put(edit.payload.getbuf(), edit.payload.length());
        sendpacket(cn, 1, q.finalize());
    }

    static void sendworldstate(clientinfo &ci, bool reset)
    {
        ci.worldready = false;
        sendf(ci.clientnum, 1, "ri6", N_WORLDSTATE, serverworldseed,
              int(worldeditrevision), worldclockmillis, worldtimefrozen ? 1 : 0,
              reset ? 1 : 0);
    }

    static void replayworld(clientinfo &ci)
    {
        loopv(worldhistory) if(worldhistory[i]->active)
            sendserveredit(ci.clientnum, *worldhistory[i]);
        sendf(ci.clientnum, 1, "ri2", N_WORLDSYNC, int(worldeditrevision));
        ci.worldready = true;
    }

    static void resetallclients()
    {
        loopv(clients) if(clients[i] && clients[i]->connected)
            sendworldstate(*clients[i], true);
    }

    void serverinit()
    {
        copystring(smapname, serverworld);
        journalinitialized = false;
        worldclockmillis = SERVER_START_MILLIS;
        worldtimefrozen = false;
        lastworldtimesync = 0;
    }
    int reserveclients() { return 0; }
    int numchannels() { return 3; }
    void clientdisconnect(int n)
    {
        if(clientinfo *ci = getinfo(n))
        {
            if(ci->connected) sendf(-1, 1, "ri2x", N_CDIS, n, n);
            ci->connected = false;
        }
    }

    int clientconnect(int n, uint ip)
    {
        if(!ensureserverworld()) return DISC_PRIVATE;
        while(clients.length() <= n) clients.add(NULL);
        clientinfo *ci = (clientinfo *)getclientinfo(n);
        clients[n] = ci;
        ci->clientnum = n;
        ci->connected = false;
        ci->local = false;
        sendf(n, 1, "ri5ss", N_SERVINFO, n, PROTOCOL_VERSION, rnd(INT_MAX), 0, serverdesc, "");
        return DISC_NONE;
    }

    void localconnect(int n)
    {
        if(!ensureserverworld()) return;
        while(clients.length() <= n) clients.add(NULL);
        clientinfo *ci = (clientinfo *)getclientinfo(n);
        clients[n] = ci;
        ci->clientnum = n;
        ci->connected = ci->local = true;
        ci->privilege = PRIV_ADMIN;
        sendf(n, 1, "ri5ss", N_SERVINFO, n, PROTOCOL_VERSION, rnd(INT_MAX), 0, serverdesc, "");
        sendf(n, 1, "ri", N_WELCOME);
        sendprivilege(n, n, ci->privilege);
        sendworldstate(*ci, false);
    }

    void localdisconnect(int n) { clientdisconnect(n); }
    bool allowbroadcast(int n) { clientinfo *ci = getinfo(n); return ci && ci->connected; }
    void recordpacket(int chan, void *data, int len) {}

    static bool validselection(const clientinfo &ci, const selinfo &sel, const char *&error)
    {
        if(sel.grid <= 0 || sel.grid > 4096 || (sel.grid & (sel.grid - 1)) ||
           sel.s.x <= 0 || sel.s.y <= 0 || sel.s.z <= 0 ||
           sel.orient < 0 || sel.orient > 5 ||
           sel.o.x % sel.grid || sel.o.y % sel.grid || sel.o.z % sel.grid)
        {
            error = "invalid or unaligned edit selection";
            return false;
        }
        long long volume = (long long)sel.s.x * sel.s.y * sel.s.z,
                  maxx = (long long)sel.o.x + (long long)sel.s.x * sel.grid,
                  maxy = (long long)sel.o.y + (long long)sel.s.y * sel.grid,
                  maxz = (long long)sel.o.z + (long long)sel.s.z * sel.grid;
        if(volume <= 0 || volume > (1 << 20) ||
           maxx < INT_MIN || maxx > INT_MAX || maxy < INT_MIN || maxy > INT_MAX ||
           sel.o.z < 0 || maxz > (1 << 13))
        {
            error = "edit selection is outside the generated world or too large";
            return false;
        }
        if(ci.privilege < PRIV_ADMIN)
        {
            if(sel.grid != 16 || sel.s != ivec(1, 1, 1))
            {
                error = "normal players may only modify one gridsize 4 (16-unit) block";
                return false;
            }
            if(!ci.hasposition)
            {
                error = "send a valid position before editing";
                return false;
            }
            vec center(sel.o.x + 8.0f, sel.o.y + 8.0f, sel.o.z + 8.0f);
            if(center.dist(ci.o) > 160.0f)
            {
                error = "block is beyond creative reach";
                return false;
            }
        }
        return true;
    }

    static bool validateedit(clientinfo &ci, int type, packetbuf &p,
                             serveredit &edit, const char *&error)
    {
        int start = p.length();
        selinfo sel;
        if(!editselectiontype(type) || !readselection(p, sel) ||
           !validselection(ci, sel, error))
            return false;

        int arg1 = 0, arg2 = 0, arg3 = 0, extra = 0;
        switch(type)
        {
            case N_EDITF:
                arg1 = getint(p); arg2 = getint(p);
                if(arg1 < -1 || arg1 > 1 || arg2 < 0 || arg2 > 2)
                {
                    error = "invalid face edit";
                    return false;
                }
                break;
            case N_EDITT:
                arg1 = getint(p); arg2 = getint(p);
                if(p.remaining() < 2) { error = "truncated texture edit"; return false; }
                extra = lilswap(*(const ushort *)p.pad(2));
                if(extra > p.remaining()) { error = "truncated texture payload"; return false; }
                p.pad(extra);
                if(arg1 < 0 || arg1 > 0xFFFF || arg2 < 0 || arg2 > 1)
                {
                    error = "invalid texture edit";
                    return false;
                }
                break;
            case N_EDITM:
                arg1 = getint(p); arg2 = getint(p);
                break;
            case N_FLIP:
            case N_DELCUBE:
                break;
            case N_ROTATE:
                arg1 = getint(p);
                if(arg1 < -3 || arg1 > 3 || !arg1)
                {
                    error = "invalid rotation";
                    return false;
                }
                break;
            case N_REPLACE:
                arg1 = getint(p); arg2 = getint(p); arg3 = getint(p);
                if(p.remaining() < 2) { error = "truncated replace edit"; return false; }
                extra = lilswap(*(const ushort *)p.pad(2));
                if(extra > p.remaining()) { error = "truncated replace payload"; return false; }
                p.pad(extra);
                if(arg1 < 0 || arg2 < 0 || arg3 != 1)
                {
                    error = "invalid replace edit";
                    return false;
                }
                break;
            case N_EDITVSLOT:
                arg1 = getint(p); arg2 = getint(p);
                if(p.remaining() < 2) { error = "truncated vslot edit"; return false; }
                extra = lilswap(*(const ushort *)p.pad(2));
                if(extra > p.remaining()) { error = "truncated vslot payload"; return false; }
                p.pad(extra);
                break;
            case N_EDITSCATTER:
                arg1 = getint(p);
                arg2 = getint(p);
                if(arg1 < 0 || arg1 > 255 || (arg2 != 0 && arg2 != 1) ||
                   sel.grid != 16 || sel.s != ivec(1, 1, 1) ||
                   sel.orient == WORLD_ORIENT_BOTTOM)
                {
                    error = "invalid scatter edit";
                    return false;
                }
                break;
            default:
                error = "unsupported world edit";
                return false;
        }
        if(p.overread() || p.remaining())
        {
            error = "malformed edit packet";
            return false;
        }
        if(ci.privilege < PRIV_ADMIN)
        {
            bool allowedface = type == N_EDITF && arg1 == -1 && arg2 == 1,
                 allowedtexture = type == N_EDITT && arg1 <= 0xFFF &&
                                  arg2 == 1 && extra == 0,
                 alloweddelete = type == N_DELCUBE,
                 allowedscatter = type == N_EDITSCATTER;
            if(!allowedface && !allowedtexture && !alloweddelete &&
               !allowedscatter)
            {
                error = "this edit operation requires admin";
                return false;
            }
        }

        edit.author = ci.clientnum;
        edit.type = type;
        edit.selection = sel;
        edit.hasselection = true;
        edit.payload.put(&p.buf[start], p.length() - start);
        return true;
    }

    static void acceptededit(serveredit *edit)
    {
        edit->revision = ++worldeditrevision;
        edit->timestamp = uint(time(NULL));
        if(!appendserveredit(*edit))
        {
            --worldeditrevision;
            clientinfo *ci = getinfo(edit->author);
            if(ci) sendf(ci->clientnum, 1, "ris", N_SERVMSG,
                         "world edit rejected: server could not persist it");
            delete edit;
            return;
        }
        worldhistory.add(edit);
        worldredostack.deletecontents();
        loopv(clients)
        {
            clientinfo *recipient = clients[i];
            if(recipient && recipient->connected && recipient->worldready)
                sendserveredit(recipient->clientnum, *edit);
        }
    }

    static serveredit *cloneserveredit(const serveredit &source)
    {
        serveredit *edit = new serveredit;
        edit->revision = source.revision;
        edit->timestamp = source.timestamp;
        edit->author = source.author;
        edit->type = source.type;
        edit->active = source.active;
        edit->hasselection = source.hasselection;
        edit->selection = source.selection;
        edit->payload.put(source.payload.getbuf(), source.payload.length());
        return edit;
    }

    static bool sameserveredit(const serveredit &a, const serveredit &b)
    {
        return a.type == b.type && a.payload.length() == b.payload.length() &&
               (!a.payload.length() ||
                !memcmp(a.payload.getbuf(), b.payload.getbuf(), a.payload.length()));
    }

    static void sendcommandresult(clientinfo &ci, const char *message)
    {
        sendf(ci.clientnum, 1, "ris", N_SERVMSG, message);
    }

    static bool editinarea(const serveredit &edit, const ivec &minimum, const ivec &maximum)
    {
        if(!edit.hasselection) return false;
        ivec end = ivec(edit.selection.s).mul(edit.selection.grid).add(edit.selection.o);
        return edit.selection.o.x < maximum.x && end.x > minimum.x &&
               edit.selection.o.y < maximum.y && end.y > minimum.y &&
               edit.selection.o.z < maximum.z && end.z > minimum.z;
    }

    static void serverworldcommand(clientinfo &ci, const char *request)
    {
        if(ci.privilege < PRIV_ADMIN)
        {
            sendcommandresult(ci, "permission denied: this world command requires admin");
            return;
        }

        string command;
        // Command arguments are separated by whitespace. Preserve and
        // normalize it while stripping other non-printing characters.
        filtertext(command, request ? request : "", true, true, sizeof(command));
        char *args = command;
        while(*args && !iscubespace(*args)) ++args;
        if(*args) *args++ = '\0';
        while(iscubespace(*args)) ++args;

        if(cubecaseequal(command, "time"))
        {
            if(cubecaseequal(args, "freeze")) worldtimefrozen = true;
            else
            {
                char *end = NULL;
                double hour = strtod(args, &end);
                while(end && iscubespace(*end)) ++end;
                if(end == args || (end && *end) || hour < 0 || hour > 24)
                {
                    sendcommandresult(ci, "usage: /time <hour 0-24|freeze>");
                    return;
                }
                if(hour == 24) hour = 0;
                worldclockmillis = int(hour * SERVER_DAY_MILLIS / 24.0);
                worldtimefrozen = false;
            }
            sendworldtime();
            sendcommandresult(ci, "authoritative world time updated");
            return;
        }

        if(cubecaseequal(command, "worldundo"))
        {
            int requested = args[0] ? clamp(atoi(args), 1, 1000) : 1, applied = 0;
            for(int i = worldhistory.length() - 1; i >= 0 && applied < requested; --i)
            {
                serveredit *edit = worldhistory[i];
                if(!edit->active || !editselectiontype(edit->type)) continue;
                edit->active = false;
                serveredit *redo = cloneserveredit(*edit);
                redo->active = true;
                worldredostack.add(redo);
                ++worldeditrevision;
                ++applied;
            }
            if(applied)
            {
                rewriteserverjournal();
                resetallclients();
            }
            defformatstring(message, "worldundo: %d authoritative change%s reverted",
                            applied, applied == 1 ? "" : "s");
            sendcommandresult(ci, message);
            return;
        }

        if(cubecaseequal(command, "worldredo"))
        {
            int requested = args[0] ? clamp(atoi(args), 1, 1000) : 1, applied = 0;
            while(applied < requested)
            {
                serveredit *edit = NULL;
                if(!worldredostack.empty()) edit = worldredostack.pop();
                else for(int i = worldhistory.length() - 1; i >= 0; --i)
                    if(!worldhistory[i]->active && editselectiontype(worldhistory[i]->type))
                    {
                        bool alreadyactive = false;
                        for(int j = i + 1; j < worldhistory.length(); ++j)
                            if(worldhistory[j]->active &&
                               sameserveredit(*worldhistory[i], *worldhistory[j]))
                            {
                                alreadyactive = true;
                                break;
                            }
                        if(alreadyactive) continue;
                        edit = cloneserveredit(*worldhistory[i]);
                        break;
                    }
                if(!edit) break;
                edit->revision = ++worldeditrevision;
                edit->timestamp = uint(time(NULL));
                edit->author = ci.clientnum;
                edit->active = true;
                worldhistory.add(edit);
                ++applied;
            }
            if(applied)
            {
                rewriteserverjournal();
                resetallclients();
            }
            defformatstring(message, "worldredo: %d authoritative change%s restored",
                            applied, applied == 1 ? "" : "s");
            sendcommandresult(ci, message);
            return;
        }

        if(cubecaseequal(command, "worldlog"))
        {
            int shown = 0;
            for(int i = worldhistory.length() - 1; i >= 0 && shown < 20; --i)
            {
                const serveredit &edit = *worldhistory[i];
                if(!editselectiontype(edit.type)) continue;
                defformatstring(message, "rev %u author %d op %d at %d %d %d%s",
                                edit.revision, edit.author, edit.type,
                                edit.hasselection ? edit.selection.o.x : 0,
                                edit.hasselection ? edit.selection.o.y : 0,
                                edit.hasselection ? edit.selection.o.z : 0,
                                edit.active ? "" : " (undone)");
                sendcommandresult(ci, message);
                ++shown;
            }
            if(!shown) sendcommandresult(ci, "worldlog: no authoritative edits");
            return;
        }

        if(cubecaseequal(command, "worldrevert"))
        {
            int applied = 0;
            if(!strncmp(args, "player ", 7))
            {
                int author = atoi(args + 7);
                loopv(worldhistory)
                {
                    serveredit &edit = *worldhistory[i];
                    if(edit.active && edit.author == author && editselectiontype(edit.type))
                    {
                        edit.active = false;
                        ++worldeditrevision;
                        ++applied;
                    }
                }
            }
            else if(!strncmp(args, "area ", 5))
            {
                int x1, y1, z1, x2, y2, z2;
                if(sscanf(args + 5, "%d %d %d %d %d %d", &x1, &y1, &z1, &x2, &y2, &z2) != 6)
                {
                    sendcommandresult(ci, "usage: /worldrevert player <id> | area <x1 y1 z1> <x2 y2 z2>");
                    return;
                }
                ivec minimum(min(x1, x2), min(y1, y2), min(z1, z2)),
                     maximum(max(x1, x2) + 1, max(y1, y2) + 1, max(z1, z2) + 1);
                loopv(worldhistory)
                {
                    serveredit &edit = *worldhistory[i];
                    if(edit.active && editinarea(edit, minimum, maximum))
                    {
                        edit.active = false;
                        ++worldeditrevision;
                        ++applied;
                    }
                }
            }
            else
            {
                sendcommandresult(ci, "usage: /worldrevert player <id> | area <x1 y1 z1> <x2 y2 z2>");
                return;
            }
            if(applied)
            {
                worldredostack.deletecontents();
                rewriteserverjournal();
                resetallclients();
            }
            defformatstring(message, "worldrevert: %d authoritative change%s reverted",
                            applied, applied == 1 ? "" : "s");
            sendcommandresult(ci, message);
            return;
        }

        if(cubecaseequal(command, "worldrestore"))
        {
            int x, y, z;
            uint revision;
            if(sscanf(args, "chunk %d %d %d %u", &x, &y, &z, &revision) != 4)
            {
                sendcommandresult(ci, "usage: /worldrestore chunk <x y z> <revision>");
                return;
            }
            int applied = 0;
            ivec minimum(x * 1024, y * 1024, 0), maximum((x + 1) * 1024, (y + 1) * 1024, 8192);
            loopv(worldhistory)
            {
                serveredit &edit = *worldhistory[i];
                if(edit.active && edit.revision > revision && editinarea(edit, minimum, maximum))
                {
                    edit.active = false;
                    ++worldeditrevision;
                    ++applied;
                }
            }
            if(applied) { rewriteserverjournal(); resetallclients(); }
            defformatstring(message, "worldrestore: %d change%s reverted in chunk %d %d",
                            applied, applied == 1 ? "" : "s", x, y);
            sendcommandresult(ci, message);
            return;
        }

        if(cubecaseequal(command, "worlddiff"))
        {
            int active = 0;
            loopv(worldhistory) if(worldhistory[i]->active) ++active;
            if(!strncmp(args, "compact", 7)) rewriteserverjournal();
            if(strncmp(args, "stats", 5) && strncmp(args, "compact", 7) &&
               strncmp(args, "verify", 6))
            {
                sendcommandresult(ci, "usage: /worlddiff <stats|compact|verify>");
                return;
            }
            defformatstring(message, "worlddiff: %d active, %d audit records, revision %u, seed %d",
                            active, worldhistory.length(), worldeditrevision, serverworldseed);
            sendcommandresult(ci, message);
            return;
        }

        sendcommandresult(ci, "unknown authoritative world command");
    }

    void parsepacket(int sender, int chan, packetbuf &p)
    {
        if(chan == 0)
        {
            clientinfo *ci = getinfo(sender);
            while(ci && ci->connected && ci->worldready && p.remaining())
            {
                int packetstart = p.length();
                int type = getint(p);
                if(type != N_POS)
                {
                    p.pad(p.remaining());
                    break;
                }

                int cn = getuint(p);
                int coords[3];
                loopk(3) coords[k] = getint(p);
                int physstate = p.get();
                uint flags = getuint(p);
                p.get();
                p.get();
                p.get();
                p.get();
                if(flags&(1<<3)) p.get();
                p.get();
                p.get();
                if(flags&(1<<4))
                {
                    p.get();
                    if(flags&(1<<5)) p.get();
                    if(flags&(1<<6))
                    {
                        p.get();
                        p.get();
                    }
                }
                if(p.overread()) return;
                if(cn != sender || (physstate&7) > PHYS_BOUNCE) continue;

                vec nextposition(coords[0]/DMF, coords[1]/DMF, coords[2]/DMF);
                int now = max(totalmillis, 1),
                    elapsed = ci->hasposition ? max(now - ci->lastpositionmillis, 1) : 0;
                if(nextposition.z < 0 || nextposition.z > (1 << 13) ||
                   (ci->hasposition && nextposition.dist(ci->o) > 32.0f + elapsed * 0.5f))
                    continue;

                ci->position.setsize(0);
                ci->position.put(&p.buf[packetstart], p.length() - packetstart);
                ci->o = nextposition;
                ci->hasposition = true;
                ci->lastpositionmillis = now;
            }
            return;
        }
        if(chan != 1) return;
        while(p.remaining())
        {
            int type = getint(p);
            switch(type)
            {
                case N_CONNECT:
                {
                    clientinfo *ci = getinfo(sender);
                    string pass;
                    getstring(pass, p, sizeof(pass));
                    if(!ci || ci->connected) break;
                    if(!serverworldready)
                    {
                        disconnect_client(sender, DISC_PRIVATE);
                        return;
                    }
                    if(serverpass[0] && strcmp(pass, serverpass))
                    {
                        disconnect_client(sender, DISC_PASSWORD);
                        return;
                    }
                    ci->connected = true;
                    sendf(sender, 1, "ri", N_WELCOME);
                    loopv(clients)
                    {
                        clientinfo *other = clients[i];
                        if(other && other->connected)
                            sendprivilege(sender, other->clientnum, other->privilege);
                    }
                    if(servermotd[0]) sendcommandresult(*ci, servermotd);
                    sendworldstate(*ci, false);
                    break;
                }
                case N_INITCLIENT:
                {
                    clientinfo *ci = getinfo(sender);
                    string name;
                    getstring(name, p, sizeof(name));
                    if(!ci || !ci->connected) break;

                    bool firstinit = !ci->name[0];
                    filtertext(ci->name, name, false, false, MAXSTRLEN);
                    if(!ci->name[0]) formatstring(ci->name, "player%d", sender);
                    if(firstinit) loopv(clients)
                    {
                        clientinfo *other = clients[i];
                        if(i != sender && other && other->connected && other->name[0])
                            sendf(sender, 1, "ri2s", N_INITCLIENT, i, other->name);
                    }
                    sendf(-1, 1, "ri2s", N_INITCLIENT, sender, ci->name);
                    sendprivilege(-1, sender, ci->privilege);
                    break;
                }
                case N_TEXT:
                {
                    clientinfo *ci = getinfo(sender);
                    string text;
                    getstring(text, p, sizeof(text));
                    if(ci && ci->connected)
                    {
                        defformatstring(message, "%s: %s", ci->name[0] ? ci->name : "player", text);
                        sendf(-1, 1, "ris", N_SERVMSG, message);
                    }
                    break;
                }
                case N_EDITENT:
                case N_EDITF: case N_EDITT: case N_EDITM: case N_FLIP: case N_COPY: case N_PASTE: case N_ROTATE: case N_REPLACE: case N_DELCUBE: case N_CALCLIGHT: case N_REMIP: case N_EDITVSLOT: case N_EDITSCATTER: case N_UNDO: case N_REDO: case N_EDITVAR:
                {
                    clientinfo *ci = getinfo(sender);
                    if(!ci || !ci->connected || !ci->worldready)
                    {
                        p.pad(p.remaining());
                        break;
                    }
                    serveredit *edit = new serveredit;
                    const char *error = NULL;
                    if(!validateedit(*ci, type, p, *edit, error))
                    {
                        delete edit;
                        sendcommandresult(*ci, error ? error : "world edit rejected");
                        p.pad(p.remaining());
                        break;
                    }
                    acceptededit(edit);
                    break;
                }
                case N_NEWMAP:
                {
                    clientinfo *ci = getinfo(sender);
                    getint(p);
                    if(ci) sendcommandresult(*ci, "newmap is disabled: the server seed owns the base world");
                    break;
                }
                case N_WORLDREADY:
                {
                    clientinfo *ci = getinfo(sender);
                    getint(p);
                    if(ci && ci->connected) replayworld(*ci);
                    break;
                }
                case N_SETMASTER:
                {
                    clientinfo *ci = getinfo(sender);
                    string password;
                    getstring(password, p, sizeof(password));
                    if(!ci || !ci->connected) break;
                    if(!strcmp(password, "0") && !ci->local)
                    {
                        ci->privilege = PRIV_NONE;
                        sendprivilege(-1, sender, ci->privilege);
                        sendcommandresult(*ci, "admin privilege relinquished");
                    }
                    else if(ci->local || (adminpass[0] && !strcmp(password, adminpass)))
                    {
                        ci->privilege = PRIV_ADMIN;
                        sendprivilege(-1, sender, ci->privilege);
                        sendcommandresult(*ci, "admin privilege granted");
                    }
                    else sendcommandresult(*ci, "admin authentication failed");
                    break;
                }
                case N_SERVERCOMMAND:
                {
                    clientinfo *ci = getinfo(sender);
                    string command;
                    getstring(command, p, sizeof(command));
                    if(ci && ci->connected) serverworldcommand(*ci, command);
                    break;
                }
                case N_GETMAP:
                    if(mapdata) sendfile(sender, 2, mapdata, "i", N_SENDMAP);
                    break;
                case N_EDITMODE:
                {
                    const bool enabled = getint(p) != 0;
                    clientinfo *ci = getinfo(sender);
                    if(enabled && ci && ci->connected && ci->privilege < PRIV_ADMIN)
                    {
                        sendcommandresult(*ci, "permission denied: full edit mode requires admin");
                        sendf(ci->clientnum, 1, "ri2", N_EDITMODE, 0);
                    }
                    break;
                }
                default:
                {
                    int size = msgsizelookup(type);
                    if(size > 0) loopi(size-1) getint(p);
                    p.pad(p.remaining());
                    break;
                }
            }
        }
    }

    void sendservmsg(const char *s) { sendf(-1, 1, "ris", N_SERVMSG, s); }
    static enet_uint32 lastsend = 0;

    static bool sendpositionbatch(int cn, vector<uchar> &batch)
    {
        if(batch.empty()) return false;
        packetbuf p(batch.length());
        p.put(batch.getbuf(), batch.length());
        sendpacket(cn, 0, p.finalize());
        batch.setsize(0);
        return true;
    }

    bool sendpackets(bool force)
    {
        enet_uint32 curtime = enet_time_get() - lastsend;
        if(curtime < 33 && !force) return false;
        lastsend += curtime - (curtime%33);

        bool sent = false;
        int mtu = getservermtu() - 100;
        if(mtu <= 0) mtu = MAXTRANS;
        loopv(clients)
        {
            clientinfo *recipient = clients[i];
            if(!recipient || !recipient->connected || !recipient->worldready) continue;

            vector<uchar> batch;
            loopvj(clients)
            {
                clientinfo *source = clients[j];
                if(!source || !source->connected || !source->worldready ||
                   source == recipient || source->position.empty()) continue;
                if(!batch.empty() && batch.length() + source->position.length() > mtu)
                    sent |= sendpositionbatch(recipient->clientnum, batch);
                batch.put(source->position.getbuf(), source->position.length());
            }
            sent |= sendpositionbatch(recipient->clientnum, batch);
        }
        loopv(clients) if(clients[i]) clients[i]->position.setsize(0);
        return sent;
    }
    void serverinforeply(ucharbuf &req, ucharbuf &p)
    {
        putint(p, PROTOCOL_VERSION);
        int players = 0;
        loopv(clients) if(clients[i] && clients[i]->connected) ++players;
        putint(p, players);
        putint(p, maxclients);
        putint(p, 3);
        putint(p, gamemode);
        putint(p, 0);
        putint(p, MM_OPEN);
        sendstring(smapname, p);
        sendstring(serverdesc, p);
        sendserverinforeply(p);
    }
    void serverupdate()
    {
        if(!journalinitialized) return;
        if(!worldtimefrozen && curtime > 0)
        {
            worldclockmillis += curtime;
            while(worldclockmillis >= SERVER_DAY_MILLIS) worldclockmillis -= SERVER_DAY_MILLIS;
        }
        if(totalmillis - lastworldtimesync >= 5000)
        {
            lastworldtimesync = totalmillis;
            sendworldtime();
        }
    }
    int protocolversion() { return PROTOCOL_VERSION; }
    int laninfoport() { return TESSERACT_LANINFO_PORT; }
    int serverport() { return TESSERACT_SERVER_PORT; }
    const char *defaultmaster() { return ""; }
    int masterport() { return TESSERACT_MASTER_PORT; }
    void processmasterinput(const char *cmd, int cmdlen, const char *args) {}
    void masterconnected() {}
    void masterdisconnected() {}
    bool ispaused() { return false; }
    int scaletime(int t) { return t*100; }

    const char *modename(int n, const char *unknown) { return m_valid(n) ? gamemodes[n - STARTGAMEMODE].name : unknown; }
    const char *modeprettyname(int n, const char *unknown) { return m_valid(n) ? gamemodes[n - STARTGAMEMODE].prettyname : unknown; }
    const char *mastermodename(int n, const char *unknown) { return n >= 0 && n < 3 ? mastermodes[n] : unknown; }
    void startintermission() {}
    void stopdemo() {}
    void timeupdate(int secs) {}
    const char *getdemofile(const char *file, bool init) { return NULL; }
    void forcemap(const char *map, int mode) {}
    void forcepaused(bool paused) {}
    void forcegamespeed(int speed) {}
    void hashpassword(int cn, int sessionid, const char *pwd, char *result, int maxlen) { hashstring(pwd, result, maxlen); }
    int msgsizelookup(int msg)
    {
        for(const int *p = msgsizes; *p >= 0; p += 2) if(p[0] == msg) return p[1];
        return -1;
    }
    bool serveroption(const char *arg) { return false; }
    bool delayspawn(int type) { return false; }
}

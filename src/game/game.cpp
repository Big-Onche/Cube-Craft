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
    int mastermode = MM_OPEN;
    gameent *player1 = NULL;
    vector<gameent *> players, clients;
    vector<uchar> messages;
#ifndef STANDALONE
    static uint nextworldrequestid = 1;

    struct predictedworldaction
    {
        uint requestid;
        int action, orient, item;
        ivec target;

        predictedworldaction() : requestid(0), action(-1), orient(0), item(-1), target(0, 0, 0) {}
    };

    static vector<predictedworldaction *> predictedworldactions;
#endif

    float horizontalmeterspersecond(const physent *d)
    {
        if(!d) return 0.0f;
        float movescale = d->inwater && d->state != CS_EDITING && d->state != CS_SPECTATOR ? 0.5f : 1.0f;
        float x = d->vel.x * movescale + d->falling.x,
              y = d->vel.y * movescale + d->falling.y;
        return sqrtf(x*x + y*y) / GAMEUNITSPERMETER;
    }

    static string connectpass = "";
    static int lastpositionsend = -1000;
    static string sentname = "";
#ifndef STANDALONE
    static void updatesurvivalbreaking();
    static void cancelclientbreakrequest(uint requestid);
#endif

    static void putsel(packetbuf &p, const selinfo &sel)
    {
        putint(p, sel.o.x); putint(p, sel.o.y); putint(p, sel.o.z);
        putint(p, sel.s.x); putint(p, sel.s.y); putint(p, sel.s.z);
        putint(p, sel.grid); putint(p, sel.orient);
        putint(p, sel.cx); putint(p, sel.cxs); putint(p, sel.cy); putint(p, sel.cys);
        putint(p, sel.corner);
    }

#ifndef STANDALONE
    static uint newworldrequestid()
    {
        if(!nextworldrequestid) ++nextworldrequestid;
        return nextworldrequestid++;
    }

    static predictedworldaction *findpredictedworldaction(uint requestid)
    {
        loopv(predictedworldactions) if(predictedworldactions[i]->requestid == requestid) return predictedworldactions[i];
        return NULL;
    }

    static void worldactionselection(selinfo &sel, const ivec &origin, int orient)
    {
        sel.o = origin;
        sel.s = ivec(1, 1, 1);
        sel.grid = 16;
        sel.orient = orient;
        sel.cx = sel.cy = sel.corner = 0;
        sel.cxs = sel.cys = 2;
    }

    static ivec worldactionplacecell(const ivec &support, int orient)
    {
        ivec target = support;
        const int dimension = orient >> 1;
        target[dimension] += orient&1 ? 16 : -16;
        return target;
    }

    static bool applyworldaction(int action, const ivec &absolutetarget, int orient, int item)
    {
        selinfo sel;
        worldactionselection(sel, absolutetarget, orient);
        worldselectiontolocal(sel);
        if(!sel.validate() || !worldselectionready(sel)) return false;
        const ivec target = sel.o;
        switch(action)
        {
            case WORLD_ACTION_PLACE_CUBE:
            {
                if(item < 0 || item >= numworldcubes()) return false;
                const ivec placedorigin = worldactionplacecell(target, orient);
                selinfo placed;
                worldactionselection(placed, placedorigin, orient);
                mpeditface(-1, 1, sel, false);
                mpedittex(getworldcubeslot(item), 1, placed, false);
                return true;
            }
            case WORLD_ACTION_PLACE_SCATTER:
                return item >= numworldcubes() &&
                       editworldscatter(item - numworldcubes(), target, orient, true);
            case WORLD_ACTION_BREAK_CUBE_START:
                mpdelcube(sel, false);
                return true;
            case WORLD_ACTION_BREAK_SCATTER_START:
                return item >= numworldcubes() &&
                       editworldscatter(item - numworldcubes(), target, orient, false);
            default:
                return false;
        }
    }

    static void rollbackworldaction(const predictedworldaction &prediction)
    {
        if(prediction.action == WORLD_ACTION_PLACE_CUBE)
        {
            ivec target = worldactionplacecell(prediction.target, prediction.orient);
            selinfo sel;
            worldactionselection(sel, target, prediction.orient);
            worldselectiontolocal(sel);
            mpdelcube(sel, false);
        }
        else if(prediction.action == WORLD_ACTION_PLACE_SCATTER)
        {
            selinfo sel;
            worldactionselection(sel, prediction.target, prediction.orient);
            worldselectiontolocal(sel);
            editworldscatter(prediction.item - numworldcubes(), sel.o, prediction.orient, false);
        }
        else if(prediction.action == WORLD_ACTION_BREAK_CUBE_START)
        {
            ivec target = prediction.target,
                 support = target;
            const int dimension = prediction.orient >> 1;
            support[dimension] += prediction.orient&1 ? -16 : 16;
            applyworldaction(WORLD_ACTION_PLACE_CUBE, support, prediction.orient, prediction.item);
        }
        else if(prediction.action == WORLD_ACTION_BREAK_SCATTER_START)
            applyworldaction(WORLD_ACTION_PLACE_SCATTER, prediction.target, prediction.orient, prediction.item);
    }
#endif

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
    static void clearclients()
    {
        loopv(clients) if(clients[i]) delete clients[i];
        clients.setsize(0);
        players.setsize(0);
        if(player1) players.add(player1);
        cleardynentcache();
    }

    void gamedisconnect(bool cleanup)
    {
        connected = remote = false;
        predictedworldactions.deletecontents();
        nextworldrequestid = 1;
        resetsurvivalinventory();
        receiveserversettings(5000, 250);
#ifndef STANDALONE
        resetclientreceive();
#endif
        localworldactive = false;
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
        resetclientreceive();
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
        if(edit.type == N_WORLDAUTH)
        {
            if(player1 && edit.author == player1->clientnum && edit.requestid && findpredictedworldaction(edit.requestid)) return true;
            setworldeditauthor(edit.author);
            setworldeditrevision(edit.revision);
            return applyworldaction(edit.args[0], ivec(edit.args[1], edit.args[2], edit.args[3]), edit.args[4], edit.args[5]);
        }

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

    void processnetworkedits()
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
        CREATIVE_ARM_RELEASE = 120,
        SURVIVAL_BREAK_STAGES = 8,
        SURVIVAL_BREAK_PARTICLE_MILLIS = 125,
        SURVIVAL_STACK_SIZE = 64
    };

    static const float CREATIVE_ARM_PITCH = 70.0f;
    static int authoritativebreakmillis = 5000, authoritativescatterbreakmillis = 250;

    static void sendworldaction(uint requestid, int action, const ivec &localtarget, int orient, int item, int slot)
    {
        selinfo selection;
        worldactionselection(selection, localtarget, orient);
        if(waitforserveredit()) worldselectiontoabsolute(selection);
        addmsg(N_WORLDACTION, "ri8", int(requestid), action, selection.o.x, selection.o.y, selection.o.z, orient, item, slot);
    }

    static void addpredictedworldaction(uint requestid, int action, const ivec &absolutetarget, int orient, int item)
    {
        predictedworldaction *prediction = new predictedworldaction;
        prediction->requestid = requestid;
        prediction->action = action;
        prediction->target = absolutetarget;
        prediction->orient = orient;
        prediction->item = item;
        predictedworldactions.add(prediction);
    }

    static uint predictworldaction(int action, const ivec &localtarget, int orient, int item, int slot)
    {
        const uint requestid = newworldrequestid();
        selinfo selection;
        worldactionselection(selection, localtarget, orient);
        if(waitforserveredit()) worldselectiontoabsolute(selection);
        addpredictedworldaction(requestid, action, selection.o, orient, item);
        sendworldaction(requestid, action, localtarget, orient, item, slot);
        return requestid;
    }

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

    void receiveinventory(const int *items, const int *counts, int slots, int selected)
    {
        loadsurvivalinventory(items, counts, slots);
        creativehotbarslot = clamp(selected, 0, CREATIVE_HOTBAR_SLOTS - 1);
    }

    void receiveserversettings(int breakmillis, int scatterbreakmillis)
    {
        authoritativebreakmillis = clamp(breakmillis, 100, 60000);
        authoritativescatterbreakmillis = clamp(scatterbreakmillis, 50, 60000);
    }

    void receiveactionresult(uint requestid, int result, const char *reason)
    {
#ifndef STANDALONE
        if(result != ACTION_RESULT_ACCEPTED) cancelclientbreakrequest(requestid);
#endif
        loopv(predictedworldactions)
        {
            predictedworldaction *prediction = predictedworldactions[i];
            if(prediction->requestid != requestid) continue;
            if(result == ACTION_RESULT_REJECTED) rollbackworldaction(*prediction);
            delete prediction;
            predictedworldactions.remove(i);
            break;
        }
        if(result != ACTION_RESULT_ACCEPTED && reason && reason[0]) conoutf(CON_WARN, "server action rejected: %s", reason);
    }

#ifndef STANDALONE
    static void emitnetworkblockchips(const selinfo &sel, int orient, int num)
    {
        if(num <= 0 || orient < 0 || orient > 5) return;
        const int dimension = orient>>1;
        vec normal(0, 0, 0), hitpoint = vec(sel.o).add(sel.grid*0.5f);
        normal[dimension] = orient&1 ? 1 : -1;
        hitpoint[dimension] = sel.o[dimension] + (orient&1 ? sel.grid : 0);
        const ivec position = ivec(sel.o).add(sel.grid / 2);
        particle_blockchips(getworldcubetextureslotat(position, orient), hitpoint, normal, num);
    }
#endif

    void receivebreakstate(int actor, uint requestid, int phase, int action, const ivec &absolutetarget, int orient, int stage)
    {
#ifndef STANDALONE
        if(player1 && actor == player1->clientnum && (phase == BREAK_STATE_CANCEL || phase == BREAK_STATE_COMPLETE)) cancelclientbreakrequest(requestid);
        gameent *d = clients.inrange(actor) ? clients[actor] : NULL;
        if(d && d != player1)
        {
            const bool active = phase == BREAK_STATE_START || phase == BREAK_STATE_UPDATE;
            if(active && !d->renderattacking)
            {
                d->renderattacking = true;
                d->renderattackmillis = lastmillis;
            }
            else if(!active && d->renderattacking)
            {
                d->renderattacking = false;
                d->renderattackreleasemillis = lastmillis;
            }
        }
        if(action == WORLD_ACTION_BREAK_CUBE_START)
        {
            if(phase == BREAK_STATE_START || phase == BREAK_STATE_UPDATE)
            {
                selinfo sel;
                worldactionselection(sel, absolutetarget, orient);
                worldselectiontolocal(sel);
                setbreakstain(actor, requestid, sel.o, sel.grid, clamp(stage, 0, SURVIVAL_BREAK_STAGES - 1));
                if(!player1 || actor != player1->clientnum) emitnetworkblockchips(sel, orient, phase == BREAK_STATE_START ? 2 : 3);
            }
            else clearbreakstain(actor, requestid);
        }
#else
        (void)actor; (void)requestid; (void)phase; (void)action; (void)absolutetarget; (void)orient; (void)stage;
#endif
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

    float creativearmwave(int elapsed)
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

    static bool creativehit(selinfo &hit, vec *hitpoint = NULL)
    {
        if(!buildenabled()) return false;

        const vec origin = camera1 ? camera1->o : player1->o;
        vec hitpos;
        float dist = raycubepos(origin, camdir, hitpos, CREATIVE_REACH,
                                RAY_CLIPMAT | RAY_SKIPFIRST, CREATIVE_GRID);
        if(dist >= CREATIVE_REACH) return false;
        if(hitpoint) *hitpoint = hitpos;

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
        vec center, radius, hitpoint;

        creativetarget() : type(CREATIVE_TARGET_NONE), entity(-1), center(0, 0, 0), radius(0, 0, 0), hitpoint(0, 0, 0) {}
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

        if(!creativehit(target.cube, &target.hitpoint)) return false;
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
            if(!waitforserveredit())
            {
                scatteredittrigger(type, hit.o, hit.orient, true);
                if(m_survival) consumesurvivalitem();
                player1->renderplacemillis = lastmillis;
                player1->renderplacetoggle = !player1->renderplacetoggle;
                return;
            }
            if(!editworldscatter(type, hit.o, hit.orient, true)) return;
            predictworldaction(WORLD_ACTION_PLACE_SCATTER, hit.o, hit.orient, selected, clampcreativehotbarslot());
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
        if(!waitforserveredit())
        {
            mpeditface(-1, 1, hit, true);
            mpedittex(getworldcubeslot(selected), 1, placed, true);
        }
        else
        {
            // mpeditface advances hit.o to the placed cell, while the protocol carries the support cell.
            const ivec support = hit.o;
            mpeditface(-1, 1, hit, false);
            mpedittex(getworldcubeslot(selected), 1, placed, false);
            predictworldaction(WORLD_ACTION_PLACE_CUBE, support, hit.orient, selected, clampcreativehotbarslot());
        }
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
            {
                if(!waitforserveredit()) scatteredittrigger(type, support, mountorient, false);
                else
                {
                    editworldscatter(type, support, mountorient, false);
                    predictworldaction(WORLD_ACTION_BREAK_SCATTER_START, support, mountorient, numworldcubes() + type, -1);
                    sendworldaction(predictedworldactions.last()->requestid, WORLD_ACTION_BREAK_COMPLETE,
                                    support, mountorient, numworldcubes() + type, -1);
                }
            }
            return;
        }
        if(!waitforserveredit()) mpdelcube(target.cube, true);
        else
        {
            const int item = getworldcubeindexat(ivec(target.cube.o).add(target.cube.grid / 2), target.cube.orient);
            mpdelcube(target.cube, false);
            predictworldaction(WORLD_ACTION_BREAK_CUBE_START, target.cube.o, target.cube.orient, item, -1);
            sendworldaction(predictedworldactions.last()->requestid, WORLD_ACTION_BREAK_COMPLETE, target.cube.o, target.cube.orient, item, -1);
        }
    }

#ifndef STANDALONE
    static bool survivalbreakactive = false;
    static creativetarget survivalbreaktarget;
    static int survivalbreakstart = 0, survivalbreakparticlemillis = -1, survivalbreaklaststage = -1;
    static uint survivalbreakrequestid = 0;
    static int survivalblockitem(const creativetarget &target);

    static void cancelsurvivalbreak()
    {
        const uint requestid = survivalbreakrequestid;
        if(survivalbreakrequestid && waitforserveredit())
        {
            if(survivalbreaktarget.type == CREATIVE_TARGET_SCATTER)
            {
                int type, orient;
                ivec support;
                if(getworldscatterentityedit(survivalbreaktarget.entity, type, support, orient))
                    sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_CANCEL, support, orient, numworldcubes() + type, -1);
            }
            else
                sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_CANCEL, survivalbreaktarget.cube.o,
                                survivalbreaktarget.cube.orient, survivalblockitem(survivalbreaktarget), -1);
        }
        clearbreakstain(player1 ? player1->clientnum : -1, requestid);
        survivalbreakrequestid = 0;
        survivalbreaklaststage = -1;
    }

    static void cancelclientbreakrequest(uint requestid)
    {
        if(!requestid || requestid != survivalbreakrequestid) return;
        clearbreakstain(player1 ? player1->clientnum : -1, requestid);
        survivalbreakactive = false;
        survivalbreakrequestid = 0;
        survivalbreaklaststage = -1;
        survivalbreakparticlemillis = -1;
    }

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

    static void emitsurvivalblockchips(const creativetarget &target, int num)
    {
        if(target.type != CREATIVE_TARGET_CUBE) return;
        vec normal(0, 0, 0);
        normal[target.cube.orient>>1] = target.cube.orient&1 ? 1 : -1;
        const ivec position = ivec(target.cube.o).add(target.cube.grid / 2);
        particle_blockchips(getworldcubetextureslotat(position, target.cube.orient), target.hitpoint, normal, num);
    }

    static void updatesurvivalbreaking()
    {
        if(!survivalenabled() || !player1->renderattacking)
        {
            if(survivalbreakactive) cancelsurvivalbreak();
            survivalbreakactive = false;
            survivalbreakparticlemillis = -1;
            clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
            return;
        }

        creativetarget target;
        if(!findcreativetarget(target))
        {
            if(survivalbreakactive) cancelsurvivalbreak();
            survivalbreakactive = false;
            survivalbreakparticlemillis = -1;
            clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
            return;
        }
        if(!survivalbreakactive || !samesurvivaltarget(target, survivalbreaktarget))
        {
            if(survivalbreakactive) cancelsurvivalbreak();
            survivalbreaktarget = target;
            survivalbreakstart = lastmillis;
            survivalbreakactive = true;
            survivalbreaklaststage = 0;
            if(waitforserveredit())
            {
                survivalbreakrequestid = newworldrequestid();
                if(target.type == CREATIVE_TARGET_SCATTER)
                {
                    int type, mountorient;
                    ivec support;
                    if(getworldscatterentityedit(target.entity, type, support, mountorient))
                        sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_SCATTER_START, support, mountorient, numworldcubes() + type, -1);
                }
                else
                    sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_CUBE_START, target.cube.o, target.cube.orient,
                                    survivalblockitem(target), -1);
            }
            if(target.type == CREATIVE_TARGET_CUBE)
            {
                setbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid, target.cube.o, target.cube.grid, 0);
                emitsurvivalblockchips(target, 2);
                survivalbreakparticlemillis = lastmillis;
            }
            else
            {
                clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
                survivalbreakparticlemillis = -1;
            }
            return;
        }
        const int breakmillis = target.type == CREATIVE_TARGET_SCATTER
                              ? authoritativescatterbreakmillis
                              : authoritativebreakmillis;
        const int elapsed = max(lastmillis - survivalbreakstart, 0);
        if(target.type == CREATIVE_TARGET_CUBE)
        {
            const int stage = clamp(elapsed, 0, authoritativebreakmillis - 1) * SURVIVAL_BREAK_STAGES / authoritativebreakmillis;
            setbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid, target.cube.o, target.cube.grid, stage);
            if(waitforserveredit() && survivalbreakrequestid && stage != survivalbreaklaststage)
            {
                sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_UPDATE, survivalbreaktarget.cube.o,
                                survivalbreaktarget.cube.orient, survivalblockitem(survivalbreaktarget), stage);
                survivalbreaklaststage = stage;
            }
            if(survivalbreakparticlemillis < 0 || lastmillis - survivalbreakparticlemillis >= SURVIVAL_BREAK_PARTICLE_MILLIS)
            {
                const int num = survivalbreakparticlemillis < 0 ? 1 : min((lastmillis - survivalbreakparticlemillis) / SURVIVAL_BREAK_PARTICLE_MILLIS, 3);
                emitsurvivalblockchips(target, num);
                survivalbreakparticlemillis = lastmillis;
            }
        }
        else
        {
            clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
            survivalbreakparticlemillis = -1;
        }
        if(elapsed < breakmillis) return;

        int item = -1;
        bool broken = false;
        clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
        if(survivalbreaktarget.type == CREATIVE_TARGET_SCATTER)
        {
            int type, mountorient;
            ivec support;
            if(getworldscatterentityedit(survivalbreaktarget.entity, type, support, mountorient))
            {
                item = numworldcubes() + type;
                if(!waitforserveredit()) scatteredittrigger(type, support, mountorient, false);
                else
                {
                    editworldscatter(type, support, mountorient, false);
                    selinfo absolute;
                    worldactionselection(absolute, support, mountorient);
                    worldselectiontoabsolute(absolute);
                    addpredictedworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_SCATTER_START, absolute.o, mountorient, item);
                    sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_COMPLETE, support, mountorient, item, -1);
                }
                broken = true;
            }
        }
        else
        {
            item = survivalblockitem(survivalbreaktarget);
            emitsurvivalblockchips(target, 8);
            if(!waitforserveredit()) mpdelcube(survivalbreaktarget.cube, true);
            else
            {
                mpdelcube(survivalbreaktarget.cube, false);
                selinfo absolute = survivalbreaktarget.cube;
                worldselectiontoabsolute(absolute);
                addpredictedworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_CUBE_START, absolute.o,
                                        survivalbreaktarget.cube.orient, item);
                sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_COMPLETE, survivalbreaktarget.cube.o,
                                survivalbreaktarget.cube.orient, item, -1);
            }
            broken = true;
        }
        if(broken && !addsurvivalitem(item)) conoutf(CON_WARN, "inventory is full; the broken block was not collected");
        survivalbreakactive = false;
        survivalbreakparticlemillis = -1;
        survivalbreakrequestid = 0;
        survivalbreaklaststage = -1;
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
            if(survivalbreakactive) cancelsurvivalbreak();
            survivalbreakactive = false;
            survivalbreakparticlemillis = -1;
            clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
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
        if(m_survival && waitforserveredit())
            addmsg(N_INVENTORYACTION, "ri4", int(newworldrequestid()), INVENTORY_ACTION_SELECT, creativehotbarslot, 0);
    });
    ICOMMAND(creativehotbarselect, "i", (int *slot),
    {
        creativehotbarslot = clamp(*slot, 0, CREATIVE_HOTBAR_SLOTS - 1);
        if(m_survival && waitforserveredit())
            addmsg(N_INVENTORYACTION, "ri4", int(newworldrequestid()), INVENTORY_ACTION_SELECT, creativehotbarslot, 0);
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
            if(waitforserveredit())
                addmsg(N_INVENTORYACTION, "ri4", int(newworldrequestid()), INVENTORY_ACTION_SWAP, *from, *to);
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
            if(waitforserveredit())
            {
                conoutf(CON_ERROR, "the multiplayer server owns the game mode");
                intret(0);
            }
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

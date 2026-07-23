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
    int sessionid = 0, mastermode = MM_OPEN;
    gameent *player1 = NULL;
    vector<gameent *> players, clients;
    vector<uchar> messages;

    static string connectpass = "", servdesc = "";

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

    bool addmsg(int type, const char *fmt, ...)
    {
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
    }

    void parseoptions(vector<const char *> &args)
    {
        loopv(args) conoutf(CON_ERROR, "unknown command-line option: %s", args[i]);
    }

    const char *gameident() { return "HoverEngine"; }

#ifndef STANDALONE
    const char *gameconfig() { return "config/game.cfg"; }
    const char *savedconfig() { return "config/saved.cfg"; }
    const char *restoreconfig() { return "config/restore.cfg"; }
    const char *defaultconfig() { return "config/default.cfg"; }
    const char *autoexec() { return "config/autoexec.cfg"; }
    const char *savedservers() { return "config/servers.cfg"; }
    void loadconfigs() {}

    void initclient()
    {
        player1 = new gameent;
        copystring(player1->name, "camera");
        players.add(player1);
    }

    void resetgamestate() {}
    void gamedisconnect(bool cleanup) { connected = remote = false; }
    void connectattempt(const char *name, const char *password, const ENetAddress &address) { copystring(connectpass, password ? password : ""); }
    void connectfail() {}

    void gameconnect(bool _remote)
    {
        remote = _remote;
        if(remote) addmsg(N_CONNECT, "rs", connectpass);
        else connected = true;
    }

    bool allowedittoggle() { return true; }
    void edittoggled(bool on)
    {
        if(player1) player1->state = player1->editstate = on ? CS_EDITING : CS_ALIVE;
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
        gamemode = STARTGAMEMODE;
        if(name && name[0]) load_world(name);
        else emptymap(0, true, NULL);
    }

    void changemap(const char *name) { changemap(name, STARTGAMEMODE); }
    void forceedit(const char *name) { if(name && name[0]) copystring(clientmap, name); }
    bool ispaused() { return gamepaused; }
    int scaletime(int t) { return t*100; }
    bool allowmouselook() { return true; }

    void updateworld()
    {
        updateworldchunks();
        physicsframe();
        if(player1)
        {
            player1->state = editmode ? CS_EDITING : CS_ALIVE;
            moveplayer(player1, 10, true);
            updateworldchunks();
        }
        gets2c();
        c2sinfo();
    }

    void physicstrigger(physent *d, bool local, int floorlevel, int waterlevel, int material) {}
    void bounced(physent *d, const vec &surface) {}

    void edittrigger(const selinfo &sel, int op, int arg1, int arg2, int arg3, const VSlot *vs)
    {
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
                putsel(p, sel);
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
        if(!initing)
        {
            connected = true;
            mainmenu = 0;
            if(!editmode) toggleedit(true);
        }
#endif
        findplayerspawn(player1, -1, 0);
    }
    void preload() { entities::preloadentities(); }
    float abovegameplayhud(int w, int h) { return 1.0f; }
    void gameplayhud(int w, int h) {}
    bool canjump() { return true; }
    bool cancrouch() { return true; }
    bool allowmove(physent *d) { return true; }
    dynent *iterdynents(int i) { return i == 0 ? player1 : NULL; }
    int numdynents() { return player1 ? 1 : 0; }
    void rendergame() { entities::renderentities(); }
    void renderavatar() {}
    void renderplayerpreview(int model, int color, int team, int weap) {}
    int numanims() { return ANIM_GAMESPECIFIC; }
    void findanims(const char *pattern, vector<int> &anims) {}
    void writegamedata(vector<char> &extras) {}
    void readgamedata(vector<char> &extras) {}
    float clipconsole(float w, float h) { return 0; }
    const char *defaultcrosshair(int index) { return "media/interface/crosshair/default.png"; }
    int selectcrosshair(vec &col) { return 0; }
    void setupcamera() {}
    bool allowthirdperson(bool msg) { return false; }
    bool detachcamera() { return false; }
    bool collidecamera() { return false; }
    void adddynlights() {}
    void particletrack(physent *owner, vec &o, vec &d) {}
    void dynlighttrack(physent *owner, vec &o, vec &hud) {}
    int maxsoundradius(int n) { return 500; }
    bool needminimap() { return true; }

    void c2sinfo(bool force)
    {
        if(connected && player1) addmsg(N_CLIENTPING, "i", player1->ping);
    }

    void parsepacketclient(int chan, packetbuf &p)
    {
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
            case N_MAPCHANGE:
            {
                string name;
                getstring(name, p, sizeof(name));
                getint(p);
                getint(p);
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
            {
                selinfo sel;
                getsel(p, sel);
                if(!sel.validate()) break;
                switch(type)
                {
                    case N_EDITF:
                    {
                        int dir = getint(p), mode = getint(p);
                        mpeditface(dir, mode, sel, false);
                        break;
                    }
                    case N_EDITT:
                    {
                        int tex = getint(p), allfaces = getint(p);
                        if(p.remaining() < 2) return;
                        int extra = lilswap(*(const ushort *)p.pad(2));
                        if(p.remaining() < extra) return;
                        ucharbuf ebuf = p.subbuf(extra);
                        mpedittex(tex, allfaces, sel, ebuf);
                        break;
                    }
                    case N_EDITM:
                    {
                        int mat = getint(p), filter = getint(p);
                        mpeditmat(mat, filter, sel, false);
                        break;
                    }
                    case N_FLIP: mpflip(sel, false); break;
                    case N_COPY: if(player1) mpcopy(player1->edit, sel, false); break;
                    case N_PASTE: if(player1) mppaste(player1->edit, sel, false); break;
                    case N_ROTATE:
                    {
                        int dir = getint(p);
                        mprotate(dir, sel, false);
                        break;
                    }
                    case N_REPLACE:
                    {
                        int oldtex = getint(p), newtex = getint(p), insel = getint(p);
                        if(p.remaining() < 2) return;
                        int extra = lilswap(*(const ushort *)p.pad(2));
                        if(p.remaining() < extra) return;
                        ucharbuf ebuf = p.subbuf(extra);
                        mpreplacetex(oldtex, newtex, insel > 0, sel, ebuf);
                        break;
                    }
                    case N_DELCUBE: mpdelcube(sel, false); break;
                    case N_EDITVSLOT:
                    {
                        int delta = getint(p), allfaces = getint(p);
                        if(p.remaining() < 2) return;
                        int extra = lilswap(*(const ushort *)p.pad(2));
                        if(p.remaining() < extra) return;
                        ucharbuf ebuf = p.subbuf(extra);
                        mpeditvslot(delta, allfaces, sel, ebuf);
                        break;
                    }
                }
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
    ICOMMAND(ismaster, "i", (int *cn), intret(0));
    ICOMMAND(isadmin, "i", (int *cn), intret(0));
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
        if(*numargs > 0 && name[0]) changemap(name, STARTGAMEMODE);
        else if(clientmap[0]) changemap(clientmap, STARTGAMEMODE);
        else emptymap(0, true, NULL);
    });
    ICOMMAND(m_timed, "i", (int *mode), intret(0));
    ICOMMANDN(m_edit, _icmd_m_edit_cmd, "i", (int *mode), intret(m_valid(*mode) ? 1 : 0));
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
    ICOMMAND(allowthirdperson, "b", (int *msg), intret(0));
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
#endif
}

namespace server
{
    struct clientinfo
    {
        int clientnum;
        bool connected, local;
        string name;
        ENetPacket *getmap;

        clientinfo() : clientnum(-1), connected(false), local(false), getmap(NULL) { name[0] = '\0'; }
    };

    vector<clientinfo *> clients;
    string smapname = "";
    stream *mapdata = NULL;
    int gamemode = STARTGAMEMODE;

    clientinfo *getinfo(int n)
    {
        return clients.inrange(n) ? clients[n] : NULL;
    }

    void *newclientinfo() { return new clientinfo; }
    void deleteclientinfo(void *ci) { delete (clientinfo *)ci; }
    void serverinit() {}
    int reserveclients() { return 0; }
    int numchannels() { return 3; }
    void clientdisconnect(int n) { if(clientinfo *ci = getinfo(n)) ci->connected = false; }

    int clientconnect(int n, uint ip)
    {
        while(clients.length() <= n) clients.add(NULL);
        clientinfo *ci = (clientinfo *)getclientinfo(n);
        clients[n] = ci;
        ci->clientnum = n;
        ci->connected = true;
        ci->local = false;
        sendf(n, 1, "ri5ss", N_SERVINFO, n, PROTOCOL_VERSION, rnd(INT_MAX), 0, "", "");
        sendf(n, 1, "ri", N_WELCOME);
        return DISC_NONE;
    }

    void localconnect(int n)
    {
        while(clients.length() <= n) clients.add(NULL);
        clientinfo *ci = (clientinfo *)getclientinfo(n);
        clients[n] = ci;
        ci->clientnum = n;
        ci->connected = ci->local = true;
    }

    void localdisconnect(int n) { if(clientinfo *ci = getinfo(n)) ci->connected = false; }
    bool allowbroadcast(int n) { clientinfo *ci = getinfo(n); return ci && ci->connected; }
    void recordpacket(int chan, void *data, int len) {}

    void broadcastedit(int sender, int chan, packetbuf &p, int msg)
    {
        packetbuf q(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(q, msg);
        q.put(p.buf, p.remaining());
        sendpacket(-1, chan, q.finalize(), sender);
    }

    void parsepacket(int sender, int chan, packetbuf &p)
    {
        if(chan != 1) return;
        while(p.remaining())
        {
            int type = getint(p);
            switch(type)
            {
                case N_CONNECT:
                {
                    string pass;
                    getstring(pass, p, sizeof(pass));
                    sendf(sender, 1, "ri5ss", N_SERVINFO, sender, PROTOCOL_VERSION, rnd(INT_MAX), 0, "", "");
                    sendf(sender, 1, "ri", N_WELCOME);
                    break;
                }
                case N_TEXT:
                {
                    string text;
                    getstring(text, p, sizeof(text));
                    sendf(-1, 1, "ris", N_SERVMSG, text);
                    break;
                }
                case N_EDITENT:
                case N_EDITF: case N_EDITT: case N_EDITM: case N_FLIP: case N_COPY: case N_PASTE: case N_ROTATE: case N_REPLACE: case N_DELCUBE: case N_CALCLIGHT: case N_REMIP: case N_EDITVSLOT: case N_UNDO: case N_REDO: case N_EDITVAR:
                    broadcastedit(sender, 1, p, type);
                    p.pad(p.remaining());
                    break;
                case N_NEWMAP:
                    broadcastedit(sender, 1, p, type);
                    p.pad(p.remaining());
                    break;
                case N_GETMAP:
                    if(mapdata) sendfile(sender, 2, mapdata, "i", N_SENDMAP);
                    break;
                default:
                {
                    int size = msgsizelookup(type);
                    if(size > 0) loopi(size-1) getint(p);
                    else p.pad(p.remaining());
                    break;
                }
            }
        }
    }

    void sendservmsg(const char *s) { sendf(-1, 1, "ris", N_SERVMSG, s); }
    bool sendpackets(bool force) { return false; }
    void serverinforeply(ucharbuf &req, ucharbuf &p)
    {
        putint(p, PROTOCOL_VERSION);
        putint(p, 0);
        putint(p, maxclients);
        putint(p, 3);
        putint(p, gamemode);
        putint(p, 0);
        putint(p, MM_OPEN);
        sendstring(smapname, p);
        sendstring("Hover engine", p);
        sendserverinforeply(p);
    }
    void serverupdate() {}
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

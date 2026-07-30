#include "game.h"

namespace game
{
    static int sessionid = 0;
    static string servdesc = "";
    static int authoritativeauthor = -1;
    static uint authoritativerevision = 0, authoritativerequestid = 0, synchronizedrevision = 0;
    vector<networkedit *> pendingnetworkedits;

    static void getsel(packetbuf &p, selinfo &sel)
    {
        sel.o.x = getint(p); sel.o.y = getint(p); sel.o.z = getint(p);
        sel.s.x = getint(p); sel.s.y = getint(p); sel.s.z = getint(p);
        sel.grid = getint(p); sel.orient = getint(p);
        sel.cx = getint(p); sel.cxs = getint(p); sel.cy = getint(p); sel.cys = getint(p);
        sel.corner = getint(p);
    }

#ifndef STANDALONE
    enum { PLAYER_IDENTITY_VERSION = 1, PLAYER_IDENTITY_MAX_RECORDS = 4096 };

    struct playeridentity
    {
        string serverid, playerid, privatekey, publickey;
    };

    static vector<playeridentity *> playeridentities;
    static string currentserverid = "";
    static playeridentity *currentidentity = NULL;
    static bool playeridentitiesloaded = false, playeridentitiescorrupt = false,
                currentidentitycreated = false;

    bool pendingnetworkworld = false, pendingnetworkreset = false,
         pendingnetworkfrozen = false, pendingnetworkrestoreposition = false;
    int pendingnetworkseed = 0, pendingnetworktime = 0;
    vec pendingnetworkposition;

    void resetclientreceive()
    {
        currentserverid[0] = '\0';
        currentidentity = NULL;
        currentidentitycreated = false;
        pendingnetworkworld = pendingnetworkreset = pendingnetworkrestoreposition = false;
        pendingnetworkedits.deletecontents();
        authoritativeauthor = -1;
        authoritativerevision = authoritativerequestid = synchronizedrevision = 0;
    }

    static bool validhex(const char *value, int minlen, int maxlen)
    {
        if(!value) return false;
        int len = 0;
        for(; value[len]; ++len)
            if(!isxdigit((uchar)value[len]) || len >= maxlen) return false;
        return len >= minlen;
    }

    static bool validcurvepoint(const char *value)
    {
        return value && (*value == '+' || *value == '-') && validhex(value + 1, 1, 64);
    }

    static bool writeidentitystring(stream &file, const char *value)
    {
        int len = value ? int(strlen(value)) : 0;
        return len <= USHRT_MAX && file.putlil<ushort>(ushort(len)) && (!len || file.write(value, len) == size_t(len));
    }

    static bool readidentitystring(stream &file, char *value, int size)
    {
        uint len = file.getlil<ushort>();
        if(len >= uint(size)) return false;
        if(len && file.read(value, len) != len) return false;
        value[len] = '\0';
        return true;
    }

    static bool replaceplayeridentityfile(const char *temporary, const char *finalname)
    {
#ifdef WIN32
        return MoveFileEx(temporary, finalname, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        return rename(temporary, finalname) == 0;
#endif
    }

    static bool writeplayeridentityfile(const char *name, playeridentity *single = NULL)
    {
        defformatstring(tempname, "%s.tmp", name);
        string finalpath, temppath;
        copystring(finalpath, findfile(name, "wb"));
        copystring(temppath, findfile(tempname, "wb"));
        stream *file = openrawfile(tempname, "wb");
        if(!file) return false;
        uint count = single ? 1U : uint(playeridentities.length());
        bool ok = file->write("CCPI", 4) == 4 &&
                  file->putlil<uint>(PLAYER_IDENTITY_VERSION) &&
                  file->putlil<uint>(count);
        loopv(playeridentities)
        {
            playeridentity *identity = single ? single : playeridentities[i];
            if(ok) ok = writeidentitystring(*file, identity->serverid) &&
                        writeidentitystring(*file, identity->playerid) &&
                        writeidentitystring(*file, identity->privatekey) &&
                        writeidentitystring(*file, identity->publickey);
            if(single) break;
        }
        delete file;
        if(!ok)
        {
            remove(temppath);
            return false;
        }
        if(!replaceplayeridentityfile(temppath, finalpath))
        {
            remove(temppath);
            return false;
        }
        return true;
    }

    static bool loadplayeridentityfile(const char *name, vector<playeridentity *> &identities)
    {
        stream *file = openrawfile(name, "rb");
        if(!file) return false;
        char magic[4];
        uint version = 0, count = 0;
        bool ok = file->read(magic, 4) == 4 && !memcmp(magic, "CCPI", 4) &&
                  (version = file->getlil<uint>()) == PLAYER_IDENTITY_VERSION &&
                  (count = file->getlil<uint>()) <= PLAYER_IDENTITY_MAX_RECORDS;
        loopi(ok ? int(count) : 0)
        {
            playeridentity *identity = new playeridentity;
            ok = readidentitystring(*file, identity->serverid, sizeof(identity->serverid)) &&
                 readidentitystring(*file, identity->playerid, sizeof(identity->playerid)) &&
                 readidentitystring(*file, identity->privatekey, sizeof(identity->privatekey)) &&
                 readidentitystring(*file, identity->publickey, sizeof(identity->publickey)) &&
                 validhex(identity->serverid, 48, 48) &&
                 (!identity->playerid[0] || validhex(identity->playerid, 48, 48)) &&
                 validhex(identity->privatekey, 1, 64) && validcurvepoint(identity->publickey);
            vector<char> calculated;
            if(ok) ok = calcpubkey(identity->privatekey, calculated) && !strcmp(identity->publickey, calculated.getbuf());
            if(!ok) delete identity;
            else identities.add(identity);
            if(!ok) break;
        }
        if(ok) ok = file->tell() == file->size();
        delete file;
        if(!ok) identities.deletecontents();
        return ok;
    }

    static void ensureplayeridentities()
    {
        if(playeridentitiesloaded) return;
        playeridentitiesloaded = true;
        bool loaded = loadplayeridentityfile("config/player-identities.dat", playeridentities);
        bool exists = fileexists(findfile("config/player-identities.dat", "rb"), "r");
        if(!loaded && exists)
        {
            playeridentitiescorrupt = true;
            conoutf(CON_ERROR, "player identity error: config/player-identities.dat is corrupt; no identities were loaded");
        }
    }

    static playeridentity *findplayeridentity(const char *serverid)
    {
        loopv(playeridentities)
        {
            if(!strcmp(playeridentities[i]->serverid, serverid)) return playeridentities[i];
        }
        return NULL;
    }

    static bool identityseed(char *seed, int size)
    {
        return identityrandomhex(seed, size, 32);
    }

    static playeridentity *createplayeridentity(const char *serverid)
    {
        playeridentity *identity = new playeridentity;
        copystring(identity->serverid, serverid);
        identity->playerid[0] = '\0';
        char seed[256];
        if(!identityseed(seed, sizeof(seed)))
        {
            delete identity;
            return NULL;
        }
        vector<char> privatekey, publickey;
        genprivkey(seed, privatekey, publickey);
        memset(seed, 0, sizeof(seed));
        copystring(identity->privatekey, privatekey.getbuf());
        copystring(identity->publickey, publickey.getbuf());
        playeridentities.add(identity);
        if(!writeplayeridentityfile("config/player-identities.dat"))
        {
            playeridentities.removeobj(identity);
            delete identity;
            return NULL;
        }
        return identity;
    }
#endif


    static void removeclient(int cn)
    {
        if(!clients.inrange(cn) || !clients[cn]) return;
        gameent *d = clients[cn];
        clients[cn] = NULL;
        players.removeobj(d);
        delete d;
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
            case N_SERVERIDENTITY:
            {
                int version = getint(p);
                string serverid;
                getstring(serverid, p, sizeof(serverid));
                if(version != PLAYER_IDENTITY_VERSION || !validhex(serverid, 48, 48))
                {
                    conoutf(CON_ERROR, "server sent an invalid player identity announcement");
                    disconnect();
                    return;
                }
                ensureplayeridentities();
                if(playeridentitiescorrupt)
                {
                    conoutf(CON_ERROR, "repair or import the corrupt player identity file before connecting");
                    disconnect();
                    return;
                }
                copystring(currentserverid, serverid);
                currentidentity = findplayeridentity(serverid);
                if(!currentidentity)
                {
                    currentidentity = createplayeridentity(serverid);
                    if(!currentidentity)
                    {
                        conoutf(CON_ERROR, "identity error: could not save config/player-identities.dat");
                        disconnect();
                        return;
                    }
                    currentidentitycreated = true;
                }
                if(currentidentity->playerid[0]) addmsg(N_IDENTITYLOGIN, "ris", PLAYER_IDENTITY_VERSION, currentidentity->playerid);
                else addmsg(N_IDENTITYREGISTER, "riss", PLAYER_IDENTITY_VERSION, currentidentity->publickey, player1 ? player1->name : "");
                break;
            }
            case N_IDENTITYCHALLENGE:
            {
                int version = getint(p);
                string challenge;
                getstring(challenge, p, sizeof(challenge));
                if(version != PLAYER_IDENTITY_VERSION || !currentidentity ||
                   !validcurvepoint(challenge))
                {
                    conoutf(CON_ERROR, "server sent an invalid player identity challenge");
                    disconnect();
                    return;
                }
                vector<char> answer;
                if(!answerchallenge(currentidentity->privatekey, challenge, answer))
                {
                    conoutf(CON_ERROR, "could not answer player identity challenge");
                    disconnect();
                    return;
                }
                addmsg(N_IDENTITYRESPONSE, "ris", PLAYER_IDENTITY_VERSION, answer.getbuf());
                break;
            }
            case N_IDENTITYSUCCESS:
            {
                int version = getint(p);
                string playerid;
                getstring(playerid, p, sizeof(playerid));
                if(version != PLAYER_IDENTITY_VERSION || !currentidentity ||
                   !validhex(playerid, 48, 48))
                {
                    conoutf(CON_ERROR, "server sent an invalid player identity result");
                    disconnect();
                    return;
                }
                bool assignedplayerid = !currentidentity->playerid[0];
                if(strcmp(currentidentity->playerid, playerid))
                {
                    copystring(currentidentity->playerid, playerid);
                    if(!writeplayeridentityfile("config/player-identities.dat"))
                    {
                        currentidentity->playerid[0] = '\0';
                        conoutf(CON_ERROR, "could not atomically save the assigned player ID");
                        disconnect();
                        return;
                    }
                }
                if(assignedplayerid) conoutf("identity %s (%s)", currentidentitycreated ? "created" : "accepted", currentidentitycreated ? "new player" : "returning player");
                else conoutf("identity accepted: returning player");
                currentidentitycreated = false;
                break;
            }
            case N_IDENTITYFAILURE:
            {
                int version = getint(p);
                string reason;
                getstring(reason, p, sizeof(reason));
                conoutf(CON_ERROR, "identity rejected (%s)%s%s",
                        currentidentity && currentidentity->playerid[0] ? "returning" :
                        (currentidentitycreated ? "new" : "pending"),
                        version == PLAYER_IDENTITY_VERSION && reason[0] ? ": " : "",
                        version == PLAYER_IDENTITY_VERSION ? reason : "");
                disconnect();
                return;
            }
            case N_IDENTITYREVOKED:
            {
                getint(p);
                string reason;
                getstring(reason, p, sizeof(reason));
                conoutf(CON_ERROR, "identity revoked%s%s", reason[0] ? ": " : "", reason);
                disconnect();
                return;
            }
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
                authoritativerequestid = uint(getint(p));
                break;
            case N_WORLDSTATE:
            {
                pendingnetworkseed = getint(p);
                synchronizedrevision = uint(getint(p));
                pendingnetworktime = getint(p);
                pendingnetworkfrozen = getint(p) != 0;
                pendingnetworkreset = getint(p) != 0;
                gamemode = getint(p);
                if(!m_valid(gamemode) || (!m_creative && !m_survival)) gamemode = STARTGAMEMODE;
                const int breakmillis = getint(p), scatterbreakmillis = getint(p);
                receiveserversettings(breakmillis, scatterbreakmillis);
                pendingnetworkrestoreposition = pendingnetworkreset && player1;
                if(pendingnetworkrestoreposition)
                {
                    pendingnetworkposition = player1->o;
                    worldpositiontoabsolute(pendingnetworkposition);
                }
                pendingnetworkedits.deletecontents();
                authoritativeauthor = -1;
                authoritativerevision = authoritativerequestid = 0;
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
            case N_WORLDAUTH:
            {
                networkedit *edit = new networkedit;
                edit->type = type;
                edit->author = authoritativeauthor;
                edit->revision = authoritativerevision;
                edit->requestid = authoritativerequestid;
                if(type != N_WORLDAUTH) getsel(p, edit->selection);
                switch(type)
                {
                    case N_WORLDAUTH:
                        loopi(6) edit->args[i] = getint(p);
                        break;
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
                authoritativerevision = authoritativerequestid = 0;
                break;
            }
            case N_INVENTORYSTATE:
            {
                const int slots = getint(p), selected = getint(p);
                if(slots != SURVIVAL_USABLE_SLOTS)
                {
                    conoutf(CON_ERROR, "server sent an invalid survival inventory");
                    disconnect();
                    return;
                }
                int items[SURVIVAL_USABLE_SLOTS], counts[SURVIVAL_USABLE_SLOTS];
                loopi(SURVIVAL_USABLE_SLOTS)
                {
                    items[i] = -1;
                    counts[i] = 0;
                }
                loopi(max(slots, 0))
                {
                    const int item = getint(p), count = getint(p);
                    if(i < SURVIVAL_USABLE_SLOTS)
                    {
                        items[i] = item;
                        counts[i] = count;
                    }
                }
                if(!p.overread()) receiveinventory(items, counts, SURVIVAL_USABLE_SLOTS, selected);
                break;
            }
            case N_ACTIONRESULT:
            {
                const uint requestid = uint(getint(p));
                const int result = getint(p);
                string reason;
                getstring(reason, p, sizeof(reason));
                if(result < ACTION_RESULT_REJECTED || result > ACTION_RESULT_CORRECTED)
                {
                    conoutf(CON_ERROR, "server sent an invalid action result");
                    disconnect();
                    return;
                }
                receiveactionresult(requestid, result, reason);
                break;
            }
            case N_BREAKSTATE:
            {
                const int actor = getint(p);
                const uint requestid = uint(getint(p));
                const int phase = getint(p), action = getint(p);
                ivec target;
                target.x = getint(p); target.y = getint(p); target.z = getint(p);
                const int orient = getint(p), stage = getint(p);
                receivebreakstate(actor, requestid, phase, action, target, orient, stage);
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
    static void executeidentityalias(const char *name, const char *first, const char *second, int numargs)
    {
        string command;
        copystring(command, name);
        if(numargs > 0)
        {
            string escaped;
            copystring(escaped, escapestring(first ? first : ""));
            concformatstring(command, " %s", escaped);
        }
        if(numargs > 1)
        {
            string escaped;
            copystring(escaped, escapestring(second ? second : ""));
            concformatstring(command, " %s", escaped);
        }
        execute(command);
    }

    ICOMMAND(identityinfo, "sN", (char *serverid, int *numargs),
    {
        ensureplayeridentities();
        const char *wanted = *numargs > 0 && serverid[0] ? serverid : currentserverid;
        playeridentity *identity = wanted[0] ? findplayeridentity(wanted) : NULL;
        if(!identity) conoutf(CON_ERROR, "no player identity found");
        else conoutf("server ID: %s, player ID: %s, public key: %s", identity->serverid, identity->playerid[0] ? identity->playerid : "(registration pending)", identity->publickey);
    });
    ICOMMAND(identityexport, "ss", (char *serverid, char *filename),
    {
        ensureplayeridentities();
        playeridentity *identity = findplayeridentity(serverid);
        if(!identity) conoutf(CON_ERROR, "no player identity found for that server ID");
        else if(!filename[0] || !writeplayeridentityfile(filename, identity)) conoutf(CON_ERROR, "could not export player identity");
        else conoutf(CON_WARN, "identity exported; anyone obtaining that file can impersonate this player");
    });
    ICOMMAND(identityimport, "s", (char *filename),
    {
        ensureplayeridentities();
        vector<playeridentity *> imported;
        if(!filename[0] || !loadplayeridentityfile(filename, imported) || imported.length() != 1)
        {
            imported.deletecontents();
            conoutf(CON_ERROR, "identity import must contain exactly one valid identity");
        }
        else
        {
            playeridentity *identity = imported.remove(0);
            loopvrev(playeridentities) if(!strcmp(playeridentities[i]->serverid, identity->serverid))
            {
                playeridentity *old = playeridentities.remove(i);
                if(old == currentidentity) currentidentity = identity;
                delete old;
            }
            playeridentities.add(identity);
            if(!writeplayeridentityfile("config/player-identities.dat")) conoutf(CON_ERROR, "identity was imported in memory but could not be saved");
            else
            {
                playeridentitiescorrupt = false;
                conoutf("imported player identity for server %s", identity->serverid);
            }
        }
    });
    ICOMMAND(identitydelete, "sN", (char *serverid, int *numargs),
    {
        ensureplayeridentities();
        const char *wanted = *numargs > 0 && serverid[0] ? serverid : currentserverid;
        int found = -1;
        loopv(playeridentities) if(!strcmp(playeridentities[i]->serverid, wanted)) { found = i; break; }
        if(found < 0) conoutf(CON_ERROR, "no player identity found");
        else
        {
            playeridentity *identity = playeridentities.remove(found);
            if(identity == currentidentity) currentidentity = NULL;
            delete identity;
            if(!writeplayeridentityfile("config/player-identities.dat")) conoutf(CON_ERROR, "identity was deleted in memory but the identity file could not be updated");
            else conoutf(CON_WARN, "identity deleted locally; this does not delete its server-side data");
        }
    });
    ICOMMAND(identityrotate, "s", (char *serverid),
    {
        ensureplayeridentities();
        playeridentity *identity = findplayeridentity(serverid);
        if(!identity || !identity->playerid[0])
            conoutf(CON_ERROR, "identity rotation requires a registered server and player ID");
        else
        {
            string oldprivate;
            string oldpublic;
            copystring(oldprivate, identity->privatekey);
            copystring(oldpublic, identity->publickey);
            char seed[256];
            if(!identityseed(seed, sizeof(seed)))
            {
                conoutf(CON_ERROR, "operating system randomness is unavailable");
                return;
            }
            vector<char> privatekey;
            vector<char> publickey;
            genprivkey(seed, privatekey, publickey);
            memset(seed, 0, sizeof(seed));
            copystring(identity->privatekey, privatekey.getbuf());
            copystring(identity->publickey, publickey.getbuf());
            if(!writeplayeridentityfile("config/player-identities.dat"))
            {
                copystring(identity->privatekey, oldprivate);
                copystring(identity->publickey, oldpublic);
                conoutf(CON_ERROR, "could not save the rotated player identity");
            }
            else conoutf(CON_WARN, "identity key rotated; an admin must replace the registered " "public key with %s before reconnecting", identity->publickey);
            memset(oldprivate, 0, sizeof(oldprivate));
        }
    });
    ICOMMAND(idinfo, "sN", (char *serverid, int *numargs), executeidentityalias("identityinfo", serverid, NULL, *numargs > 0 ? 1 : 0));
    ICOMMAND(idexport, "ss", (char *serverid, char *filename), executeidentityalias("identityexport", serverid, filename, 2));
    ICOMMAND(idimport, "s", (char *filename), executeidentityalias("identityimport", filename, NULL, 1));
    ICOMMAND(iddelete, "sN", (char *serverid, int *numargs), executeidentityalias("identitydelete", serverid, NULL, *numargs > 0 ? 1 : 0));
    ICOMMAND(idrotate, "s", (char *serverid), executeidentityalias("identityrotate", serverid, NULL, 1));
}

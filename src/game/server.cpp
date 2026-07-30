#include "game.h"

namespace server
{
    enum
    {
        SERVER_DAY_MILLIS = 20 * 60 * 1000,
        SERVER_START_MILLIS = 8 * SERVER_DAY_MILLIS / 24,
        SERVER_JOURNAL_VERSION = 1,
        MIN_SERVER_JOURNAL_PROTOCOL = 8,
        PLAYER_IDENTITY_VERSION = 1,
        PLAYER_IDENTITY_TIMEOUT = 15000,
        PLAYER_IDENTITY_MAX_RECORDS = 100000
    };

    enum identityauthstate
    {
        IDENTITY_UNAUTHENTICATED = 0,
        IDENTITY_AWAITING_IDENTITY,
        IDENTITY_AWAITING_RESPONSE,
        IDENTITY_AUTHENTICATED,
        IDENTITY_REJECTED
    };

    enum identitykind
    {
        IDENTITY_KIND_NONE = 0,
        IDENTITY_KIND_NEW,
        IDENTITY_KIND_RETURNING,
        IDENTITY_KIND_RECOVERY
    };

    SVAR(serverpass, "");
    SVAR(adminpass, "");
    SVAR(serverworld, "multiplayer");
    SVAR(serverdesc, "Cube-Craft authoritative server");
    SVAR(servermotd, "");
    VAR(serverworldseed, 0, 1337, INT_MAX);
    VAR(identityduplicatepolicy, 0, 0, 1);

    struct serveridentity
    {
        string playerid, publickey, nickname;
        int permissions;
        bool revoked, banned;

        serveridentity() : permissions(0), revoked(false), banned(false)
        {
            playerid[0] = publickey[0] = nickname[0] = '\0';
        }
    };

    static vector<serveridentity *> serveridentities;
    static string persistentserverid = "";
    static bool serveridentitiesloaded = false;

    struct identityratelimit
    {
        uint ip;
        int failures, window;
    };

    static vector<identityratelimit> identityratelimits;

    struct clientinfo
    {
        int clientnum, privilege, lastpositionmillis, identitystate, identitykind,
            identitychallengemillis,
            identityfailures, identityfailurewindow;
        uint ip;
        bool connected, local, worldready, hasposition;
        string name, playerid, pendingpublickey, pendingname;
        vector<uchar> position;
        vec o;
        ENetPacket *getmap;
        void *identitychallenge;
        serveridentity *identity;

        clientinfo() : clientnum(-1), privilege(PRIV_NONE), lastpositionmillis(0),
                       identitystate(IDENTITY_UNAUTHENTICATED), identitykind(IDENTITY_KIND_NONE),
                       identitychallengemillis(0),
                       identityfailures(0), identityfailurewindow(0),
                       ip(0),
                       connected(false), local(false),
                       worldready(false), hasposition(false), o(0, 0, 0), getmap(NULL),
                       identitychallenge(NULL), identity(NULL)
        {
            name[0] = playerid[0] = pendingpublickey[0] = pendingname[0] = '\0';
        }

        ~clientinfo()
        {
            if(identitychallenge) freechallenge(identitychallenge);
        }
    };

    struct serveredit
    {
        uint revision, timestamp;
        int author, type;
        bool active, hasselection;
        string ownerid;
        selinfo selection;
        vector<uchar> payload;

        serveredit() : revision(0), timestamp(0), author(-1), type(-1),
                       active(true), hasselection(false)
        {
            ownerid[0] = '\0';
        }
    };

    vector<clientinfo *> clients;
    vector<serveredit *> worldhistory, worldredostack;
    string smapname = "";
    stream *mapdata = NULL;
    int gamemode = STARTGAMEMODE;
    uint worldeditrevision = 0;
    int worldclockmillis = SERVER_START_MILLIS, lastworldtimesync = 0;
    bool worldtimefrozen = false, serverworldready = true, journalinitialized = false;

    static bool valididentityhex(const char *value, int minlen, int maxlen)
    {
        if(!value) return false;
        int len = 0;
        for(; value[len]; ++len)
            if(!isxdigit((uchar)value[len]) || len >= maxlen) return false;
        return len >= minlen;
    }

    static bool valididentitypoint(const char *value)
    {
        return value && (*value == '+' || *value == '-') && valididentityhex(value + 1, 1, 64);
    }

    static bool writeserveridentitystring(stream &file, const char *value)
    {
        int len = value ? int(strlen(value)) : 0;
        return len <= USHRT_MAX && file.putlil<ushort>(ushort(len)) && (!len || file.write(value, len) == size_t(len));
    }

    static bool readserveridentitystring(stream &file, char *value, int size)
    {
        uint len = file.getlil<ushort>();
        if(len >= uint(size)) return false;
        if(len && file.read(value, len) != len) return false;
        value[len] = '\0';
        return true;
    }

    static bool replaceserveridentityfile(const char *temporary, const char *finalname)
    {
#ifdef WIN32
        return MoveFileEx(temporary, finalname, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        return rename(temporary, finalname) == 0;
#endif
    }

    static bool writeserveridentities()
    {
        const char *name = "config/server-identities.dat";
        defformatstring(tempname, "%s.tmp", name);
        string finalpath, temppath;
        copystring(finalpath, findfile(name, "wb"));
        copystring(temppath, findfile(tempname, "wb"));
        stream *file = openrawfile(tempname, "wb");
        if(!file) return false;
        bool ok = file->write("CCSI", 4) == 4 &&
                  file->putlil<uint>(PLAYER_IDENTITY_VERSION) &&
                  writeserveridentitystring(*file, persistentserverid) &&
                  file->putlil<uint>(uint(serveridentities.length()));
        loopv(serveridentities)
        {
            serveridentity &identity = *serveridentities[i];
            if(ok) ok = writeserveridentitystring(*file, identity.playerid) &&
                        writeserveridentitystring(*file, identity.publickey) &&
                        writeserveridentitystring(*file, identity.nickname) &&
                        file->putlil<int>(identity.permissions) &&
                        file->putlil<uint>(identity.revoked ? 1U : 0U) &&
                        file->putlil<uint>(identity.banned ? 1U : 0U);
        }
        delete file;
        if(!ok)
        {
            remove(temppath);
            return false;
        }
        if(!replaceserveridentityfile(temppath, finalpath))
        {
            remove(temppath);
            return false;
        }
        return true;
    }

    static void makepersistentid(char *id, int size, int discriminator = 0)
    {
        (void)discriminator;
        if(!identityrandomhex(id, size, 24)) id[0] = '\0';
    }

    static serveridentity *findserveridentity(const char *playerid)
    {
        loopv(serveridentities)
        {
            if(!strcmp(serveridentities[i]->playerid, playerid)) return serveridentities[i];
        }
        return NULL;
    }

    static serveridentity *findserveridentitybykey(const char *publickey)
    {
        loopv(serveridentities)
        {
            if(!strcmp(serveridentities[i]->publickey, publickey)) return serveridentities[i];
        }
        return NULL;
    }

    static bool loadserveridentities()
    {
        if(serveridentitiesloaded) return persistentserverid[0] != '\0';
        serveridentitiesloaded = true;
        stream *file = openrawfile("config/server-identities.dat", "rb");
        if(!file)
        {
            makepersistentid(persistentserverid, sizeof(persistentserverid));
            bool saved = persistentserverid[0] && writeserveridentities();
            if(saved) conoutf("created persistent server identity; player database is empty");
            else conoutf(CON_ERROR, "could not create the persistent server identity database");
            return saved;
        }
        char magic[4];
        uint version = 0, count = 0;
        bool ok = file->read(magic, 4) == 4 && !memcmp(magic, "CCSI", 4) &&
                  (version = file->getlil<uint>()) == PLAYER_IDENTITY_VERSION &&
                  readserveridentitystring(*file, persistentserverid, sizeof(persistentserverid)) &&
                  valididentityhex(persistentserverid, 48, 48) &&
                  (count = file->getlil<uint>()) <= PLAYER_IDENTITY_MAX_RECORDS;
        loopi(ok ? int(count) : 0)
        {
            serveridentity *identity = new serveridentity;
            uint revoked = 0, banned = 0;
            ok = readserveridentitystring(*file, identity->playerid, sizeof(identity->playerid)) &&
                 readserveridentitystring(*file, identity->publickey, sizeof(identity->publickey)) &&
                 readserveridentitystring(*file, identity->nickname, sizeof(identity->nickname)) &&
                 (identity->permissions = file->getlil<int>(), true) &&
                 (revoked = file->getlil<uint>(), true) &&
                 (banned = file->getlil<uint>(), true) &&
                 valididentityhex(identity->playerid, 48, 48) &&
                 valididentitypoint(identity->publickey) && revoked <= 1 && banned <= 1 &&
                 !findserveridentity(identity->playerid) &&
                 !findserveridentitybykey(identity->publickey);
            void *parsed = ok ? parsepubkey(identity->publickey) : NULL;
            if(parsed) freepubkey(parsed);
            else ok = false;
            identity->revoked = revoked != 0;
            identity->banned = banned != 0;
            if(ok) serveridentities.add(identity);
            else delete identity;
            if(!ok) break;
        }
        if(ok) ok = file->tell() == file->size();
        delete file;
        if(!ok)
        {
            persistentserverid[0] = '\0';
            serveridentities.deletecontents();
            conoutf(CON_ERROR, "server identity database is corrupt");
        }
        else
        {
            int revoked = 0, banned = 0;
            loopv(serveridentities)
            {
                if(serveridentities[i]->revoked) ++revoked;
                if(serveridentities[i]->banned) ++banned;
            }
            conoutf("loaded %d registered player identities (%d revoked, %d banned)", serveridentities.length(), revoked, banned);
        }
        return ok;
    }

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
        journalput32(body, uint(strlen(edit.ownerid)));
        body.put((const uchar *)edit.ownerid, strlen(edit.ownerid));
        body.put(edit.payload.getbuf(), edit.payload.length());
        return file.write("OP02", 4) == 4 &&
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
           ((protocol = file->getlil<uint>()) < MIN_SERVER_JOURNAL_PROTOCOL ||
            protocol > PROTOCOL_VERSION) ||
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
            bool hasowner = !memcmp(magic, "OP02", 4);
            if((!hasowner && memcmp(magic, "OP01", 4)) ||
               length < uint(hasowner ? 28 : 24) || length > uint(MAXTRANS + MAXSTRLEN + 64))
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
            uint revision, timestamp, author, type, active, payloadlen, ownerlen = 0;
            if(!journalread32(p, revision) || !journalread32(p, timestamp) ||
               !journalread32(p, author) || !journalread32(p, type) ||
               !journalread32(p, active) || !journalread32(p, payloadlen) ||
               (hasowner && !journalread32(p, ownerlen)) ||
               ownerlen >= MAXSTRLEN || ownerlen + payloadlen != uint(p.remaining()))
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
            if(ownerlen)
            {
                memcpy(edit->ownerid, p.pad(ownerlen), ownerlen);
                edit->ownerid[ownerlen] = '\0';
                if(!valididentityhex(edit->ownerid, 48, 48))
                {
                    delete edit;
                    recovered = true;
                    break;
                }
            }
            edit->payload.put(p.pad(payloadlen), payloadlen);
            updateservereditmetadata(*edit);
            worldhistory.add(edit);
            worldeditrevision = max(worldeditrevision, revision);
        }
        delete file;
        if(recovered)
        {
            conoutf(CON_WARN, "authoritative journal had a corrupt tail; recovered %d valid revisions", worldhistory.length());
            // Remove an actually incomplete tail before future appends;
            // otherwise every later record would remain hidden behind it.
            if(!rewriteserverjournal()) serverworldready = false;
        }
        conoutf("loaded %d authoritative world revisions for seed %d", worldhistory.length(), serverworldseed);
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

    static void sendprivilege(int cn, int subject, int privilege);
    static void sendworldstate(clientinfo &ci, bool reset);
    static void sendcommandresult(clientinfo &ci, const char *message);

    static void clearidentitychallenge(clientinfo &ci)
    {
        if(ci.identitychallenge)
        {
            freechallenge(ci.identitychallenge);
            ci.identitychallenge = NULL;
        }
        ci.identitychallengemillis = 0;
    }

    static identityratelimit *identitylimit(uint ip)
    {
        loopv(identityratelimits) if(identityratelimits[i].ip == ip)
            return &identityratelimits[i];
        identityratelimit &limit = identityratelimits.add();
        limit.ip = ip;
        limit.failures = 0;
        limit.window = max(totalmillis, 1);
        return &limit;
    }

    static bool identityratelimited(uint ip)
    {
        loopvrev(identityratelimits)
        {
            if(totalmillis - identityratelimits[i].window > 60000) identityratelimits.remove(i);
        }
        if(identityratelimits.length() >= 4096) identityratelimits.remove(0);
        identityratelimit *limit = identitylimit(ip);
        return limit->failures >= 5;
    }

    static void rejectidentity(clientinfo &ci, const char *reason, bool revoked = false)
    {
        clearidentitychallenge(ci);
        ci.identitystate = IDENTITY_REJECTED;
        identityratelimit *limit = identitylimit(ci.ip);
        if(totalmillis - limit->window > 60000)
        {
            limit->window = max(totalmillis, 1);
            limit->failures = 0;
        }
        ++limit->failures;
        const char *kind = ci.identitykind == IDENTITY_KIND_NEW ? "new player" :
                           ci.identitykind == IDENTITY_KIND_RETURNING ? "returning player" :
                           ci.identitykind == IDENTITY_KIND_RECOVERY ? "registration recovery" :
                           "unclassified identity";
        conoutf(CON_WARN, "identity rejected: client %d, %s, reason: %s (failures %d/5)", ci.clientnum, kind, reason ? reason : "authentication rejected", limit->failures);
        sendf(ci.clientnum, 1, "ri2s", revoked ? N_IDENTITYREVOKED : N_IDENTITYFAILURE, PLAYER_IDENTITY_VERSION, reason ? reason : "authentication rejected");
    }

    static bool beginidentitychallenge(clientinfo &ci, const char *publickey)
    {
        if(!valididentitypoint(publickey)) return false;
        void *parsed = parsepubkey(publickey);
        if(!parsed) return false;
        uint seed[8];
        if(!identityrandombytes((uchar *)seed, sizeof(seed)))
        {
            freepubkey(parsed);
            return false;
        }
        seed[6] ^= uint(max(totalmillis, 1));
        seed[7] ^= uint(ci.clientnum);
        vector<char> challenge;
        clearidentitychallenge(ci);
        ci.identitychallenge = genchallenge(parsed, seed, sizeof(seed), challenge);
        freepubkey(parsed);
        if(!ci.identitychallenge) return false;
        ci.identitychallengemillis = max(totalmillis, 1);
        ci.identitystate = IDENTITY_AWAITING_RESPONSE;
        sendf(ci.clientnum, 1, "ri2s", N_IDENTITYCHALLENGE, PLAYER_IDENTITY_VERSION, challenge.getbuf());
        return true;
    }

    static bool duplicateidentity(clientinfo &ci)
    {
        loopv(clients)
        {
            clientinfo *other = clients[i];
            if(!other || other == &ci || !other->connected || strcmp(other->playerid, ci.playerid)) continue;
            if(identityduplicatepolicy)
            {
                disconnect_client(other->clientnum, DISC_PRIVATE);
                return false;
            }
            rejectidentity(ci, "this player identity is already connected");
            return true;
        }
        return false;
    }

    static void completeidentity(clientinfo &ci)
    {
        if(duplicateidentity(ci)) return;
        ci.identitystate = IDENTITY_AUTHENTICATED;
        ci.connected = true;
        const char *kind = ci.identitykind == IDENTITY_KIND_NEW ? "new player" : "returning player";
        conoutf("identity accepted: client %d, %s", ci.clientnum, kind);
        sendf(ci.clientnum, 1, "ri2s", N_IDENTITYSUCCESS, PLAYER_IDENTITY_VERSION, ci.playerid);
        sendf(ci.clientnum, 1, "ri", N_WELCOME);
        loopv(clients)
        {
            clientinfo *other = clients[i];
            if(other && other->connected) sendprivilege(ci.clientnum, other->clientnum, other->privilege);
        }
        if(servermotd[0]) sendcommandresult(ci, servermotd);
        sendworldstate(ci, false);
    }

    void *newclientinfo() { return new clientinfo; }
    void deleteclientinfo(void *info)
    {
        clientinfo *ci = (clientinfo *)info;
        if(ci && clients.inrange(ci->clientnum) && clients[ci->clientnum] == ci) clients[ci->clientnum] = NULL;
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
        sendf(ci.clientnum, 1, "ri6", N_WORLDSTATE, serverworldseed, int(worldeditrevision), worldclockmillis, worldtimefrozen ? 1 : 0, reset ? 1 : 0);
    }

    static void replayworld(clientinfo &ci)
    {
        loopv(worldhistory)
        {
            if(worldhistory[i]->active) sendserveredit(ci.clientnum, *worldhistory[i]);
        }
        sendf(ci.clientnum, 1, "ri2", N_WORLDSYNC, int(worldeditrevision));
        ci.worldready = true;
    }

    static void resetallclients()
    {
        loopv(clients)
        {
            if(clients[i] && clients[i]->connected) sendworldstate(*clients[i], true);
        }
    }

    void serverinit()
    {
        copystring(smapname, serverworld);
        journalinitialized = false;
        if(!loadserveridentities()) serverworldready = false;
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
            if(!ci->connected &&
               (ci->identitystate == IDENTITY_AWAITING_IDENTITY ||
                ci->identitystate == IDENTITY_AWAITING_RESPONSE))
                conoutf(CON_WARN, "identity authentication interrupted: client %d disconnected while %s",
                        ci->clientnum, ci->identitystate == IDENTITY_AWAITING_IDENTITY ? "awaiting identity selection" : "awaiting challenge response");
            if(ci->connected) sendf(-1, 1, "ri2x", N_CDIS, n, n);
            ci->connected = false;
            ci->identitystate = IDENTITY_REJECTED;
            clearidentitychallenge(*ci);
        }
    }

    int clientconnect(int n, uint ip)
    {
        if(!persistentserverid[0] || !ensureserverworld()) return DISC_PRIVATE;
        if(identityratelimited(ip))
        {
            conoutf(CON_WARN, "identity connection rate-limited: 5 failures in 60 seconds");
            return DISC_PRIVATE;
        }
        while(clients.length() <= n) clients.add(NULL);
        clientinfo *ci = (clientinfo *)getclientinfo(n);
        clients[n] = ci;
        ci->clientnum = n;
        ci->ip = ip;
        ci->connected = false;
        ci->local = false;
        ci->identitystate = IDENTITY_UNAUTHENTICATED;
        sendf(n, 1, "ri5ss", N_SERVINFO, n, PROTOCOL_VERSION, rnd(INT_MAX), 0, serverdesc, "");
        sendf(n, 1, "ri2s", N_SERVERIDENTITY, PLAYER_IDENTITY_VERSION, persistentserverid);
        return DISC_NONE;
    }

    void localconnect(int n)
    {
        if(!persistentserverid[0] || !ensureserverworld()) return;
        while(clients.length() <= n) clients.add(NULL);
        clientinfo *ci = (clientinfo *)getclientinfo(n);
        clients[n] = ci;
        ci->clientnum = n;
        ci->connected = ci->local = true;
        ci->identitystate = IDENTITY_AUTHENTICATED;
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
        if(!editselectiontype(type) || !readselection(p, sel) || !validselection(ci, sel, error)) return false;

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
            if(!allowedface && !allowedtexture && !alloweddelete && !allowedscatter)
            {
                error = "this edit operation requires admin";
                return false;
            }
        }

        edit.author = ci.clientnum;
        copystring(edit.ownerid, ci.playerid);
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
            if(ci) sendf(ci->clientnum, 1, "ris", N_SERVMSG, "world edit rejected: server could not persist it");
            delete edit;
            return;
        }
        worldhistory.add(edit);
        worldredostack.deletecontents();
        loopv(clients)
        {
            clientinfo *recipient = clients[i];
            if(recipient && recipient->connected && recipient->worldready) sendserveredit(recipient->clientnum, *edit);
        }
    }

    static serveredit *cloneserveredit(const serveredit &source)
    {
        serveredit *edit = new serveredit;
        edit->revision = source.revision;
        edit->timestamp = source.timestamp;
        edit->author = source.author;
        copystring(edit->ownerid, source.ownerid);
        edit->type = source.type;
        edit->active = source.active;
        edit->hasselection = source.hasselection;
        edit->selection = source.selection;
        edit->payload.put(source.payload.getbuf(), source.payload.length());
        return edit;
    }

    static bool sameserveredit(const serveredit &a, const serveredit &b)
    {
        return a.type == b.type && a.payload.length() == b.payload.length() && (!a.payload.length() || !memcmp(a.payload.getbuf(), b.payload.getbuf(), a.payload.length()));
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

        if(cubecaseequal(command, "identityrevoke"))
        {
            serveridentity *identity = valididentityhex(args, 48, 48) ? findserveridentity(args) : NULL;
            if(!identity)
            {
                sendcommandresult(ci, "usage: /identityrevoke <player ID>");
                return;
            }
            bool old = identity->revoked;
            identity->revoked = true;
            if(!writeserveridentities())
            {
                identity->revoked = old;
                sendcommandresult(ci, "could not persist identity revocation");
                return;
            }
            conoutf(CON_WARN, "identity administration: identity revoked by client %d",
                    ci.clientnum);
            sendcommandresult(ci, "player identity revoked");
            loopv(clients)
            {
                clientinfo *other = clients[i];
                if(!other || strcmp(other->playerid, identity->playerid)) continue;
                sendf(other->clientnum, 1, "ri2s", N_IDENTITYREVOKED, PLAYER_IDENTITY_VERSION, "revoked by an administrator");
                disconnect_client(other->clientnum, DISC_PRIVATE);
            }
            return;
        }

        if(cubecaseequal(command, "identityreplace"))
        {
            char *publickey = args;
            while(*publickey && !iscubespace(*publickey)) ++publickey;
            if(*publickey) *publickey++ = '\0';
            while(iscubespace(*publickey)) ++publickey;
            serveridentity *identity = valididentityhex(args, 48, 48) ?
                                       findserveridentity(args) : NULL;
            void *parsed = valididentitypoint(publickey) ? parsepubkey(publickey) : NULL;
            serveridentity *duplicate = parsed ? findserveridentitybykey(publickey) : NULL;
            if(!identity || !parsed || (duplicate && duplicate != identity))
            {
                if(parsed) freepubkey(parsed);
                sendcommandresult(ci, "usage: /identityreplace <player ID> <new public key>");
                return;
            }
            freepubkey(parsed);
            string oldkey;
            copystring(oldkey, identity->publickey);
            bool oldrevoked = identity->revoked;
            copystring(identity->publickey, publickey);
            identity->revoked = false;
            if(!writeserveridentities())
            {
                copystring(identity->publickey, oldkey);
                identity->revoked = oldrevoked;
                sendcommandresult(ci, "could not persist identity replacement");
                return;
            }
            conoutf(CON_WARN, "identity administration: public key replaced by client %d",
                    ci.clientnum);
            sendcommandresult(ci, "player identity public key replaced");
            loopv(clients)
            {
                clientinfo *other = clients[i];
                if(other && !strcmp(other->playerid, identity->playerid)) disconnect_client(other->clientnum, DISC_PRIVATE);
            }
            return;
        }

        if(cubecaseequal(command, "identityban") ||
           cubecaseequal(command, "identityunban"))
        {
            serveridentity *identity = valididentityhex(args, 48, 48) ? findserveridentity(args) : NULL;
            if(!identity)
            {
                sendcommandresult(ci, "usage: /identityban|identityunban <player ID>");
                return;
            }
            bool old = identity->banned;
            identity->banned = cubecaseequal(command, "identityban");
            if(!writeserveridentities())
            {
                identity->banned = old;
                sendcommandresult(ci, "could not persist identity ban state");
                return;
            }
            conoutf(CON_WARN, "identity administration: identity %s by client %d",
                    identity->banned ? "banned" : "unbanned", ci.clientnum);
            sendcommandresult(ci, identity->banned ? "player identity banned" : "player identity unbanned");
            if(identity->banned) loopv(clients)
            {
                clientinfo *other = clients[i];
                if(other && !strcmp(other->playerid, identity->playerid))
                    disconnect_client(other->clientnum, DISC_PRIVATE);
            }
            return;
        }

        if(cubecaseequal(command, "identitypermission"))
        {
            char *permission = args;
            while(*permission && !iscubespace(*permission)) ++permission;
            if(*permission) *permission++ = '\0';
            while(iscubespace(*permission)) ++permission;
            char *end = NULL;
            long value = strtol(permission, &end, 10);
            serveridentity *identity = valididentityhex(args, 48, 48) ?
                                       findserveridentity(args) : NULL;
            if(!identity || end == permission || *end || value < INT_MIN || value > INT_MAX)
            {
                sendcommandresult(ci, "usage: /identitypermission <player ID> <integer>");
                return;
            }
            int old = identity->permissions;
            identity->permissions = int(value);
            if(!writeserveridentities())
            {
                identity->permissions = old;
                sendcommandresult(ci, "could not persist identity permissions");
                return;
            }
            conoutf("identity administration: permissions changed from %d to %d by client %d",
                    old, identity->permissions, ci.clientnum);
            sendcommandresult(ci, "player identity permissions updated");
            return;
        }

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
            defformatstring(message, "worldundo: %d authoritative change%s reverted", applied, applied == 1 ? "" : "s");
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
                copystring(edit->ownerid, ci.playerid);
                edit->active = true;
                worldhistory.add(edit);
                ++applied;
            }
            if(applied)
            {
                rewriteserverjournal();
                resetallclients();
            }
            defformatstring(message, "worldredo: %d authoritative change%s restored", applied, applied == 1 ? "" : "s");
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
                ivec minimum(min(x1, x2), min(y1, y2), min(z1, z2)), maximum(max(x1, x2) + 1, max(y1, y2) + 1, max(z1, z2) + 1);
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
            defformatstring(message, "worldrevert: %d authoritative change%s reverted", applied, applied == 1 ? "" : "s");
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
            defformatstring(message, "worldrestore: %d change%s reverted in chunk %d %d", applied, applied == 1 ? "" : "s", x, y);
            sendcommandresult(ci, message);
            return;
        }

        if(cubecaseequal(command, "worlddiff"))
        {
            int active = 0;
            loopv(worldhistory) if(worldhistory[i]->active) ++active;
            if(!strncmp(args, "compact", 7)) rewriteserverjournal();
            if(strncmp(args, "stats", 5) && strncmp(args, "compact", 7) && strncmp(args, "verify", 6))
            {
                sendcommandresult(ci, "usage: /worlddiff <stats|compact|verify>");
                return;
            }
            defformatstring(message, "worlddiff: %d active, %d audit records, revision %u, seed %d", active, worldhistory.length(), worldeditrevision, serverworldseed);
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
            if(ci && !ci->connected)
            {
                p.pad(p.remaining());
                if(ci->identitystate == IDENTITY_REJECTED) disconnect_client(sender, DISC_MSGERR);
                else rejectidentity(*ci, "authentication is required before position updates");
                return;
            }
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
        if(chan != 1)
        {
            clientinfo *ci = getinfo(sender);
            if(ci && !ci->connected)
            {
                if(ci->identitystate == IDENTITY_REJECTED) disconnect_client(sender, DISC_MSGERR);
                else rejectidentity(*ci, "authentication is required before data requests");
            }
            return;
        }
        while(p.remaining())
        {
            int type = getint(p);
            clientinfo *senderinfo = getinfo(sender);
            bool identitypacket = type == N_CONNECT || type == N_IDENTITYLOGIN ||
                                  type == N_IDENTITYREGISTER || type == N_IDENTITYRESPONSE;
            bool identitystatevalid = senderinfo &&
                ((type == N_CONNECT && senderinfo->identitystate == IDENTITY_UNAUTHENTICATED) ||
                 ((type == N_IDENTITYLOGIN || type == N_IDENTITYREGISTER) &&
                  senderinfo->identitystate == IDENTITY_AWAITING_IDENTITY) ||
                 (type == N_IDENTITYRESPONSE &&
                  senderinfo->identitystate == IDENTITY_AWAITING_RESPONSE));
            if(senderinfo && !senderinfo->connected && identitypacket && !identitystatevalid)
            {
                p.pad(p.remaining());
                if(senderinfo->identitystate == IDENTITY_REJECTED) disconnect_client(sender, DISC_MSGERR);
                else rejectidentity(*senderinfo, "unexpected player identity message");
                return;
            }
            if(senderinfo && !senderinfo->connected && !identitypacket)
            {
                p.pad(p.remaining());
                if(senderinfo->identitystate == IDENTITY_REJECTED)
                    disconnect_client(sender, DISC_MSGERR);
                else rejectidentity(*senderinfo, "authentication is required before gameplay");
                return;
            }
            switch(type)
            {
                case N_CONNECT:
                {
                    clientinfo *ci = getinfo(sender);
                    string pass;
                    getstring(pass, p, sizeof(pass));
                    if(!ci || ci->connected || ci->identitystate != IDENTITY_UNAUTHENTICATED) break;
                    if(p.remaining())
                    {
                        p.pad(p.remaining());
                        rejectidentity(*ci, "malformed connection negotiation");
                        break;
                    }
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
                    ci->identitystate = IDENTITY_AWAITING_IDENTITY;
                    break;
                }
                case N_IDENTITYLOGIN:
                {
                    clientinfo *ci = getinfo(sender);
                    int version = getint(p);
                    string playerid;
                    getstring(playerid, p, sizeof(playerid));
                    if(!ci || ci->identitystate != IDENTITY_AWAITING_IDENTITY) break;
                    if(version != PLAYER_IDENTITY_VERSION || !valididentityhex(playerid, 48, 48) || p.remaining())
                    {
                        rejectidentity(*ci, "malformed identity login");
                        break;
                    }
                    ci->identitykind = IDENTITY_KIND_RETURNING;
                    copystring(ci->playerid, playerid);
                    serveridentity *identity = findserveridentity(playerid);
                    if(!identity)
                    {
                        rejectidentity(*ci, "unknown player identity");
                        break;
                    }
                    if(identity->revoked || identity->banned)
                    {
                        rejectidentity(*ci, identity->banned ? "identity is banned" : "identity was revoked", identity->revoked);
                        break;
                    }
                    ci->identity = identity;
                    if(!beginidentitychallenge(*ci, identity->publickey)) rejectidentity(*ci, "registered public key is invalid");
                    break;
                }
                case N_IDENTITYREGISTER:
                {
                    clientinfo *ci = getinfo(sender);
                    int version = getint(p);
                    string publickey, nickname;
                    getstring(publickey, p, sizeof(publickey));
                    getstring(nickname, p, sizeof(nickname));
                    if(!ci || ci->identitystate != IDENTITY_AWAITING_IDENTITY) break;
                    if(version != PLAYER_IDENTITY_VERSION || !valididentitypoint(publickey) || p.remaining())
                    {
                        rejectidentity(*ci, "malformed registration");
                        break;
                    }
                    void *parsed = parsepubkey(publickey);
                    if(!parsed)
                    {
                        rejectidentity(*ci, "malformed public key");
                        break;
                    }
                    freepubkey(parsed);
                    serveridentity *existing = findserveridentitybykey(publickey);
                    if(existing)
                    {
                        if(existing->revoked || existing->banned) rejectidentity(*ci, existing->banned ? "identity is banned" : "identity was revoked", existing->revoked);
                        else
                        {
                            ci->identity = existing;
                            ci->identitykind = IDENTITY_KIND_RECOVERY;
                            copystring(ci->playerid, existing->playerid);
                            if(!beginidentitychallenge(*ci, existing->publickey)) rejectidentity(*ci, "registered public key is invalid");
                        }
                        break;
                    }
                    filtertext(ci->pendingname, nickname, false, false, MAXSTRLEN);
                    copystring(ci->pendingpublickey, publickey);
                    ci->identitykind = IDENTITY_KIND_NEW;
                    int attempt = 0;
                    do makepersistentid(ci->playerid, sizeof(ci->playerid), ++attempt);
                    while(findserveridentity(ci->playerid) && attempt < 100);
                    if(!ci->playerid[0] || findserveridentity(ci->playerid) || !beginidentitychallenge(*ci, ci->pendingpublickey))
                        rejectidentity(*ci, "could not create registration challenge");
                    break;
                }
                case N_IDENTITYRESPONSE:
                {
                    clientinfo *ci = getinfo(sender);
                    int version = getint(p);
                    string answer;
                    getstring(answer, p, sizeof(answer));
                    if(!ci || ci->identitystate != IDENTITY_AWAITING_RESPONSE || !ci->identitychallenge)
                    {
                        if(ci) rejectidentity(*ci, "no authentication challenge is pending");
                        break;
                    }
                    void *expected = ci->identitychallenge;
                    ci->identitychallenge = NULL;
                    int issued = ci->identitychallengemillis;
                    ci->identitychallengemillis = 0;
                    bool valid = version == PLAYER_IDENTITY_VERSION &&
                                 valididentityhex(answer, 1, 64) &&
                                 !p.remaining() &&
                                 totalmillis - issued <= PLAYER_IDENTITY_TIMEOUT &&
                                 checkchallenge(answer, expected);
                    freechallenge(expected);
                    if(!valid)
                    {
                        rejectidentity(*ci, totalmillis - issued > PLAYER_IDENTITY_TIMEOUT ? "authentication challenge expired" : "challenge verification failed");
                        break;
                    }
                    if(!ci->identity)
                    {
                        if(findserveridentitybykey(ci->pendingpublickey))
                        {
                            rejectidentity(*ci, "public key was registered concurrently");
                            break;
                        }
                        serveridentity *identity = new serveridentity;
                        copystring(identity->playerid, ci->playerid);
                        copystring(identity->publickey, ci->pendingpublickey);
                        copystring(identity->nickname, ci->pendingname);
                        serveridentities.add(identity);
                        if(!writeserveridentities())
                        {
                            serveridentities.removeobj(identity);
                            delete identity;
                            rejectidentity(*ci, "could not persist player registration");
                            break;
                        }
                        ci->identity = identity;
                    }
                    completeidentity(*ci);
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
                    if(ci->identity && strcmp(ci->identity->nickname, ci->name))
                    {
                        copystring(ci->identity->nickname, ci->name);
                        if(!writeserveridentities())
                            conoutf(CON_ERROR, "could not persist identity nickname for client %d",
                                    ci->clientnum);
                    }
                    if(firstinit) loopv(clients)
                    {
                        clientinfo *other = clients[i];
                        if(i != sender && other && other->connected && other->name[0]) sendf(sender, 1, "ri2s", N_INITCLIENT, i, other->name);
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
                if(!source || !source->connected || !source->worldready || source == recipient || source->position.empty()) continue;
                if(!batch.empty() && batch.length() + source->position.length() > mtu) sent |= sendpositionbatch(recipient->clientnum, batch);
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
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci && ci->identitystate == IDENTITY_AWAITING_RESPONSE && ci->identitychallenge && totalmillis - ci->identitychallengemillis > PLAYER_IDENTITY_TIMEOUT)
                rejectidentity(*ci, "authentication challenge expired");
        }
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

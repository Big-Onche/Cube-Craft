#include "game.h"

namespace entities
{
    vector<extentity *> ents;

    int extraentinfosize() { return 0; }
    void writeent(entity &e, char *buf) {}
    void readent(entity &e, char *buf, int ver) {}

#ifndef STANDALONE
    vector<extentity *> &getents() { return ents; }

    bool mayattach(extentity &e) { return false; }
    bool attachent(extentity &e, extentity &a) { return false; }

    const char *entmodel(const entity &e) { return NULL; }

    void preloadentities() {}
    void renderentities() {}

    void checkitems(gameent *d) {}
    void resetspawns()
    {
        loopv(ents)
        {
            ents[i]->clearspawned();
            ents[i]->clearnopickup();
        }
    }
    void spawnitems(bool force) {}
    void putitems(packetbuf &p) { putint(p, -1); }
    void setspawn(int i, bool on) {}
    void teleport(int n, gameent *d) {}
    void pickupeffects(int n, gameent *d) {}
    void teleporteffects(gameent *d, int tp, int td, bool local) {}
    void jumppadeffects(gameent *d, int jp, bool local) {}

    extentity *newentity() { return new gameentity(); }
    void deleteentity(extentity *e) { delete (gameentity *)e; }

    void clearents()
    {
        while(ents.length()) deleteentity(ents.pop());
    }

    void animatemapmodel(const extentity &e, int &anim, int &basetime) {}

    void fixentity(extentity &e)
    {
        switch(e.type)
        {
            case PLAYERSTART:
                e.attr1 = int(game::player1 ? game::player1->yaw : 0);
                break;
        }
    }

    void entradius(extentity &e, bool color)
    {
        switch(e.type)
        {
            case PLAYERSTART:
            {
                vec dir;
                vecfromyawpitch(e.attr1, 0, 1, 0, dir);
                renderentarrow(e, dir, 4);
                break;
            }
        }
    }

    bool printent(extentity &e, char *buf, int len) { return false; }
    const char *entnameinfo(entity &e) { return ""; }

    const char *entname(int i)
    {
        static const char * const entnames[MAXENTTYPES] =
        {
            "none?", "light", "mapmodel", "playerstart", "envmap", "particles", "sound", "spotlight", "decal"
        };
        return i >= 0 && i < MAXENTTYPES ? entnames[i] : "";
    }

    void editent(int i, bool local)
    {
        if(!local || !ents.inrange(i)) return;
        extentity &e = *ents[i];
        game::addmsg(N_EDITENT, "rii3ii5", i, int(e.o.x*DMF), int(e.o.y*DMF), int(e.o.z*DMF), e.type, e.attr1, e.attr2, e.attr3, e.attr4, e.attr5);
    }

    float dropheight(entity &e) { return 4.0f; }
#endif
}

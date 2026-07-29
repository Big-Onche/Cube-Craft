// worldio.cpp: loading & saving of maps and savegames

#include "engine.h"
#ifndef STANDALONE
#include "../game/world.h"
#endif
#include <errno.h>

#ifdef WIN32
#define WORLD_ULL_FORMAT "%I64u"
#else
#define WORLD_ULL_FORMAT "%llu"
#endif

void validmapname(char *dst, const char *src, const char *prefix = NULL, const char *alt = "untitled", size_t maxlen = 100)
{
    if(prefix) while(*prefix) *dst++ = *prefix++;
    const char *start = dst;
    if(src) loopi(maxlen)
    {
        char c = *src++;
        if(iscubealnum(c) || c == '_' || c == '-' || c == '/' || c == '\\') *dst++ = c;
        else break;
    }
    if(dst > start) *dst = '\0';
    else if(dst != alt) copystring(dst, alt, maxlen);
}

void fixmapname(char *name)
{
    validmapname(name, name, NULL, "");
}

static bool loadmapheader(stream *f, const char *mapname, mapheader &hdr)
{
    if(f->read(&hdr, sizeof(hdr)) != sizeof(hdr))
    {
        conoutf(CON_ERROR, "map %s has a malformed lightweight header", mapname);
        return false;
    }
    lilswap(&hdr.version, 4);
    if(memcmp(hdr.magic, "TMAP", 4) || hdr.version != MAPVERSION)
    {
        conoutf(CON_ERROR, "map %s is not a version %d lightweight octree", mapname, MAPVERSION);
        return false;
    }
    if(hdr.worldsize < (1 << 9) || hdr.worldsize > (1 << 16) ||
       (hdr.worldsize & (hdr.worldsize - 1)))
    {
        conoutf(CON_ERROR, "map %s has an invalid world size", mapname);
        return false;
    }
    return true;
}

bool loadents(const char *fname, vector<entity> &ents, uint *crc)
{
    string name;
    validmapname(name, fname);
    defformatstring(ogzname, "media/map/%s.ogz", name);
    path(ogzname);
    stream *f = openrawfile(ogzname, "rb");
    if(!f) return false;

    mapheader hdr;
    bool loaded = loadmapheader(f, ogzname, hdr);
    if(crc) *crc = 0;

    delete f;
    return loaded;
}

#ifndef STANDALONE
string ogzname, bakname, cfgname, picname;

VARP(savebak, 0, 2, 2);

enum
{
    WORLD_GRID_POWER = 4,
    WORLD_BLOCK_SIZE = 1 << WORLD_GRID_POWER,
    WORLD_CHUNK_BLOCKS = 64,
    WORLD_CHUNK_SIZE = WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS,
    WORLD_SECTION_BLOCKS = 16,
    WORLD_SECTION_SIZE = WORLD_BLOCK_SIZE * WORLD_SECTION_BLOCKS,
    WORLD_MIN_HEIGHT = -256,
    WORLD_MAX_HEIGHT = 256,
    WORLD_HEIGHT_BLOCKS = WORLD_MAX_HEIGHT - WORLD_MIN_HEIGHT,
    WORLD_MAP_SIZE = WORLD_HEIGHT_BLOCKS * WORLD_BLOCK_SIZE,
    WORLD_CHUNK_SCALE = WORLD_GRID_POWER + 9,
    WORLD_CHUNK_MAP_SIZE = 1 << WORLD_CHUNK_SCALE,
    WORLD_CHUNK_ROOT_SIZE = WORLD_CHUNK_MAP_SIZE >> 1,
    WORLD_SECTION_LAYERS = WORLD_MAP_SIZE / WORLD_SECTION_SIZE,
    WORLD_GROUND_HEIGHT = -WORLD_MIN_HEIGHT * WORLD_BLOCK_SIZE,
    WORLD_DIRT_DEPTH = 4 * WORLD_BLOCK_SIZE,
    WORLD_RUNTIME_SCALE = 16,
    WORLD_RUNTIME_SIZE = 1 << WORLD_RUNTIME_SCALE,
    WORLD_RUNTIME_CHUNKS = WORLD_RUNTIME_SIZE / WORLD_CHUNK_SIZE,
    WORLD_RUNTIME_CENTER = WORLD_RUNTIME_CHUNKS / 2,
    WORLD_MAX_CHUNK_DIST = WORLD_RUNTIME_CENTER - 2,
    WORLD_SECTION_COLUMNS = WORLD_CHUNK_SIZE / WORLD_SECTION_SIZE,
    WORLD_SECTION_TILES = WORLD_SECTION_COLUMNS * WORLD_SECTION_COLUMNS,
    WORLD_MAX_PREPARED_CHUNKS = 8,
    WORLD_MAX_COLUMN_CHANGES = 64,
    WORLD_MAX_SECTION_BATCH = 16,
    WORLD_MAX_SECTION_REGIONS = WORLD_MAX_SECTION_BATCH * 7
};

enum
{
    WORLD_SAVE_FORMAT_VERSION = 1,
    WORLDGEN_VERSION = 1,
    WORLD_DIFF_Z = 0,
    WORLD_DIFF_FRAME_MAX = 64 << 20,
    WORLD_DIFF_FLUSH_MILLIS = 10000
};

struct worldscatterinstance
{
    int x, y, z, type, orient;
    mutable float renderoffsetx, renderoffsety, rendermaxoffset;
    mutable int renderyaw;
    mutable bool rendertransformvalid;

    worldscatterinstance()
        : x(0), y(0), z(0), type(-1), orient(O_TOP),
          renderoffsetx(0), renderoffsety(0),
          rendermaxoffset(0), renderyaw(0), rendertransformvalid(false) {}
    worldscatterinstance(int x, int y, int z, int type, int orient = O_TOP)
        : x(x), y(y), z(z), type(type), orient(orient),
          renderoffsetx(0), renderoffsety(0),
          rendermaxoffset(0), renderyaw(0), rendertransformvalid(false) {}

    bool operator==(const worldscatterinstance &other) const
    {
        return x == other.x && y == other.y && z == other.z && type == other.type && orient == other.orient;
    }
};

struct worlddiffnode
{
    int x, y, z, size;
    uchar edges[12];
    ushort texture[6], material;

    worlddiffnode() : x(0), y(0), z(0), size(0), material(MAT_AIR)
    {
        memset(edges, 0, sizeof(edges));
        loopi(6) texture[i] = DEFAULT_GEOM;
    }
};

struct worldeditrecord
{
    int chunkx, chunky, chunkz, operation, author;
    int args[4];
    ullong revision, timestamp;
    selinfo selection;
    vector<worlddiffnode> before, after;
    vector<worldscatterinstance> scatterbefore, scatterafter;

    worldeditrecord()
        : chunkx(0), chunky(0), chunkz(WORLD_DIFF_Z), operation(0), author(-1),
          revision(0), timestamp(0)
    {
        memset(args, 0, sizeof(args));
    }
};

struct worldchunkdiffstate
{
    int x, y, z;
    ullong revision, snapshotrevision;
    ullong canonicalhash;
    vector<worldeditrecord *> pending, journal, audit;

    worldchunkdiffstate(int x, int y, int z = WORLD_DIFF_Z)
        : x(x), y(y), z(z), revision(0), snapshotrevision(0), canonicalhash(0)
    {
    }

    ~worldchunkdiffstate()
    {
        pending.deletecontents();
        journal.deletecontents();
        audit.deletecontents();
    }
};

struct worlddiffmetadata
{
    int seed, worldgenversion, saveformatversion;
    ullong parameterhash;
    bool valid;

    worlddiffmetadata()
        : seed(0), worldgenversion(0), saveformatversion(0), parameterhash(0), valid(false)
    {
    }
};

struct worldspawnmetadata
{
    bool valid;
    double x, y;
    float z, yaw, pitch;

    worldspawnmetadata() : valid(false), x(0), y(0), z(0), yaw(0), pitch(0) {}
};

VARP(maxchunkdist, 2, 3, WORLD_MAX_CHUNK_DIST);

struct worldcubedefinition
{
    string name, texture, sidetexture, bottom, bottomtexture;
    float texsize;
    int slot, sideslot, bottomslot;

    worldcubedefinition()
        : texsize(1), slot(DEFAULT_GEOM), sideslot(DEFAULT_GEOM), bottomslot(DEFAULT_GEOM)
    {
        name[0] = texture[0] = sidetexture[0] = bottom[0] = bottomtexture[0] = '\0';
    }
};

struct worldscatterdefinition
{
    string name, model, icon;
    int mapmodel;
    bool torch;

    worldscatterdefinition() : mapmodel(-1), torch(false)
    {
        name[0] = model[0] = icon[0] = '\0';
    }
};

static vector<worldcubedefinition *> worldcubedefinitions;
static vector<worldscatterdefinition *> worldscatterdefinitions;
static int worldgrassscatter = -1, worldrosescatter = -1,
           worldtulipscatter = -1, worlddandelionscatter = -1;
static int worldgrasstexture = DEFAULT_GEOM, worldgrasssidetexture = DEFAULT_GEOM,
           worldgrassbottomtexture = DEFAULT_GEOM,
           worlddirttexture = DEFAULT_GEOM, worldstonetexture = DEFAULT_GEOM,
           worldsandtexture = DEFAULT_GEOM, worldsnowtexture = DEFAULT_GEOM,
           worldwoodtexture = DEFAULT_GEOM, worldleaftexture = DEFAULT_GEOM;
static void updateleavesalpha();
static void setworldleavesalpha(cube *root, bool enabled);

int numworldcubes()
{
    return worldcubedefinitions.length();
}

int getworldcubeslot(int index)
{
    return worldcubedefinitions.inrange(index) ? worldcubedefinitions[index]->slot : DEFAULT_GEOM;
}

const char *getworldcubename(int index)
{
    return worldcubedefinitions.inrange(index) ? worldcubedefinitions[index]->name : "";
}

const char *getworldcubetexture(int index, int face)
{
    static string texturepath;
    if(!worldcubedefinitions.inrange(index)) return "";
    worldcubedefinition &type = *worldcubedefinitions[index];
    const char *texture = type.texture;
    if(face == WORLD_CUBE_SIDE && type.sidetexture[0]) texture = type.sidetexture;
    else if(face == WORLD_CUBE_BOTTOM)
        texture = type.bottomtexture[0] ? type.bottomtexture
                : type.sidetexture[0] ? type.sidetexture
                : type.texture;
    formatstring(texturepath, "media/texture/%s", texture);
    return texturepath;
}

int numworldscatters()
{
    return worldscatterdefinitions.length();
}

const char *getworldscattername(int index)
{
    return worldscatterdefinitions.inrange(index) ? worldscatterdefinitions[index]->name : "";
}

const char *getworldscattermodel(int index)
{
    return worldscatterdefinitions.inrange(index) ? worldscatterdefinitions[index]->model : "";
}

const char *getworldscattericon(int index)
{
    static string iconpath;
    if(!worldscatterdefinitions.inrange(index)) return "";
    const worldscatterdefinition &type = *worldscatterdefinitions[index];
    if(type.icon[0]) return type.icon;
    formatstring(iconpath, "media/model/%s/diffuse.png", type.model);
    return iconpath;
}

bool isworldtorch(int index)
{
    return worldscatterdefinitions.inrange(index) && worldscatterdefinitions[index]->torch;
}
VARFP(leavesalpha, 0, 1, 1, updateleavesalpha());

static bool isworldleaftexture(const cube &c)
{
    if(worldleaftexture == DEFAULT_GEOM || c.children || isempty(c)) return false;
    loopi(6) if(c.texture[i] != worldleaftexture) return false;
    return true;
}

bool isworldleafcube(const cube &c)
{
    return leavesalpha != 0 && isworldleaftexture(c);
}

static worldcubedefinition *findworldcube(const char *name)
{
    loopv(worldcubedefinitions) if(!cubecasecmp(worldcubedefinitions[i]->name, name)) return worldcubedefinitions[i];
    return NULL;
}

static worldscatterdefinition *findworldscatter(const char *name)
{
    loopv(worldscatterdefinitions)
        if(!cubecasecmp(worldscatterdefinitions[i]->name, name))
            return worldscatterdefinitions[i];
    return NULL;
}

void worldreset()
{
    worldcubedefinitions.deletecontents();
    worldscatterdefinitions.deletecontents();
    worldgrassscatter = worldrosescatter = worldtulipscatter = worlddandelionscatter = -1;
    worldgrasstexture = worldgrasssidetexture = worldgrassbottomtexture = worlddirttexture =
        worldstonetexture = worldsandtexture = worldsnowtexture = worldwoodtexture =
        worldleaftexture = DEFAULT_GEOM;
}

COMMAND(worldreset, "");

static void defineworldcube(const char *name, const char *texture, float texsize,
                              const char *side, const char *bottom, int numargs)
{
    if(!name[0] || !texture[0])
    {
        conoutf(CON_ERROR, "worldcube requires a cube name and texture path");
        return;
    }

    worldcubedefinition *type = findworldcube(name);
    if(!type) type = worldcubedefinitions.add(new worldcubedefinition);
    copystring(type->name, name);
    copystring(type->texture, texture);
    copystring(type->sidetexture, numargs >= 4 && side ? side : "");
    copystring(type->bottom, numargs >= 5 && bottom ? bottom : "");
    type->bottomtexture[0] = '\0';
    type->texsize = texsize > 0 ? texsize : 1;
}

ICOMMAND(worldcube, "ssfssN", (char *name, char *texture, float *texsize,
                                char *side, char *bottom, int *numargs),
{
    defineworldcube(name, texture, *texsize, side, bottom, *numargs);
});

static void defineworldscatter(const char *name, const char *model, bool torch)
{
    if(!name[0] || !model[0])
    {
        conoutf(CON_ERROR, "worldscatter requires a name and model path");
        return;
    }
    worldscatterdefinition *type = findworldscatter(name);
    if(!type) type = worldscatterdefinitions.add(new worldscatterdefinition);
    copystring(type->name, name);
    copystring(type->model, model);
    type->icon[0] = '\0';
    type->torch = torch;
}

ICOMMAND(worldscatter, "ss", (char *name, char *model),
{
    defineworldscatter(name, model, false);
});

ICOMMAND(worldtorch, "ss", (char *name, char *model),
{
    defineworldscatter(name, model, true);
});

static int loadworldtextureslot(const char *path, float texsize, bool alpha)
{
    const char *texture = escapestring(path);
    string command;
    if(alpha)
        formatstring(command, "setshader leafworld; texture 0 %s; texture a %s; texscale %.9g; texalpha 1 1",
                     texture, texture, texsize);
    else
        formatstring(command, "setshader stdworld; texture 0 %s; texscale %.9g",
                     texture, texsize);
    execute(command);
    return slots.last()->variants->index;
}

static bool findworldscatterimage(const char *model, const char *basename, string &imagepath)
{
    defformatstring(directory, "media/model/%s", model);
    vector<char *> files;
    listfiles(directory, NULL, files);
    files.sort();

    const size_t baselen = strlen(basename);
    loopv(files)
    {
        const char *filename = files[i];
        if(cubecasecmp(filename, basename, baselen) ||
           filename[baselen] != '.' || !filename[baselen + 1])
            continue;

        defformatstring(candidate, "%s/%s", directory, filename);
        if(textureload(candidate, 3, true, false) == notexture) continue;
        copystring(imagepath, candidate);
        files.deletecontents();
        return true;
    }
    files.deletecontents();
    return false;
}

static void resolveworldscattericon(worldscatterdefinition &type)
{
    type.icon[0] = '\0';
    if(findworldscatterimage(type.model, "logo", type.icon)) return;
    if(findworldscatterimage(type.model, "diffuse", type.icon)) return;
    formatstring(type.icon, "media/model/%s/diffuse.png", type.model);
}

static bool loadworlddefinitions()
{
    worldreset();
    if(!execfile("config/world.cfg", false))
    {
        conoutf(CON_ERROR, "could not load config/world.cfg");
        return false;
    }

    worldcubedefinition *grass = findworldcube("Grass"), *dirt = findworldcube("Dirt"),
                    *stone = findworldcube("Stone"), *sand = findworldcube("Sand"),
                    *snow = findworldcube("Snow"), *wood = findworldcube("Wood"),
                    *leaves = findworldcube("Leaves");
    if(!grass || !dirt || !stone || !sand || !snow || !wood || !leaves)
    {
        conoutf(CON_ERROR, "world.cfg must define Grass, Dirt, Stone, Sand, Snow, Wood, and Leaves cubes");
        return false;
    }

    execute("texturereset; texsky; setshader stdworld");
    loopv(worldcubedefinitions)
    {
        worldcubedefinition &type = *worldcubedefinitions[i];
        const bool alpha = &type == leaves;
        type.slot = loadworldtextureslot(type.texture, type.texsize, alpha);
        type.sideslot = type.sidetexture[0]
                      ? loadworldtextureslot(type.sidetexture, type.texsize, alpha)
                      : type.slot;
    }
    loopv(worldcubedefinitions)
    {
        worldcubedefinition &type = *worldcubedefinitions[i];
        worldcubedefinition *bottomtype = type.bottom[0] ? findworldcube(type.bottom) : NULL;
        if(type.bottom[0] && !bottomtype)
        {
            conoutf(CON_ERROR, "world cube %s references unknown bottom cube %s",
                    type.name, type.bottom);
            return false;
        }
        type.bottomslot = bottomtype ? bottomtype->slot : type.sideslot;
        copystring(type.bottomtexture, bottomtype ? bottomtype->texture
                                                 : type.sidetexture[0] ? type.sidetexture
                                                                       : type.texture);
    }

    worldgrasstexture = grass->slot;
    worldgrasssidetexture = grass->sideslot;
    worldgrassbottomtexture = grass->bottomslot;
    worlddirttexture = dirt->slot;
    worldstonetexture = stone->slot;
    worldsandtexture = sand->slot;
    worldsnowtexture = snow->slot;
    worldwoodtexture = wood->slot;
    worldleaftexture = leaves->slot;
    loopv(worldscatterdefinitions)
    {
        worldscatterdefinition &type = *worldscatterdefinitions[i];
        resolveworldscattericon(type);
        type.mapmodel = registermapmodelpath(type.model);
        if(type.mapmodel < 0 || !loadmapmodel(type.mapmodel))
        {
            conoutf(CON_ERROR, "could not load world scatter model %s", type.model);
            type.mapmodel = -1;
        }
    }
    worldgrassscatter = worldscatterdefinitions.find(findworldscatter("Grass"));
    worldrosescatter = worldscatterdefinitions.find(findworldscatter("Rose"));
    worldtulipscatter = worldscatterdefinitions.find(findworldscatter("Tulip"));
    worlddandelionscatter = worldscatterdefinitions.find(findworldscatter("Dandelion"));
    setworldleavesalpha(worldroot, leavesalpha != 0);
    conoutf(CON_DEBUG, "loaded %d world cube and %d world scatter definitions",
            worldcubedefinitions.length(), worldscatterdefinitions.length());
    return true;
}

ICOMMAND(worldload, "", (), intret(loadworlddefinitions() ? 1 : 0));

struct worldchunk
{
    int x, y;
    cube *root;
    vector<worldscatterinstance> scatter;
    uint mountedtiles[WORLD_SECTION_LAYERS];
    uint contentknown[WORLD_SECTION_LAYERS], contenttiles[WORLD_SECTION_LAYERS],
         exposureknown[WORLD_SECTION_LAYERS], exposedtiles[WORLD_SECTION_LAYERS];
    schar surfacesections[WORLD_SECTION_TILES];
    uint request;
    bool loading, generating, saved, dirty, corrupted;

    worldchunk(int x, int y, cube *root, bool loading = false, bool saved = false)
        : x(x), y(y), root(root), request(0), loading(loading), generating(false),
          saved(saved), dirty(false), corrupted(false)
    {
        memclear(mountedtiles);
        memclear(contentknown);
        memclear(contenttiles);
        memclear(exposureknown);
        memclear(exposedtiles);
        loopi(WORLD_SECTION_TILES) surfacesections[i] = -2;
    }
};

static bool worldchunkmounted(const worldchunk &chunk);
static int worldchunkvaupdatekey(const ivec &origin);

struct worldsectionowner
{
    int chunkx, chunky;
    ushort section, tile;

    worldsectionowner() : chunkx(0), chunky(0), section(0), tile(0) {}
    worldsectionowner(int chunkx, int chunky, int section, int tile)
        : chunkx(chunkx), chunky(chunky), section(section), tile(tile) {}

    bool matches(const worldchunk &chunk, int section, int tile) const
    {
        return chunkx == chunk.x && chunky == chunk.y &&
               this->section == section && this->tile == tile;
    }
};

VARP(chunkremip, 0, 0, 1); // optional CPU-for-memory octree collapse on generation/load

struct worldchunkjob
{
    int x, y, seed, grasstexture, grasssidetexture, grassbottomtexture;
    int dirttexture, stonetexture, sandtexture, snowtexture, woodtexture, leaftexture;
    game::worldsettings settings;
    int families, optimized, loaderror;
    ullong revision, canonicalhash;
    uint epoch, request;
    bool loaded, remip;
    SDL_atomic_t cancelled;
    cube *root;
    vector<worldscatterinstance> scatter;
    string filename;

    worldchunkjob(int x, int y, uint epoch, uint request)
        : x(x), y(y), seed(game::getworldseed()),
          grasstexture(worldgrasstexture), grasssidetexture(worldgrasssidetexture),
          grassbottomtexture(worldgrassbottomtexture),
          dirttexture(worlddirttexture), stonetexture(worldstonetexture),
          sandtexture(worldsandtexture), snowtexture(worldsnowtexture),
          woodtexture(worldwoodtexture), leaftexture(worldleaftexture),
          families(0), optimized(0), loaderror(0), revision(0), canonicalhash(0),
          epoch(epoch), request(request),
          loaded(false), remip(chunkremip != 0), root(NULL)
    {
        SDL_AtomicSet(&cancelled, 0);
        filename[0] = '\0';
    }
};

static vector<worldchunk> worldchunks;
static vector<worldscatterinstance> reconstructedworldscatter;
static bool reconstructedworldscatterready = false;
static vector<worldchunkjob *> worldchunkjobs, worldchunkactivejobs, worldchunkresults;
static vector<worldchunkdiffstate *> worldchunkdiffstates;
static string worldfolder = "";
static bool applyloadworlddefaults = false;
static int activeworldchunk = -1;
static int worldfirstchunkx = 0, worldfirstchunky = 0;
static int lastplayerchunkx = INT_MIN, lastplayerchunky = INT_MIN, lastchunkdist = -1;
static bool rebuildingworldchunks = false;
static bool suppressworldchunkdirty = false;
static vector<SDL_Thread *> worldchunkworkers;
static SDL_mutex *worldchunkmutex = NULL;
static SDL_cond *worldchunkcond = NULL;
static bool stopworldchunkthread = false;
static uint worldchunkepoch = 1;
static uint worldchunkrequest = 1;
static int lastworldchunkpublish = -1;
static int worldchunkfocusx = 0, worldchunkfocusy = 0;
static int worldchunkaheadx = 0, worldchunkaheady = 0;
static int worldchunkviewx = 0, worldchunkviewy = 0;
static double lastworldchunkposx = 0, lastworldchunkposy = 0;
static float worldchunkvelocityx = 0, worldchunkvelocityy = 0;
static int lastworldchunkmotion = -1;
static vector<int> worldchunkvaupdates;
static hashset<int> worldchunkvaupdateset(1<<14);
static hashtable<int, worldsectionowner> worldsectionowners(1<<15);
static float worldchunkvasectionmillis = 2.0f;
static worlddiffmetadata activeworldmetadata;
static int worldeditauthor = -1, lastworlddiffflush = 0;
static ullong worldeditrevision = 0, incomingworldeditrevision = 0;
static vector<worldeditrecord *> worldredostack;

void worldpositiontoabsolute(vec &position)
{
    position.x += float(double(worldfirstchunkx) * WORLD_CHUNK_SIZE);
    position.y += float(double(worldfirstchunky) * WORLD_CHUNK_SIZE);
}

void worldpositiontolocal(vec &position)
{
    position.x -= float(double(worldfirstchunkx) * WORLD_CHUNK_SIZE);
    position.y -= float(double(worldfirstchunky) * WORLD_CHUNK_SIZE);
}

void worldselectiontoabsolute(selinfo &selection)
{
    selection.o.x += worldfirstchunkx * WORLD_CHUNK_SIZE;
    selection.o.y += worldfirstchunky * WORLD_CHUNK_SIZE;
}

void worldselectiontolocal(selinfo &selection)
{
    selection.o.x -= worldfirstchunkx * WORLD_CHUNK_SIZE;
    selection.o.y -= worldfirstchunky * WORLD_CHUNK_SIZE;
}

struct worldeditcapture
{
    bool active;
    int operation, author, args[4];
    selinfo selection;
    vector<worldeditrecord *> records;

    worldeditcapture() : active(false), operation(0), author(-1)
    {
        memset(args, 0, sizeof(args));
    }

    void clear()
    {
        records.deletecontents();
        active = false;
    }
};

static worldeditcapture currentworldedit;

static void setworldleavesalpha(cube *root, bool enabled)
{
    if(!root || worldleaftexture == DEFAULT_GEOM) return;
    loopi(8)
    {
        cube &c = root[i];
        if(c.children) setworldleavesalpha(c.children, enabled);
        else if(isworldleaftexture(c))
        {
            if(enabled) c.material |= MAT_ALPHA;
            else c.material &= ~MAT_ALPHA;
            c.visible = c.merged = 0;
        }
    }
}

static void updateleavesalpha()
{
    setworldleavesalpha(worldroot, leavesalpha != 0);
    loopv(worldchunks) if(worldchunks[i].root && worldchunks[i].root != worldroot)
        setworldleavesalpha(worldchunks[i].root, leavesalpha != 0);
    if(worldroot) allchanged();
}

VARP(asyncchunkloads, 2, 4, 4);
VARP(chunkthreads, 0, 0, 16);
VARP(chunkcachedist, 0, 0, 0);
VARP(chunkpendinglimit, 4, 8, 8);
VARP(chunklookahead, 0, 2, 8);
VARP(chunkpublishbudget, 4, 6, 33);
VARP(chunkcleanupbudget, 1, 6, 33);
VARP(chunksectionbatch, 1, 1, WORLD_MAX_SECTION_BATCH);
VARP(chunkvastagelimit, 1, 4, 16);
VARP(chunksurfaceloaddepth, 0, 3, WORLD_SECTION_LAYERS - 1); // depth in 16-block sections
VARP(drawfullchunk, 0, 0, 1);

static cube *generateworldchunk(int chunkx, int chunky);
static void generateworldscatter(cube *root, int chunkx, int chunky, const game::worldsettings &settings, vector<worldscatterinstance> &scatter);
static void cacheworldscattertransform(int chunkx, int chunky, float maxoffset, const worldscatterinstance &scatter);
static void cacheworldscattertransforms(int chunkx, int chunky, float maxoffset, const vector<worldscatterinstance> &scatter);
static bool validgeneratedworldscatter(const cube *root, const worldscatterinstance &scatter);
static cube *prepareworldchunk(worldchunkjob &job);
static void freepreparedworldchunk(cube *root);
static cube *newpreparedfamily(int &families);
static int worldchunkloader(void *);
static void shutdownworldchunkloader();
static void updateworldscatterers();
static void clearworldscattererentities();
static int findworldchunk(int x, int y);
static int remipworldchunk(cube *root, bool prepared, int &families, SDL_atomic_t *cancelled = NULL);
static int pruneworldchunkcache(int chunkx, int chunky, int limit);
static bool saveworldconfig();
static void worldchunkname(char *name, size_t len, const worldchunk &chunk);
static worldchunkdiffstate *findworldchunkdiffstate(int x, int y, bool create = false);
static bool applyworldchunkdiff(cube *root, int x, int y, const char *filename, vector<worldscatterinstance> &scatter, bool prepared, int &families, ullong &revision, ullong &canonicalhash);
static ullong hashworldchunk(cube *root);
static void flushworlddiffjournals(bool force = false);
static bool compactworldchunkdiff(worldchunk &chunk);
static void shutdownworlddiffwriter();
static ullong currentworldparameterhash();
static bool loadworldmetadata(const char *folder, int &chunkx, int &chunky, worldspawnmetadata &spawn, worlddiffmetadata &metadata);
void setmapfilenames(const char *fname, const char *cname);

int getworldsectionsize()
{
    return worldchunks.empty() ? 0 : WORLD_SECTION_SIZE;
}

static vector<uchar> worldskytransparent, worldskylight;
static vector<int> worldskyqueue;
static ivec worldskyorigin(0, 0, 0);
static int worldskydiameter = 0;

static void clearworldskyexposure()
{
    worldskytransparent.setsize(0);
    worldskylight.setsize(0);
    worldskyqueue.setsize(0);
    worldskydiameter = 0;
}

VARFP(skyexposureradius, 4, 16, 64, clearworldskyexposure());
VARFP(skyexposureattenuation, 1, 16, 255, clearworldskyexposure());

static bool worldskylighttransparent(const cube &c)
{
    return isempty(c) || (c.material&MATF_VOLUME) == MAT_GLASS || isworldleaftexture(c);
}

static bool worldskyfieldcontains(int blockx, int blocky)
{
    if(!worldskydiameter) return false;
    const int margin = max((worldskydiameter - 1) / 4, 1),
              worldblocks = worldsize / WORLD_BLOCK_SIZE;
    const bool insideleft = worldskyorigin.x == 0 ? blockx >= 0 : blockx >= worldskyorigin.x + margin,
               insideright = worldskyorigin.x + worldskydiameter >= worldblocks ? blockx < worldblocks : blockx < worldskyorigin.x + worldskydiameter - margin,
               insidefront = worldskyorigin.y == 0 ? blocky >= 0 : blocky >= worldskyorigin.y + margin,
               insideback = worldskyorigin.y + worldskydiameter >= worldblocks ? blocky < worldblocks : blocky < worldskyorigin.y + worldskydiameter - margin;
    return insideleft && insideright && insidefront && insideback;
}

static void invalidateworldskyexposure(const ivec &bbmin, const ivec &bbmax)
{
    if(!worldskydiameter) return;
    const ivec fieldmin(worldskyorigin.x * WORLD_BLOCK_SIZE, worldskyorigin.y * WORLD_BLOCK_SIZE, 0);
    const ivec fieldmax((worldskyorigin.x + worldskydiameter) * WORLD_BLOCK_SIZE, (worldskyorigin.y + worldskydiameter) * WORLD_BLOCK_SIZE, WORLD_MAP_SIZE);
    if(bbmax.x > fieldmin.x && bbmin.x < fieldmax.x && bbmax.y > fieldmin.y && bbmin.y < fieldmax.y && bbmax.z > fieldmin.z && bbmin.z < fieldmax.z)
        clearworldskyexposure();
}

static void buildworldskyexposure(int blockx, int blocky)
{
    ZoneScopedN("World/Six-direction skylight");
    const int worldblocks = worldsize / WORLD_BLOCK_SIZE,
              radius = min(skyexposureradius, max((worldblocks - 1) / 2, 0)),
              diameter = 2 * radius + 1,
              plane = diameter * diameter,
              cellcount = plane * WORLD_HEIGHT_BLOCKS;
    if(diameter <= 0 || cellcount <= 0)
    {
        clearworldskyexposure();
        return;
    }

    worldskyorigin.x = clamp(blockx - radius, 0, max(worldblocks - diameter, 0));
    worldskyorigin.y = clamp(blocky - radius, 0, max(worldblocks - diameter, 0));
    worldskyorigin.z = 0;
    worldskydiameter = diameter;
    worldskytransparent.setsize(0);
    worldskylight.setsize(0);
    worldskytransparent.pad(cellcount);
    worldskylight.pad(cellcount);
    worldskyqueue.setsize(0);
    worldskyqueue.reserve(cellcount);
    memset(worldskytransparent.getbuf(), 0, cellcount);
    memset(worldskylight.getbuf(), 0, cellcount);

    loop(y, diameter) loop(x, diameter)
    {
        bool directsky = true;
        for(int z = WORLD_HEIGHT_BLOCKS - 1; z >= 0;)
        {
            const ivec center((worldskyorigin.x + x) * WORLD_BLOCK_SIZE + WORLD_BLOCK_SIZE / 2,
                              (worldskyorigin.y + y) * WORLD_BLOCK_SIZE + WORLD_BLOCK_SIZE / 2,
                              z * WORLD_BLOCK_SIZE + WORLD_BLOCK_SIZE / 2);
            ivec cubeorigin;
            int cubesize;
            const cube &c = lookupcube(center, 0, cubeorigin, cubesize);
            const bool transparent = worldskylighttransparent(c);
            int bottom = cubesize >= WORLD_BLOCK_SIZE ? cubeorigin.z / WORLD_BLOCK_SIZE : z;
            bottom = clamp(bottom, 0, z);
            if(!transparent) directsky = false;

            for(; z >= bottom; --z)
            {
                if(!transparent) continue;
                const int index = (z * diameter + y) * diameter + x;

                worldskytransparent[index] = 1;
                if(directsky)
                {
                    worldskylight[index] = 255;
                    worldskyqueue.add(index);
                }
            }
        }
    }

    for(int cursor = 0; cursor < worldskyqueue.length(); ++cursor)
    {
        const int index = worldskyqueue[cursor],
                  light = worldskylight[index];
        if(light <= skyexposureattenuation) continue;
        const int propagated = light - skyexposureattenuation,
                  z = index / plane,
                  offset = index - z * plane,
                  y = offset / diameter,
                  x = offset - y * diameter;

        #define PROPAGATESKYLIGHT(neighbor) do { \
            const int next = (neighbor); \
            if(worldskytransparent[next] && worldskylight[next] < propagated) \
            { \
                worldskylight[next] = propagated; \
                worldskyqueue.add(next); \
            } \
        } while(0)

        if(x > 0) PROPAGATESKYLIGHT(index - 1);
        if(x + 1 < diameter) PROPAGATESKYLIGHT(index + 1);
        if(y > 0) PROPAGATESKYLIGHT(index - diameter);
        if(y + 1 < diameter) PROPAGATESKYLIGHT(index + diameter);
        if(z > 0) PROPAGATESKYLIGHT(index - plane);
        if(z + 1 < WORLD_HEIGHT_BLOCKS) PROPAGATESKYLIGHT(index + plane);

        #undef PROPAGATESKYLIGHT
    }
    worldskyqueue.setsize(0);
}

static float sampleworldskylight(float x, float y, float z)
{
    const int diameter = worldskydiameter, plane = diameter * diameter;
    float positions[3] =
    {
        x / WORLD_BLOCK_SIZE - worldskyorigin.x - 0.5f,
        y / WORLD_BLOCK_SIZE - worldskyorigin.y - 0.5f,
        z / WORLD_BLOCK_SIZE - 0.5f
    };
    int lower[3], upper[3];
    float blend[3];
    const int limits[3] = { diameter, diameter, WORLD_HEIGHT_BLOCKS };
    loopi(3)
    {
        positions[i] = clamp(positions[i], 0.0f, float(limits[i] - 1));
        lower[i] = int(floorf(positions[i]));
        upper[i] = min(lower[i] + 1, limits[i] - 1);
        blend[i] = positions[i] - lower[i];
    }

    float samples[2][2][2];
    loop(zindex, 2) loop(yindex, 2) loop(xindex, 2)
    {
        const int sx = xindex ? upper[0] : lower[0],
                  sy = yindex ? upper[1] : lower[1],
                  sz = zindex ? upper[2] : lower[2];
        samples[zindex][yindex][xindex] = worldskylight[sz * plane + sy * diameter + sx] / 255.0f;
    }

    float layers[2];
    loop(zindex, 2)
    {
        const float low = samples[zindex][0][0] + (samples[zindex][0][1] - samples[zindex][0][0]) * blend[0],
                    high = samples[zindex][1][0] + (samples[zindex][1][1] - samples[zindex][1][0]) * blend[0];
        layers[zindex] = low + (high - low) * blend[1];
    }
    return layers[0] + (layers[1] - layers[0]) * blend[2];
}

float getworldskyexposure(const vec &position)
{
    if(worldchunks.empty() || !worldroot || !insideworld(position) || position.z < 0 || position.z >= WORLD_MAP_SIZE)
        return 1.0f;

    const int blockx = int(floorf(position.x / WORLD_BLOCK_SIZE)),
              blocky = int(floorf(position.y / WORLD_BLOCK_SIZE));
    if(!worldskyfieldcontains(blockx, blocky)) buildworldskyexposure(blockx, blocky);
    return worldskydiameter ? sampleworldskylight(position.x, position.y, position.z) : 1.0f;
}

struct worlddebugstats
{
    int chunkx, chunky;
    double absolutex, absolutey, absolutez;
    int rendered;
    int loadingqueue, generationqueue;
    float tectonicactivity, tectonicuplift, tectonictrench, tectoniccaveexpansion;
};

static void getworlddebugstats(const vec &position, worlddebugstats &stats)
{
    stats.rendered = 0;
    stats.loadingqueue = stats.generationqueue = 0;
    stats.tectonicactivity = stats.tectonicuplift = stats.tectonictrench = stats.tectoniccaveexpansion = 0;

    if(!worldchunks.empty())
    {
        const int localchunkx = int(floor(position.x / WORLD_CHUNK_SIZE)),
                  localchunky = int(floor(position.y / WORLD_CHUNK_SIZE));
        stats.chunkx = worldfirstchunkx + localchunkx;
        stats.chunky = worldfirstchunky + localchunky;
        stats.absolutex = double(worldfirstchunkx) * WORLD_CHUNK_SIZE + position.x;
        stats.absolutey = double(worldfirstchunky) * WORLD_CHUNK_SIZE + position.y;

        loopv(worldchunks)
        {
            const worldchunk &chunk = worldchunks[i];
            if(worldchunkmounted(chunk)) stats.rendered++;
            if(chunk.loading)
            {
                if(chunk.generating) stats.generationqueue++;
                else stats.loadingqueue++;
            }
        }
    }
    else
    {
        stats.chunkx = int(floor(position.x / WORLD_CHUNK_SIZE));
        stats.chunky = int(floor(position.y / WORLD_CHUNK_SIZE));
        stats.absolutex = position.x;
        stats.absolutey = position.y;
    }
    stats.absolutez = position.z;
    if(!worldchunks.empty())
    {
        const int blockx = int(floor(stats.absolutex / WORLD_BLOCK_SIZE)),
                  blocky = int(floor(stats.absolutey / WORLD_BLOCK_SIZE)),
                  logicalz = int(floor(position.z / WORLD_BLOCK_SIZE)) + WORLD_MIN_HEIGHT;
        game::worldgenerator generator(game::getworldseed());
        const int surfaceheight = generator.height(blockx, blocky);
        const game::worldtectonicsample tectonics = generator.tectonics(blockx, blocky, max(surfaceheight - logicalz, 0));
        stats.tectonicactivity = tectonics.activity;
        stats.tectonicuplift = tectonics.landuplift;
        stats.tectonictrench = tectonics.oceantrench;
        stats.tectoniccaveexpansion = tectonics.caveexpansion;
    }
}

static worlddebugstats worlddebugcache;
static int worlddebugcachemillis = -1;

static const worlddebugstats &currentworlddebugstats()
{
    if(worlddebugcachemillis != totalmillis)
    {
        getworlddebugstats(camera1->o, worlddebugcache);
        worlddebugcachemillis = totalmillis;
    }
    return worlddebugcache;
}

static void debugcoordinateresult(double coordinate)
{
    defformatstring(value, "%.2f", coordinate);
    result(value);
}

static void debugworldvalueresult(float value)
{
    defformatstring(formatted, "%.3f", clamp(value, 0.0f, 1.0f));
    result(formatted);
}

ICOMMAND(getdebugcamx, "", (), debugcoordinateresult(currentworlddebugstats().absolutex));
ICOMMAND(getdebugcamy, "", (), debugcoordinateresult(currentworlddebugstats().absolutey));
ICOMMAND(getdebugcamz, "", (), debugcoordinateresult(currentworlddebugstats().absolutez));
ICOMMAND(getdebugchunkx, "", (), intret(currentworlddebugstats().chunkx));
ICOMMAND(getdebugchunky, "", (), intret(currentworlddebugstats().chunky));
ICOMMAND(getdebugrenderedfull, "", (), intret(currentworlddebugstats().rendered));
ICOMMAND(getdebugtargetchunks, "", (), intret((2 * maxchunkdist + 1) * (2 * maxchunkdist + 1)));
ICOMMAND(getdebugloadingqueue, "", (), intret(currentworlddebugstats().loadingqueue));
ICOMMAND(getdebuggenerationqueue, "", (), intret(currentworlddebugstats().generationqueue));
ICOMMAND(getdebugtectonicactivity, "", (), debugworldvalueresult(currentworlddebugstats().tectonicactivity));
ICOMMAND(getdebugtectonicuplift, "", (), debugworldvalueresult(currentworlddebugstats().tectonicuplift));
ICOMMAND(getdebugtectonictrench, "", (), debugworldvalueresult(currentworlddebugstats().tectonictrench));
ICOMMAND(getdebugtectoniccave, "", (), debugworldvalueresult(currentworlddebugstats().tectoniccaveexpansion));

void clearworldchunks()
{
    ZoneScopedN("Chunks/Clear all chunks");
    flushworlddiffjournals(true);
    shutdownworlddiffwriter();
    currentworldedit.clear();
    clearworldscattererentities();
    shutdownworldchunkloader();
    worldchunkvaupdates.setsize(0);
    worldchunkvaupdateset.clear();
    worldsectionowners.clear();
    clearworldskyexposure();
    loopv(worldchunks) if(worldchunks[i].root && worldchunks[i].root != worldroot)
    {
        ZoneScopedN("Chunks/Free chunk during clear");
        ZoneTextF("%d_%d", worldchunks[i].x, worldchunks[i].y);
        freeocta(worldchunks[i].root);
    }
    worldchunks.setsize(0);
    reconstructedworldscatter.setsize(0);
    reconstructedworldscatterready = false;
    worldchunkdiffstates.deletecontents();
    worldredostack.deletecontents();
    worldfolder[0] = '\0';
    activeworldmetadata = worlddiffmetadata();
    worldeditrevision = incomingworldeditrevision = 0;
    activeworldchunk = -1;
    worldfirstchunkx = worldfirstchunky = 0;
    lastplayerchunkx = lastplayerchunky = INT_MIN;
    lastchunkdist = -1;
    rebuildingworldchunks = false;
    lastworldchunkpublish = -1;
    lastworldchunkmotion = -1;
    worldchunkvelocityx = worldchunkvelocityy = 0;
    worldchunkfocusx = worldchunkfocusy = worldchunkaheadx = worldchunkaheady = worldchunkviewx = worldchunkviewy = 0;
    worldchunkvasectionmillis = 2.0f;
    worlddebugcachemillis = -1;
    ++worldchunkepoch;
}

static void copyworldcube(const cube &src, cube &dst)
{
    dst = src;
    dst.visible = 0;
    dst.merged = 0;
    dst.ext = NULL;
    if(src.children)
    {
        dst.children = newcubes(F_EMPTY);
        loopi(8) copyworldcube(src.children[i], dst.children[i]);
    }
}

static void pasteworldcube(const cube &src, cube &dst)
{
    discardchildren(dst);
    copyworldcube(src, dst);
}

static void resetworldcube(cube &c)
{
    c.children = NULL;
    c.ext = NULL;
    c.visible = 0;
    c.merged = 0;
    c.material = MAT_AIR;
    emptyfaces(c);
    loopi(6) c.texture[i] = DEFAULT_GEOM;
}

static void moveworldcube(cube &src, cube &dst)
{
    discardchildren(dst);
    dst = src;
    resetworldcube(src);
}

static void detachworldcubegeometry(cube &c)
{
    c.visible = 0;
    c.merged = 0;
    if(c.ext)
    {
        if(c.ext->va)
        {
            destroyva(c.ext->va);
            c.ext->va = NULL;
        }
        c.ext->tjoints = -1;
        freeoctaentities(c);
    }
    if(c.children) loopi(8) detachworldcubegeometry(c.children[i]);
}

static ivec worldchunkorigin(const worldchunk &chunk, int z = 0)
{
    return ivec((chunk.x - worldfirstchunkx) * WORLD_CHUNK_SIZE,
                (chunk.y - worldfirstchunky) * WORLD_CHUNK_SIZE, z);
}

static cube &lookupworldchunkcube(worldchunk &chunk, const ivec &pos, int size)
{
    int scale = WORLD_CHUNK_SCALE - 1;
    cube *c = &chunk.root[octastep(pos.x, pos.y, pos.z, scale)];
    while(!(size >> scale))
    {
        if(!c->children) subdividecube(*c);
        --scale;
        c = &c->children[octastep(pos.x, pos.y, pos.z, scale)];
    }
    return *c;
}

static const cube &lookupworldchunkcube(const worldchunk &chunk, const ivec &pos, int size)
{
    int scale = WORLD_CHUNK_SCALE - 1;
    const cube *c = &chunk.root[octastep(pos.x, pos.y, pos.z, scale)];
    while(!(size >> scale) && c->children)
    {
        --scale;
        c = &c->children[octastep(pos.x, pos.y, pos.z, scale)];
    }
    return *c;
}

static worldchunkdiffstate *findworldchunkdiffstate(int x, int y, bool create)
{
    loopv(worldchunkdiffstates)
    {
        worldchunkdiffstate *state = worldchunkdiffstates[i];
        if(state->x == x && state->y == y && state->z == WORLD_DIFF_Z) return state;
    }
    if(!create) return NULL;
    return worldchunkdiffstates.add(new worldchunkdiffstate(x, y));
}

static bool sameworlddiffnode(const worlddiffnode &a, const worlddiffnode &b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.size == b.size &&
           a.material == b.material && !memcmp(a.edges, b.edges, sizeof(a.edges)) &&
           !memcmp(a.texture, b.texture, sizeof(a.texture));
}

static bool sameworldscatterlist(const vector<worldscatterinstance> &a, const vector<worldscatterinstance> &b)
{
    if(a.length() != b.length()) return false;
    loopv(a)
    {
        bool found = false;
        loopvj(b) if(a[i] == b[j]) { found = true; break; }
        if(!found) return false;
    }
    return true;
}

static void copyworlddiffnode(const cube &c, const ivec &o, int size, const ivec &chunkorigin, worlddiffnode &node)
{
    node.x = o.x - chunkorigin.x;
    node.y = o.y - chunkorigin.y;
    node.z = o.z;
    node.size = size;
    memcpy(node.edges, c.edges, sizeof(node.edges));
    memcpy(node.texture, c.texture, sizeof(node.texture));
    node.material = c.material;
}

static void captureworlddiffnodes(const cube &c, const ivec &o, int size, const ivec &bbmin, const ivec &bbmax, const ivec &chunkorigin, vector<worlddiffnode> &nodes)
{
    ivec nodeend = ivec(o).add(size);
    if(nodeend.x <= bbmin.x || nodeend.y <= bbmin.y || nodeend.z <= bbmin.z ||
       o.x >= bbmax.x || o.y >= bbmax.y || o.z >= bbmax.z)
        return;

    bool contained = o.x >= bbmin.x && o.y >= bbmin.y && o.z >= bbmin.z &&
                     nodeend.x <= bbmax.x && nodeend.y <= bbmax.y && nodeend.z <= bbmax.z;
    if(contained && !c.children)
    {
        copyworlddiffnode(c, o, size, chunkorigin, nodes.add());
        return;
    }
    if(size <= 1)
    {
        copyworlddiffnode(c, o, size, chunkorigin, nodes.add());
        return;
    }

    int childsize = size >> 1;
    loopi(8)
    {
        ivec co(i, o, childsize);
        captureworlddiffnodes(c.children ? c.children[i] : c, co, childsize,
                              bbmin, bbmax, chunkorigin, nodes);
    }
}

static void captureworlddiffregion(const worldchunk &chunk, const ivec &bbmin, const ivec &bbmax, vector<worlddiffnode> &nodes)
{
    if(!worldroot || !worldchunkmounted(chunk)) return;
    ivec chunkorigin = worldchunkorigin(chunk),
         clipmin(max(bbmin.x, chunkorigin.x), max(bbmin.y, chunkorigin.y),
                 max(bbmin.z, 0)),
         clipmax(min(bbmax.x, chunkorigin.x + WORLD_CHUNK_SIZE),
                 min(bbmax.y, chunkorigin.y + WORLD_CHUNK_SIZE),
                 min(bbmax.z, int(WORLD_MAP_SIZE)));
    if(clipmin.x >= clipmax.x || clipmin.y >= clipmax.y || clipmin.z >= clipmax.z) return;
    int rootsize = worldsize >> 1;
    loopi(8)
        captureworlddiffnodes(worldroot[i], ivec(i, ivec(0, 0, 0), rootsize), rootsize, clipmin, clipmax, chunkorigin, nodes);
}

static bool worldscatterinregion(const worldscatterinstance &scatter, const ivec &chunkorigin, const ivec &bbmin, const ivec &bbmax)
{
    const ivec position = ivec(chunkorigin).add(
        ivec(scatter.x, scatter.y, scatter.z));
    return position.x >= bbmin.x && position.x < bbmax.x &&
           position.y >= bbmin.y && position.y < bbmax.y &&
           position.z >= bbmin.z && position.z < bbmax.z;
}

static void captureworldscatterregion(const worldchunk &chunk, const ivec &bbmin, const ivec &bbmax, vector<worldscatterinstance> &scatter)
{
    if(!worldchunkmounted(chunk)) return;
    const ivec origin = worldchunkorigin(chunk);
    loopv(chunk.scatter)
        if(worldscatterinregion(chunk.scatter[i], origin, bbmin, bbmax))
            scatter.add(chunk.scatter[i]);
}

static ivec worldorientnormal(int orient)
{
    ivec normal(0, 0, 0);
    if(orient >= O_LEFT && orient <= O_TOP)
        normal[dimension(orient)] = dimcoord(orient) ? 1 : -1;
    return normal;
}

static bool validworldscatter(const worldchunk &chunk, const worldscatterinstance &scatter)
{
    if(scatter.type < 0 || scatter.type >= numworldscatters() ||
       scatter.x < 0 || scatter.x >= WORLD_CHUNK_SIZE ||
       scatter.y < 0 || scatter.y >= WORLD_CHUNK_SIZE ||
       scatter.z < 0 || scatter.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE ||
       scatter.orient < O_LEFT || scatter.orient > O_TOP)
        return false;

    const ivec origin = worldchunkorigin(chunk);
    const ivec center = ivec(origin).add(ivec(
        scatter.x + WORLD_BLOCK_SIZE / 2,
        scatter.y + WORLD_BLOCK_SIZE / 2,
        scatter.z + WORLD_BLOCK_SIZE / 2));
    const ivec normal = worldorientnormal(scatter.orient),
               supportcenter = ivec(center).sub(
                   ivec(normal).mul(WORLD_BLOCK_SIZE));
    if(!insideworld(center) || !insideworld(supportcenter)) return false;
    ivec cubeorigin;
    int cubesize;
    const cube &occupied = lookupcube(center, 0, cubeorigin, cubesize);
    if(!isempty(occupied) || occupied.material != MAT_AIR) return false;

    const bool torch = isworldtorch(scatter.type);
    if((!torch && scatter.orient != O_TOP) ||
       (torch && scatter.orient == O_BOTTOM))
        return false;
    const cube &support = lookupcube(supportcenter, 0, cubeorigin, cubesize);
    if(isempty(support) || !isentirelysolid(support) ||
       support.material != MAT_AIR)
        return false;
    return true;
}

static void removeworldinvalidscatter(worldchunk &chunk, const ivec &bbmin, const ivec &bbmax)
{
    const ivec origin = worldchunkorigin(chunk);
    for(int i = chunk.scatter.length() - 1; i >= 0; --i)
        if(worldscatterinregion(chunk.scatter[i], origin, bbmin, bbmax) &&
           !validworldscatter(chunk, chunk.scatter[i]))
            chunk.scatter.removeunordered(i);
}

static worldeditrecord *cloneworldeditrecord(const worldeditrecord &source)
{
    worldeditrecord *copy = new worldeditrecord;
    copy->chunkx = source.chunkx;
    copy->chunky = source.chunky;
    copy->chunkz = source.chunkz;
    copy->operation = source.operation;
    copy->author = source.author;
    memcpy(copy->args, source.args, sizeof(copy->args));
    copy->revision = source.revision;
    copy->timestamp = source.timestamp;
    copy->selection = source.selection;
    loopv(source.before) copy->before.add(source.before[i]);
    loopv(source.after) copy->after.add(source.after[i]);
    loopv(source.scatterbefore) copy->scatterbefore.add(source.scatterbefore[i]);
    loopv(source.scatterafter) copy->scatterafter.add(source.scatterafter[i]);
    return copy;
}

void setworldeditauthor(int author)
{
    worldeditauthor = author;
}

void setworldeditrevision(uint revision)
{
    incomingworldeditrevision = revision;
}

void cancelworldedit()
{
    currentworldedit.clear();
}

void beginworldedit(int operation, const selinfo &selection, int arg1, int arg2, int arg3, int arg4)
{
    cancelworldedit();
    if(worldchunks.empty() || activeworldchunk < 0 || selection.s.iszero()) return;

    currentworldedit.active = true;
    currentworldedit.operation = operation;
    currentworldedit.author = worldeditauthor;
    currentworldedit.args[0] = arg1;
    currentworldedit.args[1] = arg2;
    currentworldedit.args[2] = arg3;
    currentworldedit.args[3] = arg4;
    currentworldedit.selection = selection;

    ivec bbmin = selection.o,
         bbmax = ivec(selection.s).mul(selection.grid).add(selection.o);
    const ivec scattermin = ivec(bbmin).sub(WORLD_BLOCK_SIZE),
               scattermax = ivec(bbmax).add(WORLD_BLOCK_SIZE);
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) continue;
        ivec origin = worldchunkorigin(chunk);
        if(scattermax.x <= origin.x ||
           scattermin.x >= origin.x + WORLD_CHUNK_SIZE ||
           scattermax.y <= origin.y ||
           scattermin.y >= origin.y + WORLD_CHUNK_SIZE ||
           scattermax.z <= 0 || scattermin.z >= WORLD_MAP_SIZE)
            continue;

        worldeditrecord *record = currentworldedit.records.add(new worldeditrecord);
        record->chunkx = chunk.x;
        record->chunky = chunk.y;
        record->operation = operation;
        record->author = currentworldedit.author;
        memcpy(record->args, currentworldedit.args, sizeof(record->args));
        record->selection = selection;
        captureworlddiffregion(chunk, bbmin, bbmax, record->before);
        captureworldscatterregion(chunk, scattermin, scattermax, record->scatterbefore);
    }
}

void commitworldedit()
{
    if(!currentworldedit.active) return;
    ivec bbmin = currentworldedit.selection.o,
         bbmax = ivec(currentworldedit.selection.s).mul(currentworldedit.selection.grid) .add(currentworldedit.selection.o);
    const ivec scattermin = ivec(bbmin).sub(WORLD_BLOCK_SIZE),
               scattermax = ivec(bbmax).add(WORLD_BLOCK_SIZE);
    ullong timestamp = ullong(time(NULL));
    ullong revision = incomingworldeditrevision
                    ? max(worldeditrevision, incomingworldeditrevision)
                    : worldeditrevision + 1;
    worldeditrevision = revision;
    incomingworldeditrevision = 0;
    bool scatterchanged = false;
    loopv(currentworldedit.records)
    {
        worldeditrecord *record = currentworldedit.records[i];
        int chunkindex = findworldchunk(record->chunkx, record->chunky);
        if(!worldchunks.inrange(chunkindex)) continue;
        worldchunk &chunk = worldchunks[chunkindex];
        removeworldinvalidscatter(chunk, scattermin, scattermax);
        captureworlddiffregion(chunk, bbmin, bbmax, record->after);
        captureworldscatterregion(chunk, scattermin, scattermax,
                                  record->scatterafter);

        bool identical = record->before.length() == record->after.length();
        if(identical) loopvj(record->before)
            if(!sameworlddiffnode(record->before[j], record->after[j]))
            {
                identical = false;
                break;
            }
        const bool samescatter =
            sameworldscatterlist(record->scatterbefore, record->scatterafter);
        if(!samescatter) scatterchanged = true;
        if(identical) identical = samescatter;
        if(identical) continue;

        worldchunkdiffstate *state = findworldchunkdiffstate(record->chunkx, record->chunky, true);
        record->revision = revision;
        state->revision = max(state->revision, revision);
        record->timestamp = timestamp;
        state->pending.add(cloneworldeditrecord(*record));
        state->journal.add(cloneworldeditrecord(*record));
        state->audit.add(cloneworldeditrecord(*record));
        chunk.dirty = true;
    }
    currentworldedit.clear();
    if(scatterchanged) updateworldscatterers();
}

static void worlddiffput32(vector<uchar> &out, uint value)
{
    loopi(4) out.add(uchar(value >> (i * 8)));
}

static void worlddiffput64(vector<uchar> &out, ullong value)
{
    loopi(8) out.add(uchar(value >> (i * 8)));
}

static void worlddiffputbytes(vector<uchar> &out, const void *data, int length)
{
    out.put((const uchar *)data, length);
}

static uint worlddiffchecksum(const uchar *data, int length)
{
    uint hash = 2166136261U;
    loopi(length)
    {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

static void serializeworlddiffnode(vector<uchar> &out, const worlddiffnode &node)
{
    worlddiffput32(out, uint(node.x));
    worlddiffput32(out, uint(node.y));
    worlddiffput32(out, uint(node.z));
    worlddiffput32(out, uint(node.size));
    worlddiffputbytes(out, node.edges, sizeof(node.edges));
    loopi(6)
    {
        out.add(uchar(node.texture[i]));
        out.add(uchar(node.texture[i] >> 8));
    }
    out.add(uchar(node.material));
    out.add(uchar(node.material >> 8));
}

static void serializeworldscatterinstance(vector<uchar> &out, const worldscatterinstance &scatter)
{
    const uint encodedtype = uint(scatter.type & 0xFFFF) |
                             (uint((scatter.orient + 1) & 0x7) << 16);
    worlddiffput32(out, uint(scatter.x));
    worlddiffput32(out, uint(scatter.y));
    worlddiffput32(out, uint(scatter.z));
    worlddiffput32(out, encodedtype);
}

static void serializeworldeditrecord(vector<uchar> &out, const worldeditrecord &record)
{
    vector<uchar> body;
    worlddiffput32(body, uint(record.chunkx));
    worlddiffput32(body, uint(record.chunky));
    worlddiffput32(body, uint(record.chunkz));
    worlddiffput64(body, record.revision);
    worlddiffput64(body, record.timestamp);
    worlddiffput32(body, uint(record.author));
    worlddiffput32(body, uint(record.operation));
    worlddiffput32(body, uint(record.selection.o.x));
    worlddiffput32(body, uint(record.selection.o.y));
    worlddiffput32(body, uint(record.selection.o.z));
    worlddiffput32(body, uint(record.selection.s.x));
    worlddiffput32(body, uint(record.selection.s.y));
    worlddiffput32(body, uint(record.selection.s.z));
    worlddiffput32(body, uint(record.selection.grid));
    worlddiffput32(body, uint(record.selection.orient));
    worlddiffput32(body, uint(record.selection.corner));
    loopi(4) worlddiffput32(body, uint(record.args[i]));
    worlddiffput32(body, uint(record.before.length()));
    loopv(record.before) serializeworlddiffnode(body, record.before[i]);
    worlddiffput32(body, uint(record.after.length()));
    loopv(record.after) serializeworlddiffnode(body, record.after[i]);
    worlddiffput32(body, uint(record.scatterbefore.length()));
    loopv(record.scatterbefore) serializeworldscatterinstance(body, record.scatterbefore[i]);
    worlddiffput32(body, uint(record.scatterafter.length()));
    loopv(record.scatterafter) serializeworldscatterinstance(body, record.scatterafter[i]);
    worlddiffput32(out, uint(body.length()));
    worlddiffput32(out, worlddiffchecksum(body.getbuf(), body.length()));
    worlddiffputbytes(out, body.getbuf(), body.length());
}

static void makeworlddiffframe(vector<uchar> &frame, uchar type, int chunkx, int chunky, const vector<worldeditrecord *> &records, ullong expectedhash = 0)
{
    vector<uchar> payload;
    payload.add(type);
    worlddiffput32(payload, WORLD_SAVE_FORMAT_VERSION);
    worlddiffput32(payload, WORLDGEN_VERSION);
    worlddiffput32(payload, uint(chunkx));
    worlddiffput32(payload, uint(chunky));
    worlddiffput32(payload, WORLD_DIFF_Z);
    worlddiffput64(payload, expectedhash);
    worlddiffput32(payload, uint(records.length()));
    loopv(records) serializeworldeditrecord(payload, *records[i]);

    worlddiffputbytes(frame, "CDF1", 4);
    worlddiffput32(frame, uint(payload.length()));
    worlddiffput32(frame, worlddiffchecksum(payload.getbuf(), payload.length()));
    worlddiffputbytes(frame, payload.getbuf(), payload.length());
}

struct worlddiffflushjob
{
    string filename, auditfilename;
    int chunkx, chunky;
    vector<worldeditrecord *> records;

    worlddiffflushjob() : chunkx(0), chunky(0) {}
    ~worlddiffflushjob() { records.deletecontents(); }
};

static vector<worlddiffflushjob *> worlddiffflushjobs;
static SDL_mutex *worlddiffwritermutex = NULL;
static SDL_cond *worlddiffwritercond = NULL;
static SDL_Thread *worlddiffwriterthread = NULL;
static bool stopworlddiffwriter = false;

static bool appendworlddiffbytes(const char *filename, const vector<uchar> &bytes)
{
    stream *file = openrawfile(filename, "ab");
    if(!file) return false;
    bool written = file->write(bytes.getbuf(), bytes.length()) == size_t(bytes.length());
    delete file;
    return written;
}

static int worlddiffwriter(void *)
{
#ifdef TRACY_ENABLE
    tracy::SetThreadName("World diff writer");
#endif
    for(;;)
    {
        SDL_LockMutex(worlddiffwritermutex);
        while(worlddiffflushjobs.empty() && !stopworlddiffwriter)
            SDL_CondWait(worlddiffwritercond, worlddiffwritermutex);
        if(worlddiffflushjobs.empty() && stopworlddiffwriter)
        {
            SDL_UnlockMutex(worlddiffwritermutex);
            break;
        }
        worlddiffflushjob *job = worlddiffflushjobs.remove(0);
        SDL_UnlockMutex(worlddiffwritermutex);

        vector<uchar> frame;
        makeworlddiffframe(frame, 2, job->chunkx, job->chunky, job->records);
        if(!appendworlddiffbytes(job->filename, frame))
            conoutf(CON_ERROR, "could not append chunk diff journal %s", job->filename);
        if(!appendworlddiffbytes(job->auditfilename, frame))
            conoutf(CON_ERROR, "could not append world audit journal %s", job->auditfilename);
        delete job;
    }
    return 0;
}

static bool startworlddiffwriter()
{
    if(worlddiffwriterthread) return true;
    worlddiffwritermutex = SDL_CreateMutex();
    worlddiffwritercond = SDL_CreateCond();
    stopworlddiffwriter = false;
    if(!worlddiffwritermutex || !worlddiffwritercond) return false;
    worlddiffwriterthread = SDL_CreateThread(worlddiffwriter, "world diff writer", NULL);
    return worlddiffwriterthread != NULL;
}

static void queueworlddiffflush(worldchunkdiffstate &state, vector<worldeditrecord *> &records)
{
    if(records.empty() || !startworlddiffwriter()) return;
    worlddiffflushjob *job = new worlddiffflushjob;
    defformatstring(relative, "media/map/%s/chunks/%d_%d_%d.diff",
                    worldfolder, state.x, state.y, state.z);
    copystring(job->filename, path(relative));
    defformatstring(auditrelative, "media/map/%s/audit.log", worldfolder);
    copystring(job->auditfilename, path(auditrelative));
    job->chunkx = state.x;
    job->chunky = state.y;
    job->records.move(records);

    SDL_LockMutex(worlddiffwritermutex);
    worlddiffflushjobs.add(job);
    SDL_CondSignal(worlddiffwritercond);
    SDL_UnlockMutex(worlddiffwritermutex);
}

static void flushworlddiffjournals(bool force)
{
    if(worldfolder[0] == '\0') return;
    if(!force && totalmillis - lastworlddiffflush < WORLD_DIFF_FLUSH_MILLIS) return;
    lastworlddiffflush = totalmillis;
    loopv(worldchunkdiffstates)
    {
        worldchunkdiffstate &state = *worldchunkdiffstates[i];
        if(state.pending.empty()) continue;
        vector<worldeditrecord *> flush;
        if(worlddiffwritermutex) SDL_LockMutex(worlddiffwritermutex);
        flush.move(state.pending);
        if(worlddiffwritermutex) SDL_UnlockMutex(worlddiffwritermutex);
        queueworlddiffflush(state, flush);
        flush.deletecontents();
    }
}

static void shutdownworlddiffwriter()
{
    if(!worlddiffwriterthread) return;
    SDL_LockMutex(worlddiffwritermutex);
    stopworlddiffwriter = true;
    SDL_CondBroadcast(worlddiffwritercond);
    SDL_UnlockMutex(worlddiffwritermutex);
    SDL_WaitThread(worlddiffwriterthread, NULL);
    worlddiffwriterthread = NULL;
    worlddiffflushjobs.deletecontents();
    SDL_DestroyCond(worlddiffwritercond);
    SDL_DestroyMutex(worlddiffwritermutex);
    worlddiffwritercond = NULL;
    worlddiffwritermutex = NULL;
    stopworlddiffwriter = false;
}

static bool worldcubehascontent(const cube &c)
{
    if(c.children)
    {
        loopi(8) if(worldcubehascontent(c.children[i])) return true;
        return false;
    }
    return !isempty(c) || c.material != MAT_AIR;
}

static bool worldchunksectionhascontent(worldchunk &chunk, int tile, int section)
{
    const uint tilebit = 1U << tile;
    if(chunk.contentknown[section] & tilebit)
        return (chunk.contenttiles[section] & tilebit) != 0;
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE,
             section * WORLD_SECTION_SIZE);
    bool content;
    if(chunk.mountedtiles[section] & tilebit)
    {
        ivec actualorigin;
        int actualsize;
        content = worldcubehascontent(
            lookupcube(ivec(worldchunkorigin(chunk)).add(pos), -WORLD_SECTION_SIZE,
                       actualorigin, actualsize));
    }
    else content = worldcubehascontent(
        lookupworldchunkcube(static_cast<const worldchunk &>(chunk),
                             pos, WORLD_SECTION_SIZE));
    chunk.contentknown[section] |= tilebit;
    if(content) chunk.contenttiles[section] |= tilebit;
    else chunk.contenttiles[section] &= ~tilebit;
    return content;
}

static void setworldchunksectioncontent(worldchunk &chunk, int tile, int section, bool content)
{
    const uint tilebit = 1U << tile;
    chunk.contentknown[section] |= tilebit;
    if(content) chunk.contenttiles[section] |= tilebit;
    else chunk.contenttiles[section] &= ~tilebit;
}

static int worldchunksurfacesection(worldchunk &chunk, int tile)
{
    if(chunk.surfacesections[tile] >= -1) return chunk.surfacesections[tile];
    for(int section = WORLD_SECTION_LAYERS - 1; section >= 0; --section)
    {
        if(!worldchunksectionhascontent(chunk, tile, section)) continue;
        chunk.surfacesections[tile] = section;
        return section;
    }
    chunk.surfacesections[tile] = -1;
    return -1;
}

static bool worldchunkmounted(const worldchunk &chunk)
{
    loopi(WORLD_SECTION_LAYERS) if(chunk.mountedtiles[i]) return true;
    return false;
}

static bool worldchunkfullymounted(const worldchunk &chunk)
{
    const uint alltiles = (1U << WORLD_SECTION_TILES) - 1;
    loopi(WORLD_SECTION_LAYERS) if(chunk.mountedtiles[i] != alltiles) return false;
    return true;
}

void markworldchunksdirty(const ivec &bbmin, const ivec &bbmax)
{
    if(suppressworldchunkdirty || worldchunks.empty()) return;
    invalidateworldskyexposure(bbmin, bbmax);
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(!worldchunkmounted(chunk)) continue;
        ivec origin = worldchunkorigin(chunk);
        if(bbmax.x <= origin.x || bbmin.x >= origin.x + WORLD_CHUNK_SIZE ||
           bbmax.y <= origin.y || bbmin.y >= origin.y + WORLD_CHUNK_SIZE ||
           bbmax.z <= 0 || bbmin.z >= WORLD_MAP_SIZE)
            continue;
        chunk.dirty = true;
    }
}

static bool syncmountedworldchunk(worldchunk &chunk)
{
    if(!worldchunkmounted(chunk) || !chunk.root || !worldroot) return !chunk.corrupted;
    ZoneScopedN("Chunks/Sync mounted chunk");
    ZoneTextF("%d_%d", chunk.x, chunk.y);
    bool valid = !chunk.corrupted;
    loopi(WORLD_SECTION_LAYERS) if(chunk.mountedtiles[i]) loopj(WORLD_SECTION_TILES)
    {
        if(!(chunk.mountedtiles[i] & (1U << j))) continue;
        int x = j % WORLD_SECTION_COLUMNS, y = j / WORLD_SECTION_COLUMNS;
        ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, i * WORLD_SECTION_SIZE);
        ivec runtimepos = ivec(worldchunkorigin(chunk)).add(pos);
        int key = worldchunkvaupdatekey(runtimepos);
        worldsectionowner *owner = worldsectionowners.access(key);
        if(!owner || !owner->matches(chunk, i, j))
        {
            conoutf(CON_ERROR, "refusing to sync chunk %d_%d section %d:%d: runtime ownership mismatch",
                    chunk.x, chunk.y, i, j);
            chunk.corrupted = true;
            valid = false;
            continue;
        }
        pasteworldcube(lookupcube(runtimepos, WORLD_SECTION_SIZE),
                       lookupworldchunkcube(chunk, pos, WORLD_SECTION_SIZE));
    }
    return valid;
}

static bool mountworldchunktile(worldchunk &chunk, int section, int tile)
{
    const uint tilebit = 1U << tile;
    if(chunk.mountedtiles[section] & tilebit) return false;
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE);
    ivec runtimepos = ivec(worldchunkorigin(chunk)).add(pos);
    int key = worldchunkvaupdatekey(runtimepos);
    worldsectionowner *owner = worldsectionowners.access(key);
    if(owner)
    {
        if(owner->matches(chunk, section, tile))
        {
            chunk.mountedtiles[section] |= tilebit;
            return false;
        }
        conoutf(CON_ERROR,
                "refusing to mount chunk %d_%d section %d:%d over chunk %d_%d section %d:%d",
                chunk.x, chunk.y, section, tile, owner->chunkx, owner->chunky,
                int(owner->section), int(owner->tile));
        chunk.corrupted = true;
        return false;
    }
    moveworldcube(lookupworldchunkcube(chunk, pos, WORLD_SECTION_SIZE),
                  lookupcube(runtimepos, WORLD_SECTION_SIZE));
    invalidateworldskyexposure(runtimepos, ivec(runtimepos).add(WORLD_SECTION_SIZE));
    worldsectionowners[key] = worldsectionowner(chunk.x, chunk.y, section, tile);
    chunk.mountedtiles[section] |= tilebit;
    return true;
}

static bool unmountworldchunktile(worldchunk &chunk, int section, int tile)
{
    const uint tilebit = 1U << tile;
    if(!(chunk.mountedtiles[section] & tilebit)) return false;
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE);
    ivec runtimepos = ivec(worldchunkorigin(chunk)).add(pos);
    int key = worldchunkvaupdatekey(runtimepos);
    worldsectionowner *owner = worldsectionowners.access(key);
    if(!owner || !owner->matches(chunk, section, tile))
    {
        conoutf(CON_ERROR, "refusing to unmount chunk %d_%d section %d:%d: runtime ownership mismatch",
                chunk.x, chunk.y, section, tile);
        chunk.corrupted = true;
        chunk.mountedtiles[section] &= ~tilebit;
        return false;
    }
    cube &c = lookupcube(runtimepos, WORLD_SECTION_SIZE);
    setworldchunksectioncontent(chunk, tile, section, worldcubehascontent(c));
    chunk.surfacesections[tile] = -2;
    if(chunk.dirty) loopv(worldchunks) memclear(worldchunks[i].exposureknown);
    invalidateworldskyexposure(runtimepos, ivec(runtimepos).add(WORLD_SECTION_SIZE));
    detachworldcubegeometry(c);
    moveworldcube(c, lookupworldchunkcube(chunk, pos, WORLD_SECTION_SIZE));
    worldsectionowners.remove(key);
    chunk.mountedtiles[section] &= ~tilebit;
    return true;
}

static void unmountworldchunk(worldchunk &chunk)
{
    if(!worldchunkmounted(chunk)) return;
    ZoneScopedN("Chunks/Unmount");
    ZoneTextF("%d_%d", chunk.x, chunk.y);
    loopi(WORLD_SECTION_LAYERS) loopj(WORLD_SECTION_TILES)
        unmountworldchunktile(chunk, i, j);
}

static bool worldchunkcolumnmounted(const worldchunk &chunk, int tile)
{
    const uint tilebit = 1U << tile;
    loopi(WORLD_SECTION_LAYERS) if(!(chunk.mountedtiles[i] & tilebit)) return false;
    return true;
}

static int unmountworldchunkcolumnbatch(worldchunk &chunk, int tile, int *sections, int maxsections)
{
    ZoneScopedN("Chunks/Unmount column sections");
    ZoneTextF("%d_%d tile %d", chunk.x, chunk.y, tile);
    const uint tilebit = 1U << tile;
    const int playersection = clamp(player ? int(player->o.z) / WORLD_SECTION_SIZE
                                           : WORLD_SECTION_LAYERS / 2,
                                    0, int(WORLD_SECTION_LAYERS) - 1);
    int unmounted = 0;
    while(unmounted < maxsections)
    {
        int best = -1, bestdist = -1;
        loopi(WORLD_SECTION_LAYERS)
        {
            if(!(chunk.mountedtiles[i] & tilebit)) continue;
            int dist = abs(i - playersection);
            if(dist <= bestdist) continue;
            best = i;
            bestdist = dist;
        }
        if(best < 0 || !unmountworldchunktile(chunk, best, tile)) break;
        sections[unmounted++] = best;
    }
    ZoneValue(unmounted);
    return unmounted;
}

static int worldchunkvaupdatekey(const ivec &origin)
{
    const int rowsize = WORLD_RUNTIME_SIZE / WORLD_SECTION_SIZE;
    return ((origin.z / WORLD_SECTION_SIZE) * rowsize
          + origin.y / WORLD_SECTION_SIZE) * rowsize
          + origin.x / WORLD_SECTION_SIZE;
}

static bool queueworldchunkvaupdate(const ivec &origin)
{
    int key = worldchunkvaupdatekey(origin);
    if(worldchunkvaupdateset.access(key)) return false;
    worldchunkvaupdateset.add(key);
    worldchunkvaupdates.add(key);
    TracyPlot("Chunks/Pending VA sections", int64_t(worldchunkvaupdates.length()));
    return true;
}

static void queueworldchunksectionupdates(const worldchunk &chunk, int tile, const int *sections, int numsections)
{
    ZoneScopedN("Chunks/Queue affected VA sections");
    static const int offsets[][3] =
    {
        { 0, 0, 0 },
        { -1, 0, 0 }, { 1, 0, 0 },
        { 0, -1, 0 }, { 0, 1, 0 },
        { 0, 0, -1 }, { 0, 0, 1 }
    };
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec bbmins[WORLD_MAX_SECTION_REGIONS], bbmaxs[WORLD_MAX_SECTION_REGIONS];
    int numregions = 0;
    loopi(numsections)
    {
        ivec center = worldchunkorigin(chunk, sections[i] * WORLD_SECTION_SIZE);
        center.add(ivec(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, 0));
        loopj(int(sizeof(offsets)/sizeof(offsets[0])))
        {
            ivec bbmin = ivec(center).add(ivec(offsets[j][0] * WORLD_SECTION_SIZE,
                                               offsets[j][1] * WORLD_SECTION_SIZE,
                                               offsets[j][2] * WORLD_SECTION_SIZE));
            ivec bbmax = ivec(bbmin).add(WORLD_SECTION_SIZE);
            if(bbmin.x < 0 || bbmin.y < 0 || bbmin.z < 0 ||
               bbmax.x > worldsize || bbmax.y > worldsize || bbmax.z > WORLD_MAP_SIZE)
                continue;
            if(!queueworldchunkvaupdate(bbmin)) continue;
            bbmins[numregions] = bbmin;
            bbmaxs[numregions] = bbmax;
            numregions++;
        }
    }
    if(numregions)
    {
        // Runtime cubes have already moved. Their old parent VAs must be
        // destroyed before another frame can draw them at stale coordinates.
        // Building replacement VAs remains grouped in processworldchunkvaupdates().
        bool oldsuppress = suppressworldchunkdirty;
        suppressworldchunkdirty = true;
        changedstreaming(bbmins, bbmaxs, numregions, false);
        suppressworldchunkdirty = oldsuppress;
    }
    ZoneValue(numsections);
}

static int findworldchunk(int x, int y)
{
    loopv(worldchunks) if(worldchunks[i].x == x && worldchunks[i].y == y) return i;
    return -1;
}

static int worldchunkdistance(int x, int y, int focusx, int focusy)
{
    long long dx = (long long)x - focusx, dy = (long long)y - focusy;
    if(dx < 0) dx = -dx;
    if(dy < 0) dy = -dy;
    return int(min(max(dx, dy), (long long)INT_MAX));
}

static bool worldchunkinview(const worldchunk &chunk, int chunkx, int chunky)
{
    return worldchunkdistance(chunk.x, chunk.y, chunkx, chunky) <= maxchunkdist;
}

static bool worldchunkjobwanted(int x, int y, int chunkx, int chunky, int aheadx, int aheady)
{
    (void)aheadx;
    (void)aheady;
    return worldchunkdistance(x, y, chunkx, chunky) <= maxchunkdist;
}

static void worldchunkviewfocus(int chunkx, int chunky, int &viewx, int &viewy)
{
    float dominant = max(fabsf(camdir.x), fabsf(camdir.y));
    if(!camera1 || dominant < 0.05f)
    {
        viewx = chunkx;
        viewy = chunky;
        return;
    }
    int reach = min(maxchunkdist, 6);
    viewx = chunkx + int(roundf(camdir.x / dominant * reach));
    viewy = chunky + int(roundf(camdir.y / dominant * reach));
}

static int worldchunkcoordinatescore(int x, int y)
{
    int currentdist = worldchunkdistance(x, y, worldchunkfocusx, worldchunkfocusy),
        aheaddist = worldchunkdistance(x, y, worldchunkaheadx, worldchunkaheady),
        viewdist = worldchunkdistance(x, y, worldchunkviewx, worldchunkviewy);
    long long dx = (long long)x - worldchunkaheadx,
              dy = (long long)y - worldchunkaheady,
              urgent = currentdist <= 1 ? 0 : 0x10000000LL,
              score = urgent + (long long)currentdist * 0x200000
                    + (long long)viewdist * 0x20000
                    + (long long)aheaddist * 0x1000 + dx * dx + dy * dy;
    return int(min(score, (long long)INT_MAX));
}

static int worldchunkjobscore(const worldchunkjob &job)
{
    long long score = worldchunkcoordinatescore(job.x, job.y);
    // Disk hits are normally much faster than generation. Prefer one only
    // within the same spatial band so nearby collision terrain still wins.
    if(job.filename[0]) score = max(score - 0x1000, 0LL);
    return int(min(score, (long long)INT_MAX));
}

static int worldchunkloader(void *)
{
#ifdef TRACY_ENABLE
    tracy::SetThreadName("World chunk loader");
#endif
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for(;;)
    {
        SDL_LockMutex(worldchunkmutex);
        while(worldchunkjobs.empty() && !stopworldchunkthread)
            SDL_CondWait(worldchunkcond, worldchunkmutex);
        if(stopworldchunkthread)
        {
            SDL_UnlockMutex(worldchunkmutex);
            return 0;
        }
        worldchunkjob *job = NULL;
        {
            ZoneScopedN("Chunks/Worker select job");
            ZoneValue(worldchunkjobs.length());
            int best = 0, bestscore = worldchunkjobscore(*worldchunkjobs[0]);
            loopv(worldchunkjobs) if(i)
            {
                int score = worldchunkjobscore(*worldchunkjobs[i]);
                if(score < bestscore) { best = i; bestscore = score; }
            }
            job = worldchunkjobs.remove(best);
            worldchunkactivejobs.add(job);
            TracyPlot("Chunks/Queued jobs", int64_t(worldchunkjobs.length()));
            TracyPlot("Chunks/Active workers", int64_t(worldchunkactivejobs.length()));
        }
        SDL_UnlockMutex(worldchunkmutex);

        {
            ZoneScopedN("Chunks/Worker job");
            ZoneTextF("%d_%d", job->x, job->y);
            if(!SDL_AtomicGet(&job->cancelled)) job->root = prepareworldchunk(*job);
        }
        if(SDL_AtomicGet(&job->cancelled) && job->root)
        {
            ZoneScopedN("Chunks/Worker discard cancelled");
            freepreparedworldchunk(job->root);
            job->root = NULL;
        }

        SDL_LockMutex(worldchunkmutex);
        worldchunkactivejobs.removeobj(job);
        TracyPlot("Chunks/Active workers", int64_t(worldchunkactivejobs.length()));
        while(worldchunkresults.length() >= WORLD_MAX_PREPARED_CHUNKS && !stopworldchunkthread)
            SDL_CondWait(worldchunkcond, worldchunkmutex);
        if(stopworldchunkthread)
        {
            SDL_UnlockMutex(worldchunkmutex);
            {
                ZoneScopedN("Chunks/Worker discard on shutdown");
                freepreparedworldchunk(job->root);
            }
            delete job;
            return 0;
        }
        {
            ZoneScopedN("Chunks/Worker enqueue result");
            worldchunkresults.add(job);
            TracyPlot("Chunks/Ready results", int64_t(worldchunkresults.length()));
        }
        SDL_UnlockMutex(worldchunkmutex);
    }
}

static bool startworldchunkloader()
{
    if(!worldchunkworkers.empty()) return true;
    ZoneScopedN("Chunks/Start worker pool");
    worldchunkmutex = SDL_CreateMutex();
    worldchunkcond = SDL_CreateCond();
    stopworldchunkthread = false;
    if(!worldchunkmutex || !worldchunkcond)
    {
        if(worldchunkcond) SDL_DestroyCond(worldchunkcond);
        if(worldchunkmutex) SDL_DestroyMutex(worldchunkmutex);
        worldchunkcond = NULL;
        worldchunkmutex = NULL;
        return false;
    }

    // Procedural generation is both compute and memory intensive. Using every
    // logical CPU starves the render thread even though the workers have low
    // scheduler priority, so automatic mode leaves a core free and avoids
    // saturating the memory subsystem on high-core-count machines.
    int workers = chunkthreads > 0 ? chunkthreads : min(max(numcpus - 1, 1), 4);
    loopi(workers)
    {
        SDL_Thread *worker = SDL_CreateThread(worldchunkloader, "world chunk loader", NULL);
        if(!worker) break;
        worldchunkworkers.add(worker);
    }
    if(worldchunkworkers.empty())
    {
        SDL_DestroyCond(worldchunkcond);
        SDL_DestroyMutex(worldchunkmutex);
        worldchunkcond = NULL;
        worldchunkmutex = NULL;
        return false;
    }
    conoutf(CON_DEBUG, "started %d low-priority world chunk workers (numcpus %d)",
            worldchunkworkers.length(), numcpus);
    return true;
}

static void shutdownworldchunkloader()
{
    ZoneScopedN("Chunks/Shutdown worker pool");
    if(!worldchunkworkers.empty())
    {
        SDL_LockMutex(worldchunkmutex);
        stopworldchunkthread = true;
        loopv(worldchunkactivejobs) SDL_AtomicSet(&worldchunkactivejobs[i]->cancelled, 1);
        SDL_CondBroadcast(worldchunkcond);
        SDL_UnlockMutex(worldchunkmutex);
        {
            ZoneScopedN("Chunks/Join worker threads");
            loopv(worldchunkworkers) SDL_WaitThread(worldchunkworkers[i], NULL);
        }
        worldchunkworkers.setsize(0);
    }

    loopv(worldchunkjobs) delete worldchunkjobs[i];
    worldchunkjobs.setsize(0);
    ASSERT(worldchunkactivejobs.empty());
    loopv(worldchunkresults)
    {
        {
            ZoneScopedN("Chunks/Free queued result");
            ZoneTextF("%d_%d", worldchunkresults[i]->x, worldchunkresults[i]->y);
            freepreparedworldchunk(worldchunkresults[i]->root);
        }
        delete worldchunkresults[i];
    }
    worldchunkresults.setsize(0);

    if(worldchunkcond) SDL_DestroyCond(worldchunkcond);
    if(worldchunkmutex) SDL_DestroyMutex(worldchunkmutex);
    worldchunkcond = NULL;
    worldchunkmutex = NULL;
    stopworldchunkthread = false;
}

static int acquireworldchunksync(int x, int y, int &generated)
{
    int index = findworldchunk(x, y);
    if(index >= 0) return index;

    ZoneScopedN("Chunks/Load synchronous");
    ZoneTextF("%d_%d", x, y);
    defformatstring(diffname, "media/map/%s/chunks/%d_%d_%d.diff",
                    worldfolder, x, y, WORLD_DIFF_Z);
    path(diffname);
    const char *found = findfile(diffname, "rb");
    string diffpath;
    diffpath[0] = '\0';
    if(found && fileexists(found, "r")) copystring(diffpath, diffname);
    cube *root = generateworldchunk(x, y);
    vector<worldscatterinstance> scatter;
    generateworldscatter(root, x, y, game::worldsettings(), scatter);
    bool loaded = diffpath[0] != '\0';
    if(root && loaded)
    {
        int families = 0;
        ullong revision = 0, canonicalhash = 0;
        applyworldchunkdiff(root, x, y, diffpath, scatter, false, families,
                            revision, canonicalhash);
        if(chunkremip) remipworldchunk(root, false, families);
        worldchunkdiffstate *state = findworldchunkdiffstate(x, y, true);
        state->revision = revision;
        worldeditrevision = max(worldeditrevision, revision);
        state->canonicalhash = hashworldchunk(root);
    }
    else generated++;
    worldchunk &chunk = worldchunks.add(worldchunk(x, y, root, false, loaded));
    chunk.scatter.move(scatter);
    return worldchunks.length() - 1;
}

static void loadinitialworldchunks(int chunkx, int chunky)
{
    ZoneScopedN("Chunks/Load initial chunks");
    ZoneTextF("%d_%d", chunkx, chunky);
    static const int offsets[][2] =
    {
        { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 },
        { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 }
    };
    // Only the entry chunk blocks map startup. Everything around it uses the
    // same asynchronous path as runtime streaming.
    int target = 1,
        ready = findworldchunk(chunkx, chunky) >= 0 ? 1 : 0, generated = 0;
    renderprogress(target > 0 ? ready / float(target) : 1, "loading nearby chunks...");
    loopi(sizeof(offsets) / sizeof(offsets[0]))
    {
        if(ready >= target) break;
        int x = chunkx + offsets[i][0], y = chunky + offsets[i][1];
        if(abs(offsets[i][0]) > maxchunkdist || abs(offsets[i][1]) > maxchunkdist ||
           findworldchunk(x, y) >= 0)
            continue;
        acquireworldchunksync(x, y, generated);
        ready++;
        renderprogress(ready / float(target), "loading nearby chunks...");
    }
}

static int queueworldchunk(int x, int y)
{
    ZoneScopedN("Chunks/Queue chunk");
    ZoneTextF("%d_%d", x, y);
    int index = findworldchunk(x, y);
    if(index >= 0) return index;

    if(!startworldchunkloader())
    {
        int generated = 0;
        return acquireworldchunksync(x, y, generated);
    }

    uint request = ++worldchunkrequest;
    if(!request) request = ++worldchunkrequest;
    worldchunkjob *job = new worldchunkjob(x, y, worldchunkepoch, request);
    defformatstring(chunkfile, "media/map/%s/chunks/%d_%d_%d.diff",
                    worldfolder, x, y, WORLD_DIFF_Z);
    path(chunkfile);
    const char *found = findfile(chunkfile, "rb");
    if(found && fileexists(found, "r"))
        copystring(job->filename, chunkfile);

    worldchunk &chunk = worldchunks.add(worldchunk(x, y, NULL, true));
    chunk.request = request;
    chunk.generating = !job->filename[0];
    SDL_LockMutex(worldchunkmutex);
    worldchunkjobs.add(job);
    TracyPlot("Chunks/Queued jobs", int64_t(worldchunkjobs.length()));
    SDL_CondSignal(worldchunkcond);
    SDL_UnlockMutex(worldchunkmutex);
    return worldchunks.length() - 1;
}

static int worldchunkoutstandingjobs()
{
    if(!worldchunkmutex) return 0;
    SDL_LockMutex(worldchunkmutex);
    int outstanding = worldchunkjobs.length() + worldchunkactivejobs.length() + worldchunkresults.length();
    SDL_UnlockMutex(worldchunkmutex);
    return outstanding;
}

static int queueworldchunkview(int chunkx, int chunky, int aheadx, int aheady)
{
    ZoneScopedN("Chunks/Fill load queue");
    ZoneTextF("focus %d_%d ahead %d_%d", chunkx, chunky, aheadx, aheady);
    if(!startworldchunkloader()) return 0;
    int viewx, viewy;
    worldchunkviewfocus(chunkx, chunky, viewx, viewy);
    SDL_LockMutex(worldchunkmutex);
    worldchunkfocusx = chunkx;
    worldchunkfocusy = chunky;
    worldchunkaheadx = aheadx;
    worldchunkaheady = aheady;
    worldchunkviewx = viewx;
    worldchunkviewy = viewy;
    SDL_UnlockMutex(worldchunkmutex);

    int queued = 0, outstanding = worldchunkoutstandingjobs(),
        minx = chunkx - maxchunkdist,
        maxx = chunkx + maxchunkdist,
        miny = chunky - maxchunkdist,
        maxy = chunky + maxchunkdist;
    while(outstanding < chunkpendinglimit)
    {
        int bestx = 0, besty = 0, bestscore = INT_MAX;
        bool found = false;
        for(int y = miny; y <= maxy; ++y) for(int x = minx; x <= maxx; ++x)
        {
            if(!worldchunkjobwanted(x, y, chunkx, chunky, aheadx, aheady) ||
               findworldchunk(x, y) >= 0)
                continue;
            int score = worldchunkcoordinatescore(x, y);
            if(score >= bestscore) continue;
            bestx = x;
            besty = y;
            bestscore = score;
            found = true;
        }
        if(!found || queueworldchunk(bestx, besty) < 0) break;
        queued++;
        outstanding++;
    }
    return queued;
}

static int reprioritizeworldchunkqueue(int chunkx, int chunky, int aheadx, int aheady)
{
    ZoneScopedN("Chunks/Reprioritize queue");
    ZoneTextF("focus %d_%d ahead %d_%d", chunkx, chunky, aheadx, aheady);
    int viewx, viewy;
    worldchunkviewfocus(chunkx, chunky, viewx, viewy);
    if(!worldchunkmutex)
    {
        worldchunkfocusx = chunkx;
        worldchunkfocusy = chunky;
        worldchunkaheadx = aheadx;
        worldchunkaheady = aheady;
        worldchunkviewx = viewx;
        worldchunkviewy = viewy;
        return 0;
    }

    int cancelled = 0;
    vector<worldchunkjob *> stale;
    SDL_LockMutex(worldchunkmutex);
    worldchunkfocusx = chunkx;
    worldchunkfocusy = chunky;
    worldchunkaheadx = aheadx;
    worldchunkaheady = aheady;
    worldchunkviewx = viewx;
    worldchunkviewy = viewy;
    for(int i = worldchunkjobs.length() - 1; i >= 0; --i)
    {
        worldchunkjob *job = worldchunkjobs[i];
        if(worldchunkjobwanted(job->x, job->y, chunkx, chunky, aheadx, aheady)) continue;
        delete worldchunkjobs.remove(i);
        cancelled++;
    }
    loopv(worldchunkactivejobs)
    {
        worldchunkjob *job = worldchunkactivejobs[i];
        if(worldchunkjobwanted(job->x, job->y, chunkx, chunky, aheadx, aheady)) continue;
        if(!SDL_AtomicGet(&job->cancelled))
        {
            SDL_AtomicSet(&job->cancelled, 1);
            cancelled++;
        }
    }
    for(int i = worldchunkresults.length() - 1; i >= 0; --i)
    {
        worldchunkjob *job = worldchunkresults[i];
        if(worldchunkjobwanted(job->x, job->y, chunkx, chunky, aheadx, aheady)) continue;
        SDL_AtomicSet(&job->cancelled, 1);
        stale.add(worldchunkresults.remove(i));
        cancelled++;
    }
    if(!stale.empty()) SDL_CondBroadcast(worldchunkcond);
    SDL_UnlockMutex(worldchunkmutex);

    loopv(stale)
    {
        {
            ZoneScopedN("Chunks/Free stale result");
            ZoneTextF("%d_%d", stale[i]->x, stale[i]->y);
            freepreparedworldchunk(stale[i]->root);
        }
        delete stale[i];
    }

    // A job already owned by a worker cannot be cancelled safely. Removing
    // its placeholder makes its eventual result self-discard instead of
    // publishing terrain that the camera has already outrun.
    for(int i = worldchunks.length() - 1; i >= 0; --i)
    {
        worldchunk &chunk = worldchunks[i];
        if(!chunk.loading ||
           worldchunkjobwanted(chunk.x, chunk.y, chunkx, chunky, aheadx, aheady))
            continue;
        worldchunks.removeunordered(i);
    }
    return cancelled;
}

static int processworldchunkresults()
{
    if(worldchunkworkers.empty()) return 0;
    ZoneScopedN("Chunks/Process worker results");

    int handled = 0, published = 0, loaded = 0, generated = 0, optimized = 0;
    while(handled < asyncchunkloads)
    {
        worldchunkjob *job = NULL;
        {
            ZoneScopedN("Chunks/Dequeue worker result");
            SDL_LockMutex(worldchunkmutex);
            ZoneValue(worldchunkresults.length());
            if(!worldchunkresults.empty())
            {
                int best = -1, bestscore = INT_MAX;
                loopv(worldchunkresults)
                {
                    if(SDL_AtomicGet(&worldchunkresults[i]->cancelled) || !worldchunkresults[i]->root)
                    {
                        best = i;
                        break;
                    }
                    int score = worldchunkjobscore(*worldchunkresults[i]);
                    if(score < bestscore) { best = i; bestscore = score; }
                }
                if(best >= 0) job = worldchunkresults.remove(best);
            }
            TracyPlot("Chunks/Ready results", int64_t(worldchunkresults.length()));
            if(job) SDL_CondSignal(worldchunkcond);
            SDL_UnlockMutex(worldchunkmutex);
        }
        if(!job) break;

        int index = findworldchunk(job->x, job->y);
        bool current = index >= 0 && worldchunks[index].loading &&
                       worldchunks[index].request == job->request;
        if(job->epoch != worldchunkepoch || SDL_AtomicGet(&job->cancelled) ||
           !job->root || !current)
        {
            ZoneScopedN("Chunks/Discard worker result");
            ZoneTextF("%d_%d", job->x, job->y);
            if(current)
                worldchunks.removeunordered(index);
            freepreparedworldchunk(job->root);
            delete job;
            continue;
        }
        handled++;

        {
            ZoneScopedN("Chunks/Publish worker result");
            ZoneTextF("%d_%d %s families %d", job->x, job->y,
                      job->loaded ? "disk" : "generated", job->families);
            ZoneValue(job->families);
            worldchunk &chunk = worldchunks[index];
            chunk.root = job->root;
            chunk.scatter.move(job->scatter);
            setworldleavesalpha(chunk.root, leavesalpha != 0);
            chunk.loading = false;
            chunk.saved = job->loaded;
            chunk.dirty = false;
            worldchunkdiffstate *diffstate = findworldchunkdiffstate(chunk.x, chunk.y, true);
            diffstate->revision = max(diffstate->revision, job->revision);
            worldeditrevision = max(worldeditrevision, job->revision);
            diffstate->canonicalhash = job->canonicalhash;
            allocnodes += job->families;
            if(!job->loaded && job->filename[0])
                conoutf(CON_WARN, "asynchronous load of chunk %d_%d failed at stage %d; regenerated it",
                        job->x, job->y, job->loaderror);
            if(job->loaded) loaded++; else generated++;
            optimized += job->optimized;
            published++;
        }
        delete job;
    }

    if(published)
        conoutf(CON_DEBUG, "prepared %d chunks asynchronously (%d loaded, %d generated, %d octree families remipped)",
                published, loaded, generated, optimized);
    return published;
}

static bool worldchunksectionnearplayer(const worldchunk &chunk, int tile, int section, int radius)
{
    if(!player && !camera1) return false;
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec origin = ivec(worldchunkorigin(chunk)).add(
        ivec(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE,
             section * WORLD_SECTION_SIZE));
    const vec &focus = player ? player->o : camera1->o;
    int sectionx = origin.x / WORLD_SECTION_SIZE,
        sectiony = origin.y / WORLD_SECTION_SIZE,
        focusx = int(floorf(focus.x / WORLD_SECTION_SIZE)),
        focusy = int(floorf(focus.y / WORLD_SECTION_SIZE)),
        focusz = clamp(int(floorf(focus.z / WORLD_SECTION_SIZE)),
                       0, int(WORLD_SECTION_LAYERS) - 1);
    return abs(sectionx - focusx) <= radius && abs(sectiony - focusy) <= radius &&
           abs(section - focusz) <= radius;
}

static bool worldchunksectionexposed(worldchunk &chunk, int tile, int section)
{
    static const int offsets[][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    const uint tilebit = 1U << tile;
    if(chunk.exposureknown[section] & tilebit)
        return (chunk.exposedtiles[section] & tilebit) != 0;
    int tilex = tile % WORLD_SECTION_COLUMNS, tiley = tile / WORLD_SECTION_COLUMNS;
    bool allneighborsknown = true;
    loopi(int(sizeof(offsets) / sizeof(offsets[0])))
    {
        int chunkx = chunk.x, chunky = chunk.y,
            x = tilex + offsets[i][0], y = tiley + offsets[i][1];
        if(x < 0) { --chunkx; x += WORLD_SECTION_COLUMNS; }
        else if(x >= WORLD_SECTION_COLUMNS) { ++chunkx; x -= WORLD_SECTION_COLUMNS; }
        if(y < 0) { --chunky; y += WORLD_SECTION_COLUMNS; }
        else if(y >= WORLD_SECTION_COLUMNS) { ++chunky; y -= WORLD_SECTION_COLUMNS; }

        worldchunk *neighbor = &chunk;
        if(chunkx != chunk.x || chunky != chunk.y)
        {
            int index = findworldchunk(chunkx, chunky);
            if(!worldchunks.inrange(index))
            {
                allneighborsknown = false;
                continue;
            }
            neighbor = &worldchunks[index];
        }
        if(neighbor->loading || neighbor->corrupted || !neighbor->root)
        {
            allneighborsknown = false;
            continue;
        }
        int neighborsurface = worldchunksurfacesection(
            *neighbor, y * WORLD_SECTION_COLUMNS + x);
        if(neighborsurface < section)
        {
            chunk.exposureknown[section] |= tilebit;
            chunk.exposedtiles[section] |= tilebit;
            return true;
        }
    }
    if(allneighborsknown)
    {
        chunk.exposureknown[section] |= tilebit;
        chunk.exposedtiles[section] &= ~tilebit;
    }
    return false;
}

static bool worldchunksectionrequired(worldchunk &chunk, int tile, int section, int playerradius)
{
    if(drawfullchunk || worldchunksectionnearplayer(chunk, tile, section, playerradius))
        return true;
    int surface = worldchunksurfacesection(chunk, tile);
    if(surface >= 0 && section <= surface &&
       surface - section <= chunksurfaceloaddepth)
        return true;
    return worldchunksectionhascontent(chunk, tile, section) &&
           worldchunksectionexposed(chunk, tile, section);
}

static long long worldchunksectionpriority(worldchunk &chunk, int tile, int section)
{
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec localpos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE,
                  section * WORLD_SECTION_SIZE),
         origin = ivec(worldchunkorigin(chunk)).add(localpos),
         sectionpos(origin.x / WORLD_SECTION_SIZE, origin.y / WORLD_SECTION_SIZE, section);
    vec focus = player ? player->o : camera1 ? camera1->o : vec(0, 0, 0);
    ivec focussection(int(floorf(focus.x / WORLD_SECTION_SIZE)),
                      int(floorf(focus.y / WORLD_SECTION_SIZE)),
                      clamp(int(floorf(focus.z / WORLD_SECTION_SIZE)),
                            0, int(WORLD_SECTION_LAYERS) - 1));
    int dx = sectionpos.x - focussection.x, dy = sectionpos.y - focussection.y,
        dz = section - focussection.z,
        surfacesection = worldchunksurfacesection(chunk, tile),
        surfacedelta = surfacesection >= 0 ? abs(section - surfacesection)
                                           : WORLD_SECTION_LAYERS;

    bool nearplayer = abs(dx) <= 1 && abs(dy) <= 1 && abs(dz) <= 1,
         surface = section == surfacesection,
         surfacesupport = surfacesection >= 0 && section < surfacesection &&
                          surfacesection - section <= chunksurfaceloaddepth,
         surfaceband = surface || surfacesupport,
         content = worldchunksectionhascontent(chunk, tile, section),
         exposed = content && worldchunksectionexposed(chunk, tile, section);
    if(!drawfullchunk && !nearplayer && !surfaceband && !exposed) return LLONG_MAX;
    int visibility = camera1 ? isvisiblebb(origin, ivec(WORLD_SECTION_SIZE,
                                                        WORLD_SECTION_SIZE,
                                                        WORLD_SECTION_SIZE))
                             : VFC_FULL_VISIBLE;
    bool visible = visibility == VFC_FULL_VISIBLE || visibility == VFC_PART_VISIBLE;
    int tier = nearplayer ? 0 :
               surfaceband && visible ? 1 :
               surfaceband ? 2 :
               exposed && visible ? 3 :
               exposed ? 4 :
               visible && content ? 5 :
               content ? 6 : 7;

    long long distance = (long long)dx * dx + (long long)dy * dy + (long long)dz * dz;
    int viewpenalty = 0;
    if(camera1 && visible)
    {
        vec delta = vec(origin).add(WORLD_SECTION_SIZE / 2).sub(camera1->o);
        float len = delta.magnitude();
        if(len > 1e-3f)
        {
            float alignment = clamp(delta.dot(camdir) / len, -1.0f, 1.0f);
            viewpenalty = int((1.0f - alignment) * 2048.0f);
        }
    }
    return ((long long)tier << 48) + (distance << 16)
         + (long long)min(surfacedelta, int(WORLD_SECTION_LAYERS)) * 4096
         + viewpenalty;
}

struct worldsectioncandidate
{
    int chunkindex, tile, section;
    long long score;
};

static int findworldchunkmountsections(int chunkx, int chunky,
                                       worldsectioncandidate *candidates, int maxcandidates)
{
    ZoneScopedN("Chunks/Select prioritized render section");
    int numcandidates = 0;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || chunk.corrupted || !chunk.root || worldchunkfullymounted(chunk) ||
           !worldchunkinview(chunk, chunkx, chunky))
            continue;
        loopj(WORLD_SECTION_TILES)
        {
            if(worldchunkcolumnmounted(chunk, j)) continue;
            const uint tilebit = 1U << j;
            loopk(WORLD_SECTION_LAYERS)
            {
                if(chunk.mountedtiles[k] & tilebit) continue;
                long long score = worldchunksectionpriority(chunk, j, k);
                if(score == LLONG_MAX) continue;
                int insert = numcandidates;
                while(insert > 0 && score < candidates[insert - 1].score) --insert;
                if(insert >= maxcandidates) continue;
                int newcount = min(numcandidates + 1, maxcandidates);
                for(int move = newcount - 1; move > insert; --move)
                    candidates[move] = candidates[move - 1];
                candidates[insert].chunkindex = i;
                candidates[insert].tile = j;
                candidates[insert].section = k;
                candidates[insert].score = score;
                numcandidates = newcount;
            }
        }
    }
    ZoneValue(numcandidates);
    return numcandidates;
}

static bool findworldchunkunloadcolumn(int chunkx, int chunky, int &chunkindex, int &tile)
{
    int bestdist = -1;
    chunkindex = tile = -1;
    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(!worldchunkmounted(chunk) || worldchunkinview(chunk, chunkx, chunky)) continue;
        int dist = worldchunkdistance(chunk.x, chunk.y, chunkx, chunky);
        if(chunkindex >= 0 && dist <= bestdist) continue;
        loopj(WORLD_SECTION_TILES)
        {
            uint tilebit = 1U << j;
            loopk(WORLD_SECTION_LAYERS) if(chunk.mountedtiles[k] & tilebit)
            {
                bestdist = dist;
                chunkindex = i;
                tile = j;
                break;
            }
            if(chunkindex == i) break;
        }
    }
    return chunkindex >= 0;
}

static int findworldchunkcachedsections(int chunkx, int chunky,
                                        worldsectioncandidate *candidates, int maxcandidates)
{
    if(drawfullchunk || maxcandidates <= 0) return 0;
    ZoneScopedN("Chunks/Select occluded sections for caching");
    vec focus = player ? player->o : camera1 ? camera1->o : vec(0, 0, 0);
    int focusx = int(floorf(focus.x / WORLD_SECTION_SIZE)),
        focusy = int(floorf(focus.y / WORLD_SECTION_SIZE)),
        focusz = clamp(int(floorf(focus.z / WORLD_SECTION_SIZE)),
                       0, int(WORLD_SECTION_LAYERS) - 1),
        numcandidates = 0;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || chunk.corrupted || !chunk.root || !worldchunkmounted(chunk) ||
           !worldchunkinview(chunk, chunkx, chunky))
            continue;
        loopj(WORLD_SECTION_TILES)
        {
            int x = j % WORLD_SECTION_COLUMNS, y = j / WORLD_SECTION_COLUMNS,
                sectionx = (chunk.x - worldfirstchunkx) * WORLD_SECTION_COLUMNS + x,
                sectiony = (chunk.y - worldfirstchunky) * WORLD_SECTION_COLUMNS + y,
                dx = sectionx - focusx, dy = sectiony - focusy,
                surface = worldchunksurfacesection(chunk, j);
            const uint tilebit = 1U << j;
            loopk(WORLD_SECTION_LAYERS)
            {
                if(!(chunk.mountedtiles[k] & tilebit) ||
                   worldchunksectionrequired(chunk, j, k, 2))
                    continue;
                int dz = k - focusz,
                    depth = surface >= 0 ? max(surface - k, 0) : WORLD_SECTION_LAYERS;
                long long distance = (long long)dx * dx + (long long)dy * dy +
                                     (long long)dz * dz,
                          score = ((long long)depth << 32) + distance;
                int insert = numcandidates;
                while(insert > 0 && score > candidates[insert - 1].score) --insert;
                if(insert >= maxcandidates) continue;
                int newcount = min(numcandidates + 1, maxcandidates);
                for(int move = newcount - 1; move > insert; --move)
                    candidates[move] = candidates[move - 1];
                candidates[insert].chunkindex = i;
                candidates[insert].tile = j;
                candidates[insert].section = k;
                candidates[insert].score = score;
                numcandidates = newcount;
            }
        }
    }
    ZoneValue(numcandidates);
    return numcandidates;
}

static int processworldchunkvaupdates()
{
    int pending = worldchunkvaupdates.length();
    if(pending <= 0) return 0;

    ZoneScopedN("Chunks/Process prioritized VA updates");
    ZoneValue(pending);

    Uint64 start = SDL_GetPerformanceCounter();
    {
        ZoneScopedN("Chunks/Commit invalidated VA updates");
        ZoneValue(pending);
        commitchanges();
    }
    worldchunkvaupdates.setsize(0);
    worldchunkvaupdateset.clear();
    TracyPlot("Chunks/Pending VA sections", int64_t(0));
    float sample = max(float((SDL_GetPerformanceCounter() - start) * 1000.0 /
                             SDL_GetPerformanceFrequency()) / pending, 0.05f);
    worldchunkvasectionmillis = worldchunkvasectionmillis * 0.75f + sample * 0.25f;
    TracyPlot("Chunks/VA section milliseconds", double(worldchunkvasectionmillis));
    return pending;
}

static int processworldchunkchanges(int chunkx, int chunky)
{
    ZoneScopedN("Chunks/Process geometry changes");
    ZoneTextF("focus %d_%d", chunkx, chunky);
    Uint64 phasestart = SDL_GetPerformanceCounter();
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    int changedcolumns = 0, unloaded = 0, unloadedsections = 0,
        unloadtarget = WORLD_MAX_COLUMN_CHANGES;

    // Cleanup has its own budget and always runs before publication. This
    // prevents rapid movement from leaving a growing trail of live geometry.
    {
        ZoneScopedN("Chunks/Unload columns");
        while(unloaded < unloadtarget && unloadedsections < chunkvastagelimit)
        {
            double elapsed = (SDL_GetPerformanceCounter() - phasestart) * 1000.0 / frequency;
            if(unloaded && elapsed >= chunkcleanupbudget) break;
            int chunkindex, tile;
            if(!findworldchunkunloadcolumn(chunkx, chunky, chunkindex, tile)) break;
            worldchunk &chunk = worldchunks[chunkindex];
            int sections[WORLD_MAX_SECTION_BATCH],
                numsections = unmountworldchunkcolumnbatch(chunk, tile, sections,
                    min(chunksectionbatch, chunkvastagelimit - unloadedsections));
            if(!numsections) break;
            queueworldchunksectionupdates(chunk, tile, sections, numsections);
            unloadedsections += numsections;
            unloaded++;
            changedcolumns++;
        }
        if(unloadedsections < chunkvastagelimit)
        {
            worldsectioncandidate candidates[WORLD_MAX_SECTION_BATCH];
            int numcandidates = findworldchunkcachedsections(
                chunkx, chunky, candidates,
                min(chunkvastagelimit - unloadedsections, int(WORLD_MAX_SECTION_BATCH)));
            loopi(numcandidates)
            {
                double elapsed = (SDL_GetPerformanceCounter() - phasestart) * 1000.0 / frequency;
                if(unloaded && elapsed >= chunkcleanupbudget) break;
                worldsectioncandidate &candidate = candidates[i];
                worldchunk &chunk = worldchunks[candidate.chunkindex];
                if(!unmountworldchunktile(chunk, candidate.section, candidate.tile)) continue;
                queueworldchunksectionupdates(chunk, candidate.tile, &candidate.section, 1);
                unloadedsections++;
                unloaded++;
                changedcolumns++;
            }
        }
        ZoneValue(unloaded);
    }

    phasestart = SDL_GetPerformanceCounter();
    int mounted = 0, mountedsections = 0, mounttarget = WORLD_MAX_COLUMN_CHANGES;
    {
        ZoneScopedN("Chunks/Mount prioritized sections");
        worldsectioncandidate candidates[WORLD_MAX_SECTION_BATCH];
        int numcandidates = findworldchunkmountsections(chunkx, chunky, candidates,
                                                        min(chunkvastagelimit,
                                                            int(WORLD_MAX_SECTION_BATCH)));
        loopi(numcandidates)
        {
            double elapsed = (SDL_GetPerformanceCounter() - phasestart) * 1000.0 / frequency;
            if(mounted && elapsed >= chunkpublishbudget) break;
            worldsectioncandidate &candidate = candidates[i];
            worldchunk &chunk = worldchunks[candidate.chunkindex];
            if(!mountworldchunktile(chunk, candidate.section, candidate.tile)) continue;
            queueworldchunksectionupdates(chunk, candidate.tile, &candidate.section, 1);
            mountedsections++;
            mounted++;
            changedcolumns++;
            if(mounted >= mounttarget) break;
        }
        ZoneValue(mountedsections);
    }

    processworldchunkvaupdates();
    return changedcolumns;
}

static void processworldchunkupdates(int chunkx, int chunky, int aheadx, int aheady)
{
    if(lastworldchunkpublish == totalmillis) return;
    ZoneScopedN("Chunks/Streaming update");
    ZoneTextF("focus %d_%d ahead %d_%d", chunkx, chunky, aheadx, aheady);
    lastworldchunkpublish = totalmillis;
    reprioritizeworldchunkqueue(chunkx, chunky, aheadx, aheady);
    processworldchunkresults();
    queueworldchunkview(chunkx, chunky, aheadx, aheady);
    processworldchunkchanges(chunkx, chunky);
    pruneworldchunkcache(chunkx, chunky, INT_MAX);
    activeworldchunk = findworldchunk(chunkx, chunky);
}

static void rebaseworldchunks(int chunkx, int chunky, bool translateplayer = true)
{
    ZoneScopedN("Chunks/Rebase runtime world");
    ZoneTextF("%d_%d", chunkx, chunky);
    worldchunkvaupdates.setsize(0);
    worldchunkvaupdateset.clear();
    clearworldscattererentities();
    loopv(worldchunks) if(worldchunkmounted(worldchunks[i])) unmountworldchunk(worldchunks[i]);
    if(worldsectionowners.numelems)
    {
        conoutf(CON_ERROR, "discarding %d stale runtime section owners during chunk rebase",
                worldsectionowners.numelems);
        worldsectionowners.clear();
    }

    int newfirstx = chunkx - WORLD_RUNTIME_CENTER,
        newfirsty = chunky - WORLD_RUNTIME_CENTER;
    long long shiftx = ((long long)newfirstx - worldfirstchunkx) * WORLD_CHUNK_SIZE,
              shifty = ((long long)newfirsty - worldfirstchunky) * WORLD_CHUNK_SIZE;
    {
        ZoneScopedN("Chunks/Rebase free old octree");
        freeocta(worldroot);
    }
    worldroot = newcubes(F_EMPTY);
    worldfirstchunkx = newfirstx;
    worldfirstchunky = newfirsty;
    if(player && translateplayer)
    {
        player->o.x -= float(shiftx);
        player->o.y -= float(shifty);
    }
    conoutf(CON_DEBUG, "rebased chunk window around %d_%d", chunkx, chunky);
}

static void mountworldchunksafetyregion(int chunkx, int chunky, bool updategeometry = true)
{
    if(!player) return;
    ZoneScopedN("Chunks/Mount safety region");
    ZoneTextF("%d_%d", chunkx, chunky);
    int playertilex = int(player->o.x) / WORLD_SECTION_SIZE,
        playertiley = int(player->o.y) / WORLD_SECTION_SIZE,
        playersection = clamp(int(player->o.z) / WORLD_SECTION_SIZE,
                              0, int(WORLD_SECTION_LAYERS) - 1),
        changedsections = 0;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || chunk.corrupted || !chunk.root ||
           !worldchunkinview(chunk, chunkx, chunky))
            continue;
        loopj(WORLD_SECTION_TILES)
        {
            int x = j % WORLD_SECTION_COLUMNS, y = j / WORLD_SECTION_COLUMNS,
                worldtilex = (chunk.x - worldfirstchunkx) * WORLD_SECTION_COLUMNS + x,
                worldtiley = (chunk.y - worldfirstchunky) * WORLD_SECTION_COLUMNS + y;
            if(worldtilex != playertilex || worldtiley != playertiley) continue;
            int sections[3], numsections = 0;
            for(int section = max(playersection - 1, 0);
                section <= min(playersection + 1, int(WORLD_SECTION_LAYERS) - 1);
                ++section)
            {
                if(!mountworldchunktile(chunk, section, j)) continue;
                sections[numsections++] = section;
            }
            if(!numsections) continue;
            if(updategeometry) queueworldchunksectionupdates(chunk, j, sections, numsections);
            changedsections += numsections;
        }
    }
    if(changedsections && updategeometry)
    {
        ZoneScopedN("Chunks/Queue safety region geometry");
        ZoneValue(changedsections);
        processworldchunkvaupdates();
    }
}

static int pruneworldchunkcache(int chunkx, int chunky, int limit)
{
    ZoneScopedN("Chunks/Prune cache");
    ZoneTextF("focus %d_%d limit %d", chunkx, chunky, limit);
    Uint64 start = SDL_GetPerformanceCounter();
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    int released = 0, cachedist = maxchunkdist + chunkcachedist;
    for(int i = worldchunks.length() - 1; i >= 0; --i)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || worldchunkmounted(chunk) || chunk.dirty || !chunk.root ||
           worldchunkdistance(chunk.x, chunk.y, chunkx, chunky) <= cachedist)
            continue;
        {
            ZoneScopedN("Chunks/Release cache");
            ZoneTextF("%d_%d", chunk.x, chunk.y);
            freeocta(chunk.root);
        }
        worldchunks.removeunordered(i);
        released++;
        double elapsed = (SDL_GetPerformanceCounter() - start) * 1000.0 / frequency;
        if(released >= limit || elapsed >= chunkcleanupbudget) break;
    }
    return released;
}

static void rebuildworldchunks(int chunkx, int chunky, int aheadx, int aheady, bool load, bool updategeometry)
{
    ZoneScopedN("Chunks/Rebuild view");
    ZoneTextF("focus %d_%d ahead %d_%d", chunkx, chunky, aheadx, aheady);
    rebuildingworldchunks = true;
    int cancelled = reprioritizeworldchunkqueue(chunkx, chunky, aheadx, aheady),
        queued = load ? 0 : queueworldchunkview(chunkx, chunky, aheadx, aheady);

    vector<int> entering, leaving;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        bool shouldmount = worldchunkinview(chunk, chunkx, chunky);
        if(worldchunkmounted(chunk) && !shouldmount) leaving.add(i);
        else if(!worldchunkmounted(chunk) && !chunk.loading && !chunk.corrupted &&
                chunk.root && shouldmount)
            entering.add(i);
    }

    lastplayerchunkx = chunkx;
    lastplayerchunky = chunky;
    lastchunkdist = maxchunkdist;
    if(load)
    {
        // Bootstrap collision only. Surface and camera-visible sections are
        // published by the normal priority scheduler instead of mounting the
        // entire entry chunk before the first frame.
        mountworldchunksafetyregion(chunkx, chunky, false);
        ZoneScopedN("Chunks/Validate runtime octree");
        validatec(worldroot, worldsize >> 1);
    }
    if(updategeometry)
    {
        if(load)
        {
            ZoneScopedN("Chunks/Rebuild all geometry");
            allchanged(worldfolder[0] != '\0');
        }
    }
    // Keep CPU-heavy generation workers out of the synchronous bootstrap.
    // In optimized builds their startup used to overlap VA/material creation.
    if(load) queued = queueworldchunkview(chunkx, chunky, aheadx, aheady);

    int released = pruneworldchunkcache(chunkx, chunky, 1);
    activeworldchunk = findworldchunk(chunkx, chunky);
    if(worldchunks.inrange(activeworldchunk))
    {
        string name;
        worldchunkname(name, sizeof(name), worldchunks[activeworldchunk]);
        setmapfilenames(name, NULL);
    }

    int mounted = 0;
    loopv(worldchunks) if(worldchunkmounted(worldchunks[i])) mounted++;
    rebuildingworldchunks = false;
    conoutf(CON_DEBUG, "chunk view %d_%d: +%d -%d, %d queued, %d cancelled, %d cached released, %d/%d mounted",
            chunkx, chunky, entering.length(), leaving.length(), queued, cancelled, released,
            mounted, (2 * maxchunkdist + 1) * (2 * maxchunkdist + 1));
}

static void updateworldchunkprediction(int chunkx, int chunky, double absolutex, double absolutey)
{
    if(lastworldchunkmotion < 0)
    {
        lastworldchunkposx = absolutex;
        lastworldchunkposy = absolutey;
        lastworldchunkmotion = totalmillis;
        worldchunkvelocityx = worldchunkvelocityy = 0;
        worldchunkaheadx = chunkx;
        worldchunkaheady = chunky;
        return;
    }

    if(totalmillis > lastworldchunkmotion)
    {
        int elapsed = totalmillis - lastworldchunkmotion;
        if(elapsed > 500)
        {
            worldchunkvelocityx = worldchunkvelocityy = 0;
        }
        else
        {
            float samplex = float((absolutex - lastworldchunkposx) / elapsed),
                  sampley = float((absolutey - lastworldchunkposy) / elapsed);
            worldchunkvelocityx = worldchunkvelocityx * 0.65f + samplex * 0.35f;
            worldchunkvelocityy = worldchunkvelocityy * 0.65f + sampley * 0.35f;
        }
        lastworldchunkposx = absolutex;
        lastworldchunkposy = absolutey;
        lastworldchunkmotion = totalmillis;
    }

    if(chunklookahead <= 0)
    {
        worldchunkaheadx = chunkx;
        worldchunkaheady = chunky;
        return;
    }

    const float horizon = 750.0f;
    int predictedx = int(floor((absolutex + worldchunkvelocityx * horizon) / WORLD_CHUNK_SIZE)),
        predictedy = int(floor((absolutey + worldchunkvelocityy * horizon) / WORLD_CHUNK_SIZE));
    worldchunkaheadx = chunkx + clamp(predictedx - chunkx, -chunklookahead, chunklookahead);
    worldchunkaheady = chunky + clamp(predictedy - chunky, -chunklookahead, chunklookahead);
}

void updateworldchunks(bool force)
{
    if(worldchunks.empty() || rebuildingworldchunks || !worldroot) return;
    ZoneScopedN("Chunks/Update world chunks");
    flushworlddiffjournals(false);

    int localchunkx = 0, localchunky = 0;
    if(player)
    {
        localchunkx = int(floor(player->o.x / WORLD_CHUNK_SIZE));
        localchunky = int(floor(player->o.y / WORLD_CHUNK_SIZE));
    }
    int chunkx = worldfirstchunkx + localchunkx,
        chunky = worldfirstchunky + localchunky;
    double absolutex = double(worldfirstchunkx) * WORLD_CHUNK_SIZE + (player ? player->o.x : 0),
           absolutey = double(worldfirstchunky) * WORLD_CHUNK_SIZE + (player ? player->o.y : 0);
    updateworldchunkprediction(chunkx, chunky, absolutex, absolutey);
    if(!force) processworldchunkupdates(chunkx, chunky, worldchunkaheadx, worldchunkaheady);
    if(!force && chunkx == lastplayerchunkx && chunky == lastplayerchunky &&
       maxchunkdist == lastchunkdist)
    {
        updateworldscatterers();
        return;
    }

    int viewdist = maxchunkdist;
    bool rebase = localchunkx - viewdist <= 0 || localchunkx + viewdist >= WORLD_RUNTIME_CHUNKS - 1 ||
                  localchunky - viewdist <= 0 || localchunky + viewdist >= WORLD_RUNTIME_CHUNKS - 1;
    if(rebase)
    {
        rebaseworldchunks(chunkx, chunky);
        mountworldchunksafetyregion(chunkx, chunky);
    }
    rebuildworldchunks(chunkx, chunky, worldchunkaheadx, worldchunkaheady, force && !rebase, true);
    updateworldscatterers();
}

static bool parseworldcoordinate(const char *text, double &coordinate)
{
    if(!text || !*text) return false;
    const char *number = text;
    if(*number == '+' || *number == '-') ++number;
    if(!isdigit(*number) && *number != '.') return false;

    char *end = NULL;
    errno = 0;
    coordinate = strtod(text, &end);
    return errno != ERANGE && end != text && !*end &&
           coordinate >= -DBL_MAX && coordinate <= DBL_MAX;
}

static void teleportplayer(char *xtext, char *ytext, char *ztext)
{
    if(!player)
    {
        conoutf(CON_ERROR, "teleport: no player is available");
        return;
    }

    double x, y, z;
    if(!parseworldcoordinate(xtext, x) || !parseworldcoordinate(ytext, y) ||
       !parseworldcoordinate(ztext, z))
    {
        conoutf(CON_ERROR, "usage: /teleport <absolute x> <absolute y> <absolute z>");
        return;
    }

    if(worldchunks.empty())
    {
        if(x < 0 || x >= worldsize || y < 0 || y >= worldsize ||
           z < 0 || z >= worldsize)
        {
            conoutf(CON_ERROR, "teleport: coordinates must be inside this map (0 <= x, y, z < %d)",
                    worldsize);
            return;
        }

        player->o = vec(float(x), float(y), float(z));
        player->reset();
        player->resetinterp();
        conoutf("teleported to %.2f %.2f %.2f", x, y, z);
        return;
    }

    if(z < 0 || z >= WORLD_MAP_SIZE)
    {
        conoutf(CON_ERROR, "teleport: z must be in the generated world band (0 <= z < %d)",
                WORLD_MAP_SIZE);
        return;
    }

    double chunkxd = floor(x / WORLD_CHUNK_SIZE),
           chunkyd = floor(y / WORLD_CHUNK_SIZE);
    const int chunkmargin = max(int(WORLD_RUNTIME_CENTER), maxchunkdist) + 1,
              minchunk = INT_MIN + chunkmargin,
              maxchunk = INT_MAX - chunkmargin;
    if(chunkxd < minchunk || chunkxd > maxchunk ||
       chunkyd < minchunk || chunkyd > maxchunk)
    {
        double mincoordinate = double(minchunk) * WORLD_CHUNK_SIZE,
               maxcoordinate = double(maxchunk + 1LL) * WORLD_CHUNK_SIZE;
        conoutf(CON_ERROR,
                "teleport: x and y must be in the safe streamed range [%.0f, %.0f)",
                mincoordinate, maxcoordinate);
        return;
    }

    int chunkx = int(chunkxd), chunky = int(chunkyd);

    // A teleport can invalidate every queued streaming request. Stop the
    // workers and remove their placeholders before preparing the destination
    // synchronously, ensuring collision exists as soon as the player arrives.
    shutdownworldchunkloader();
    for(int i = worldchunks.length() - 1; i >= 0; --i)
        if(worldchunks[i].loading) worldchunks.removeunordered(i);

    int generated = 0;
    int destination = acquireworldchunksync(chunkx, chunky, generated);
    if(!worldchunks.inrange(destination) || !worldchunks[destination].root)
    {
        conoutf(CON_ERROR, "teleport: could not prepare destination chunk %d_%d",
                chunkx, chunky);
        return;
    }

    rebaseworldchunks(chunkx, chunky, false);
    player->o = vec(float(x - double(worldfirstchunkx) * WORLD_CHUNK_SIZE),
                    float(y - double(worldfirstchunky) * WORLD_CHUNK_SIZE),
                    float(z));
    player->reset();
    player->resetinterp();

    lastworldchunkmotion = -1;
    worldchunkaheadx = chunkx;
    worldchunkaheady = chunky;
    worlddebugcachemillis = -1;
    rebuildworldchunks(chunkx, chunky, chunkx, chunky, true, true);

    conoutf("teleported to absolute %.2f %.2f %.2f (chunk %d_%d%s)",
            x, y, z, chunkx, chunky, generated ? ", generated" : "");
}

COMMANDN(teleport, teleportplayer, "sss");

struct worldgencontext
{
    game::worldgenerator generator;
    game::worldsettings settings;
    int heightmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar biomemap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar coastmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar cliffmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar rockmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar tectonicactivitymap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar tectonicupliftmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar fracturecorridormap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    int seed, grasstexture, grasssidetexture, grassbottomtexture;
    int dirttexture, stonetexture, sandtexture, snowtexture, woodtexture, leaftexture;
    bool prepared, remip;
    int families, optimized;
    SDL_atomic_t *cancelled;

    worldgencontext(int seed, int grasstexture, int grasssidetexture, int grassbottomtexture,
                    int dirttexture, int stonetexture, int sandtexture, int snowtexture,
                    int woodtexture, int leaftexture,
                    bool prepared, bool remip, const game::worldsettings &settings,
                    SDL_atomic_t *cancelled = NULL)
        : generator(seed, settings), settings(settings), seed(seed), grasstexture(grasstexture),
          grasssidetexture(grasssidetexture), grassbottomtexture(grassbottomtexture),
          dirttexture(dirttexture), stonetexture(stonetexture), sandtexture(sandtexture),
          snowtexture(snowtexture), woodtexture(woodtexture), leaftexture(leaftexture),
          prepared(prepared), remip(remip), families(0), optimized(0), cancelled(cancelled)
    {
    }

    bool iscanceled() const { return cancelled && SDL_AtomicGet(cancelled); }
};

static cube *allocworldgenfamily(worldgencontext &ctx)
{
    if(!ctx.prepared) return newcubes(F_EMPTY);
    cube *c = new cube[8];
    loopi(8) resetworldcube(c[i]);
    ctx.families++;
    return c;
}

static void freepreparedworldchunk(cube *root)
{
    if(!root) return;
    loopi(8) if(root[i].children) freepreparedworldchunk(root[i].children);
    delete[] root;
}

static int worldmidedge(const ivec &a, const ivec &b, int xd, int yd, bool &perfect)
{
    int ax = a[xd], ay = a[yd], bx = b[xd], by = b[yd];
    if(ay == by) return ay;
    if(ax == bx) { perfect = false; return ay; }
    bool crossx = (ax < 8 && bx > 8) || (ax > 8 && bx < 8),
         crossy = (ay < 8 && by > 8) || (ay > 8 && by < 8);
    if(crossy && !crossx) { worldmidedge(a, b, yd, xd, perfect); return 8; }
    if(ax <= 8 && bx <= 8) return ax > bx ? ay : by;
    if(ax >= 8 && bx >= 8) return ax < bx ? ay : by;
    int risex = (by - ay) * (8 - ax) * 256,
        s = risex / (bx - ax),
        y = s / 256 + ay;
    if((abs(s) & 0xFF) || (crossy && y != 8) || y < 0 || y > 16) perfect = false;
    return crossy ? 8 : clamp(y, 0, 16);
}

static inline bool worldcrosscenter(const ivec &a, const ivec &b, int xd, int yd)
{
    int ax = a[xd], ay = a[yd], bx = b[xd], by = b[yd];
    return (((ax <= 8 && bx <= 8) || (ax >= 8 && bx >= 8)) &&
            ((ay <= 8 && by <= 8) || (ay >= 8 && by >= 8))) ||
           (ax + bx == 16 && ay + by == 16);
}

// Worker-safe counterpart of subdividecube(). Temporary candidate children
// are deliberately detached from allocnodes and renderer-owned cubeext state.
static bool subdivideworldmip(const cube &c, cube *children)
{
    if(isempty(c) || isentirelysolid(c))
    {
        loopi(8)
        {
            resetworldcube(children[i]);
            if(isentirelysolid(c)) solidfaces(children[i]);
            children[i].material = c.material;
            loopj(6) children[i].texture[j] = c.texture[j];
        }
        return true;
    }

    loopi(8)
    {
        resetworldcube(children[i]);
        solidfaces(children[i]);
        children[i].material = c.material;
    }
    bool perfect = true;
    ivec v[8];
    loopi(8)
    {
        cube &source = const_cast<cube &>(c);
        v[i].x = edgeget(cubeedge(source, 0, (i >> R[0]) & 1, (i >> C[0]) & 1), (i >> D[0]) & 1);
        v[i].y = edgeget(cubeedge(source, 1, (i >> R[1]) & 1, (i >> C[1]) & 1), (i >> D[1]) & 1);
        v[i].z = edgeget(cubeedge(source, 2, (i >> R[2]) & 1, (i >> C[2]) & 1), (i >> D[2]) & 1);
        v[i].mul(2);
    }

    loopj(6)
    {
        int d = dimension(j), z = dimcoord(j);
        const ivec &v00 = v[octaindex(d, 0, 0, z)],
                   &v10 = v[octaindex(d, 1, 0, z)],
                   &v01 = v[octaindex(d, 0, 1, z)],
                   &v11 = v[octaindex(d, 1, 1, z)];
        int e[3][3];
        e[0][0] = v00[d];
        e[0][2] = v01[d];
        e[2][0] = v10[d];
        e[2][2] = v11[d];
        e[0][1] = worldmidedge(v00, v01, C[d], d, perfect);
        e[1][0] = worldmidedge(v00, v10, R[d], d, perfect);
        e[1][2] = worldmidedge(v11, v01, R[d], d, perfect);
        e[2][1] = worldmidedge(v11, v10, C[d], d, perfect);
        bool p1 = perfect, p2 = perfect;
        int c1 = worldmidedge(v00, v11, R[d], d, p1),
            c2 = worldmidedge(v01, v10, R[d], d, p2);
        if(z ? c1 > c2 : c1 < c2)
        {
            e[1][1] = c1;
            perfect = p1 && (c1 == c2 || worldcrosscenter(v00, v11, C[d], R[d]));
        }
        else
        {
            e[1][1] = c2;
            perfect = p2 && (c1 == c2 || worldcrosscenter(v01, v10, C[d], R[d]));
        }

        loopi(8)
        {
            children[i].texture[j] = c.texture[j];
            int rd = (i >> R[d]) & 1, cd = (i >> C[d]) & 1, dd = (i >> D[d]) & 1;
            edgeset(cubeedge(children[i], d, 0, 0), z, clamp(e[rd][cd] - dd * 8, 0, 8));
            edgeset(cubeedge(children[i], d, 1, 0), z, clamp(e[1 + rd][cd] - dd * 8, 0, 8));
            edgeset(cubeedge(children[i], d, 0, 1), z, clamp(e[rd][1 + cd] - dd * 8, 0, 8));
            edgeset(cubeedge(children[i], d, 1, 1), z, clamp(e[1 + rd][1 + cd] - dd * 8, 0, 8));
        }
    }

    // validatec() normally performs this leaf validation, but it may touch the
    // global allocator. Candidate children never contain descendants.
    loopi(8) loopj(3)
    {
        uint f = children[i].faces[j], e0 = f & 0x0F0F0F0FU, e1 = (f >> 4) & 0x0F0F0F0FU;
        if(e0 == e1 || ((e1 + 0x07070707U) | (e1 - e0)) & 0xF0F0F0F0U)
        {
            emptyfaces(children[i]);
            break;
        }
    }
    return perfect;
}

static const cube *lookupworldmipneighbour(cube *root, int orient, const ivec &co, int size,
                                          ivec &origin, int &neighboursize)
{
    ivec position = co;
    int dim = dimension(orient);
    if(dimcoord(orient)) position[dim] += size;
    else position[dim] -= size;
    if(position[dim] < 0 || position[dim] >= WORLD_CHUNK_MAP_SIZE) return NULL;

    int scale = WORLD_CHUNK_SCALE - 1;
    const cube *neighbour = &root[octastep(position.x, position.y, position.z, scale)];
    while(!(size >> scale) && neighbour->children)
    {
        --scale;
        neighbour = &neighbour->children[octastep(position.x, position.y, position.z, scale)];
    }
    origin = ivec(position).mask(~0U << scale);
    neighboursize = 1 << scale;
    return neighbour;
}

static bool remipworldchunk(cube &c, const ivec &co, int size, cube *root,
                            bool prepared, int &families, int &merged)
{
    cube *children = c.children;
    if(!children) return true;

    bool perfect = true;
    loopi(8) if(!remipworldchunk(children[i], ivec(i, co, size), size >> 1, root,
                                 prepared, families, merged))
        perfect = false;

    solidfaces(c);
    loopi(6) c.texture[i] = getmippedtexture(c, i);
    if(!perfect || (size << 1) > 0x1000) return false;

    ushort material = MAT_AIR;
    loopi(8)
    {
        material = children[i].material;
        if((material & MATF_CLIP) == MAT_NOCLIP || material & MAT_ALPHA)
        {
            if(i > 0) return false;
            while(++i < 8) if(children[i].material != material) return false;
            break;
        }
        else if(!isentirelysolid(children[i]))
        {
            while(++i < 8)
            {
                int othermaterial = children[i].material;
                if(isentirelysolid(children[i])
                    ? (othermaterial & MATF_CLIP) == MAT_NOCLIP || othermaterial & MAT_ALPHA
                    : material != othermaterial)
                    return false;
            }
            break;
        }
    }

    cube candidate = c;
    candidate.ext = NULL;
    forcemip(candidate);
    candidate.children = NULL;
    cube reconstructed[8];
    if(!subdivideworldmip(candidate, reconstructed)) return false;

    uchar visible[6] = { 0, 0, 0, 0, 0, 0 };
    loopi(8)
    {
        if(children[i].faces[0] != reconstructed[i].faces[0] ||
           children[i].faces[1] != reconstructed[i].faces[1] ||
           children[i].faces[2] != reconstructed[i].faces[2])
            return false;
        if(isempty(children[i]) && isempty(reconstructed[i])) continue;

        ivec childorigin(i, co, size);
        loopj(6)
        {
            ivec neighbourorigin;
            int neighboursize;
            const cube *neighbour = lookupworldmipneighbour(root, j, childorigin, size,
                                                            neighbourorigin, neighboursize);
            if(neighbour && !visiblefaceagainst(children[i], j, childorigin, size,
                                                *neighbour, neighbourorigin, neighboursize,
                                                MAT_AIR, (material & MAT_ALPHA) ^ MAT_ALPHA, MAT_ALPHA))
                continue;
            if(children[i].texture[j] != candidate.texture[j]) return false;
            visible[j] |= 1 << i;
        }
    }

    delete[] children;
    if(prepared) families--;
    else allocnodes--;
    c.children = NULL;
    loopi(3) c.faces[i] = candidate.faces[i];
    c.material = material;
    c.visible = 0;
    loopi(6) if(visible[i]) c.visible |= 1 << i;
    if(c.visible) c.visible |= 0x40;
    c.merged = 0;
    merged++;
    return true;
}

static int remipworldchunk(cube *root, bool prepared, int &families, SDL_atomic_t *cancelled)
{
    int merged = 0;
    loopi(8)
    {
        if(cancelled && SDL_AtomicGet(cancelled)) break;
        remipworldchunk(root[i], ivec(i, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE),
                        WORLD_CHUNK_ROOT_SIZE >> 1, root, prepared, families, merged);
    }
    return merged;
}

static void setworldcubetexture(cube &c, int texture, int toptexture = -1,
                                int bottomtexture = -1, int material = MAT_AIR)
{
    solidfaces(c);
    c.material = material;
    loopi(6) c.texture[i] = texture;
    if(toptexture >= 0) c.texture[O_TOP] = toptexture;
    if(bottomtexture >= 0) c.texture[O_BOTTOM] = bottomtexture;
}

static void setworldcubematerial(cube &c, int material)
{
    emptyfaces(c);
    c.material = material;
}

enum { WORLD_EMPTY, WORLD_STONE, WORLD_DIRT, WORLD_GRASS, WORLD_SAND, WORLD_SNOW, WORLD_WATER, WORLD_MIXED };

static float worldsmoothstep(float low, float high, float value)
{
    if(high <= low) return value >= high ? 1.0f : 0.0f;
    float t = clamp((value - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static int generateworldheight(const worldgencontext &ctx, int chunkx, int chunky,
                               int blockx, int blocky,
                               game::worldtectonicsample *tectonics = NULL)
{
    const int x = chunkx * WORLD_CHUNK_BLOCKS + blockx,
              y = chunky * WORLD_CHUNK_BLOCKS + blocky;
    return ctx.generator.height(x, y, tectonics) * WORLD_BLOCK_SIZE;
}

static void generateworldcoastmap(worldgencontext &ctx, int chunkx, int chunky)
{
    memset(ctx.coastmap, 0, sizeof(ctx.coastmap));
    if(ctx.settings.coastwidth <= 0) return;

    const int maxcoastwidth = max(ctx.settings.coastwidth + ctx.settings.coastvariation,
                                  int(ceil(ctx.generator.maxcoasttransitionwidth()))),
              halo = maxcoastwidth + 1,
              mapsize = WORLD_CHUNK_BLOCKS + 2 * halo,
              maparea = mapsize * mapsize,
              fardistance = INT_MAX / 8,
              seaheight = ctx.settings.sealevel * WORLD_BLOCK_SIZE;
    vector<uchar> water;
    vector<int> distance;
    water.pad(maparea);
    distance.pad(maparea);

    loop(y, mapsize) loop(x, mapsize)
    {
        const int blockx = x - halo, blocky = y - halo,
                  height = blockx >= 0 && blockx < WORLD_CHUNK_BLOCKS &&
                           blocky >= 0 && blocky < WORLD_CHUNK_BLOCKS
                         ? ctx.heightmap[blocky * WORLD_CHUNK_BLOCKS + blockx]
                         : generateworldheight(ctx, chunkx, chunky, blockx, blocky);
        water[y * mapsize + x] = height < seaheight;
        distance[y * mapsize + x] = fardistance;
    }

    for(int y = 1; y < mapsize - 1; ++y) for(int x = 1; x < mapsize - 1; ++x)
    {
        const int index = y * mapsize + x;
        const uchar iswater = water[index];
        if(water[index - 1] != iswater || water[index + 1] != iswater ||
           water[index - mapsize] != iswater || water[index + mapsize] != iswater)
            distance[index] = 0;
    }

    for(int y = 1; y < mapsize - 1; ++y) for(int x = 1; x < mapsize - 1; ++x)
    {
        const int index = y * mapsize + x;
        distance[index] = min(distance[index], distance[index - 1] + 3);
        distance[index] = min(distance[index], distance[index - mapsize] + 3);
        distance[index] = min(distance[index], distance[index - mapsize - 1] + 4);
        distance[index] = min(distance[index], distance[index - mapsize + 1] + 4);
    }
    for(int y = mapsize - 2; y >= 1; --y) for(int x = mapsize - 2; x >= 1; --x)
    {
        const int index = y * mapsize + x;
        distance[index] = min(distance[index], distance[index + 1] + 3);
        distance[index] = min(distance[index], distance[index + mapsize] + 3);
        distance[index] = min(distance[index], distance[index + mapsize + 1] + 4);
        distance[index] = min(distance[index], distance[index + mapsize - 1] + 4);
    }

    loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
    {
        const float noisex = float(chunkx) * WORLD_CHUNK_BLOCKS + x + 10000.5f,
                    noisey = float(chunky) * WORLD_CHUNK_BLOCKS + y - 10000.5f,
                    configuredwidth = max(ctx.settings.coastwidth
                                        + ctx.generator.biomeblend.GetNoise(noisex, noisey)
                                          * ctx.settings.coastvariation,
                                          0.0f),
                    profilewidth = ctx.generator.coasttransitionwidth(
                        chunkx * WORLD_CHUNK_BLOCKS + x,
                        chunky * WORLD_CHUNK_BLOCKS + y),
                    width = max(configuredwidth, profilewidth);
        ctx.coastmap[y * WORLD_CHUNK_BLOCKS + x] =
            distance[(y + halo) * mapsize + x + halo] <= int(floor(width * 3.0f + 0.5f));
    }
}

static int generateworldbiome(const worldgencontext &ctx, int chunkx, int chunky,
                              int blockx, int blocky, int height)
{
    const int x = chunkx * WORLD_CHUNK_BLOCKS + blockx,
              y = chunky * WORLD_CHUNK_BLOCKS + blocky;
    return ctx.generator.biome(x, y, height / WORLD_BLOCK_SIZE);
}

static bool generateworldrock(const worldgencontext &ctx, int chunkx, int chunky,
                              int blockx, int blocky, int height)
{
    const int x = chunkx * WORLD_CHUNK_BLOCKS + blockx,
              y = chunky * WORLD_CHUNK_BLOCKS + blocky;
    return ctx.generator.rock(x, y, height / WORLD_BLOCK_SIZE);
}

static bool generateworldcliff(const worldgencontext &ctx, int chunkx, int chunky,
                               int blockx, int blocky, int height)
{
    const int x = chunkx * WORLD_CHUNK_BLOCKS + blockx,
              y = chunky * WORLD_CHUNK_BLOCKS + blocky;
    return ctx.generator.cliff(x, y, height / WORLD_BLOCK_SIZE);
}

static bool generateworldheightmap(worldgencontext &ctx, int chunkx, int chunky)
{
    {
        ZoneScopedN("Chunks/Generate terrain heights");
        loop(y, WORLD_CHUNK_BLOCKS)
        {
            if(ctx.iscanceled()) return false;
            loop(x, WORLD_CHUNK_BLOCKS)
            {
                const int index = y * WORLD_CHUNK_BLOCKS + x;
                game::worldtectonicsample tectonics;
                ctx.heightmap[index] = generateworldheight(ctx, chunkx, chunky, x, y, &tectonics);
                ctx.tectonicactivitymap[index] = uchar(clamp(int(floor(tectonics.activity
                                                               * 255.0f + 0.5f)), 0, 255));
                ctx.tectonicupliftmap[index] = uchar(clamp(int(floor(tectonics.landuplift
                                                             * 255.0f + 0.5f)), 0, 255));
                const int worldx = chunkx * WORLD_CHUNK_BLOCKS + x,
                          worldy = chunky * WORLD_CHUNK_BLOCKS + y;
                ctx.fracturecorridormap[index] = uchar(clamp(int(floor(
                    ctx.generator.fracturecorridor(worldx, worldy) * 255.0f + 0.5f)), 0, 255));
            }
        }
    }
    {
        ZoneScopedN("Chunks/Generate coast map");
        generateworldcoastmap(ctx, chunkx, chunky);
    }
    {
        ZoneScopedN("Chunks/Generate biome maps");
        loop(y, WORLD_CHUNK_BLOCKS)
        {
            if(ctx.iscanceled()) return false;
            loop(x, WORLD_CHUNK_BLOCKS)
            {
                const int index = y * WORLD_CHUNK_BLOCKS + x;
                ctx.biomemap[index] = generateworldbiome(ctx, chunkx, chunky, x, y,
                                                         ctx.heightmap[index]);
                ctx.cliffmap[index] = generateworldcliff(ctx, chunkx, chunky, x, y,
                                                         ctx.heightmap[index]);
                ctx.rockmap[index] = generateworldrock(ctx, chunkx, chunky, x, y, ctx.heightmap[index]);
            }
        }
    }
    return !ctx.iscanceled();
}

static int worldheight(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.heightmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE];
}

static int worldbiome(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.biomemap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE];
}

static bool worldcoast(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.coastmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE] != 0;
}

static bool worldrock(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.rockmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE] != 0;
}

static bool worldcliff(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.cliffmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE] != 0;
}

static int worldcolumncubetype(const worldgencontext &ctx, int z, int size, int height,
                               int biome, bool coast, bool cliff, bool rock)
{
    const int surface = WORLD_GROUND_HEIGHT + height,
              watertop = WORLD_GROUND_HEIGHT + ctx.settings.sealevel * WORLD_BLOCK_SIZE,
              dirtbottom = surface - WORLD_BLOCK_SIZE - WORLD_DIRT_DEPTH,
              grassbottom = surface - WORLD_BLOCK_SIZE,
              beachmin = (ctx.settings.sealevel
                        + min(ctx.settings.beachminheight, ctx.settings.beachmaxheight)) * WORLD_BLOCK_SIZE,
              beachmax = (ctx.settings.sealevel
                        + max(ctx.settings.beachminheight, ctx.settings.beachmaxheight)) * WORLD_BLOCK_SIZE;
    const bool beach = coast && height >= beachmin && height <= beachmax;

    if(z >= max(surface, watertop)) return WORLD_EMPTY;
    if(surface < watertop && z >= surface && z + size <= watertop) return WORLD_WATER;
    if(z + size <= dirtbottom) return WORLD_STONE;
    if(cliff)
    {
        // Every exposed stair of the cliff belongs to the rock face. Normal
        // surface rules resume immediately behind this band, producing a grassy
        // plateau without grass caps scattered down the vertical wall.
        if(z >= dirtbottom && z + size <= surface) return WORLD_STONE;
        return WORLD_MIXED;
    }
    if(rock)
    {
        if(biome == game::WORLD_BIOME_SNOW && z >= grassbottom && z + size <= surface) return WORLD_SNOW;
        if(z >= dirtbottom && z + size <= surface) return WORLD_STONE;
        return WORLD_MIXED;
    }
    if(beach || biome == game::WORLD_BIOME_DESERT)
    {
        if(z >= dirtbottom && z + size <= surface) return WORLD_SAND;
        return WORLD_MIXED;
    }
    if(biome == game::WORLD_BIOME_OCEAN)
    {
        if(z >= dirtbottom && z + size <= surface) return WORLD_DIRT;
        return WORLD_MIXED;
    }
    if(z >= dirtbottom && z + size <= grassbottom) return WORLD_DIRT;
    if(biome == game::WORLD_BIOME_SNOW && z >= grassbottom && z + size <= surface) return WORLD_SNOW;
    if(z >= grassbottom && z + size <= surface) return WORLD_GRASS;
    return WORLD_MIXED;
}

static int worldcubetype(const worldgencontext &ctx, const ivec &o, int size)
{
    if(o.x >= WORLD_CHUNK_SIZE || o.y >= WORLD_CHUNK_SIZE || o.z >= WORLD_MAP_SIZE)
        return WORLD_EMPTY;
    if(o.x + size > WORLD_CHUNK_SIZE || o.y + size > WORLD_CHUNK_SIZE || o.z + size > WORLD_MAP_SIZE)
        return WORLD_MIXED;

    int type = -1;
    for(int y = o.y; y < o.y + size; y += WORLD_BLOCK_SIZE)
    for(int x = o.x; x < o.x + size; x += WORLD_BLOCK_SIZE)
    {
        int columntype = worldcolumncubetype(ctx, o.z, size, worldheight(ctx, x, y),
                                            worldbiome(ctx, x, y), worldcoast(ctx, x, y),
                                            worldcliff(ctx, x, y), worldrock(ctx, x, y));
        if(columntype == WORLD_MIXED || (type >= 0 && type != columntype)) return WORLD_MIXED;
        type = columntype;
    }
    return type;
}

static int worldrepresentativecubetype(const worldgencontext &ctx, const ivec &o, int size)
{
    const int x = clamp(o.x + size / 2, 0, WORLD_CHUNK_SIZE - 1),
              y = clamp(o.y + size / 2, 0, WORLD_CHUNK_SIZE - 1),
              height = worldheight(ctx, x, y),
              biome = worldbiome(ctx, x, y),
              surface = WORLD_GROUND_HEIGHT + height,
              watertop = WORLD_GROUND_HEIGHT + ctx.settings.sealevel * WORLD_BLOCK_SIZE,
              visibletop = max(surface, watertop);
    int z = clamp(o.z + size / 2, 0, WORLD_MAP_SIZE - 1);

    // A coarse cube intersecting the visible column top represents its
    // surface, not the greater volume underneath it. Sample immediately below
    // that top so grass/sand/snow/stone wins over dirt, and water wins for a
    // submerged terrain column. Cubes wholly underground retain the centre
    // sample used for their dominant interior material.
    if(visibletop > o.z && visibletop <= o.z + size)
        z = clamp(visibletop - 1, 0, WORLD_MAP_SIZE - 1);

    return worldcolumncubetype(ctx, z, 1, height, biome, worldcoast(ctx, x, y),
                               worldcliff(ctx, x, y), worldrock(ctx, x, y));
}

static bool generateworldcube(worldgencontext &ctx, cube &c, const ivec &o, int size, int mingridsize)
{
    if(ctx.iscanceled()) return false;
    int type = worldcubetype(ctx, o, size);
    if(type == WORLD_MIXED && size <= mingridsize)
        type = worldrepresentativecubetype(ctx, o, size);
    switch(type)
    {
        case WORLD_EMPTY:
            setworldcubematerial(c, MAT_AIR);
            return true;

        case WORLD_STONE:
            setworldcubetexture(c, ctx.stonetexture);
            return true;

        case WORLD_DIRT:
            setworldcubetexture(c, ctx.dirttexture);
            return true;

        case WORLD_GRASS:
            setworldcubetexture(c, ctx.grasssidetexture, ctx.grasstexture,
                               ctx.grassbottomtexture);
            return true;

        case WORLD_SAND:
            setworldcubetexture(c, ctx.sandtexture);
            return true;

        case WORLD_SNOW:
            setworldcubetexture(c, ctx.snowtexture);
            return true;

        case WORLD_WATER:
            setworldcubematerial(c, MAT_WATER);
            return true;
    }

    if(size <= mingridsize)
    {
        setworldcubematerial(c, MAT_AIR);
        return true;
    }

    c.children = allocworldgenfamily(ctx);
    const int childsize = size >> 1;
    loopi(8) if(!generateworldcube(ctx, c.children[i], ivec(i, o, childsize), childsize, mingridsize))
        return false;
    return true;
}

static uint hashworldtree(uint seed, int chunkx, int chunky, int blockx, int blocky, uint salt)
{
    const uint worldx = uint(chunkx) * uint(WORLD_CHUNK_BLOCKS) + uint(blockx),
               worldy = uint(chunky) * uint(WORLD_CHUNK_BLOCKS) + uint(blocky);
    uint hash = seed ^ salt;
    hash ^= worldx * 0x9E3779B9U;
    hash ^= worldy * 0x85EBCA6BU;
    hash ^= hash >> 16;
    hash *= 0x7FEB352DU;
    hash ^= hash >> 15;
    hash *= 0x846CA68BU;
    hash ^= hash >> 16;
    return hash;
}

static float worldtreeunit(uint hash)
{
    return float(hash & 0x00FFFFFFU) / float(0x01000000U);
}

static uint hashworldgrass(uint seed, uint worldx, uint worldy, uint salt)
{
    uint hash = seed ^ salt;
    hash ^= worldx * 0x9E3779B9U;
    hash ^= worldy * 0x85EBCA6BU;
    hash ^= hash >> 16;
    hash *= 0x7FEB352DU;
    hash ^= hash >> 15;
    hash *= 0x846CA68BU;
    hash ^= hash >> 16;
    return hash;
}

static void cacheworldscattertransform(int chunkx, int chunky, float maxoffset, const worldscatterinstance &scatter)
{
    if(scatter.rendertransformvalid && scatter.rendermaxoffset == maxoffset) return;

    const uint worldx = uint(chunkx * WORLD_CHUNK_BLOCKS + scatter.x / WORLD_BLOCK_SIZE),
               worldy = uint(chunky * WORLD_CHUNK_BLOCKS + scatter.y / WORLD_BLOCK_SIZE),
               seed = uint(game::getworldseed());

    scatter.renderyaw = int(worldtreeunit(hashworldgrass(seed, worldx, worldy, 0x63D83595U)) * 360.0f);

    const float angle = worldtreeunit(hashworldgrass(seed, worldx, worldy, 0xC2B2AE35U)) * 2.0f * M_PI,
                offsetunit = worldtreeunit(hashworldgrass(seed, worldx, worldy, 0x27D4EB2FU)),
                offset = maxoffset * WORLD_BLOCK_SIZE * offsetunit * offsetunit;

    scatter.renderoffsetx = cosf(angle) * offset;
    scatter.renderoffsety = sinf(angle) * offset;
    scatter.rendermaxoffset = maxoffset;
    scatter.rendertransformvalid = true;
}

static void cacheworldscattertransforms(int chunkx, int chunky, float maxoffset, const vector<worldscatterinstance> &scatter)
{
    loopv(scatter) cacheworldscattertransform(chunkx, chunky, maxoffset, scatter[i]);
}

VARP(staticentsmaxdistance, 0, 64, 1024);
VARP(staticentsmaxamount, 0, 8192, MAXENTS);
VARP(staticlightmaxdistance, 0, 64, 1024);

struct worldgrasscollectcontext
{
    FastNoiseLite distribution, flowerdistribution[3];
    game::worldsettings settings;
    vector<worldscatterinstance> &scatter;
    int chunkx, chunky;
    uint seed;

    worldgrasscollectcontext(int chunkx, int chunky, const game::worldsettings &settings, vector<worldscatterinstance> &scatter)
        : settings(settings), scatter(scatter), chunkx(chunkx), chunky(chunky), seed(uint(game::getworldseed()))
    {
        distribution.SetSeed(game::getworldseed() ^ 0x6E624EB7);
        distribution.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
        distribution.SetFrequency(settings.grassfrequency);
        distribution.SetFractalType(FastNoiseLite::FractalType_FBm);
        distribution.SetFractalOctaves(2);
        distribution.SetFractalLacunarity(1.8f);
        distribution.SetFractalGain(0.5f);

        static const uint flowersalts[3] =
        {
            0x9E21F4A7U, 0xC13FA9A9U, 0x91E10DA5U
        };
        loopi(3)
        {
            flowerdistribution[i].SetSeed(game::getworldseed() ^ flowersalts[i]);
            flowerdistribution[i].SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
            flowerdistribution[i].SetFrequency(settings.grassfrequency * 0.35f);
            flowerdistribution[i].SetFractalType(FastNoiseLite::FractalType_FBm);
            flowerdistribution[i].SetFractalOctaves(2);
            flowerdistribution[i].SetFractalLacunarity(1.8f);
            flowerdistribution[i].SetFractalGain(0.5f);
        }
    }
};

static const cube &lookupgeneratedworldcube(const cube *root, const ivec &pos)
{
    int scale = WORLD_CHUNK_SCALE - 1;
    const cube *c = &root[octastep(pos.x, pos.y, pos.z, scale)];
    while(c->children)
    {
        --scale;
        c = &c->children[octastep(pos.x, pos.y, pos.z, scale)];
    }
    return *c;
}

static bool validgeneratedworldscatter(const cube *root,
                                       const worldscatterinstance &scatter)
{
    if(!root || scatter.x < 0 || scatter.x >= WORLD_CHUNK_SIZE ||
       scatter.y < 0 || scatter.y >= WORLD_CHUNK_SIZE ||
       scatter.z < 0 || scatter.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE ||
       scatter.type < 0 || scatter.type >= numworldscatters() ||
       scatter.orient < O_LEFT || scatter.orient > O_TOP)
        return false;
    const ivec center(scatter.x + WORLD_BLOCK_SIZE / 2,
                      scatter.y + WORLD_BLOCK_SIZE / 2,
                      scatter.z + WORLD_BLOCK_SIZE / 2);
    const cube &occupied = lookupgeneratedworldcube(root, center);
    if(!isempty(occupied) || occupied.material != MAT_AIR) return false;

    const bool torch = isworldtorch(scatter.type);
    if((!torch && scatter.orient != O_TOP) ||
       (torch && scatter.orient == O_BOTTOM))
        return false;
    const ivec supportcenter = ivec(center).sub(
        ivec(worldorientnormal(scatter.orient)).mul(WORLD_BLOCK_SIZE));
    // An edge-mounted torch can be owned by the neighboring chunk. Its support
    // is checked once both chunks are mounted in the runtime world.
    if(supportcenter.x < 0 || supportcenter.x >= WORLD_CHUNK_SIZE ||
       supportcenter.y < 0 || supportcenter.y >= WORLD_CHUNK_SIZE)
        return torch;
    const cube &support = lookupgeneratedworldcube(
        root, supportcenter);
    if(isempty(support) || !isentirelysolid(support) ||
       support.material != MAT_AIR)
        return false;
    return true;
}

static bool worldflowerspaced(const worldgrasscollectcontext &ctx, uint worldx,
                              uint worldy, int flower)
{
    static const uint spacingsalts[3] =
    {
        0xD1B54A35U, 0x94D049BBU, 0x369DEA0FU
    };
    const uint priority = hashworldgrass(ctx.seed, worldx, worldy,
                                         spacingsalts[flower]);
    for(int oy = -1; oy <= 1; ++oy) for(int ox = -1; ox <= 1; ++ox)
    {
        if(!ox && !oy) continue;
        const uint other = hashworldgrass(ctx.seed, worldx + ox, worldy + oy,
                                          spacingsalts[flower]);
        if(other < priority ||
           (other == priority && (oy < 0 || (!oy && ox < 0))))
            return false;
    }
    return true;
}

static int chooseworldflower(worldgrasscollectcontext &ctx, float noisex,
                             float noisey, uint worldx, uint worldy)
{
    const float weights[3] =
    {
        worldrosescatter >= 0 ? max(ctx.settings.roseweight, 0.0f) : 0.0f,
        worldtulipscatter >= 0 ? max(ctx.settings.tulipweight, 0.0f) : 0.0f,
        worlddandelionscatter >= 0 ? max(ctx.settings.dandelionweight, 0.0f) : 0.0f
    };
    const int types[3] =
    {
        worldrosescatter, worldtulipscatter, worlddandelionscatter
    };
    const float weightsum = weights[0] + weights[1] + weights[2];
    if(ctx.settings.flowerchance <= 0 || weightsum <= 0) return -1;

    static const uint chancesalts[3] =
    {
        0xDB4F0B91U, 0xBBE05633U, 0xA0F2EC75U
    };
    static const uint choicesalts[3] =
    {
        0x89E18285U, 0xC6BC2796U, 0xCA01F9DDU
    };
    int selected = -1;
    float selectedscore = -1;
    loopi(3)
    {
        if(weights[i] <= 0) continue;

        const float noise = clamp(ctx.flowerdistribution[i].GetNoise(noisex, noisey)
                                  * 0.5f + 0.5f, 0.0f, 1.0f),
                    patch = worldsmoothstep(0.48f, 0.72f, noise),
                    chance = clamp(ctx.settings.flowerchance
                                   * (weights[i] / weightsum)
                                   * (0.05f + 4.95f * patch * patch),
                                   0.0f, 1.0f);
        if(worldtreeunit(hashworldgrass(ctx.seed, worldx, worldy,
                                        chancesalts[i])) >= chance ||
           !worldflowerspaced(ctx, worldx, worldy, i))
            continue;

        const float score = patch
                          + worldtreeunit(hashworldgrass(ctx.seed, worldx, worldy,
                                                        choicesalts[i])) * 0.05f;
        if(score > selectedscore)
        {
            selected = types[i];
            selectedscore = score;
        }
    }
    return selected;
}

static void collectworldgrassnode(worldgrasscollectcontext &ctx, const cube &c, const cube *root, const ivec &o, int size)
{
    if(o.z >= WORLD_MAP_SIZE || o.x >= WORLD_CHUNK_SIZE || o.y >= WORLD_CHUNK_SIZE)
        return;

    if(c.children)
    {
        const int childsize = size >> 1;
        loopi(8)
            collectworldgrassnode(ctx, c.children[i], root,
                                  ivec(i, o, childsize), childsize);
        return;
    }

    if(size < WORLD_BLOCK_SIZE || isempty(c) || !isentirelysolid(c) || c.material != MAT_AIR || c.texture[O_TOP] != worldgrasstexture)
        return;

    const int top = o.z + size;
    if(top >= WORLD_MAP_SIZE) return;

    const int startx = max(o.x, 0), starty = max(o.y, 0),
              endx = min(o.x + size, int(WORLD_CHUNK_SIZE)),
              endy = min(o.y + size, int(WORLD_CHUNK_SIZE));
    for(int y = starty; y < endy; y += WORLD_BLOCK_SIZE)
    for(int x = startx; x < endx; x += WORLD_BLOCK_SIZE)
    {
        const cube &above = lookupgeneratedworldcube(root, ivec(x + WORLD_BLOCK_SIZE / 2, y + WORLD_BLOCK_SIZE / 2, top));
        if(!isempty(above) || above.material != MAT_AIR) continue;

        const int blockx = ctx.chunkx * WORLD_CHUNK_BLOCKS
                         + x / WORLD_BLOCK_SIZE,
                  blocky = ctx.chunky * WORLD_CHUNK_BLOCKS
                         + y / WORLD_BLOCK_SIZE;
        const uint worldx = uint(blockx), worldy = uint(blocky);
        const float noisex = float(blockx) + 0.5f,
                    noisey = float(blocky) + 0.5f;
        int type = chooseworldflower(ctx, noisex, noisey, worldx, worldy);
        if(type < 0)
        {
            if(worldgrassscatter < 0) continue;
            const float
                    noise = clamp(ctx.distribution.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    patch = worldsmoothstep(0.2f, 0.8f, noise),
                    density = clamp(ctx.settings.grassdensity * (0.12f + 1.88f * patch * patch), 0.0f, 1.0f);
            if(worldtreeunit(hashworldgrass(ctx.seed, worldx, worldy, 0xA511E9B3U))
               >= density)
                continue;
            type = worldgrassscatter;
        }

        worldscatterinstance &scatter = ctx.scatter.add(worldscatterinstance(x, y, top, type));
        cacheworldscattertransform(ctx.chunkx, ctx.chunky, ctx.settings.grassmaxoffset, scatter);
    }
}

static void generateworldscatter(cube *root, int chunkx, int chunky, const game::worldsettings &settings, vector<worldscatterinstance> &scatter)
{
    scatter.setsize(0);
    if(!root || (worldgrassscatter < 0 && worldrosescatter < 0 && worldtulipscatter < 0 && worlddandelionscatter < 0))
        return;
    const bool grass = worldgrassscatter >= 0 && settings.grassdensity > 0,
               flowers = settings.flowerchance > 0 &&
                         ((worldrosescatter >= 0 && settings.roseweight > 0) ||
                          (worldtulipscatter >= 0 && settings.tulipweight > 0) ||
                          (worlddandelionscatter >= 0 &&
                           settings.dandelionweight > 0));
    if(!grass && !flowers) return;
    worldgrasscollectcontext ctx(chunkx, chunky, settings, scatter);
    loopi(8)
        collectworldgrassnode(ctx, root[i], root, ivec(i, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE), WORLD_CHUNK_ROOT_SIZE);
}

struct worldgrasscandidate
{
    ivec key;
    vec position;
    int model, yaw, pitch, roll;
    bool matched;

    worldgrasscandidate(const ivec &key, const vec &position, int model,
                        int yaw, int pitch, int roll)
        : key(key), position(position), model(model), yaw(yaw),
          pitch(pitch), roll(roll), matched(false) {}
};

struct worldscatterchunkcandidate
{
    int chunkindex;
    float distancesquared;

    worldscatterchunkcandidate() : chunkindex(-1), distancesquared(0) {}
    worldscatterchunkcandidate(int chunkindex, float distancesquared)
        : chunkindex(chunkindex), distancesquared(distancesquared) {}

    bool operator<(const worldscatterchunkcandidate &other) const
    {
        return distancesquared < other.distancesquared;
    }
};

struct worldgrassentity
{
    ivec key;
    int id;

    worldgrassentity(const ivec &key, int id) : key(key), id(id) {}
};

static vector<worldgrassentity> worldgrassentities;

static void clearworldscattererentities()
{
    loopv(worldgrassentities) destroyworldmapmodelentity(worldgrassentities[i].id);
    worldgrassentities.setsize(0);
}

static ivec worldscatterkey(const worldchunk &chunk, const worldscatterinstance &scatter)
{
    return ivec(chunk.x * WORLD_CHUNK_BLOCKS + scatter.x / WORLD_BLOCK_SIZE,
                chunk.y * WORLD_CHUNK_BLOCKS + scatter.y / WORLD_BLOCK_SIZE,
                scatter.z);
}

static void worldscattertransform(const worldchunk &chunk, const worldscatterinstance &scatter, float maxoffset, vec &position, int &yaw, int &pitch, int &roll)
{
    pitch = roll = 0;
    const ivec origin = worldchunkorigin(chunk);
    if(isworldtorch(scatter.type))
    {
        yaw = 0;
        position = vec(origin.x + scatter.x + WORLD_BLOCK_SIZE * 0.5f,
                       origin.y + scatter.y + WORLD_BLOCK_SIZE * 0.5f,
                       float(scatter.z));
        if(scatter.orient == O_TOP) return;

        const ivec normal = worldorientnormal(scatter.orient);
        position.z += WORLD_BLOCK_SIZE * 0.25f;
        const int axis = dimension(scatter.orient);
        position[axis] -= normal[axis] * WORLD_BLOCK_SIZE * 0.5f;
        position[axis] += normal[axis] * 1.25f;
        switch(scatter.orient)
        {
            case O_BACK:  yaw = 0; break;
            case O_RIGHT: yaw = 90; break;
            case O_FRONT: yaw = 180; break;
            case O_LEFT:  yaw = 270; break;
        }
        pitch = 23;
        return;
    }

    cacheworldscattertransform(chunk.x, chunk.y, maxoffset, scatter);
    yaw = scatter.renderyaw;
    position = vec(origin.x + scatter.x + WORLD_BLOCK_SIZE * 0.5f + scatter.renderoffsetx, origin.y + scatter.y + WORLD_BLOCK_SIZE * 0.5f + scatter.renderoffsety, float(scatter.z));
}

static bool worldtorchflameposition(const worldchunk &chunk, const worldscatterinstance &scatter, float maxoffset, vec &flame)
{
    vec position;
    int yaw, pitch, roll;
    worldscattertransform(chunk, scatter, maxoffset, position, yaw, pitch, roll);
    return worldscatterdefinitions.inrange(scatter.type) && modeltagposition(worldscatterdefinitions[scatter.type]->model, "tag_emitter", flame, position, yaw, pitch, roll);
}

static bool worldscattermounted(const worldchunk &chunk, const worldscatterinstance &scatter)
{
    const int tilex = scatter.x / WORLD_SECTION_SIZE,
              tiley = scatter.y / WORLD_SECTION_SIZE,
              tile = tiley * WORLD_SECTION_COLUMNS + tilex,
              section = clamp((scatter.z - 1) / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_LAYERS) - 1);
    return (chunk.mountedtiles[section] & (1U << tile)) != 0;
}

static float worldscatterchunkdistance(const worldchunk &chunk, const vec &focus, float expansion)
{
    const ivec origin = worldchunkorigin(chunk);
    const float minx = origin.x - expansion,
                miny = origin.y - expansion,
                maxx = origin.x + WORLD_CHUNK_SIZE + expansion,
                maxy = origin.y + WORLD_CHUNK_SIZE + expansion,
                dx = focus.x < minx ? minx - focus.x
                   : focus.x > maxx ? focus.x - maxx : 0.0f,
                dy = focus.y < miny ? miny - focus.y
                   : focus.y > maxy ? focus.y - maxy : 0.0f;
    return dx * dx + dy * dy;
}

static void updateworldscatterers()
{
    const vec *focus = player ? &player->o : camera1 ? &camera1->o : NULL;
    if(staticentsmaxdistance <= 0 || staticentsmaxamount <= 0 || !focus || worldchunks.empty())
    {
        clearworldscattererentities();
        return;
    }

    vector<worldgrasscandidate> candidates;
    vector<worldscatterchunkcandidate> scatterchunks;
    const game::worldsettings settings;
    const float radius = staticentsmaxdistance * WORLD_BLOCK_SIZE,
                radiussquared = radius * radius,
                maxoffset = max(settings.grassmaxoffset, 0.0f) * WORLD_BLOCK_SIZE;

    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) continue;
        const float distance = worldscatterchunkdistance(chunk, *focus, maxoffset);
        if(distance > radiussquared) continue;
        scatterchunks.add(worldscatterchunkcandidate(i, distance));
    }
    scatterchunks.sort();

    loopv(scatterchunks)
    {
        const worldchunk &chunk = worldchunks[scatterchunks[i].chunkindex];
        loopvj(chunk.scatter)
        {
            const worldscatterinstance &scatter = chunk.scatter[j];
            if(!worldscatterdefinitions.inrange(scatter.type) || worldscatterdefinitions[scatter.type]->mapmodel < 0 || !worldscattermounted(chunk, scatter))
                continue;
            vec position;
            int yaw, pitch, roll;
            worldscattertransform(chunk, scatter, settings.grassmaxoffset, position, yaw, pitch, roll);
            const float dx = position.x - focus->x, dy = position.y - focus->y;
            if(dx * dx + dy * dy > radiussquared) continue;
            candidates.add(worldgrasscandidate(worldscatterkey(chunk, scatter), position, worldscatterdefinitions[scatter.type]->mapmodel, yaw, pitch, roll));
            if(candidates.length() >= staticentsmaxamount) break;
        }
        if(candidates.length() >= staticentsmaxamount) break;
    }

    hashtable<ivec, int> desired(1<<12);
    loopv(candidates) desired[candidates[i].key] = i;
    for(int i = worldgrassentities.length() - 1; i >= 0; --i)
    {
        worldgrassentity &active = worldgrassentities[i];
        int *candidateindex = desired.access(active.key);
        if(!candidateindex || !isworldmapmodelentity(active.id, candidates[*candidateindex].model))
        {
            destroyworldmapmodelentity(active.id);
            worldgrassentities.removeunordered(i);
            continue;
        }
        worldgrasscandidate &candidate = candidates[*candidateindex];
        if(!updateworldmapmodelentity(active.id, candidate.position, candidate.model, candidate.yaw, candidate.pitch, candidate.roll))
        {
            destroyworldmapmodelentity(active.id);
            worldgrassentities.removeunordered(i);
            continue;
        }
        candidates[*candidateindex].matched = true;
    }

    loopv(candidates) if(!candidates[i].matched)
    {
        int id = createworldmapmodelentity(candidates[i].position, candidates[i].model, candidates[i].yaw, candidates[i].pitch, candidates[i].roll);
        if(id < 0) break;
        worldgrassentities.add(worldgrassentity(candidates[i].key, id));
    }
}

void addworldtorchlights()
{
    if(staticlightmaxdistance <= 0 || !camera1 || worldchunks.empty()) return;

    static const float TORCH_LIGHT_RADIUS = 14.0f * WORLD_BLOCK_SIZE;
    const float maxdistance = staticlightmaxdistance * WORLD_BLOCK_SIZE,
                maxdistancesquared = maxdistance * maxdistance,
                fullshadowdistance = maxdistance / 3.0f,
                dynshadowdistance = fullshadowdistance * 2.0f;
    const game::worldsettings settings;

    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) continue;
        loopvj(chunk.scatter)
        {
            const worldscatterinstance &scatter = chunk.scatter[j];
            if(!isworldtorch(scatter.type) || !worldscattermounted(chunk, scatter)) continue;

            vec flame;
            if(!worldtorchflameposition(chunk, scatter, settings.grassmaxoffset, flame)) continue;

            const float distancesquared = flame.squaredist(camera1->o);
            if(distancesquared > maxdistancesquared) continue;
            const float distance = sqrtf(distancesquared);
            const int flags = distance <= fullshadowdistance ? 0 : distance <= dynshadowdistance ? L_NODYNSHADOW : L_NOSHADOW;
            adddynlight(flame, TORCH_LIGHT_RADIUS, vec(1.0f, 0.58f, 0.24f), 0, 0, flags | DL_NODIST);
        }
    }
}

void addworldtorchparticles()
{
    if(staticentsmaxdistance <= 0 || !camera1 || worldchunks.empty()) return;

    const float maxdistance = staticentsmaxdistance * WORLD_BLOCK_SIZE, maxdistancesquared = maxdistance * maxdistance;
    const game::worldsettings settings;
    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) continue;
        loopvj(chunk.scatter)
        {
            const worldscatterinstance &scatter = chunk.scatter[j];
            if(!isworldtorch(scatter.type) || !worldscattermounted(chunk, scatter)) continue;

            vec flame;
            if(!worldtorchflameposition(chunk, scatter, settings.grassmaxoffset, flame)) continue;
            if(flame.squaredist(camera1->o) > maxdistancesquared) continue;
            regular_particle_flame(PART_FLAME, flame, 0.7f, 0.7f, 0xFF8628, 1, 2.4f, 35.0f, 220.0f, -10);
            regular_particle_flame(PART_SMOKE, flame, 0.9f, 1.1f, 0xAA8C4E, 1, 3.0f, 16.0f, 1100.0f, -25);
        }
    }
}

ICOMMAND(getworldgrasscount, "", (),
{
    int count = 0;
    const int model = worldscatterdefinitions.inrange(worldgrassscatter) ? worldscatterdefinitions[worldgrassscatter]->mapmodel : -1;
    if(model >= 0) loopv(worldgrassentities)
        if(isworldmapmodelentity(worldgrassentities[i].id, model)) ++count;
    intret(count);
});

static int worldflowerscattertype(int flower)
{
    switch(flower)
    {
        case 0: return worldrosescatter;
        case 1: return worldtulipscatter;
        case 2: return worlddandelionscatter;
        default: return -1;
    }
}

ICOMMAND(getworldflowercount, "", (),
{
    int count = 0;
    loopv(worldgrassentities)
    {
        loopj(3)
        {
            const int type = worldflowerscattertype(j);
            const int model = worldscatterdefinitions.inrange(type)
                            ? worldscatterdefinitions[type]->mapmodel : -1;
            if(model >= 0 &&
               isworldmapmodelentity(worldgrassentities[i].id, model))
            {
                ++count;
                break;
            }
        }
    }
    intret(count);
});

ICOMMAND(getworldscatterdrawn, "", (), intret(worldgrassentities.length()));

bool isworldscatterentity(int id)
{
    loopv(worldgrassentities) if(worldgrassentities[i].id == id) return true;
    return false;
}

bool getworldscatterentitybox(int id, vec &center, vec &radius)
{
    if(!isworldscatterentity(id)) return false;
    const vector<extentity *> &ents = entities::getents();
    if(!ents.inrange(id)) return false;
    const extentity &e = *ents[id];
    model *m = loadmapmodel(e.attr1);
    if(!m) return false;

    m->boundbox(center, radius);
    if(e.attr5 > 0)
    {
        const float scale = e.attr5 / 100.0f;
        center.mul(scale);
        radius.mul(scale);
    }
    rotatebb(center, radius, e.attr2, e.attr3, e.attr4);
    center.add(e.o);
    return true;
}

bool getworldscatterentityedit(int id, int &type, ivec &support, int &orient)
{
    loopv(worldgrassentities)
    {
        const worldgrassentity &active = worldgrassentities[i];
        if(active.id != id) continue;
        loopvj(worldchunks)
        {
            const worldchunk &chunk = worldchunks[j];
            loopvk(chunk.scatter)
            {
                const worldscatterinstance &scatter = chunk.scatter[k];
                if(worldscatterkey(chunk, scatter) != active.key) continue;
                type = scatter.type;
                orient = scatter.orient;
                support = ivec(worldchunkorigin(chunk))
                    .add(ivec(scatter.x, scatter.y, scatter.z))
                    .sub(ivec(worldorientnormal(orient)).mul(
                        WORLD_BLOCK_SIZE));
                return true;
            }
        }
    }
    return false;
}

static void commitworldscatterrecord(worldchunk &chunk,
                                     const worldscatterinstance &scatter,
                                     const ivec &support, bool place)
{
    worldeditrecord record;
    record.chunkx = chunk.x;
    record.chunky = chunk.y;
    record.operation = place ? WORLD_EDIT_SET_SCATTER
                             : WORLD_EDIT_DELETE_SCATTER;
    record.author = worldeditauthor;
    record.revision = incomingworldeditrevision
                    ? max(worldeditrevision, incomingworldeditrevision)
                    : worldeditrevision + 1;
    worldeditrevision = record.revision;
    incomingworldeditrevision = 0;
    record.timestamp = ullong(time(NULL));
    record.args[0] = scatter.type;
    record.selection.o = support;
    record.selection.s = ivec(1, 1, 1);
    record.selection.grid = WORLD_BLOCK_SIZE;
    record.selection.orient = scatter.orient;
    record.selection.cx = record.selection.cy = record.selection.corner = 0;
    record.selection.cxs = record.selection.cys = 2;
    if(place) record.scatterafter.add(scatter);
    else record.scatterbefore.add(scatter);

    worldchunkdiffstate *state = findworldchunkdiffstate(chunk.x, chunk.y, true);
    state->revision = max(state->revision, record.revision);
    state->pending.add(cloneworldeditrecord(record));
    state->journal.add(cloneworldeditrecord(record));
    state->audit.add(cloneworldeditrecord(record));
    chunk.dirty = true;
}

bool worldtorchincell(const ivec &cell)
{
    if(cell.x < 0 || cell.y < 0 || cell.z < 0 ||
       cell.x >= worldsize || cell.y >= worldsize ||
       cell.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE)
        return false;
    const int chunkx = worldfirstchunkx + cell.x / WORLD_CHUNK_SIZE,
              chunky = worldfirstchunky + cell.y / WORLD_CHUNK_SIZE,
              chunkindex = findworldchunk(chunkx, chunky);
    if(!worldchunks.inrange(chunkindex)) return false;
    const worldchunk &chunk = worldchunks[chunkindex];
    const ivec origin = worldchunkorigin(chunk);
    loopv(chunk.scatter)
    {
        const worldscatterinstance &scatter = chunk.scatter[i];
        if(isworldtorch(scatter.type) &&
           scatter.x == cell.x - origin.x &&
           scatter.y == cell.y - origin.y &&
           scatter.z == cell.z)
            return true;
    }
    return false;
}

bool editworldscatter(int type, const ivec &support, int orient, bool place)
{
    if(!worldscatterdefinitions.inrange(type) || orient < O_LEFT || orient > O_TOP || (!isworldtorch(type) && orient != O_TOP) || (isworldtorch(type) && orient == O_BOTTOM))
        return false;

    const ivec target = ivec(support).add(ivec(worldorientnormal(orient)).mul(WORLD_BLOCK_SIZE));

    if(target.x < 0 || target.y < 0 || target.z < 0 || target.x >= worldsize || target.y >= worldsize || target.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE)
        return false;

    const int chunkx = worldfirstchunkx + target.x / WORLD_CHUNK_SIZE,
              chunky = worldfirstchunky + target.y / WORLD_CHUNK_SIZE,
              chunkindex = findworldchunk(chunkx, chunky);

    if(!worldchunks.inrange(chunkindex)) return false;
    worldchunk &chunk = worldchunks[chunkindex];
    if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) return false;
    const ivec origin = worldchunkorigin(chunk);
    worldscatterinstance scatter(target.x - origin.x, target.y - origin.y, target.z, type, orient);
    cacheworldscattertransform(chunk.x, chunk.y, game::worldsettings().grassmaxoffset, scatter);

    int existing = -1;
    loopv(chunk.scatter)
    {
        if(chunk.scatter[i].x == scatter.x && chunk.scatter[i].y == scatter.y && chunk.scatter[i].z == scatter.z)
        {
            existing = i;
            break;
        }
    }

    if(place)
    {
        if(existing >= 0 || !validworldscatter(chunk, scatter)) return false;
        chunk.scatter.add(scatter);
    }
    else
    {
        if(existing < 0 || chunk.scatter[existing].type != type || chunk.scatter[existing].orient != orient)
            return false;

        scatter = chunk.scatter[existing];
        chunk.scatter.removeunordered(existing);
    }
    commitworldscatterrecord(chunk, scatter, support, place);
    updateworldscatterers();
    return true;
}

static void addworldtreeblock(vector<ivec> &blocks, int blockx, int blocky, int blockz)
{
    if(blockx < 0 || blockx >= WORLD_CHUNK_BLOCKS || blocky < 0 || blocky >= WORLD_CHUNK_BLOCKS || blockz < 0 || blockz >= WORLD_HEIGHT_BLOCKS) return;
    blocks.add(ivec(blockx * WORLD_BLOCK_SIZE, blocky * WORLD_BLOCK_SIZE, blockz * WORLD_BLOCK_SIZE));
}

static void addworldregulartree(vector<ivec> &wood, vector<ivec> &leaves, int blockx, int blocky, int basez, int height, uint shapehash)
{
    loop(z, height) addworldtreeblock(wood, blockx, blocky, basez + z);

    for(int z = height - 2; z <= height; ++z)
    {
        const int radius = z == height ? 1 : 2;
        for(int y = -radius; y <= radius; ++y) for(int x = -radius; x <= radius; ++x)
        {
            if(radius == 2 && abs(x) == 2 && abs(y) == 2 && (hashworldtree(shapehash, x, y, z, height, 0xA511E9B3U) & 1U)) continue;
            addworldtreeblock(leaves, blockx + x, blocky + y, basez + z);
        }
    }
}

static void addworldpinetree(vector<ivec> &wood, vector<ivec> &leaves, int blockx, int blocky, int basez, int height)
{
    loop(z, height) addworldtreeblock(wood, blockx, blocky, basez + z);
    addworldtreeblock(leaves, blockx, blocky, basez + height);

    for(int z = 2; z < height; ++z)
    {
        const int fromtop = height - z, radius = min(3, 1 + fromtop / 3);
        for(int y = -radius; y <= radius; ++y) for(int x = -radius; x <= radius; ++x)
        {
            if(abs(x) + abs(y) > radius + 1) continue;
            addworldtreeblock(leaves, blockx + x, blocky + y, basez + z);
        }
    }
}

static void subdivideworldgencube(worldgencontext &ctx, cube &c)
{
    if(c.children) return;
    cube parent = c;
    c.children = allocworldgenfamily(ctx);
    loopi(8)
    {
        c.children[i] = parent;
        c.children[i].children = NULL;
        c.children[i].ext = NULL;
        c.children[i].visible = 0;
        c.children[i].merged = 0;
    }
}

static cube &lookupworldgenblock(worldgencontext &ctx, cube *root, const ivec &position)
{
    cube *family = root;
    ivec origin(0, 0, 0);
    int size = WORLD_CHUNK_ROOT_SIZE;
    for(;;)
    {
        const int index = (position.x >= origin.x + size ? 1 : 0)
                        | (position.y >= origin.y + size ? 2 : 0)
                        | (position.z >= origin.z + size ? 4 : 0);
        cube &c = family[index];
        if(size == WORLD_BLOCK_SIZE) return c;
        subdivideworldgencube(ctx, c);
        origin = ivec(index, origin, size);
        family = c.children;
        size >>= 1;
    }
}

enum { WORLD_CARVE_NONE, WORLD_CARVE_AIR, WORLD_CARVE_LAVA };

static int worldcarveindex(int x, int y, int blockz)
{
    return (blockz * WORLD_CHUNK_BLOCKS + y) * WORLD_CHUNK_BLOCKS + x;
}

static uint mixworldfeaturehash(uint hash, uint value)
{
    hash ^= value + 0x9E3779B9U + (hash << 6) + (hash >> 2);
    hash ^= hash >> 16;
    hash *= 0x7FEB352DU;
    hash ^= hash >> 15;
    return hash;
}

static uint hashworldfeature(uint seed, long long x, long long y, int z, uint salt)
{
    const unsigned long long ux = (unsigned long long)x,
                             uy = (unsigned long long)y;
    uint hash = seed ^ salt;
    hash = mixworldfeaturehash(hash, uint(ux));
    hash = mixworldfeaturehash(hash, uint(ux >> 32));
    hash = mixworldfeaturehash(hash, uint(uy));
    hash = mixworldfeaturehash(hash, uint(uy >> 32));
    return mixworldfeaturehash(hash, uint(z));
}

static long long worldfloordiv(long long value, int divisor)
{
    long long quotient = value / divisor;
    if(value < 0 && value % divisor) --quotient;
    return quotient;
}

static bool generateworldcaveentrance(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky, int height)
{
    const int mindepth = min(ctx.settings.cavemindepth, ctx.settings.cavefulldepth), logicalz = height / WORLD_BLOCK_SIZE - 1;
    const float tunnelweight = worldsmoothstep(1.0f, float(mindepth), 1.0f),
                veinwidth = ctx.settings.caveentrancewidth + (ctx.settings.tunnelwidth - ctx.settings.caveentrancewidth) * tunnelweight,
                noisex = float(chunkx) * WORLD_CHUNK_BLOCKS + blockx + 17500.5f,
                noisey = float(chunky) * WORLD_CHUNK_BLOCKS + blocky - 17500.5f,
                noisez = logicalz + 3500.5f;
    return fabs(ctx.generator.tunnela.GetNoise(noisex, noisey, noisez)) < veinwidth &&
           fabs(ctx.generator.tunnelb.GetNoise(noisex, noisey, noisez)) < veinwidth;
}

static bool generateworldcheesecaves(worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky)
{
    const int bottomlayers = clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS));
    const int minheight = WORLD_MIN_HEIGHT + bottomlayers;
    const int mindepth = min(ctx.settings.cavemindepth, ctx.settings.cavefulldepth);
    const int fulldepth = max(ctx.settings.cavemindepth, ctx.settings.cavefulldepth);

    const float deepdenominator = max(float(ctx.settings.cavedeepheight - minheight), 1.0f);

    loop(y, WORLD_CHUNK_BLOCKS)
    {
        if(ctx.iscanceled()) return false;

        loop(x, WORLD_CHUNK_BLOCKS)
        {
            const int index = y * WORLD_CHUNK_BLOCKS + x;
            const int surfaceheight = ctx.heightmap[index] / WORLD_BLOCK_SIZE;
            const int caveceiling = min(surfaceheight - 1, WORLD_MAX_HEIGHT - 1);

            const float tectonicactivity = ctx.tectonicactivitymap[index] / 255.0f;
            const float tectonicuplift = ctx.tectonicupliftmap[index] / 255.0f;
            const float fracturecorridor = ctx.fracturecorridormap[index] / 255.0f;
            const float foundationprotection = 1.0f - tectonicuplift * 0.70f;
            const float tectonicprotecteddepth = max(float(mindepth), 12.0f);
            const float tectonicfulldepth = max(float(fulldepth), 20.0f);

            const float noisex = float(chunkx) * WORLD_CHUNK_BLOCKS + x + 17500.5f;
            const float noisey = float(chunky) * WORLD_CHUNK_BLOCKS + y - 17500.5f;

            for(int logicalz = minheight; logicalz <= caveceiling; ++logicalz)
            {
                const float depth = float(surfaceheight - logicalz);
                const float depthweight = worldsmoothstep(float(mindepth), float(fulldepth), depth);
                const float tectonicdepthweight = worldsmoothstep(tectonicprotecteddepth, max(tectonicfulldepth, tectonicprotecteddepth + 1.0f), depth );
                const float tectonicbase = tectonicactivity * tectonicdepthweight * foundationprotection;
                const float caveexpansion = tectonicbase * ctx.settings.tectoniccavestrength;
                const float tunnelweight = worldsmoothstep(1.0f, float(mindepth), depth);

                const float veinwidth = ctx.settings.caveentrancewidth + (ctx.settings.tunnelwidth - ctx.settings.caveentrancewidth) * tunnelweight;

                const float surfacepenalty = (1.0f - depthweight) * 0.35f;
                const float deepweight = clamp((ctx.settings.cavedeepheight - logicalz) / deepdenominator, 0.0f, 1.0f );

                const float largecavethreshold = ctx.settings.largecavethreshold +
                    (ctx.settings.largecavedeepthreshold - ctx.settings.largecavethreshold) * deepweight + surfacepenalty - caveexpansion * 0.22f;

                const float fracturewidth = ctx.settings.tectonicfracturestrength * tectonicbase * 0.06f;
                const float noisez = logicalz + 3500.5f;

                bool carve =
                    fabs(ctx.generator.tunnela.GetNoise(noisex, noisey, noisez)) < veinwidth &&
                    fabs(ctx.generator.tunnelb.GetNoise(noisex, noisey, noisez)) < veinwidth;

                if(!carve && fracturecorridor < fracturewidth)
                {
                    const float fracturez = logicalz * 0.18f + 5000.5f;

                    carve = ctx.generator.fracturevertical.GetNoise(noisex + 13500.0f, noisey - 13500.0f, fracturez) > -0.25f;
                }

                if(!carve && depth >= mindepth)
                {
                    carve = ctx.generator.caves.GetNoise(noisex, noisey, noisez) > ctx.settings.cavethreshold + surfacepenalty ||
                            ctx.generator.largecaves.GetNoise(noisex, noisey, noisez) > largecavethreshold;
                }

                if(carve) carvemap[worldcarveindex(x, y, logicalz - WORLD_MIN_HEIGHT)] = WORLD_CARVE_AIR;
            }
        }
    }

    return true;
}

static bool generateworldlavalakes(worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky)
{
    const int spacing = max(ctx.settings.lavalakespacing, 1),
              verticalspacing = max(spacing / 2, 8),
              minradius = min(ctx.settings.lavalakeminsize, ctx.settings.lavalakemaxsize),
              maxradius = max(ctx.settings.lavalakeminsize, ctx.settings.lavalakemaxsize),
              bottomlayers = clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS)),
              minimumheight = WORLD_MIN_HEIGHT + bottomlayers,
              startheight = max(ctx.settings.lavalakestartheight, ctx.settings.lavalakedeepheight),
              deepheight = min(ctx.settings.lavalakestartheight, ctx.settings.lavalakedeepheight);
    const long long chunkstartx = (long long)chunkx * WORLD_CHUNK_BLOCKS,
                    chunkstarty = (long long)chunky * WORLD_CHUNK_BLOCKS,
                    mincellx = worldfloordiv(chunkstartx - maxradius, spacing),
                    maxcellx = worldfloordiv(chunkstartx + WORLD_CHUNK_BLOCKS - 1 + maxradius, spacing),
                    mincelly = worldfloordiv(chunkstarty - maxradius, spacing),
                    maxcelly = worldfloordiv(chunkstarty + WORLD_CHUNK_BLOCKS - 1 + maxradius, spacing);
    const int mincellz = int(worldfloordiv(minimumheight - maxradius, verticalspacing)),
              maxcellz = int(worldfloordiv(startheight, verticalspacing));

    for(long long celly = mincelly; celly <= maxcelly; ++celly)
    for(long long cellx = mincellx; cellx <= maxcellx; ++cellx)
    for(int cellz = mincellz; cellz <= maxcellz; ++cellz)
    {
        if(ctx.iscanceled()) return false;
        const uint positionhash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, 0xC13FA9A9U),
                   chancehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, 0x91E10DA5U),
                   sizehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, 0xD192ED03U);
        const long long centerx = cellx * spacing + int(positionhash % uint(spacing)),
                        centery = celly * spacing + int((positionhash >> 8) % uint(spacing));
        const int centerz = cellz * verticalspacing + int((positionhash >> 16) % uint(verticalspacing));
        if(centerz < minimumheight || centerz > startheight) continue;

        const float approachweight = deepheight < startheight
                                   ? clamp((startheight - centerz) / float(startheight - deepheight),
                                           0.0f, 1.0f)
                                   : 1.0f,
                    deepweight = deepheight > minimumheight
                               ? clamp((deepheight - centerz) / float(deepheight - minimumheight),
                                       0.0f, 1.0f)
                               : centerz <= deepheight ? 1.0f : 0.0f,
                    lakechance = ctx.settings.lavalakeshallowchance * approachweight
                               + (ctx.settings.lavalakedeepchance
                                - ctx.settings.lavalakeshallowchance) * deepweight;
        if(worldtreeunit(chancehash) >= clamp(lakechance, 0.0f, 1.0f)) continue;

        const int depthmaxradius = clamp(int(floor(minradius
                                               + (maxradius - minradius)
                                               * (0.25f + deepweight * 0.75f) + 0.5f)),
                                           minradius, maxradius),
                  radiusrange = max(depthmaxradius - minradius + 1, 1),
                  radius = minradius + int(sizehash % uint(radiusrange)),
                  minorradius = max(2, int(floor(radius
                                  * (0.55f + ((sizehash >> 8) & 0xFFU) / 637.5f) + 0.5f))),
                  verticalradius = max(2, (radius + minorradius) / 4),
                  lavalevel = centerz - int((sizehash >> 28) % uint(max(verticalradius / 2, 1)));
        const float angle = ((sizehash >> 16) & 0x0FFFU) / 4096.0f * 2.0f * M_PI,
                    anglecos = cosf(angle), anglesin = sinf(angle),
                    lobeangle = angle + (((positionhash >> 24) & 0xFFU) / 255.0f - 0.5f) * M_PI,
                    lobedistance = radius * (0.15f + ((chancehash >> 24) & 0xFFU) / 1275.0f),
                    lobecenterx = cosf(lobeangle) * lobedistance,
                    lobecentery = sinf(lobeangle) * lobedistance,
                    loberadius = max(radius * 0.62f, 1.0f),
                    lobeminorradius = max(minorradius * 0.7f, 1.0f),
                    shapevariation = clamp(ctx.settings.lavalakeshapevariation, 0.0f, 0.75f);
        const int centerlocalx = int(centerx - chunkstartx),
                  centerlocaly = int(centery - chunkstarty);
        if(centerlocalx + radius < 0 || centerlocalx - radius >= WORLD_CHUNK_BLOCKS ||
           centerlocaly + radius < 0 || centerlocaly - radius >= WORLD_CHUNK_BLOCKS) continue;

        const int centerblockx = int(centerx - (long long)chunkx * WORLD_CHUNK_BLOCKS),
                  centerblocky = int(centery - (long long)chunky * WORLD_CHUNK_BLOCKS),
                  centerheight = generateworldheight(ctx, chunkx, chunky,
                                                           centerblockx, centerblocky) / WORLD_BLOCK_SIZE;
        if(centerz + verticalradius > centerheight - ctx.settings.cavemindepth) continue;

        const int xmin = max(centerlocalx - radius, 0),
                  xmax = min(centerlocalx + radius, WORLD_CHUNK_BLOCKS - 1),
                  ymin = max(centerlocaly - radius, 0),
                  ymax = min(centerlocaly + radius, WORLD_CHUNK_BLOCKS - 1),
                  zmin = max(centerz - verticalradius, minimumheight),
                  zmax = min(centerz + verticalradius, WORLD_MAX_HEIGHT - 1);
        for(int y = ymin; y <= ymax; ++y) for(int x = xmin; x <= xmax; ++x)
        {
            const float localx = float(x - centerlocalx),
                        localy = float(y - centerlocaly),
                        rotatedx = localx * anglecos + localy * anglesin,
                        rotatedy = -localx * anglesin + localy * anglecos,
                        primary = rotatedx * rotatedx / float(radius * radius)
                                + rotatedy * rotatedy / float(minorradius * minorradius),
                        lobex = rotatedx - lobecenterx,
                        lobey = rotatedy - lobecentery,
                        lobe = lobex * lobex / (loberadius * loberadius)
                             + lobey * lobey / (lobeminorradius * lobeminorradius),
                        horizontal = min(primary, lobe),
                        shapenoise = ctx.generator.lakeshape.GetNoise(float(chunkx) * WORLD_CHUNK_BLOCKS + x + 9200.5f,
                                                           float(chunky) * WORLD_CHUNK_BLOCKS + y - 9200.5f),
                        boundary = 1.0f - shapevariation * 0.5f
                                 + shapenoise * shapevariation * 0.5f;
            if(horizontal > boundary) continue;

            const int surfaceheight = ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE;
            for(int logicalz = zmin; logicalz <= zmax; ++logicalz)
            {
                if(surfaceheight - logicalz < ctx.settings.cavemindepth) continue;
                const float dz = (logicalz - centerz) / float(verticalradius);
                if(horizontal + dz * dz > boundary) continue;

                uchar &carve = carvemap[worldcarveindex(x, y, logicalz - WORLD_MIN_HEIGHT)];
                if(logicalz <= lavalevel) carve = WORLD_CARVE_LAVA;
                else if(carve == WORLD_CARVE_NONE) carve = WORLD_CARVE_AIR;
            }
        }
    }
    return true;
}

static bool placeworldcaves(worldgencontext &ctx, cube *root, int chunkx, int chunky)
{
    const int mapblocks = WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS * WORLD_HEIGHT_BLOCKS;
    vector<uchar> carvemap;
    uchar *carve;
    {
        ZoneScopedN("Chunks/Allocate cave map");
        carve = carvemap.pad(mapblocks);
        memset(carve, WORLD_CARVE_NONE, mapblocks * sizeof(uchar));
    }

    {
        ZoneScopedN("Chunks/Generate cave fields");
        if(!generateworldcheesecaves(ctx, carve, chunkx, chunky)) return false;
    }
    {
        ZoneScopedN("Chunks/Generate lava lakes");
        if(!generateworldlavalakes(ctx, carve, chunkx, chunky)) return false;
    }

    {
        ZoneScopedN("Chunks/Apply cave map");
        const int bottomlayers = clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS));
        loop(z, bottomlayers) loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
            carve[worldcarveindex(x, y, z)] = WORLD_CARVE_LAVA;

        loop(z, WORLD_HEIGHT_BLOCKS)
        {
            if(ctx.iscanceled()) return false;
            loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
            {
                const uchar type = carve[worldcarveindex(x, y, z)];
                if(type == WORLD_CARVE_NONE) continue;
                cube &c = lookupworldgenblock(ctx, root, ivec(x * WORLD_BLOCK_SIZE,
                                                             y * WORLD_BLOCK_SIZE,
                                                             z * WORLD_BLOCK_SIZE));
                if(type == WORLD_CARVE_LAVA) setworldcubematerial(c, MAT_LAVA);
                else if(!isempty(c) && c.material == MAT_AIR) setworldcubematerial(c, MAT_AIR);
            }
        }
    }
    return true;
}

static bool placeworldtrees(worldgencontext &ctx, cube *root, int chunkx, int chunky)
{
    vector<ivec> wood, leaves;
    const int halo = 3,
              beachmin = (ctx.settings.sealevel
                        + min(ctx.settings.beachminheight, ctx.settings.beachmaxheight)) * WORLD_BLOCK_SIZE,
              beachmax = (ctx.settings.sealevel
                        + max(ctx.settings.beachminheight, ctx.settings.beachmaxheight)) * WORLD_BLOCK_SIZE,
              coasttreemax = (ctx.settings.sealevel + 2) * WORLD_BLOCK_SIZE;

    {
        ZoneScopedN("Chunks/Select tree blocks");
        for(int y = -halo; y < WORLD_CHUNK_BLOCKS + halo; ++y)
        for(int x = -halo; x < WORLD_CHUNK_BLOCKS + halo; ++x)
        {
            if(x == -halo && ctx.iscanceled()) return false;
            const bool inside = x >= 0 && x < WORLD_CHUNK_BLOCKS &&
                                y >= 0 && y < WORLD_CHUNK_BLOCKS;
            const int index = inside ? y * WORLD_CHUNK_BLOCKS + x : 0;
            const int height = inside ? ctx.heightmap[index]
                                      : generateworldheight(ctx, chunkx, chunky, x, y),
                      biome = inside ? ctx.biomemap[index]
                                     : generateworldbiome(ctx, chunkx, chunky, x, y, height);
            if(biome != game::WORLD_BIOME_FOREST && biome != game::WORLD_BIOME_PLAINS) continue;
            if(inside ? ctx.rockmap[index] != 0
                      : generateworldrock(ctx, chunkx, chunky, x, y, height)) continue;
            if(ctx.settings.coastwidth > 0 && height >= beachmin
            && height <= max(beachmax, coasttreemax)) continue;
            if(generateworldcaveentrance(ctx, chunkx, chunky, x, y, height)) continue;

            const float density = biome == game::WORLD_BIOME_FOREST
                                ? ctx.settings.foresttreedensity
                                : ctx.settings.plainstreedensity;
            const uint spawn = hashworldtree(uint(ctx.seed), chunkx, chunky, x, y, 0xD1B54A35U);
            if(worldtreeunit(spawn) >= density) continue;

            const float heightblocks = height / float(WORLD_BLOCK_SIZE),
                        pinelow = float(min(ctx.settings.pinestartheight, ctx.settings.pinefullheight)),
                        pinehigh = float(max(ctx.settings.pinestartheight, ctx.settings.pinefullheight)),
                        pinechance = worldsmoothstep(pinelow, pinehigh, heightblocks);
            const uint shape = hashworldtree(uint(ctx.seed), chunkx, chunky, x, y, 0x94D049BBU);
            const bool pine = worldtreeunit(shape) < pinechance;
            const int treeheight = pine ? 6 + int((shape >> 24) & 3U)
                                        : 4 + int((shape >> 24) % 3U),
                      basez = WORLD_GROUND_HEIGHT / WORLD_BLOCK_SIZE + height / WORLD_BLOCK_SIZE;
            if(basez + treeheight >= WORLD_HEIGHT_BLOCKS) continue;

            if(pine) addworldpinetree(wood, leaves, x, y, basez, treeheight);
            else addworldregulartree(wood, leaves, x, y, basez, treeheight, shape);
        }
    }

    {
        ZoneScopedN("Chunks/Apply tree blocks");
        ZoneValue(wood.length() + leaves.length());
        loopv(leaves)
        {
            cube &c = lookupworldgenblock(ctx, root, leaves[i]);
            if(isempty(c) && c.material == MAT_AIR)
                setworldcubetexture(c, ctx.leaftexture, -1, -1,
                                    leavesalpha ? MAT_ALPHA : MAT_AIR);
        }
        loopv(wood)
        {
            cube &c = lookupworldgenblock(ctx, root, wood[i]);
            if((isempty(c) && c.material == MAT_AIR) || c.texture[0] == ctx.leaftexture)
                setworldcubetexture(c, ctx.woodtexture);
        }
    }
    return !ctx.iscanceled();
}

static cube *generateworldchunk(int chunkx, int chunky, worldgencontext &ctx)
{
    ZoneScopedN("Chunks/Generate");
    ZoneTextF("%d_%d", chunkx, chunky);
    {
        ZoneScopedN("Chunks/Generate height and biomes");
        if(!generateworldheightmap(ctx, chunkx, chunky)) return NULL;
    }
    cube *root;
    {
        ZoneScopedN("Chunks/Generate base octree");
        root = allocworldgenfamily(ctx);
        const int rootsize = WORLD_CHUNK_ROOT_SIZE;
        loopi(8) if(!generateworldcube(ctx, root[i], ivec(i, ivec(0, 0, 0), rootsize), rootsize, WORLD_BLOCK_SIZE))
        {
            ZoneScopedN("Chunks/Free failed generation");
            freepreparedworldchunk(root);
            return NULL;
        }
    }
    {
        ZoneScopedN("Chunks/Generate caves");
        if(!placeworldcaves(ctx, root, chunkx, chunky))
        {
            ZoneScopedN("Chunks/Free failed generation");
            freepreparedworldchunk(root);
            return NULL;
        }
    }
    {
        ZoneScopedN("Chunks/Generate trees");
        if(!placeworldtrees(ctx, root, chunkx, chunky))
        {
            ZoneScopedN("Chunks/Free failed generation");
            freepreparedworldchunk(root);
            return NULL;
        }
    }
    if(ctx.remip)
    {
        ZoneScopedN("Chunks/Remip generated octree");
        ctx.optimized = remipworldchunk(root, ctx.prepared, ctx.families, ctx.cancelled);
        ZoneValue(ctx.optimized);
    }
    else ctx.optimized = 0;
    if(ctx.iscanceled())
    {
        ZoneScopedN("Chunks/Free cancelled generation");
        freepreparedworldchunk(root);
        return NULL;
    }
    ZoneValue(ctx.families);
    return root;
}

static cube *generateworldchunk(int chunkx, int chunky)
{
    ZoneScopedN("Chunks/Generate synchronous");
    ZoneTextF("%d_%d", chunkx, chunky);
    const game::worldsettings settings;
    worldgencontext ctx(game::getworldseed(), worldgrasstexture, worldgrasssidetexture,
                        worldgrassbottomtexture,
                        worlddirttexture, worldstonetexture, worldsandtexture, worldsnowtexture,
                        worldwoodtexture, worldleaftexture, false, chunkremip != 0, settings);
    return generateworldchunk(chunkx, chunky, ctx);
}

static bool chunkcoords(const char *name, int &x, int &y)
{
    if(!name || !*name) return false;
    char *end = NULL;
    long parsedx = strtol(name, &end, 10);
    if(end == name || *end != '_') return false;
    const char *second = end + 1;
    long parsedy = strtol(second, &end, 10);
    if(end == second || *end || parsedx < INT_MIN || parsedx > INT_MAX || parsedy < INT_MIN || parsedy > INT_MAX)
        return false;
    x = int(parsedx);
    y = int(parsedy);
    return true;
}

static bool chunkbasename(const char *name)
{
    int x, y;
    return chunkcoords(name, x, y);
}

static void normalizeworldfolder(char *folder, size_t len, const char *requested)
{
    string name;
    validmapname(name, requested && *requested ? requested : game::getclientmap(), NULL, "untitled");
    loopi(strlen(name)) if(name[i] == '\\') name[i] = '/';

    char *slash = strrchr(name, '/');
    if(slash && chunkbasename(slash + 1)) *slash = '\0';
    copystring(folder, name[0] ? name : "untitled", len);
}

static void chooseworldfolder(const char *requested)
{
    normalizeworldfolder(worldfolder, sizeof(worldfolder), requested);
}

static void worldchunkname(char *name, size_t len, const worldchunk &chunk)
{
    snprintf(name, len, "%s/%d_%d", worldfolder, chunk.x, chunk.y);
}

void setmapfilenames(const char *fname, const char *cname = NULL)
{
    string name;
    validmapname(name, fname);
    formatstring(ogzname, "media/map/%s.ogz", name);
    formatstring(picname, "media/map/%s.png", name);
    if(savebak==1) formatstring(bakname, "media/map/%s.BAK", name);
    else
    {
        string baktime;
        time_t t = time(NULL);
        size_t len = strftime(baktime, sizeof(baktime), "%Y-%m-%d_%H.%M.%S", localtime(&t));
        baktime[min(len, sizeof(baktime)-1)] = '\0';
        formatstring(bakname, "media/map/%s_%s.BAK", name, baktime);
    }

    validmapname(name, cname ? cname : fname);
    formatstring(cfgname, "media/map/%s.cfg", name);

    path(ogzname);
    path(bakname);
    path(cfgname);
    path(picname);
}

void mapcfgname()
{
    const char *mname = game::getclientmap();
    string name;
    validmapname(name, mname);
    defformatstring(cfgname, "media/map/%s.cfg", name);
    path(cfgname);
    result(cfgname);
}

COMMAND(mapcfgname, "");

void backup(const char *name, const char *backupname)
{
    string backupfile;
    copystring(backupfile, findfile(backupname, "wb"));
    remove(backupfile);
    rename(findfile(name, "wb"), backupfile);
}

// Leaf payloads contain only shape, face type IDs, and an optional material ID.
enum { OCTSAV_CHILDREN = 0, OCTSAV_EMPTY, OCTSAV_SOLID, OCTSAV_NORMAL };

static int savemapprogress = 0;

void savec(cube *c, const ivec &o, int size, stream *f)
{
    if((savemapprogress++&0xFFF)==0) renderprogress(float(savemapprogress)/allocnodes, "saving octree...");

    loopi(8)
    {
        ivec co(i, o, size);
        if(c[i].children)
        {
            f->putchar(OCTSAV_CHILDREN);
            savec(c[i].children, co, size>>1, f);
        }
        else
        {
            int octsav = isempty(c[i]) ? OCTSAV_EMPTY :
                         isentirelysolid(c[i]) ? OCTSAV_SOLID : OCTSAV_NORMAL;
            if(c[i].material != MAT_AIR) octsav |= 0x40;
            f->putchar(octsav);
            if((octsav & 0x7) == OCTSAV_NORMAL) f->write(c[i].edges, sizeof(c[i].edges));
            if((octsav & 0x7) != OCTSAV_EMPTY)
            {
                loopj(6) f->putlil<ushort>(c[i].texture[j]);
            }
            if(octsav & 0x40) f->putlil<ushort>(c[i].material);
        }
    }
}

cube *loadchildren(stream *f, const ivec &co, int size, bool &failed);

void loadc(stream *f, cube &c, const ivec &co, int size, bool &failed)
{
    int octsav = f->getchar();
    if(octsav < 0 || octsav & ~0x47) { failed = true; return; }
    switch(octsav&0x7)
    {
        case OCTSAV_CHILDREN:
            if(octsav != OCTSAV_CHILDREN) { failed = true; return; }
            c.children = loadchildren(f, co, size>>1, failed);
            return;

        case OCTSAV_EMPTY:  emptyfaces(c);        break;
        case OCTSAV_SOLID:  solidfaces(c);        break;
        case OCTSAV_NORMAL:
            if(f->read(c.edges, sizeof(c.edges)) != sizeof(c.edges)) { failed = true; return; }
            break;
        default: failed = true; return;
    }
    if((octsav & 0x7) != OCTSAV_EMPTY) loopi(6) c.texture[i] = f->getlil<ushort>();
    if(octsav&0x40) c.material = f->getlil<ushort>();
}

cube *loadchildren(stream *f, const ivec &co, int size, bool &failed)
{
    cube *c = newcubes();
    loopi(8)
    {
        loadc(f, c[i], ivec(i, co, size), size, failed);
        if(failed) break;
    }
    return c;
}

static cube *newpreparedfamily(int &families)
{
    cube *c = new cube[8];
    loopi(8) resetworldcube(c[i]);
    families++;
    return c;
}

struct worldchunkreader
{
    const uchar *pos, *end;

    worldchunkreader(const uchar *data, size_t length) : pos(data), end(data + length) {}

    bool readbyte(uchar &value)
    {
        if(pos >= end) return false;
        value = *pos++;
        return true;
    }

    bool readushort(ushort &value)
    {
        if(end - pos < 2) return false;
        value = ushort(pos[0] | (uint(pos[1]) << 8));
        pos += 2;
        return true;
    }

    bool read(void *dst, size_t length)
    {
        if(size_t(end - pos) < length) return false;
        memcpy(dst, pos, length);
        pos += length;
        return true;
    }

    bool readuint(uint &value)
    {
        if(end - pos < 4) return false;
        value = uint(pos[0]) | (uint(pos[1]) << 8) | (uint(pos[2]) << 16) |
                (uint(pos[3]) << 24);
        pos += 4;
        return true;
    }

    bool readullong(ullong &value)
    {
        if(end - pos < 8) return false;
        value = 0;
        loopi(8) value |= ullong(pos[i]) << (i * 8);
        pos += 8;
        return true;
    }

    int remaining() const { return int(end - pos); }
};

static void subdivideworlddiffcube(cube &c, bool prepared, int &families)
{
    if(c.children) return;
    cube parent = c;
    c.children = prepared ? newpreparedfamily(families) : newcubes(F_EMPTY);
    loopi(8)
    {
        cube &child = c.children[i];
        memcpy(child.edges, parent.edges, sizeof(child.edges));
        memcpy(child.texture, parent.texture, sizeof(child.texture));
        child.material = parent.material;
        child.visible = child.merged = 0;
        child.ext = NULL;
        child.children = NULL;
    }
}

bool worldselectionready(const selinfo &selection)
{
    if(!worldroot || selection.grid <= 0 || selection.s.x <= 0 ||
       selection.s.y <= 0 || selection.s.z <= 0)
        return false;

    int minx = worldfirstchunkx + int(floor(double(selection.o.x) / WORLD_CHUNK_SIZE)),
        miny = worldfirstchunky + int(floor(double(selection.o.y) / WORLD_CHUNK_SIZE)),
        maxx = worldfirstchunkx + int(floor(double(selection.o.x +
                    selection.s.x * selection.grid - 1) / WORLD_CHUNK_SIZE)),
        maxy = worldfirstchunky + int(floor(double(selection.o.y +
                    selection.s.y * selection.grid - 1) / WORLD_CHUNK_SIZE));
    for(int y = miny; y <= maxy; ++y) for(int x = minx; x <= maxx; ++x)
    {
        int index = findworldchunk(x, y);
        if(!worldchunks.inrange(index) || worldchunks[index].loading ||
           !worldchunks[index].root || !worldchunkmounted(worldchunks[index]))
            return false;
        const worldchunk &chunk = worldchunks[index];
        ivec origin = worldchunkorigin(chunk);
        int localminx = clamp(selection.o.x - origin.x, 0, int(WORLD_CHUNK_SIZE) - 1),
            localminy = clamp(selection.o.y - origin.y, 0, int(WORLD_CHUNK_SIZE) - 1),
            localmaxx = clamp(selection.o.x + selection.s.x * selection.grid - 1 - origin.x,
                              0, int(WORLD_CHUNK_SIZE) - 1),
            localmaxy = clamp(selection.o.y + selection.s.y * selection.grid - 1 - origin.y,
                              0, int(WORLD_CHUNK_SIZE) - 1),
            minsection = clamp(selection.o.z / WORLD_SECTION_SIZE, 0,
                               int(WORLD_SECTION_LAYERS) - 1),
            maxsection = clamp((selection.o.z + selection.s.z * selection.grid - 1) /
                               WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_LAYERS) - 1);
        for(int section = minsection; section <= maxsection; ++section)
            for(int tiley = localminy / WORLD_SECTION_SIZE;
                tiley <= localmaxy / WORLD_SECTION_SIZE; ++tiley)
                for(int tilex = localminx / WORLD_SECTION_SIZE;
                    tilex <= localmaxx / WORLD_SECTION_SIZE; ++tilex)
                {
                    int tile = tiley * WORLD_SECTION_COLUMNS + tilex;
                    if(!(chunk.mountedtiles[section] & (1U << tile))) return false;
                }
    }
    return true;
}

static void applyworlddiffnode(cube *root, const worlddiffnode &node,
                               bool prepared, int &families)
{
    if(!root || node.size <= 0 || (node.size & (node.size - 1)) ||
       node.x < 0 || node.y < 0 || node.z < 0 ||
       node.x + node.size > WORLD_CHUNK_MAP_SIZE ||
       node.y + node.size > WORLD_CHUNK_MAP_SIZE ||
       node.z + node.size > WORLD_CHUNK_MAP_SIZE)
        return;

    ivec pos(node.x, node.y, node.z);
    int scale = WORLD_CHUNK_SCALE - 1;
    cube *c = &root[octastep(pos.x, pos.y, pos.z, scale)];
    while((1 << scale) > node.size)
    {
        subdivideworlddiffcube(*c, prepared, families);
        --scale;
        c = &c->children[octastep(pos.x, pos.y, pos.z, scale)];
    }
    if(c->children)
    {
        if(prepared) freepreparedworldchunk(c->children);
        else discardchildren(*c);
        c->children = NULL;
    }
    memcpy(c->edges, node.edges, sizeof(node.edges));
    memcpy(c->texture, node.texture, sizeof(node.texture));
    c->material = node.material;
    c->visible = c->merged = 0;
    c->ext = NULL;
}

static bool deserializeworlddiffnode(worldchunkreader &reader, worlddiffnode &node)
{
    uint value;
    if(!reader.readuint(value)) return false;
    node.x = int(value);
    if(!reader.readuint(value)) return false;
    node.y = int(value);
    if(!reader.readuint(value)) return false;
    node.z = int(value);
    if(!reader.readuint(value)) return false;
    node.size = int(value);
    if(!reader.read(node.edges, sizeof(node.edges))) return false;
    loopi(6) if(!reader.readushort(node.texture[i])) return false;
    return reader.readushort(node.material);
}

static bool deserializeworldscatterinstance(worldchunkreader &reader,
                                            worldscatterinstance &scatter)
{
    uint value;
    if(!reader.readuint(value)) return false;
    scatter.x = int(value);
    if(!reader.readuint(value)) return false;
    scatter.y = int(value);
    if(!reader.readuint(value)) return false;
    scatter.z = int(value);
    if(!reader.readuint(value)) return false;
    // Legacy records stored only the type and were always mounted on top.
    const int encodedorient = int((value >> 16) & 0x7);
    scatter.orient = encodedorient ? encodedorient - 1 : O_TOP;
    scatter.type = int(value & 0xFFFF);
    scatter.rendertransformvalid = false;
    return scatter.x >= 0 && scatter.x < WORLD_CHUNK_SIZE &&
           scatter.y >= 0 && scatter.y < WORLD_CHUNK_SIZE &&
           scatter.z >= 0 &&
           scatter.z + WORLD_BLOCK_SIZE <= WORLD_MAP_SIZE &&
           scatter.type >= 0 && scatter.type < numworldscatters() &&
           scatter.orient >= O_LEFT && scatter.orient <= O_TOP;
}

static bool deserializeworldeditrecord(worldchunkreader &reader, worldeditrecord &record)
{
    uint length, checksum;
    if(!reader.readuint(length) || !reader.readuint(checksum) ||
       length > uint(reader.remaining()))
        return false;
    const uchar *recordbytes = reader.pos;
    worldchunkreader body(recordbytes, length);
    reader.pos += length;
    if(worlddiffchecksum(recordbytes, length) != checksum) return false;

    uint value, count;
    if(!body.readuint(value)) return false;
    record.chunkx = int(value);
    if(!body.readuint(value)) return false;
    record.chunky = int(value);
    if(!body.readuint(value)) return false;
    record.chunkz = int(value);
    if(!body.readullong(record.revision) || !body.readullong(record.timestamp)) return false;
    if(!body.readuint(value)) return false;
    record.author = int(value);
    if(!body.readuint(value)) return false;
    record.operation = int(value);
    if(!body.readuint(value)) return false;
    record.selection.o.x = int(value);
    if(!body.readuint(value)) return false;
    record.selection.o.y = int(value);
    if(!body.readuint(value)) return false;
    record.selection.o.z = int(value);
    if(!body.readuint(value)) return false;
    record.selection.s.x = int(value);
    if(!body.readuint(value)) return false;
    record.selection.s.y = int(value);
    if(!body.readuint(value)) return false;
    record.selection.s.z = int(value);
    if(!body.readuint(value)) return false;
    record.selection.grid = int(value);
    if(!body.readuint(value)) return false;
    record.selection.orient = int(value);
    if(!body.readuint(value)) return false;
    record.selection.corner = int(value);
    loopi(4)
    {
        if(!body.readuint(value)) return false;
        record.args[i] = int(value);
    }
    if(!body.readuint(count) || count > uint(body.remaining() / 42)) return false;
    loopi(count) if(!deserializeworlddiffnode(body, record.before.add())) return false;
    if(!body.readuint(count) || count > uint(body.remaining() / 42)) return false;
    loopi(count) if(!deserializeworlddiffnode(body, record.after.add())) return false;
    // Records written before persistent scatter support end after cube state.
    if(!body.remaining()) return true;
    if(!body.readuint(count) || count > uint(body.remaining() / 16)) return false;
    loopi(count)
        if(!deserializeworldscatterinstance(body, record.scatterbefore.add()))
            return false;
    if(!body.readuint(count) || count > uint(body.remaining() / 16)) return false;
    loopi(count)
        if(!deserializeworldscatterinstance(body, record.scatterafter.add()))
            return false;
    return body.remaining() == 0;
}

static void applyworldscatterchange(vector<worldscatterinstance> &scatter,
                                    const vector<worldscatterinstance> &before,
                                    const vector<worldscatterinstance> &after)
{
    loopv(before)
    {
        int index = scatter.find(before[i]);
        if(index >= 0) scatter.removeunordered(index);
    }
    loopv(after) if(scatter.find(after[i]) < 0) scatter.add(after[i]);
}

static ullong hashworlddiffbytes(ullong hash, const void *data, int length)
{
    const uchar *bytes = (const uchar *)data;
    loopi(length)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static ullong hashworlddiffcube(const cube &c, ullong hash)
{
    uchar children = c.children ? 1 : 0;
    hash = hashworlddiffbytes(hash, &children, sizeof(children));
    if(c.children)
    {
        loopi(8) hash = hashworlddiffcube(c.children[i], hash);
        return hash;
    }
    hash = hashworlddiffbytes(hash, c.edges, sizeof(c.edges));
    hash = hashworlddiffbytes(hash, c.texture, sizeof(c.texture));
    return hashworlddiffbytes(hash, &c.material, sizeof(c.material));
}

static ullong hashworldchunk(cube *root)
{
    ullong hash = 1469598103934665603ULL;
    loopi(8) hash = hashworlddiffcube(root[i], hash);
    return hash;
}

static bool sameworldcubeleaf(const cube &a, const cube &b)
{
    return !a.children && !b.children && a.material == b.material &&
           !memcmp(a.edges, b.edges, sizeof(a.edges)) &&
           !memcmp(a.texture, b.texture, sizeof(a.texture));
}

static bool worldsubtreematchesleaf(const cube &tree, const cube &leaf)
{
    if(!tree.children) return sameworldcubeleaf(tree, leaf);
    loopi(8) if(!worldsubtreematchesleaf(tree.children[i], leaf)) return false;
    return true;
}

static void collectworldchunkoverrides(const cube &current, const cube &base,
                                       const ivec &o, int size,
                                       vector<worlddiffnode> &overrides)
{
    if(!current.children)
    {
        if((!base.children && sameworldcubeleaf(current, base)) ||
           (base.children && worldsubtreematchesleaf(base, current)))
            return;
        copyworlddiffnode(current, o, size, ivec(0, 0, 0), overrides.add());
        return;
    }
    if(!base.children)
    {
        if(worldsubtreematchesleaf(current, base)) return;
        int childsize = size >> 1;
        loopi(8)
            collectworldchunkoverrides(current.children[i], base,
                                       ivec(i, o, childsize), childsize, overrides);
        return;
    }
    int childsize = size >> 1;
    loopi(8)
        collectworldchunkoverrides(current.children[i], base.children[i],
                                   ivec(i, o, childsize), childsize, overrides);
}

static bool applyworldchunkdiff(cube *root, int x, int y, const char *filename,
                                vector<worldscatterinstance> &scatter,
                                bool prepared, int &families,
                                ullong &revision, ullong &canonicalhash)
{
    revision = canonicalhash = 0;
    if(!filename || !*filename)
    {
        canonicalhash = hashworldchunk(root);
        return true;
    }
    stream *file = openrawfile(filename, "rb");
    if(!file)
    {
        canonicalhash = hashworldchunk(root);
        conoutf(CON_WARN, "could not open chunk diff %s", filename);
        return false;
    }
    stream::offset filelength = file->size();
    if(filelength <= 0 || filelength > INT_MAX)
    {
        delete file;
        canonicalhash = hashworldchunk(root);
        return false;
    }
    vector<uchar> contents;
    uchar *dst = contents.pad(int(filelength));
    bool readok = file->read(dst, size_t(filelength)) == size_t(filelength);
    delete file;
    if(!readok)
    {
        canonicalhash = hashworldchunk(root);
        return false;
    }

    worldchunkreader reader(contents.getbuf(), contents.length());
    bool valid = true;
    ullong expectedhash = 0;
    while(reader.remaining() >= 12)
    {
        char magic[4];
        uint length, checksum;
        if(!reader.read(magic, sizeof(magic)) || memcmp(magic, "CDF1", 4) ||
           !reader.readuint(length) || !reader.readuint(checksum) ||
           length > WORLD_DIFF_FRAME_MAX || length > uint(reader.remaining()))
        {
            valid = false;
            break;
        }
        const uchar *payloadbytes = reader.pos;
        reader.pos += length;
        if(worlddiffchecksum(payloadbytes, length) != checksum)
        {
            valid = false;
            continue;
        }
        worldchunkreader payload(payloadbytes, length);
        uchar type;
        uint saveversion, genversion, chunkx, chunky, chunkz, count;
        ullong framehash;
        if(!payload.readbyte(type) || !payload.readuint(saveversion) ||
           !payload.readuint(genversion) || !payload.readuint(chunkx) ||
           !payload.readuint(chunky) || !payload.readuint(chunkz) ||
           !payload.readullong(framehash) ||
           !payload.readuint(count) ||
           saveversion != WORLD_SAVE_FORMAT_VERSION ||
           genversion != WORLDGEN_VERSION || int(chunkx) != x || int(chunky) != y ||
           int(chunkz) != WORLD_DIFF_Z || (type != 1 && type != 2) ||
           count > 1000000U)
        {
            valid = false;
            continue;
        }
        if(framehash) expectedhash = framehash;
        else if(type == 2 && count) expectedhash = 0;
        loopi(count)
        {
            worldeditrecord record;
            if(!deserializeworldeditrecord(payload, record) ||
               record.chunkx != x || record.chunky != y ||
               record.chunkz != WORLD_DIFF_Z || record.revision <= revision)
            {
                valid = false;
                break;
            }
            loopv(record.after) applyworlddiffnode(root, record.after[i], prepared, families);
            applyworldscatterchange(scatter, record.scatterbefore,
                                    record.scatterafter);
            revision = record.revision;
        }
        if(payload.remaining()) valid = false;
    }
    if(reader.remaining()) valid = false;
    for(int i = scatter.length() - 1; i >= 0; --i)
        if(!validgeneratedworldscatter(root, scatter[i]))
            scatter.removeunordered(i);
    cacheworldscattertransforms(x, y, game::worldsettings().grassmaxoffset, scatter);
    canonicalhash = hashworldchunk(root);
    if(expectedhash && canonicalhash != expectedhash)
    {
        valid = false;
        conoutf(CON_ERROR,"chunk diff %s reconstructed hash " WORLD_ULL_FORMAT " but expected " WORLD_ULL_FORMAT, filename, canonicalhash, expectedhash);
    }
    if(!valid)
        conoutf(CON_WARN, "chunk diff %s has an incomplete or corrupt frame; valid revisions were recovered", filename);
    return valid;
}

static bool compactworldchunkdiff(worldchunk &chunk)
{
    if(!chunk.root || chunk.loading || chunk.corrupted) return false;
    flushworlddiffjournals(true);
    shutdownworlddiffwriter();
    if(worldchunkmounted(chunk) && !syncmountedworldchunk(chunk)) return false;

    cube *base = generateworldchunk(chunk.x, chunk.y);
    if(!base) return false;
    vector<worldscatterinstance> basescatter;
    generateworldscatter(base, chunk.x, chunk.y, game::worldsettings(),
                         basescatter);
    int families = 0;
    if(chunkremip)
    {
        remipworldchunk(chunk.root, false, families);
        remipworldchunk(base, false, families);
    }

    vector<worlddiffnode> overrides;
    loopi(8)
        collectworldchunkoverrides(chunk.root[i], base[i],
                                   ivec(i, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE),
                                   WORLD_CHUNK_ROOT_SIZE, overrides);
    ullong finalhash = hashworldchunk(chunk.root);
    freeocta(base);

    vector<worldscatterinstance> scatterremoved, scatteradded;
    loopv(basescatter) if(chunk.scatter.find(basescatter[i]) < 0)
        scatterremoved.add(basescatter[i]);
    loopv(chunk.scatter) if(basescatter.find(chunk.scatter[i]) < 0)
        scatteradded.add(chunk.scatter[i]);

    defformatstring(relative, "media/map/%s/chunks/%d_%d_%d.diff",
                    worldfolder, chunk.x, chunk.y, WORLD_DIFF_Z);
    path(relative);
    string finalpath;
    copystring(finalpath, findfile(relative, "wb"));
    worldchunkdiffstate *state = findworldchunkdiffstate(chunk.x, chunk.y, true);
    if(overrides.empty() && scatterremoved.empty() && scatteradded.empty())
    {
        remove(finalpath);
        state->journal.deletecontents();
        state->snapshotrevision = state->revision;
        state->canonicalhash = finalhash;
        chunk.saved = true;
        chunk.dirty = false;
        conoutf("chunk %d %d matches its generated base; removed its override",
                chunk.x, chunk.y);
        return true;
    }

    worldeditrecord snapshot;
    snapshot.chunkx = chunk.x;
    snapshot.chunky = chunk.y;
    snapshot.operation = WORLD_EDIT_SET_CUBE;
    snapshot.author = -1;
    snapshot.revision = state->revision;
    snapshot.timestamp = ullong(time(NULL));
    snapshot.after.move(overrides);
    snapshot.scatterbefore.move(scatterremoved);
    snapshot.scatterafter.move(scatteradded);
    vector<worldeditrecord *> records;
    records.add(&snapshot);
    vector<uchar> frame;
    makeworlddiffframe(frame, 1, chunk.x, chunk.y, records, finalhash);

    defformatstring(temprelative, "%s.tmp", relative);
    string temppath;
    copystring(temppath, findfile(temprelative, "wb"));
    stream *file = openrawfile(temprelative, "wb");
    bool written = file && file->write(frame.getbuf(), frame.length()) == size_t(frame.length());
    delete file;
    if(!written)
    {
        remove(temppath);
        conoutf(CON_ERROR, "could not write compacted chunk diff %s", temppath);
        return false;
    }
    if(rename(temppath, finalpath))
    {
        remove(finalpath);
        if(rename(temppath, finalpath))
        {
            remove(temppath);
            conoutf(CON_ERROR, "could not atomically publish compacted chunk diff %s", finalpath);
            return false;
        }
    }
    state->journal.deletecontents();
    state->snapshotrevision = state->revision;
    state->canonicalhash = finalhash;
    chunk.saved = true;
    chunk.dirty = false;
    conoutf("compacted chunk %d %d to %d sparse overrides (%d bytes)",
            chunk.x, chunk.y, snapshot.after.length(), frame.length());
    return true;
}

static void loadworldauditlog()
{
    defformatstring(relative, "media/map/%s/audit.log", worldfolder);
    stream *file = openfile(path(relative), "rb");
    if(!file) return;
    stream::offset length = file->size();
    if(length <= 0 || length > INT_MAX)
    {
        delete file;
        return;
    }
    vector<uchar> contents;
    uchar *bytes = contents.pad(int(length));
    bool readok = file->read(bytes, size_t(length)) == size_t(length);
    delete file;
    if(!readok) return;

    worldchunkreader reader(contents.getbuf(), contents.length());
    while(reader.remaining() >= 12)
    {
        char magic[4];
        uint framelength, framechecksum;
        if(!reader.read(magic, sizeof(magic)) || memcmp(magic, "CDF1", 4) ||
           !reader.readuint(framelength) || !reader.readuint(framechecksum) ||
           framelength > WORLD_DIFF_FRAME_MAX || framelength > uint(reader.remaining()))
            break;
        const uchar *payloadbytes = reader.pos;
        reader.pos += framelength;
        if(worlddiffchecksum(payloadbytes, framelength) != framechecksum) continue;
        worldchunkreader payload(payloadbytes, framelength);
        uchar type;
        uint saveversion, genversion, chunkx, chunky, chunkz, count;
        ullong ignoredhash;
        if(!payload.readbyte(type) || !payload.readuint(saveversion) ||
           !payload.readuint(genversion) || !payload.readuint(chunkx) ||
           !payload.readuint(chunky) || !payload.readuint(chunkz) ||
           !payload.readullong(ignoredhash) ||
           !payload.readuint(count) || type != 2 ||
           saveversion != WORLD_SAVE_FORMAT_VERSION ||
           genversion != WORLDGEN_VERSION || int(chunkz) != WORLD_DIFF_Z ||
           count > 1000000U)
            continue;
        worldchunkdiffstate *state =
            findworldchunkdiffstate(int(chunkx), int(chunky), true);
        loopi(count)
        {
            worldeditrecord *record = new worldeditrecord;
            if(!deserializeworldeditrecord(payload, *record))
            {
                delete record;
                break;
            }
            state->audit.add(record);
            state->revision = max(state->revision, record->revision);
            worldeditrevision = max(worldeditrevision, record->revision);
        }
    }
}

static cube *prepareworldchunk(worldchunkjob &job)
{
    ZoneScopedN("Chunks/Prepare");
    ZoneTextF("%d_%d", job.x, job.y);
    if(SDL_AtomicGet(&job.cancelled)) return NULL;
    {
        ZoneScopedN("Chunks/Generate base and apply diff");
        worldgencontext ctx(job.seed, job.grasstexture, job.grasssidetexture,
                            job.grassbottomtexture,
                            job.dirttexture, job.stonetexture, job.sandtexture, job.snowtexture,
                            job.woodtexture, job.leaftexture, true, job.remip,
                            job.settings, &job.cancelled);
        cube *root = generateworldchunk(job.x, job.y, ctx);
        job.families = ctx.families;
        job.optimized = ctx.optimized;
        if(!root) return NULL;
        generateworldscatter(root, job.x, job.y, job.settings, job.scatter);
        if(job.filename[0])
        {
            applyworldchunkdiff(root, job.x, job.y, job.filename, job.scatter,
                                true, job.families,
                                job.revision, job.canonicalhash);
            if(job.remip)
                job.optimized += remipworldchunk(root, true, job.families);
            job.canonicalhash = hashworldchunk(root);
            job.loaded = true;
        }
        else
        {
            job.loaded = false;
            job.canonicalhash = hashworldchunk(root);
        }
        return root;
    }
}

static bool loadworldchunks(const char *mname)
{
    ZoneScopedN("Chunks/Initialize streamed world");
    ZoneText(mname, strlen(mname));
    string mapname;
    validmapname(mapname, mname, NULL, "");
    loopi(strlen(mapname)) if(mapname[i] == '\\') mapname[i] = '/';
    char *slash = strrchr(mapname, '/');
    int currentx, currenty;
    if(!slash || !chunkcoords(slash + 1, currentx, currenty)) return false;
    *slash = '\0';
    worldspawnmetadata storedspawn;
    worlddiffmetadata metadata;
    int entryx, entryy;
    if(!loadworldmetadata(mapname, entryx, entryy, storedspawn, metadata)) return false;
    if(game::getworldseed() != metadata.seed ||
       currentworldparameterhash() != metadata.parameterhash)
    {
        conoutf(CON_ERROR,
                "world %s generator parameter hash does not match world.meta; refusing silent terrain changes",
                mapname);
        return false;
    }
    game::loadworldseed(metadata.seed);
    activeworldmetadata = metadata;

    cube *currentroot = worldroot;
    worldroot = NULL;
    copystring(worldfolder, mapname);
    if(!reconstructedworldscatterready)
    {
        cube *base = generateworldchunk(currentx, currenty);
        if(base)
        {
            generateworldscatter(base, currentx, currenty,
                                 game::worldsettings(),
                                 reconstructedworldscatter);
            defformatstring(diffname, "media/map/%s/chunks/%d_%d_%d.diff",
                            worldfolder, currentx, currenty, WORLD_DIFF_Z);
            path(diffname);
            const char *found = findfile(diffname, "rb");
            if(found && fileexists(found, "r"))
            {
                int families = 0;
                ullong revision = 0, canonicalhash = 0;
                applyworldchunkdiff(base, currentx, currenty, diffname,
                                    reconstructedworldscatter, false, families,
                                    revision, canonicalhash);
            }
            freeocta(base);
        }
    }
    reconstructedworldscatterready = false;
    activeworldchunk = 0;
    worldchunk &currentchunk =
        worldchunks.add(worldchunk(currentx, currenty, currentroot, false, true));
    currentchunk.scatter.move(reconstructedworldscatter);
    loadinitialworldchunks(currentx, currenty);

    worldfirstchunkx = currentx - WORLD_RUNTIME_CENTER;
    worldfirstchunky = currenty - WORLD_RUNTIME_CENTER;
    setvar("mapscale", WORLD_RUNTIME_SCALE, true, false);
    setvar("mapsize", WORLD_RUNTIME_SIZE, true, false);
    worldroot = newcubes(F_EMPTY);
    if(player)
    {
        player->o = vec((currentx - worldfirstchunkx) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        (currenty - worldfirstchunky) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        WORLD_GROUND_HEIGHT + player->eyeheight + 1);
    }
    rebuildworldchunks(currentx, currenty, currentx, currenty, true, false);
    conoutf("loaded infinite world %s around chunk %d_%d", worldfolder, currentx, currenty);
    return true;
}

bool save_world(const char *mname)
{
    if(!*mname) mname = game::getclientmap();
    setmapfilenames(*mname ? mname : "untitled");
    if(savebak) backup(ogzname, bakname);
    stream *f = openrawfile(ogzname, "wb");
    if(!f) { conoutf(CON_WARN, "could not write map to %s", ogzname); return false; }

    savemapprogress = 0;
    renderprogress(0, "saving lightweight octree...");

    mapheader hdr;
    memcpy(hdr.magic, "TMAP", 4);
    hdr.version = MAPVERSION;
    hdr.worldsize = worldsize;
    hdr.chunkx = hdr.chunky = INT_MIN;
    const char *forwardslash = strrchr(mname, '/'), *backslash = strrchr(mname, '\\'),
               *basename = !forwardslash ? backslash :
                           !backslash || forwardslash > backslash ? forwardslash : backslash;
    basename = basename ? basename + 1 : mname;
    chunkcoords(basename, hdr.chunkx, hdr.chunky);
    lilswap(&hdr.version, 4);
    f->write(&hdr, sizeof(hdr));

    savec(worldroot, ivec(0, 0, 0), worldsize>>1, f);

    delete f;
    conoutf("wrote lightweight octree %s", ogzname);
    return true;
}

static bool saveworldconfig()
{
    defformatstring(name, "media/map/%s/world.cfg", worldfolder);
    stream *f = openfile(path(name), "w");
    if(!f)
    {
        conoutf(CON_WARN, "could not write world configuration to %s", name);
        return false;
    }

    f->printf(
        "// Generated by newworld. Logical height 0 is local Z=%d.\n"
        "worldchunksize = %d\n"
        "worldgridpower = %d\n"
        "worldblocksize = %d\n"
        "worldminheight = %d\n"
        "worldmaxheight = %d\n"
        "worldinfinite = 1\n\n"
        "worldload\n\n",
        WORLD_GROUND_HEIGHT, WORLD_CHUNK_BLOCKS, WORLD_GRID_POWER, WORLD_BLOCK_SIZE,
        WORLD_MIN_HEIGHT, WORLD_MAX_HEIGHT
    );
    game::saveworldsettings(f);
    delete f;

    return true;
}

static worldspawnmetadata requestedworldspawn;
static bool hasrequestedworldspawn = false;
static bool preparedworldspawn = false;
static vec preparedworldspawnposition;
static float preparedworldspawnyaw = 0, preparedworldspawnpitch = 0;

static ullong currentworldparameterhash()
{
    game::worldsettings settings;
    return hashworlddiffbytes(1469598103934665603ULL, &settings,
                              sizeof(settings));
}

static bool saveworldmetadata(int chunkx, int chunky)
{
    if(activeworldmetadata.valid &&
       activeworldmetadata.worldgenversion != WORLDGEN_VERSION)
    {
        conoutf(CON_ERROR,
                "refusing to change worldgen version %d to %d for existing world %s",
                activeworldmetadata.worldgenversion, WORLDGEN_VERSION, worldfolder);
        return false;
    }
    defformatstring(name, "media/map/%s/world.meta", worldfolder);
    stream *f = openfile(path(name), "w");
    if(!f)
    {
        conoutf(CON_WARN, "could not write world metadata to %s", name);
        return false;
    }
    activeworldmetadata.seed = game::getworldseed();
    activeworldmetadata.worldgenversion = WORLDGEN_VERSION;
    activeworldmetadata.parameterhash = currentworldparameterhash();
    activeworldmetadata.saveformatversion = WORLD_SAVE_FORMAT_VERSION;
    activeworldmetadata.valid = true;
    f->printf("CUBECRAFT_WORLD 3\n");
    f->printf("world_seed %d\n", activeworldmetadata.seed);
    f->printf("worldgen_version %d\n", activeworldmetadata.worldgenversion);
    f->printf("worldgen_parameter_hash " WORLD_ULL_FORMAT "\n",
              activeworldmetadata.parameterhash);
    f->printf("save_format_version %d\n", activeworldmetadata.saveformatversion);
    f->printf("entry %d %d\n", chunkx, chunky);
    if(player)
    {
        const double absolutex = double(worldfirstchunkx) * WORLD_CHUNK_SIZE + player->o.x,
                     absolutey = double(worldfirstchunky) * WORLD_CHUNK_SIZE + player->o.y;
        f->printf("spawn %.17g %.17g %.9g %.9g %.9g\n", absolutex, absolutey, player->o.z, player->yaw, player->pitch);
    }
    delete f;
    return true;
}

static bool loadworldmetadata(const char *folder, int &chunkx, int &chunky,
                              worldspawnmetadata &spawn, worlddiffmetadata &metadata)
{
    chunkx = chunky = 0;
    spawn = worldspawnmetadata();
    metadata = worlddiffmetadata();
    defformatstring(name, "media/map/%s/world.meta", folder);
    stream *f = openfile(path(name), "r");
    if(!f) return false;
    int metarevision = 0;
    string line;
    while(f->getline(line, sizeof(line)))
    {
        int x, y;
        if(sscanf(line, "CUBECRAFT_WORLD %d", &metarevision) == 1) continue;
        if(sscanf(line, "world_seed %d", &metadata.seed) == 1) continue;
        if(sscanf(line, "worldgen_version %d", &metadata.worldgenversion) == 1) continue;
        static const char hashprefix[] = "worldgen_parameter_hash ";
        if(!strncmp(line, hashprefix, sizeof(hashprefix) - 1))
        {
            char *end = NULL;
            metadata.parameterhash = strtoull(line + sizeof(hashprefix) - 1, &end, 10);
            if(end != line + sizeof(hashprefix) - 1) continue;
        }
        if(sscanf(line, "save_format_version %d", &metadata.saveformatversion) == 1) continue;
        if(sscanf(line, "entry %d %d", &x, &y) == 2)
        {
            chunkx = x;
            chunky = y;
            continue;
        }
        double spawnx, spawny;
        float spawnz, yaw = 0, pitch = 0;
        if(sscanf(line, "spawn %lf %lf %f %f %f",
                  &spawnx, &spawny, &spawnz, &yaw, &pitch) >= 3)
        {
            spawn.valid = true;
            spawn.x = spawnx;
            spawn.y = spawny;
            spawn.z = spawnz;
            spawn.yaw = yaw;
            spawn.pitch = pitch;
        }
    }
    delete f;

    metadata.valid = metarevision == 3 && metadata.seed >= 0 &&
                     metadata.worldgenversion > 0 && metadata.saveformatversion > 0;
    if(!metadata.valid)
    {
        conoutf(CON_ERROR,
                "world %s uses legacy metadata without a pinned generator; explicit migration is required",
                folder);
        return false;
    }
    if(metadata.worldgenversion != WORLDGEN_VERSION)
    {
        conoutf(CON_ERROR,
                "world %s requires worldgen version %d, but this build provides version %d",
                folder, metadata.worldgenversion, WORLDGEN_VERSION);
        return false;
    }
    if(metadata.saveformatversion != WORLD_SAVE_FORMAT_VERSION)
    {
        conoutf(CON_ERROR, "world %s uses unsupported save format version %d",
                folder, metadata.saveformatversion);
        return false;
    }
    return true;
}

static bool dryworldspawnblock(const game::worldgenerator &generator,
                               const game::worldsettings &settings, int x, int y)
{
    const int height = generator.height(x, y);
    return height >= settings.sealevel && height <= WORLD_MAX_HEIGHT - 3;
}

static bool chooseworldspawn(double originx, double originy, double &spawnx, double &spawny)
{
    const int originblockx = int(floor(originx / WORLD_BLOCK_SIZE)),
              originblocky = int(floor(originy / WORLD_BLOCK_SIZE));
    game::worldsettings settings;
    game::worldgenerator generator(game::getworldseed(), settings);

    if(dryworldspawnblock(generator, settings, originblockx, originblocky))
    {
        spawnx = (double(originblockx) + 0.5) * WORLD_BLOCK_SIZE;
        spawny = (double(originblocky) + 0.5) * WORLD_BLOCK_SIZE;
        return true;
    }

    renderprogress(0.82f, "choosing a better spawn point because you had no chance...");

    int bestx = originblockx, besty = originblocky;
    long long bestdist = LLONG_MAX;

    // Search every nearby block first, then cover a continent-scale area on a
    // coarse grid. A final local pass turns the best coarse hit into a block-
    // precise dry spawn without evaluating millions of noise samples.
    const int exactradius = 64;
    for(int y = originblocky - exactradius; y <= originblocky + exactradius; ++y)
    for(int x = originblockx - exactradius; x <= originblockx + exactradius; ++x)
    {
        if(!dryworldspawnblock(generator, settings, x, y)) continue;
        const long long dx = x - originblockx, dy = y - originblocky,
                        dist = dx * dx + dy * dy;
        if(dist >= bestdist) continue;
        bestx = x;
        besty = y;
        bestdist = dist;
    }

    if(bestdist == LLONG_MAX)
    {
        const int searchradius = 8192, searchstep = 64;
        for(int y = originblocky - searchradius; y <= originblocky + searchradius; y += searchstep)
        for(int x = originblockx - searchradius; x <= originblockx + searchradius; x += searchstep)
        {
            if(!dryworldspawnblock(generator, settings, x, y)) continue;
            const long long dx = x - originblockx, dy = y - originblocky,
                            dist = dx * dx + dy * dy;
            if(dist >= bestdist) continue;
            bestx = x;
            besty = y;
            bestdist = dist;
        }
    }

    if(bestdist == LLONG_MAX) return false;

    {
        const int refine = 64;
        int refinedx = bestx, refinedy = besty;
        long long refineddist = bestdist;
        for(int y = besty - refine; y <= besty + refine; ++y)
        for(int x = bestx - refine; x <= bestx + refine; ++x)
        {
            if(!dryworldspawnblock(generator, settings, x, y)) continue;
            const long long dx = x - originblockx, dy = y - originblocky,
                            dist = dx * dx + dy * dy;
            if(dist >= refineddist) continue;
            refinedx = x;
            refinedy = y;
            refineddist = dist;
        }
        bestx = refinedx;
        besty = refinedy;
    }

    spawnx = (double(bestx) + 0.5) * WORLD_BLOCK_SIZE;
    spawny = (double(besty) + 0.5) * WORLD_BLOCK_SIZE;
    return true;
}

static bool mountworldspawncolumn(worldchunk &chunk, double absolutex, double absolutey)
{
    if(!chunk.root || chunk.loading || chunk.corrupted) return false;
    int localx = int(floor(absolutex - double(chunk.x) * WORLD_CHUNK_SIZE)),
        localy = int(floor(absolutey - double(chunk.y) * WORLD_CHUNK_SIZE));
    if(localx < 0 || localx >= WORLD_CHUNK_SIZE ||
       localy < 0 || localy >= WORLD_CHUNK_SIZE)
        return false;

    int tilex = localx / WORLD_SECTION_SIZE,
        tiley = localy / WORLD_SECTION_SIZE,
        tile = tiley * WORLD_SECTION_COLUMNS + tilex;
    loopi(WORLD_SECTION_LAYERS) mountworldchunktile(chunk, i, tile);
    return !chunk.corrupted;
}

static bool prepareworldspawn(const worldspawnmetadata &saved)
{
    if(!player || worldchunks.empty() || !worldroot) return false;

    renderprogress(0.78f, "waiting for ground...");

    double absolutex, absolutey;
    if(saved.valid)
    {
        absolutex = saved.x;
        absolutey = saved.y;
    }
    else
    {
        const worldchunk &entry = worldchunks.inrange(activeworldchunk)
                                ? worldchunks[activeworldchunk] : worldchunks[0];
        absolutex = double(entry.x) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2;
        absolutey = double(entry.y) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2;
        if(!chooseworldspawn(absolutex, absolutey, absolutex, absolutey))
        {
            conoutf(CON_ERROR, "could not find dry ground for the player spawn");
            return false;
        }
    }

    int chunkx = int(floor(absolutex / WORLD_CHUNK_SIZE)),
        chunky = int(floor(absolutey / WORLD_CHUNK_SIZE)),
        generated = 0,
        destination = acquireworldchunksync(chunkx, chunky, generated);
    if(!worldchunks.inrange(destination) || !worldchunks[destination].root)
    {
        conoutf(CON_ERROR, "could not prepare spawn chunk %d_%d", chunkx, chunky);
        return false;
    }

    rebaseworldchunks(chunkx, chunky, false);
    const float runtimex = float(absolutex - double(worldfirstchunkx) * WORLD_CHUNK_SIZE),
                runtimey = float(absolutey - double(worldfirstchunky) * WORLD_CHUNK_SIZE);
    player->o = vec(runtimex, runtimey, WORLD_MAP_SIZE - 1.0f);
    if(!mountworldspawncolumn(worldchunks[destination], absolutex, absolutey))
    {
        conoutf(CON_ERROR, "could not mount the geometry beneath the spawn point");
        return false;
    }

    vec sky(runtimex, runtimey, WORLD_MAP_SIZE - 1.0f);
    float grounddist = raycube(sky, vec(0, 0, -1), WORLD_MAP_SIZE, RAY_CLIPMAT);
    if(grounddist >= WORLD_MAP_SIZE)
    {
        conoutf(CON_ERROR, "could not find solid ground beneath the spawn point");
        return false;
    }
    const float groundz = sky.z - grounddist;

    if(saved.valid)
    {
        player->o = vec(runtimex, runtimey, clamp(saved.z, 0.0f, float(WORLD_MAP_SIZE - 1)));
        player->yaw = saved.yaw;
        player->pitch = saved.pitch;
        player->reset();
        player->resetinterp();
    }
    else
    {
        player->o = vec(runtimex, runtimey, groundz + player->eyeheight + 0.1f);
        player->yaw = player->pitch = 0;
        player->reset();
        player->resetinterp();
    }

    const int material = lookupmaterial(vec(player->o.x, player->o.y,
                                            max(player->o.z - player->eyeheight + 1, 0.0f)));
    if(!saved.valid && isliquid(material & MATF_VOLUME))
    {
        conoutf(CON_ERROR, "refusing to finish loading with the player spawn in water");
        return false;
    }

    lastworldchunkmotion = -1;
    worldchunkaheadx = chunkx;
    worldchunkaheady = chunky;
    worlddebugcachemillis = -1;
    rebuildworldchunks(chunkx, chunky, chunkx, chunky, true, false);

    preparedworldspawnposition = player->o;
    preparedworldspawnyaw = player->yaw;
    preparedworldspawnpitch = player->pitch;
    preparedworldspawn = true;
    renderprogress(0.9f, "ground found - putting your boots on...");
    return true;
}

static void applypreparedworldspawn()
{
    if(!preparedworldspawn || !player) return;
    player->o = preparedworldspawnposition;
    player->yaw = preparedworldspawnyaw;
    player->pitch = preparedworldspawnpitch;
    player->reset();
    player->resetinterp();
}

void saveworld();

static void createworld(const char *requestedname)
{
    chooseworldfolder(requestedname);
    string chosenfolder, activechunkname;
    copystring(chosenfolder, worldfolder);
    formatstring(activechunkname, "%s/0_0", chosenfolder);

    defformatstring(metadatafile, "media/map/%s/world.meta", chosenfolder);
    path(metadatafile);
    const char *existingmetadata = findfile(metadatafile, "rb");
    if(existingmetadata && fileexists(existingmetadata, "r"))
    {
        conoutf(CON_ERROR,
                "world %s already exists; use loadworld %s or choose a new name",
                chosenfolder, chosenfolder);
        return;
    }

    // Snapshot the menu/console seed before loading or resetting anything.
    // The active seed belongs to the currently loaded world and must not be
    // reused implicitly when creating a differently named world.
    const int chosenworldseed = game::getconfiguredworldseed();
    game::beginlocalworld();
    if(!emptymap(WORLD_RUNTIME_SCALE, true, activechunkname)) return;
    copystring(worldfolder, chosenfolder);
    worldfirstchunkx = worldfirstchunky = -WORLD_RUNTIME_CENTER;
    if(!loadworlddefinitions()) return;
    game::loadworldseed(chosenworldseed);

    freeocta(worldroot);
    worldroot = NULL;
    activeworldchunk = worldchunks.length();
    {
        worldchunk &chunk =
            worldchunks.add(worldchunk(0, 0, generateworldchunk(0, 0)));
        generateworldscatter(chunk.root, 0, 0, game::worldsettings(),
                             chunk.scatter);
    }
    loadinitialworldchunks(0, 0);

    setvar("mapscale", WORLD_RUNTIME_SCALE, true, false);
    setvar("mapsize", WORLD_RUNTIME_SIZE, true, false);
    worldroot = newcubes(F_EMPTY);
    if(player)
    {
        player->o = vec((0 - worldfirstchunkx) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        (0 - worldfirstchunky) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        WORLD_GROUND_HEIGHT + player->eyeheight + 1);
    }
    preparedworldspawn = false;
    worldspawnmetadata newspawn;
    if(!prepareworldspawn(newspawn)) return;
    updateworldchunks(true);
    applypreparedworldspawn();

    renderprogress(0.94f, "saving your new home before handing over the keys...");
    saveworld();

    int mounted = 0;
    loopv(worldchunks) if(worldchunkmounted(worldchunks[i])) mounted++;
    conoutf("generated infinite world %s with seed %d and %d initial chunks; %d chunks queued asynchronously",
            worldfolder, game::getworldseed(), mounted, worldchunks.length() - mounted);
    conoutf("new chunks are prepared on demand; use saveworld to write ready chunks");
}

ICOMMAND(newworld, "ssN", (char *arg1, char *arg2, int *numargs),
{
    const char *name = NULL;
    if(*numargs > 0)
    {
        char *end = NULL;
        strtol(arg1, &end, 10);
        bool legacysize = end != arg1 && !*end;
        if(legacysize)
        {
            conoutf(CON_WARN, "newworld size is no longer used; the world expands on demand");
            if(*numargs > 1) name = arg2;
        }
        else name = arg1;
    }
    createworld(name);
});

static void loadworldcommand(const char *requested)
{
    if(!requested || !*requested)
    {
        conoutf(CON_ERROR, "usage: loadworld <worldname>");
        return;
    }

    string folder;
    normalizeworldfolder(folder, sizeof(folder), requested);
    int chunkx, chunky;
    worldspawnmetadata spawn;
    worlddiffmetadata metadata;
    if(!loadworldmetadata(folder, chunkx, chunky, spawn, metadata))
    {
        conoutf(CON_ERROR, "could not find a saved world named %s", folder);
        return;
    }

    game::beginlocalworld();
    game::loadworldseed(metadata.seed);
    activeworldmetadata = metadata;
    conoutf("loading saved world %s with pinned seed %d", folder, metadata.seed);
    defformatstring(entry, "%s/%d_%d", folder, chunkx, chunky);
    requestedworldspawn = spawn;
    hasrequestedworldspawn = true;
    applyloadworlddefaults = true;
    game::changemap(entry);
    applyloadworlddefaults = false;
    hasrequestedworldspawn = false;
}

void startnetworkworld(int seed)
{
    game::loadworldseed(seed);
    if(!emptymap(WORLD_RUNTIME_SCALE, true, "network/0_0", true, false)) return;
    worldfolder[0] = '\0';
    worldfirstchunkx = worldfirstchunky = -WORLD_RUNTIME_CENTER;
    if(!loadworlddefinitions()) return;

    freeocta(worldroot);
    worldroot = NULL;
    activeworldchunk = worldchunks.length();
    {
        worldchunk &chunk =
            worldchunks.add(worldchunk(0, 0, generateworldchunk(0, 0)));
        generateworldscatter(chunk.root, 0, 0, game::worldsettings(),
                             chunk.scatter);
    }
    loadinitialworldchunks(0, 0);

    setvar("mapscale", WORLD_RUNTIME_SCALE, true, false);
    setvar("mapsize", WORLD_RUNTIME_SIZE, true, false);
    worldroot = newcubes(F_EMPTY);
    if(player)
    {
        player->o = vec(WORLD_RUNTIME_CENTER * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        WORLD_RUNTIME_CENTER * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        WORLD_GROUND_HEIGHT + player->eyeheight + 1);
    }
    preparedworldspawn = false;
    worldspawnmetadata spawn;
    if(!prepareworldspawn(spawn)) return;
    updateworldchunks(true);
    applypreparedworldspawn();
    conoutf("joined authoritative world with seed %d", seed);
}

ICOMMAND(loadworld, "s", (char *name), loadworldcommand(name));

void saveworld()
{
    if(worldchunks.empty() || activeworldchunk < 0)
    {
        conoutf(CON_ERROR, "no procedural world is active; use newworld first");
        return;
    }

    if(!saveworldconfig()) return;
    flushworlddiffjournals(true);
    loopv(worldchunkdiffstates) if(!worldchunkdiffstates[i]->journal.empty())
    {
        int chunkindex = findworldchunk(worldchunkdiffstates[i]->x,
                                        worldchunkdiffstates[i]->y);
        if(worldchunks.inrange(chunkindex))
            compactworldchunkdiff(worldchunks[chunkindex]);
    }
    int written = 0, unchanged = 0, ready = 0;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(!chunk.root || chunk.loading) continue;
        ready++;
        if(!chunk.dirty)
        {
            unchanged++;
            continue;
        }
        chunk.saved = true;
        chunk.dirty = false;
        written++;
    }

    int entryx = lastplayerchunkx, entryy = lastplayerchunky;
    if(player)
    {
        const double absolutex = double(worldfirstchunkx) * WORLD_CHUNK_SIZE + player->o.x,
                     absolutey = double(worldfirstchunky) * WORLD_CHUNK_SIZE + player->o.y;
        entryx = int(floor(absolutex / WORLD_CHUNK_SIZE));
        entryy = int(floor(absolutey / WORLD_CHUNK_SIZE));
    }
    int entry = findworldchunk(entryx, entryy);
    if(!worldchunks.inrange(entry)) entry = activeworldchunk;
    if(worldchunks.inrange(entry))
    {
        entryx = worldchunks[entry].x;
        entryy = worldchunks[entry].y;
    }
    if(!saveworldmetadata(entryx, entryy)) return;

    int released = 0;
    for(int i = worldchunks.length() - 1; i >= 0; --i)
    {
        worldchunk &chunk = worldchunks[i];
        if(worldchunkmounted(chunk) || chunk.loading || !chunk.root ||
           worldchunkinview(chunk, lastplayerchunkx, lastplayerchunky))
            continue;
        freeocta(chunk.root);
        worldchunks.removeunordered(i);
        released++;
    }
    activeworldchunk = findworldchunk(lastplayerchunkx, lastplayerchunky);
    string name;
    if(worldchunks.inrange(activeworldchunk))
    {
        worldchunkname(name, sizeof(name), worldchunks[activeworldchunk]);
        setmapfilenames(name);
    }
    conoutf("saved world %s: %d chunk journals queued, %d unchanged, %d ready; released %d cached chunks",
            worldfolder, written, unchanged, ready, released);
}

void closeproceduralworld(bool save)
{
    // Save while the active folder, mounted chunks and diff states still
    // identify the world. clearworldchunks() then flushes and joins both the
    // diff writer and generation workers before releasing their state.
    if(save && !worldchunks.empty() && activeworldchunk >= 0) saveworld();
    clearworldchunks();
    resetmap();
    freeocta(worldroot);
    worldroot = newcubes(F_SOLID);
}

COMMAND(saveworld, "");

static const char *worldeditoperationname(int operation)
{
    switch(operation)
    {
        case WORLD_EDIT_SET_CUBE: return "SET_CUBE";
        case WORLD_EDIT_DELETE_CUBE: return "DELETE_CUBE";
        case WORLD_EDIT_SET_MATERIAL: return "SET_MATERIAL";
        case WORLD_EDIT_MOVE_CORNER: return "MOVE_CORNER";
        case WORLD_EDIT_FILL_VOLUME: return "FILL_VOLUME";
        case WORLD_EDIT_DELETE_VOLUME: return "DELETE_VOLUME";
        case WORLD_EDIT_PASTE_BLUEPRINT: return "PASTE_BLUEPRINT";
        case WORLD_EDIT_DELETE_BLUEPRINT: return "DELETE_BLUEPRINT";
        case WORLD_EDIT_SET_SCATTER: return "SET_SCATTER";
        case WORLD_EDIT_DELETE_SCATTER: return "DELETE_SCATTER";
        default: return "UNKNOWN";
    }
}

static void pasteworlddiffnode(cube &c, const worlddiffnode &node)
{
    discardchildren(c);
    memcpy(c.edges, node.edges, sizeof(node.edges));
    memcpy(c.texture, node.texture, sizeof(node.texture));
    c.material = node.material;
    c.visible = c.merged = 0;
}

static bool commitworldadminrecord(const worldeditrecord &source, bool inverse)
{
    int chunkindex = findworldchunk(source.chunkx, source.chunky);
    if(!worldchunks.inrange(chunkindex))
    {
        int generated = 0;
        chunkindex = acquireworldchunksync(source.chunkx, source.chunky, generated);
    }
    if(!worldchunks.inrange(chunkindex)) return false;
    worldchunk &chunk = worldchunks[chunkindex];
    const vector<worlddiffnode> &target = inverse ? source.before : source.after;
    const vector<worlddiffnode> &oldstate = inverse ? source.after : source.before;
    const vector<worldscatterinstance> &scattertarget =
        inverse ? source.scatterbefore : source.scatterafter;
    const vector<worldscatterinstance> &scatterold =
        inverse ? source.scatterafter : source.scatterbefore;
    int families = 0;
    loopv(target)
    {
        const worlddiffnode &node = target[i];
        applyworlddiffnode(chunk.root, node, false, families);
        if(worldchunkmounted(chunk))
        {
            ivec pos = ivec(worldchunkorigin(chunk)).add(ivec(node.x, node.y, node.z));
            cube &runtimecube = lookupcube(pos, node.size);
            pasteworlddiffnode(runtimecube, node);
            changed(pos, ivec(pos).add(node.size), false);
        }
    }
    applyworldscatterchange(chunk.scatter, scatterold, scattertarget);
    cacheworldscattertransforms(chunk.x, chunk.y, game::worldsettings().grassmaxoffset, chunk.scatter);
    commitchanges();

    worldchunkdiffstate *state = findworldchunkdiffstate(chunk.x, chunk.y, true);
    worldeditrecord record;
    record.chunkx = chunk.x;
    record.chunky = chunk.y;
    record.operation = inverse ? WORLD_EDIT_DELETE_BLUEPRINT : WORLD_EDIT_PASTE_BLUEPRINT;
    record.author = worldeditauthor;
    record.revision = ++worldeditrevision;
    state->revision = max(state->revision, record.revision);
    record.timestamp = ullong(time(NULL));
    record.args[0] = int(source.revision & 0xFFFFFFFFU);
    record.args[1] = int(source.revision >> 32);
    record.args[2] = inverse ? 1 : 2;
    record.args[3] = INT_MIN;
    record.selection = source.selection;
    loopv(oldstate) record.before.add(oldstate[i]);
    loopv(target) record.after.add(target[i]);
    loopv(scatterold) record.scatterbefore.add(scatterold[i]);
    loopv(scattertarget) record.scatterafter.add(scattertarget[i]);
    state->pending.add(cloneworldeditrecord(record));
    state->journal.add(cloneworldeditrecord(record));
    state->audit.add(cloneworldeditrecord(record));
    state->canonicalhash = hashworldchunk(chunk.root);
    chunk.dirty = true;
    updateworldscatterers();
    return true;
}

static bool worldauditrecordundone(const worldeditrecord &source)
{
    worldchunkdiffstate *state = findworldchunkdiffstate(source.chunkx, source.chunky);
    if(!state) return false;
    int status = 0;
    loopv(state->audit)
    {
        const worldeditrecord &record = *state->audit[i];
        if(record.args[3] != INT_MIN) continue;
        ullong referenced = uint(record.args[0]) | (ullong(uint(record.args[1])) << 32);
        if(referenced == source.revision) status = record.args[2];
    }
    return status == 1;
}

static worldeditrecord *latestworldauditrecord()
{
    worldeditrecord *latest = NULL;
    loopv(worldchunkdiffstates) loopvj(worldchunkdiffstates[i]->audit)
    {
        worldeditrecord *record = worldchunkdiffstates[i]->audit[j];
        if(record->args[3] == INT_MIN) continue;
        if(worldauditrecordundone(*record)) continue;
        if(!latest || record->timestamp > latest->timestamp ||
           (record->timestamp == latest->timestamp && record->revision > latest->revision))
            latest = record;
    }
    return latest;
}

static void worldundocommand(int *requested)
{
    if(game::waitforserveredit())
    {
        defformatstring(command, "worldundo %d", max(*requested, 1));
        game::requestworldcommand(command);
        return;
    }
    int count = max(*requested, 1), applied = 0;
    while(applied < count)
    {
        worldeditrecord *record = latestworldauditrecord();
        if(!record || !commitworldadminrecord(*record, true)) break;
        worldredostack.add(cloneworldeditrecord(*record));
        applied++;
    }
    conoutf("worldundo committed %d inverse revision%s", applied, applied == 1 ? "" : "s");
}

static void worldredocommand(int *requested)
{
    if(game::waitforserveredit())
    {
        defformatstring(command, "worldredo %d", max(*requested, 1));
        game::requestworldcommand(command);
        return;
    }
    int count = max(*requested, 1), applied = 0;
    while(applied < count)
    {
        worldeditrecord *record = NULL;
        bool owned = false;
        if(!worldredostack.empty())
        {
            record = worldredostack.pop();
            owned = true;
        }
        else loopv(worldchunkdiffstates)
        {
            worldchunkdiffstate &state = *worldchunkdiffstates[i];
            loopvj(state.audit)
            {
                worldeditrecord *candidate = state.audit[j];
                if(candidate->args[3] == INT_MIN || !worldauditrecordundone(*candidate)) continue;
                if(!record || candidate->timestamp > record->timestamp ||
                   (candidate->timestamp == record->timestamp &&
                    candidate->revision > record->revision))
                    record = candidate;
            }
        }
        if(!record) break;
        if(commitworldadminrecord(*record, false)) applied++;
        if(owned) delete record;
    }
    conoutf("worldredo committed %d new revision%s", applied, applied == 1 ? "" : "s");
}

COMMANDN(worldundo, worldundocommand, "i");
COMMANDN(worldredo, worldredocommand, "i");

static void worldlogcommand(char *playertext, int *radius, int *minutes)
{
    if(game::waitforserveredit())
    {
        defformatstring(command, "worldlog %s %d %d",
                        playertext ? playertext : "", *radius, *minutes);
        game::requestworldcommand(command);
        return;
    }
    int author = playertext && playertext[0] ? game::findclientnum(playertext) : INT_MIN,
        seconds = *minutes > 0 ? *minutes * 60 : INT_MAX, shown = 0;
    if(playertext && playertext[0] && author < 0)
    {
        conoutf(CON_ERROR, "worldlog: unknown player %s", playertext);
        return;
    }
    ullong now = ullong(time(NULL));
    loopv(worldchunkdiffstates) loopvj(worldchunkdiffstates[i]->audit)
    {
        const worldeditrecord &record = *worldchunkdiffstates[i]->audit[j];
        if(author != INT_MIN && record.author != author) continue;
        if(now > record.timestamp && now - record.timestamp > ullong(seconds)) continue;
        if(*radius > 0 && player)
        {
            int chunkindex = findworldchunk(record.chunkx, record.chunky);
            if(!worldchunks.inrange(chunkindex)) continue;
            ivec origin = worldchunkorigin(worldchunks[chunkindex]);
            if(abs(origin.x - int(player->o.x)) > *radius ||
               abs(origin.y - int(player->o.y)) > *radius)
                continue;
        }
        conoutf("chunk %d %d rev " WORLD_ULL_FORMAT " author %d time "
                WORLD_ULL_FORMAT " %s (%d nodes)",
                record.chunkx, record.chunky, record.revision, record.author,
                record.timestamp, worldeditoperationname(record.operation),
                record.after.length());
        shown++;
    }
    conoutf("worldlog: %d matching revision%s", shown, shown == 1 ? "" : "s");
}

COMMANDN(worldlog, worldlogcommand, "sii");

static void worldrevertcommand(char *mode, char *arg1, char *arg2, char *arg3,
                               char *arg4, char *arg5, char *arg6, char *arg7)
{
    if(game::waitforserveredit())
    {
        defformatstring(command, "worldrevert %s %s %s %s %s %s %s %s",
                        mode, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
        game::requestworldcommand(command);
        return;
    }
    int reverted = 0;
    ullong now = ullong(time(NULL));
    if(!strcmp(mode, "player"))
    {
        int author = game::findclientnum(arg1),
            minutes = arg2[0] ? max(atoi(arg2), 0) : 0;
        if(author < 0)
        {
            conoutf(CON_ERROR, "worldrevert: unknown player %s", arg1);
            return;
        }
        loopv(worldchunkdiffstates)
        {
            worldchunkdiffstate &state = *worldchunkdiffstates[i];
            for(int j = state.audit.length() - 1; j >= 0; --j)
            {
                worldeditrecord &record = *state.audit[j];
                if(record.args[3] == INT_MIN || record.author != author) continue;
                if(minutes > 0 && now > record.timestamp &&
                   now - record.timestamp > ullong(minutes * 60))
                    continue;
                if(commitworldadminrecord(record, true)) reverted++;
            }
        }
    }
    else if(!strcmp(mode, "area"))
    {
        int x1 = atoi(arg1), y1 = atoi(arg2), z1 = atoi(arg3),
            x2 = atoi(arg4), y2 = atoi(arg5), z2 = atoi(arg6),
            minutes = arg7[0] ? max(atoi(arg7), 0) : 0;
        if(x1 > x2) swap(x1, x2);
        if(y1 > y2) swap(y1, y2);
        if(z1 > z2) swap(z1, z2);
        loopv(worldchunkdiffstates)
        {
            worldchunkdiffstate &state = *worldchunkdiffstates[i];
            for(int j = state.audit.length() - 1; j >= 0; --j)
            {
                worldeditrecord &record = *state.audit[j];
                if(record.args[3] == INT_MIN) continue;
                if(minutes > 0 && now > record.timestamp &&
                   now - record.timestamp > ullong(minutes * 60))
                    continue;
                bool intersects = false;
                loopvk(record.after)
                {
                    const worlddiffnode &node = record.after[k];
                    int nx = record.chunkx * WORLD_CHUNK_SIZE + node.x,
                        ny = record.chunky * WORLD_CHUNK_SIZE + node.y;
                    if(nx + node.size > x1 && nx < x2 &&
                       ny + node.size > y1 && ny < y2 &&
                       node.z + node.size > z1 && node.z < z2)
                    {
                        intersects = true;
                        break;
                    }
                }
                if(intersects && commitworldadminrecord(record, true)) reverted++;
            }
        }
    }
    else
    {
        conoutf(CON_ERROR,
                "usage: /worldrevert player <id> [minutes] | area <x1 y1 z1> <x2 y2 z2> [minutes]");
        return;
    }
    conoutf("worldrevert committed %d inverse revision%s",
            reverted, reverted == 1 ? "" : "s");
}

COMMANDN(worldrevert, worldrevertcommand, "ssssssss");

static void worldrestorecommand(char *kind, char *xtext, char *ytext,
                                char *ztext, char *revisiontext)
{
    if(game::waitforserveredit())
    {
        defformatstring(command, "worldrestore %s %s %s %s %s",
                        kind, xtext, ytext, ztext, revisiontext);
        game::requestworldcommand(command);
        return;
    }
    if(strcmp(kind, "chunk"))
    {
        conoutf(CON_ERROR, "usage: /worldrestore chunk <x y z> <revision>");
        return;
    }
    int x = atoi(xtext), y = atoi(ytext), z = atoi(ztext);
    ullong revision = strtoull(revisiontext, NULL, 10);
    if(z != WORLD_DIFF_Z)
    {
        conoutf(CON_ERROR, "this world stores its full vertical band as chunk z=0");
        return;
    }
    worldchunkdiffstate *state = findworldchunkdiffstate(x, y);
    if(!state)
    {
        conoutf(CON_ERROR, "chunk %d %d has no revision history", x, y);
        return;
    }
    int restored = 0;
    for(int i = state->audit.length() - 1; i >= 0; --i)
    {
        worldeditrecord &record = *state->audit[i];
        if(record.args[3] == INT_MIN || record.revision <= revision) continue;
        if(commitworldadminrecord(record, true)) restored++;
    }
    conoutf("worldrestore chunk %d %d to revision " WORLD_ULL_FORMAT
            " committed %d inverse revision%s",
            x, y, revision, restored, restored == 1 ? "" : "s");
}

COMMANDN(worldrestore, worldrestorecommand, "sssss");

static void worlddiffcommand(char *action, char *xtext, char *ytext, char *ztext)
{
    if(game::waitforserveredit())
    {
        defformatstring(command, "worlddiff %s %s %s %s",
                        action ? action : "", xtext ? xtext : "",
                        ytext ? ytext : "", ztext ? ztext : "");
        game::requestworldcommand(command);
        return;
    }
    if(!action || !action[0])
    {
        conoutf(CON_ERROR, "usage: /worlddiff <stats|compact|verify> [x y z|all]");
        return;
    }
    bool all = xtext && !strcmp(xtext, "all");
    int x = xtext && xtext[0] && !all ? atoi(xtext) : lastplayerchunkx,
        y = ytext && ytext[0] ? atoi(ytext) : lastplayerchunky,
        z = ztext && ztext[0] ? atoi(ztext) : WORLD_DIFF_Z;
    if(z != WORLD_DIFF_Z)
    {
        conoutf(CON_ERROR, "this world stores its full vertical band as chunk z=0");
        return;
    }
    if(!strcmp(action, "stats"))
    {
        worldchunkdiffstate *state = findworldchunkdiffstate(x, y);
        if(!state)
        {
            conoutf("chunk %d %d %d: generated base only, revision 0, zero disk override", x, y, z);
            return;
        }
        conoutf("chunk %d %d %d: revision " WORLD_ULL_FORMAT ", snapshot "
                WORLD_ULL_FORMAT ", %d pending, %d journal, %d audit, hash "
                WORLD_ULL_FORMAT,
                x, y, z, state->revision, state->snapshotrevision,
                state->pending.length(), state->journal.length(), state->audit.length(),
                state->canonicalhash);
        return;
    }
    if(!strcmp(action, "compact"))
    {
        int compacted = 0;
        loopv(worldchunks)
        {
            worldchunk &chunk = worldchunks[i];
            if((all || (chunk.x == x && chunk.y == y)) && compactworldchunkdiff(chunk))
                compacted++;
        }
        conoutf("worlddiff compact: %d chunk%s", compacted, compacted == 1 ? "" : "s");
        return;
    }
    if(!strcmp(action, "verify"))
    {
        int verified = 0, failed = 0;
        flushworlddiffjournals(true);
        shutdownworlddiffwriter();
        loopv(worldchunks)
        {
            worldchunk &chunk = worldchunks[i];
            if(!all && (chunk.x != x || chunk.y != y)) continue;
            if(worldchunkmounted(chunk) && !syncmountedworldchunk(chunk))
            {
                failed++;
                continue;
            }
            ullong livehash = hashworldchunk(chunk.root);
            cube *reconstructed = generateworldchunk(chunk.x, chunk.y);
            if(!reconstructed)
            {
                failed++;
                continue;
            }
            defformatstring(relative, "media/map/%s/chunks/%d_%d_%d.diff",
                            worldfolder, chunk.x, chunk.y, WORLD_DIFF_Z);
            path(relative);
            const char *found = findfile(relative, "rb");
            string filename;
            filename[0] = '\0';
            if(found && fileexists(found, "r")) copystring(filename, relative);
            int families = 0;
            ullong revision = 0, reconstructedhash = 0;
            vector<worldscatterinstance> reconstructedscatter;
            generateworldscatter(reconstructed, chunk.x, chunk.y,
                                 game::worldsettings(), reconstructedscatter);
            bool valid = applyworldchunkdiff(reconstructed, chunk.x, chunk.y,
                                             filename, reconstructedscatter,
                                             false, families,
                                             revision, reconstructedhash);
            if(chunkremip) remipworldchunk(reconstructed, false, families);
            reconstructedhash = hashworldchunk(reconstructed);
            freeocta(reconstructed);
            if(!valid || livehash != reconstructedhash ||
               !sameworldscatterlist(chunk.scatter, reconstructedscatter))
                failed++;
            else
            {
                worldchunkdiffstate *state =
                    findworldchunkdiffstate(chunk.x, chunk.y, true);
                state->revision = max(state->revision, revision);
                worldeditrevision = max(worldeditrevision, revision);
                state->canonicalhash = livehash;
                verified++;
            }
        }
        conoutf("worlddiff verify: %d verified, %d mismatched", verified, failed);
        return;
    }
    conoutf(CON_ERROR, "unknown worlddiff action %s", action);
}

COMMANDN(worlddiff, worlddiffcommand, "ssss");

static uint mapcrc = 0;

uint getmapcrc() { return mapcrc; }
void clearmapcrc() { mapcrc = 0; }

static bool loadseedworld(const char *mname, const char *cname)
{
    string folder, normalized;
    copystring(normalized, mname);
    loopi(strlen(normalized)) if(normalized[i] == '\\') normalized[i] = '/';
    char *slash = strrchr(normalized, '/');
    int chunkx, chunky;
    if(!slash || !chunkcoords(slash + 1, chunkx, chunky)) return false;
    *slash = '\0';
    copystring(folder, normalized);

    worldspawnmetadata spawn;
    worlddiffmetadata metadata;
    int entryx, entryy;
    if(!loadworldmetadata(folder, entryx, entryy, spawn, metadata) ||
       entryx != chunkx || entryy != chunky)
        return false;

    setmapfilenames(mname, cname);
    clearworldchunks();
    resetmap();
    activeworldmetadata = metadata;
    game::loadworldseed(metadata.seed);

    identflags |= IDF_OVERRIDDEN;
    execfile("config/default_map_settings.cfg", false);
    defformatstring(worldconfig, "media/map/%s/world.cfg", folder);
    if(!execfile(worldconfig, false))
    {
        identflags &= ~IDF_OVERRIDDEN;
        conoutf(CON_ERROR, "could not load deterministic world configuration %s", worldconfig);
        return false;
    }
    identflags &= ~IDF_OVERRIDDEN;
    if(game::getworldseed() != metadata.seed ||
       currentworldparameterhash() != metadata.parameterhash)
    {
        conoutf(CON_ERROR,
                "world %s generator parameter hash does not match world.meta; refusing silent terrain changes",
                folder);
        return false;
    }

    setvar("mapscale", WORLD_CHUNK_SCALE, true, false);
    setvar("mapsize", WORLD_CHUNK_MAP_SIZE, true, false);
    texmru.shrink(0);
    freeocta(worldroot);
    worldroot = generateworldchunk(chunkx, chunky);
    if(!worldroot) return false;
    generateworldscatter(worldroot, chunkx, chunky, game::worldsettings(),
                         reconstructedworldscatter);
    reconstructedworldscatterready = true;

    defformatstring(diffrelative, "media/map/%s/chunks/%d_%d_%d.diff",
                    folder, chunkx, chunky, WORLD_DIFF_Z);
    path(diffrelative);
    const char *found = findfile(diffrelative, "rb");
    if(found && fileexists(found, "r"))
    {
        int families = 0;
        ullong revision = 0, canonicalhash = 0;
        applyworldchunkdiff(worldroot, chunkx, chunky, diffrelative,
                            reconstructedworldscatter, false, families,
                            revision, canonicalhash);
        if(chunkremip) remipworldchunk(worldroot, false, families);
        worldchunkdiffstate *state = findworldchunkdiffstate(chunkx, chunky, true);
        state->revision = revision;
        worldeditrevision = max(worldeditrevision, revision);
        state->canonicalhash = hashworldchunk(worldroot);
    }

    preparedworldspawn = false;
    requestedworldspawn = spawn;
    hasrequestedworldspawn = true;
    if(!loadworldchunks(mname) || !prepareworldspawn(spawn))
    {
        hasrequestedworldspawn = false;
        return false;
    }
    loadworldauditlog();
    hasrequestedworldspawn = false;
    allchanged(true);
    clearmainmenu();
    startmap(cname ? cname : mname);
    applypreparedworldspawn();
    mapcrc = 0;
    conoutf("reconstructed world %s from seed %d and chunk diffs", folder, metadata.seed);
    return true;
}

bool load_world(const char *mname, const char *cname)
{
    ZoneScopedN("Chunks/Load entry map");
    ZoneText(mname, strlen(mname));
    int loadingstart = SDL_GetTicks();
    setmapfilenames(mname, cname);
    stream *f;
    {
        ZoneScopedN("Chunks/Open entry map");
        f = openrawfile(ogzname, "rb");
    }
    if(!f)
    {
        if(loadseedworld(mname, cname)) return true;
        conoutf(CON_ERROR, "could not read map %s or reconstruct a seed-based world", ogzname);
        return false;
    }

    mapheader hdr;
    {
        ZoneScopedN("Chunks/Read entry header");
        if(!loadmapheader(f, ogzname, hdr)) { delete f; return false; }
        const char *forwardslash = strrchr(mname, '/'), *backslash = strrchr(mname, '\\'),
                   *basename = !forwardslash ? backslash :
                               !backslash || forwardslash > backslash ? forwardslash : backslash;
        basename = basename ? basename + 1 : mname;
        int expectedx, expectedy;
        if(chunkcoords(basename, expectedx, expectedy) &&
           (hdr.chunkx != expectedx || hdr.chunky != expectedy))
        {
            conoutf(CON_ERROR, "map %s identifies itself as chunk %d_%d, expected %d_%d",
                    ogzname, hdr.chunkx, hdr.chunky, expectedx, expectedy);
            delete f;
            return false;
        }
    }

    {
        ZoneScopedN("Chunks/Reset previous world");
        clearworldchunks();
        resetmap();
    }

    Texture *mapshot = textureload(picname, 3, true, false);
    renderbackground("loading...", mapshot, mname, game::getmapinfo());

    setvar("mapversion", hdr.version, true, false);

    renderprogress(0, "clearing world...");

    freeocta(worldroot);
    worldroot = NULL;

    int worldscale = 0;
    while(1<<worldscale < hdr.worldsize) worldscale++;
    setvar("mapsize", 1<<worldscale, true, false);
    setvar("mapscale", worldscale, true, false);

    texmru.shrink(0);

    renderprogress(0, "loading lightweight octree...");
    bool failed = false;
    {
        ZoneScopedN("Chunks/Decode entry octree");
        worldroot = loadchildren(f, ivec(0, 0, 0), hdr.worldsize>>1, failed);
    }
    {
        ZoneScopedN("Chunks/Close entry map");
        delete f;
    }
    mapcrc = 0;
    if(failed)
    {
        conoutf(CON_ERROR, "map %s contains a malformed octree", ogzname);
        freeocta(worldroot);
        worldroot = newcubes(F_EMPTY);
        return false;
    }

    conoutf("read map %s (%.1f seconds)", ogzname, (SDL_GetTicks()-loadingstart)/1000.0f);

    clearmainmenu();

    {
        ZoneScopedN("Chunks/Load world configuration");
        identflags |= IDF_OVERRIDDEN;
        execfile("config/default_map_settings.cfg", false);
        if(applyloadworlddefaults)
        {
            setvar("ambient", 0x252525);
            setvar("sunlight", 0xFFF8E0);
            setfvar("sunlightyaw", 30);
            setfvar("sunlightpitch", 50);
            setvar("atmo", 1);
        }
        execfile(cfgname, false);
        identflags &= ~IDF_OVERRIDDEN;
    }

    bool streamedworld = false;
    preparedworldspawn = false;
    if(!cname && hdr.worldsize == WORLD_CHUNK_MAP_SIZE)
    {
        streamedworld = loadworldchunks(mname);
        if(streamedworld)
        {
            worldspawnmetadata spawn;
            if(hasrequestedworldspawn) spawn = requestedworldspawn;
            if(!prepareworldspawn(spawn)) return false;
        }
    }

    {
        ZoneScopedN("Chunks/Build entry geometry");
        allchanged(true);
    }

    renderbackground("loading...", mapshot, mname, game::getmapinfo());

    if(maptitle[0] && strcmp(maptitle, "Untitled Map by Unknown")) conoutf(CON_ECHO, "%s", maptitle);

    startmap(cname ? cname : mname);

    if(streamedworld) applypreparedworldspawn();

    return true;
}

void savecurrentmap() { save_world(game::getclientmap()); }
void savemap(char *mname) { save_world(mname); }

COMMAND(savemap, "s");
COMMAND(savecurrentmap, "");

void writeobj(char *name)
{
    defformatstring(fname, "%s.obj", name);
    stream *f = openfile(path(fname), "w");
    if(!f) return;
    f->printf("# obj file of Cube 2 level\n\n");
    defformatstring(mtlname, "%s.mtl", name);
    path(mtlname);
    f->printf("mtllib %s\n\n", mtlname);
    vector<vec> verts, texcoords;
    hashtable<vec, int> shareverts(1<<16), sharetc(1<<16);
    hashtable<int, vector<ivec2> > mtls(1<<8);
    vector<int> usedmtl;
    vec bbmin(1e16f, 1e16f, 1e16f), bbmax(-1e16f, -1e16f, -1e16f);
    loopv(valist)
    {
        vtxarray &va = *valist[i];
        if(!va.edata || !va.vdata) continue;
        ushort *edata = va.edata + va.eoffset;
        vertex *vdata = va.vdata;
        ushort *idx = edata;
        loopj(va.texs)
        {
            elementset &es = va.texelems[j];
            if(usedmtl.find(es.texture) < 0) usedmtl.add(es.texture);
            vector<ivec2> &keys = mtls[es.texture];
            loopk(es.length)
            {
                const vertex &v = vdata[idx[k]];
                const vec &pos = v.pos;
                const vec &tc = v.tc;
                ivec2 &key = keys.add();
                key.x = shareverts.access(pos, verts.length());
                if(key.x == verts.length())
                {
                    verts.add(pos);
                    bbmin.min(pos);
                    bbmax.max(pos);
                }
                key.y = sharetc.access(tc, texcoords.length());
                if(key.y == texcoords.length()) texcoords.add(tc);
            }
            idx += es.length;
        }
    }

    vec center(-(bbmax.x + bbmin.x)/2, -(bbmax.y + bbmin.y)/2, -bbmin.z);
    loopv(verts)
    {
        vec v = verts[i];
        v.add(center);
        if(v.y != floor(v.y)) f->printf("v %.3f ", -v.y); else f->printf("v %d ", int(-v.y));
        if(v.z != floor(v.z)) f->printf("%.3f ", v.z); else f->printf("%d ", int(v.z));
        if(v.x != floor(v.x)) f->printf("%.3f\n", v.x); else f->printf("%d\n", int(v.x));
    }
    f->printf("\n");
    loopv(texcoords)
    {
        const vec &tc = texcoords[i];
        f->printf("vt %.6f %.6f\n", tc.x, 1-tc.y);
    }
    f->printf("\n");

    usedmtl.sort();
    loopv(usedmtl)
    {
        vector<ivec2> &keys = mtls[usedmtl[i]];
        f->printf("g slot%d\n", usedmtl[i]);
        f->printf("usemtl slot%d\n\n", usedmtl[i]);
        for(int i = 0; i < keys.length(); i += 3)
        {
            f->printf("f");
            loopk(3) f->printf(" %d/%d", keys[i+2-k].x+1, keys[i+2-k].y+1);
            f->printf("\n");
        }
        f->printf("\n");
    }
    delete f;

    f = openfile(mtlname, "w");
    if(!f) return;
    f->printf("# mtl file of Cube 2 level\n\n");
    loopv(usedmtl)
    {
        VSlot &vslot = lookupvslot(usedmtl[i], false);
        f->printf("newmtl slot%d\n", usedmtl[i]);
        f->printf("map_Kd %s\n", vslot.slot->sts.empty() ? notexture->name : path(makerelpath("media", vslot.slot->sts[0].name)));
        f->printf("\n");
    }
    delete f;

    conoutf("generated model %s", fname);
}

COMMAND(writeobj, "s");

void writecollideobj(char *name)
{
    extern bool havesel;
    extern selinfo sel;
    if(!havesel)
    {
        conoutf(CON_ERROR, "geometry for collide model not selected");
        return;
    }
    vector<extentity *> &ents = entities::getents();
    extentity *mm = NULL;
    loopv(entgroup)
    {
        extentity &e = *ents[entgroup[i]];
        if(e.type != ET_MAPMODEL || !pointinsel(sel, e.o)) continue;
        mm = &e;
        break;
    }
    if(!mm) loopv(ents)
    {
        extentity &e = *ents[i];
        if(e.type != ET_MAPMODEL || !pointinsel(sel, e.o)) continue;
        mm = &e;
        break;
    }
    if(!mm)
    {
        conoutf(CON_ERROR, "could not find map model in selection");
        return;
    }
    model *m = loadmapmodel(mm->attr1);
    if(!m)
    {
        mapmodelinfo *mmi = getmminfo(mm->attr1);
        if(mmi) conoutf(CON_ERROR, "could not load map model: %s", mmi->name);
        else conoutf(CON_ERROR, "could not find map model: %d", mm->attr1);
        return;
    }

    matrix4x3 xform;
    m->calctransform(xform);
    float scale = mm->attr5 > 0 ? mm->attr5/100.0f : 1;
    int yaw = mm->attr2, pitch = mm->attr3, roll = mm->attr4;
    matrix3 orient;
    orient.identity();
    if(scale != 1) orient.scale(scale);
    if(yaw) orient.rotate_around_z(sincosmod360(yaw));
    if(pitch) orient.rotate_around_x(sincosmod360(pitch));
    if(roll) orient.rotate_around_y(sincosmod360(-roll));
    xform.mul(orient, mm->o, matrix4x3(xform));
    xform.invert();

    ivec selmin = sel.o, selmax = ivec(sel.s).mul(sel.grid).add(sel.o);
    vector<vec> verts;
    hashtable<vec, int> shareverts;
    vector<int> tris;
    loopv(valist)
    {
        vtxarray &va = *valist[i];
        if(va.geommin.x > selmax.x || va.geommin.y > selmax.y || va.geommin.z > selmax.z ||
           va.geommax.x < selmin.x || va.geommax.y < selmin.y || va.geommax.z < selmin.z)
            continue;
        if(!va.edata || !va.vdata) continue;
        ushort *edata = va.edata + va.eoffset;
        vertex *vdata = va.vdata;
        ushort *idx = edata;
        loopj(va.texs)
        {
            elementset &es = va.texelems[j];
            for(int k = 0; k < es.length; k += 3)
            {
                const vec &v0 = vdata[idx[k]].pos, &v1 = vdata[idx[k+1]].pos, &v2 = vdata[idx[k+2]].pos;
                if(!v0.insidebb(selmin, selmax) || !v1.insidebb(selmin, selmax) || !v2.insidebb(selmin, selmax))
                    continue;
                int i0 = shareverts.access(v0, verts.length());
                if(i0 == verts.length()) verts.add(v0);
                tris.add(i0);
                int i1 = shareverts.access(v1, verts.length());
                if(i1 == verts.length()) verts.add(v1);
                tris.add(i1);
                int i2 = shareverts.access(v2, verts.length());
                if(i2 == verts.length()) verts.add(v2);
                tris.add(i2);
            }
            idx += es.length;
        }
    }

    defformatstring(fname, "%s.obj", name);
    stream *f = openfile(path(fname), "w");
    if(!f) return;
    f->printf("# obj file of Cube 2 collide model\n\n");
    loopv(verts)
    {
        vec v = xform.transform(verts[i]);
        if(v.y != floor(v.y)) f->printf("v %.3f ", -v.y); else f->printf("v %d ", int(-v.y));
        if(v.z != floor(v.z)) f->printf("%.3f ", v.z); else f->printf("%d ", int(v.z));
        if(v.x != floor(v.x)) f->printf("%.3f\n", v.x); else f->printf("%d\n", int(v.x));
    }
    f->printf("\n");
    for(int i = 0; i < tris.length(); i += 3)
       f->printf("f %d %d %d\n", tris[i+2]+1, tris[i+1]+1, tris[i]+1);
    f->printf("\n");

    delete f;

    conoutf("generated collide model %s", fname);
}

COMMAND(writecollideobj, "s");

#endif

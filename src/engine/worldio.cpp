// worldio.cpp: loading & saving of maps and savegames

#include "FastNoiseLite.h"
#include "engine.h"

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

static void fixent(entity &e, int version)
{
    if(version <= 0)
    {
        if(e.type >= ET_DECAL) e.type++;
    }
}

static bool loadmapheader(stream *f, const char *ogzname, mapheader &hdr, octaheader &ohdr)
{
    if(f->read(&hdr, 3*sizeof(int)) != 3*sizeof(int)) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); return false; }
    lilswap(&hdr.version, 2);

    if(!memcmp(hdr.magic, "TMAP", 4))
    {
        if(hdr.version>MAPVERSION) { conoutf(CON_ERROR, "map %s requires a newer version of Tesseract", ogzname); return false; }
        if(f->read(&hdr.worldsize, 6*sizeof(int)) != 6*sizeof(int)) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); return false; }
        lilswap(&hdr.worldsize, 6);
        if(hdr.worldsize <= 0|| hdr.numents < 0) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); return false; }
    }
    else if(!memcmp(hdr.magic, "OCTA", 4))
    {
        if(hdr.version!=OCTAVERSION) { conoutf(CON_ERROR, "map %s uses an unsupported map format version", ogzname); return false; }
        if(f->read(&ohdr.worldsize, 7*sizeof(int)) != 7*sizeof(int)) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); return false; }
        lilswap(&ohdr.worldsize, 7);
        if(ohdr.worldsize <= 0|| ohdr.numents < 0) { conoutf(CON_ERROR, "map %s has malformatted header", ogzname); return false; }
        memcpy(hdr.magic, "TMAP", 4);
        hdr.version = 0;
        hdr.headersize = sizeof(hdr);
        hdr.worldsize = ohdr.worldsize;
        hdr.numents = ohdr.numents;
        hdr.numpvs = ohdr.numpvs;
        hdr.blendmap = ohdr.blendmap;
        hdr.numvars = ohdr.numvars;
        hdr.numvslots = ohdr.numvslots;
    }
    else { conoutf(CON_ERROR, "map %s uses an unsupported map type", ogzname); return false; }

    return true;
}

bool loadents(const char *fname, vector<entity> &ents, uint *crc)
{
    string name;
    validmapname(name, fname);
    defformatstring(ogzname, "media/map/%s.ogz", name);
    path(ogzname);
    stream *f = opengzfile(ogzname, "rb");
    if(!f) return false;

    mapheader hdr;
    octaheader ohdr;
    if(!loadmapheader(f, ogzname, hdr, ohdr)) { delete f; return false; }

    loopi(hdr.numvars)
    {
        int type = f->getchar(), ilen = f->getlil<ushort>();
        f->seek(ilen, SEEK_CUR);
        switch(type)
        {
            case ID_VAR: f->getlil<int>(); break;
            case ID_FVAR: f->getlil<float>(); break;
            case ID_SVAR: { int slen = f->getlil<ushort>(); f->seek(slen, SEEK_CUR); break; }
        }
    }

    string gametype;
    bool samegame = true;
    int len = f->getchar();
    if(len >= 0) f->read(gametype, len+1);
    gametype[max(len, 0)] = '\0';
    if(strcmp(gametype, game::gameident()))
    {
        samegame = false;
        conoutf(CON_WARN, "WARNING: loading map from %s game, ignoring entities except for lights/mapmodels", gametype);
    }
    int eif = f->getlil<ushort>();
    int extrasize = f->getlil<ushort>();
    f->seek(extrasize, SEEK_CUR);

    ushort nummru = f->getlil<ushort>();
    f->seek(nummru*sizeof(ushort), SEEK_CUR);

    loopi(min(hdr.numents, MAXENTS))
    {
        entity &e = ents.add();
        f->read(&e, sizeof(entity));
        lilswap(&e.o.x, 3);
        lilswap(&e.attr1, 5);
        fixent(e, hdr.version);
        if(eif > 0) f->seek(eif, SEEK_CUR);
        if(samegame)
        {
            entities::readent(e, NULL, hdr.version);
        }
        else if(e.type>=ET_GAMESPECIFIC)
        {
            ents.pop();
            continue;
        }
    }

    if(crc)
    {
        f->seek(0, SEEK_END);
        *crc = f->getcrc();
    }

    delete f;

    return true;
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
    WORLD_MAX_PREPARED_CHUNKS = 8
};

VARP(maxchunkdist, 2, 3, WORLD_MAX_CHUNK_DIST);
VARP(worldseed, 0, 1337, INT_MAX);

FVAR(terraincontinentfreq, 0.000001f, 0.0005f, 1.0f);
FVAR(terrainmountainfreq, 0.000001f, 0.002f, 1.0f);
FVAR(terrainerosionfreq, 0.000001f, 0.003f, 1.0f);
FVAR(terrainhillfreq, 0.000001f, 0.01f, 1.0f);
FVAR(terraindetailfreq, 0.000001f, 0.04f, 1.0f);
FVAR(terraincontinentwarpfreq, 0.000001f, 0.001f, 1.0f);
FVAR(terraincontinentwarpamp, 0.0f, 40.0f, float(WORLD_HEIGHT_BLOCKS * 4));
FVAR(terrainfeaturewarpfreq, 0.000001f, 0.001f, 1.0f);
FVAR(terrainfeaturewarpamp, 0.0f, 120.0f, float(WORLD_HEIGHT_BLOCKS * 4));
FVAR(terraintemperaturefreq, 0.000001f, 0.0004f, 1.0f);
FVAR(terrainmoisturefreq, 0.000001f, 0.0006f, 1.0f);
FVAR(terrainweirdnessfreq, 0.000001f, 0.001f, 1.0f);
FVAR(terrainweirdnessstrength, 0.0f, 0.15f, 1.0f);
FVAR(terrainmountainstonefreq, 0.000001f, 0.08f, 1.0f);

VAR(terrainsealevel, WORLD_MIN_HEIGHT + 1, 0, WORLD_MAX_HEIGHT - 1);
VAR(terrainsnowheight, WORLD_MIN_HEIGHT + 1, 70, WORLD_MAX_HEIGHT - 1);
VAR(terrainmountainstonelow, WORLD_MIN_HEIGHT + 1, 35, WORLD_MAX_HEIGHT - 1);
VAR(terrainmountainstonehigh, WORLD_MIN_HEIGHT + 1, 45, WORLD_MAX_HEIGHT - 1);
VAR(terrainbiomeblend, 0, 16, 64);
VAR(terraincoastwidth, 0, 8, 32);
VAR(terraincoastvariation, 0, 4, 16);
VAR(terrainbeachminheight, -32, -2, 32);
VAR(terrainbeachmaxheight, -32, 2, 32);
FVAR(terraincontinentheight, 0.0f, 35.0f, float(WORLD_HEIGHT_BLOCKS));
FVAR(terrainhillheight, 0.0f, 10.0f, float(WORLD_HEIGHT_BLOCKS));
FVAR(terrainmountainheight, 0.0f, 80.0f, float(WORLD_HEIGHT_BLOCKS));
FVAR(terrainerosionheight, 0.0f, 15.0f, float(WORLD_HEIGHT_BLOCKS));
FVAR(terraindetailheight, 0.0f, 2.0f, float(WORLD_HEIGHT_BLOCKS));

FVAR(terrainlandmasklow, -1.0f, -0.2f, 1.0f);
FVAR(terrainlandmaskhigh, -1.0f, 0.1f, 1.0f);
FVAR(terrainmountainmasklow, -1.0f, 0.15f, 1.0f);
FVAR(terrainmountainmaskhigh, -1.0f, 0.65f, 1.0f);
FVAR(terraindeserttemperature, -1.0f, 0.4f, 1.0f);
FVAR(terraindesertmoisture, -1.0f, -0.25f, 1.0f);
FVAR(terrainforestmoisture, -1.0f, 0.35f, 1.0f);
FVAR(terrainforesttreedensity, 0.0f, 0.1f, 0.25f);
FVAR(terrainplainstreedensity, 0.0f, 0.0015f, 0.25f);
VAR(terrainpinestartheight, WORLD_MIN_HEIGHT + 1, 25, WORLD_MAX_HEIGHT - 1);
VAR(terrainpinefullheight, WORLD_MIN_HEIGHT + 1, 45, WORLD_MAX_HEIGHT - 1);

FVAR(terraincavefreq, 0.0001f, 0.035f, 0.25f);
FVAR(terraincavethreshold, -1.0f, 0.55f, 1.0f);
FVAR(terrainlargecavefreq, 0.0001f, 0.012f, 0.25f);
FVAR(terrainlargecavethreshold, -1.0f, 0.72f, 1.0f);
FVAR(terrainlargecavedeepthreshold, -1.0f, 0.48f, 1.0f);
FVAR(terraintunnelfreq, 0.0001f, 0.018f, 0.25f);
FVAR(terraintunnelwidth, 0.001f, 0.065f, 0.3f);
FVAR(terraincaveentrancewidth, 0.001f, 0.015f, 0.3f);
VAR(terraincavemindepth, 1, 8, 64);
VAR(terraincavefulldepth, 1, 24, 128);
VAR(terraincavedeepheight, WORLD_MIN_HEIGHT + 1, -64, WORLD_MAX_HEIGHT - 1);

VAR(terrainbottomlavalayers, 0, 3, 16);
VAR(terrainlavalakestartheight, WORLD_MIN_HEIGHT + 1, -16, WORLD_MAX_HEIGHT - 1);
VAR(terrainlavalakedeepheight, WORLD_MIN_HEIGHT + 1, -64, WORLD_MAX_HEIGHT - 1);
FVAR(terrainlavalakeshallowchance, 0.0f, 0.03f, 1.0f);
FVAR(terrainlavalakedeepchance, 0.0f, 0.22f, 1.0f);
VAR(terrainlavalakeminsize, 1, 4, 32);
VAR(terrainlavalakemaxsize, 1, 14, 32);
VAR(terrainlavalakespacing, 8, 24, 64);
FVAR(terrainlavalakeshapefreq, 0.001f, 0.08f, 1.0f);
FVAR(terrainlavalakeshapevariation, 0.0f, 0.35f, 0.75f);

static int activeworldseed = 1337;

struct terrainsettings
{
    float continentfreq, mountainfreq, erosionfreq, hillfreq, detailfreq;
    float continentwarpfreq, continentwarpamp, featurewarpfreq, featurewarpamp;
    float temperaturefreq, moisturefreq, weirdnessfreq, weirdnessstrength, mountainstonefreq;
    float continentheight, hillheight, mountainheight, erosionheight, detailheight;
    float landmasklow, landmaskhigh, mountainmasklow, mountainmaskhigh;
    float deserttemperature, desertmoisture, forestmoisture;
    float foresttreedensity, plainstreedensity;
    float cavefreq, cavethreshold, largecavefreq, largecavethreshold, largecavedeepthreshold;
    float tunnelfreq, tunnelwidth, caveentrancewidth;
    float lavalakeshallowchance, lavalakedeepchance, lavalakeshapefreq, lavalakeshapevariation;
    int sealevel, snowheight, mountainstonelow, mountainstonehigh;
    int biomeblend, coastwidth, coastvariation;
    int beachminheight, beachmaxheight;
    int pinestartheight, pinefullheight;
    int cavemindepth, cavefulldepth, cavedeepheight;
    int bottomlavalayers, lavalakestartheight, lavalakedeepheight;
    int lavalakeminsize, lavalakemaxsize, lavalakespacing;

    terrainsettings()
        : continentfreq(terraincontinentfreq), mountainfreq(terrainmountainfreq),
          erosionfreq(terrainerosionfreq), hillfreq(terrainhillfreq), detailfreq(terraindetailfreq),
          continentwarpfreq(terraincontinentwarpfreq), continentwarpamp(terraincontinentwarpamp),
          featurewarpfreq(terrainfeaturewarpfreq), featurewarpamp(terrainfeaturewarpamp),
          temperaturefreq(terraintemperaturefreq), moisturefreq(terrainmoisturefreq),
          weirdnessfreq(terrainweirdnessfreq), weirdnessstrength(terrainweirdnessstrength),
          mountainstonefreq(terrainmountainstonefreq),
          continentheight(terraincontinentheight), hillheight(terrainhillheight),
          mountainheight(terrainmountainheight), erosionheight(terrainerosionheight),
          detailheight(terraindetailheight), landmasklow(terrainlandmasklow),
          landmaskhigh(terrainlandmaskhigh), mountainmasklow(terrainmountainmasklow),
          mountainmaskhigh(terrainmountainmaskhigh), deserttemperature(terraindeserttemperature),
          desertmoisture(terraindesertmoisture), forestmoisture(terrainforestmoisture),
          foresttreedensity(terrainforesttreedensity), plainstreedensity(terrainplainstreedensity),
          cavefreq(terraincavefreq), cavethreshold(terraincavethreshold),
          largecavefreq(terrainlargecavefreq), largecavethreshold(terrainlargecavethreshold),
          largecavedeepthreshold(terrainlargecavedeepthreshold),
          tunnelfreq(terraintunnelfreq), tunnelwidth(terraintunnelwidth),
          caveentrancewidth(terraincaveentrancewidth),
          lavalakeshallowchance(terrainlavalakeshallowchance),
          lavalakedeepchance(terrainlavalakedeepchance),
          lavalakeshapefreq(terrainlavalakeshapefreq),
          lavalakeshapevariation(terrainlavalakeshapevariation),
          sealevel(terrainsealevel), snowheight(terrainsnowheight),
          mountainstonelow(terrainmountainstonelow), mountainstonehigh(terrainmountainstonehigh),
          biomeblend(terrainbiomeblend),
          coastwidth(terraincoastwidth), coastvariation(terraincoastvariation),
          beachminheight(terrainbeachminheight), beachmaxheight(terrainbeachmaxheight),
          pinestartheight(terrainpinestartheight), pinefullheight(terrainpinefullheight),
          cavemindepth(terraincavemindepth), cavefulldepth(terraincavefulldepth),
          cavedeepheight(terraincavedeepheight), bottomlavalayers(terrainbottomlavalayers),
          lavalakestartheight(terrainlavalakestartheight),
          lavalakedeepheight(terrainlavalakedeepheight),
          lavalakeminsize(terrainlavalakeminsize), lavalakemaxsize(terrainlavalakemaxsize),
          lavalakespacing(terrainlavalakespacing)
    {
    }
};

static void setupworldnoiselayer(FastNoiseLite &noise, int seed, float frequency, int octaves)
{
    noise.SetSeed(seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
    noise.SetFrequency(frequency);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(octaves);
    noise.SetFractalLacunarity(2.0f);
    noise.SetFractalGain(0.5f);
}

static void setupworldwarp(FastNoiseLite &warp, int seed, float frequency, float amplitude)
{
    warp.SetSeed(seed);
    warp.SetDomainWarpType(FastNoiseLite::DomainWarpType_OpenSimplex2);
    warp.SetFrequency(frequency);
    warp.SetDomainWarpAmp(amplitude);
}

static void setupworldnoise(FastNoiseLite &continent, FastNoiseLite &mountains, FastNoiseLite &erosion,
                            FastNoiseLite &hills, FastNoiseLite &detail, int seed,
                            const terrainsettings &settings)
{
    setupworldnoiselayer(continent, seed, settings.continentfreq, 4);
    setupworldnoiselayer(mountains, seed ^ 0x68E31DA4, settings.mountainfreq, 3);
    setupworldnoiselayer(erosion, seed ^ 0x1B56C4E9, settings.erosionfreq, 3);
    setupworldnoiselayer(hills, seed ^ 0x4A39B70D, settings.hillfreq, 3);
    setupworldnoiselayer(detail, seed ^ 0x2C1B3C6D, settings.detailfreq, 2);
}

static void loadworldseed(int seed)
{
    worldseed = max(seed, 0);
    activeworldseed = worldseed;
}

ICOMMAND(worldloadseed, "i", (int *seed), loadworldseed(*seed));

struct terraincubetype
{
    string name, texture, sides;
    float texsize;
    int slot, sideslot;

    terraincubetype() : texsize(1), slot(DEFAULT_GEOM), sideslot(DEFAULT_GEOM)
    {
        name[0] = texture[0] = sides[0] = '\0';
    }
};

static vector<terraincubetype *> terraincubetypes;
static int worldgrasstexture = DEFAULT_GEOM, worldgrasssidetexture = DEFAULT_GEOM,
           worlddirttexture = DEFAULT_GEOM, worldstonetexture = DEFAULT_GEOM,
           worldsandtexture = DEFAULT_GEOM, worldsnowtexture = DEFAULT_GEOM,
           worldwoodtexture = DEFAULT_GEOM, worldleaftexture = DEFAULT_GEOM;

static terraincubetype *findterraincube(const char *name)
{
    loopv(terraincubetypes) if(!cubecasecmp(terraincubetypes[i]->name, name)) return terraincubetypes[i];
    return NULL;
}

void terrainreset()
{
    terraincubetypes.deletecontents();
    worldgrasstexture = worldgrasssidetexture = worlddirttexture = worldstonetexture =
        worldsandtexture = worldsnowtexture = worldwoodtexture = worldleaftexture = DEFAULT_GEOM;
}

COMMAND(terrainreset, "");

static void defineterraincube(const char *name, const char *texture, float texsize, const char *sides)
{
    if(!name[0] || !texture[0])
    {
        conoutf(CON_ERROR, "terraincube requires a cube name and texture path");
        return;
    }

    terraincubetype *type = findterraincube(name);
    if(!type) type = terraincubetypes.add(new terraincubetype);
    copystring(type->name, name);
    copystring(type->texture, texture);
    copystring(type->sides, sides ? sides : "");
    type->texsize = texsize > 0 ? texsize : 1;
}

ICOMMAND(terraincube, "ssfsN", (char *name, char *texture, float *texsize, char *sides, int *numargs),
{
    defineterraincube(name, texture, *texsize, *numargs >= 4 ? sides : NULL);
});

static bool loadterrain()
{
    terrainreset();
    if(!execfile("config/terrain.cfg", false))
    {
        conoutf(CON_ERROR, "could not load config/terrain.cfg");
        return false;
    }

    terraincubetype *grass = findterraincube("Grass"), *dirt = findterraincube("Dirt"),
                    *stone = findterraincube("Stone"), *sand = findterraincube("Sand"),
                    *snow = findterraincube("Snow"), *wood = findterraincube("Wood"),
                    *leaves = findterraincube("Leaves");
    if(!grass || !dirt || !stone || !sand || !snow || !wood || !leaves)
    {
        conoutf(CON_ERROR, "terrain.cfg must define Grass, Dirt, Stone, Sand, Snow, Wood, and Leaves cubes");
        return false;
    }

    execute("texturereset; texsky; setshader stdworld");
    loopv(terraincubetypes)
    {
        terraincubetype &type = *terraincubetypes[i];
        defformatstring(command, "texture 0 %s; texscale %.9g", escapestring(type.texture), type.texsize);
        execute(command);
        type.slot = slots.last()->variants->index;
    }
    loopv(terraincubetypes)
    {
        terraincubetype &type = *terraincubetypes[i];
        terraincubetype *sidetype = type.sides[0] ? findterraincube(type.sides) : NULL;
        if(type.sides[0] && !sidetype)
        {
            conoutf(CON_ERROR, "terrain cube %s references unknown side cube %s", type.name, type.sides);
            return false;
        }
        type.sideslot = sidetype ? sidetype->slot : type.slot;
    }

    worldgrasstexture = grass->slot;
    worldgrasssidetexture = grass->sideslot;
    worlddirttexture = dirt->slot;
    worldstonetexture = stone->slot;
    worldsandtexture = sand->slot;
    worldsnowtexture = snow->slot;
    worldwoodtexture = wood->slot;
    worldleaftexture = leaves->slot;
    conoutf(CON_DEBUG, "loaded %d terrain cube definitions", terraincubetypes.length());
    return true;
}

ICOMMAND(terrainload, "", (), intret(loadterrain() ? 1 : 0));

struct worldchunk
{
    int x, y;
    cube *root;
    bool mounted, loading, generating, saved, dirty;

    worldchunk(int x, int y, cube *root, bool loading = false, bool saved = false)
        : x(x), y(y), root(root), mounted(false), loading(loading), generating(false), saved(saved), dirty(false) {}
};

static bool worldchunkmounted(const worldchunk &chunk);

struct worldchunkjob
{
    int x, y, seed, grasstexture, grasssidetexture, dirttexture, stonetexture, sandtexture, snowtexture,
        woodtexture, leaftexture;
    terrainsettings terrain;
    int families, optimized, loaderror;
    uint epoch;
    bool loaded;
    cube *root;
    string filename;

    worldchunkjob(int x, int y, uint epoch)
        : x(x), y(y), seed(activeworldseed),
          grasstexture(worldgrasstexture), grasssidetexture(worldgrasssidetexture),
          dirttexture(worlddirttexture), stonetexture(worldstonetexture),
          sandtexture(worldsandtexture), snowtexture(worldsnowtexture),
          woodtexture(worldwoodtexture), leaftexture(worldleaftexture),
          families(0), optimized(0), loaderror(0), epoch(epoch), loaded(false), root(NULL)
    {
        filename[0] = '\0';
    }
};

static vector<worldchunk> worldchunks;
static vector<worldchunkjob *> worldchunkjobs, worldchunkresults;
static string worldfolder = "";
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
static int lastworldchunkpublish = -1;

VARP(asyncchunkloads, 1, 1, 4);
VARP(chunkthreads, 0, 0, 16);
VARP(chunkcachedist, 0, 2, 8);

static cube *generateworldchunk(int chunkx, int chunky);
static cube *loadworldchunkroot(const char *mname);
static cube *prepareworldchunk(worldchunkjob &job);
static void freepreparedworldchunk(cube *root);
static int worldchunkloader(void *);
static void shutdownworldchunkloader();
static int pruneworldchunkcache(int chunkx, int chunky, int limit);
static bool saveworldconfig();
static void worldchunkname(char *name, size_t len, const worldchunk &chunk);
void setmapfilenames(const char *fname, const char *cname);

int getworldsectionsize()
{
    return worldchunks.empty() ? 0 : WORLD_SECTION_SIZE;
}

struct worlddebugstats
{
    int chunkx, chunky;
    double absolutex, absolutey, absolutez;
    int rendered;
    int loadingqueue, generationqueue;
};

static void getworlddebugstats(const vec &position, worlddebugstats &stats)
{
    stats.rendered = 0;
    stats.loadingqueue = stats.generationqueue = 0;

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
            if(worldchunkmounted(chunk))
            {
                stats.rendered++;
            }
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

ICOMMAND(getdebugcamx, "", (), debugcoordinateresult(currentworlddebugstats().absolutex));
ICOMMAND(getdebugcamy, "", (), debugcoordinateresult(currentworlddebugstats().absolutey));
ICOMMAND(getdebugcamz, "", (), debugcoordinateresult(currentworlddebugstats().absolutez));
ICOMMAND(getdebugchunkx, "", (), intret(currentworlddebugstats().chunkx));
ICOMMAND(getdebugchunky, "", (), intret(currentworlddebugstats().chunky));
ICOMMAND(getdebugrenderedfull, "", (), intret(currentworlddebugstats().rendered));
ICOMMAND(getdebugtargetchunks, "", (), intret((2 * maxchunkdist + 1) * (2 * maxchunkdist + 1)));
ICOMMAND(getdebugloadingqueue, "", (), intret(currentworlddebugstats().loadingqueue));
ICOMMAND(getdebuggenerationqueue, "", (), intret(currentworlddebugstats().generationqueue));

void clearworldchunks()
{
    shutdownworldchunkloader();
    loopv(worldchunks) if(worldchunks[i].root && worldchunks[i].root != worldroot)
        freeocta(worldchunks[i].root);
    worldchunks.setsize(0);
    worldfolder[0] = '\0';
    activeworldchunk = -1;
    worldfirstchunkx = worldfirstchunky = 0;
    lastplayerchunkx = lastplayerchunky = INT_MIN;
    lastchunkdist = -1;
    rebuildingworldchunks = false;
    lastworldchunkpublish = -1;
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

static bool worldchunkmounted(const worldchunk &chunk)
{
    return chunk.mounted;
}

void markworldchunksdirty(const ivec &bbmin, const ivec &bbmax)
{
    if(suppressworldchunkdirty || worldchunks.empty()) return;
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

static void syncmountedworldchunk(worldchunk &chunk)
{
    if(!worldchunkmounted(chunk) || !chunk.root || !worldroot) return;
    loopi(WORLD_SECTION_LAYERS) loopj(WORLD_SECTION_TILES)
    {
        int x = j % WORLD_SECTION_COLUMNS, y = j / WORLD_SECTION_COLUMNS;
        ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, i * WORLD_SECTION_SIZE);
        pasteworldcube(lookupcube(ivec(worldchunkorigin(chunk)).add(pos), WORLD_SECTION_SIZE),
                       lookupworldchunkcube(chunk, pos, WORLD_SECTION_SIZE));
    }
}

static void syncmountedworldchunks()
{
    if(worldchunks.empty() || !worldroot) return;
    loopv(worldchunks) if(!worldchunks[i].saved || worldchunks[i].dirty)
        syncmountedworldchunk(worldchunks[i]);
}

static void mountworldchunk(worldchunk &chunk)
{
    if(chunk.mounted) return;
    loopi(WORLD_SECTION_LAYERS) loopj(WORLD_SECTION_TILES)
    {
        int x = j % WORLD_SECTION_COLUMNS, y = j / WORLD_SECTION_COLUMNS;
        ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, i * WORLD_SECTION_SIZE);
        moveworldcube(lookupworldchunkcube(chunk, pos, WORLD_SECTION_SIZE),
                      lookupcube(ivec(worldchunkorigin(chunk)).add(pos), WORLD_SECTION_SIZE));
    }
    chunk.mounted = true;
}

static void unmountworldchunk(worldchunk &chunk)
{
    if(!chunk.mounted) return;
    loopi(WORLD_SECTION_LAYERS) loopj(WORLD_SECTION_TILES)
    {
        int x = j % WORLD_SECTION_COLUMNS, y = j / WORLD_SECTION_COLUMNS;
        ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, i * WORLD_SECTION_SIZE);
        cube &c = lookupcube(ivec(worldchunkorigin(chunk)).add(pos), WORLD_SECTION_SIZE);
        detachworldcubegeometry(c);
        moveworldcube(c, lookupworldchunkcube(chunk, pos, WORLD_SECTION_SIZE));
    }
    chunk.mounted = false;
}

static void invalidateworldchunk(const worldchunk &chunk)
{
    ivec bbmin = worldchunkorigin(chunk), bbmax = bbmin;
    bbmin.sub(1).max(0);
    bbmax.add(ivec(WORLD_CHUNK_SIZE, WORLD_CHUNK_SIZE, WORLD_MAP_SIZE)).add(1).min(worldsize);
    bool oldsuppress = suppressworldchunkdirty;
    suppressworldchunkdirty = true;
    changed(bbmin, bbmax, false);
    suppressworldchunkdirty = oldsuppress;
}

static int findworldchunk(int x, int y)
{
    loopv(worldchunks) if(worldchunks[i].x == x && worldchunks[i].y == y) return i;
    return -1;
}

static bool worldchunkinview(const worldchunk &chunk, int chunkx, int chunky)
{
    long long dx = (long long)chunk.x - chunkx, dy = (long long)chunk.y - chunky;
    if(dx < 0) dx = -dx;
    if(dy < 0) dy = -dy;
    int dist = int(max(dx, dy));
    return dist <= maxchunkdist;
}

static bool worldchunkjobinview(int x, int y, int chunkx, int chunky)
{
    int dist = max(abs(x - chunkx), abs(y - chunky));
    return dist <= maxchunkdist;
}

static int worldchunkfocusx = 0, worldchunkfocusy = 0;

static int worldchunkjobscore(const worldchunkjob &job)
{
    int dx = job.x - worldchunkfocusx, dy = job.y - worldchunkfocusy,
        dist = max(abs(dx), abs(dy));
    return dist * 0x10000 + dx * dx + dy * dy;
}

static int worldchunkloader(void *)
{
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
        int best = 0, bestscore = worldchunkjobscore(*worldchunkjobs[0]);
        loopv(worldchunkjobs) if(i)
        {
            int score = worldchunkjobscore(*worldchunkjobs[i]);
            if(score < bestscore) { best = i; bestscore = score; }
        }
        worldchunkjob *job = worldchunkjobs.remove(best);
        SDL_UnlockMutex(worldchunkmutex);

        job->root = prepareworldchunk(*job);

        SDL_LockMutex(worldchunkmutex);
        while(worldchunkresults.length() >= WORLD_MAX_PREPARED_CHUNKS && !stopworldchunkthread)
            SDL_CondWait(worldchunkcond, worldchunkmutex);
        if(stopworldchunkthread)
        {
            SDL_UnlockMutex(worldchunkmutex);
            freepreparedworldchunk(job->root);
            delete job;
            return 0;
        }
        worldchunkresults.add(job);
        SDL_UnlockMutex(worldchunkmutex);
    }
}

static bool startworldchunkloader()
{
    if(!worldchunkworkers.empty()) return true;
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
    if(!worldchunkworkers.empty())
    {
        SDL_LockMutex(worldchunkmutex);
        stopworldchunkthread = true;
        SDL_CondBroadcast(worldchunkcond);
        SDL_UnlockMutex(worldchunkmutex);
        loopv(worldchunkworkers) SDL_WaitThread(worldchunkworkers[i], NULL);
        worldchunkworkers.setsize(0);
    }

    loopv(worldchunkjobs) delete worldchunkjobs[i];
    worldchunkjobs.setsize(0);
    loopv(worldchunkresults)
    {
        freepreparedworldchunk(worldchunkresults[i]->root);
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

    defformatstring(chunkname, "%s/%d_%d", worldfolder, x, y);
    // loadworldchunkroot() resolves the configured home and package paths.
    // A direct fileexists() on the relative media path misses saved chunks
    // when the game was launched with -u, causing them to be regenerated.
    cube *root = loadworldchunkroot(chunkname);
    bool loaded = root != NULL;
    if(!root)
    {
        root = generateworldchunk(x, y);
        generated++;
    }
    worldchunks.add(worldchunk(x, y, root, false, loaded));
    return worldchunks.length() - 1;
}

static void loadinitialworldchunks(int chunkx, int chunky)
{
    static const int offsets[][2] =
    {
        { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 },
        { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 }
    };
    int target = min(4, (2 * maxchunkdist + 1) * (2 * maxchunkdist + 1)),
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
    int index = findworldchunk(x, y);
    if(index >= 0) return index;

    if(!startworldchunkloader())
    {
        int generated = 0;
        return acquireworldchunksync(x, y, generated);
    }

    worldchunkjob *job = new worldchunkjob(x, y, worldchunkepoch);
    defformatstring(chunkfile, "media/map/%s/%d_%d.ogz", worldfolder, x, y);
    path(chunkfile);
    const char *found = findfile(chunkfile, "rb");
    if(found && fileexists(found, "r"))
        copystring(job->filename, found);

    worldchunk &chunk = worldchunks.add(worldchunk(x, y, NULL, true));
    chunk.generating = !job->filename[0];
    SDL_LockMutex(worldchunkmutex);
    worldchunkjobs.add(job);
    SDL_CondSignal(worldchunkcond);
    SDL_UnlockMutex(worldchunkmutex);
    return worldchunks.length() - 1;
}

static int queueworldchunkview(int chunkx, int chunky)
{
    int queued = 0;
    for(int dist = 0; dist <= maxchunkdist; ++dist)
    for(int y = -dist; y <= dist; ++y)
    for(int x = -dist; x <= dist; ++x)
    {
        if(max(abs(x), abs(y)) != dist) continue;
        if(findworldchunk(chunkx + x, chunky + y) >= 0) continue;
        queueworldchunk(chunkx + x, chunky + y);
        queued++;
    }
    return queued;
}

static int reprioritizeworldchunkqueue(int chunkx, int chunky)
{
    if(!worldchunkmutex)
    {
        worldchunkfocusx = chunkx;
        worldchunkfocusy = chunky;
        return 0;
    }

    int cancelled = 0;
    SDL_LockMutex(worldchunkmutex);
    worldchunkfocusx = chunkx;
    worldchunkfocusy = chunky;
    for(int i = worldchunkjobs.length() - 1; i >= 0; --i)
    {
        worldchunkjob *job = worldchunkjobs[i];
        if(worldchunkjobinview(job->x, job->y, chunkx, chunky)) continue;
        delete worldchunkjobs.remove(i);
        cancelled++;
    }
    SDL_UnlockMutex(worldchunkmutex);

    // A job already owned by a worker cannot be cancelled safely. Removing
    // its placeholder makes its eventual result self-discard instead of
    // publishing terrain that the camera has already outrun.
    for(int i = worldchunks.length() - 1; i >= 0; --i)
    {
        worldchunk &chunk = worldchunks[i];
        if(!chunk.loading ||
           worldchunkjobinview(chunk.x, chunk.y, chunkx, chunky))
            continue;
        worldchunks.removeunordered(i);
    }
    return cancelled;
}

static int processworldchunkresults()
{
    if(worldchunkworkers.empty()) return 0;

    int handled = 0, published = 0, loaded = 0, generated = 0, optimized = 0;
    while(handled < asyncchunkloads)
    {
        SDL_LockMutex(worldchunkmutex);
        worldchunkjob *job = NULL;
        if(!worldchunkresults.empty())
        {
            int best = 0, bestscore = worldchunkjobscore(*worldchunkresults[0]);
            loopv(worldchunkresults) if(i)
            {
                int score = worldchunkjobscore(*worldchunkresults[i]);
                if(score < bestscore) { best = i; bestscore = score; }
            }
            job = worldchunkresults.remove(best);
        }
        if(job) SDL_CondSignal(worldchunkcond);
        SDL_UnlockMutex(worldchunkmutex);
        if(!job) break;
        handled++;

        int index = findworldchunk(job->x, job->y);
        if(job->epoch != worldchunkepoch || index < 0 || !worldchunks[index].loading)
        {
            freepreparedworldchunk(job->root);
            delete job;
            continue;
        }

        worldchunk &chunk = worldchunks[index];
        chunk.root = job->root;
        chunk.loading = false;
        chunk.saved = job->loaded;
        chunk.dirty = false;
        allocnodes += job->families;
        // Both generated and disk-loaded chunks were structurally validated
        // and remipped by the worker. Walking the complete cave octree again
        // here turns an asynchronous load into a main-thread frame spike.
        if(!job->loaded && job->filename[0])
            conoutf(CON_WARN, "asynchronous load of chunk %d_%d failed at stage %d; regenerated it",
                    job->x, job->y, job->loaderror);
        if(job->loaded) loaded++; else generated++;
        optimized += job->optimized;
        published++;
        delete job;
    }

    if(published)
        conoutf(CON_DEBUG, "prepared %d chunks asynchronously (%d loaded, %d generated, %d octree families remipped)",
                published, loaded, generated, optimized);
    return published;
}

static int findreadyworldchunk(int chunkx, int chunky)
{
    int best = -1, bestscore = INT_MAX;
    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root || worldchunkmounted(chunk) ||
           !worldchunkinview(chunk, chunkx, chunky))
            continue;
        int dx = chunk.x - chunkx, dy = chunk.y - chunky,
            score = max(abs(dx), abs(dy)) * 0x10000 + dx * dx + dy * dy;
        if(score >= bestscore) continue;
        best = i;
        bestscore = score;
    }
    return best;
}

static bool publishworldchunk(int chunkindex)
{
    if(!worldchunks.inrange(chunkindex)) return false;
    worldchunk &chunk = worldchunks[chunkindex];
    mountworldchunk(chunk);
    invalidateworldchunk(chunk);
    commitchanges();
    return true;
}

static void processworldchunkupdates(int chunkx, int chunky)
{
    if(lastworldchunkpublish == totalmillis) return;
    lastworldchunkpublish = totalmillis;
    // Repair coverage continuously, not only when crossing a chunk boundary.
    // A cancelled or discarded job can therefore never leave a permanent
    // hole inside maxchunkdist.
    queueworldchunkview(chunkx, chunky);
    processworldchunkresults();
    int ready = findreadyworldchunk(chunkx, chunky);
    if(ready >= 0) publishworldchunk(ready);
    else pruneworldchunkcache(chunkx, chunky, 1);
}

static void rebaseworldchunks(int chunkx, int chunky)
{
    loopv(worldchunks) if(worldchunkmounted(worldchunks[i])) unmountworldchunk(worldchunks[i]);

    int newfirstx = chunkx - WORLD_RUNTIME_CENTER,
        newfirsty = chunky - WORLD_RUNTIME_CENTER,
        shiftx = (newfirstx - worldfirstchunkx) * WORLD_CHUNK_SIZE,
        shifty = (newfirsty - worldfirstchunky) * WORLD_CHUNK_SIZE;
    freeocta(worldroot);
    worldroot = newcubes(F_EMPTY);
    worldfirstchunkx = newfirstx;
    worldfirstchunky = newfirsty;
    if(player)
    {
        player->o.x -= shiftx;
        player->o.y -= shifty;
    }
    conoutf(CON_DEBUG, "rebased chunk window around %d_%d", chunkx, chunky);
}

static int pruneworldchunkcache(int chunkx, int chunky, int limit)
{
    int released = 0, cachedist = maxchunkdist + chunkcachedist;
    for(int i = worldchunks.length() - 1; i >= 0; --i)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || worldchunkmounted(chunk) || chunk.dirty || !chunk.root ||
           max(abs(chunk.x - chunkx), abs(chunk.y - chunky)) <= cachedist)
            continue;
        freeocta(chunk.root);
        worldchunks.removeunordered(i);
        released++;
        if(released >= limit) break;
    }
    return released;
}

static void rebuildworldchunks(int chunkx, int chunky, bool load, bool updategeometry)
{
    rebuildingworldchunks = true;
    int cancelled = reprioritizeworldchunkqueue(chunkx, chunky),
        queued = queueworldchunkview(chunkx, chunky);

    vector<int> entering, leaving;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        bool shouldmount = worldchunkinview(chunk, chunkx, chunky);
        if(worldchunkmounted(chunk) && !shouldmount) leaving.add(i);
        else if(!worldchunkmounted(chunk) && !chunk.loading && chunk.root && shouldmount) entering.add(i);
    }

    // The distance limit is a hard runtime boundary. Prepared roots may stay
    // in the CPU cache for a quick reversal, but out-of-range geometry must
    // leave the live octree immediately.
    loopv(leaving) unmountworldchunk(worldchunks[leaving[i]]);
    if(load) loopv(entering)
    {
        worldchunk &chunk = worldchunks[entering[i]];
        mountworldchunk(chunk);
    }

    lastplayerchunkx = chunkx;
    lastplayerchunky = chunky;
    lastchunkdist = maxchunkdist;
    if(load) validatec(worldroot, worldsize >> 1);
    if(updategeometry)
    {
        if(load) allchanged(true);
        else if(!leaving.empty())
        {
            loopv(leaving) invalidateworldchunk(worldchunks[leaving[i]]);
            commitchanges();
        }
    }

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

void updateworldchunks(bool force)
{
    if(worldchunks.empty() || rebuildingworldchunks || !worldroot) return;

    int localchunkx = 0, localchunky = 0;
    if(player)
    {
        localchunkx = int(floor(player->o.x / WORLD_CHUNK_SIZE));
        localchunky = int(floor(player->o.y / WORLD_CHUNK_SIZE));
    }
    int chunkx = worldfirstchunkx + localchunkx,
        chunky = worldfirstchunky + localchunky;
    if(!force) processworldchunkupdates(chunkx, chunky);
    if(!force && chunkx == lastplayerchunkx && chunky == lastplayerchunky &&
       maxchunkdist == lastchunkdist)
        return;

    int viewdist = maxchunkdist;
    bool rebase = localchunkx - viewdist <= 0 || localchunkx + viewdist >= WORLD_RUNTIME_CHUNKS - 1 ||
                  localchunky - viewdist <= 0 || localchunky + viewdist >= WORLD_RUNTIME_CHUNKS - 1;
    if(rebase) rebaseworldchunks(chunkx, chunky);
    rebuildworldchunks(chunkx, chunky, force && !rebase, true);
}

struct worldgencontext
{
    FastNoiseLite continentwarp, featurewarp;
    FastNoiseLite continent, mountains, erosion, hills, detail;
    FastNoiseLite temperature, moisture, weirdness, biomeblend, rockiness;
    FastNoiseLite caves, largecaves, tunnela, tunnelb, lakeshape;
    terrainsettings terrain;
    int heightmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar biomemap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar coastmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar rockmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    int seed, grasstexture, grasssidetexture, dirttexture, stonetexture, sandtexture, snowtexture,
        woodtexture, leaftexture;
    bool prepared;
    int families, optimized;

    worldgencontext(int seed, int grasstexture, int grasssidetexture, int dirttexture, int stonetexture,
                    int sandtexture, int snowtexture, int woodtexture, int leaftexture,
                    bool prepared, const terrainsettings &terrain)
        : terrain(terrain), seed(seed), grasstexture(grasstexture), grasssidetexture(grasssidetexture),
          dirttexture(dirttexture), stonetexture(stonetexture), sandtexture(sandtexture),
          snowtexture(snowtexture), woodtexture(woodtexture), leaftexture(leaftexture),
          prepared(prepared), families(0), optimized(0)
    {
        setupworldwarp(continentwarp, seed ^ 0x6C8E9CF5, terrain.continentwarpfreq, terrain.continentwarpamp);
        setupworldwarp(featurewarp, seed ^ 0x35A4F2D1, terrain.featurewarpfreq, terrain.featurewarpamp);
        setupworldnoise(continent, mountains, erosion, hills, detail, seed, terrain);
        setupworldnoiselayer(temperature, seed ^ 0x51D7348B, terrain.temperaturefreq, 3);
        setupworldnoiselayer(moisture, seed ^ 0x2F6E2B1D, terrain.moisturefreq, 3);
        setupworldnoiselayer(weirdness, seed ^ 0x749A7C15, terrain.weirdnessfreq, 3);
        setupworldnoiselayer(biomeblend, seed ^ 0x13C6E91F,
                             terrain.biomeblend > 0 ? 1.0f / terrain.biomeblend : 1.0f, 1);
        setupworldnoiselayer(rockiness, seed ^ 0x5E4A19C3, terrain.mountainstonefreq, 2);
        setupworldnoiselayer(caves, seed ^ 0x7A84F12D, terrain.cavefreq, 2);
        setupworldnoiselayer(largecaves, seed ^ 0x36B9C7E5, terrain.largecavefreq, 2);
        setupworldnoiselayer(tunnela, seed ^ 0x19F3A6C7, terrain.tunnelfreq, 2);
        setupworldnoiselayer(tunnelb, seed ^ 0x5C2D8E91, terrain.tunnelfreq, 2);
        setupworldnoiselayer(lakeshape, seed ^ 0x43E7B5D9, terrain.lavalakeshapefreq, 2);
    }
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

static int remipworldchunk(cube *root, bool prepared, int &families)
{
    int merged = 0;
    loopi(8) remipworldchunk(root[i], ivec(i, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE),
                             WORLD_CHUNK_ROOT_SIZE >> 1, root, prepared, families, merged);
    return merged;
}

static void setworldcubetexture(cube &c, int texture, int toptexture = -1)
{
    solidfaces(c);
    c.material = MAT_AIR;
    loopi(6) c.texture[i] = texture;
    if(toptexture >= 0) c.texture[O_TOP] = toptexture;
}

static void setworldcubematerial(cube &c, int material)
{
    emptyfaces(c);
    c.material = material;
}

enum { WORLD_EMPTY, WORLD_STONE, WORLD_DIRT, WORLD_GRASS, WORLD_SAND, WORLD_SNOW, WORLD_WATER, WORLD_MIXED };
enum { BIOME_OCEAN, BIOME_SNOWY_MOUNTAIN, BIOME_DESERT, BIOME_FOREST, BIOME_PLAINS };

static float worldterrainsmoothstep(float low, float high, float value)
{
    if(high <= low) return value >= high ? 1.0f : 0.0f;
    float t = clamp((value - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static int generateworldterrainheight(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky)
{
    const float noisex = float(chunkx) * WORLD_CHUNK_BLOCKS + blockx + 10000.5f,
                noisey = float(chunky) * WORLD_CHUNK_BLOCKS + blocky - 10000.5f;
    float continentx = noisex, contingenty = noisey,
          featurex = noisex, featurey = noisey;
    ctx.continentwarp.DomainWarp(continentx, contingenty);
    ctx.featurewarp.DomainWarp(featurex, featurey);

    const float continental = ctx.continent.GetNoise(continentx, contingenty),
                mountains = ctx.mountains.GetNoise(featurex, featurey),
                erosion = ctx.erosion.GetNoise(featurex, featurey),
                hills = ctx.hills.GetNoise(noisex, noisey),
                detail = ctx.detail.GetNoise(noisex, noisey);
    const float landmask = worldterrainsmoothstep(ctx.terrain.landmasklow, ctx.terrain.landmaskhigh, continental),
                mountainmask = worldterrainsmoothstep(ctx.terrain.mountainmasklow,
                                                      ctx.terrain.mountainmaskhigh, continental);
    float ridge = 1.0f - fabs(mountains);
    ridge = ridge * ridge * ridge;

    const float height = ctx.terrain.sealevel
                       + continental * ctx.terrain.continentheight
                       + landmask * hills * ctx.terrain.hillheight
                       + mountainmask * ridge * ctx.terrain.mountainheight
                       - erosion * mountainmask * ctx.terrain.erosionheight
                       + detail * ctx.terrain.detailheight;
    return clamp(int(floor(height + 0.5f)), WORLD_MIN_HEIGHT + 1, WORLD_MAX_HEIGHT - 1) * WORLD_BLOCK_SIZE;
}

static void generateworldcoastmap(worldgencontext &ctx, int chunkx, int chunky)
{
    memset(ctx.coastmap, 0, sizeof(ctx.coastmap));
    if(ctx.terrain.coastwidth <= 0) return;

    const int maxcoastwidth = ctx.terrain.coastwidth + ctx.terrain.coastvariation,
              halo = maxcoastwidth + 1,
              mapsize = WORLD_CHUNK_BLOCKS + 2 * halo,
              maparea = mapsize * mapsize,
              fardistance = INT_MAX / 8,
              seaheight = ctx.terrain.sealevel * WORLD_BLOCK_SIZE;
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
                         : generateworldterrainheight(ctx, chunkx, chunky, blockx, blocky);
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
                    width = max(ctx.terrain.coastwidth
                              + ctx.biomeblend.GetNoise(noisex, noisey) * ctx.terrain.coastvariation,
                                0.0f);
        ctx.coastmap[y * WORLD_CHUNK_BLOCKS + x] =
            distance[(y + halo) * mapsize + x + halo] <= int(floor(width * 3.0f + 0.5f));
    }
}

static int generateworldbiome(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky,
                              int height)
{
    if(height < ctx.terrain.sealevel * WORLD_BLOCK_SIZE) return BIOME_OCEAN;

    const float noisex = float(chunkx) * WORLD_CHUNK_BLOCKS + blockx + 10000.5f,
                noisey = float(chunky) * WORLD_CHUNK_BLOCKS + blocky - 10000.5f,
                weirdness = ctx.weirdness.GetNoise(noisex, noisey),
                temperature = ctx.temperature.GetNoise(noisex, noisey)
                            + weirdness * ctx.terrain.weirdnessstrength,
                moisture = ctx.moisture.GetNoise(noisex, noisey)
                         - weirdness * ctx.terrain.weirdnessstrength,
                heightblocks = height / float(WORLD_BLOCK_SIZE);

    if(ctx.terrain.biomeblend <= 0)
    {
        if(heightblocks > ctx.terrain.snowheight) return BIOME_SNOWY_MOUNTAIN;
        if(temperature > ctx.terrain.deserttemperature && moisture < ctx.terrain.desertmoisture)
            return BIOME_DESERT;
        if(moisture > ctx.terrain.forestmoisture) return BIOME_FOREST;
        return BIOME_PLAINS;
    }

    const float blendblocks = ctx.terrain.biomeblend,
                temperatureblend = max(blendblocks * ctx.terrain.temperaturefreq * 2.0f, 0.001f),
                moistureblend = max(blendblocks * ctx.terrain.moisturefreq * 2.0f, 0.001f),
                selector = clamp((ctx.biomeblend.GetNoise(noisex, noisey) + 1.0f) * 0.5f, 0.0f, 1.0f),
                snowweight = worldterrainsmoothstep(ctx.terrain.snowheight - blendblocks * 0.5f,
                                                    ctx.terrain.snowheight + blendblocks * 0.5f,
                                                    heightblocks),
                hotweight = worldterrainsmoothstep(ctx.terrain.deserttemperature - temperatureblend,
                                                   ctx.terrain.deserttemperature + temperatureblend,
                                                   temperature),
                dryweight = 1.0f - worldterrainsmoothstep(ctx.terrain.desertmoisture - moistureblend,
                                                         ctx.terrain.desertmoisture + moistureblend,
                                                         moisture),
                forestweight = worldterrainsmoothstep(ctx.terrain.forestmoisture - moistureblend,
                                                      ctx.terrain.forestmoisture + moistureblend,
                                                      moisture);
    if(snowweight > selector) return BIOME_SNOWY_MOUNTAIN;
    if(hotweight * dryweight > selector) return BIOME_DESERT;
    if(forestweight > selector) return BIOME_FOREST;
    return BIOME_PLAINS;
}

static bool generateworldrock(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky,
                              int height)
{
    const float low = min(ctx.terrain.mountainstonelow, ctx.terrain.mountainstonehigh),
                high = max(ctx.terrain.mountainstonelow, ctx.terrain.mountainstonehigh),
                heightblocks = height / float(WORLD_BLOCK_SIZE);
    if(heightblocks <= low) return false;
    if(heightblocks >= high) return true;

    const float noisex = float(chunkx) * WORLD_CHUNK_BLOCKS + blockx + 10000.5f,
                noisey = float(chunky) * WORLD_CHUNK_BLOCKS + blocky - 10000.5f,
                rockweight = worldterrainsmoothstep(low, high, heightblocks),
                selector = clamp(ctx.rockiness.GetNoise(noisex, noisey) * 1.25f + 0.5f, 0.0f, 1.0f);
    return rockweight > selector;
}

static void generateworldheightmap(worldgencontext &ctx, int chunkx, int chunky)
{
    loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
    {
        const int index = y * WORLD_CHUNK_BLOCKS + x;
        ctx.heightmap[index] = generateworldterrainheight(ctx, chunkx, chunky, x, y);
    }
    generateworldcoastmap(ctx, chunkx, chunky);
    loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
    {
        const int index = y * WORLD_CHUNK_BLOCKS + x;
        ctx.biomemap[index] = generateworldbiome(ctx, chunkx, chunky, x, y, ctx.heightmap[index]);
        ctx.rockmap[index] = generateworldrock(ctx, chunkx, chunky, x, y, ctx.heightmap[index]);
    }
}

static int worldterrainheight(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.heightmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE];
}

static int worldterrainbiome(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.biomemap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE];
}

static bool worldterraincoast(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.coastmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE] != 0;
}

static bool worldterrainrock(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.rockmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE] != 0;
}

static int worldcolumncubetype(const worldgencontext &ctx, int z, int size, int height,
                               int biome, bool coast, bool rock)
{
    const int surface = WORLD_GROUND_HEIGHT + height,
              watertop = WORLD_GROUND_HEIGHT + ctx.terrain.sealevel * WORLD_BLOCK_SIZE,
              dirtbottom = surface - WORLD_BLOCK_SIZE - WORLD_DIRT_DEPTH,
              grassbottom = surface - WORLD_BLOCK_SIZE,
              beachmin = (ctx.terrain.sealevel
                        + min(ctx.terrain.beachminheight, ctx.terrain.beachmaxheight)) * WORLD_BLOCK_SIZE,
              beachmax = (ctx.terrain.sealevel
                        + max(ctx.terrain.beachminheight, ctx.terrain.beachmaxheight)) * WORLD_BLOCK_SIZE;
    const bool beach = coast && height >= beachmin && height <= beachmax;

    if(z >= max(surface, watertop)) return WORLD_EMPTY;
    if(surface < watertop && z >= surface && z + size <= watertop) return WORLD_WATER;
    if(z + size <= dirtbottom) return WORLD_STONE;
    if(rock)
    {
        if(biome == BIOME_SNOWY_MOUNTAIN && z >= grassbottom && z + size <= surface) return WORLD_SNOW;
        if(z >= dirtbottom && z + size <= surface) return WORLD_STONE;
        return WORLD_MIXED;
    }
    if(beach || biome == BIOME_DESERT)
    {
        if(z >= dirtbottom && z + size <= surface) return WORLD_SAND;
        return WORLD_MIXED;
    }
    if(biome == BIOME_OCEAN)
    {
        if(z >= dirtbottom && z + size <= surface) return WORLD_DIRT;
        return WORLD_MIXED;
    }
    if(z >= dirtbottom && z + size <= grassbottom) return WORLD_DIRT;
    if(biome == BIOME_SNOWY_MOUNTAIN && z >= grassbottom && z + size <= surface) return WORLD_SNOW;
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
        int columntype = worldcolumncubetype(ctx, o.z, size, worldterrainheight(ctx, x, y),
                                            worldterrainbiome(ctx, x, y), worldterraincoast(ctx, x, y),
                                            worldterrainrock(ctx, x, y));
        if(columntype == WORLD_MIXED || (type >= 0 && type != columntype)) return WORLD_MIXED;
        type = columntype;
    }
    return type;
}

static int worldrepresentativecubetype(const worldgencontext &ctx, const ivec &o, int size)
{
    const int x = clamp(o.x + size / 2, 0, WORLD_CHUNK_SIZE - 1),
              y = clamp(o.y + size / 2, 0, WORLD_CHUNK_SIZE - 1),
              height = worldterrainheight(ctx, x, y),
              biome = worldterrainbiome(ctx, x, y),
              surface = WORLD_GROUND_HEIGHT + height,
              watertop = WORLD_GROUND_HEIGHT + ctx.terrain.sealevel * WORLD_BLOCK_SIZE,
              visibletop = max(surface, watertop);
    int z = clamp(o.z + size / 2, 0, WORLD_MAP_SIZE - 1);

    // A coarse cube intersecting the visible column top represents its
    // surface, not the greater volume underneath it. Sample immediately below
    // that top so grass/sand/snow/stone wins over dirt, and water wins for a
    // submerged terrain column. Cubes wholly underground retain the centre
    // sample used for their dominant interior material.
    if(visibletop > o.z && visibletop <= o.z + size)
        z = clamp(visibletop - 1, 0, WORLD_MAP_SIZE - 1);

    return worldcolumncubetype(ctx, z, 1, height, biome, worldterraincoast(ctx, x, y),
                               worldterrainrock(ctx, x, y));
}

static void generateworldcube(worldgencontext &ctx, cube &c, const ivec &o, int size, int mingridsize)
{
    int type = worldcubetype(ctx, o, size);
    if(type == WORLD_MIXED && size <= mingridsize)
        type = worldrepresentativecubetype(ctx, o, size);
    switch(type)
    {
        case WORLD_EMPTY:
            setworldcubematerial(c, MAT_AIR);
            return;

        case WORLD_STONE:
            setworldcubetexture(c, ctx.stonetexture);
            return;

        case WORLD_DIRT:
            setworldcubetexture(c, ctx.dirttexture);
            return;

        case WORLD_GRASS:
            setworldcubetexture(c, ctx.grasssidetexture, ctx.grasstexture);
            return;

        case WORLD_SAND:
            setworldcubetexture(c, ctx.sandtexture);
            return;

        case WORLD_SNOW:
            setworldcubetexture(c, ctx.snowtexture);
            return;

        case WORLD_WATER:
            setworldcubematerial(c, MAT_WATER);
            return;
    }

    if(size <= mingridsize)
    {
        setworldcubematerial(c, MAT_AIR);
        return;
    }

    c.children = allocworldgenfamily(ctx);
    const int childsize = size >> 1;
    loopi(8) generateworldcube(ctx, c.children[i], ivec(i, o, childsize), childsize, mingridsize);
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

static void addworldtreeblock(vector<ivec> &blocks, int blockx, int blocky, int blockz)
{
    if(blockx < 0 || blockx >= WORLD_CHUNK_BLOCKS ||
       blocky < 0 || blocky >= WORLD_CHUNK_BLOCKS ||
       blockz < 0 || blockz >= WORLD_HEIGHT_BLOCKS) return;
    blocks.add(ivec(blockx * WORLD_BLOCK_SIZE, blocky * WORLD_BLOCK_SIZE, blockz * WORLD_BLOCK_SIZE));
}

static void addworldregulartree(vector<ivec> &wood, vector<ivec> &leaves, int blockx, int blocky,
                                int basez, int height, uint shapehash)
{
    loop(z, height) addworldtreeblock(wood, blockx, blocky, basez + z);

    for(int z = height - 2; z <= height; ++z)
    {
        const int radius = z == height ? 1 : 2;
        for(int y = -radius; y <= radius; ++y) for(int x = -radius; x <= radius; ++x)
        {
            if(radius == 2 && abs(x) == 2 && abs(y) == 2 &&
               (hashworldtree(shapehash, x, y, z, height, 0xA511E9B3U) & 1U)) continue;
            addworldtreeblock(leaves, blockx + x, blocky + y, basez + z);
        }
    }
}

static void addworldpinetree(vector<ivec> &wood, vector<ivec> &leaves, int blockx, int blocky,
                             int basez, int height)
{
    loop(z, height) addworldtreeblock(wood, blockx, blocky, basez + z);
    addworldtreeblock(leaves, blockx, blocky, basez + height);

    for(int z = 2; z < height; ++z)
    {
        const int fromtop = height - z,
                  radius = min(3, 1 + fromtop / 3);
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

static bool generateworldcaveentrance(const worldgencontext &ctx, int chunkx, int chunky,
                                      int blockx, int blocky, int height)
{
    const int mindepth = min(ctx.terrain.cavemindepth, ctx.terrain.cavefulldepth),
              logicalz = height / WORLD_BLOCK_SIZE - 1;
    const float tunnelweight = worldterrainsmoothstep(1.0f, float(mindepth), 1.0f),
                veinwidth = ctx.terrain.caveentrancewidth
                          + (ctx.terrain.tunnelwidth - ctx.terrain.caveentrancewidth)
                          * tunnelweight,
                noisex = float(chunkx) * WORLD_CHUNK_BLOCKS + blockx + 17500.5f,
                noisey = float(chunky) * WORLD_CHUNK_BLOCKS + blocky - 17500.5f,
                noisez = logicalz + 3500.5f;
    return fabs(ctx.tunnela.GetNoise(noisex, noisey, noisez)) < veinwidth &&
           fabs(ctx.tunnelb.GetNoise(noisex, noisey, noisez)) < veinwidth;
}

static void generateworldcheesecaves(worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky)
{
    const int bottomlayers = clamp(ctx.terrain.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS)),
              minheight = WORLD_MIN_HEIGHT + bottomlayers,
              mindepth = min(ctx.terrain.cavemindepth, ctx.terrain.cavefulldepth),
              fulldepth = max(ctx.terrain.cavemindepth, ctx.terrain.cavefulldepth);
    const float deepdenominator = max(float(ctx.terrain.cavedeepheight - minheight), 1.0f);

    loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
    {
        const int surfaceheight = ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE,
                  caveceiling = min(surfaceheight - 1, WORLD_MAX_HEIGHT - 1);
        for(int logicalz = minheight; logicalz <= caveceiling; ++logicalz)
        {
            const float depth = float(surfaceheight - logicalz),
                        depthweight = worldterrainsmoothstep(float(mindepth), float(fulldepth), depth),
                        tunnelweight = worldterrainsmoothstep(1.0f, float(mindepth), depth),
                        veinwidth = ctx.terrain.caveentrancewidth
                                  + (ctx.terrain.tunnelwidth - ctx.terrain.caveentrancewidth)
                                  * tunnelweight,
                        surfacepenalty = (1.0f - depthweight) * 0.35f,
                        deepweight = clamp((ctx.terrain.cavedeepheight - logicalz) / deepdenominator,
                                           0.0f, 1.0f),
                        largecavethreshold = ctx.terrain.largecavethreshold
                                           + (ctx.terrain.largecavedeepthreshold
                                            - ctx.terrain.largecavethreshold) * deepweight
                                           + surfacepenalty;
            const float noisex = float(chunkx) * WORLD_CHUNK_BLOCKS + x + 17500.5f,
                        noisey = float(chunky) * WORLD_CHUNK_BLOCKS + y - 17500.5f,
                        noisez = logicalz + 3500.5f;
            bool carve = fabs(ctx.tunnela.GetNoise(noisex, noisey, noisez)) < veinwidth &&
                         fabs(ctx.tunnelb.GetNoise(noisex, noisey, noisez)) < veinwidth;
            if(!carve && depth >= mindepth)
                carve = ctx.caves.GetNoise(noisex, noisey, noisez)
                            > ctx.terrain.cavethreshold + surfacepenalty ||
                        ctx.largecaves.GetNoise(noisex, noisey, noisez)
                            > largecavethreshold;
            if(carve)
                carvemap[worldcarveindex(x, y, logicalz - WORLD_MIN_HEIGHT)] = WORLD_CARVE_AIR;
        }
    }
}

static void generateworldlavalakes(worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky)
{
    const int spacing = max(ctx.terrain.lavalakespacing, 1),
              verticalspacing = max(spacing / 2, 8),
              minradius = min(ctx.terrain.lavalakeminsize, ctx.terrain.lavalakemaxsize),
              maxradius = max(ctx.terrain.lavalakeminsize, ctx.terrain.lavalakemaxsize),
              bottomlayers = clamp(ctx.terrain.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS)),
              minimumheight = WORLD_MIN_HEIGHT + bottomlayers,
              startheight = max(ctx.terrain.lavalakestartheight, ctx.terrain.lavalakedeepheight),
              deepheight = min(ctx.terrain.lavalakestartheight, ctx.terrain.lavalakedeepheight);
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
        const uint positionhash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, 0xC13FA9A9U),
                   chancehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, 0x91E10DA5U),
                   sizehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, 0xD192ED03U);
        const long long centerx = cellx * spacing + int(positionhash % uint(spacing)),
                        centery = celly * spacing + int((positionhash >> 8) % uint(spacing));
        const int centerz = cellz * verticalspacing
                          + int((positionhash >> 16) % uint(verticalspacing));
        if(centerz < minimumheight || centerz > startheight) continue;

        const float approachweight = deepheight < startheight
                                   ? clamp((startheight - centerz) / float(startheight - deepheight),
                                           0.0f, 1.0f)
                                   : 1.0f,
                    deepweight = deepheight > minimumheight
                               ? clamp((deepheight - centerz) / float(deepheight - minimumheight),
                                       0.0f, 1.0f)
                               : centerz <= deepheight ? 1.0f : 0.0f,
                    lakechance = ctx.terrain.lavalakeshallowchance * approachweight
                               + (ctx.terrain.lavalakedeepchance
                                - ctx.terrain.lavalakeshallowchance) * deepweight;
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
                    shapevariation = clamp(ctx.terrain.lavalakeshapevariation, 0.0f, 0.75f);
        const int centerlocalx = int(centerx - chunkstartx),
                  centerlocaly = int(centery - chunkstarty);
        if(centerlocalx + radius < 0 || centerlocalx - radius >= WORLD_CHUNK_BLOCKS ||
           centerlocaly + radius < 0 || centerlocaly - radius >= WORLD_CHUNK_BLOCKS) continue;

        const int centerblockx = int(centerx - (long long)chunkx * WORLD_CHUNK_BLOCKS),
                  centerblocky = int(centery - (long long)chunky * WORLD_CHUNK_BLOCKS),
                  centerheight = generateworldterrainheight(ctx, chunkx, chunky,
                                                           centerblockx, centerblocky) / WORLD_BLOCK_SIZE;
        if(centerz + verticalradius > centerheight - ctx.terrain.cavemindepth) continue;

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
                        shapenoise = ctx.lakeshape.GetNoise(float(chunkx) * WORLD_CHUNK_BLOCKS + x + 9200.5f,
                                                           float(chunky) * WORLD_CHUNK_BLOCKS + y - 9200.5f),
                        boundary = 1.0f - shapevariation * 0.5f
                                 + shapenoise * shapevariation * 0.5f;
            if(horizontal > boundary) continue;

            const int surfaceheight = ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE;
            for(int logicalz = zmin; logicalz <= zmax; ++logicalz)
            {
                if(surfaceheight - logicalz < ctx.terrain.cavemindepth) continue;
                const float dz = (logicalz - centerz) / float(verticalradius);
                if(horizontal + dz * dz > boundary) continue;

                uchar &carve = carvemap[worldcarveindex(x, y, logicalz - WORLD_MIN_HEIGHT)];
                if(logicalz <= lavalevel) carve = WORLD_CARVE_LAVA;
                else if(carve == WORLD_CARVE_NONE) carve = WORLD_CARVE_AIR;
            }
        }
    }
}

static void placeworldcaves(worldgencontext &ctx, cube *root, int chunkx, int chunky)
{
    const int mapblocks = WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS * WORLD_HEIGHT_BLOCKS;
    vector<uchar> carvemap;
    uchar *carve = carvemap.pad(mapblocks);
    memset(carve, WORLD_CARVE_NONE, mapblocks * sizeof(uchar));

    generateworldcheesecaves(ctx, carve, chunkx, chunky);
    generateworldlavalakes(ctx, carve, chunkx, chunky);

    const int bottomlayers = clamp(ctx.terrain.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS));
    loop(z, bottomlayers) loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
        carve[worldcarveindex(x, y, z)] = WORLD_CARVE_LAVA;

    loop(z, WORLD_HEIGHT_BLOCKS) loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
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

static void placeworldtrees(worldgencontext &ctx, cube *root, int chunkx, int chunky)
{
    vector<ivec> wood, leaves;
    const int halo = 3,
              beachmin = (ctx.terrain.sealevel
                        + min(ctx.terrain.beachminheight, ctx.terrain.beachmaxheight)) * WORLD_BLOCK_SIZE,
              beachmax = (ctx.terrain.sealevel
                        + max(ctx.terrain.beachminheight, ctx.terrain.beachmaxheight)) * WORLD_BLOCK_SIZE;

    for(int y = -halo; y < WORLD_CHUNK_BLOCKS + halo; ++y)
    for(int x = -halo; x < WORLD_CHUNK_BLOCKS + halo; ++x)
    {
        const bool inside = x >= 0 && x < WORLD_CHUNK_BLOCKS &&
                            y >= 0 && y < WORLD_CHUNK_BLOCKS;
        const int index = inside ? y * WORLD_CHUNK_BLOCKS + x : 0,
                  height = inside ? ctx.heightmap[index]
                                  : generateworldterrainheight(ctx, chunkx, chunky, x, y),
                  biome = inside ? ctx.biomemap[index]
                                 : generateworldbiome(ctx, chunkx, chunky, x, y, height);
        if(biome != BIOME_FOREST && biome != BIOME_PLAINS) continue;
        if(inside ? ctx.rockmap[index] != 0
                  : generateworldrock(ctx, chunkx, chunky, x, y, height)) continue;
        if(ctx.terrain.coastwidth > 0 && height >= beachmin && height <= beachmax) continue;
        if(generateworldcaveentrance(ctx, chunkx, chunky, x, y, height)) continue;

        const float density = biome == BIOME_FOREST
                            ? ctx.terrain.foresttreedensity
                            : ctx.terrain.plainstreedensity;
        const uint spawn = hashworldtree(uint(ctx.seed), chunkx, chunky, x, y, 0xD1B54A35U);
        if(worldtreeunit(spawn) >= density) continue;

        const float heightblocks = height / float(WORLD_BLOCK_SIZE),
                    pinelow = float(min(ctx.terrain.pinestartheight, ctx.terrain.pinefullheight)),
                    pinehigh = float(max(ctx.terrain.pinestartheight, ctx.terrain.pinefullheight)),
                    pinechance = worldterrainsmoothstep(pinelow, pinehigh, heightblocks);
        const uint shape = hashworldtree(uint(ctx.seed), chunkx, chunky, x, y, 0x94D049BBU);
        const bool pine = worldtreeunit(shape) < pinechance;
        const int treeheight = pine ? 6 + int((shape >> 24) & 3U)
                                    : 4 + int((shape >> 24) % 3U),
                  basez = WORLD_GROUND_HEIGHT / WORLD_BLOCK_SIZE + height / WORLD_BLOCK_SIZE;
        if(basez + treeheight >= WORLD_HEIGHT_BLOCKS) continue;

        if(pine) addworldpinetree(wood, leaves, x, y, basez, treeheight);
        else addworldregulartree(wood, leaves, x, y, basez, treeheight, shape);
    }

    loopv(leaves)
    {
        cube &c = lookupworldgenblock(ctx, root, leaves[i]);
        if(isempty(c) && c.material == MAT_AIR) setworldcubetexture(c, ctx.leaftexture);
    }
    loopv(wood)
    {
        cube &c = lookupworldgenblock(ctx, root, wood[i]);
        if((isempty(c) && c.material == MAT_AIR) || c.texture[0] == ctx.leaftexture)
            setworldcubetexture(c, ctx.woodtexture);
    }
}

static cube *generateworldchunk(int chunkx, int chunky, worldgencontext &ctx)
{
    generateworldheightmap(ctx, chunkx, chunky);
    cube *root = allocworldgenfamily(ctx);
    const int rootsize = WORLD_CHUNK_ROOT_SIZE;
    loopi(8) generateworldcube(ctx, root[i], ivec(i, ivec(0, 0, 0), rootsize), rootsize, WORLD_BLOCK_SIZE);
    placeworldcaves(ctx, root, chunkx, chunky);
    placeworldtrees(ctx, root, chunkx, chunky);
    ctx.optimized = remipworldchunk(root, ctx.prepared, ctx.families);
    return root;
}

static cube *generateworldchunk(int chunkx, int chunky)
{
    const terrainsettings terrain;
    worldgencontext ctx(activeworldseed, worldgrasstexture, worldgrasssidetexture,
                        worlddirttexture, worldstonetexture, worldsandtexture, worldsnowtexture,
                        worldwoodtexture, worldleaftexture, false, terrain);
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

enum { OCTSAV_CHILDREN = 0, OCTSAV_EMPTY, OCTSAV_SOLID, OCTSAV_NORMAL };

#define LM_PACKW 512
#define LM_PACKH 512
#define LAYER_DUP (1<<7)

struct polysurfacecompat
{
    uchar lmid[2];
    uchar verts, numverts;
};

static int savemapprogress = 0;

void savec(cube *c, const ivec &o, int size, stream *f, bool nolms)
{
    if((savemapprogress++&0xFFF)==0) renderprogress(float(savemapprogress)/allocnodes, "saving octree...");

    loopi(8)
    {
        ivec co(i, o, size);
        if(c[i].children)
        {
            f->putchar(OCTSAV_CHILDREN);
            savec(c[i].children, co, size>>1, f, nolms);
        }
        else
        {
            int oflags = 0, surfmask = 0, totalverts = 0;
            if(c[i].material!=MAT_AIR) oflags |= 0x40;
            if(isempty(c[i])) f->putchar(oflags | OCTSAV_EMPTY);
            else
            {
                if(!nolms)
                {
                    if(c[i].merged) oflags |= 0x80;
                    if(c[i].ext) loopj(6)
                    {
                        const surfaceinfo &surf = c[i].ext->surfaces[j];
                        if(!surf.used()) continue;
                        oflags |= 0x20;
                        surfmask |= 1<<j;
                        totalverts += surf.totalverts();
                    }
                }

                if(isentirelysolid(c[i])) f->putchar(oflags | OCTSAV_SOLID);
                else
                {
                    f->putchar(oflags | OCTSAV_NORMAL);
                    f->write(c[i].edges, 12);
                }
            }

            loopj(6) f->putlil<ushort>(c[i].texture[j]);

            if(oflags&0x40) f->putlil<ushort>(c[i].material);
            if(oflags&0x80) f->putchar(c[i].merged);
            if(oflags&0x20)
            {
                f->putchar(surfmask);
                f->putchar(totalverts);
                loopj(6) if(surfmask&(1<<j))
                {
                    surfaceinfo surf = c[i].ext->surfaces[j];
                    vertinfo *verts = c[i].ext->verts() + surf.verts;
                    int layerverts = surf.numverts&MAXFACEVERTS, numverts = surf.totalverts(),
                        vertmask = 0, vertorder = 0,
                        dim = dimension(j), vc = C[dim], vr = R[dim];
                    if(numverts)
                    {
                        if(c[i].merged&(1<<j))
                        {
                            vertmask |= 0x04;
                            if(layerverts == 4)
                            {
                                ivec v[4] = { verts[0].getxyz(), verts[1].getxyz(), verts[2].getxyz(), verts[3].getxyz() };
                                loopk(4)
                                {
                                    const ivec &v0 = v[k], &v1 = v[(k+1)&3], &v2 = v[(k+2)&3], &v3 = v[(k+3)&3];
                                    if(v1[vc] == v0[vc] && v1[vr] == v2[vr] && v3[vc] == v2[vc] && v3[vr] == v0[vr])
                                    {
                                        vertmask |= 0x01;
                                        vertorder = k;
                                        break;
                                    }
                                }
                            }
                        }
                        else
                        {
                            int vis = visibletris(c[i], j, co, size);
                            if(vis&4 || faceconvexity(c[i], j) < 0) vertmask |= 0x01;
                            if(layerverts < 4 && vis&2) vertmask |= 0x02;
                        }
                        bool matchnorm = true;
                        loopk(numverts)
                        {
                            const vertinfo &v = verts[k];
                            if(v.norm) { vertmask |= 0x80; if(v.norm != verts[0].norm) matchnorm = false; }
                        }
                        if(matchnorm) vertmask |= 0x08;
                    }
                    surf.verts = vertmask;
                    f->write(&surf, sizeof(surf));
                    bool hasxyz = (vertmask&0x04)!=0, hasnorm = (vertmask&0x80)!=0;
                    if(layerverts == 4)
                    {
                        if(hasxyz && vertmask&0x01)
                        {
                            ivec v0 = verts[vertorder].getxyz(), v2 = verts[(vertorder+2)&3].getxyz();
                            f->putlil<ushort>(v0[vc]); f->putlil<ushort>(v0[vr]);
                            f->putlil<ushort>(v2[vc]); f->putlil<ushort>(v2[vr]);
                            hasxyz = false;
                        }
                    }
                    if(hasnorm && vertmask&0x08) { f->putlil<ushort>(verts[0].norm); hasnorm = false; }
                    if(hasxyz || hasnorm) loopk(layerverts)
                    {
                        const vertinfo &v = verts[(k+vertorder)%layerverts];
                        if(hasxyz)
                        {
                            ivec xyz = v.getxyz();
                            f->putlil<ushort>(xyz[vc]); f->putlil<ushort>(xyz[vr]);
                        }
                        if(hasnorm) f->putlil<ushort>(v.norm);
                    }
                }
            }
        }
    }
}

cube *loadchildren(stream *f, const ivec &co, int size, bool &failed);

void loadc(stream *f, cube &c, const ivec &co, int size, bool &failed)
{
    int octsav = f->getchar();
    switch(octsav&0x7)
    {
        case OCTSAV_CHILDREN:
            c.children = loadchildren(f, co, size>>1, failed);
            return;

        case OCTSAV_EMPTY:  emptyfaces(c);        break;
        case OCTSAV_SOLID:  solidfaces(c);        break;
        case OCTSAV_NORMAL: f->read(c.edges, 12); break;
        default: failed = true; return;
    }
    loopi(6) c.texture[i] = f->getlil<ushort>();
    if(octsav&0x40) c.material = f->getlil<ushort>();
    if(octsav&0x80) c.merged = f->getchar();
    if(octsav&0x20)
    {
        int surfmask, totalverts;
        surfmask = f->getchar();
        totalverts = max(f->getchar(), 0);
        newcubeext(c, totalverts, false);
        memset(c.ext->surfaces, 0, sizeof(c.ext->surfaces));
        memset(c.ext->verts(), 0, totalverts*sizeof(vertinfo));
        int offset = 0;
        loopi(6) if(surfmask&(1<<i))
        {
            surfaceinfo &surf = c.ext->surfaces[i];
            if(mapversion <= 0)
            {
                polysurfacecompat psurf;
                f->read(&psurf, sizeof(polysurfacecompat));
                surf.verts = psurf.verts;
                surf.numverts = psurf.numverts;
            }
            else f->read(&surf, sizeof(surf));
            int vertmask = surf.verts, numverts = surf.totalverts();
            if(!numverts) { surf.verts = 0; continue; }
            surf.verts = offset;
            vertinfo *verts = c.ext->verts() + offset;
            offset += numverts;
            ivec v[4], n, vo = ivec(co).mask(0xFFF).shl(3);
            int layerverts = surf.numverts&MAXFACEVERTS, dim = dimension(i), vc = C[dim], vr = R[dim], bias = 0;
            genfaceverts(c, i, v);
            bool hasxyz = (vertmask&0x04)!=0, hasuv = mapversion <= 0 && (vertmask&0x40)!=0, hasnorm = (vertmask&0x80)!=0;
            if(hasxyz)
            {
                ivec e1, e2, e3;
                n.cross((e1 = v[1]).sub(v[0]), (e2 = v[2]).sub(v[0]));
                if(n.iszero()) n.cross(e2, (e3 = v[3]).sub(v[0]));
                bias = -n.dot(ivec(v[0]).mul(size).add(vo));
            }
            else
            {
                int vis = layerverts < 4 ? (vertmask&0x02 ? 2 : 1) : 3, order = vertmask&0x01 ? 1 : 0, k = 0;
                verts[k++].setxyz(v[order].mul(size).add(vo));
                if(vis&1) verts[k++].setxyz(v[order+1].mul(size).add(vo));
                verts[k++].setxyz(v[order+2].mul(size).add(vo));
                if(vis&2) verts[k++].setxyz(v[(order+3)&3].mul(size).add(vo));
            }
            if(layerverts == 4)
            {
                if(hasxyz && vertmask&0x01)
                {
                    ushort c1 = f->getlil<ushort>(), r1 = f->getlil<ushort>(), c2 = f->getlil<ushort>(), r2 = f->getlil<ushort>();
                    ivec xyz;
                    xyz[vc] = c1; xyz[vr] = r1; xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                    verts[0].setxyz(xyz);
                    xyz[vc] = c1; xyz[vr] = r2; xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                    verts[1].setxyz(xyz);
                    xyz[vc] = c2; xyz[vr] = r2; xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                    verts[2].setxyz(xyz);
                    xyz[vc] = c2; xyz[vr] = r1; xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                    verts[3].setxyz(xyz);
                    hasxyz = false;
                }
                if(hasuv && vertmask&0x02)
                {
                    loopk(4) f->getlil<ushort>();
                    if(surf.numverts&LAYER_DUP) loopk(4) f->getlil<ushort>();
                    hasuv = false;
                }
            }
            if(hasnorm && vertmask&0x08)
            {
                ushort norm = f->getlil<ushort>();
                loopk(layerverts) verts[k].norm = norm;
                hasnorm = false;
            }
            if(hasxyz || hasuv || hasnorm) loopk(layerverts)
            {
                vertinfo &v = verts[k];
                if(hasxyz)
                {
                    ivec xyz;
                    xyz[vc] = f->getlil<ushort>(); xyz[vr] = f->getlil<ushort>();
                    xyz[dim] = n[dim] ? -(bias + n[vc]*xyz[vc] + n[vr]*xyz[vr])/n[dim] : vo[dim];
                    v.setxyz(xyz);
                }
                if(hasuv) { f->getlil<ushort>(); f->getlil<ushort>(); }
                if(hasnorm) v.norm = f->getlil<ushort>();
            }
            if(hasuv && surf.numverts&LAYER_DUP) loopk(layerverts) { f->getlil<ushort>(); f->getlil<ushort>(); }
        }
    }
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

static bool skipworldchunkvslots(stream *f, int remaining)
{
    while(remaining > 0)
    {
        int changed = f->getlil<int>();
        if(changed < 0)
        {
            if(-changed > remaining) return false;
            remaining += changed;
            continue;
        }

        f->getlil<int>(); // previous variant
        remaining--;
        if(changed & (1 << VSLOT_SHPARAM))
        {
            int params = f->getlil<ushort>();
            loopi(params)
            {
                int len = f->getlil<ushort>();
                f->seek(len + 4 * sizeof(float), SEEK_CUR);
            }
        }
        if(changed & (1 << VSLOT_SCALE)) f->seek(sizeof(float), SEEK_CUR);
        if(changed & (1 << VSLOT_ROTATION)) f->seek(sizeof(int), SEEK_CUR);
        if(changed & (1 << VSLOT_OFFSET)) f->seek(2 * sizeof(int), SEEK_CUR);
        if(changed & (1 << VSLOT_SCROLL)) f->seek(2 * sizeof(float), SEEK_CUR);
        if(changed & (1 << VSLOT_LAYER)) f->seek(sizeof(int), SEEK_CUR);
        if(changed & (1 << VSLOT_ALPHA)) f->seek(2 * sizeof(float), SEEK_CUR);
        if(changed & (1 << VSLOT_COLOR)) f->seek(3 * sizeof(float), SEEK_CUR);
        if(changed & (1 << VSLOT_REFRACT)) f->seek(4 * sizeof(float), SEEK_CUR);
        if(changed & (1 << VSLOT_DETAIL)) f->seek(sizeof(int), SEEK_CUR);
    }
    return true;
}

static cube *loadworldchunkroot(const char *mname)
{
    string name;
    validmapname(name, mname);
    defformatstring(filename, "media/map/%s.ogz", name);
    path(filename);
    stream *f = opengzfile(filename, "rb");
    if(!f) return NULL;

    mapheader hdr;
    octaheader ohdr;
    memset(&ohdr, 0, sizeof(ohdr));
    if(!loadmapheader(f, filename, hdr, ohdr) || hdr.worldsize != WORLD_CHUNK_MAP_SIZE || hdr.numvslots < 0)
    {
        delete f;
        return NULL;
    }

    loopi(hdr.numvars)
    {
        int type = f->getchar(), len = f->getlil<ushort>();
        f->seek(len, SEEK_CUR);
        switch(type)
        {
            case ID_VAR: f->seek(sizeof(int), SEEK_CUR); break;
            case ID_FVAR: f->seek(sizeof(float), SEEK_CUR); break;
            case ID_SVAR:
            {
                int slen = f->getlil<ushort>();
                f->seek(slen, SEEK_CUR);
                break;
            }
            default: delete f; return NULL;
        }
    }

    int gamelen = f->getchar();
    if(gamelen < 0) { delete f; return NULL; }
    f->seek(gamelen + 1, SEEK_CUR);
    int eif = f->getlil<ushort>(), extrasize = f->getlil<ushort>();
    f->seek(extrasize, SEEK_CUR);

    int nummru = f->getlil<ushort>();
    f->seek(nummru * sizeof(ushort), SEEK_CUR);
    f->seek(hdr.numents * (sizeof(entity) + eif), SEEK_CUR);
    if(!skipworldchunkvslots(f, hdr.numvslots)) { delete f; return NULL; }

    int oldmapversion = mapversion;
    mapversion = hdr.version;
    bool failed = false;
    cube *root = loadchildren(f, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE, failed);
    mapversion = oldmapversion;
    delete f;
    if(failed)
    {
        freeocta(root);
        return NULL;
    }
    validatec(root, WORLD_CHUNK_ROOT_SIZE);
    return root;
}

static cube *newpreparedfamily(int &families)
{
    cube *c = new cube[8];
    loopi(8) resetworldcube(c[i]);
    families++;
    return c;
}

struct worldchunkmemorystream : stream
{
    const uchar *data;
    size_t length, pos;

    worldchunkmemorystream(const uchar *data, size_t length) : data(data), length(length), pos(0) {}
    void close() { pos = length; }
    bool end() { return pos >= length; }
    offset tell() { return pos; }
    offset size() { return length; }
    bool seek(offset off, int whence)
    {
        offset target = whence == SEEK_CUR ? offset(pos) + off :
                        whence == SEEK_END ? offset(length) + off : off;
        if(target < 0 || target > offset(length)) return false;
        pos = size_t(target);
        return true;
    }
    size_t read(void *dst, size_t len)
    {
        len = min(len, length - pos);
        if(len) memcpy(dst, data + pos, len);
        pos += len;
        return len;
    }
};

static cube *loadpreparedchildren(stream *f, int &families, bool &failed, int &loaderror);

static void loadpreparedcube(stream *f, cube &c, int &families, bool &failed, int &loaderror)
{
    int octsav = f->getchar();
    if(octsav < 0) { failed = true; loaderror = 6; return; }
    switch(octsav & 0x7)
    {
        case OCTSAV_CHILDREN:
            c.children = loadpreparedchildren(f, families, failed, loaderror);
            return;

        case OCTSAV_EMPTY:  emptyfaces(c);        break;
        case OCTSAV_SOLID:  solidfaces(c);        break;
        case OCTSAV_NORMAL:
            if(f->read(c.edges, 12) != 12) { failed = true; loaderror = 6; return; }
            break;

        default:
            failed = true;
            loaderror = 6;
            return;
    }

    loopi(6) c.texture[i] = f->getlil<ushort>();
    if(octsav & 0x40) c.material = f->getlil<ushort>();
    if(octsav & 0x80) c.merged = f->getchar();

    // Procedural chunks are always saved without baked lightmap surfaces.
    // Reject unexpected surface data instead of touching render-owned cubeext state on the worker.
    if(octsav & 0x20) { failed = true; loaderror = 7; }
}

static cube *loadpreparedchildren(stream *f, int &families, bool &failed, int &loaderror)
{
    cube *c = newpreparedfamily(families);
    loopi(8)
    {
        loadpreparedcube(f, c[i], families, failed, loaderror);
        if(failed) break;
    }
    return c;
}

static cube *loadpreparedworldchunk(const char *filename, int &families, int &optimized, int &loaderror)
{
    gzFile gz = gzopen(filename, "rb");
    if(!gz) { loaderror = 1; return NULL; }
    vector<uchar> contents;
    for(;;)
    {
        const int blocksize = 16 * 1024, oldlen = contents.length();
        uchar *dst = contents.pad(blocksize);
        int len = gzread(gz, dst, blocksize);
        if(len < 0)
        {
            contents.setsize(oldlen);
            gzclose(gz);
            loaderror = 1;
            return NULL;
        }
        contents.setsize(oldlen + len);
        if(len < blocksize) break;
    }
    if(gzclose(gz) != Z_OK || contents.empty()) { loaderror = 1; return NULL; }
    worldchunkmemorystream input(contents.getbuf(), contents.length());
    stream *f = &input;

    mapheader hdr;
    memset(&hdr, 0, sizeof(hdr));
    bool failed = f->read(&hdr, 3 * sizeof(int)) != 3 * sizeof(int);
    if(failed) loaderror = 2;
    if(!failed)
    {
        lilswap(&hdr.version, 2);
        failed = memcmp(hdr.magic, "TMAP", 4) || hdr.version > MAPVERSION ||
                 f->read(&hdr.worldsize, 6 * sizeof(int)) != 6 * sizeof(int);
        if(failed) loaderror = 2;
    }
    if(!failed)
    {
        lilswap(&hdr.worldsize, 6);
        failed = hdr.worldsize != WORLD_CHUNK_MAP_SIZE || hdr.numents < 0 ||
                 hdr.numvars < 0 || hdr.numvslots < 0;
        if(failed) loaderror = 2;
    }

    loopi(failed ? 0 : hdr.numvars)
    {
        int type = f->getchar(), len = f->getlil<ushort>();
        if(type < 0 || !f->seek(len, SEEK_CUR)) { failed = true; loaderror = 3; break; }
        switch(type)
        {
            case ID_VAR:  failed = !f->seek(sizeof(int), SEEK_CUR); break;
            case ID_FVAR: failed = !f->seek(sizeof(float), SEEK_CUR); break;
            case ID_SVAR:
            {
                int slen = f->getlil<ushort>();
                failed = !f->seek(slen, SEEK_CUR);
                break;
            }
            default: failed = true; break;
        }
        if(failed) { loaderror = 3; break; }
    }

    if(!failed)
    {
        int gamelen = f->getchar();
        if(gamelen < 0 || !f->seek(gamelen + 1, SEEK_CUR)) { failed = true; loaderror = 4; }
    }
    if(!failed)
    {
        int eif = f->getlil<ushort>(), extrasize = f->getlil<ushort>();
        if(!f->seek(extrasize, SEEK_CUR)) failed = true;
        int nummru = failed ? 0 : f->getlil<ushort>();
        if(!failed && (!f->seek(nummru * sizeof(ushort), SEEK_CUR) ||
                       !f->seek(hdr.numents * (sizeof(entity) + eif), SEEK_CUR)))
            { failed = true; loaderror = 4; }
    }
    if(!failed && !skipworldchunkvslots(f, hdr.numvslots)) { failed = true; loaderror = 5; }

    cube *root = failed ? NULL : loadpreparedchildren(f, families, failed, loaderror);
    if(failed)
    {
        freepreparedworldchunk(root);
        families = 0;
        return NULL;
    }
    optimized = remipworldchunk(root, true, families);
    return root;
}

static cube *prepareworldchunk(worldchunkjob &job)
{
    if(job.filename[0])
    {
        cube *root = loadpreparedworldchunk(job.filename, job.families, job.optimized, job.loaderror);
        if(root)
        {
            job.loaded = true;
            return root;
        }
    }

    worldgencontext ctx(job.seed, job.grasstexture, job.grasssidetexture,
                        job.dirttexture, job.stonetexture, job.sandtexture, job.snowtexture,
                        job.woodtexture, job.leaftexture, true, job.terrain);
    cube *root = generateworldchunk(job.x, job.y, ctx);
    job.families = ctx.families;
    job.optimized = ctx.optimized;
    job.loaded = false;
    return root;
}

static bool loadworldchunks(const char *mname)
{
    string mapname;
    validmapname(mapname, mname, NULL, "");
    loopi(strlen(mapname)) if(mapname[i] == '\\') mapname[i] = '/';
    char *slash = strrchr(mapname, '/');
    int currentx, currenty;
    if(!slash || !chunkcoords(slash + 1, currentx, currenty)) return false;
    *slash = '\0';

    cube *currentroot = worldroot;
    worldroot = NULL;
    copystring(worldfolder, mapname);
    activeworldchunk = 0;
    worldchunks.add(worldchunk(currentx, currenty, currentroot, false, true));
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
    rebuildworldchunks(currentx, currenty, true, false);
    conoutf("loaded infinite world %s around chunk %d_%d", worldfolder, currentx, currenty);
    return true;
}

VAR(dbgvars, 0, 0, 1);

void savevslot(stream *f, VSlot &vs, int prev)
{
    f->putlil<int>(vs.changed);
    f->putlil<int>(prev);
    if(vs.changed & (1<<VSLOT_SHPARAM))
    {
        f->putlil<ushort>(vs.params.length());
        loopv(vs.params)
        {
            SlotShaderParam &p = vs.params[i];
            f->putlil<ushort>(strlen(p.name));
            f->write(p.name, strlen(p.name));
            loopk(4) f->putlil<float>(p.val[k]);
        }
    }
    if(vs.changed & (1<<VSLOT_SCALE)) f->putlil<float>(vs.scale);
    if(vs.changed & (1<<VSLOT_ROTATION)) f->putlil<int>(vs.rotation);
    if(vs.changed & (1<<VSLOT_OFFSET))
    {
        loopk(2) f->putlil<int>(vs.offset[k]);
    }
    if(vs.changed & (1<<VSLOT_SCROLL))
    {
        loopk(2) f->putlil<float>(vs.scroll[k]);
    }
    if(vs.changed & (1<<VSLOT_LAYER)) f->putlil<int>(vs.layer);
    if(vs.changed & (1<<VSLOT_ALPHA))
    {
        f->putlil<float>(vs.alphafront);
        f->putlil<float>(vs.alphaback);
    }
    if(vs.changed & (1<<VSLOT_COLOR))
    {
        loopk(3) f->putlil<float>(vs.colorscale[k]);
    }
    if(vs.changed & (1<<VSLOT_REFRACT))
    {
        f->putlil<float>(vs.refractscale);
        loopk(3) f->putlil<float>(vs.refractcolor[k]);
    }
    if(vs.changed & (1<<VSLOT_DETAIL)) f->putlil<int>(vs.detail);
}

void savevslots(stream *f, int numvslots)
{
    if(vslots.empty()) return;
    int *prev = new int[numvslots];
    memset(prev, -1, numvslots*sizeof(int));
    loopi(numvslots)
    {
        VSlot *vs = vslots[i];
        if(vs->changed) continue;
        for(;;)
        {
            VSlot *cur = vs;
            do vs = vs->next; while(vs && vs->index >= numvslots);
            if(!vs) break;
            prev[vs->index] = cur->index;
        }
    }
    int lastroot = 0;
    loopi(numvslots)
    {
        VSlot &vs = *vslots[i];
        if(!vs.changed) continue;
        if(lastroot < i) f->putlil<int>(-(i - lastroot));
        savevslot(f, vs, prev[i]);
        lastroot = i+1;
    }
    if(lastroot < numvslots) f->putlil<int>(-(numvslots - lastroot));
    delete[] prev;
}

void loadvslot(stream *f, VSlot &vs, int changed)
{
    vs.changed = changed;
    if(vs.changed & (1<<VSLOT_SHPARAM))
    {
        int numparams = f->getlil<ushort>();
        string name;
        loopi(numparams)
        {
            SlotShaderParam &p = vs.params.add();
            int nlen = f->getlil<ushort>();
            f->read(name, min(nlen, MAXSTRLEN-1));
            name[min(nlen, MAXSTRLEN-1)] = '\0';
            if(nlen >= MAXSTRLEN) f->seek(nlen - (MAXSTRLEN-1), SEEK_CUR);
            p.name = getshaderparamname(name);
            p.loc = -1;
            loopk(4) p.val[k] = f->getlil<float>();
        }
    }
    if(vs.changed & (1<<VSLOT_SCALE)) vs.scale = f->getlil<float>();
    if(vs.changed & (1<<VSLOT_ROTATION)) vs.rotation = clamp(f->getlil<int>(), 0, 7);
    if(vs.changed & (1<<VSLOT_OFFSET))
    {
        loopk(2) vs.offset[k] = f->getlil<int>();
    }
    if(vs.changed & (1<<VSLOT_SCROLL))
    {
        loopk(2) vs.scroll[k] = f->getlil<float>();
    }
    if(vs.changed & (1<<VSLOT_LAYER)) vs.layer = f->getlil<int>();
    if(vs.changed & (1<<VSLOT_ALPHA))
    {
        vs.alphafront = f->getlil<float>();
        vs.alphaback = f->getlil<float>();
    }
    if(vs.changed & (1<<VSLOT_COLOR))
    {
        loopk(3) vs.colorscale[k] = f->getlil<float>();
    }
    if(vs.changed & (1<<VSLOT_REFRACT))
    {
        vs.refractscale = f->getlil<float>();
        loopk(3) vs.refractcolor[k] = f->getlil<float>();
    }
    if(vs.changed & (1<<VSLOT_DETAIL)) vs.detail = f->getlil<int>();
}

void loadvslots(stream *f, int numvslots)
{
    int *prev = new (false) int[numvslots];
    if(!prev) return;
    memset(prev, -1, numvslots*sizeof(int));
    while(numvslots > 0)
    {
        int changed = f->getlil<int>();
        if(changed < 0)
        {
            loopi(-changed) vslots.add(new VSlot(NULL, vslots.length()));
            numvslots += changed;
        }
        else
        {
            prev[vslots.length()] = f->getlil<int>();
            loadvslot(f, *vslots.add(new VSlot(NULL, vslots.length())), changed);
            numvslots--;
        }
    }
    loopv(vslots) if(vslots.inrange(prev[i])) vslots[prev[i]]->next = vslots[i];
    delete[] prev;
}

bool save_world(const char *mname, bool nolms)
{
    if(!*mname) mname = game::getclientmap();
    setmapfilenames(*mname ? mname : "untitled");
    if(savebak) backup(ogzname, bakname);
    stream *f = opengzfile(ogzname, "wb");
    if(!f) { conoutf(CON_WARN, "could not write map to %s", ogzname); return false; }

    int numvslots = vslots.length();
    if(!nolms && !multiplayer(false))
    {
        numvslots = compactvslots();
        allchanged();
    }

    savemapprogress = 0;
    renderprogress(0, "saving map...");

    mapheader hdr;
    memcpy(hdr.magic, "TMAP", 4);
    hdr.version = MAPVERSION;
    hdr.headersize = sizeof(hdr);
    hdr.worldsize = worldsize;
    hdr.numents = 0;
    const vector<extentity *> &ents = entities::getents();
    loopv(ents) if(ents[i]->type!=ET_EMPTY || nolms) hdr.numents++;
    hdr.numpvs = nolms ? 0 : getnumviewcells();
    hdr.blendmap = shouldsaveblendmap();
    hdr.numvars = 0;
    hdr.numvslots = numvslots;
    enumerate(idents, ident, id,
    {
        if((id.type == ID_VAR || id.type == ID_FVAR || id.type == ID_SVAR) && id.flags&IDF_OVERRIDE && !(id.flags&IDF_READONLY) && id.flags&IDF_OVERRIDDEN) hdr.numvars++;
    });
    lilswap(&hdr.version, 8);
    f->write(&hdr, sizeof(hdr));

    enumerate(idents, ident, id,
    {
        if((id.type!=ID_VAR && id.type!=ID_FVAR && id.type!=ID_SVAR) || !(id.flags&IDF_OVERRIDE) || id.flags&IDF_READONLY || !(id.flags&IDF_OVERRIDDEN)) continue;
        f->putchar(id.type);
        f->putlil<ushort>(strlen(id.name));
        f->write(id.name, strlen(id.name));
        switch(id.type)
        {
            case ID_VAR:
                if(dbgvars) conoutf(CON_DEBUG, "wrote var %s: %d", id.name, *id.storage.i);
                f->putlil<int>(*id.storage.i);
                break;

            case ID_FVAR:
                if(dbgvars) conoutf(CON_DEBUG, "wrote fvar %s: %f", id.name, *id.storage.f);
                f->putlil<float>(*id.storage.f);
                break;

            case ID_SVAR:
                if(dbgvars) conoutf(CON_DEBUG, "wrote svar %s: %s", id.name, *id.storage.s);
                f->putlil<ushort>(strlen(*id.storage.s));
                f->write(*id.storage.s, strlen(*id.storage.s));
                break;
        }
    });

    if(dbgvars) conoutf(CON_DEBUG, "wrote %d vars", hdr.numvars);

    f->putchar((int)strlen(game::gameident()));
    f->write(game::gameident(), (int)strlen(game::gameident())+1);
    f->putlil<ushort>(entities::extraentinfosize());
    vector<char> extras;
    game::writegamedata(extras);
    f->putlil<ushort>(extras.length());
    f->write(extras.getbuf(), extras.length());

    f->putlil<ushort>(texmru.length());
    loopv(texmru) f->putlil<ushort>(texmru[i]);
    char *ebuf = new char[entities::extraentinfosize()];
    loopv(ents)
    {
        if(ents[i]->type!=ET_EMPTY || nolms)
        {
            entity tmp = *ents[i];
            lilswap(&tmp.o.x, 3);
            lilswap(&tmp.attr1, 5);
            f->write(&tmp, sizeof(entity));
            entities::writeent(*ents[i], ebuf);
            if(entities::extraentinfosize()) f->write(ebuf, entities::extraentinfosize());
        }
    }
    delete[] ebuf;

    savevslots(f, numvslots);

    renderprogress(0, "saving octree...");
    savec(worldroot, ivec(0, 0, 0), worldsize>>1, f, nolms);

    if(!nolms)
    {
        if(getnumviewcells()>0) { renderprogress(0, "saving pvs..."); savepvs(f); }
    }
    if(shouldsaveblendmap()) { renderprogress(0, "saving blendmap..."); saveblendmap(f); }

    delete f;
    conoutf("wrote map file %s", ogzname);
    return true;
}

static bool saveworldchunkconfig(const worldchunk &chunk)
{
    string chunkname;
    worldchunkname(chunkname, sizeof(chunkname), chunk);
    defformatstring(chunkcfg, "media/map/%s.cfg", chunkname);
    stream *f = openfile(path(chunkcfg), "w");
    if(!f)
    {
        conoutf(CON_WARN, "could not write chunk configuration to %s", chunkcfg);
        return false;
    }
    f->printf("exec \"media/map/%s/world.cfg\"\n", worldfolder);
    delete f;
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
        "worldloadseed %d\n"
        "worldminheight = %d\n"
        "worldmaxheight = %d\n"
        "worldinfinite = 1\n\n"
        "terrainload\n\n"
        "terraincontinentfreq %.9g\n"
        "terrainmountainfreq %.9g\n"
        "terrainerosionfreq %.9g\n"
        "terrainhillfreq %.9g\n"
        "terraindetailfreq %.9g\n"
        "terraincontinentwarpfreq %.9g\n"
        "terraincontinentwarpamp %.9g\n"
        "terrainfeaturewarpfreq %.9g\n"
        "terrainfeaturewarpamp %.9g\n"
        "terraintemperaturefreq %.9g\n"
        "terrainmoisturefreq %.9g\n"
        "terrainweirdnessfreq %.9g\n"
        "terrainweirdnessstrength %.9g\n"
        "terrainmountainstonefreq %.9g\n"
        "terrainsealevel %d\n"
        "terrainsnowheight %d\n"
        "terrainmountainstonelow %d\n"
        "terrainmountainstonehigh %d\n"
        "terrainbiomeblend %d\n"
        "terraincoastwidth %d\n"
        "terraincoastvariation %d\n"
        "terrainbeachminheight %d\n"
        "terrainbeachmaxheight %d\n"
        "terraincontinentheight %.9g\n"
        "terrainhillheight %.9g\n"
        "terrainmountainheight %.9g\n"
        "terrainerosionheight %.9g\n"
        "terraindetailheight %.9g\n"
        "terrainlandmasklow %.9g\n"
        "terrainlandmaskhigh %.9g\n"
        "terrainmountainmasklow %.9g\n"
        "terrainmountainmaskhigh %.9g\n"
        "terraindeserttemperature %.9g\n"
        "terraindesertmoisture %.9g\n"
        "terrainforestmoisture %.9g\n"
        "terrainforesttreedensity %.9g\n"
        "terrainplainstreedensity %.9g\n"
        "terrainpinestartheight %d\n"
        "terrainpinefullheight %d\n"
        "terraincavefreq %.9g\n"
        "terraincavethreshold %.9g\n"
        "terrainlargecavefreq %.9g\n"
        "terrainlargecavethreshold %.9g\n"
        "terrainlargecavedeepthreshold %.9g\n"
        "terraintunnelfreq %.9g\n"
        "terraintunnelwidth %.9g\n"
        "terraincaveentrancewidth %.9g\n"
        "terraincavemindepth %d\n"
        "terraincavefulldepth %d\n"
        "terraincavedeepheight %d\n"
        "terrainbottomlavalayers %d\n"
        "terrainlavalakestartheight %d\n"
        "terrainlavalakedeepheight %d\n"
        "terrainlavalakeshallowchance %.9g\n"
        "terrainlavalakedeepchance %.9g\n"
        "terrainlavalakeminsize %d\n"
        "terrainlavalakemaxsize %d\n"
        "terrainlavalakespacing %d\n"
        "terrainlavalakeshapefreq %.9g\n"
        "terrainlavalakeshapevariation %.9g\n",
        WORLD_GROUND_HEIGHT, WORLD_CHUNK_BLOCKS, WORLD_GRID_POWER, WORLD_BLOCK_SIZE, activeworldseed,
        WORLD_MIN_HEIGHT, WORLD_MAX_HEIGHT,
        terraincontinentfreq, terrainmountainfreq, terrainerosionfreq, terrainhillfreq, terraindetailfreq,
        terraincontinentwarpfreq, terraincontinentwarpamp, terrainfeaturewarpfreq, terrainfeaturewarpamp,
        terraintemperaturefreq, terrainmoisturefreq, terrainweirdnessfreq, terrainweirdnessstrength,
        terrainmountainstonefreq, terrainsealevel, terrainsnowheight,
        terrainmountainstonelow, terrainmountainstonehigh,
        terrainbiomeblend, terraincoastwidth, terraincoastvariation,
        terrainbeachminheight, terrainbeachmaxheight,
        terraincontinentheight, terrainhillheight, terrainmountainheight,
        terrainerosionheight, terraindetailheight, terrainlandmasklow, terrainlandmaskhigh,
        terrainmountainmasklow, terrainmountainmaskhigh, terraindeserttemperature,
        terraindesertmoisture, terrainforestmoisture,
        terrainforesttreedensity, terrainplainstreedensity,
        terrainpinestartheight, terrainpinefullheight,
        terraincavefreq, terraincavethreshold, terrainlargecavefreq,
        terrainlargecavethreshold, terrainlargecavedeepthreshold,
        terraintunnelfreq, terraintunnelwidth, terraincaveentrancewidth,
        terraincavemindepth, terraincavefulldepth, terraincavedeepheight,
        terrainbottomlavalayers, terrainlavalakestartheight, terrainlavalakedeepheight,
        terrainlavalakeshallowchance, terrainlavalakedeepchance,
        terrainlavalakeminsize, terrainlavalakemaxsize, terrainlavalakespacing,
        terrainlavalakeshapefreq, terrainlavalakeshapevariation
    );
    delete f;

    loopv(worldchunks) if((!worldchunks[i].saved || worldchunks[i].dirty) &&
                          !saveworldchunkconfig(worldchunks[i]))
        return false;

    return true;
}

static bool worldchunkfileexists(const char *folder, int x, int y)
{
    defformatstring(name, "media/map/%s/%d_%d.ogz", folder, x, y);
    stream *f = openfile(path(name), "rb");
    if(!f) return false;
    delete f;
    return true;
}

static bool saveworldmetadata(int chunkx, int chunky)
{
    defformatstring(name, "media/map/%s/world.meta", worldfolder);
    stream *f = openfile(path(name), "w");
    if(!f)
    {
        conoutf(CON_WARN, "could not write world metadata to %s", name);
        return false;
    }
    f->printf("CUBECRAFT_WORLD 1\n");
    f->printf("entry %d %d\n", chunkx, chunky);
    delete f;
    return true;
}

static bool loadworldmetadata(const char *folder, int &chunkx, int &chunky)
{
    chunkx = chunky = 0;
    defformatstring(name, "media/map/%s/world.meta", folder);
    stream *f = openfile(path(name), "r");
    if(f)
    {
        string line;
        while(f->getline(line, sizeof(line)))
        {
            int x, y;
            if(sscanf(line, "entry %d %d", &x, &y) == 2)
            {
                chunkx = x;
                chunky = y;
                break;
            }
        }
        delete f;
    }

    if(worldchunkfileexists(folder, chunkx, chunky)) return true;
    chunkx = chunky = 0;
    return worldchunkfileexists(folder, 0, 0);
}

static int loadingworldchunks()
{
    int loading = 0;
    loopv(worldchunks) if(worldchunks[i].loading) loading++;
    return loading;
}

static bool finishworldchunkloads()
{
    int remaining = loadingworldchunks(), total = remaining;
    while(remaining > 0)
    {
        if(worldchunkworkers.empty())
        {
            conoutf(CON_ERROR, "cannot finish %d queued chunks: the chunk worker pool is not running", remaining);
            return false;
        }

        int prepared = processworldchunkresults();
        remaining = loadingworldchunks();
        renderprogress(total > 0 ? (total - remaining) / float(total) : 1,
                       "finishing chunks before save...");
        if(!prepared && remaining > 0) SDL_Delay(1);
    }
    return true;
}

static void createworld(const char *requestedname)
{
    chooseworldfolder(requestedname);
    string chosenfolder, activechunkname;
    copystring(chosenfolder, worldfolder);
    formatstring(activechunkname, "%s/0_0", chosenfolder);

    if(!emptymap(WORLD_RUNTIME_SCALE, true, activechunkname)) return;
    copystring(worldfolder, chosenfolder);
    worldfirstchunkx = worldfirstchunky = -WORLD_RUNTIME_CENTER;
    if(!loadterrain()) return;
    loadworldseed(worldseed);

    freeocta(worldroot);
    worldroot = NULL;
    activeworldchunk = worldchunks.length();
    worldchunks.add(worldchunk(0, 0, generateworldchunk(0, 0)));
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
    updateworldchunks(true);
    if(player) entinmap(player);

    int mounted = 0;
    loopv(worldchunks) if(worldchunkmounted(worldchunks[i])) mounted++;
    conoutf("generated infinite world %s with %d initial chunks; %d chunks queued asynchronously",
            worldfolder, mounted, worldchunks.length() - mounted);
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
    if(!loadworldmetadata(folder, chunkx, chunky))
    {
        conoutf(CON_ERROR, "could not find a saved world named %s", folder);
        return;
    }

    defformatstring(entry, "%s/%d_%d", folder, chunkx, chunky);
    game::changemap(entry);
}

ICOMMAND(loadworld, "s", (char *name), loadworldcommand(name));

void saveworld()
{
    if(worldchunks.empty() || activeworldchunk < 0)
    {
        conoutf(CON_ERROR, "no procedural world is active; use newworld first");
        return;
    }

    if(!finishworldchunkloads()) return;
    if(!saveworldconfig()) return;

    syncmountedworldchunks();
    cube *runtimeroot = worldroot;
    const int runtimescale = worldscale, runtimesize = worldsize;
    setvar("mapscale", WORLD_CHUNK_SCALE, true, false);
    setvar("mapsize", WORLD_CHUNK_MAP_SIZE, true, false);
    int written = 0, unchanged = 0, failed = 0, ready = 0;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(!chunk.root || chunk.loading) continue;
        ready++;
        if(chunk.saved && !chunk.dirty)
        {
            unchanged++;
            continue;
        }

        worldroot = chunk.root;
        string name;
        worldchunkname(name, sizeof(name), chunk);
        if(save_world(name, true))
        {
            chunk.saved = true;
            chunk.dirty = false;
            written++;
        }
        else failed++;
    }
    worldroot = runtimeroot;
    setvar("mapscale", runtimescale, true, false);
    setvar("mapsize", runtimesize, true, false);

    if(failed)
    {
        conoutf(CON_ERROR, "failed to save %d of %d changed chunks for world %s; keeping all chunks in memory for retry",
                failed, written + failed, worldfolder);
        return;
    }

    int entryx = 0, entryy = 0, entry = findworldchunk(lastplayerchunkx, lastplayerchunky);
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
    conoutf("saved world %s: %d chunks written, %d unchanged, %d ready; released %d cached chunks",
            worldfolder, written, unchanged, ready, released);
}

COMMAND(saveworld, "");

static uint mapcrc = 0;

uint getmapcrc() { return mapcrc; }
void clearmapcrc() { mapcrc = 0; }

bool load_world(const char *mname, const char *cname)        // still supports all map formats that have existed since the earliest cube betas!
{
    int loadingstart = SDL_GetTicks();
    setmapfilenames(mname, cname);
    stream *f = opengzfile(ogzname, "rb");
    if(!f) { conoutf(CON_ERROR, "could not read map %s", ogzname); return false; }

    mapheader hdr;
    octaheader ohdr;
    memset(&ohdr, 0, sizeof(ohdr));
    if(!loadmapheader(f, ogzname, hdr, ohdr)) { delete f; return false; }

    clearworldchunks();
    resetmap();

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

    renderprogress(0, "loading vars...");

    loopi(hdr.numvars)
    {
        int type = f->getchar(), ilen = f->getlil<ushort>();
        string name;
        f->read(name, min(ilen, MAXSTRLEN-1));
        name[min(ilen, MAXSTRLEN-1)] = '\0';
        if(ilen >= MAXSTRLEN) f->seek(ilen - (MAXSTRLEN-1), SEEK_CUR);
        ident *id = getident(name);
        tagval val;
        string str;
        switch(type)
        {
            case ID_VAR: val.setint(f->getlil<int>()); break;
            case ID_FVAR: val.setfloat(f->getlil<float>()); break;
            case ID_SVAR:
            {
                int slen = f->getlil<ushort>();
                f->read(str, min(slen, MAXSTRLEN-1));
                str[min(slen, MAXSTRLEN-1)] = '\0';
                if(slen >= MAXSTRLEN) f->seek(slen - (MAXSTRLEN-1), SEEK_CUR);
                val.setstr(str);
                break;
            }
            default: continue;
        }
        if(id && id->flags&IDF_OVERRIDE) switch(id->type)
        {
            case ID_VAR:
            {
                int i = val.getint();
                if(id->minval <= id->maxval && i >= id->minval && i <= id->maxval)
                {
                    setvar(name, i);
                    if(dbgvars) conoutf(CON_DEBUG, "read var %s: %d", name, i);
                }
                break;
            }
            case ID_FVAR:
            {
                float f = val.getfloat();
                if(id->minvalf <= id->maxvalf && f >= id->minvalf && f <= id->maxvalf)
                {
                    setfvar(name, f);
                    if(dbgvars) conoutf(CON_DEBUG, "read fvar %s: %f", name, f);
                }
                break;
            }
            case ID_SVAR:
                setsvar(name, val.getstr());
                if(dbgvars) conoutf(CON_DEBUG, "read svar %s: %s", name, val.getstr());
                break;
        }
    }
    if(dbgvars) conoutf(CON_DEBUG, "read %d vars", hdr.numvars);

    string gametype;
    bool samegame = true;
    int len = f->getchar();
    if(len >= 0) f->read(gametype, len+1);
    gametype[max(len, 0)] = '\0';
    if(strcmp(gametype, game::gameident())!=0)
    {
        samegame = false;
        conoutf(CON_WARN, "WARNING: loading map from %s game, ignoring entities except for lights/mapmodels", gametype);
    }
    int eif = f->getlil<ushort>();
    int extrasize = f->getlil<ushort>();
    vector<char> extras;
    f->read(extras.pad(extrasize), extrasize);
    if(samegame) game::readgamedata(extras);

    texmru.shrink(0);
    ushort nummru = f->getlil<ushort>();
    loopi(nummru) texmru.add(f->getlil<ushort>());

    renderprogress(0, "loading entities...");

    vector<extentity *> &ents = entities::getents();
    int einfosize = entities::extraentinfosize();
    char *ebuf = einfosize > 0 ? new char[einfosize] : NULL;
    loopi(min(hdr.numents, MAXENTS))
    {
        extentity &e = *entities::newentity();
        ents.add(&e);
        f->read(&e, sizeof(entity));
        lilswap(&e.o.x, 3);
        lilswap(&e.attr1, 5);
        fixent(e, hdr.version);
        if(samegame)
        {
            if(einfosize > 0) f->read(ebuf, einfosize);
            entities::readent(e, ebuf, mapversion);
        }
        else
        {
            if(eif > 0) f->seek(eif, SEEK_CUR);
            if(e.type>=ET_GAMESPECIFIC)
            {
                entities::deleteentity(ents.pop());
                continue;
            }
        }
        if(!insideworld(e.o))
        {
            if(e.type != ET_LIGHT && e.type != ET_SPOTLIGHT)
            {
                conoutf(CON_WARN, "warning: ent outside of world: enttype[%s] index %d (%f, %f, %f)", entities::entname(e.type), i, e.o.x, e.o.y, e.o.z);
            }
        }
    }
    if(ebuf) delete[] ebuf;

    if(hdr.numents > MAXENTS)
    {
        conoutf(CON_WARN, "warning: map has %d entities", hdr.numents);
        f->seek((hdr.numents-MAXENTS)*(samegame ? sizeof(entity) + einfosize : eif), SEEK_CUR);
    }

    renderprogress(0, "loading slots...");
    loadvslots(f, hdr.numvslots);

    renderprogress(0, "loading octree...");
    bool failed = false;
    worldroot = loadchildren(f, ivec(0, 0, 0), hdr.worldsize>>1, failed);
    if(failed) conoutf(CON_ERROR, "garbage in map");

    renderprogress(0, "validating...");
    validatec(worldroot, hdr.worldsize>>1);

    if(!failed)
    {
        if(mapversion <= 0) loopi(ohdr.lightmaps)
        {
            int type = f->getchar();
            if(type&0x80)
            {
                f->getlil<ushort>();
                f->getlil<ushort>();
            }
            int bpp = 3;
            if(type&(1<<4) && (type&0x0F)!=2) bpp = 4;
            f->seek(bpp*LM_PACKW*LM_PACKH, SEEK_CUR);
        }

        if(hdr.numpvs > 0) loadpvs(f, hdr.numpvs);
        if(hdr.blendmap) loadblendmap(f, hdr.blendmap);
    }

    mapcrc = f->getcrc();
    delete f;

    conoutf("read map %s (%.1f seconds)", ogzname, (SDL_GetTicks()-loadingstart)/1000.0f);

    clearmainmenu();

    identflags |= IDF_OVERRIDDEN;
    execfile("config/default_map_settings.cfg", false);
    execfile(cfgname, false);
    identflags &= ~IDF_OVERRIDDEN;

    if(!cname && hdr.worldsize == WORLD_CHUNK_MAP_SIZE) loadworldchunks(mname);

    preloadusedmapmodels(true);

    game::preload();
    flushpreloadedmodels();

    preloadmapsounds();

    entitiesinoctanodes();
    attachentities();
    allchanged(true);

    renderbackground("loading...", mapshot, mname, game::getmapinfo());

    if(maptitle[0] && strcmp(maptitle, "Untitled Map by Unknown")) conoutf(CON_ECHO, "%s", maptitle);

    startmap(cname ? cname : mname);

    if(!worldchunks.empty() && player && worldchunks.inrange(activeworldchunk))
    {
        const worldchunk &chunk = worldchunks[activeworldchunk];
        player->o = vec((chunk.x - worldfirstchunkx) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        (chunk.y - worldfirstchunky) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        WORLD_GROUND_HEIGHT + player->eyeheight + 1);
        entinmap(player);
    }

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

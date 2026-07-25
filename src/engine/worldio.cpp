// worldio.cpp: loading & saving of maps and savegames

#include "FastNoiseLite.h"
#include "engine.h"
#include <errno.h>

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

FVAR(terraincavefreq, 0.0001f, 0.045f, 0.25f);
FVAR(terraincavethreshold, -1.0f, 0.58f, 1.0f);
FVAR(terrainlargecavefreq, 0.0001f, 0.018f, 0.25f);
FVAR(terrainlargecavethreshold, -1.0f, 0.76f, 1.0f);
FVAR(terrainlargecavedeepthreshold, -1.0f, 0.58f, 1.0f);
FVAR(terraintunnelfreq, 0.0001f, 0.025f, 0.25f);
FVAR(terraintunnelwidth, 0.001f, 0.075f, 0.3f);
FVAR(terraincaveentrancewidth, 0.001f, 0.05f, 0.3f);
VAR(terraincavemindepth, 1, 12, 64);
VAR(terraincavefulldepth, 1, 32, 128);
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
static void updateleavesalpha();
static void setworldleavesalpha(cube *root, bool enabled);
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
        const char *texture = escapestring(type.texture);
        string command;
        if(&type == leaves)
            formatstring(command, "setshader leafworld; texture 0 %s; texture a %s; texscale %.9g; texalpha 1 1",
                         texture, texture, type.texsize);
        else
            formatstring(command, "setshader stdworld; texture 0 %s; texscale %.9g",
                         texture, type.texsize);
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
    setworldleavesalpha(worldroot, leavesalpha != 0);
    conoutf(CON_DEBUG, "loaded %d terrain cube definitions", terraincubetypes.length());
    return true;
}

ICOMMAND(terrainload, "", (), intret(loadterrain() ? 1 : 0));

struct worldchunk
{
    int x, y;
    cube *root;
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
    int x, y, seed, grasstexture, grasssidetexture, dirttexture, stonetexture, sandtexture, snowtexture,
        woodtexture, leaftexture;
    terrainsettings terrain;
    int families, optimized, loaderror;
    uint epoch, request;
    bool loaded, remip;
    SDL_atomic_t cancelled;
    cube *root;
    string filename;

    worldchunkjob(int x, int y, uint epoch, uint request)
        : x(x), y(y), seed(activeworldseed),
          grasstexture(worldgrasstexture), grasssidetexture(worldgrasssidetexture),
          dirttexture(worlddirttexture), stonetexture(worldstonetexture),
          sandtexture(worldsandtexture), snowtexture(worldsnowtexture),
          woodtexture(worldwoodtexture), leaftexture(worldleaftexture),
          families(0), optimized(0), loaderror(0), epoch(epoch), request(request),
          loaded(false), remip(chunkremip != 0), root(NULL)
    {
        SDL_AtomicSet(&cancelled, 0);
        filename[0] = '\0';
    }
};

static vector<worldchunk> worldchunks;
static vector<worldchunkjob *> worldchunkjobs, worldchunkactivejobs, worldchunkresults;
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
static cube *loadworldchunkroot(const char *mname, int expectedx, int expectedy);
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
    ZoneScopedN("Chunks/Clear all chunks");
    shutdownworldchunkloader();
    worldchunkvaupdates.setsize(0);
    worldchunkvaupdateset.clear();
    worldsectionowners.clear();
    loopv(worldchunks) if(worldchunks[i].root && worldchunks[i].root != worldroot)
    {
        ZoneScopedN("Chunks/Free chunk during clear");
        ZoneTextF("%d_%d", worldchunks[i].x, worldchunks[i].y);
        freeocta(worldchunks[i].root);
    }
    worldchunks.setsize(0);
    worldfolder[0] = '\0';
    activeworldchunk = -1;
    worldfirstchunkx = worldfirstchunky = 0;
    lastplayerchunkx = lastplayerchunky = INT_MIN;
    lastchunkdist = -1;
    rebuildingworldchunks = false;
    lastworldchunkpublish = -1;
    lastworldchunkmotion = -1;
    worldchunkvelocityx = worldchunkvelocityy = 0;
    worldchunkfocusx = worldchunkfocusy = worldchunkaheadx = worldchunkaheady =
        worldchunkviewx = worldchunkviewy = 0;
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

static bool syncmountedworldchunks()
{
    if(worldchunks.empty() || !worldroot) return true;
    bool valid = true;
    loopv(worldchunks)
    {
        if(worldchunks[i].corrupted) valid = false;
        if(!worldchunks[i].saved || worldchunks[i].dirty)
            valid &= syncmountedworldchunk(worldchunks[i]);
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

static void queueworldchunksectionupdates(const worldchunk &chunk, int tile,
                                          const int *sections, int numsections)
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
    defformatstring(chunkname, "%s/%d_%d", worldfolder, x, y);
    // loadworldchunkroot() resolves the configured home and package paths.
    // A direct fileexists() on the relative media path misses saved chunks
    // when the game was launched with -u, causing them to be regenerated.
    cube *root = loadworldchunkroot(chunkname, x, y);
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
    defformatstring(chunkfile, "media/map/%s/%d_%d.ogz", worldfolder, x, y);
    path(chunkfile);
    const char *found = findfile(chunkfile, "rb");
    if(found && fileexists(found, "r"))
        copystring(job->filename, found);

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
            setworldleavesalpha(chunk.root, leavesalpha != 0);
            chunk.loading = false;
            chunk.saved = job->loaded;
            chunk.dirty = false;
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
        queued = queueworldchunkview(chunkx, chunky, aheadx, aheady);

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
            allchanged(true);
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
        return;

    int viewdist = maxchunkdist;
    bool rebase = localchunkx - viewdist <= 0 || localchunkx + viewdist >= WORLD_RUNTIME_CHUNKS - 1 ||
                  localchunky - viewdist <= 0 || localchunky + viewdist >= WORLD_RUNTIME_CHUNKS - 1;
    if(rebase)
    {
        rebaseworldchunks(chunkx, chunky);
        mountworldchunksafetyregion(chunkx, chunky);
    }
    rebuildworldchunks(chunkx, chunky, worldchunkaheadx, worldchunkaheady, force && !rebase, true);
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
    bool prepared, remip;
    int families, optimized;
    SDL_atomic_t *cancelled;

    worldgencontext(int seed, int grasstexture, int grasssidetexture, int dirttexture, int stonetexture,
                    int sandtexture, int snowtexture, int woodtexture, int leaftexture,
                    bool prepared, bool remip, const terrainsettings &terrain, SDL_atomic_t *cancelled = NULL)
        : terrain(terrain), seed(seed), grasstexture(grasstexture), grasssidetexture(grasssidetexture),
          dirttexture(dirttexture), stonetexture(stonetexture), sandtexture(sandtexture),
          snowtexture(snowtexture), woodtexture(woodtexture), leaftexture(leaftexture),
          prepared(prepared), remip(remip), families(0), optimized(0), cancelled(cancelled)
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

static int remipworldchunk(cube *root, bool prepared, int &families, SDL_atomic_t *cancelled = NULL)
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

static void setworldcubetexture(cube &c, int texture, int toptexture = -1, int material = MAT_AIR)
{
    solidfaces(c);
    c.material = material;
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
                ctx.heightmap[index] = generateworldterrainheight(ctx, chunkx, chunky, x, y);
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
                ctx.biomemap[index] = generateworldbiome(ctx, chunkx, chunky, x, y, ctx.heightmap[index]);
                ctx.rockmap[index] = generateworldrock(ctx, chunkx, chunky, x, y, ctx.heightmap[index]);
            }
        }
    }
    return !ctx.iscanceled();
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
            setworldcubetexture(c, ctx.grasssidetexture, ctx.grasstexture);
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

static bool generateworldcheesecaves(worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky)
{
    const int bottomlayers = clamp(ctx.terrain.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS)),
              minheight = WORLD_MIN_HEIGHT + bottomlayers,
              mindepth = min(ctx.terrain.cavemindepth, ctx.terrain.cavefulldepth),
              fulldepth = max(ctx.terrain.cavemindepth, ctx.terrain.cavefulldepth);
    const float deepdenominator = max(float(ctx.terrain.cavedeepheight - minheight), 1.0f);

    loop(y, WORLD_CHUNK_BLOCKS)
    {
        if(ctx.iscanceled()) return false;
        loop(x, WORLD_CHUNK_BLOCKS)
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
    return true;
}

static bool generateworldlavalakes(worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky)
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
        if(ctx.iscanceled()) return false;
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
        const int bottomlayers = clamp(ctx.terrain.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS));
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
              beachmin = (ctx.terrain.sealevel
                        + min(ctx.terrain.beachminheight, ctx.terrain.beachmaxheight)) * WORLD_BLOCK_SIZE,
              beachmax = (ctx.terrain.sealevel
                        + max(ctx.terrain.beachminheight, ctx.terrain.beachmaxheight)) * WORLD_BLOCK_SIZE;

    {
        ZoneScopedN("Chunks/Select tree blocks");
        for(int y = -halo; y < WORLD_CHUNK_BLOCKS + halo; ++y)
        for(int x = -halo; x < WORLD_CHUNK_BLOCKS + halo; ++x)
        {
            if(x == -halo && ctx.iscanceled()) return false;
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
    }

    {
        ZoneScopedN("Chunks/Apply tree blocks");
        ZoneValue(wood.length() + leaves.length());
        loopv(leaves)
        {
            cube &c = lookupworldgenblock(ctx, root, leaves[i]);
            if(isempty(c) && c.material == MAT_AIR)
                setworldcubetexture(c, ctx.leaftexture, -1, leavesalpha ? MAT_ALPHA : MAT_AIR);
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
    const terrainsettings terrain;
    worldgencontext ctx(activeworldseed, worldgrasstexture, worldgrasssidetexture,
                        worlddirttexture, worldstonetexture, worldsandtexture, worldsnowtexture,
                        worldwoodtexture, worldleaftexture, false, chunkremip != 0, terrain);
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

static cube *loadworldchunkroot(const char *mname, int expectedx, int expectedy)
{
    ZoneScopedN("Chunks/Load from disk");
    ZoneText(mname, strlen(mname));
    string name;
    validmapname(name, mname);
    defformatstring(filename, "media/map/%s.ogz", name);
    path(filename);
    stream *f;
    {
        ZoneScopedN("Chunks/Open chunk file");
        f = openrawfile(filename, "rb");
    }
    if(!f) return NULL;

    mapheader hdr;
    {
        ZoneScopedN("Chunks/Read chunk header");
        bool headerok = loadmapheader(f, filename, hdr);
        if(!headerok || hdr.worldsize != WORLD_CHUNK_MAP_SIZE ||
           hdr.chunkx != expectedx || hdr.chunky != expectedy)
        {
            if(headerok && (hdr.chunkx != expectedx || hdr.chunky != expectedy))
                conoutf(CON_ERROR, "chunk file %s identifies itself as %d_%d, expected %d_%d",
                        filename, hdr.chunkx, hdr.chunky, expectedx, expectedy);
            delete f;
            return NULL;
        }
    }

    bool failed = false;
    cube *root;
    {
        ZoneScopedN("Chunks/Decode octree synchronous");
        root = loadchildren(f, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE, failed);
    }
    {
        ZoneScopedN("Chunks/Close chunk file");
        delete f;
    }
    if(failed)
    {
        ZoneScopedN("Chunks/Free failed octree");
        freeocta(root);
        return NULL;
    }
    setworldleavesalpha(root, leavesalpha != 0);
    return root;
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
};

static cube *loadpreparedchildren(worldchunkreader &input, int &families, bool &failed, int &loaderror);

static void loadpreparedcube(worldchunkreader &input, cube &c,
                             int &families, bool &failed, int &loaderror)
{
    uchar octsav;
    if(!input.readbyte(octsav) || octsav & ~0x47) { failed = true; loaderror = 3; return; }
    switch(octsav & 0x7)
    {
        case OCTSAV_CHILDREN:
            if(octsav != OCTSAV_CHILDREN) { failed = true; loaderror = 3; return; }
            c.children = loadpreparedchildren(input, families, failed, loaderror);
            return;

        case OCTSAV_EMPTY:  emptyfaces(c);        break;
        case OCTSAV_SOLID:  solidfaces(c);        break;
        case OCTSAV_NORMAL:
            if(!input.read(c.edges, sizeof(c.edges))) { failed = true; loaderror = 3; return; }
            break;

        default:
            failed = true;
            loaderror = 3;
            return;
    }

    if((octsav & 0x7) != OCTSAV_EMPTY) loopi(6)
    {
        if(!input.readushort(c.texture[i])) { failed = true; loaderror = 3; return; }
    }
    if((octsav & 0x40) && !input.readushort(c.material))
    {
        failed = true;
        loaderror = 3;
    }
}

static cube *loadpreparedchildren(worldchunkreader &input, int &families, bool &failed, int &loaderror)
{
    cube *c = newpreparedfamily(families);
    loopi(8)
    {
        loadpreparedcube(input, c[i], families, failed, loaderror);
        if(failed) break;
    }
    return c;
}

static cube *loadpreparedworldchunk(const char *filename, int expectedx, int expectedy,
                                    bool remip, int &families, int &optimized, int &loaderror)
{
    ZoneScopedN("Chunks/Load prepared from disk");
    ZoneText(filename, strlen(filename));
    vector<uchar> contents;
    {
        ZoneScopedN("Chunks/Read raw chunk bytes");
        stream *file = openrawfile(filename, "rb");
        if(!file) { loaderror = 1; return NULL; }
        stream::offset length = file->size();
        if(length < stream::offset(sizeof(mapheader)) || length > INT_MAX)
        {
            delete file;
            loaderror = 1;
            return NULL;
        }
        uchar *dst = contents.pad(int(length));
        if(file->read(dst, size_t(length)) != size_t(length))
        {
            delete file;
            loaderror = 1;
            return NULL;
        }
        delete file;
        ZoneValue(length);
    }
    worldchunkmemorystream input(contents.getbuf(), contents.length());
    stream *f = &input;
    mapheader hdr;
    bool failed;
    {
        ZoneScopedN("Chunks/Read chunk header");
        bool headerok = loadmapheader(f, filename, hdr);
        failed = !headerok || hdr.worldsize != WORLD_CHUNK_MAP_SIZE;
        if(headerok && (hdr.chunkx != expectedx || hdr.chunky != expectedy))
        {
            conoutf(CON_ERROR, "chunk file %s identifies itself as %d_%d, expected %d_%d",
                    filename, hdr.chunkx, hdr.chunky, expectedx, expectedy);
            failed = true;
            loaderror = 4;
        }
        else if(failed) loaderror = 2;
    }
    worldchunkreader reader(contents.getbuf() + input.tell(), contents.length() - input.tell());
    cube *root = NULL;
    if(!failed)
    {
        ZoneScopedN("Chunks/Decode prepared octree");
        root = loadpreparedchildren(reader, families, failed, loaderror);
        ZoneValue(families);
    }
    if(failed)
    {
        {
            ZoneScopedN("Chunks/Free failed octree");
            freepreparedworldchunk(root);
        }
        families = 0;
        return NULL;
    }
    setworldleavesalpha(root, leavesalpha != 0);
    if(remip)
    {
        ZoneScopedN("Chunks/Remip loaded octree");
        optimized = remipworldchunk(root, true, families);
        ZoneValue(optimized);
    }
    else optimized = 0;
    return root;
}

static cube *prepareworldchunk(worldchunkjob &job)
{
    ZoneScopedN("Chunks/Prepare");
    ZoneTextF("%d_%d", job.x, job.y);
    if(job.filename[0])
    {
        ZoneScopedN("Chunks/Prepare disk chunk");
        cube *root = loadpreparedworldchunk(job.filename, job.x, job.y, job.remip, job.families,
                                            job.optimized, job.loaderror);
        if(root)
        {
            job.loaded = true;
            return root;
        }
    }

    if(SDL_AtomicGet(&job.cancelled)) return NULL;
    {
        ZoneScopedN("Chunks/Prepare generated chunk");
        worldgencontext ctx(job.seed, job.grasstexture, job.grasssidetexture,
                            job.dirttexture, job.stonetexture, job.sandtexture, job.snowtexture,
                            job.woodtexture, job.leaftexture, true, job.remip,
                            job.terrain, &job.cancelled);
        cube *root = generateworldchunk(job.x, job.y, ctx);
        job.families = ctx.families;
        job.optimized = ctx.optimized;
        job.loaded = false;
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
    applyloadworlddefaults = true;
    game::changemap(entry);
    applyloadworlddefaults = false;
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

    if(!syncmountedworldchunks())
    {
        conoutf(CON_ERROR, "refusing to save world %s: runtime chunk ownership is inconsistent",
                worldfolder);
        return;
    }
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
        if(save_world(name))
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
    if(!f) { conoutf(CON_ERROR, "could not read map %s", ogzname); return false; }

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

    if(!cname && hdr.worldsize == WORLD_CHUNK_MAP_SIZE) loadworldchunks(mname);

    {
        ZoneScopedN("Chunks/Build entry geometry");
        allchanged(true);
    }

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

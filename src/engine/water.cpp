#include "engine.h"

enum
{
    WATER_BLOCK_SIZE = 16,
    WATER_MAX_LEVEL = 7,
    WATER_DROP_SEARCH = 4,
    WATER_SOURCE_NONE = 0,
    WATER_SOURCE_MANUAL,
    WATER_SOURCE_NATURAL_ACTIVE
};

struct fluidcell
{
    uchar level;
    uchar sourcekind;
    bool falling, queued;
    int update;
    ivec origin;

    fluidcell() : level(0), sourcekind(WATER_SOURCE_NONE), falling(false), queued(false), update(0), origin(0, 0, 0) {}
    fluidcell(int level, int sourcekind, bool falling, const ivec &origin)
        : level(uchar(level)), sourcekind(uchar(sourcekind)), falling(falling), queued(false), update(0), origin(origin) {}

    bool source() const { return sourcekind != WATER_SOURCE_NONE; }
};

static hashtable<ivec, fluidcell> fluidcells(1<<14);
static vector<ivec> fluidupdates;
static int fluidupdatecursor = 0;
static bool changingwatermaterial = false;
VARP(fluidupdatespertick, 1, 1024, 16384);
VARP(simulationmaxdist, 1, 128, 1024);
FVARP(waterflowspeed, 0.1f, 4.0f, 20.0f);
static bool authoritativewatersettings = false;
static int authoritativewaterupdates = 1024, authoritativewaterdistance = 128, authoritativewaterspeed = 4000;

static void schedulewater(const ivec &position, int delay = -1);

static int watersourcematerial(int sourcekind)
{
    switch(sourcekind)
    {
        case WATER_SOURCE_MANUAL: return MAT_WATER | MAT_WATER_SOURCE_MANUAL;
        case WATER_SOURCE_NATURAL_ACTIVE: return MAT_WATER | MAT_WATER_SOURCE_NATURAL_ACTIVE;
        default: return MAT_WATER;
    }
}

static int watermaterialsource(int material)
{
    if((material&MATF_VOLUME) != MAT_WATER) return WATER_SOURCE_NONE;
    if(material&MAT_WATER_SOURCE_MANUAL) return WATER_SOURCE_MANUAL;
    if(material&MAT_WATER_SOURCE_NATURAL_ACTIVE) return WATER_SOURCE_NATURAL_ACTIVE;
    return WATER_SOURCE_NONE;
}

static bool waterpositionless(const ivec &a, const ivec &b)
{
    return a.x < b.x || (a.x == b.x && (a.y < b.y || (a.y == b.y && a.z < b.z)));
}

void resetwatersimulation()
{
    fluidcells.clear();
    fluidupdates.setsize(0);
    fluidupdatecursor = 0;
}

void setwatersimulationsettings(int updates, int distance, int speed)
{
    authoritativewaterupdates = clamp(updates, 1, 16384);
    authoritativewaterdistance = clamp(distance, 1, 1024);
    authoritativewaterspeed = clamp(speed, 100, 20000);
    authoritativewatersettings = true;
}

void resetwatersimulationsettings()
{
    authoritativewatersettings = false;
}

static void waterselection(selinfo &sel, const ivec &absolute)
{
    sel.o = absolute;
    sel.s = ivec(1, 1, 1);
    sel.grid = WATER_BLOCK_SIZE;
    sel.orient = WORLD_ORIENT_TOP;
    sel.cx = sel.cy = sel.corner = 0;
    sel.cxs = sel.cys = 2;
    worldselectiontolocal(sel);
}

static bool watermaterial(const ivec &absolute)
{
    selinfo sel;
    waterselection(sel, absolute);
    if(!sel.validate() || !worldselectionready(sel)) return false;
    return worldcellacceptswater(sel.o) && worldcellhaswater(sel.o);
}

int getwatercelllevel(const ivec &position, bool &falling)
{
    const ivec local = ivec(position).mask(~(WATER_BLOCK_SIZE - 1));
    selinfo absolute;
    absolute.o = local;
    worldselectiontoabsolute(absolute);
    fluidcell *cell = fluidcells.access(absolute.o);
    if(!cell)
    {
        const int sourcekind = watermaterialsource(worldcellmaterial(local));
        if(sourcekind == WATER_SOURCE_NONE)
        {
            falling = false;
            return -1;
        }
        cell = &fluidcells.access(absolute.o, fluidcell(0, sourcekind, false, absolute.o));
        schedulewater(absolute.o);
    }
    falling = cell->falling;
    return cell->source() ? 0 : cell->level;
}

void watermaterialloaded(const ivec &position, int material)
{
    const int sourcekind = watermaterialsource(material);
    if(sourcekind == WATER_SOURCE_NONE) return;
    selinfo absolute;
    absolute.o = ivec(position).mask(~(WATER_BLOCK_SIZE - 1));
    worldselectiontoabsolute(absolute);
    fluidcell *cell = fluidcells.access(absolute.o);
    if(!cell)
        cell = &fluidcells.access(absolute.o, fluidcell(0, sourcekind, false, absolute.o));
    else
    {
        cell->level = 0;
        cell->sourcekind = uchar(sourcekind);
        cell->falling = false;
        cell->origin = absolute.o;
    }
    schedulewater(absolute.o);
}

void getflowingwatercells(vector<ivec> &cells)
{
    enumeratekt(fluidcells, ivec, position, fluidcell, cell,
    {
        if(!cell.source()) cells.add(position);
    });
}

static bool wateraccepts(const ivec &absolute)
{
    selinfo sel;
    waterselection(sel, absolute);
    if(!sel.validate() || !worldselectionready(sel)) return false;
    return worldcellacceptswater(sel.o);
}

static bool setwatermaterial(const ivec &absolute, bool water, bool persist = true, int sourcekind = WATER_SOURCE_NONE)
{
    selinfo sel;
    waterselection(sel, absolute);
    if(!sel.validate() || !worldselectionready(sel)) return false;
    const int existingmaterial = worldcellmaterial(sel.o);
    if(water)
    {
        if(!worldcellacceptswater(sel.o)) return false;
        const int material = watersourcematerial(sourcekind);
        if(existingmaterial == material) return true;
        changingwatermaterial = true;
        mpeditmat(material, (existingmaterial&MATF_VOLUME) == MAT_WATER ? existingmaterial : -1, sel, false, persist);
        changingwatermaterial = false;
    }
    else
    {
        if((existingmaterial&MATF_VOLUME) != MAT_WATER) return true;
        changingwatermaterial = true;
        mpeditmat(MAT_AIR, existingmaterial, sel, false, persist);
        changingwatermaterial = false;
    }
    return true;
}

static int effectivewaterspeed()
{
    return authoritativewatersettings ? authoritativewaterspeed : clamp(int(waterflowspeed * 1000.0f + 0.5f), 100, 20000);
}

static int waterstepmillis()
{
    return max(1000000 / effectivewaterspeed(), 1);
}

static void schedulewater(const ivec &position, int delay)
{
    fluidcell *cell = fluidcells.access(position);
    if(!cell) return;
    const int due = totalmillis + (delay >= 0 ? delay : waterstepmillis());
    if(cell->queued)
    {
        cell->update = min(cell->update, due);
        return;
    }
    cell->queued = true;
    cell->update = due;
    fluidupdates.add(position);
}

static bool addwatercell(const ivec &position, int level, int sourcekind, bool falling, int delay = -1, bool refresh = true,
                         const ivec *floworigin = NULL)
{
    const ivec origin = sourcekind != WATER_SOURCE_NONE || !floworigin ? position : *floworigin;
    fluidcell *existing = fluidcells.access(position);
    if(existing)
    {
        bool changed = sourcekind != WATER_SOURCE_NONE && !existing->source();
        if(sourcekind != WATER_SOURCE_NONE)
        {
            existing->sourcekind = uchar(sourcekind);
            existing->falling = false;
            existing->level = 0;
            existing->origin = position;
        }
        else if(!existing->source() &&
                (level < existing->level ||
                 (level == existing->level && (falling < existing->falling ||
                  (falling == existing->falling && waterpositionless(origin, existing->origin))))))
        {
            existing->level = uchar(min(level, int(WATER_MAX_LEVEL)));
            existing->falling = falling;
            existing->origin = origin;
            changed = true;
        }
        if(changed) schedulewater(position, delay);
        return changed;
    }
    if(!wateraccepts(position)) return false;
    const bool materialexists = watermaterial(position);
    if(materialexists && sourcekind == WATER_SOURCE_NONE) return false;
    fluidcells.access(position, fluidcell(min(level, int(WATER_MAX_LEVEL)), sourcekind, falling, origin));
    if((!materialexists || sourcekind != WATER_SOURCE_NONE) &&
       !setwatermaterial(position, true, sourcekind != WATER_SOURCE_NONE, sourcekind))
    {
        fluidcells.remove(position);
        return false;
    }
    if(materialexists && refresh) worldwaterchanged();
    schedulewater(position, delay);
    return true;
}

bool addmanualwatersource(const ivec &position)
{
    fluidcell *existing = fluidcells.access(position);
    if(existing && existing->source()) return true;
    return addwatercell(position, 0, WATER_SOURCE_MANUAL, false);
}

static void removewatercell(const ivec &position);

bool removewatersource(const ivec &position)
{
    fluidcell *cell = fluidcells.access(position);
    if(!cell || !cell->source()) return false;
    removewatercell(position);
    return true;
}

static void schedulewaterneighbors(const ivec &position, int delay = -1)
{
    static const ivec offsets[] =
    {
        ivec(-WATER_BLOCK_SIZE, 0, 0), ivec(WATER_BLOCK_SIZE, 0, 0),
        ivec(0, -WATER_BLOCK_SIZE, 0), ivec(0, WATER_BLOCK_SIZE, 0),
        ivec(0, 0, -WATER_BLOCK_SIZE), ivec(0, 0, WATER_BLOCK_SIZE)
    };
    loopi(6) schedulewater(ivec(position).add(offsets[i]), delay);
}

static void removewatercell(const ivec &position)
{
    fluidcell *cell = fluidcells.access(position);
    if(!cell) return;
    const bool persist = cell->source();
    fluidcells.remove(position);
    setwatermaterial(position, false, persist);
    schedulewaterneighbors(position);
}

static void activatenaturalwaterneighbors(const ivec &position)
{
    static const ivec offsets[] =
    {
        ivec(-WATER_BLOCK_SIZE, 0, 0), ivec(WATER_BLOCK_SIZE, 0, 0),
        ivec(0, -WATER_BLOCK_SIZE, 0), ivec(0, WATER_BLOCK_SIZE, 0),
        ivec(0, 0, -WATER_BLOCK_SIZE), ivec(0, 0, WATER_BLOCK_SIZE)
    };
    loopi(6)
    {
        const ivec neighbor = ivec(position).add(offsets[i]);
        if(!fluidcells.access(neighbor) && watermaterial(neighbor))
            addwatercell(neighbor, 0, WATER_SOURCE_NATURAL_ACTIVE, false, -1, false);
    }
}

void watergeometryopening(const selinfo &selection)
{
    selinfo absolute = selection;
    worldselectiontoabsolute(absolute);
    const ivec end = ivec(absolute.o).add(ivec(absolute.s).mul(absolute.grid));
    const ivec first = ivec(absolute.o).mask(~(WATER_BLOCK_SIZE - 1));
    const ivec last = ivec(end).sub(1).mask(~(WATER_BLOCK_SIZE - 1));
    for(int z = first.z; z <= last.z; z += WATER_BLOCK_SIZE)
    for(int y = first.y; y <= last.y; y += WATER_BLOCK_SIZE)
    for(int x = first.x; x <= last.x; x += WATER_BLOCK_SIZE)
        activatenaturalwaterneighbors(ivec(x, y, z));
}

void waterterrainchanged(const ivec &position)
{
    static const ivec offsets[] =
    {
        ivec(0, 0, 0), ivec(-WATER_BLOCK_SIZE, 0, 0), ivec(WATER_BLOCK_SIZE, 0, 0),
        ivec(0, -WATER_BLOCK_SIZE, 0), ivec(0, WATER_BLOCK_SIZE, 0),
        ivec(0, 0, -WATER_BLOCK_SIZE), ivec(0, 0, WATER_BLOCK_SIZE)
    };
    loopi(7)
    {
        const ivec neighbor = ivec(position).add(offsets[i]);
        fluidcell *cell = fluidcells.access(neighbor);
        if(neighbor == position && (!watermaterial(neighbor) || !wateraccepts(neighbor)))
        {
            if(cell) removewatercell(neighbor);
            else if(!wateraccepts(neighbor)) setwatermaterial(neighbor, false);
            continue;
        }
        if(cell) schedulewater(neighbor);
    }
}

void watermaterialchanged(const selinfo &selection, int material)
{
    if(changingwatermaterial) return;
    selinfo absolute = selection;
    worldselectiontoabsolute(absolute);
    const ivec end = ivec(absolute.o).add(ivec(absolute.s).mul(absolute.grid));
    bool activated = false;
    for(int z = absolute.o.z; z < end.z; z += WATER_BLOCK_SIZE)
    for(int y = absolute.o.y; y < end.y; y += WATER_BLOCK_SIZE)
    for(int x = absolute.o.x; x < end.x; x += WATER_BLOCK_SIZE)
    {
        const ivec position = ivec(x, y, z).mask(~(WATER_BLOCK_SIZE - 1));
        if(material >= 0 && (material&MATF_VOLUME) == MAT_WATER)
            activated |= addwatercell(position, 0, WATER_SOURCE_MANUAL, false, 0, false);
        else waterterrainchanged(position);
    }
    if(activated) worldwaterchanged();
}

static bool watercanflowinto(const ivec &position)
{
    if(!wateraccepts(position)) return false;
    if(fluidcells.access(position)) return true;
    return !watermaterial(position);
}

static int waterlevel(const ivec &position)
{
    fluidcell *cell = fluidcells.access(position);
    if(cell) return cell->source() ? 0 : cell->level;
    return watermaterial(position) ? 0 : WATER_MAX_LEVEL + 1;
}

static bool waterflow(const ivec &position, vec &flow)
{
    const int west = waterlevel(ivec(position).add(ivec(-WATER_BLOCK_SIZE, 0, 0))),
              east = waterlevel(ivec(position).add(ivec(WATER_BLOCK_SIZE, 0, 0))),
              south = waterlevel(ivec(position).add(ivec(0, -WATER_BLOCK_SIZE, 0))),
              north = waterlevel(ivec(position).add(ivec(0, WATER_BLOCK_SIZE, 0)));
    fluidcell *current = fluidcells.access(position);
    if(current && current->falling)
    {
        flow = vec(0, 0, -1);
        return true;
    }
    flow = vec(float(east - west), float(north - south), 0);
    if(flow.iszero()) return false;
    flow.normalize();
    return true;
}

static bool waterflowforplayer(const physent *pl, vec &flow, float &immersion)
{
    const float feet = pl->o.z - pl->eyeheight,
                head = pl->o.z + pl->aboveeye,
                height = max(head - feet, 1.0f);
    const int firstcell = int(floorf(feet / float(WATER_BLOCK_SIZE))) * WATER_BLOCK_SIZE,
              lastcell = int(ceilf(head / float(WATER_BLOCK_SIZE))) * WATER_BLOCK_SIZE;
    float waterdepth = 0.0f;
    vec weightedflow(0, 0, 0);

    for(int z = firstcell; z < lastcell; z += WATER_BLOCK_SIZE)
    {
        const vec sample(pl->o.x, pl->o.y, z + 1.0f);
        bool falling = false;
        const int level = getwatercelllevel(ivec(sample), falling);
        if(level < 0) continue;

        const int levelclamped = min(level, int(WATER_MAX_LEVEL));
        const float celltop = z + (falling ? WATER_BLOCK_SIZE : WATER_BLOCK_SIZE - levelclamped * 2.0f - WATER_OFFSET),
                    waterbottom = max(feet, float(z)),
                    watertop = min(head, celltop),
                    depth = max(watertop - waterbottom, 0.0f);
        if(depth <= 0.0f) continue;

        waterdepth += depth;
        vec cellflow;
        vec absolute(pl->o.x, pl->o.y, z);
        worldpositiontoabsolute(absolute);
        if(waterflow(ivec(absolute).mask(~(WATER_BLOCK_SIZE - 1)), cellflow)) weightedflow.add(vec(cellflow).mul(depth));
    }

    // A player is about two cubes tall, so 24 units of water coverage is the
    // threshold at which the current should overpower player movement.
    immersion = waterdepth >= height ? 1.0f : clamp(waterdepth / (2.0f * WATER_BLOCK_SIZE), 0.0f, 1.0f);
    if(waterdepth <= 0.0f || weightedflow.iszero()) return false;
    flow = weightedflow.div(waterdepth);
    return !flow.iszero();
}

float watermovementscale(const physent *pl)
{
    if(!pl || pl->type != ENT_PLAYER || pl->state == CS_EDITING || pl->state == CS_SPECTATOR) return 1.0f;

    vec flow;
    float immersion;
    waterflowforplayer(pl, flow, immersion);

    // Water resistance starts quickly and then tapers off logarithmically,
    // leaving only 5% of normal player control at full immersion.
    const float resistance = logf(1.0f + 7.0f * immersion) / logf(8.0f);
    return 1.0f - 0.95f * resistance;
}

void applywaterflow(physent *pl, bool water, int timestep)
{
    if(!pl || pl->type != ENT_PLAYER || pl->state == CS_EDITING || pl->state == CS_SPECTATOR || timestep <= 0) return;

    vec flow;
    float immersion;
    if(!waterflowforplayer(pl, flow, immersion)) return;

    // Calibrate the impulse against the player-control blend. This makes the
    // 75% immersion point meaningful regardless of the physics timestep or
    // whether the player is just entering the water.
    const float control = 1.0f - pow(1.0f - 1.0f / (water ? 20.0f : 30.0f), timestep / 20.0f),
                threshold = 0.75f,
                belowthreshold = immersion / threshold,
                abovethreshold = (immersion - threshold) / (1.0f - threshold),
                strength = immersion < threshold ?
                           0.95f * logf(1.0f + 9.0f * belowthreshold) / logf(10.0f) :
                           1.05f + 0.95f * logf(1.0f + abovethreshold) / logf(2.0f),
                impulse = pl->maxspeed * control * strength * clamp(flow.magnitude(), 0.0f, 1.0f);
    pl->vel.add(flow.mul(impulse));
}

static bool watersupported(const ivec &position)
{
    ivec below = position;
    below.z -= WATER_BLOCK_SIZE;
    if(below.z < 0) return true;
    selinfo sel;
    waterselection(sel, below);
    if(!sel.validate() || !worldselectionready(sel)) return false;
    return worldcellsolid(sel.o);
}

static int waterdropcost(const ivec &position, const ivec &direction)
{
    ivec cursor = position;
    loopi(WATER_DROP_SEARCH)
    {
        cursor.add(direction);
        if(!watercanflowinto(cursor)) return WATER_DROP_SEARCH + 1;
        ivec below = cursor;
        below.z -= WATER_BLOCK_SIZE;
        if(below.z >= 0 && watercanflowinto(below)) return i;
    }
    return WATER_DROP_SEARCH + 1;
}

static void updatewatercell(const ivec &position)
{
    fluidcell *cell = fluidcells.access(position);
    if(!cell) return;
    if(!watermaterial(position))
    {
        removewatercell(position);
        return;
    }

    static const ivec directions[] =
    {
        ivec(-WATER_BLOCK_SIZE, 0, 0), ivec(WATER_BLOCK_SIZE, 0, 0),
        ivec(0, -WATER_BLOCK_SIZE, 0), ivec(0, WATER_BLOCK_SIZE, 0)
    };
    if(!cell->source())
    {
        int desiredlevel = WATER_MAX_LEVEL + 1;
        bool desiredfalling = false;
        ivec desiredorigin = cell->origin;
        ivec above = position;
        above.z += WATER_BLOCK_SIZE;
        fluidcell *abovefluid = fluidcells.access(above);
        if(abovefluid)
        {
            desiredlevel = abovefluid->source() ? 0 : abovefluid->level;
            desiredfalling = true;
            desiredorigin = abovefluid->origin;
        }
        loopi(4)
        {
            const ivec neighborposition = ivec(position).add(directions[i]);
            fluidcell *neighbor = fluidcells.access(neighborposition);
            // Falling water only feeds sideways from the cell where the waterfall lands.
            if(!neighbor || (neighbor->falling && !watersupported(neighborposition))) continue;
            const int neighborlevel = neighbor->source() ? 0 : neighbor->level;
            const int candidatelevel = neighborlevel + 1;
            if(candidatelevel < desiredlevel ||
               (candidatelevel == desiredlevel && (desiredfalling || waterpositionless(neighbor->origin, desiredorigin))))
            {
                desiredlevel = candidatelevel;
                desiredfalling = false;
                desiredorigin = neighbor->origin;
            }
        }
        if(desiredlevel > WATER_MAX_LEVEL)
        {
            removewatercell(position);
            return;
        }
        if(cell->level != desiredlevel || cell->falling != desiredfalling || cell->origin != desiredorigin)
        {
            cell->level = uchar(desiredlevel);
            cell->falling = desiredfalling;
            cell->origin = desiredorigin;
            schedulewaterneighbors(position);
        }
    }

    ivec below = position;
    below.z -= WATER_BLOCK_SIZE;
    if(below.z >= 0 && watercanflowinto(below))
    {
        const ivec origin = cell->origin;
        addwatercell(below, cell->level, WATER_SOURCE_NONE, true, -1, true, &origin);
        return;
    }

    if(!cell->source() && watersupported(position))
    {
        int sources = 0;
        loopi(4)
        {
            fluidcell *neighbor = fluidcells.access(ivec(position).add(directions[i]));
            if(neighbor && neighbor->source()) ++sources;
        }
        if(sources >= 2)
        {
            cell->sourcekind = WATER_SOURCE_NATURAL_ACTIVE;
            cell->falling = false;
            cell->level = 0;
            cell->origin = position;
            setwatermaterial(position, true, true, WATER_SOURCE_NATURAL_ACTIVE);
        }
    }

    const int nextlevel = cell->source() ? 1 : cell->falling ? 1 : int(cell->level) + 1;
    if(nextlevel > WATER_MAX_LEVEL) return;
    const ivec floworigin = cell->origin;
    int costs[4], best = WATER_DROP_SEARCH + 1;
    loopi(4)
    {
        const ivec neighbor = ivec(position).add(directions[i]);
        costs[i] = watercanflowinto(neighbor) ? waterdropcost(position, directions[i]) : WATER_DROP_SEARCH + 1;
        best = min(best, costs[i]);
    }
    loopi(4)
    {
        const ivec neighbor = ivec(position).add(directions[i]);
        if(!watercanflowinto(neighbor) || (best <= WATER_DROP_SEARCH && costs[i] != best)) continue;
        addwatercell(neighbor, nextlevel, WATER_SOURCE_NONE, false, -1, true, &floworigin);
    }
}

static bool waterinsimulationrange(const ivec &position)
{
    if(!camera1 && !player) return false;
    vec focus = camera1 ? camera1->o : player->o;
    worldpositiontoabsolute(focus);
    const int distance = authoritativewatersettings ? authoritativewaterdistance : simulationmaxdist;
    const float range = distance * float(WATER_BLOCK_SIZE);
    return vec(position).add(WATER_BLOCK_SIZE * 0.5f).squaredist(focus) <= range * range;
}

void updatewatersimulation()
{
    const int updatebudget = authoritativewatersettings ? authoritativewaterupdates : fluidupdatespertick;
    int processed = 0, inspected = 0;
    const int inspectionlimit = min(fluidupdates.length(), max(updatebudget * 4, 256));
    while(fluidupdates.length() && processed < updatebudget && inspected < inspectionlimit)
    {
        if(fluidupdatecursor >= fluidupdates.length()) fluidupdatecursor = 0;
        const ivec position = fluidupdates[fluidupdatecursor];
        fluidcell *cell = fluidcells.access(position);
        if(!cell)
        {
            fluidupdates.removeunordered(fluidupdatecursor);
            continue;
        }
        if(cell->update > totalmillis || !waterinsimulationrange(position))
        {
            ++fluidupdatecursor;
            ++inspected;
            continue;
        }
        cell->queued = false;
        fluidupdates.removeunordered(fluidupdatecursor);
        updatewatercell(position);
        ++processed;
        ++inspected;
    }

}

#define NUMCAUSTICS 32

static Texture *caustictex[NUMCAUSTICS] = { NULL };

void loadcaustics(bool force)
{
    static bool needcaustics = false;
    if(force) needcaustics = true;
    if(!caustics || !needcaustics) return;
    useshaderbyname("caustics");
    if(caustictex[0]) return;
    loopi(NUMCAUSTICS)
    {
        defformatstring(name, "<grey><noswizzle>media/texture/mat_water/caustic/caust%.2d.png", i);
        caustictex[i] = textureload(name);
    }
}

void cleanupcaustics()
{
    loopi(NUMCAUSTICS) caustictex[i] = NULL;
}

VARFR(causticscale, 0, 50, 10000, preloadwatershaders());
VARFR(causticmillis, 0, 75, 1000, preloadwatershaders());
FVARR(causticcontrast, 0, 0.6f, 2);
FVARR(causticoffset, 0, 0.7f, 1);
VARFP(caustics, 0, 1, 1, { loadcaustics(); preloadwatershaders(); });

void setupcaustics(int tmu, float surface = -1e16f)
{
    if(!caustictex[0]) loadcaustics(true);

    vec s = vec(0.011f, 0, 0.0066f).mul(100.0f/causticscale), t = vec(0, 0.011f, 0.0066f).mul(100.0f/causticscale);
    int tex = (lastmillis/causticmillis)%NUMCAUSTICS;
    float frac = float(lastmillis%causticmillis)/causticmillis;
    loopi(2)
    {
        glActiveTexture_(GL_TEXTURE0+tmu+i);
        glBindTexture(GL_TEXTURE_2D, caustictex[(tex+i)%NUMCAUSTICS]->id);
    }
    glActiveTexture_(GL_TEXTURE0);
    float blendscale = causticcontrast, blendoffset = 1;
    if(surface > -1e15f)
    {
        float bz = surface + camera1->o.z + (vertwater ? WATER_AMPLITUDE : 0);
        matrix4 m(vec4(s.x, t.x,  0, 0),
                  vec4(s.y, t.y,  0, 0),
                  vec4(s.z, t.z, -1, 0),
                  vec4(  0,   0, bz, 1));
        m.mul(worldmatrix);
        GLOBALPARAM(causticsmatrix, m);
        blendscale *= 0.5f;
        blendoffset = 0;
    }
    else
    {
        GLOBALPARAM(causticsS, s);
        GLOBALPARAM(causticsT, t);
    }
    GLOBALPARAMF(causticsblend, blendscale*(1-frac), blendscale*frac, blendoffset - causticoffset*blendscale);
}

void rendercaustics(float surface, float syl, float syr)
{
    if(!caustics || !causticscale || !causticmillis) return;
    glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
    setupcaustics(0, surface);
    SETSHADER(caustics);
    gle::defvertex(2);
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(1, -1);
    gle::attribf(-1, -1);
    gle::attribf(1, syr);
    gle::attribf(-1, syl);
    gle::end();
}

void renderwaterfog(int mat, float surface)
{
    glDepthFunc(GL_NOTEQUAL);
    glDepthMask(GL_FALSE);
    glDepthRange(1, 1);

    glEnable(GL_BLEND);

    glActiveTexture_(GL_TEXTURE9);
    if(msaalight) glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msdepthtex);
    else glBindTexture(GL_TEXTURE_RECTANGLE, gdepthtex);
    glActiveTexture_(GL_TEXTURE0);

    vec p[4] =
    {
        invcamprojmatrix.perspectivetransform(vec(-1, -1, -1)),
        invcamprojmatrix.perspectivetransform(vec(-1, 1, -1)),
        invcamprojmatrix.perspectivetransform(vec(1, -1, -1)),
        invcamprojmatrix.perspectivetransform(vec(1, 1, -1))
    };
    float bz = surface + camera1->o.z + (vertwater ? WATER_AMPLITUDE : 0),
          syl = p[1].z > p[0].z ? 2*(bz - p[0].z)/(p[1].z - p[0].z) - 1 : 1,
          syr = p[3].z > p[2].z ? 2*(bz - p[2].z)/(p[3].z - p[2].z) - 1 : 1;

    if((mat&MATF_VOLUME) == MAT_WATER)
    {
        const bvec &deepcolor = getwaterdeepcolour(mat);
        int deep = getwaterdeep(mat);
        GLOBALPARAMF(waterdeepcolor, deepcolor.x*ldrscaleb, deepcolor.y*ldrscaleb, deepcolor.z*ldrscaleb);
        vec deepfade = getwaterdeepfade(mat).tocolor().mul(deep);
        GLOBALPARAMF(waterdeepfade,
            deepfade.x ? calcfogdensity(deepfade.x) : -1e4f,
            deepfade.y ? calcfogdensity(deepfade.y) : -1e4f,
            deepfade.z ? calcfogdensity(deepfade.z) : -1e4f,
            deep ? calcfogdensity(deep) : -1e4f);

        rendercaustics(surface, syl, syr);
    }
    else
    {
        GLOBALPARAMF(waterdeepcolor, 0, 0, 0);
        GLOBALPARAMF(waterdeepfade, -1e4f, -1e4f, -1e4f, -1e4f);
    }

    GLOBALPARAMF(waterheight, bz);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    SETSHADER(waterfog);
    gle::defvertex(3);
    gle::begin(GL_TRIANGLE_STRIP);
    gle::attribf(1, -1, 1);
    gle::attribf(-1, -1, 1);
    gle::attribf(1, syr, 1);
    gle::attribf(-1, syl, 1);
    gle::end();

    glDisable(GL_BLEND);

    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDepthRange(0, 1);
}

/* vertex water */
VARP(watersubdiv, 0, 2, 3);
VARP(waterlod, 0, 1, 3);

static int wx1, wy1, wx2, wy2, wz, wsize, wsubdiv;
static float whoffset, whphase;

static inline float vertwangle(int v1, int v2)
{
    static const float whscale = 59.0f/23.0f/(2*M_PI);
    v1 &= wsize-1;
    v2 &= wsize-1;
    return v1*v2*whscale+whoffset;
}

static inline float vertwphase(float angle)
{
    float s = angle - int(angle) - 0.5f;
    s *= 8 - fabs(s)*16;
    return WATER_AMPLITUDE*s-WATER_OFFSET;
}

static inline void vertw(int v1, int v2, int v3)
{
    float h = vertwphase(vertwangle(v1, v2));
    gle::attribf(v1, v2, v3+h);
}

static inline void vertwq(float v1, float v2, float v3)
{
    gle::attribf(v1, v2, v3+whphase);
}

static inline void vertwn(float v1, float v2, float v3)
{
    float h = -WATER_OFFSET;
    gle::attribf(v1, v2, v3+h);
}

struct waterstrip
{
    int x1, y1, x2, y2, z;
    ushort size, subdiv;

    int numverts() const { return 2*((y2-y1)/subdiv + 1)*((x2-x1)/subdiv); }

    void save()
    {
        x1 = wx1;
        y1 = wy1;
        x2 = wx2;
        y2 = wy2;
        z = wz;
        size = wsize;
        subdiv = wsubdiv;
    }

    void restore()
    {
        wx1 = x1;
        wy1 = y1;
        wx2 = x2;
        wy2 = y2;
        wz = z;
        wsize = size;
        wsubdiv = subdiv;
    }
};
vector<waterstrip> waterstrips;

void flushwaterstrips()
{
    if(gle::attribbuf.length()) xtraverts += gle::end();
    gle::defvertex();
    int numverts = 0;
    loopv(waterstrips) numverts += waterstrips[i].numverts();
    gle::begin(GL_TRIANGLE_STRIP, numverts);
    loopv(waterstrips)
    {
        waterstrips[i].restore();
        for(int x = wx1; x < wx2; x += wsubdiv)
        {
            for(int y = wy1; y <= wy2; y += wsubdiv)
            {
                vertw(x,         y, wz);
                vertw(x+wsubdiv, y, wz);
            }
            x += wsubdiv;
            if(x >= wx2) break;
            for(int y = wy2; y >= wy1; y -= wsubdiv)
            {
                vertw(x,         y, wz);
                vertw(x+wsubdiv, y, wz);
            }
        }
        gle::multidraw();
    }
    waterstrips.setsize(0);
    wsize = 0;
    xtraverts += gle::end();
}

void flushwater(int mat = MAT_WATER, bool force = true)
{
    if(wsize)
    {
        if(wsubdiv >= wsize)
        {
            if(gle::attribbuf.empty()) { gle::defvertex(); gle::begin(GL_QUADS); }
            vertwq(wx1, wy1, wz);
            vertwq(wx2, wy1, wz);
            vertwq(wx2, wy2, wz);
            vertwq(wx1, wy2, wz);
        }
        else waterstrips.add().save();
        wsize = 0;
    }

    if(force)
    {
        if(gle::attribbuf.length()) xtraverts += gle::end();
        if(waterstrips.length()) flushwaterstrips();
    }
}

void rendervertwater(int subdiv, int xo, int yo, int z, int size, int mat)
{
    if(wsize == size && wsubdiv == subdiv && wz == z)
    {
        if(wx2 == xo)
        {
            if(wy1 == yo && wy2 == yo + size) { wx2 += size; return; }
        }
        else if(wy2 == yo && wx1 == xo && wx2 == xo + size) { wy2 += size; return; }
    }

    flushwater(mat, false);

    wx1 = xo;
    wy1 = yo;
    wx2 = xo + size,
    wy2 = yo + size;
    wz = z;
    wsize = size;
    wsubdiv = subdiv;

    ASSERT((wx1 & (subdiv - 1)) == 0);
    ASSERT((wy1 & (subdiv - 1)) == 0);
}

int calcwatersubdiv(int x, int y, int z, int size)
{
    float dist;
    if(camera1->o.x >= x && camera1->o.x < x + size &&
       camera1->o.y >= y && camera1->o.y < y + size)
        dist = fabs(camera1->o.z - float(z));
    else
        dist = vec(x + size/2, y + size/2, z + size/2).dist(camera1->o) - size*1.42f/2;
    int subdiv = watersubdiv + int(dist) / (32 << waterlod);
    return subdiv >= 31 ? INT_MAX : 1<<subdiv;
}

int renderwaterlod(int x, int y, int z, int size, int mat)
{
    if(size <= (32 << waterlod))
    {
        int subdiv = calcwatersubdiv(x, y, z, size);
        if(subdiv < size * 2) rendervertwater(min(subdiv, size), x, y, z, size, mat);
        return subdiv;
    }
    else
    {
        int subdiv = calcwatersubdiv(x, y, z, size);
        if(subdiv >= size)
        {
            if(subdiv < size * 2) rendervertwater(size, x, y, z, size, mat);
            return subdiv;
        }
        int childsize = size / 2,
            subdiv1 = renderwaterlod(x, y, z, childsize, mat),
            subdiv2 = renderwaterlod(x + childsize, y, z, childsize, mat),
            subdiv3 = renderwaterlod(x + childsize, y + childsize, z, childsize, mat),
            subdiv4 = renderwaterlod(x, y + childsize, z, childsize, mat),
            minsubdiv = subdiv1;
        minsubdiv = min(minsubdiv, subdiv2);
        minsubdiv = min(minsubdiv, subdiv3);
        minsubdiv = min(minsubdiv, subdiv4);
        if(minsubdiv < size * 2)
        {
            if(minsubdiv >= size) rendervertwater(size, x, y, z, size, mat);
            else
            {
                if(subdiv1 >= size) rendervertwater(childsize, x, y, z, childsize, mat);
                if(subdiv2 >= size) rendervertwater(childsize, x + childsize, y, z, childsize, mat);
                if(subdiv3 >= size) rendervertwater(childsize, x + childsize, y + childsize, z, childsize, mat);
                if(subdiv4 >= size) rendervertwater(childsize, x, y + childsize, z, childsize, mat);
            }
        }
        return minsubdiv;
    }
}

void renderflatwater(int x, int y, int z, int rsize, int csize, int mat)
{
    if(gle::attribbuf.empty()) { gle::defvertex(); gle::begin(GL_QUADS); }
    vertwn(x,       y,       z);
    vertwn(x+rsize, y,       z);
    vertwn(x+rsize, y+csize, z);
    vertwn(x,       y+csize, z);
}

VARFP(vertwater, 0, 1, 1, allchanged());

static inline void renderwater(const materialsurface &m, int mat = MAT_WATER)
{
    bool falling = false;
    if(getwatermateriallevel(m, falling) >= 0 && !falling)
    {
        flushwater(mat);
        if(gle::attribbuf.empty()) { gle::defvertex(); gle::begin(GL_QUADS); }
        const float offset = drawtex == DRAWTEX_MINIMAP ? -WATER_OFFSET : whphase;
        const float x = m.o.x, y = m.o.y, z = m.o.z, rsize = m.rsize, csize = m.csize;
        gle::attribf(x,         y,         z - getwatercornerdrop(x,         y,         z) + offset);
        gle::attribf(x + rsize, y,         z - getwatercornerdrop(x + rsize, y,         z) + offset);
        gle::attribf(x + rsize, y + csize, z - getwatercornerdrop(x + rsize, y + csize, z) + offset);
        gle::attribf(x,         y + csize, z - getwatercornerdrop(x,         y + csize, z) + offset);
        return;
    }
    const int z = m.o.z - int(getwatermaterialdrop(m));
    if(!vertwater || drawtex == DRAWTEX_MINIMAP) renderflatwater(m.o.x, m.o.y, z, m.rsize, m.csize, mat);
    else if(renderwaterlod(m.o.x, m.o.y, z, m.csize, mat) >= int(m.csize) * 2)
        rendervertwater(m.csize, m.o.x, m.o.y, z, m.csize, mat);
}

#define WATERVARS(name) \
    CVAR0R(name##colour, 0x6495FF); \
    CVAR0R(name##deepcolour, 0x021420); \
    CVAR0R(name##deepfade, 0x60BFFF); \
    CVAR0R(name##refractcolour, 0xFFFFFF); \
    VARR(name##fog, 0, 300, 10000); \
    VARR(name##deep, 0, 50, 10000); \
    VARR(name##spec, 0, 150, 200); \
    FVARR(name##refract, 0, 0.1f, 1e3f); \
    CVARR(name##fallcolour, 0); \
    CVARR(name##fallrefractcolour, 0); \
    VARR(name##fallspec, 0, 150, 200); \
    FVARR(name##fallrefract, 0, 0.1f, 1e3f);

WATERVARS(water)
WATERVARS(water2)
WATERVARS(water3)
WATERVARS(water4)

GETMATIDXVAR(water, colour, const bvec &)
GETMATIDXVAR(water, deepcolour, const bvec &)
GETMATIDXVAR(water, deepfade, const bvec &)
GETMATIDXVAR(water, refractcolour, const bvec &)
GETMATIDXVAR(water, fallcolour, const bvec &)
GETMATIDXVAR(water, fallrefractcolour, const bvec &)
GETMATIDXVAR(water, fog, int)
GETMATIDXVAR(water, deep, int)
GETMATIDXVAR(water, spec, int)
GETMATIDXVAR(water, refract, float)
GETMATIDXVAR(water, fallspec, int)
GETMATIDXVAR(water, fallrefract, float)

#define LAVAVARS(name) \
    CVAR0R(name##colour, 0xFF4000); \
    VARR(name##fog, 0, 50, 10000); \
    FVARR(name##glowmin, 0, 0.25f, 2); \
    FVARR(name##glowmax, 0, 1.0f, 2); \
    VARR(name##spec, 0, 25, 200);

LAVAVARS(lava)
LAVAVARS(lava2)
LAVAVARS(lava3)
LAVAVARS(lava4)

GETMATIDXVAR(lava, colour, const bvec &)
GETMATIDXVAR(lava, fog, int)
GETMATIDXVAR(lava, glowmin, float)
GETMATIDXVAR(lava, glowmax, float)
GETMATIDXVAR(lava, spec, int)

VARFP(waterreflect, 0, 1, 1, { preloadwatershaders(); });
VARR(waterreflectstep, 1, 32, 10000);
VARFP(waterenvmap, 0, 1, 1, { preloadwatershaders(); });
VARFP(waterfallenv, 0, 1, 1, preloadwatershaders());

void preloadwatershaders(bool force)
{
    static bool needwater = false;
    if(force) needwater = true;
    if(!needwater) return;

    if(caustics && causticscale && causticmillis)
    {
        if(waterreflect) useshaderbyname("waterreflectcaustics");
        else if(waterenvmap) useshaderbyname("waterenvcaustics");
        else useshaderbyname("watercaustics");
    }
    else
    {
        if(waterreflect) useshaderbyname("waterreflect");
        else if(waterenvmap) useshaderbyname("waterenv");
        else useshaderbyname("water");
    }

    useshaderbyname("underwater");

    if(waterfallenv) useshaderbyname("waterfallenv");
    useshaderbyname("waterfall");

    useshaderbyname("waterfog");

    useshaderbyname("waterminimap");
}

static float wfwave = 0.0f;

static void renderwaterfall(const materialsurface &m, float offset)
{
    if(gle::attribbuf.empty())
    {
        gle::defvertex();
        gle::defnormal(4, GL_BYTE);
        gle::begin(GL_QUADS);
    }
    if(m.orient == O_BOTTOM)
    {
        const float x = m.o.x, y = m.o.y, z = m.o.z - offset;
        gle::attribf(x,           y,           z); gle::attrib(matnormals[O_BOTTOM]);
        gle::attribf(x,           y + m.csize, z); gle::attrib(matnormals[O_BOTTOM]);
        gle::attribf(x + m.rsize, y + m.csize, z); gle::attrib(matnormals[O_BOTTOM]);
        gle::attribf(x + m.rsize, y,           z); gle::attrib(matnormals[O_BOTTOM]);
        return;
    }
    float x = m.o.x, y = m.o.y, zmin = m.o.z, zmax = zmin - getwatermaterialdrop(m);
    bool falling = false;
    if(getwatermateriallevel(m, falling) >= 0 && !falling)
    {
        const float ztop = zmin + (dimension(m.orient) == 0 ? m.csize : m.rsize);
        switch(m.orient)
        {
            case O_LEFT:
                gle::attribf(x - offset, y + m.rsize, ztop - getwatercornerdrop(x, y + m.rsize, ztop) + wfwave);
                gle::attrib(matnormals[m.orient]);
                gle::attribf(x - offset, y + m.rsize, zmin); gle::attrib(matnormals[m.orient]);
                gle::attribf(x - offset, y, zmin); gle::attrib(matnormals[m.orient]);
                gle::attribf(x - offset, y, ztop - getwatercornerdrop(x, y, ztop) + wfwave);
                gle::attrib(matnormals[m.orient]);
                return;
            case O_RIGHT:
                gle::attribf(x + offset, y + m.rsize, ztop - getwatercornerdrop(x, y + m.rsize, ztop) + wfwave);
                gle::attrib(matnormals[m.orient]);
                gle::attribf(x + offset, y, ztop - getwatercornerdrop(x, y, ztop) + wfwave);
                gle::attrib(matnormals[m.orient]);
                gle::attribf(x + offset, y, zmin); gle::attrib(matnormals[m.orient]);
                gle::attribf(x + offset, y + m.rsize, zmin); gle::attrib(matnormals[m.orient]);
                return;
            case O_BACK:
                gle::attribf(x + m.csize, y - offset, ztop - getwatercornerdrop(x + m.csize, y, ztop) + wfwave);
                gle::attrib(matnormals[m.orient]);
                gle::attribf(x, y - offset, ztop - getwatercornerdrop(x, y, ztop) + wfwave);
                gle::attrib(matnormals[m.orient]);
                gle::attribf(x, y - offset, zmin); gle::attrib(matnormals[m.orient]);
                gle::attribf(x + m.csize, y - offset, zmin); gle::attrib(matnormals[m.orient]);
                return;
            case O_FRONT:
                gle::attribf(x, y + offset, zmin); gle::attrib(matnormals[m.orient]);
                gle::attribf(x, y + offset, ztop - getwatercornerdrop(x, y, ztop) + wfwave);
                gle::attrib(matnormals[m.orient]);
                gle::attribf(x + m.csize, y + offset, ztop - getwatercornerdrop(x + m.csize, y, ztop) + wfwave);
                gle::attrib(matnormals[m.orient]);
                gle::attribf(x + m.csize, y + offset, zmin); gle::attrib(matnormals[m.orient]);
                return;
        }
    }
    if(m.ends&1) zmin += -WATER_OFFSET-WATER_AMPLITUDE;
    if(m.ends&2) zmax += wfwave;
    int csize = m.csize, rsize = m.rsize;
    switch(m.orient)
    {
    #define GENFACEORIENT(orient, v0, v1, v2, v3) \
        case orient: v0 v1 v2 v3 break;
    #define GENFACEVERT(orient, vert, mx,my,mz, sx,sy,sz) \
        { \
            gle::attribf(mx sx, my sy, mz sz); \
            gle::attrib(matnormals[orient]); \
        }
        GENFACEVERTSXY(x, x, y, y, zmin, zmax, /**/, + csize, /**/, + rsize, + offset, - offset)
    #undef GENFACEORIENT
    #undef GENFACEVERT
    }
}

void renderlava()
{
    loopk(4)
    {
        if(lavasurfs[k].empty() && (drawtex == DRAWTEX_MINIMAP || lavafallsurfs[k].empty())) continue;

        MatSlot &lslot = lookupmaterialslot(MAT_LAVA+k);

        SETSHADER(lava);
        float t = lastmillis/2000.0f;
        t -= floor(t);
        t = 1.0f - 2*fabs(t-0.5f);
        t = 0.5f + 0.5f*t;
        float glowmin = getlavaglowmin(k), glowmax = getlavaglowmax(k);
        int spec = getlavaspec(k);
        LOCALPARAMF(lavaglow, 0.5f*(glowmin + (glowmax-glowmin)*t));
        LOCALPARAMF(lavaspec, spec/100.0f);

        if(lavasurfs[k].length())
        {
            Texture *tex = lslot.sts.inrange(0) ? lslot.sts[0].t: notexture;
            float xscale = TEX_SCALE/(tex->xs*lslot.scale);
            float yscale = TEX_SCALE/(tex->ys*lslot.scale);
            float scroll = lastmillis/1000.0f;
            LOCALPARAMF(lavatexgen, xscale, yscale, scroll, scroll);

            whoffset = fmod(float(lastmillis/2000.0f/(2*M_PI)), 1.0f);
            whphase = vertwphase(whoffset);

            glBindTexture(GL_TEXTURE_2D, tex->id);
            glActiveTexture_(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, lslot.sts.inrange(1) ? lslot.sts[1].t->id : notexture->id);
            glActiveTexture_(GL_TEXTURE0);

            gle::normal(vec(0, 0, 1));

            vector<materialsurface> &surfs = lavasurfs[k];
            loopv(surfs) renderwater(surfs[i], MAT_LAVA);
            flushwater(MAT_LAVA);
        }

        if(drawtex != DRAWTEX_MINIMAP && lavafallsurfs[k].length())
        {
            Texture *tex = lslot.sts.inrange(2) ? lslot.sts[2].t : (lslot.sts.inrange(0) ? lslot.sts[0].t : notexture);
            float angle = fmod(float(lastmillis/2000.0f/(2*M_PI)), 1.0f),
                  s = angle - int(angle) - 0.5f;
            s *= 8 - fabs(s)*16;
            wfwave = vertwater ? WATER_AMPLITUDE*s-WATER_OFFSET : -WATER_OFFSET;
            float scroll = -16.0f*lastmillis/3000.0f;
            float xscale = TEX_SCALE/(tex->xs*lslot.scale);
            float yscale = TEX_SCALE/(tex->ys*lslot.scale);
            LOCALPARAMF(lavatexgen, xscale, yscale, 0.0f, scroll);

            glBindTexture(GL_TEXTURE_2D, tex->id);
            glActiveTexture_(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, lslot.sts.inrange(2) ? (lslot.sts.inrange(3) ? lslot.sts[3].t->id : notexture->id) : (lslot.sts.inrange(1) ? lslot.sts[1].t->id : notexture->id));
            glActiveTexture_(GL_TEXTURE0);

            vector<materialsurface> &surfs = lavafallsurfs[k];
            loopv(surfs)
            {
                materialsurface &m = surfs[i];
                renderwaterfall(m, 0.1f);
            }
            xtraverts += gle::end();
        }
    }
}

void renderwaterfalls()
{
    loopk(4)
    {
        vector<materialsurface> &surfs = waterfallsurfs[k];
        if(surfs.empty()) continue;

        MatSlot &wslot = lookupmaterialslot(MAT_WATER+k);

        Texture *tex = wslot.sts.inrange(2) ? wslot.sts[2].t : (wslot.sts.inrange(0) ? wslot.sts[0].t : notexture);
        float angle = fmod(float(lastmillis/600.0f/(2*M_PI)), 1.0f),
              s = angle - int(angle) - 0.5f;
        s *= 8 - fabs(s)*16;
        wfwave = vertwater ? WATER_AMPLITUDE*s-WATER_OFFSET : -WATER_OFFSET;
        float scroll = -16.0f*lastmillis/1000.0f;
        float xscale = TEX_SCALE/(tex->xs*wslot.scale);
        float yscale = TEX_SCALE/(tex->ys*wslot.scale);
        GLOBALPARAMF(waterfalltexgen, xscale, yscale, 0.0f, scroll);

        bvec color = getwaterfallcolour(k), refractcolor = getwaterfallrefractcolour(k);
        if(color.iszero()) color = getwatercolour(k);
        if(refractcolor.iszero()) refractcolor = getwaterrefractcolour(k);
        float colorscale = (0.5f/255), refractscale = colorscale/ldrscale;
        float refract = getwaterfallrefract(k);
        int spec = getwaterfallspec(k);
        GLOBALPARAMF(waterfallcolor, color.x*colorscale, color.y*colorscale, color.z*colorscale);
        GLOBALPARAMF(waterfallrefract, refractcolor.x*refractscale, refractcolor.y*refractscale, refractcolor.z*refractscale, refract*viewh);
        GLOBALPARAMF(waterfallspec, spec/100.0f);

        if(waterfallenv) SETSHADER(waterfallenv);
        else SETSHADER(waterfall);

        glBindTexture(GL_TEXTURE_2D, tex->id);
        glActiveTexture_(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, wslot.sts.inrange(2) ? (wslot.sts.inrange(3) ? wslot.sts[3].t->id : notexture->id) : (wslot.sts.inrange(1) ? wslot.sts[1].t->id : notexture->id));
        if(waterfallenv)
        {
            glActiveTexture_(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_CUBE_MAP, lookupenvmap(wslot));
        }
        glActiveTexture_(GL_TEXTURE0);

        loopv(surfs)
        {
            materialsurface &m = surfs[i];
            renderwaterfall(m, 0.1f);
        }
        xtraverts += gle::end();
    }
}

void renderwater()
{
    loopk(4)
    {
        vector<materialsurface> &surfs = watersurfs[k];
        if(surfs.empty()) continue;

        MatSlot &wslot = lookupmaterialslot(MAT_WATER+k);

        Texture *tex = wslot.sts.inrange(0) ? wslot.sts[0].t: notexture;
        float xscale = TEX_SCALE/(tex->xs*wslot.scale);
        float yscale = TEX_SCALE/(tex->ys*wslot.scale);
        GLOBALPARAMF(watertexgen, xscale, yscale);

        whoffset = fmod(float(lastmillis/600.0f/(2*M_PI)), 1.0f);
        whphase = vertwphase(whoffset);

        glBindTexture(GL_TEXTURE_2D, tex->id);
        glActiveTexture_(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, wslot.sts.inrange(1) ? wslot.sts[1].t->id : notexture->id);
        if(caustics && causticscale && causticmillis) setupcaustics(2);
        if(waterenvmap && !waterreflect && drawtex != DRAWTEX_MINIMAP)
        {
            glActiveTexture_(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_CUBE_MAP, lookupenvmap(wslot));
        }
        glActiveTexture_(GL_TEXTURE0);

        float colorscale = 0.5f/255, refractscale = colorscale/ldrscale, reflectscale = 0.5f/ldrscale;
        const bvec &color = getwatercolour(k);
        const bvec &deepcolor = getwaterdeepcolour(k);
        const bvec &refractcolor = getwaterrefractcolour(k);
        int fog = getwaterfog(k), deep = getwaterdeep(k), spec = getwaterspec(k);
        float refract = getwaterrefract(k);
        GLOBALPARAMF(watercolor, color.x*colorscale, color.y*colorscale, color.z*colorscale);
        GLOBALPARAMF(waterdeepcolor, deepcolor.x*colorscale, deepcolor.y*colorscale, deepcolor.z*colorscale);
        float fogdensity = fog ? calcfogdensity(fog) : -1e4f;
        GLOBALPARAMF(waterfog, fogdensity);
        vec deepfade = getwaterdeepfade(k).tocolor().mul(deep);
        GLOBALPARAMF(waterdeepfade,
            deepfade.x ? calcfogdensity(deepfade.x) : -1e4f,
            deepfade.y ? calcfogdensity(deepfade.y) : -1e4f,
            deepfade.z ? calcfogdensity(deepfade.z) : -1e4f,
            deep ? calcfogdensity(deep) : -1e4f);
        GLOBALPARAMF(waterspec, spec/100.0f);
        GLOBALPARAMF(waterreflect, reflectscale, reflectscale, reflectscale, waterreflectstep);
        GLOBALPARAMF(waterrefract, refractcolor.x*refractscale, refractcolor.y*refractscale, refractcolor.z*refractscale, refract*viewh);

        #define SETWATERSHADER(which, name) \
        do { \
            static Shader *name##shader = NULL; \
            if(!name##shader) name##shader = lookupshaderbyname(#name); \
            which##shader = name##shader; \
        } while(0)

        Shader *aboveshader = NULL;
        if(drawtex == DRAWTEX_MINIMAP) SETWATERSHADER(above, waterminimap);
        else if(caustics && causticscale && causticmillis)
        {
            if(waterreflect) SETWATERSHADER(above, waterreflectcaustics);
            else if(waterenvmap) SETWATERSHADER(above, waterenvcaustics);
            else SETWATERSHADER(above, watercaustics);
        }
        else
        {
            if(waterreflect) SETWATERSHADER(above, waterreflect);
            else if(waterenvmap) SETWATERSHADER(above, waterenv);
            else SETWATERSHADER(above, water);
        }

        Shader *belowshader = NULL;
        if(drawtex != DRAWTEX_MINIMAP) SETWATERSHADER(below, underwater);

        aboveshader->set();
        loopv(surfs)
        {
            materialsurface &m = surfs[i];
            if(camera1->o.z < m.o.z - WATER_OFFSET - getwatermaterialdrop(m)) continue;
            renderwater(m);
        }
        flushwater();

        if(belowshader)
        {
            belowshader->set();
            loopv(surfs)
            {
                materialsurface &m = surfs[i];
                if(camera1->o.z >= m.o.z - WATER_OFFSET - getwatermaterialdrop(m)) continue;
                renderwater(m);
            }
            flushwater();
        }
    }
}

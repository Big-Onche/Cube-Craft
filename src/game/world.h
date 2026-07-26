#ifndef __GAME_WORLD_H__
#define __GAME_WORLD_H__

#ifdef SQRT3
#pragma push_macro("SQRT3")
#undef SQRT3
#define RESTORE_WORLD_SQRT3
#endif
#include "FastNoiseLite.h"
#ifdef RESTORE_WORLD_SQRT3
#pragma pop_macro("SQRT3")
#undef RESTORE_WORLD_SQRT3
#endif

struct stream;

namespace game
{
    enum worldbiome
    {
        WORLD_BIOME_OCEAN,
        WORLD_BIOME_SNOW,
        WORLD_BIOME_DESERT,
        WORLD_BIOME_FOREST,
        WORLD_BIOME_PLAINS
    };

    struct worldsettings
    {
        float geologyfrequency, maxcontinentheight, maxoceandepth;
        float oceancoverage, terraincoverage;
        float temperaturefrequency, moisturefrequency, biomevariationfrequency;
        float biomevariationstrength, rockfrequency;
        float deserttemperature, desertmoisture, forestmoisture;
        float foresttreedensity, plainstreedensity;
        float cavefrequency, cavethreshold, largecavefrequency;
        float largecavethreshold, largecavedeepthreshold;
        float tunnelfrequency, tunnelwidth, caveentrancewidth;
        float lavalakeshallowchance, lavalakedeepchance;
        float lavalakeshapefrequency, lavalakeshapevariation;
        int sealevel, snowheight, stonelow, stonehigh;
        int biomeblend, coastwidth, coastvariation;
        int beachminheight, beachmaxheight;
        int pinestartheight, pinefullheight;
        int cavemindepth, cavefulldepth, cavedeepheight;
        int bottomlavalayers, lavalakestartheight, lavalakedeepheight;
        int lavalakeminsize, lavalakemaxsize, lavalakespacing;

        worldsettings();
    };

    struct worldgenerator
    {
        FastNoiseLite geology, hills;
        FastNoiseLite temperature, moisture, biomevariation, biomeblend, rockiness;
        FastNoiseLite caves, largecaves, tunnela, tunnelb, lakeshape;
        worldsettings settings;
        int seed;

        worldgenerator(int seed, const worldsettings &settings = worldsettings());

        int height(int x, int y) const;
        int biome(int x, int y, int height) const;
        bool rock(int x, int y, int height) const;
    };

    extern int getworldseed();
    extern void loadworldseed(int seed);
    extern void activateworldseed();
    extern void saveworldsettings(stream *f);
}

#endif

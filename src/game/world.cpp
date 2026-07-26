#include "game.h"
#include "world.h"

#ifndef STANDALONE

VARP(worldseed, 0, 1337, INT_MAX);

FVAR(worldgeologyfrequency, 0.00001f, 0.0012f, 0.1f);
FVAR(worldmaxcontinentheight, 1.0f, 96.0f, 255.0f);
FVAR(worldmaxoceandepth, 1.0f, 32.0f, 255.0f);
FVAR(worldoceancoverage, 0.0f, 55.0f, 100.0f);
FVAR(worldterraincoverage, 0.0f, 45.0f, 100.0f);

FVAR(worldtemperaturefrequency, 0.000001f, 0.0004f, 1.0f);
FVAR(worldmoisturefrequency, 0.000001f, 0.0006f, 1.0f);
FVAR(worldbiomevariationfrequency, 0.000001f, 0.001f, 1.0f);
FVAR(worldbiomevariationstrength, 0.0f, 0.15f, 1.0f);
FVAR(worldrockfrequency, 0.000001f, 0.08f, 1.0f);

VAR(worldsealevel, -255, 0, 255);
VAR(worldsnowheight, -255, 160, 255);
VAR(worldstonelow, -255, 75, 255);
VAR(worldstonehigh, -255, 125, 255);
VAR(worldbiomeblend, 0, 16, 64);
VAR(worldcoastwidth, 0, 8, 32);
VAR(worldcoastvariation, 0, 3, 16);
VAR(worldbeachminheight, -32, -2, 32);
VAR(worldbeachmaxheight, -32, 1, 32);

FVAR(worlddeserttemperature, -1.0f, 0.4f, 1.0f);
FVAR(worlddesertmoisture, -1.0f, -0.18f, 1.0f);
FVAR(worldforestmoisture, -1.0f, 0.10f, 1.0f);
FVAR(worldforesttreedensity, 0.0f, 0.04f, 0.25f);
FVAR(worldplainstreedensity, 0.0f, 0.0017f, 0.25f);
VAR(worldpinestartheight, -255, 50, 255);
VAR(worldpinefullheight, -255, 100, 255);

FVAR(worldcavefrequency, 0.0001f, 0.045f, 0.25f);
FVAR(worldcavethreshold, -1.0f, 0.58f, 1.0f);
FVAR(worldlargecavefrequency, 0.0001f, 0.018f, 0.25f);
FVAR(worldlargecavethreshold, -1.0f, 0.76f, 1.0f);
FVAR(worldlargecavedeepthreshold, -1.0f, 0.58f, 1.0f);
FVAR(worldtunnelfrequency, 0.0001f, 0.025f, 0.25f);
FVAR(worldtunnelwidth, 0.001f, 0.075f, 0.3f);
FVAR(worldcaveentrancewidth, 0.001f, 0.05f, 0.3f);
VAR(worldcavemindepth, 1, 12, 64);
VAR(worldcavefulldepth, 1, 32, 128);
VAR(worldcavedeepheight, -255, -64, 255);

VAR(worldbottomlavalayers, 0, 3, 16);
VAR(worldlavalakestartheight, -255, -16, 255);
VAR(worldlavalakedeepheight, -255, -64, 255);
FVAR(worldlavalakeshallowchance, 0.0f, 0.03f, 1.0f);
FVAR(worldlavalakedeepchance, 0.0f, 0.22f, 1.0f);
VAR(worldlavalakeminsize, 1, 4, 32);
VAR(worldlavalakemaxsize, 1, 14, 32);
VAR(worldlavalakespacing, 8, 24, 64);
FVAR(worldlavalakeshapefrequency, 0.001f, 0.08f, 1.0f);
FVAR(worldlavalakeshapevariation, 0.0f, 0.35f, 0.75f);

namespace game
{
    static int activeworldseed = 1337;

    static float smoothstep(float low, float high, float value)
    {
        if(high <= low) return value >= high ? 1.0f : 0.0f;
        const float t = clamp((value - low) / (high - low), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    static void setupnoise(FastNoiseLite &noise, int seed, float frequency, int octaves,
                           float gain = 0.5f)
    {
        noise.SetSeed(seed);
        noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
        noise.SetFrequency(frequency);
        noise.SetFractalType(octaves > 1 ? FastNoiseLite::FractalType_FBm
                                         : FastNoiseLite::FractalType_None);
        noise.SetFractalOctaves(octaves);
        noise.SetFractalLacunarity(1.8f);
        noise.SetFractalGain(gain);
    }

    worldsettings::worldsettings()
        : geologyfrequency(worldgeologyfrequency),
          maxcontinentheight(worldmaxcontinentheight), maxoceandepth(worldmaxoceandepth),
          oceancoverage(worldoceancoverage), terraincoverage(worldterraincoverage),
          temperaturefrequency(worldtemperaturefrequency),
          moisturefrequency(worldmoisturefrequency),
          biomevariationfrequency(worldbiomevariationfrequency),
          biomevariationstrength(worldbiomevariationstrength),
          rockfrequency(worldrockfrequency),
          deserttemperature(worlddeserttemperature), desertmoisture(worlddesertmoisture),
          forestmoisture(worldforestmoisture),
          foresttreedensity(worldforesttreedensity), plainstreedensity(worldplainstreedensity),
          cavefrequency(worldcavefrequency), cavethreshold(worldcavethreshold),
          largecavefrequency(worldlargecavefrequency),
          largecavethreshold(worldlargecavethreshold),
          largecavedeepthreshold(worldlargecavedeepthreshold),
          tunnelfrequency(worldtunnelfrequency), tunnelwidth(worldtunnelwidth),
          caveentrancewidth(worldcaveentrancewidth),
          lavalakeshallowchance(worldlavalakeshallowchance),
          lavalakedeepchance(worldlavalakedeepchance),
          lavalakeshapefrequency(worldlavalakeshapefrequency),
          lavalakeshapevariation(worldlavalakeshapevariation),
          sealevel(worldsealevel), snowheight(worldsnowheight),
          stonelow(worldstonelow), stonehigh(worldstonehigh),
          biomeblend(worldbiomeblend), coastwidth(worldcoastwidth),
          coastvariation(worldcoastvariation),
          beachminheight(worldbeachminheight), beachmaxheight(worldbeachmaxheight),
          pinestartheight(worldpinestartheight), pinefullheight(worldpinefullheight),
          cavemindepth(worldcavemindepth), cavefulldepth(worldcavefulldepth),
          cavedeepheight(worldcavedeepheight), bottomlavalayers(worldbottomlavalayers),
          lavalakestartheight(worldlavalakestartheight),
          lavalakedeepheight(worldlavalakedeepheight),
          lavalakeminsize(worldlavalakeminsize), lavalakemaxsize(worldlavalakemaxsize),
          lavalakespacing(worldlavalakespacing)
    {
    }

    worldgenerator::worldgenerator(int seed, const worldsettings &settings)
        : settings(settings), seed(seed)
    {
        // Two gentle octaves define the continental silhouette. Hills remain
        // broad and subordinate so changing one frequency scales all geology.
        setupnoise(geology, seed, settings.geologyfrequency, 2, 0.35f);
        setupnoise(hills, seed ^ 0x4A39B70D, settings.geologyfrequency * 3.5f, 2, 0.30f);
        setupnoise(temperature, seed ^ 0x51D7348B, settings.temperaturefrequency, 3);
        setupnoise(moisture, seed ^ 0x2F6E2B1D, settings.moisturefrequency, 3);
        setupnoise(biomevariation, seed ^ 0x749A7C15, settings.biomevariationfrequency, 3);
        setupnoise(biomeblend, seed ^ 0x13C6E91F,
                   settings.biomeblend > 0 ? 1.0f / settings.biomeblend : 1.0f, 1);
        setupnoise(rockiness, seed ^ 0x5E4A19C3, settings.rockfrequency, 2);
        setupnoise(caves, seed ^ 0x7A84F12D, settings.cavefrequency, 2);
        setupnoise(largecaves, seed ^ 0x36B9C7E5, settings.largecavefrequency, 2);
        setupnoise(tunnela, seed ^ 0x19F3A6C7, settings.tunnelfrequency, 2);
        setupnoise(tunnelb, seed ^ 0x5C2D8E91, settings.tunnelfrequency, 2);
        setupnoise(lakeshape, seed ^ 0x43E7B5D9, settings.lavalakeshapefrequency, 2);
    }

    int worldgenerator::height(int x, int y) const
    {
        const float noisex = x + 10000.5f, noisey = y - 10000.5f;
        const float continental = geology.GetNoise(noisex, noisey);
        const float coverage = settings.oceancoverage + settings.terraincoverage;
        const float oceanratio = coverage > 0.0f ? settings.oceancoverage / coverage : 0.5f;
        const float threshold = oceanratio <= 0.0f ? -0.98f :
                                oceanratio >= 1.0f ? 0.98f : oceanratio - 0.5f;
        float elevation;
        if(continental >= threshold)
        {
            const float distance = clamp((continental - threshold) / max(1.0f - threshold, 0.001f),
                                         0.0f, 1.0f);
            const float coastrise = smoothstep(0.0f, 0.28f, distance);
            const float inland = smoothstep(0.0f, 0.72f, distance);
            const float hill = clamp(hills.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f);
            elevation = settings.maxcontinentheight
                      * coastrise * (0.55f + 0.30f * inland + 0.15f * hill);
        }
        else
        {
            const float distance = clamp((threshold - continental) / max(threshold + 1.0f, 0.001f),
                                         0.0f, 1.0f);
            const float shelf = smoothstep(0.0f, 0.25f, distance);
            const float deepocean = smoothstep(0.15f, 0.85f, distance);
            elevation = -settings.maxoceandepth * (0.25f * shelf + 0.75f * deepocean);
        }
        elevation = clamp(elevation, -settings.maxoceandepth, settings.maxcontinentheight);
        return clamp(int(floor(settings.sealevel + elevation + 0.5f)), -255, 255);
    }

    int worldgenerator::biome(int x, int y, int height) const
    {
        if(height < settings.sealevel) return WORLD_BIOME_OCEAN;

        const float noisex = x + 10000.5f, noisey = y - 10000.5f;
        const float variation = biomevariation.GetNoise(noisex, noisey);
        const float temperaturevalue = temperature.GetNoise(noisex, noisey)
                                     + variation * settings.biomevariationstrength;
        const float moisturevalue = clamp(moisture.GetNoise(noisex, noisey)
                                        - variation * settings.biomevariationstrength,
                                          -1.0f, 1.0f);

        if(settings.biomeblend <= 0)
        {
            if(height > settings.snowheight) return WORLD_BIOME_SNOW;
            if(temperaturevalue > settings.deserttemperature &&
               moisturevalue < settings.desertmoisture) return WORLD_BIOME_DESERT;
            if(moisturevalue > settings.forestmoisture) return WORLD_BIOME_FOREST;
            return WORLD_BIOME_PLAINS;
        }

        const float blendblocks = settings.biomeblend;
        const float temperatureblend = max(blendblocks * settings.temperaturefrequency * 2.0f,
                                           0.001f);
        const float moistureblend = max(blendblocks * settings.moisturefrequency * 2.0f, 0.001f);
        const float selector = clamp((biomeblend.GetNoise(noisex, noisey) + 1.0f) * 0.5f,
                                     0.0f, 1.0f);
        const float snowweight = smoothstep(settings.snowheight - blendblocks * 0.5f,
                                            settings.snowheight + blendblocks * 0.5f, height);
        const float hotweight = smoothstep(settings.deserttemperature - temperatureblend,
                                           settings.deserttemperature + temperatureblend,
                                           temperaturevalue);
        const float dryweight = 1.0f - smoothstep(settings.desertmoisture - moistureblend,
                                                  settings.desertmoisture + moistureblend,
                                                  moisturevalue);
        const float forestweight = smoothstep(settings.forestmoisture - moistureblend,
                                              settings.forestmoisture + moistureblend,
                                              moisturevalue);
        if(snowweight > selector) return WORLD_BIOME_SNOW;
        if(hotweight * dryweight > selector) return WORLD_BIOME_DESERT;
        if(forestweight > selector) return WORLD_BIOME_FOREST;
        return WORLD_BIOME_PLAINS;
    }

    bool worldgenerator::rock(int x, int y, int height) const
    {
        const float low = min(settings.stonelow, settings.stonehigh);
        const float high = max(settings.stonelow, settings.stonehigh);
        if(height <= low) return false;
        if(height >= high) return true;

        const float rockweight = smoothstep(low, high, height);
        const float selector = clamp(rockiness.GetNoise(x + 10000.5f, y - 10000.5f) * 1.25f
                                   + 0.5f, 0.0f, 1.0f);
        return rockweight > selector;
    }

    int getworldseed()
    {
        return activeworldseed;
    }

    void loadworldseed(int seed)
    {
        worldseed = max(seed, 0);
        activeworldseed = worldseed;
    }

    void activateworldseed()
    {
        loadworldseed(worldseed);
    }

    void saveworldsettings(stream *f)
    {
        f->printf(
            "worldloadseed %d\n"
            "worldgeologyfrequency %.9g\n"
            "worldmaxcontinentheight %.9g\n"
            "worldmaxoceandepth %.9g\n"
            "worldoceancoverage %.9g\n"
            "worldterraincoverage %.9g\n"
            "worldtemperaturefrequency %.9g\n"
            "worldmoisturefrequency %.9g\n"
            "worldbiomevariationfrequency %.9g\n"
            "worldbiomevariationstrength %.9g\n"
            "worldrockfrequency %.9g\n"
            "worldsealevel %d\n"
            "worldsnowheight %d\n"
            "worldstonelow %d\n"
            "worldstonehigh %d\n"
            "worldbiomeblend %d\n"
            "worldcoastwidth %d\n"
            "worldcoastvariation %d\n"
            "worldbeachminheight %d\n"
            "worldbeachmaxheight %d\n"
            "worlddeserttemperature %.9g\n"
            "worlddesertmoisture %.9g\n"
            "worldforestmoisture %.9g\n"
            "worldforesttreedensity %.9g\n"
            "worldplainstreedensity %.9g\n"
            "worldpinestartheight %d\n"
            "worldpinefullheight %d\n"
            "worldcavefrequency %.9g\n"
            "worldcavethreshold %.9g\n"
            "worldlargecavefrequency %.9g\n"
            "worldlargecavethreshold %.9g\n"
            "worldlargecavedeepthreshold %.9g\n"
            "worldtunnelfrequency %.9g\n"
            "worldtunnelwidth %.9g\n"
            "worldcaveentrancewidth %.9g\n"
            "worldcavemindepth %d\n"
            "worldcavefulldepth %d\n"
            "worldcavedeepheight %d\n"
            "worldbottomlavalayers %d\n"
            "worldlavalakestartheight %d\n"
            "worldlavalakedeepheight %d\n"
            "worldlavalakeshallowchance %.9g\n"
            "worldlavalakedeepchance %.9g\n"
            "worldlavalakeminsize %d\n"
            "worldlavalakemaxsize %d\n"
            "worldlavalakespacing %d\n"
            "worldlavalakeshapefrequency %.9g\n"
            "worldlavalakeshapevariation %.9g\n",
            activeworldseed, worldgeologyfrequency, worldmaxcontinentheight, worldmaxoceandepth,
            worldoceancoverage, worldterraincoverage,
            worldtemperaturefrequency, worldmoisturefrequency,
            worldbiomevariationfrequency, worldbiomevariationstrength, worldrockfrequency,
            worldsealevel, worldsnowheight, worldstonelow, worldstonehigh,
            worldbiomeblend, worldcoastwidth, worldcoastvariation,
            worldbeachminheight, worldbeachmaxheight,
            worlddeserttemperature, worlddesertmoisture, worldforestmoisture,
            worldforesttreedensity, worldplainstreedensity,
            worldpinestartheight, worldpinefullheight,
            worldcavefrequency, worldcavethreshold, worldlargecavefrequency,
            worldlargecavethreshold, worldlargecavedeepthreshold,
            worldtunnelfrequency, worldtunnelwidth, worldcaveentrancewidth,
            worldcavemindepth, worldcavefulldepth, worldcavedeepheight,
            worldbottomlavalayers, worldlavalakestartheight, worldlavalakedeepheight,
            worldlavalakeshallowchance, worldlavalakedeepchance,
            worldlavalakeminsize, worldlavalakemaxsize, worldlavalakespacing,
            worldlavalakeshapefrequency, worldlavalakeshapevariation
        );
    }
}

ICOMMAND(worldloadseed, "i", (int *seed), game::loadworldseed(*seed));

#endif

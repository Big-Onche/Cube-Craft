#include "game.h"
#include "world.h"

#ifndef STANDALONE

VARP(worldseed, 0, 1337, INT_MAX);

FVAR(worldgeologyfrequency, 0.00001f, 0.0012f, 0.1f);
FVAR(worldmaxcontinentheight, 1.0f, 96.0f, 255.0f);
FVAR(worldmaxoceandepth, 1.0f, 32.0f, 255.0f);
FVAR(worldoceancoverage, 0.0f, 55.0f, 100.0f);
FVAR(worldterraincoverage, 0.0f, 45.0f, 100.0f);
FVAR(worldplainscoverage, 0.0f, 45.0f, 100.0f);
FVAR(worldhillscoverage, 0.0f, 32.0f, 100.0f);
FVAR(worldmountainscoverage, 0.0f, 18.0f, 100.0f);
FVAR(worldhighsummitscoverage, 0.0f, 5.0f, 100.0f);

FVAR(worldtectonicfrequency, 0.0001f, 0.0014f, 0.01f);
FVAR(worldtectonicwarpamplitude, 0.0f, 64.0f, 512.0f);
FVAR(worldtectonicridgepower, 0.1f, 2.2f, 8.0f);
FVAR(worldtectonicactivitythreshold, 0.0f, 0.35f, 0.95f);
FVAR(worldmaxlanduplift, 0.0f, 160.0f, 255.0f);
FVAR(worldmaxoceansubsidence, 0.0f, 100.0f, 255.0f);
FVAR(worldtectoniccavestrength, 0.0f, 0.35f, 1.0f);
FVAR(worldtectonicfracturestrength, 0.0f, 0.40f, 1.0f);
FVAR(worldcoastprotectionwidth, 0.0f, 32.0f, 256.0f);
FVAR(worldcliffchance, 0.0f, 24.0f, 100.0f);
FVAR(worldcliffmaxheight, 2.0f, 21.0f, 255.0f);

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
FVAR(worldgrassfrequency, 0.00001f, 0.02f, 1.0f);
FVAR(worldgrassdensity, 0.0f, 0.35f, 1.0f);
FVAR(worldgrassmaxoffset, 0.0f, 0.18f, 0.45f);
FVAR(worldflowerchance, 0.0f, 0.18f, 1.0f);
FVAR(worldroseweight, 0.0f, 1.0f, 100.0f);
FVAR(worldtulipweight, 0.0f, 1.0f, 100.0f);
FVAR(worlddandelionweight, 0.0f, 1.0f, 100.0f);
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

    static void setupnoise(FastNoiseLite &noise, int seed, float frequency, int octaves, float gain = 0.5f)
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

    static void setupwarp(FastNoiseLite &warp, int seed, float frequency, float amplitude)
    {
        warp.SetSeed(seed);
        warp.SetDomainWarpType(FastNoiseLite::DomainWarpType_OpenSimplex2);
        warp.SetFrequency(frequency);
        warp.SetDomainWarpAmp(amplitude);
    }

    worldsettings::worldsettings()
        : geologyfrequency(worldgeologyfrequency),
          maxcontinentheight(worldmaxcontinentheight), maxoceandepth(worldmaxoceandepth),
          oceancoverage(worldoceancoverage), terraincoverage(worldterraincoverage),
          plainscoverage(worldplainscoverage), hillscoverage(worldhillscoverage),
          mountainscoverage(worldmountainscoverage),
          highsummitscoverage(worldhighsummitscoverage),
          tectonicfrequency(worldtectonicfrequency),
          tectonicwarpamplitude(worldtectonicwarpamplitude),
          tectonicridgepower(worldtectonicridgepower),
          tectonicactivitythreshold(worldtectonicactivitythreshold),
          maxlanduplift(worldmaxlanduplift), maxoceansubsidence(worldmaxoceansubsidence),
          tectoniccavestrength(worldtectoniccavestrength),
          tectonicfracturestrength(worldtectonicfracturestrength),
          coastprotectionwidth(worldcoastprotectionwidth),
          cliffchance(worldcliffchance), cliffmaxheight(worldcliffmaxheight),
          temperaturefrequency(worldtemperaturefrequency),
          moisturefrequency(worldmoisturefrequency),
          biomevariationfrequency(worldbiomevariationfrequency),
          biomevariationstrength(worldbiomevariationstrength),
          rockfrequency(worldrockfrequency),
          deserttemperature(worlddeserttemperature), desertmoisture(worlddesertmoisture),
          forestmoisture(worldforestmoisture),
          foresttreedensity(worldforesttreedensity), plainstreedensity(worldplainstreedensity),
          grassfrequency(worldgrassfrequency), grassdensity(worldgrassdensity),
          grassmaxoffset(worldgrassmaxoffset),
          flowerchance(worldflowerchance), roseweight(worldroseweight),
          tulipweight(worldtulipweight), dandelionweight(worlddandelionweight),
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
        setupnoise(coastshape, seed ^ 0x57C8E219, settings.geologyfrequency * 8.0f, 1);
        setupnoise(covenoise, seed ^ 0x2B61D4A7, settings.geologyfrequency * 2.0f, 1);
        setupnoise(beachnoise, seed ^ 0x73A9C52D, settings.geologyfrequency * 4.0f, 1);
        setupnoise(cliffnoise, seed ^ 0x4E91A73B, settings.geologyfrequency * 3.0f, 1);
        // A slow field bounds each mountain range. Ridged medium-scale noise
        // builds the massif, while a faster field forms saddles and local peaks.
        setupnoise(mountainrange, seed ^ 0x18F47C53, settings.geologyfrequency * 1.5f, 1);
        setupnoise(mountainnoise, seed ^ 0x3D72A95B, settings.geologyfrequency * 2.5f, 2, 0.25f);
        setupnoise(mountainpeaks, seed ^ 0x25B46D81, settings.geologyfrequency * 4.5f, 1);
        setupnoise(tectonicnoise, seed ^ 0x68E31DA4, settings.tectonicfrequency, 1);
        setupwarp(tectonicwarp, seed ^ 0x6C8E9CF5, settings.tectonicfrequency * 0.5f, settings.tectonicwarpamplitude);
        setupnoise(temperature, seed ^ 0x51D7348B, settings.temperaturefrequency, 3);
        setupnoise(moisture, seed ^ 0x2F6E2B1D, settings.moisturefrequency, 3);
        setupnoise(biomevariation, seed ^ 0x749A7C15, settings.biomevariationfrequency, 3);
        setupnoise(biomeblend, seed ^ 0x13C6E91F, settings.biomeblend > 0 ? 1.0f / settings.biomeblend : 1.0f, 1);
        setupnoise(rockiness, seed ^ 0x5E4A19C3, settings.rockfrequency, 2);
        setupnoise(caves, seed ^ 0x7A84F12D, settings.cavefrequency, 2);
        setupnoise(largecaves, seed ^ 0x36B9C7E5, settings.largecavefrequency, 2);
        setupnoise(tunnela, seed ^ 0x19F3A6C7, settings.tunnelfrequency, 2);
        setupnoise(tunnelb, seed ^ 0x5C2D8E91, settings.tunnelfrequency, 2);
        setupnoise(lakeshape, seed ^ 0x43E7B5D9, settings.lavalakeshapefrequency, 2);
        setupnoise(fracturecorridors, seed ^ 0x278D4A6B,settings.tunnelfrequency * 0.35f, 1);
        setupnoise(fracturevertical, seed ^ 0x71B5C3D9, settings.tunnelfrequency, 1);
    }

    static float landthreshold(const worldsettings &settings)
    {
        const float coverage = settings.oceancoverage + settings.terraincoverage;
        const float oceanratio = coverage > 0.0f ? settings.oceancoverage / coverage : 0.5f;

        return oceanratio <= 0.0f ? -0.98f : oceanratio >= 1.0f ? 0.98f : oceanratio - 0.5f;
    }

    static float samplecontinental(const worldgenerator &generator, float noisex, float noisey)
    {
        const float base = generator.geology.GetNoise(noisex, noisey),
                    threshold = landthreshold(generator.settings),
                    amplitude = generator.settings.geologyfrequency * 48.0f,
                    coastband = max(amplitude * 3.0f, 0.08f),
                    coastweight = 1.0f - smoothstep(amplitude, coastband, fabs(base - threshold));

        return base + generator.covenoise.GetNoise(noisex, noisey) * amplitude * coastweight;
    }

    static void samplecoastprofile(const worldgenerator &generator, float noisex, float noisey, float &beachspan, float &plainrun, float &plainlevel)
    {
        const float beachshape = clamp(generator.beachnoise.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    grassshape = clamp(generator.coastshape.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f);

        // Broad beaches keep both sand terraces wide. Past them, ordinary coasts
        // settle into a low grass plain before returning to continental relief.
        beachspan = 1.0f + 7.0f * powf(beachshape, 3.0f);
        plainrun = 14.0f + 16.0f * (1.0f - powf(beachshape, 1.5f));
        plainlevel = 2.0f + grassshape;
    }

    static float samplecliffstrength(const worldgenerator &generator, float noisex, float noisey)
    {
        const float selector = clamp(generator.cliffnoise.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    chance = clamp(generator.settings.cliffchance * 0.01f, 0.0f, 1.0f),
                    center = 1.0f - chance;

        if(chance <= 0.0f) return 0.0f;
        if(chance >= 1.0f) return 1.0f;

        // Cliffs occur in coherent coastal sections, not as per-column accidents.
        // An eight-percent feather on each side keeps their boundaries gradual.
        return smoothstep(center - 0.08f, center + 0.08f, selector);
    }

    static worldtectonicsample sampletectonics(const worldgenerator &generator, int x, int y, float continental, float cavedepth)
    {
        const worldsettings &settings = generator.settings;
        const float threshold = landthreshold(settings),
                    landdensity = continental - threshold,
                    protection = max(0.02f, settings.coastprotectionwidth * settings.geologyfrequency * 0.75f);

        float tectonicx = x + 10000.5f, tectonicy = y - 10000.5f;
        generator.tectonicwarp.DomainWarp(tectonicx, tectonicy);

        const float ridge = powf(clamp(1.0f - fabs(generator.tectonicnoise.GetNoise(tectonicx, tectonicy)), 0.0f, 1.0f), max(settings.tectonicridgepower, 0.1f));

        worldtectonicsample sample;
        sample.activity = smoothstep(settings.tectonicactivitythreshold, min(settings.tectonicactivitythreshold + 0.35f, 1.0f), ridge);

        const float oceandistance = clamp(-landdensity / max(threshold + 1.0f, 0.001f), 0.0f, 1.0f),
                    oceanshelf = smoothstep(0.0f, 0.25f, oceandistance),
                    deepocean = smoothstep(0.15f, 0.85f, oceandistance),
                    normaloceandepth = settings.maxoceandepth * (0.25f * oceanshelf + 0.75f * deepocean),

                    // Give tall relief enough inland distance to fade before the
                    // protected coast instead of clipping a mountain into a wall.
                    landmask = smoothstep(protection, protection + 0.40f, landdensity),
                    oceandensitymask = smoothstep(protection, protection + 0.16f, -landdensity),
                    deepoceanmask = oceandensitymask * smoothstep(40.0f, 100.0f, normaloceandepth),
                    hill = clamp(generator.hills.GetNoise(x + 10000.5f, y - 10000.5f) * 0.5f + 0.5f, 0.0f, 1.0f),
                    rangenoise = generator.mountainrange.GetNoise(x + 10000.5f, y - 10000.5f),
                    ridgenoise = generator.mountainnoise.GetNoise(x + 10000.5f, y - 10000.5f),
                    peaknoise = generator.mountainpeaks.GetNoise(x + 10000.5f, y - 10000.5f),

                    // Tectonics amplify finite ranges rather than becoming terrain.
                    // Their broad base creates foothills and high valleys; intersecting
                    // ridges then form connected massifs, saddles, and sharp summits.
                    mountainactivity = smoothstep(settings.tectonicactivitythreshold, 1.0f, ridge),
                    tectonicproximity = smoothstep(max(settings.tectonicactivitythreshold * 0.20f, 0.02f), min(settings.tectonicactivitythreshold + 0.25f, 1.0f), ridge),
                    reliefcoverage = max(settings.plainscoverage + settings.hillscoverage + settings.mountainscoverage + settings.highsummitscoverage, 0.001f),
                    hillstart = settings.plainscoverage / reliefcoverage,
                    mountainstart = (settings.plainscoverage + settings.hillscoverage) / reliefcoverage,
                    summitshare = settings.highsummitscoverage / reliefcoverage,

                    // OpenSimplex has a narrow upper tail. Expanding only the summit
                    // share keeps the percentage control responsive without broadening
                    // ordinary mountain coverage.
                    summitstart = max(mountainstart, 1.0f - powf(summitshare, 0.80f)),

                    // The independent range field spans the full selector even in calm
                    // regions, so tectonics is never a prerequisite for mountains.
                    // Proximity and activity only bias the result upward; nested bands
                    // guarantee that every summit remains inside mountains and hills.
                    basereliefselector = clamp(0.5f + 0.5f * erff(rangenoise / 0.70f), 0.0f, 1.0f),
                    tectonicbias = clamp(0.20f * tectonicproximity + 0.10f * mountainactivity, 0.0f, 0.30f),
                    reliefselector = basereliefselector + (1.0f - basereliefselector) * tectonicbias,
                    coveragefade = 0.055f,
                    hillregion = settings.hillscoverage + settings.mountainscoverage + settings.highsummitscoverage > 0.0f ? smoothstep(hillstart - coveragefade, hillstart + coveragefade, reliefselector) : 0.0f,
                    mountainregion = settings.mountainscoverage + settings.highsummitscoverage > 0.0f ? smoothstep(mountainstart - coveragefade, mountainstart + coveragefade, reliefselector) : 0.0f,
                    summitregion = settings.highsummitscoverage > 0.0f ? smoothstep(summitstart - coveragefade, summitstart + coveragefade, reliefselector) : 0.0f,
                    foothillzone = hillregion * (0.65f + 0.20f * tectonicproximity + 0.15f * mountainactivity),
                    rangezone = mountainregion * (0.70f + 0.15f * tectonicproximity + 0.15f * mountainactivity),
                    summitzone = summitregion * (0.65f + 0.10f * tectonicproximity + 0.25f * mountainactivity),
                    primaryridge = powf(clamp(1.10f - sqrtf(ridgenoise * ridgenoise + 0.01f), 0.0f, 1.0f), 2.0f),
                    secondaryridge = powf(clamp(1.12f - sqrtf(peaknoise * peaknoise + 0.0144f), 0.0f, 1.0f), 2.4f),
                    plainhillshape = smoothstep(0.48f, 0.78f, hill),
                    backgroundrelief = 0.025f * plainhillshape * (1.0f - 0.80f * foothillzone),
                    highplateau = 0.22f * foothillzone * (0.85f + 0.15f * hill),
                    mainridges = 0.40f * rangezone * primaryridge * (0.58f + 0.42f * secondaryridge),
                    surroundingpeaks = 0.14f * rangezone * powf(secondaryridge, 1.3f) * (0.35f + 0.65f * primaryridge),
                    localsummits = 0.21f * summitzone * powf(primaryridge, 1.4f) * powf(secondaryridge, 1.2f),
                    trenchpotential = sample.activity * deepoceanmask;

        sample.landuplift = clamp(landmask * (backgroundrelief + highplateau+ mainridges + surroundingpeaks + localsummits), 0.0f, 1.0f);
        sample.oceantrench = clamp(trenchpotential * powf(sample.activity, 0.35f), 0.0f, 1.0f);

        const float protecteddepth = max(float(settings.cavemindepth), 12.0f),
                    fulldepth = max(float(settings.cavefulldepth), 20.0f),
                    depthmask = smoothstep(protecteddepth, max(fulldepth, protecteddepth + 1.0f), cavedepth),
                    foundationprotection = 1.0f - sample.landuplift * 0.70f;

        sample.caveexpansion = clamp(sample.activity * depthmask * foundationprotection * settings.tectoniccavestrength, 0.0f, 1.0f);

        return sample;
    }

    worldtectonicsample worldgenerator::tectonics(int x, int y, float cavedepth) const
    {
        const float continental = samplecontinental(*this, x + 10000.5f, y - 10000.5f);
        return sampletectonics(*this, x, y, continental, cavedepth);
    }

    float worldgenerator::coasttransitionwidth(int x, int y) const
    {
        float beachspan, plainrun, plainlevel;
        samplecoastprofile(*this, x + 10000.5f, y - 10000.5f, beachspan, plainrun, plainlevel);

        return 2.0f * beachspan + plainrun + 14.0f;
    }

    float worldgenerator::maxcoasttransitionwidth() const
    {
        return 60.0f;
    }

    float worldgenerator::fracturecorridor(int x, int y) const
    {
        return fabs(fracturecorridors.GetNoise(x + 24500.5f, y - 24500.5f));
    }

    int worldgenerator::height(int x, int y, worldtectonicsample *tectonics) const
    {
        const float noisex = x + 10000.5f, noisey = y - 10000.5f;
        const float continental = samplecontinental(*this, noisex, noisey);
        const float threshold = landthreshold(settings);
        const worldtectonicsample tectonicsample = sampletectonics(*this, x, y, continental, 0);
        if(tectonics) *tectonics = tectonicsample;
        float elevation;
        if(continental >= threshold)
        {
            const float distance = clamp((continental - threshold) / max(1.0f - threshold, 0.001f), 0.0f, 1.0f);
            const float coastrise = smoothstep(0.0f, 0.28f, distance);
            const float inland = smoothstep(0.0f, 0.72f, distance);
            const float hill = clamp(hills.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f);
            elevation = settings.maxcontinentheight * coastrise * (0.55f + 0.30f * inland + 0.15f * hill);
            const float continentalelevation = elevation;

            // Build a deliberate beach cross-section near the continental edge.
            // A local gradient converts continental density into approximate metres
            // inland, keeping the profile deterministic and continuous across chunks.
            const float coastprofilelimit = max(16.0f, min(settings.cliffmaxheight, settings.maxcontinentheight));
            if(elevation < coastprofilelimit)
            {
                const float gradientstep = 2.0f,
                            gradientx = (samplecontinental(*this, noisex + gradientstep, noisey)
                                       - samplecontinental(*this, noisex - gradientstep, noisey))
                                      / (2.0f * gradientstep),
                            gradienty = (samplecontinental(*this, noisex, noisey + gradientstep)
                                       - samplecontinental(*this, noisex, noisey - gradientstep))
                                      / (2.0f * gradientstep),
                            gradient = max(sqrtf(gradientx * gradientx + gradienty * gradienty), settings.geologyfrequency * 0.35f),
                            shoredistance = max((continental - threshold) / gradient, 0.0f);

                float beachspan, plainrun, plainlevel;
                samplecoastprofile(*this, noisex, noisey, beachspan, plainrun, plainlevel);

                const float cliffstrength = samplecliffstrength(*this, noisex, noisey),
                            // Cliff sections progressively consume the beach. At full
                            // strength the first land column can already be exposed rock.
                            effectivebeachspan = beachspan * (1.0f - cliffstrength),
                            beachend = 2.0f * effectivebeachspan,
                            sandstepratio = 0.5f + clamp(coastshape.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f) / 6.0f,
                            sandstepstart = beachend * sandstepratio,
                            grassriseend = beachend + min(8.0f, plainrun * 0.5f),
                            plainend = beachend + plainrun,
                            blendend = plainend + 14.0f;

                float normalelevation;
                if(effectivebeachspan > 0.01f && shoredistance < sandstepstart) normalelevation = 0.0f;
                else if(effectivebeachspan > 0.01f && shoredistance < beachend) normalelevation = 1.0f;
                // The first grass column is always level 2 on ordinary coasts.
                // Its slow rise prevents a rounded level-3 plain from skipping
                // an entire vertical cube immediately after the sand.
                else if(shoredistance < grassriseend) normalelevation = 2.0f + (plainlevel - 2.0f) * smoothstep(beachend, grassriseend, shoredistance);
                else if(shoredistance < plainend) normalelevation = plainlevel;
                else normalelevation = plainlevel + (continentalelevation - plainlevel) * smoothstep(plainend, blendend, shoredistance);

                // High original relief is allowed to return sooner, while ordinary
                // shores retain the deliberately broad 2–3 metre grass plain.
                const float reliefpermission = smoothstep(9.0f, 16.0f, continentalelevation);
                normalelevation += (max(continentalelevation, normalelevation) - normalelevation) * reliefpermission;

                const float cliffshape = clamp(coastshape.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                            // Keep the current 7–16 metre distribution at the
                            // default while making the configured value a hard cap.
                            cliffheight = settings.cliffmaxheight * (0.4375f + 0.5625f * cliffshape),
                            cliffrise = smoothstep(-0.75f, 2.0f, shoredistance),
                            // Hold a genuine plateau behind the rock face, then
                            // blend it into the continental surface. Unlike the old
                            // fade-to-zero profile, this cannot dig a trough between
                            // the cliff and the inland terrain.
                            cliffplateauend = max(plainend, 22.0f),
                            cliffblendend = cliffplateauend + 32.0f,
                            inlandtarget = max(continentalelevation, normalelevation),
                            cliffblend = smoothstep(cliffplateauend, cliffblendend, shoredistance),
                            cliffplateau = cliffheight + (inlandtarget - cliffheight) * cliffblend,
                            cliffelevation = max(inlandtarget, cliffplateau * cliffrise);

                elevation = normalelevation+ (cliffelevation - normalelevation) * cliffstrength;
            }
            elevation = clamp(elevation, 0.0f, settings.maxcontinentheight)+ settings.maxlanduplift * tectonicsample.landuplift;
        }
        else
        {
            const float distance = clamp((threshold - continental) / max(threshold + 1.0f, 0.001f), 0.0f, 1.0f);
            const float shelf = smoothstep(0.0f, 0.25f, distance);
            const float deepocean = smoothstep(0.15f, 0.85f, distance);
            elevation = -settings.maxoceandepth * (0.25f * shelf + 0.75f * deepocean);
            elevation = clamp(elevation, -settings.maxoceandepth, 0.0f) - settings.maxoceansubsidence * tectonicsample.oceantrench;
        }
        return clamp(int(floor(settings.sealevel + elevation + 0.5f)), -255, 255);
    }

    int worldgenerator::biome(int x, int y, int height) const
    {
        if(height < settings.sealevel) return WORLD_BIOME_OCEAN;

        const float noisex = x + 10000.5f, noisey = y - 10000.5f;
        const float variation = biomevariation.GetNoise(noisex, noisey);
        const float temperaturevalue = temperature.GetNoise(noisex, noisey) + variation * settings.biomevariationstrength;
        const float moisturevalue = clamp(moisture.GetNoise(noisex, noisey) - variation * settings.biomevariationstrength, -1.0f, 1.0f);

        if(settings.biomeblend <= 0)
        {
            if(height > settings.snowheight) return WORLD_BIOME_SNOW;
            if(temperaturevalue > settings.deserttemperature && moisturevalue < settings.desertmoisture) return WORLD_BIOME_DESERT;
            if(moisturevalue > settings.forestmoisture) return WORLD_BIOME_FOREST;
            return WORLD_BIOME_PLAINS;
        }

        const float blendblocks = settings.biomeblend;
        const float temperatureblend = max(blendblocks * settings.temperaturefrequency * 2.0f, 0.001f);
        const float moistureblend = max(blendblocks * settings.moisturefrequency * 2.0f, 0.001f);
        const float selector = clamp((biomeblend.GetNoise(noisex, noisey) + 1.0f) * 0.5f, 0.0f, 1.0f);
        const float snowweight = smoothstep(settings.snowheight - blendblocks * 0.5f, settings.snowheight + blendblocks * 0.5f, height);
        const float hotweight = smoothstep(settings.deserttemperature - temperatureblend, settings.deserttemperature + temperatureblend, temperaturevalue);
        const float dryweight = 1.0f - smoothstep(settings.desertmoisture - moistureblend, settings.desertmoisture + moistureblend, moisturevalue);
        const float forestweight = smoothstep(settings.forestmoisture - moistureblend, settings.forestmoisture + moistureblend, moisturevalue);
        if(snowweight > selector) return WORLD_BIOME_SNOW;
        if(hotweight * dryweight > selector) return WORLD_BIOME_DESERT;
        if(forestweight > selector) return WORLD_BIOME_FOREST;
        return WORLD_BIOME_PLAINS;
    }

    bool worldgenerator::cliff(int x, int y, int height) const
    {
        const float noisex = x + 10000.5f, noisey = y - 10000.5f,continental = samplecontinental(*this, noisex, noisey), threshold = landthreshold(settings), cliffstrength = samplecliffstrength(*this, noisex, noisey);
        if(continental >= threshold && height >= settings.sealevel + 2&& cliffstrength > 0.25f)
        {
            const float gradientstep = 2.0f,
                        gradientx = (samplecontinental(*this, noisex + gradientstep, noisey)
                                   - samplecontinental(*this, noisex - gradientstep, noisey))
                                  / (2.0f * gradientstep),
                        gradienty = (samplecontinental(*this, noisex, noisey + gradientstep)
                                   - samplecontinental(*this, noisex, noisey - gradientstep))
                                  / (2.0f * gradientstep),
                        gradient = max(sqrtf(gradientx * gradientx + gradienty * gradienty),
                                       settings.geologyfrequency * 0.35f),
                        shoredistance = (continental - threshold) / gradient,
                        // The geometric cliff reaches its crest after roughly
                        // three metres. Do not extend its stone material across
                        // the much wider, flat inland plateau.
                        facewidth = 2.5f + 0.5f * cliffstrength;

            if(shoredistance <= facewidth) return true;
        }
        return false;
    }

    bool worldgenerator::rock(int x, int y, int height) const
    {
        const float low = min(settings.stonelow, settings.stonehigh);
        const float high = max(settings.stonelow, settings.stonehigh);
        if(height <= low) return false;
        if(height >= high) return true;

        const float rockweight = smoothstep(low, high, height);
        const float selector = clamp(rockiness.GetNoise(x + 10000.5f, y - 10000.5f) * 1.25f + 0.5f, 0.0f, 1.0f);
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
            "worldplainscoverage %.9g\n"
            "worldhillscoverage %.9g\n"
            "worldmountainscoverage %.9g\n"
            "worldhighsummitscoverage %.9g\n"
            "worldtectonicfrequency %.9g\n"
            "worldtectonicwarpamplitude %.9g\n"
            "worldtectonicridgepower %.9g\n"
            "worldtectonicactivitythreshold %.9g\n"
            "worldmaxlanduplift %.9g\n"
            "worldmaxoceansubsidence %.9g\n"
            "worldtectoniccavestrength %.9g\n"
            "worldtectonicfracturestrength %.9g\n"
            "worldcoastprotectionwidth %.9g\n"
            "worldcliffchance %.9g\n"
            "worldcliffmaxheight %.9g\n"
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
            "worldgrassfrequency %.9g\n"
            "worldgrassdensity %.9g\n"
            "worldgrassmaxoffset %.9g\n"
            "worldflowerchance %.9g\n"
            "worldroseweight %.9g\n"
            "worldtulipweight %.9g\n"
            "worlddandelionweight %.9g\n"
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
            worldplainscoverage, worldhillscoverage,
            worldmountainscoverage, worldhighsummitscoverage,
            worldtectonicfrequency, worldtectonicwarpamplitude, worldtectonicridgepower,
            worldtectonicactivitythreshold, worldmaxlanduplift, worldmaxoceansubsidence,
            worldtectoniccavestrength, worldtectonicfracturestrength, worldcoastprotectionwidth,
            worldcliffchance, worldcliffmaxheight,
            worldtemperaturefrequency, worldmoisturefrequency,
            worldbiomevariationfrequency, worldbiomevariationstrength, worldrockfrequency,
            worldsealevel, worldsnowheight, worldstonelow, worldstonehigh,
            worldbiomeblend, worldcoastwidth, worldcoastvariation,
            worldbeachminheight, worldbeachmaxheight,
            worlddeserttemperature, worlddesertmoisture, worldforestmoisture,
            worldforesttreedensity, worldplainstreedensity,
            worldgrassfrequency, worldgrassdensity, worldgrassmaxoffset,
            worldflowerchance, worldroseweight, worldtulipweight,
            worlddandelionweight,
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

#include "game.h"

#ifndef STANDALONE

extern bvec ambient, fogcolour, skylight, sunlight;
extern float skylightscale, sunlightscale;
extern float sunlightyaw, sunlightpitch;
extern void setsunlightdir();

namespace game
{
    namespace environment
    {
        static const int DAY_MILLIS = 10 * 60 * 1000;
        static const int NIGHT_MILLIS = 10 * 60 * 1000;
        static const int CYCLE_MILLIS = DAY_MILLIS + NIGHT_MILLIS;
        static const float START_HOUR = 8.0f;
        static const float MAX_SUN_PITCH = 70.0f;
        static const int DAY_FOG_COLOR = 0x8099B3;
        static const int DAY_AMBIENT_COLOR = 0x5A5A6E;
        static const int NIGHT_FOG_COLOR = 0x0A1026;
        static const int NIGHT_AMBIENT_COLOR = 0x080C20;

        struct lightingkey
        {
            float hour;
            int sunlightcolor, skylightcolor, fogcolor, ambientcolor;
            float sunlightintensity, skylightintensity;
        };

        static const lightingkey lightingkeys[] =
        {
            {  0.0f, 0x8090C0, 0x101830, NIGHT_FOG_COLOR, NIGHT_AMBIENT_COLOR, 0.06f, 0.16f },
            {  5.0f, 0x7180B0, 0x182342, 0x151E38, 0x10172D, 0.05f, 0.20f },
            {  6.0f, 0xFF6A3D, 0x6C6F8F, 0x6A5062, 0x302A40, 0.30f, 0.48f },
            {  7.0f, 0xFFC080, 0x98B5D0, 0x887A82, 0x4B4658, 0.75f, 0.78f },
            {  8.0f, 0xFFF8E0, 0xBEE1FF, DAY_FOG_COLOR, DAY_AMBIENT_COLOR, 1.00f, 1.00f },
            { 16.0f, 0xFFF8E0, 0xBEE1FF, DAY_FOG_COLOR, DAY_AMBIENT_COLOR, 1.00f, 1.00f },
            { 17.0f, 0xFFC080, 0x98B5D0, 0x887A82, 0x4B4658, 0.75f, 0.78f },
            { 18.0f, 0xFF6238, 0x696985, 0x704858, 0x30243A, 0.28f, 0.45f },
            { 19.0f, 0x7180B0, 0x182342, 0x151E38, 0x10172D, 0.05f, 0.20f },
            { 24.0f, 0x8090C0, 0x101830, NIGHT_FOG_COLOR, NIGHT_AMBIENT_COLOR, 0.06f, 0.16f }
        };

        static double cyclemillis = START_HOUR * CYCLE_MILLIS / 24.0;
        static bool initialized = false, timefrozen = false;

        static float smoothstep(float value)
        {
            value = clamp(value, 0.0f, 1.0f);
            return value * value * (3.0f - 2.0f * value);
        }

        static float interpolate(float from, float to, float amount)
        {
            return from + (to - from) * amount;
        }

        static bvec interpolatecolor(int from, int to, float amount)
        {
            bvec result;
            result.lerp(bvec::hexcolor(from), bvec::hexcolor(to), amount);
            return result;
        }

        static void applylighting(bool resetengine)
        {
            const float hour = float(cyclemillis * 24.0 / CYCLE_MILLIS);
            const lightingkey *from = &lightingkeys[0], *to = &lightingkeys[1];
            loopi(int(sizeof(lightingkeys) / sizeof(lightingkeys[0])) - 1)
            {
                if(hour <= lightingkeys[i + 1].hour)
                {
                    from = &lightingkeys[i];
                    to = &lightingkeys[i + 1];
                    break;
                }
            }

            const float blend = smoothstep((hour - from->hour) / (to->hour - from->hour));
            const bvec newSunlight = interpolatecolor(from->sunlightcolor, to->sunlightcolor, blend);
            const bvec newSkylight = interpolatecolor(from->skylightcolor, to->skylightcolor, blend);
            const bvec newFog = interpolatecolor(from->fogcolor, to->fogcolor, blend);
            const bvec newAmbient = interpolatecolor(from->ambientcolor, to->ambientcolor, blend);
            const float newSunlightScale = interpolate(from->sunlightintensity, to->sunlightintensity, blend);
            const float newSkylightScale = interpolate(from->skylightintensity, to->skylightintensity, blend);

            const float orbit = (hour - 6.0f) * 15.0f * RAD;
            float newSunlightYaw = hour * (360.0f / 24.0f);
            if(newSunlightYaw >= 360.0f) newSunlightYaw -= 360.0f;
            const float newSunlightPitch = sinf(orbit) * MAX_SUN_PITCH;

            if(resetengine)
            {
                setvar("sunlight", newSunlight.tohexcolor());
                setvar("skylight", newSkylight.tohexcolor());
                setvar("fogcolour", newFog.tohexcolor());
                setvar("ambient", newAmbient.tohexcolor());
                setfvar("sunlightscale", newSunlightScale);
                setfvar("skylightscale", newSkylightScale);
                setfvar("sunlightyaw", newSunlightYaw);
                setfvar("sunlightpitch", newSunlightPitch);
                return;
            }

            sunlight = newSunlight;
            skylight = newSkylight;
            fogcolour = newFog;
            ambient = newAmbient;
            sunlightscale = newSunlightScale;
            skylightscale = newSkylightScale;
            sunlightyaw = newSunlightYaw;
            sunlightpitch = newSunlightPitch;
            setsunlightdir();
        }

        void reset()
        {
            cyclemillis = START_HOUR * CYCLE_MILLIS / 24.0;
            initialized = true;
            timefrozen = false;
            applylighting(true);
        }

        void update()
        {
            if(!initialized) reset();
            if(timefrozen || curtime <= 0) return;
            cyclemillis += curtime;
            while(cyclemillis >= CYCLE_MILLIS) cyclemillis -= CYCLE_MILLIS;
            applylighting(false);
        }

        ICOMMAND(time, "sN", (char *value, int *numargs),
        {
            if(*numargs == 1 && cubecaseequal(value, "freeze"))
            {
                const double hour = cyclemillis * 24.0 / CYCLE_MILLIS;
                const int minutes = int(hour * 60.0 + 0.5) % (24 * 60);
                timefrozen = true;
                conoutf("time frozen at %02d:%02d", minutes / 60, minutes % 60);
                return;
            }

            char *end = NULL;
            const double hour = *numargs == 1 ? strtod(value, &end) : -1;
            if(*numargs != 1 || end == value || *end || !(hour >= 0 && hour <= 24))
            {
                conoutf(CON_ERROR, "usage: /time <hour 0-24|freeze>");
                return;
            }

            const double normalizedhour = hour == 24 ? 0 : hour;
            cyclemillis = normalizedhour * CYCLE_MILLIS / 24.0;
            initialized = true;
            timefrozen = false;
            applylighting(true);
            const int minutes = int(normalizedhour * 60.0 + 0.5) % (24 * 60);
            conoutf("time set to %02d:%02d", minutes / 60, minutes % 60);
        });
    }
}

#endif

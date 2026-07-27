#include "game.h"

namespace game
{
    enum
    {
        PART_TORSO = 0,
        PART_HEAD,
        PART_LEFT_ARM,
        PART_RIGHT_ARM,
        PART_LEFT_LEG,
        PART_RIGHT_LEG,
        NUM_PLAYER_PARTS
    };

    static const char * const playermodels[NUM_PLAYER_PARTS] =
    {
        "game/player/torso",
        "game/player/head",
        "game/player/arm/left",
        "game/player/arm/right",
        "game/player/leg/left",
        "game/player/leg/right"
    };

    // The split meshes retain the coordinates of the original 30-unit model.
    // Their configs recenter articulated pieces on these joint heights.
    static const float HIP_HEIGHT = 11.25f, SHOULDER_HEIGHT = 22.5f;
    static const float RUN_CYCLE_SPEED = 9.0f, LEG_SWING = 32.0f, ARM_SWING = 28.0f;

    void preloadplayermodels()
    {
        loopi(NUM_PLAYER_PARTS) preloadmodel(playermodels[i]);
    }

    static void renderpart(gameent *d, int part, const vec &origin, float pitch, int flags)
    {
        rendermodel(playermodels[part], ANIM_MAPMODEL | ANIM_LOOP,
                    origin, d->yaw, pitch, 0, flags, d);
    }

    static float movementamount(const gameent *d)
    {
        float speed = sqrtf(d->vel.x*d->vel.x + d->vel.y*d->vel.y);
        float amount = clamp(speed / max(d->maxspeed, 1.0f), 0.0f, 1.0f);

        // Start the pose as soon as input begins, before physics has accelerated.
        if(d->move || d->strafe) amount = max(amount, 0.25f);
        return amount;
    }

    static void renderplayer(gameent *d, bool local)
    {
        if(!d || d->state == CS_SPECTATOR || (!local && d->smoothmillis < 0)) return;

        int flags = MDL_CULL_VFC | MDL_CULL_DIST | MDL_CULL_OCCLUDED;
        if(local && !isthirdperson()) flags |= MDL_ONLYSHADOW;

        float movement = movementamount(d);
        float phase = lastmillis * (RUN_CYCLE_SPEED / 1000.0f)
                    + max(d->clientnum, 0) * 1.37f;
        float stride = sinf(phase) * movement;
        float bob = fabsf(cosf(phase)) * 0.45f * movement;

        vec feet = d->feetpos(bob);
        vec hips = vec(feet).addz(HIP_HEIGHT);
        vec shoulders = vec(feet).addz(SHOULDER_HEIGHT);

        renderpart(d, PART_TORSO, feet, 0, flags);
        renderpart(d, PART_HEAD, shoulders, sinf(phase * 2.0f) * 1.5f * movement, flags);
        renderpart(d, PART_LEFT_ARM, shoulders, -stride * ARM_SWING, flags);
        renderpart(d, PART_RIGHT_ARM, shoulders, stride * ARM_SWING, flags);
        renderpart(d, PART_LEFT_LEG, hips, stride * LEG_SWING, flags);
        renderpart(d, PART_RIGHT_LEG, hips, -stride * LEG_SWING, flags);
    }

    void rendergame()
    {
        entities::renderentities();
        loopv(players) renderplayer(players[i], players[i] == player1);
    }

    void renderavatar() {}
    void renderplayerpreview(int model, int color, int team, int weap) {}
}

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
    static const float IDLE_HEAD_YAW = 65.0f, IDLE_BODY_TURN_SPEED = 180.0f;

    void preloadplayermodels()
    {
        loopi(NUM_PLAYER_PARTS) preloadmodel(playermodels[i]);
    }

    static void renderpart(gameent *d, int part, const vec &origin, float yaw, float pitch, int flags)
    {
        rendermodel(playermodels[part], ANIM_MAPMODEL | ANIM_LOOP, origin, yaw, pitch, 0, flags, d);
    }

    static float movementamount(const gameent *d)
    {
        float speed = sqrtf(d->vel.x*d->vel.x + d->vel.y*d->vel.y);
        float amount = clamp(speed / max(d->maxspeed, 1.0f), 0.0f, 1.0f);

        // Start the pose as soon as input begins, before physics has accelerated.
        if(d->move || d->strafe) amount = max(amount, 0.25f);
        return amount;
    }

    static float yawoffset(float yaw, float reference)
    {
        float offset = fmodf(yaw - reference, 360.0f);
        if(offset > 180.0f) offset -= 360.0f;
        else if(offset < -180.0f) offset += 360.0f;
        return offset;
    }

    static float normalizeyaw(float yaw)
    {
        yaw = fmodf(yaw, 360.0f);
        return yaw < 0 ? yaw + 360.0f : yaw;
    }

    static float headyawlimit(float movement)
    {
        return movement > 0.05f ? 0.0f : IDLE_HEAD_YAW;
    }

    static float updatebodyyaw(gameent *d, float movement)
    {
        if(d->renderbodyyawmillis < 0 || lastmillis < d->renderbodyyawmillis)
        {
            d->renderbodyyaw = normalizeyaw(d->yaw);
            d->renderbodyyawmillis = lastmillis;
            return d->renderbodyyaw;
        }

        int elapsed = min(lastmillis - d->renderbodyyawmillis, 100);
        d->renderbodyyawmillis = lastmillis;

        // Movement always faces the whole body in the player's direction.
        if(movement > 0.05f)
        {
            d->renderbodyyaw = normalizeyaw(d->yaw);
            return d->renderbodyyaw;
        }

        float headlimit = headyawlimit(movement);
        float offset = yawoffset(d->yaw, d->renderbodyyaw);
        if(fabsf(offset) > headlimit)
        {
            float target = d->yaw - (offset < 0 ? -headlimit : headlimit);
            float turn = yawoffset(target, d->renderbodyyaw);
            float maxturn = IDLE_BODY_TURN_SPEED * elapsed / 1000.0f;
            d->renderbodyyaw = normalizeyaw(d->renderbodyyaw + clamp(turn, -maxturn, maxturn));
        }

        return d->renderbodyyaw;
    }

    static void renderplayer(gameent *d, bool local)
    {
        if(!d || d->state == CS_SPECTATOR || (!local && d->smoothmillis < 0)) return;

        int flags = MDL_CULL_VFC | MDL_CULL_DIST | MDL_CULL_OCCLUDED;
        if(local && !isthirdperson()) flags |= MDL_ONLYSHADOW;

        float movement = movementamount(d);
        float phase = lastmillis * (RUN_CYCLE_SPEED / 1000.0f) + max(d->clientnum, 0) * 1.37f;
        float stride = sinf(phase) * movement;
        float bob = fabsf(cosf(phase)) * 0.45f * movement;
        float bodyyaw = updatebodyyaw(d, movement);
        float headlimit = headyawlimit(movement);
        float headyaw = normalizeyaw(bodyyaw + clamp(yawoffset(d->yaw, bodyyaw), -headlimit, headlimit));

        vec feet = d->feetpos(bob);
        vec hips = vec(feet).addz(HIP_HEIGHT);
        vec shoulders = vec(feet).addz(SHOULDER_HEIGHT);

        renderpart(d, PART_TORSO, feet, bodyyaw, 0, flags);
        renderpart(d, PART_HEAD, shoulders, headyaw, clamp(d->pitch, -80.0f, 80.0f) + sinf(phase * 2.0f) * 1.5f * movement, flags);
        renderpart(d, PART_LEFT_ARM, shoulders, bodyyaw, -stride * ARM_SWING, flags);
        renderpart(d, PART_RIGHT_ARM, shoulders, bodyyaw, stride * ARM_SWING, flags);
        renderpart(d, PART_LEFT_LEG, hips, bodyyaw, stride * LEG_SWING, flags);
        renderpart(d, PART_RIGHT_LEG, hips, bodyyaw, -stride * LEG_SWING, flags);
    }

    void rendergame()
    {
        entities::renderentities();
        loopv(players) renderplayer(players[i], players[i] == player1);
    }

    void renderavatar() {}
    void renderplayerpreview(int model, int color, int team, int weap) {}
}

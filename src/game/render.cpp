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
    static const float MIN_GAIT_CADENCE = 0.65f, MAX_GAIT_CADENCE = 3.0f;
    static const float LEG_SWING = 32.0f, ARM_SWING = 28.0f;
    static const float IDLE_HEAD_YAW = 65.0f, IDLE_BODY_TURN_SPEED = 180.0f;
    static const float CROUCH_HIP_DROP = 2.0f, CROUCH_HEAD_DROP = 0.75f;
    static const float CROUCH_TORSO_PITCH = -35.0f, CROUCH_ARM_PITCH = -15.0f,
                       CROUCH_LEG_PITCH = 35.0f;

    void preloadplayermodels()
    {
        loopi(NUM_PLAYER_PARTS) preloadmodel(playermodels[i]);
    }

    static void renderpart(gameent *d, int part, const vec &origin, float yaw, float pitch, int flags)
    {
        rendermodel(playermodels[part], ANIM_MAPMODEL | ANIM_LOOP, origin, yaw, pitch, 0, flags, d);
    }

    static float movementamount(const gameent *d, float speed)
    {
        float maxspeed = max(d->maxspeed / GAMEUNITSPERMETER, 0.01f);
        return sqrtf(clamp(speed / maxspeed, 0.0f, 1.0f));
    }

    static float updatestridephase(gameent *d, float speed)
    {
        if(d->renderstridemillis < 0 || lastmillis < d->renderstridemillis)
        {
            d->renderstridephase = fmodf(max(d->clientnum, 0) * 1.37f, 2.0f * PI);
            d->renderstridemillis = lastmillis;
            return d->renderstridephase;
        }

        int elapsed = min(lastmillis - d->renderstridemillis, 100);
        d->renderstridemillis = lastmillis;
        if(speed > 0.05f)
        {
            float maxspeed = max(d->maxspeed / GAMEUNITSPERMETER, 0.01f);
            // Cadence is cycles per second: walking stays deliberate while
            // sprinting approaches a natural three steps per second.
            float cadence = clamp(MIN_GAIT_CADENCE + sqrtf(speed / maxspeed),
                                  MIN_GAIT_CADENCE, MAX_GAIT_CADENCE);
            d->renderstridephase = fmodf(d->renderstridephase
                                       + 2.0f * PI * cadence * elapsed / 1000.0f,
                                         2.0f * PI);
        }
        return d->renderstridephase;
    }

    static float physicalcrouchamount(const gameent *d)
    {
        float range = d->maxheight * (1.0f - CROUCHHEIGHT);
        return range > 0 ? clamp((d->maxheight - d->eyeheight) / range, 0.0f, 1.0f) : 0.0f;
    }

    static float updatecrouch(gameent *d, bool local)
    {
        if(local)
        {
            d->rendercrouch = physicalcrouchamount(d);
            d->rendercrouchmillis = lastmillis;
            return d->rendercrouch;
        }

        float target = d->crouching ? 1.0f : 0.0f;
        if(d->rendercrouchmillis < 0 || lastmillis < d->rendercrouchmillis)
        {
            d->rendercrouch = target;
            d->rendercrouchmillis = lastmillis;
            return d->rendercrouch;
        }

        int elapsed = min(lastmillis - d->rendercrouchmillis, 100);
        d->rendercrouchmillis = lastmillis;
        float step = elapsed / float(CROUCHTIME);
        if(d->rendercrouch < target) d->rendercrouch = min(d->rendercrouch + step, target);
        else if(d->rendercrouch > target) d->rendercrouch = max(d->rendercrouch - step, target);
        return d->rendercrouch;
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

        float speed = horizontalmeterspersecond(d);
        float movement = movementamount(d, speed);
        float crouch = updatecrouch(d, local);
        float phase = updatestridephase(d, speed);
        float stride = sinf(phase) * movement * (1.0f - 0.55f * crouch);
        float bob = fabsf(cosf(phase)) * 0.45f * movement * (1.0f - 0.65f * crouch);
        float bodyyaw = updatebodyyaw(d, movement);
        float headlimit = headyawlimit(movement);
        float headyaw = normalizeyaw(bodyyaw + clamp(yawoffset(d->yaw, bodyyaw), -headlimit, headlimit));
        float torsopitch = CROUCH_TORSO_PITCH * crouch;
        float armpitch = CROUCH_ARM_PITCH * crouch;
        float legpitch = CROUCH_LEG_PITCH * crouch;

        vec feet = d->feetpos(bob);
        vec hips = vec(feet).addz(HIP_HEIGHT - CROUCH_HIP_DROP * crouch);
        vec shoulderoffset(0, 0, SHOULDER_HEIGHT - HIP_HEIGHT);
        shoulderoffset.rotate_around_x(torsopitch * RAD).rotate_around_z(bodyyaw * RAD);
        vec shoulders = vec(hips).add(shoulderoffset);
        vec neck = vec(shoulders).addz(-CROUCH_HEAD_DROP * crouch);

        renderpart(d, PART_TORSO, hips, bodyyaw, torsopitch, flags);
        renderpart(d, PART_HEAD, neck, headyaw, clamp(d->pitch, -80.0f, 80.0f) + sinf(phase * 2.0f) * 1.5f * movement, flags);
        renderpart(d, PART_LEFT_ARM, shoulders, bodyyaw, armpitch - stride * ARM_SWING, flags);
        renderpart(d, PART_RIGHT_ARM, shoulders, bodyyaw, armpitch + stride * ARM_SWING, flags);
        renderpart(d, PART_LEFT_LEG, hips, bodyyaw, legpitch + stride * LEG_SWING, flags);
        renderpart(d, PART_RIGHT_LEG, hips, bodyyaw, legpitch - stride * LEG_SWING, flags);
    }

    void rendergame()
    {
        entities::renderentities();
        loopv(players) renderplayer(players[i], players[i] == player1);
    }

    void renderavatar() {}
    void renderplayerpreview(int model, int color, int team, int weap) {}
}

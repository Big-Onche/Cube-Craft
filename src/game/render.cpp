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

    static const char * const heldcubemodel = "game/heldcube";

    // The split meshes retain the coordinates of the original 30-unit model.
    // Their configs recenter articulated pieces on these joint heights.
    static const float HIP_HEIGHT = 11.25f, SHOULDER_HEIGHT = 22.5f;
    static const float ARM_OFFSET = 5.625f, LEG_OFFSET = 1.875f;
    static const float MIN_GAIT_CADENCE = 0.65f, MAX_GAIT_CADENCE = 3.0f;
    static const float LEG_SWING = 32.0f, ARM_SWING = 28.0f;
    static const float LEG_STRAFE_SWING = 24.0f, ARM_STRAFE_SWING = 18.0f;
    static const float IDLE_HEAD_YAW = 65.0f, MOVING_HEAD_YAW = 75.0f;
    static const float IDLE_BODY_TURN_SPEED = 180.0f, MOVING_BODY_TURN_RESPONSE = 14.0f;
    static const float CROUCH_HIP_DROP = 2.0f, CROUCH_HEAD_DROP = 0.75f;
    static const float CROUCH_TORSO_PITCH = -35.0f, CROUCH_ARM_PITCH = -15.0f, CROUCH_LEG_PITCH = 35.0f;
    static const float HUD_ARM_FORWARD = 6.0f, HUD_ARM_SIDE = 12.0f, HUD_ARM_DOWN = 8.0f;
    static const float HUD_ARM_IDLE_PITCH = -90.0f, HUD_ARM_ROLL = -3.0f;
    static const float HUD_ARM_GAIT_SCALE = 0.45f, HUD_ARM_BOB = 0.35f;
    static const float HUD_ARM_LENGTH = 11.25f;
    static const float HUD_CUBE_GRIP_FORWARD = 1.2f, HUD_CUBE_GRIP_SIDE = -0.5f,
                       HUD_CUBE_GRIP_UP = 1.25f, HUD_CUBE_SIZE = 0.8f;

    VARP(hudgun, 0, 1, 1);

    void preloadplayermodels()
    {
        loopi(NUM_PLAYER_PARTS) preloadmodel(playermodels[i]);
        preloadmodel(heldcubemodel);
    }

    static void renderpart(gameent *d, int part, const vec &origin, float yaw, float pitch, float roll, int flags)
    {
        rendermodel(playermodels[part], ANIM_MAPMODEL | ANIM_LOOP, origin, yaw, pitch, roll, flags, d);
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
            float cadence = clamp(MIN_GAIT_CADENCE + sqrtf(speed / maxspeed), MIN_GAIT_CADENCE, MAX_GAIT_CADENCE);
            d->renderstridephase = fmodf(d->renderstridephase + 2.0f * PI * cadence * elapsed / 1000.0f, 2.0f * PI);
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
        return movement > 0.05f ? MOVING_HEAD_YAW : IDLE_HEAD_YAW;
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

        // Follow the shortest arc with exponential smoothing so the response
        // stays fast and consistent at any frame rate.
        if(movement > 0.05f)
        {
            float turn = yawoffset(d->yaw, d->renderbodyyaw);
            float blend = 1.0f - expf(-MOVING_BODY_TURN_RESPONSE * elapsed / 1000.0f);
            d->renderbodyyaw = fabsf(turn) < 0.05f
                             ? normalizeyaw(d->yaw)
                             : normalizeyaw(d->renderbodyyaw + turn * blend);
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
        float inputmagnitude = sqrtf(float(d->move*d->move + d->strafe*d->strafe));
        float forwardgait = inputmagnitude > 0 ? fabsf(d->move) / inputmagnitude : 1.0f;
        float strafegait = inputmagnitude > 0 ? fabsf(d->strafe) / inputmagnitude : 0.0f;
        float strafedirection = d->strafe < 0 ? -1.0f : 1.0f;
        float forwardstride = stride * forwardgait;
        float strafestride = stride * strafegait * strafedirection;
        float bob = fabsf(cosf(phase)) * 0.45f * movement * (1.0f - 0.65f * crouch);
        float bodyyaw = updatebodyyaw(d, movement);
        float headlimit = headyawlimit(movement);
        float headyaw = normalizeyaw(bodyyaw + clamp(yawoffset(d->yaw, bodyyaw), -headlimit, headlimit));
        float torsopitch = CROUCH_TORSO_PITCH * crouch;
        float armpitch = CROUCH_ARM_PITCH * crouch;
        float legpitch = CROUCH_LEG_PITCH * crouch;
        float actionpitch = playerarmactionpitch(d);
        bool actionactive = actionpitch >= 0;

        vec feet = d->feetpos(bob);
        vec hips = vec(feet).addz(HIP_HEIGHT - CROUCH_HIP_DROP * crouch);
        vec shoulderoffset(0, 0, SHOULDER_HEIGHT - HIP_HEIGHT);
        shoulderoffset.rotate_around_x(torsopitch * RAD).rotate_around_z(bodyyaw * RAD);
        vec shoulders = vec(hips).add(shoulderoffset);
        vec neck = vec(shoulders).addz(-CROUCH_HEAD_DROP * crouch);
        vec lateral(1, 0, 0);
        lateral.rotate_around_z(bodyyaw * RAD);
        vec leftshoulder = vec(shoulders).madd(lateral, -ARM_OFFSET);
        vec rightshoulder = vec(shoulders).madd(lateral, ARM_OFFSET);
        vec lefthip = vec(hips).madd(lateral, -LEG_OFFSET);
        vec righthip = vec(hips).madd(lateral, LEG_OFFSET);

        renderpart(d, PART_TORSO, hips, bodyyaw, torsopitch, 0, flags);
        renderpart(d, PART_HEAD, neck, headyaw, clamp(d->pitch, -80.0f, 80.0f) + sinf(phase * 2.0f) * 1.5f * movement, 0, flags);
        renderpart(d, PART_LEFT_ARM, leftshoulder, bodyyaw, armpitch + (actionactive ? actionpitch : -forwardstride * ARM_SWING), actionactive ? 0 : -strafestride * ARM_STRAFE_SWING, flags);
        renderpart(d, PART_RIGHT_ARM, rightshoulder, bodyyaw, armpitch + forwardstride * ARM_SWING, strafestride * ARM_STRAFE_SWING, flags);
        renderpart(d, PART_LEFT_LEG, lefthip, bodyyaw, legpitch + forwardstride * LEG_SWING, strafestride * LEG_STRAFE_SWING, flags);
        renderpart(d, PART_RIGHT_LEG, righthip, bodyyaw, legpitch - forwardstride * LEG_SWING, -strafestride * LEG_STRAFE_SWING, flags);
    }

    void rendergame()
    {
        entities::renderentities();
        loopv(players) renderplayer(players[i], players[i] == player1);
    }

    static void renderhudarm(int part, float side, float pitch, float roll, float bob)
    {
        vec origin(camera1->o);
        origin.madd(camdir, HUD_ARM_FORWARD)
              .madd(camright, side * HUD_ARM_SIDE)
              .madd(camup, -HUD_ARM_DOWN + bob);

        rendermodel(playermodels[part], ANIM_MAPMODEL | ANIM_LOOP, origin, camera1->yaw, camera1->pitch + pitch, roll, MDL_NOBATCH | MDL_NOSHADOW, player1);
    }

    static vec hudarmhand(float side, float pitch, float roll, float bob)
    {
        vec hand(camera1->o);
        hand.madd(camdir, HUD_ARM_FORWARD)
            .madd(camright, side * HUD_ARM_SIDE)
            .madd(camup, -HUD_ARM_DOWN + bob);

        vec reach(0, 0, -HUD_ARM_LENGTH);
        reach.rotate_around_y(-roll * RAD)
             .rotate_around_x((camera1->pitch + pitch) * RAD)
             .rotate_around_z(camera1->yaw * RAD);

        return hand.add(reach);
    }

    static bool holdinglefthand(const gameent *)
    {
        // Keep the off-hand model path ready for a future left-hand item slot.
        return false;
    }

    static void renderheldcube(int selected, float armpitch, float armroll, float bob, float actionpitch)
    {
        string toptexture, sidetexture, bottomtexture;
        copystring(toptexture, getworldcubetexture(selected, WORLD_CUBE_TOP));
        copystring(sidetexture, getworldcubetexture(selected, WORLD_CUBE_SIDE));
        copystring(bottomtexture, getworldcubetexture(selected, WORLD_CUBE_BOTTOM));
        modelskinoverride skins[] =
        {
            modelskinoverride("top", toptexture),
            modelskinoverride("side", sidetexture),
            modelskinoverride("bottom", bottomtexture)
        };

        float swingpitch = max(actionpitch, 0.0f);
        vec origin = hudarmhand(1, armpitch, armroll, bob);
        origin.madd(camdir, HUD_CUBE_GRIP_FORWARD)
              .madd(camright, HUD_CUBE_GRIP_SIDE)
              .madd(camup, HUD_CUBE_GRIP_UP);

        rendermodelwithskins(heldcubemodel, ANIM_MAPMODEL | ANIM_LOOP, origin, camera1->yaw, camera1->pitch - swingpitch * 0.35f, 0, MDL_NOSHADOW, player1, skins, 3, HUD_CUBE_SIZE);
    }

    static void renderheldscatter(int selected, float armpitch, float armroll, float bob, float actionpitch)
    {
        const char *model = getworldscattermodel(selected);
        if(!model[0]) return;
        vec origin = hudarmhand(1, armpitch, armroll, bob);
        origin.madd(camdir, HUD_CUBE_GRIP_FORWARD).madd(camright, HUD_CUBE_GRIP_SIDE).madd(camup, HUD_CUBE_GRIP_UP);

        rendermodel(model, ANIM_MAPMODEL | ANIM_LOOP, origin, camera1->yaw, camera1->pitch - max(actionpitch, 0.0f) * 0.35f, 0, MDL_NOBATCH | MDL_NOSHADOW, player1, NULL, 0, 0, 0.45f);
    }

    bool heldtorchemitterposition(vec &position)
    {
        if(!hudgun || editmode || !m_creative || !player1 || player1->state != CS_ALIVE || isthirdperson())
            return false;

        const int selected = selectedcreativeblock(), cubecount = numworldcubes();
        if(selected < cubecount || !isworldtorch(selected - cubecount)) return false;

        const char *model = getworldscattermodel(selected - cubecount);
        if(!model[0]) return false;

        const float speed = horizontalmeterspersecond(player1),
                    movement = movementamount(player1, speed),
                    crouch = player1->rendercrouch,
                    stride = sinf(player1->renderstridephase) * movement * (1.0f - 0.55f * crouch),
                    inputmagnitude = sqrtf(float(player1->move*player1->move + player1->strafe*player1->strafe)),
                    forwardgait = inputmagnitude > 0 ? fabsf(player1->move) / inputmagnitude : 1.0f,
                    strafegait = inputmagnitude > 0 ? fabsf(player1->strafe) / inputmagnitude : 0.0f,
                    strafedirection = player1->strafe < 0 ? -1.0f : 1.0f,
                    forwardstride = stride * forwardgait,
                    strafestride = stride * strafegait * strafedirection,
                    actionpitch = playerarmactionpitch(player1),
                    basepitch = HUD_ARM_IDLE_PITCH + CROUCH_ARM_PITCH * crouch,
                    bob = (0.5f - fabsf(cosf(player1->renderstridephase))) * HUD_ARM_BOB * movement * (1.0f - 0.65f * crouch),
                    armpitch = basepitch + (actionpitch >= 0 ? actionpitch : -forwardstride * ARM_SWING * HUD_ARM_GAIT_SCALE),
                    armroll = 180.0f + HUD_ARM_ROLL + (actionpitch >= 0 ? 0 : -strafestride * ARM_STRAFE_SWING * HUD_ARM_GAIT_SCALE);

        vec origin = hudarmhand(1, armpitch, armroll, bob);
        origin.madd(camdir, HUD_CUBE_GRIP_FORWARD).madd(camright, HUD_CUBE_GRIP_SIDE).madd(camup, HUD_CUBE_GRIP_UP);
        return modeltagposition(model, "tag_emitter", position, origin, camera1->yaw, camera1->pitch - max(actionpitch, 0.0f) * 0.35f, 0, 0.45f);
    }

    void renderavatar()
    {
        if(!hudgun || editmode || !player1 || player1->state != CS_ALIVE) return;

        float speed = horizontalmeterspersecond(player1);
        float movement = movementamount(player1, speed);
        float crouch = player1->rendercrouch;
        float stride = sinf(player1->renderstridephase) * movement * (1.0f - 0.55f * crouch);
        float inputmagnitude = sqrtf(float(player1->move*player1->move + player1->strafe*player1->strafe));
        float forwardgait = inputmagnitude > 0 ? fabsf(player1->move) / inputmagnitude : 1.0f;
        float strafegait = inputmagnitude > 0 ? fabsf(player1->strafe) / inputmagnitude : 0.0f;
        float strafedirection = player1->strafe < 0 ? -1.0f : 1.0f;
        float forwardstride = stride * forwardgait;
        float strafestride = stride * strafegait * strafedirection;
        float actionpitch = playerarmactionpitch(player1);
        bool actionactive = actionpitch >= 0;
        float basepitch = HUD_ARM_IDLE_PITCH + CROUCH_ARM_PITCH * crouch;
        float bob = (0.5f - fabsf(cosf(player1->renderstridephase))) * HUD_ARM_BOB * movement * (1.0f - 0.65f * crouch);
        float rightarmpitch = basepitch + (actionactive ? actionpitch : -forwardstride * ARM_SWING * HUD_ARM_GAIT_SCALE);
        float rightarmroll = 180.0f + HUD_ARM_ROLL + (actionactive ? 0 : -strafestride * ARM_STRAFE_SWING * HUD_ARM_GAIT_SCALE);

        if(holdinglefthand(player1))
            renderhudarm(PART_RIGHT_ARM, -1, basepitch + forwardstride * ARM_SWING * HUD_ARM_GAIT_SCALE, 180.0f - HUD_ARM_ROLL + strafestride * ARM_STRAFE_SWING * HUD_ARM_GAIT_SCALE, bob);

        // The exported left-arm mesh is the visually correct right action hand.
        renderhudarm(PART_LEFT_ARM, 1, rightarmpitch, rightarmroll, bob);

        if(m_creative && numworldcubes() + numworldscatters() > 0)
        {
            const int selected = selectedcreativeblock(), cubecount = numworldcubes();

            if(selected < cubecount) renderheldcube(selected, rightarmpitch, rightarmroll, bob, actionpitch);
            else renderheldscatter(selected - cubecount, rightarmpitch, rightarmroll, bob, actionpitch);
        }
    }

    void renderplayerpreview(int model, int color, int team, int weap) {}
}

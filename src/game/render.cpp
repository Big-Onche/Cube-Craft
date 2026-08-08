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
    static const char * const worldheldcubemodel = "game/heldcube/world";

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
    static const float HELD_ARM_PITCH = 50.0f;
    static const float HUD_HELD_CUBE_SIZE = 0.7f, HUD_HELD_SCATTER_SIZE = 0.5f;
    static const float WORLD_HELD_CUBE_SIZE = 0.75f, WORLD_HELD_SCATTER_SIZE = 0.55f;

    VARP(hudgun, 0, 1, 1);

    void preloadplayermodels()
    {
        loopi(NUM_PLAYER_PARTS) preloadmodel(playermodels[i]);
        preloadmodel(heldcubemodel);
        preloadmodel(worldheldcubemodel);
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

    static int heldcreativeitem(const gameent *d)
    {
        if(!d || (!m_creative && !m_survival) || d->state != CS_ALIVE) return -1;
        const int selected = d == player1 ? (editmode ? -1 : selectedcreativeblock()) : d->selectedcreative,
                  count = numinventoryitems();
        return selected >= 0 && selected < count ? selected : -1;
    }

    static void renderhelditem(gameent *d, int selected, const vec &origin, float yaw, float pitch, float roll, int flags, bool hud);

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
        vec leftshoulder = vec(shoulders).madd(lateral, ARM_OFFSET);
        vec rightshoulder = vec(shoulders).madd(lateral, -ARM_OFFSET);
        vec lefthip = vec(hips).madd(lateral, LEG_OFFSET);
        vec righthip = vec(hips).madd(lateral, -LEG_OFFSET);

        renderpart(d, PART_TORSO, hips, bodyyaw, torsopitch, 0, flags);
        renderpart(d, PART_HEAD, neck, headyaw, clamp(d->pitch, -80.0f, 80.0f) + sinf(phase * 2.0f) * 1.5f * movement, 0, flags);
        const int selected = heldcreativeitem(d);
        const float rightarmpitch = armpitch + (selected >= 0 ? HELD_ARM_PITCH : 0) + (actionactive ? actionpitch : forwardstride * ARM_SWING), rightarmroll = actionactive ? 0 : strafestride * ARM_STRAFE_SWING;
        renderpart(d, PART_LEFT_ARM, leftshoulder, bodyyaw, armpitch - forwardstride * ARM_SWING, -strafestride * ARM_STRAFE_SWING, flags);
        renderpart(d, PART_RIGHT_ARM, rightshoulder, bodyyaw, rightarmpitch, rightarmroll, flags);
        renderpart(d, PART_LEFT_LEG, lefthip, bodyyaw, legpitch + forwardstride * LEG_SWING, strafestride * LEG_STRAFE_SWING, flags);
        renderpart(d, PART_RIGHT_LEG, righthip, bodyyaw, legpitch - forwardstride * LEG_SWING, -strafestride * LEG_STRAFE_SWING, flags);

        if(selected >= 0)
        {
            vec hand;
            if(modeltagposition(playermodels[PART_RIGHT_ARM], "tag_hand", hand, rightshoulder, bodyyaw, rightarmpitch, rightarmroll))
                renderhelditem(d, selected, hand, bodyyaw, rightarmpitch + 270.0f, rightarmroll, flags, false);
        }
    }

    static gameent *worlddropplayer(int clientnum)
    {
        if(player1 && player1->clientnum == clientnum) return player1;
        return clients.inrange(clientnum) ? clients[clientnum] : NULL;
    }

    static void renderworlddrop(const worlddrop &drop)
    {
        vec position = drop.picking ? drop.pickupfrom : drop.o;
        if(waitforserveredit()) worldpositiontolocal(position);
        if(drop.picking)
        {
            gameent *picker = worlddropplayer(drop.picker);
            if(picker)
            {
                const float amount = clamp((lastmillis - drop.pickupmillis) / 250.0f, 0.0f, 1.0f),
                            smooth = amount * amount * (3.0f - 2.0f * amount);
                position.lerp(picker->o, smooth);
            }
        }
        else
        {
            const uint phaseid = drop.id ? drop.id : drop.sourcerequestid;
            position.z += 1.5f + sinf((lastmillis + int(phaseid % 1000U) * 37) / 350.0f) * 0.75f;
        }

        const float maxdistance = getdynamicentsmaxdistance() * 16.0f;
        if(camera1 && position.squaredist(camera1->o) > maxdistance * maxdistance) return;
        const int type = getworlditemtype(drop.item), worldindex = getworlditemindex(drop.item);
        const float yaw = fmodf(lastmillis * 0.09f + float((drop.id ? drop.id : drop.sourcerequestid) % 360U), 360.0f);
        const int flags = MDL_CULL_VFC | MDL_CULL_DIST | MDL_CULL_OCCLUDED;
        const char *directmodel = type == WORLD_ITEM_NONE ? getinventoryitemmodel(drop.item) : "";
        if(directmodel[0])
        {
            rendermodel(directmodel, ANIM_MAPMODEL | ANIM_LOOP, position, yaw, 0, 0, flags, NULL, NULL, 0, 0, 0.4f);
        }
        else if(type == WORLD_ITEM_CUBE || type == WORLD_ITEM_NONE)
        {
            string toptexture, sidetexture, bottomtexture;
            copystring(toptexture, getworldcubetexture(worldindex, WORLD_CUBE_TOP));
            copystring(sidetexture, getworldcubetexture(worldindex, WORLD_CUBE_SIDE));
            copystring(bottomtexture, getworldcubetexture(worldindex, WORLD_CUBE_BOTTOM));
            modelskinoverride skins[] =
            {
                modelskinoverride("top", toptexture),
                modelskinoverride("side", sidetexture),
                modelskinoverride("bottom", bottomtexture)
            };
            rendermodelwithskins(worldheldcubemodel, ANIM_MAPMODEL | ANIM_LOOP, position, yaw, 0, 0, flags, NULL, skins, 3, 0.45f);
        }
        else if(type == WORLD_ITEM_SCATTER || type == WORLD_ITEM_PLACEABLE)
        {
            const char *model = getworldscattermodel(worldindex);
            if(model[0]) rendermodel(model, ANIM_MAPMODEL | ANIM_LOOP, position, yaw, 0, 0, flags, NULL, NULL, 0, 0, 0.4f);
        }
    }

    static void renderworlddrops()
    {
        const vector<worlddrop *> &drops = getworlddrops();
        loopv(drops) renderworlddrop(*drops[i]);
    }

    void rendergame()
    {
        entities::renderentities();
        renderworlddrops();
        loopv(players) renderplayer(players[i], players[i] == player1);
    }

    struct helditempose
    {
        vec origin;
        float yaw, pitch, roll;
    };

    static bool hudrightarmpose(gameent *d, helditempose &arm, helditempose &item)
    {
        const float speed = horizontalmeterspersecond(d),
                    movement = movementamount(d, speed),
                    crouch = updatecrouch(d, true),
                    phase = updatestridephase(d, speed),
                    stride = sinf(phase) * movement * (1.0f - 0.55f * crouch),
                    inputmagnitude = sqrtf(float(d->move*d->move + d->strafe*d->strafe)),
                    forwardgait = inputmagnitude > 0 ? fabsf(d->move) / inputmagnitude : 1.0f,
                    strafegait = inputmagnitude > 0 ? fabsf(d->strafe) / inputmagnitude : 0.0f,
                    strafedirection = d->strafe < 0 ? -1.0f : 1.0f,
                    forwardstride = stride * forwardgait,
                    strafestride = stride * strafegait * strafedirection,
                    actionpitch = playerarmactionpitch(d),
                    basepitch = HUD_ARM_IDLE_PITCH + CROUCH_ARM_PITCH * crouch,
                    bob = (0.5f - fabsf(cosf(phase))) * HUD_ARM_BOB * movement * (1.0f - 0.65f * crouch),
                    relativepitch = basepitch + (actionpitch >= 0 ? actionpitch : -forwardstride * ARM_SWING * HUD_ARM_GAIT_SCALE);

        arm.origin = camera1->o;
        arm.origin.madd(camdir, HUD_ARM_FORWARD).madd(camright, HUD_ARM_SIDE).madd(camup, -HUD_ARM_DOWN + bob);
        arm.yaw = camera1->yaw;
        arm.pitch = camera1->pitch + relativepitch;
        arm.roll = 180.0f + HUD_ARM_ROLL + (actionpitch >= 0 ? 0 : -strafestride * ARM_STRAFE_SWING * HUD_ARM_GAIT_SCALE);

        item.yaw = camera1->yaw;
        item.pitch = camera1->pitch - max(actionpitch, 0.0f) * 0.35f;
        item.roll = 0;
        return modeltagposition(playermodels[PART_RIGHT_ARM], "tag_hand", item.origin, arm.origin, arm.yaw, arm.pitch, arm.roll);
    }

    static bool worldhelditempose(gameent *d, helditempose &item)
    {
        const bool local = d == player1;
        const float speed = horizontalmeterspersecond(d),
                    movement = movementamount(d, speed),
                    crouch = updatecrouch(d, local),
                    phase = updatestridephase(d, speed),
                    stride = sinf(phase) * movement * (1.0f - 0.55f * crouch),
                    inputmagnitude = sqrtf(float(d->move*d->move + d->strafe*d->strafe)),
                    forwardgait = inputmagnitude > 0 ? fabsf(d->move) / inputmagnitude : 1.0f,
                    strafegait = inputmagnitude > 0 ? fabsf(d->strafe) / inputmagnitude : 0.0f,
                    strafedirection = d->strafe < 0 ? -1.0f : 1.0f,
                    forwardstride = stride * forwardgait,
                    strafestride = stride * strafegait * strafedirection,
                    bob = fabsf(cosf(phase)) * 0.45f * movement * (1.0f - 0.65f * crouch),
                    bodyyaw = updatebodyyaw(d, movement),
                    torsopitch = CROUCH_TORSO_PITCH * crouch,
                    actionpitch = playerarmactionpitch(d);

        vec hips = d->feetpos(bob).addz(HIP_HEIGHT - CROUCH_HIP_DROP * crouch);
        vec shoulderoffset(0, 0, SHOULDER_HEIGHT - HIP_HEIGHT);
        shoulderoffset.rotate_around_x(torsopitch * RAD).rotate_around_z(bodyyaw * RAD);
        vec lateral(1, 0, 0);
        lateral.rotate_around_z(bodyyaw * RAD);
        const vec shoulder = vec(hips).add(shoulderoffset).madd(lateral, -ARM_OFFSET);

        const float armpitch = CROUCH_ARM_PITCH * crouch + HELD_ARM_PITCH + (actionpitch >= 0 ? actionpitch : forwardstride * ARM_SWING),
                    armroll = actionpitch >= 0 ? 0 : strafestride * ARM_STRAFE_SWING;
        const bool tagged = modeltagposition(playermodels[PART_RIGHT_ARM], "tag_hand", item.origin, shoulder, bodyyaw, armpitch, armroll);
        item.yaw = bodyyaw;
        item.pitch = armpitch + 270.0f;
        item.roll = armroll;
        return tagged;
    }

    static void renderheldcube(gameent *d, int selected, const helditempose &pose, int flags, float size, bool hud)
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

        rendermodelwithskins(hud ? heldcubemodel : worldheldcubemodel, ANIM_MAPMODEL | ANIM_LOOP, pose.origin, pose.yaw, pose.pitch, pose.roll, flags, d, skins, 3, size);
    }

    static void renderheldmodel(gameent *d, const char *model, const helditempose &pose, int flags, float size)
    {
        if(!model[0]) return;
        rendermodel(model, ANIM_MAPMODEL | ANIM_LOOP, pose.origin, pose.yaw, pose.pitch, pose.roll, flags, d, NULL, 0, 0, size);
    }

    static void renderheldscatter(gameent *d, int selected, const helditempose &pose, int flags, float size)
    {
        renderheldmodel(d, getworldscattermodel(selected), pose, flags, size);
    }

    static void renderhelditem(gameent *d, int selected, const vec &origin, float yaw, float pitch, float roll, int flags, bool hud)
    {
        helditempose pose;
        pose.origin = origin;
        pose.yaw = yaw;
        pose.pitch = pitch;
        pose.roll = roll;
        const int type = getworlditemtype(selected), worldindex = getworlditemindex(selected);
        if(type == WORLD_ITEM_CUBE)
            renderheldcube(d, worldindex, pose, flags, hud ? HUD_HELD_CUBE_SIZE : WORLD_HELD_CUBE_SIZE, hud);
        else if(type == WORLD_ITEM_SCATTER || type == WORLD_ITEM_PLACEABLE)
            renderheldscatter(d, worldindex, pose, flags, hud ? HUD_HELD_SCATTER_SIZE : WORLD_HELD_SCATTER_SIZE);
        else if(type == WORLD_ITEM_NONE)
            renderheldmodel(d, getinventoryitemmodel(selected), pose, flags, hud ? HUD_HELD_SCATTER_SIZE : WORLD_HELD_SCATTER_SIZE);
    }

    bool heldtorchemitterposition(gameent *d, vec &position)
    {
        const int selected = heldcreativeitem(d), worldindex = getworlditemindex(selected);
        if(getworlditemtype(selected) != WORLD_ITEM_PLACEABLE || !isworldtorch(worldindex)) return false;
        const bool hud = d == player1 && !isthirdperson();
        if(hud && !hudgun) return false;

        const char *model = getworldscattermodel(worldindex);
        if(!model[0]) return false;

        helditempose item, arm;
        if(!(hud ? hudrightarmpose(d, arm, item) : worldhelditempose(d, item)) || !modeltagposition(model, "tag_emitter", position, item.origin, item.yaw, item.pitch, item.roll, hud ? HUD_HELD_SCATTER_SIZE : WORLD_HELD_SCATTER_SIZE))
            return false;

        if(hud) position = calcavatardepthpos(position);
        return true;
    }

    void renderavatar()
    {
        if(!hudgun || editmode || (!m_creative && !m_survival) ||
           !player1 || player1->state != CS_ALIVE)
            return;

        const int selected = heldcreativeitem(player1);
        helditempose arm, item;
        const bool tagged = hudrightarmpose(player1, arm, item);
        const int flags = MDL_NOBATCH | MDL_NOSHADOW;
        rendermodel(playermodels[PART_RIGHT_ARM], ANIM_MAPMODEL | ANIM_LOOP, arm.origin, arm.yaw, arm.pitch, arm.roll, flags, player1);

        if(tagged && selected >= 0) renderhelditem(player1, selected, item.origin, item.yaw, item.pitch, item.roll, flags, true);
    }

    void renderplayerpreview(int model, int color, int team, int weap) {}
}

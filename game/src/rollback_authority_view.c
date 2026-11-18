#include "rollback_authority_view.h"

#ifdef NATIVE_PORT
#include <string.h>

#include "objects.h"
#include "particles.h"
#include "structs.h"

extern Object **gObjPtrList;
extern Object **gParticlePtrList;
extern Object **gCollisionObjects;
extern Object **D_8011AE74;
extern CheckpointNode *gTrackCheckpoints;
extern Object *(*gCameraObjList)[20];
extern Object *(*gRacers)[10];
extern Object **gRacersByPosition;
extern Object **gRacersByPort;
extern s32 gNumRacers;
extern s32 gCurrentRNGSeed;
extern Object *(*gAINodes)[128];
extern s8 (*gDrawbridgeTimers)[8];
extern unk800179D0 *D_8011AFF4;
extern ObjectHeader *(*gLoadedObjectHeaders)[ASSET_OBJECTS_COUNT];
extern u8 (*gObjectHeaderReferences)[ASSET_OBJECTS_COUNT];
extern s32 *gObjectMap[2];
extern s32 gObjectCount;
extern Particle *gTriangleParticleBuffer;
extern Particle *gRectangleParticleBuffer;
extern Particle *gSpriteParticleBuffer;
extern Particle *gLineParticleBuffer;
extern PointParticle *gPointParticleBuffer;

static void semantic_hash_u32(uint64_t *hash, uint32_t value) {
    unsigned byte;
    for (byte = 0u; byte < 4u; byte++) {
        *hash ^= (uint8_t)(value >> (byte * 8u));
        *hash *= UINT64_C(1099511628211);
    }
}

static void semantic_hash_float(uint64_t *hash, f32 value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    semantic_hash_u32(hash, bits);
}

uint64_t mdkr_object_rollback_semantic_digest(void) {
    uint64_t hash = UINT64_C(1469598103934665603);
    s32 index;

    semantic_hash_u32(&hash, (uint32_t)gCurrentRNGSeed);
    semantic_hash_u32(&hash, (uint32_t)gNumRacers);
    if (gRacers == NULL) {
        return hash;
    }
    for (index = 0; index < gNumRacers && index < 10; index++) {
        Object *object = (*gRacers)[index];
        Object_Racer *racer;
        if (object == NULL || object->racer == NULL) {
            semantic_hash_u32(&hash, UINT32_C(0xffffffff));
            continue;
        }
        racer = object->racer;
        semantic_hash_u32(&hash, (uint32_t)index);
        semantic_hash_u32(&hash, (uint32_t)racer->racerIndex);
        semantic_hash_u32(&hash, (uint32_t)racer->playerIndex);
        semantic_hash_u32(&hash, (uint32_t)racer->vehicleID);
        semantic_hash_float(&hash, object->trans.x_position);
        semantic_hash_float(&hash, object->trans.y_position);
        semantic_hash_float(&hash, object->trans.z_position);
        semantic_hash_u32(
            &hash, (uint32_t)(uint16_t)object->trans.rotation.x_rotation);
        semantic_hash_u32(
            &hash, (uint32_t)(uint16_t)object->trans.rotation.y_rotation);
        semantic_hash_u32(
            &hash, (uint32_t)(uint16_t)object->trans.rotation.z_rotation);
        semantic_hash_float(&hash, object->x_velocity);
        semantic_hash_float(&hash, object->y_velocity);
        semantic_hash_float(&hash, object->z_velocity);
        semantic_hash_float(&hash, racer->forwardVel);
        semantic_hash_float(&hash, racer->velocity);
        semantic_hash_float(&hash, racer->checkpoint_distance);
        semantic_hash_u32(&hash, (uint32_t)racer->courseCheckpoint);
        semantic_hash_u32(&hash, (uint32_t)racer->nextCheckpoint);
        semantic_hash_u32(&hash, (uint32_t)racer->lap);
        semantic_hash_u32(&hash, (uint32_t)racer->countLap);
        semantic_hash_u32(&hash, (uint32_t)racer->raceFinished);
        semantic_hash_u32(&hash, (uint32_t)racer->finishPosition);
        semantic_hash_u32(&hash, (uint32_t)racer->groundedWheels);
        semantic_hash_u32(&hash, (uint32_t)racer->drift_direction);
        semantic_hash_float(&hash, racer->buoyancy);
        semantic_hash_u32(&hash, (uint32_t)racer->attackType);
        semantic_hash_u32(&hash, (uint32_t)racer->spinout_timer);
        semantic_hash_u32(&hash, (uint32_t)racer->squish_timer);
        semantic_hash_u32(&hash, (uint32_t)racer->bubbleTrapTimer);
        semantic_hash_u32(&hash, (uint32_t)racer->shieldTimer);
        semantic_hash_u32(&hash, (uint32_t)racer->unk18C);
        semantic_hash_u32(&hash, (uint32_t)racer->unk18A);
        semantic_hash_u32(&hash, (uint32_t)racer->unk1D1);
        semantic_hash_u32(&hash, (uint32_t)racer->unk1C6);
        semantic_hash_u32(&hash, (uint32_t)racer->unk206);
        semantic_hash_u32(&hash, (uint32_t)racer->unk1F1);
        semantic_hash_u32(&hash, (uint32_t)racer->bananas);
        semantic_hash_u32(&hash, (uint32_t)racer->boost_sound);
    }
    for (index = 0; index < gObjectCount; index++) {
        Object *object = gObjPtrList[index];
        Object_Weapon *weapon;
        s32 owner = -1;
        s32 target = -1;
        s32 racerIndex;
        if (object == NULL ||
            (object->behaviorId != BHV_WEAPON &&
             object->behaviorId != BHV_WEAPON_2) ||
            object->weapon == NULL) {
            continue;
        }
        weapon = object->weapon;
        for (racerIndex = 0;
             racerIndex < gNumRacers && racerIndex < 10; racerIndex++) {
            if ((*gRacers)[racerIndex] == weapon->owner) {
                owner = racerIndex;
            }
            if ((*gRacers)[racerIndex] == weapon->target) {
                target = racerIndex;
            }
        }
        semantic_hash_u32(&hash, UINT32_C(0x5745504e));
        semantic_hash_u32(&hash, (uint32_t)index);
        semantic_hash_u32(&hash, (uint32_t)object->objectID);
        semantic_hash_float(&hash, object->trans.x_position);
        semantic_hash_float(&hash, object->trans.y_position);
        semantic_hash_float(&hash, object->trans.z_position);
        semantic_hash_float(&hash, object->x_velocity);
        semantic_hash_float(&hash, object->y_velocity);
        semantic_hash_float(&hash, object->z_velocity);
        semantic_hash_u32(
            &hash, (uint32_t)object->properties.projectile.timer);
        semantic_hash_u32(
            &hash, (uint32_t)object->properties.projectile.unk4);
        semantic_hash_u32(&hash, (uint32_t)weapon->weaponID);
        semantic_hash_u32(&hash, (uint32_t)weapon->checkpoint);
        semantic_hash_float(&hash, weapon->checkpointDist);
        semantic_hash_float(&hash, weapon->forwardVel);
        semantic_hash_u32(&hash, (uint32_t)owner);
        semantic_hash_u32(&hash, (uint32_t)target);
    }
    return hash;
}

static uint32_t semantic_fold(uint64_t hash) {
    return (uint32_t)(hash ^ (hash >> 32));
}

uint32_t mdkr_object_rollback_racer_digest(s32 index) {
    uint64_t hash = UINT64_C(1469598103934665603);
    Object *object;
    Object_Racer *racer;
    if (gRacers == NULL || index < 0 || index >= gNumRacers || index >= 10) {
        return 0u;
    }
    object = (*gRacers)[index];
    if (object == NULL || object->racer == NULL) {
        return UINT32_C(0xffffffff);
    }
    racer = object->racer;
    semantic_hash_u32(&hash, (uint32_t)racer->playerIndex);
    semantic_hash_u32(&hash, (uint32_t)racer->vehicleID);
    semantic_hash_float(&hash, object->trans.x_position);
    semantic_hash_float(&hash, object->trans.y_position);
    semantic_hash_float(&hash, object->trans.z_position);
    semantic_hash_u32(
        &hash, (uint32_t)(uint16_t)object->trans.rotation.y_rotation);
    semantic_hash_float(&hash, object->x_velocity);
    semantic_hash_float(&hash, object->y_velocity);
    semantic_hash_float(&hash, object->z_velocity);
    semantic_hash_float(&hash, racer->forwardVel);
    semantic_hash_float(&hash, racer->velocity);
    semantic_hash_u32(&hash, (uint32_t)racer->courseCheckpoint);
    semantic_hash_u32(&hash, (uint32_t)racer->lap);
    semantic_hash_u32(&hash, (uint32_t)racer->bananas);
    semantic_hash_u32(&hash, (uint32_t)racer->attackType);
    semantic_hash_u32(&hash, (uint32_t)racer->spinout_timer);
    return semantic_fold(hash);
}

uint32_t mdkr_object_rollback_world_digest(void) {
    uint64_t hash = UINT64_C(1469598103934665603);
    s32 index;
    semantic_hash_u32(&hash, (uint32_t)gObjectCount);
    if (gObjPtrList == NULL) {
        return semantic_fold(hash);
    }
    for (index = 0; index < gObjectCount; index++) {
        Object *object = gObjPtrList[index];
        if (object == NULL) {
            semantic_hash_u32(&hash, UINT32_C(0xffffffff));
            continue;
        }
        semantic_hash_u32(&hash, (uint32_t)(uint16_t)object->behaviorId);
        semantic_hash_u32(&hash, (uint32_t)(uint16_t)object->objectID);
        semantic_hash_float(&hash, object->trans.x_position);
        semantic_hash_float(&hash, object->trans.y_position);
        semantic_hash_float(&hash, object->trans.z_position);
    }
    return semantic_fold(hash);
}

uint32_t mdkr_object_rollback_object_count(void) {
    return (uint32_t)gObjectCount;
}

uint32_t mdkr_object_rollback_racer_count(void) {
    return (uint32_t)gNumRacers;
}

s32 mdkr_object_rollback_view(MdkrObjectRollbackView *view) {
    if (view == NULL) {
        return FALSE;
    }
    memset(view, 0, sizeof(*view));
    view->object_list = gObjPtrList;
    view->particle_list = gParticlePtrList;
    view->collision_objects = gCollisionObjects;
    view->animation_objects = D_8011AE74;
    view->track_checkpoints = gTrackCheckpoints;
    view->camera_objects = gCameraObjList;
    view->racers = gRacers;
    view->racers_by_position = gRacersByPosition;
    view->racers_by_port = gRacersByPort;
    view->ai_nodes = gAINodes;
    view->drawbridge_timers = gDrawbridgeTimers;
    view->behavior_scratch = D_8011AFF4;
    view->loaded_object_headers = gLoadedObjectHeaders;
    view->object_header_references = gObjectHeaderReferences;
    view->object_maps[0] = gObjectMap[0];
    view->object_maps[1] = gObjectMap[1];
    view->particle_buffers[0] = gTriangleParticleBuffer;
    view->particle_buffers[1] = gRectangleParticleBuffer;
    view->particle_buffers[2] = gSpriteParticleBuffer;
    view->particle_buffers[3] = gLineParticleBuffer;
    view->particle_buffers[4] = gPointParticleBuffer;
    return view->object_list != NULL && view->particle_list != NULL &&
           view->collision_objects != NULL &&
           view->animation_objects != NULL &&
           view->track_checkpoints != NULL && view->camera_objects != NULL &&
           view->racers != NULL && view->racers_by_position != NULL &&
           view->racers_by_port != NULL && view->ai_nodes != NULL &&
           view->drawbridge_timers != NULL &&
           view->behavior_scratch != NULL &&
           view->loaded_object_headers != NULL &&
           view->object_header_references != NULL &&
           view->particle_buffers[0] != NULL &&
           view->particle_buffers[1] != NULL &&
           view->particle_buffers[2] != NULL &&
           view->particle_buffers[3] != NULL &&
           view->particle_buffers[4] != NULL;
}

s32 mdkr_object_rollback_identify_address(
    const void *allocation_base, const void *address,
    MdkrObjectRollbackAddressInfo *info) {
    const uint8_t *behavior;
    if (allocation_base == NULL || address == NULL || info == NULL ||
        gObjPtrList == NULL) {
        return FALSE;
    }
    if (!mdkr_object_rollback_describe_object(allocation_base, info)) {
        return FALSE;
    }
    info->object_offset =
        (size_t)((const uint8_t *)address - (const uint8_t *)allocation_base);
    behavior = (const uint8_t *)((const Object *)allocation_base)->anyBehaviorData;
    if (behavior != NULL && behavior >= (const uint8_t *)allocation_base &&
        behavior <= (const uint8_t *)address) {
        info->behavior_offset =
            (size_t)((const uint8_t *)address - behavior);
        info->has_behavior_offset = TRUE;
    }
    return TRUE;
}

s32 mdkr_object_rollback_describe_object(
    const void *object_pointer, MdkrObjectRollbackAddressInfo *info) {
    const Object *object = (const Object *)object_pointer;
    s32 index;
    if (object == NULL || info == NULL || gObjPtrList == NULL) {
        return FALSE;
    }
    memset(info, 0, sizeof(*info));
    info->object_index = -1;
    for (index = 0; index < gObjectCount; index++) {
        if (gObjPtrList[index] == object) {
            info->object_index = index;
            break;
        }
    }
    info->behavior_id = object->behaviorId;
    info->header_type = object->headerType;
    info->behavior_base_offset = object->anyBehaviorData != NULL
        ? (size_t)((const uint8_t *)object->anyBehaviorData -
                   (const uint8_t *)object) : SIZE_MAX;
    info->shading_base_offset = object->shading != NULL
        ? (size_t)((const uint8_t *)object->shading -
                   (const uint8_t *)object) : SIZE_MAX;
    info->shadow_base_offset = object->shadow != NULL
        ? (size_t)((const uint8_t *)object->shadow -
                   (const uint8_t *)object) : SIZE_MAX;
    info->interaction_base_offset = object->interactObj != NULL
        ? (size_t)((const uint8_t *)object->interactObj -
                   (const uint8_t *)object) : SIZE_MAX;
    info->collision_base_offset = object->collisionData != NULL
        ? (size_t)((const uint8_t *)object->collisionData -
                   (const uint8_t *)object) : SIZE_MAX;
    return TRUE;
}
#endif

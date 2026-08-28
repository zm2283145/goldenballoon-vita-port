#include "character_workshop_model.h"

#include "modern_character_lod.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace {

constexpr uint32_t requiredContextMask(uint32_t vehicleMask) {
    return 1u | ((vehicleMask & 7u) << 1u);
}

void setRow(CharacterWorkshopReadiness      &readiness,
            CharacterWorkshopReadinessId     id,
            CharacterWorkshopReadinessStatus status,
            CharacterWorkshopTab             tab) {
    const size_t index    = static_cast<size_t>(id);
    readiness.rows[index] = {id, status, tab};
    if (status == CharacterWorkshopReadinessStatus::Ready ||
        status == CharacterWorkshopReadinessStatus::Accepted) {
        ++readiness.readyCount;
    }
}

std::string normalizedRigName(const std::string &name) {
    std::string result;
    result.reserve(name.size());
    for (unsigned char byte : name) {
        if (byte >= 'A' && byte <= 'Z') byte =
            static_cast<unsigned char>(byte - 'A' + 'a');
        if ((byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9')) {
            result.push_back(static_cast<char>(byte));
        }
    }
    return result;
}

float rigNameScore(size_t role, const std::string &key) {
    static const std::vector<std::vector<const char *>> aliases = {
        {"hips", "pelvis", "hip", "rootpelvis"},
        {"spine01", "spine1", "spine", "lowerback"},
        {"upperchest", "chest", "spine02", "spine2", "upperback"},
        {"head", "headbone"},
        {"leftupperarm", "upperarmleft", "upperarml", "leftarm", "arml"},
        {"leftlowerarm", "lowerarmleft", "leftforearm", "forearmleft", "forearml", "elbowl"},
        {"lefthand", "handleft", "handl"},
        {"rightupperarm", "upperarmright", "upperarmr", "rightarm", "armr"},
        {"rightlowerarm", "lowerarmright", "rightforearm", "forearmright", "forearmr", "elbowr"},
        {"righthand", "handright", "handr"},
        {"leftupperleg", "leftupleg", "upperlegleft", "upperlegl", "leftthigh", "thighleft", "thighl", "legl"},
        {"leftlowerleg", "leftleg", "lowerlegleft", "lowerlegl", "leftshin", "calfleft", "calfl", "kneel"},
        {"leftfoot", "footleft", "footl", "leftankle"},
        {"rightupperleg", "rightupleg", "upperlegright", "upperlegr", "rightthigh", "thighright", "thighr", "legr"},
        {"rightlowerleg", "rightleg", "lowerlegright", "lowerlegr", "rightshin", "calfright", "calfr", "kneer"},
        {"rightfoot", "footright", "footr", "rightankle"},
    };
    if (role >= aliases.size() || key.empty()) return 0.0f;
    float best = 0.0f;
    for (const char *rawAlias : aliases[role]) {
        const std::string alias(rawAlias);
        if (key == alias) best = std::max(best, 0.98f);
        else if (key.size() > alias.size() &&
                 key.compare(key.size() - alias.size(), alias.size(), alias) == 0) {
            best = std::max(best, 0.92f);
        } else if (key.find(alias) != std::string::npos) {
            best = std::max(best, 0.78f);
        }
    }
    return best;
}

bool rigJointValid(const std::vector<CharacterWorkshopRigJoint> &joints,
                   int joint) {
    return joint >= 0 && joint < static_cast<int>(joints.size());
}

bool rigAncestorInclusive(
    const std::vector<CharacterWorkshopRigJoint> &joints,
    int ancestor, int descendant) {
    if (!rigJointValid(joints, ancestor) || !rigJointValid(joints, descendant)) {
        return false;
    }
    int current = descendant;
    for (size_t depth = 0u; rigJointValid(joints, current) &&
         depth <= joints.size(); ++depth) {
        if (current == ancestor) return true;
        current = joints[static_cast<size_t>(current)].parent;
    }
    return false;
}

int rigLowestCommonAncestor(
    const std::vector<CharacterWorkshopRigJoint> &joints,
    const std::vector<int> &mapped) {
    if (mapped.empty() ||
        std::any_of(mapped.begin(), mapped.end(), [&](int joint) {
            return !rigJointValid(joints, joint);
        })) return -1;
    int candidate = mapped[0];
    for (size_t depth = 0u; rigJointValid(joints, candidate) &&
         depth <= joints.size(); ++depth) {
        if (std::all_of(mapped.begin() + 1u, mapped.end(), [&](int joint) {
                return rigAncestorInclusive(joints, candidate, joint);
            })) return candidate;
        candidate = joints[static_cast<size_t>(candidate)].parent;
    }
    return -1;
}

int rigChildOnPath(const std::vector<CharacterWorkshopRigJoint> &joints,
                   int ancestor, int descendant) {
    if (!rigAncestorInclusive(joints, ancestor, descendant) ||
        ancestor == descendant) return -1;
    int current = descendant;
    for (size_t depth = 0u; rigJointValid(joints, current) &&
         depth <= joints.size(); ++depth) {
        const int parent = joints[static_cast<size_t>(current)].parent;
        if (parent == ancestor) return current;
        current = parent;
    }
    return -1;
}

bool rigSuggestionUses(
    const CharacterWorkshopRigSuggestion &suggestion, size_t exceptRole,
    int joint) {
    for (size_t role = 0u; role < suggestion.roles.size(); ++role) {
        if (role != exceptRole && suggestion.roles[role].joint == joint) {
            return true;
        }
    }
    return false;
}

void setHierarchySuggestion(
    CharacterWorkshopRigSuggestion &suggestion, size_t role, int joint,
    float confidence, CharacterWorkshopRigEvidence evidence) {
    if (joint < 0 || rigSuggestionUses(suggestion, role, joint)) return;
    suggestion.roles[role] = {joint, confidence, evidence};
    ++suggestion.hierarchyRoles;
}

void removeRigSuggestionEvidence(CharacterWorkshopRigSuggestion &suggestion,
                                 size_t role) {
    switch (suggestion.roles[role].evidence) {
        case CharacterWorkshopRigEvidence::Name:
            if (suggestion.namedRoles != 0u) --suggestion.namedRoles;
            break;
        case CharacterWorkshopRigEvidence::GeometrySymmetry:
            if (suggestion.geometryRoles != 0u) --suggestion.geometryRoles;
            break;
        case CharacterWorkshopRigEvidence::HierarchyCommonAncestor:
        case CharacterWorkshopRigEvidence::HierarchyChain:
            if (suggestion.hierarchyRoles != 0u) --suggestion.hierarchyRoles;
            break;
        default: break;
    }
    suggestion.roles[role] = {};
}

void multiplyRigQuaternion(const std::array<float, 4> &left,
                           const std::array<float, 4> &right,
                           std::array<float, 4> &output) {
    const std::array<float, 4> value{{
        left[3] * right[0] + left[0] * right[3] +
            left[1] * right[2] - left[2] * right[1],
        left[3] * right[1] - left[0] * right[2] +
            left[1] * right[3] + left[2] * right[0],
        left[3] * right[2] + left[0] * right[1] -
            left[1] * right[0] + left[2] * right[3],
        left[3] * right[3] - left[0] * right[0] -
            left[1] * right[1] - left[2] * right[2]
    }};
    output = value;
}

bool normalizeRigQuaternion(std::array<float, 4> &value) {
    const float length = std::sqrt(
        value[0] * value[0] + value[1] * value[1] +
        value[2] * value[2] + value[3] * value[3]);
    if (!std::isfinite(length) || length < 1.0e-8f) return false;
    for (float &component : value) component /= length;
    if (value[3] < 0.0f) {
        for (float &component : value) component = -component;
    }
    return true;
}

std::array<float, 4> conjugateRigQuaternion(
    const std::array<float, 4> &value) {
    return {{-value[0], -value[1], -value[2], value[3]}};
}

void rotateRigVector(const std::array<float, 4> &rotation,
                     const std::array<float, 3> &input,
                     std::array<float, 3> &output) {
    const std::array<float, 3> twiceCross{{
        2.0f * (rotation[1] * input[2] - rotation[2] * input[1]),
        2.0f * (rotation[2] * input[0] - rotation[0] * input[2]),
        2.0f * (rotation[0] * input[1] - rotation[1] * input[0])
    }};
    output[0] = input[0] + rotation[3] * twiceCross[0] +
        rotation[1] * twiceCross[2] - rotation[2] * twiceCross[1];
    output[1] = input[1] + rotation[3] * twiceCross[1] +
        rotation[2] * twiceCross[0] - rotation[0] * twiceCross[2];
    output[2] = input[2] + rotation[3] * twiceCross[2] +
        rotation[0] * twiceCross[1] - rotation[1] * twiceCross[0];
}

bool normalizeRigVector(std::array<float, 3> &value) {
    const float length = std::sqrt(
        value[0] * value[0] + value[1] * value[1] +
        value[2] * value[2]);
    if (!std::isfinite(length) || length < 1.0e-8f) return false;
    for (float &component : value) component /= length;
    return true;
}

std::array<float, 4> sourcePresentationRotation(uint32_t sourceForward) {
    constexpr float halfRoot = 0.7071067811865475244f;
    static const std::array<std::array<float, 4>, 4> rotations{{
        {{0.0f, 0.0f, 0.0f, 1.0f}},
        {{0.0f, 1.0f, 0.0f, 0.0f}},
        {{0.0f, -halfRoot, 0.0f, halfRoot}},
        {{0.0f, halfRoot, 0.0f, halfRoot}},
    }};
    return sourceForward < rotations.size() ? rotations[sourceForward]
                                             : rotations[0];
}

bool proposeGeometryRigMap(
    const std::vector<CharacterWorkshopRigJoint> &joints,
    uint32_t sourceForward, std::array<int, 16> &mapping) {
    if (joints.size() < 16u || sourceForward > 3u) return false;
    const std::array<float, 4> presentation =
        sourcePresentationRotation(sourceForward);
    std::vector<std::array<float, 3>> positions(joints.size());
    std::vector<unsigned> childCount(joints.size(), 0u);
    float minimum[3] = {INFINITY, INFINITY, INFINITY};
    float maximum[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (size_t joint = 0u; joint < joints.size(); ++joint) {
        rotateRigVector(presentation, joints[joint].bindPosition,
                        positions[joint]);
        for (unsigned axis = 0u; axis < 3u; ++axis) {
            minimum[axis] = std::min(minimum[axis], positions[joint][axis]);
            maximum[axis] = std::max(maximum[axis], positions[joint][axis]);
        }
        if (joints[joint].parent >= 0) {
            ++childCount[static_cast<size_t>(joints[joint].parent)];
        }
    }
    const float width = maximum[0] - minimum[0];
    const float height = maximum[1] - minimum[1];
    if (!std::isfinite(width) || !std::isfinite(height) ||
        width < 1.0e-5f || height < 1.0e-5f) return false;
    const float centerX = (minimum[0] + maximum[0]) * 0.5f;
    const auto uniqueBest = [&](const auto &eligible,
                                const auto &score) {
        int best = -1;
        float bestScore = -INFINITY;
        float secondScore = -INFINITY;
        for (size_t joint = 0u; joint < joints.size(); ++joint) {
            if (!eligible(joint)) continue;
            const float candidate = score(joint);
            if (candidate > bestScore) {
                secondScore = bestScore;
                bestScore = candidate;
                best = static_cast<int>(joint);
            } else if (candidate > secondScore) {
                secondScore = candidate;
            }
        }
        // A close runner-up is real ambiguity, not a deterministic tie-break.
        return best >= 0 &&
            (!std::isfinite(secondScore) || bestScore - secondScore >= 0.03f)
            ? best : -1;
    };
    const auto leaf = [&](size_t joint) { return childCount[joint] == 0u; };
    const auto upper = [&](size_t joint) {
        return leaf(joint) &&
            positions[joint][1] > minimum[1] + height * 0.48f;
    };
    const auto lower = [&](size_t joint) {
        return leaf(joint) &&
            positions[joint][1] < minimum[1] + height * 0.40f;
    };
    const float sideThreshold = width * 0.12f;
    const int leftHand = uniqueBest(
        [&](size_t joint) {
            return upper(joint) &&
                positions[joint][0] > centerX + sideThreshold;
        },
        [&](size_t joint) {
            return (positions[joint][0] - centerX) / width +
                (positions[joint][1] - minimum[1]) / height * 0.08f;
        });
    const int rightHand = uniqueBest(
        [&](size_t joint) {
            return upper(joint) &&
                positions[joint][0] < centerX - sideThreshold;
        },
        [&](size_t joint) {
            return (centerX - positions[joint][0]) / width +
                (positions[joint][1] - minimum[1]) / height * 0.08f;
        });
    const int leftFoot = uniqueBest(
        [&](size_t joint) {
            return lower(joint) &&
                positions[joint][0] > centerX + sideThreshold * 0.25f;
        },
        [&](size_t joint) {
            return (maximum[1] - positions[joint][1]) / height +
                (positions[joint][0] - centerX) / width * 0.08f;
        });
    const int rightFoot = uniqueBest(
        [&](size_t joint) {
            return lower(joint) &&
                positions[joint][0] < centerX - sideThreshold * 0.25f;
        },
        [&](size_t joint) {
            return (maximum[1] - positions[joint][1]) / height +
                (centerX - positions[joint][0]) / width * 0.08f;
        });
    const int head = uniqueBest(
        [&](size_t joint) {
            return upper(joint) &&
                std::fabs(positions[joint][0] - centerX) < width * 0.20f;
        },
        [&](size_t joint) {
            return (positions[joint][1] - minimum[1]) / height -
                std::fabs(positions[joint][0] - centerX) / width * 0.08f;
        });
    if (leftHand < 0 || rightHand < 0 || leftFoot < 0 ||
        rightFoot < 0 || head < 0) return false;
    const auto bilateral = [&](int left, int right, float maximumYDelta,
                               float maximumSideDelta) {
        const float leftSide = positions[left][0] - centerX;
        const float rightSide = centerX - positions[right][0];
        return std::fabs(positions[left][1] - positions[right][1]) <=
                   height * maximumYDelta &&
            std::fabs(leftSide - rightSide) <= width * maximumSideDelta;
    };
    if (!bilateral(leftHand, rightHand, 0.20f, 0.20f) ||
        !bilateral(leftFoot, rightFoot, 0.12f, 0.20f)) return false;
    const int chest = rigLowestCommonAncestor(
        joints, {head, leftHand, rightHand});
    const int hips = rigLowestCommonAncestor(
        joints, {chest, leftFoot, rightFoot});
    if (chest < 0 || hips < 0 || chest == hips) return false;
    mapping.fill(-1);
    mapping[0] = hips;
    mapping[1] = rigChildOnPath(joints, hips, chest);
    mapping[2] = chest;
    mapping[3] = head;
    const struct {
        size_t upperRole;
        int endpoint;
        int root;
    } limbs[] = {
        {4u, leftHand, chest}, {7u, rightHand, chest},
        {10u, leftFoot, hips}, {13u, rightFoot, hips},
    };
    for (const auto &limb : limbs) {
        mapping[limb.upperRole] = rigChildOnPath(
            joints, limb.root, limb.endpoint);
        mapping[limb.upperRole + 1u] =
            joints[static_cast<size_t>(limb.endpoint)].parent;
        mapping[limb.upperRole + 2u] = limb.endpoint;
    }
    std::array<bool, 256> used{};
    for (int joint : mapping) {
        if (!rigJointValid(joints, joint) ||
            used[static_cast<size_t>(joint)]) return false;
        used[static_cast<size_t>(joint)] = true;
    }
    return true;
}

void proposeRigBases(const std::vector<CharacterWorkshopRigJoint> &joints,
                     uint32_t sourceForward,
                     CharacterWorkshopRigSuggestion &suggestion) {
    if (sourceForward > 3u) return;
    const std::array<float, 4> inversePresentation =
        conjugateRigQuaternion(sourcePresentationRotation(sourceForward));
    for (CharacterWorkshopRigRoleSuggestion &role : suggestion.roles) {
        if (!rigJointValid(joints, role.joint)) continue;
        std::array<float, 4> bind = joints[role.joint].bindRotation;
        if (!normalizeRigQuaternion(bind)) continue;
        multiplyRigQuaternion(conjugateRigQuaternion(bind),
                             inversePresentation, role.restRotation);
        if (!normalizeRigQuaternion(role.restRotation)) continue;
        role.restBasisAvailable = true;
        ++suggestion.restBasisRoles;
    }

    static const size_t limbs[][3] = {
        {4u, 5u, 6u}, {7u, 8u, 9u},
        {10u, 11u, 12u}, {13u, 14u, 15u},
    };
    for (const auto &limb : limbs) {
        const int upper = suggestion.roles[limb[0]].joint;
        const int middle = suggestion.roles[limb[1]].joint;
        const int end = suggestion.roles[limb[2]].joint;
        if (!rigJointValid(joints, upper) ||
            !rigJointValid(joints, middle) ||
            !rigJointValid(joints, end) ||
            !rigAncestorInclusive(joints, upper, middle) ||
            !rigAncestorInclusive(joints, middle, end)) continue;
        std::array<float, 3> first{{
            joints[middle].bindPosition[0] - joints[upper].bindPosition[0],
            joints[middle].bindPosition[1] - joints[upper].bindPosition[1],
            joints[middle].bindPosition[2] - joints[upper].bindPosition[2],
        }};
        std::array<float, 3> second{{
            joints[end].bindPosition[0] - joints[middle].bindPosition[0],
            joints[end].bindPosition[1] - joints[middle].bindPosition[1],
            joints[end].bindPosition[2] - joints[middle].bindPosition[2],
        }};
        const float firstLength = std::sqrt(
            first[0] * first[0] + first[1] * first[1] + first[2] * first[2]);
        const float secondLength = std::sqrt(
            second[0] * second[0] + second[1] * second[1] +
            second[2] * second[2]);
        std::array<float, 3> normal{{
            first[1] * second[2] - first[2] * second[1],
            first[2] * second[0] - first[0] * second[2],
            first[0] * second[1] - first[1] * second[0],
        }};
        const float normalLength = std::sqrt(
            normal[0] * normal[0] + normal[1] * normal[1] +
            normal[2] * normal[2]);
        // Below roughly two degrees the plane is too sensitive to export
        // noise; the runtime's stable automatic fallback is more truthful.
        if (!std::isfinite(firstLength) || !std::isfinite(secondLength) ||
            firstLength < 1.0e-6f || secondLength < 1.0e-6f ||
            normalLength / (firstLength * secondLength) < 0.035f ||
            !normalizeRigVector(normal)) continue;
        for (size_t roleIndex : {limb[0], limb[1]}) {
            CharacterWorkshopRigRoleSuggestion &role =
                suggestion.roles[roleIndex];
            std::array<float, 4> bind = joints[role.joint].bindRotation;
            if (!normalizeRigQuaternion(bind)) continue;
            rotateRigVector(conjugateRigQuaternion(bind), normal,
                            role.bendAxis);
            if (!normalizeRigVector(role.bendAxis)) continue;
            role.bendAxisAvailable = true;
            ++suggestion.bendAxisRoles;
        }
    }
}

} // namespace

uint32_t CharacterWorkshop_nextSceneReview(
    uint32_t sceneCount, uint32_t justCompletedScene,
    uint32_t currentSceneMask, bool refreshAll) {
    if (sceneCount == 0u || sceneCount > 32u ||
        justCompletedScene >= sceneCount) return sceneCount;
    if (refreshAll) {
        return justCompletedScene + 1u < sceneCount
            ? justCompletedScene + 1u : sceneCount;
    }
    for (uint32_t scene = 0u; scene < sceneCount; ++scene) {
        if ((currentSceneMask & (1u << scene)) == 0u) return scene;
    }
    return sceneCount;
}

CharacterWorkshopReadiness CharacterWorkshop_evaluate(
    const CharacterWorkshopFacts &facts) {
    CharacterWorkshopReadiness result;

    setRow(result, CharacterWorkshopReadinessId::Identity, facts.identityReady ? CharacterWorkshopReadinessStatus::Ready : CharacterWorkshopReadinessStatus::Missing, CharacterWorkshopTab::Identity);

    CharacterWorkshopReadinessStatus calibration =
        CharacterWorkshopReadinessStatus::Ready;
    if (!facts.geometryAvailable || !facts.anchorsReady ||
        !facts.attachmentSocketsReady) {
        calibration = CharacterWorkshopReadinessStatus::Missing;
    } else if (!facts.normalized) {
        calibration = CharacterWorkshopReadinessStatus::Review;
    }
    setRow(result, CharacterWorkshopReadinessId::Calibration, calibration, CharacterWorkshopTab::Vehicles);

    CharacterWorkshopReadinessStatus rigMotion =
        CharacterWorkshopReadinessStatus::Ready;
    if (!facts.motionReady) {
        rigMotion = facts.rigPresent
                        ? CharacterWorkshopReadinessStatus::Review
                        : CharacterWorkshopReadinessStatus::Missing;
    } else if (facts.rigPresent && !facts.rigReviewed) {
        // Complete authored motion can play without retargeting. An unreviewed
        // optional rig remains visible work, but is not misreported as absent.
        rigMotion = CharacterWorkshopReadinessStatus::Review;
    }
    setRow(result, CharacterWorkshopReadinessId::RigMotion, rigMotion, CharacterWorkshopTab::RigMotion);

    setRow(result, CharacterWorkshopReadinessId::GameplayProfile, facts.donorQualified ? CharacterWorkshopReadinessStatus::Ready : CharacterWorkshopReadinessStatus::Unavailable, CharacterWorkshopTab::Profile);

    CharacterWorkshopReadinessStatus vehicleFit =
        CharacterWorkshopReadinessStatus::Ready;
    if ((facts.supportedVehicleMask & 7u) == 0u) {
        vehicleFit = CharacterWorkshopReadinessStatus::Missing;
    } else {
        const uint32_t required = requiredContextMask(
            facts.supportedVehicleMask);
        if ((facts.reviewedContextMask & required) != required) {
            vehicleFit = CharacterWorkshopReadinessStatus::Review;
        }
    }
    setRow(result, CharacterWorkshopReadinessId::VehicleFit, vehicleFit, CharacterWorkshopTab::Vehicles);

    CharacterWorkshopReadinessStatus performance =
        CharacterWorkshopReadinessStatus::Ready;
    if (!facts.performanceAssemblyReady) {
        performance = CharacterWorkshopReadinessStatus::Missing;
    } else if (facts.performance ==
               CharacterWorkshopPerformanceState::OverTargetAccepted) {
        performance = CharacterWorkshopReadinessStatus::Accepted;
    } else if (facts.performance !=
               CharacterWorkshopPerformanceState::TargetMet) {
        performance = CharacterWorkshopReadinessStatus::Review;
    }
    setRow(result, CharacterWorkshopReadinessId::Performance, performance,
           CharacterWorkshopTab::Performance);

    result.readyToPreview = facts.geometryAvailable;
    result.readyToEnable  = facts.geometryAvailable && facts.identityReady &&
                            facts.normalized && facts.anchorsReady &&
                            facts.attachmentSocketsReady && facts.motionReady &&
                            facts.donorQualified &&
                            vehicleFit == CharacterWorkshopReadinessStatus::Ready &&
                            (performance == CharacterWorkshopReadinessStatus::Ready ||
                             performance == CharacterWorkshopReadinessStatus::Accepted);
    result.readyToPlay    = result.readyToEnable && facts.enabled;

    if (!facts.identityReady) {
        result.nextActionTab   = CharacterWorkshopTab::Identity;
        result.nextActionLabel = "Create roster identity";
    } else if (!facts.geometryAvailable || !facts.normalized ||
               !facts.anchorsReady || !facts.attachmentSocketsReady) {
        result.nextActionTab   = CharacterWorkshopTab::Vehicles;
        result.nextActionLabel = "Calibrate model and anchors";
    } else if (!facts.motionReady ||
               (facts.rigPresent && !facts.rigReviewed)) {
        result.nextActionTab   = CharacterWorkshopTab::RigMotion;
        result.nextActionLabel = "Review rig and motion";
    } else if (!facts.donorQualified) {
        result.nextActionTab   = CharacterWorkshopTab::Profile;
        result.nextActionLabel = "Choose a qualified gameplay profile";
    } else if (vehicleFit != CharacterWorkshopReadinessStatus::Ready) {
        result.nextActionTab   = CharacterWorkshopTab::Vehicles;
        result.nextActionLabel = "Review every supported context";
    } else if (!facts.performanceAssemblyReady) {
        result.nextActionTab   = CharacterWorkshopTab::Performance;
        result.nextActionLabel = "Complete performance assembly";
    } else if (facts.performance ==
               CharacterWorkshopPerformanceState::NotMeasured) {
        result.nextActionTab   = CharacterWorkshopTab::Test;
        result.nextActionLabel = "Run the performance matrix";
    } else if (facts.performance ==
               CharacterWorkshopPerformanceState::OverTarget) {
        result.nextActionTab   = CharacterWorkshopTab::Performance;
        result.nextActionLabel = "Tune performance to target";
    } else if (!facts.enabled) {
        result.nextActionTab   = CharacterWorkshopTab::Package;
        result.nextActionLabel = "Enable validated character";
    } else {
        result.nextActionTab   = CharacterWorkshopTab::Test;
        result.nextActionLabel = "Run an exact-context test";
    }
    return result;
}

const char *CharacterWorkshop_tabLabel(CharacterWorkshopTab tab) {
    static constexpr const char *kLabels[] = {
        "Overview",
        "Identity",
        "Rig & Motion",
        "Gameplay",
        "Offset Studio",
        "Performance",
        "Test",
        "Package",
    };
    const size_t index = static_cast<size_t>(tab);
    return index < static_cast<size_t>(CharacterWorkshopTab::Count)
               ? kLabels[index]
               : "Overview";
}

const char *CharacterWorkshop_tabStorageId(CharacterWorkshopTab tab) {
    static constexpr const char *kIds[] = {
        "overview",
        "identity",
        "rig-motion",
        "profile",
        "vehicles",
        "performance",
        "test",
        "package",
    };
    const size_t index = static_cast<size_t>(tab);
    return index < static_cast<size_t>(CharacterWorkshopTab::Count)
               ? kIds[index]
               : "overview";
}

CharacterWorkshopTab CharacterWorkshop_parseTab(const char *stored) {
    if (stored == nullptr) return CharacterWorkshopTab::Overview;
    for (size_t index = 0u;
         index < static_cast<size_t>(CharacterWorkshopTab::Count);
         ++index) {
        const CharacterWorkshopTab tab =
            static_cast<CharacterWorkshopTab>(index);
        if (std::strcmp(stored, CharacterWorkshop_tabStorageId(tab)) == 0) {
            return tab;
        }
    }
    return CharacterWorkshopTab::Overview;
}

const char *CharacterWorkshop_readinessLabel(
    CharacterWorkshopReadinessId id) {
    static constexpr const char *kLabels[] = {
        "Roster identity",
        "Calibration and anchors",
        "Rig and motion",
        "Gameplay profile",
        "Vehicle fit",
        "Performance evidence",
    };
    const size_t index = static_cast<size_t>(id);
    return index < static_cast<size_t>(CharacterWorkshopReadinessId::Count)
               ? kLabels[index]
               : "Unknown requirement";
}

const char *CharacterWorkshop_statusLabel(
    CharacterWorkshopReadinessStatus status) {
    static constexpr const char *kLabels[] = {
        "Ready",
        "Exception",
        "Review",
        "Missing",
        "Unavailable",
    };
    const size_t index = static_cast<size_t>(status);
    return index < 5u ? kLabels[index] : "Unavailable";
}

const CharacterWorkshopPerformancePreset *
CharacterWorkshop_performancePreset(
    CharacterWorkshopPerformanceTarget target) {
    static constexpr CharacterWorkshopPerformancePreset kPresets[] = {
        {"Quality",
         "Keeps the most detailed authored LOD longer for a one-player layout.",
         1, 2.0f},
        {"Balanced",
         "Uses authored distance bands unchanged and inspects a two-player layout.",
         2, 0.0f},
        {"Performance",
         "Moves two authored LOD bands toward lower geometry for a one-player layout.",
         1, -2.0f},
        {"Four-player",
         "Uses the strongest bounded lower-detail preference and a four-player layout.",
         4, -3.0f},
    };
    const size_t index = static_cast<size_t>(target);
    return index < static_cast<size_t>(
                       CharacterWorkshopPerformanceTarget::Count)
        ? &kPresets[index] : nullptr;
}

CharacterWorkshopPerformanceTarget CharacterWorkshop_performanceTarget(
    int players, float lodBias) {
    if (!std::isfinite(lodBias)) {
        return CharacterWorkshopPerformanceTarget::Count;
    }
    for (size_t index = 0u;
         index < static_cast<size_t>(
                     CharacterWorkshopPerformanceTarget::Count);
         ++index) {
        const CharacterWorkshopPerformancePreset *preset =
            CharacterWorkshop_performancePreset(
                static_cast<CharacterWorkshopPerformanceTarget>(index));
        if (preset != nullptr && players == preset->players &&
            std::fabs(lodBias - preset->lodBias) <= 1.0e-6f) {
            return static_cast<CharacterWorkshopPerformanceTarget>(index);
        }
    }
    return CharacterWorkshopPerformanceTarget::Count;
}

uint32_t CharacterWorkshop_selectLod(
    float viewDistance, float sourceLodBias, float localLodBias,
    uint32_t authoredLodMask) {
    return mdkr_modern_character_select_lod(
        viewDistance, sourceLodBias, localLodBias, authoredLodMask);
}

size_t CharacterWorkshop_lodBands(
    float sourceLodBias, float localLodBias, uint32_t authoredLodMask,
    CharacterWorkshopLodBand output[MDKR_MODERN_CHARACTER_LOD_LEVELS]) {
    static constexpr float minimums[] = {0.0f, 650.0f, 1300.0f, 2400.0f};
    static constexpr float maximums[] = {
        650.0f, 1300.0f, 2400.0f, INFINITY,
    };
    CharacterWorkshopLodBand resolved[MDKR_MODERN_CHARACTER_LOD_LEVELS];
    size_t count = 0u;
    if (output == nullptr) return 0u;
    for (size_t base = 0u; base < MDKR_MODERN_CHARACTER_LOD_LEVELS; ++base) {
        const uint32_t selected = CharacterWorkshop_selectLod(
            minimums[base], sourceLodBias, localLodBias, authoredLodMask);
        if (selected == UINT32_MAX) return 0u;
        if (count != 0u && resolved[count - 1u].lod == selected) {
            resolved[count - 1u].maximumDistance = maximums[base];
        } else {
            resolved[count++] = {
                minimums[base], maximums[base], selected,
            };
        }
    }
    std::copy(resolved, resolved + count, output);
    return count;
}

CharacterWorkshopFitSuggestion CharacterWorkshop_suggestFit(
    const CharacterWorkshopFitMeasurement &measurement) {
    CharacterWorkshopFitSuggestion result;
    if (!measurement.valid) return result;

    constexpr int64_t kMaximumCoordinateMicrometres = 1000000000LL;
    for (size_t axis = 0u; axis < 3u; ++axis) {
        const int64_t minimum = measurement.boundsMinimumMicrometres[axis];
        const int64_t maximum = measurement.boundsMaximumMicrometres[axis];
        if (minimum < -kMaximumCoordinateMicrometres ||
            maximum > kMaximumCoordinateMicrometres || minimum > maximum) {
            return result;
        }
    }
    const int64_t heightMicrometres =
        measurement.boundsMaximumMicrometres[1] -
        measurement.boundsMinimumMicrometres[1];
    // Reject a degenerate measurement and absurd target-space extents rather
    // than producing a plausible-looking correction from corrupt evidence.
    if (heightMicrometres < 10000LL || heightMicrometres > 100000000LL) {
        return result;
    }

    result.available = true;
    result.measuredHeightMetres =
        static_cast<float>(heightMicrometres) / 1000000.0f;
    result.measuredMinimumYMetres =
        static_cast<float>(measurement.boundsMinimumMicrometres[1]) /
        1000000.0f;
    // Selection uses a literal floor datum. For a vehicle, keeping roughly the
    // lower quarter of a standing-height envelope below the donor seat is a
    // deliberately conservative visibility-first starting point. The exact
    // in-game preview remains the authority because a bounds box cannot see a
    // vehicle shell, a seated pose, or costume-specific silhouette details.
    result.targetMinimumYMetres = measurement.vehicleContext
        ? -0.25f * result.measuredHeightMetres : 0.0f;
    result.verticalDeltaMetres =
        result.targetMinimumYMetres - result.measuredMinimumYMetres;
    if (std::fabs(result.verticalDeltaMetres) < 0.005f) {
        result.verticalDeltaMetres = 0.0f;
    } else {
        result.verticalAdjustment = true;
    }

    const float forwardX =
        static_cast<float>(measurement.forwardMilli[0]);
    const float forwardZ =
        static_cast<float>(measurement.forwardMilli[2]);
    const float horizontalLength = std::hypot(forwardX, forwardZ);
    if (horizontalLength >= 500.0f) {
        result.facingMeasured = true;
        result.yawDeltaDegrees =
            -std::atan2(forwardX, forwardZ) * 57.295779513082320876f;
        if (result.yawDeltaDegrees <= -180.0f) {
            result.yawDeltaDegrees = 180.0f;
        }
        if (std::fabs(result.yawDeltaDegrees) < 2.0f) {
            result.yawDeltaDegrees = 0.0f;
        } else {
            result.facingAdjustment = true;
        }
    }
    return result;
}

CharacterWorkshopContactSuggestion CharacterWorkshop_suggestContacts(
    const std::array<std::array<float, 3>, 4> &currentContacts,
    const CharacterWorkshopContactMeasurement *measurements,
    size_t measurementCount) {
    CharacterWorkshopContactSuggestion result;
    constexpr size_t kMaximumMeasurements = 64u;
    constexpr int64_t kCoordinateLimitMicrometres = INT64_C(1000000000);
    constexpr double kContactLimitMetres = 1.0;
    if (measurementCount > kMaximumMeasurements ||
        (measurementCount != 0u && measurements == nullptr)) return result;
    for (const auto &contact : currentContacts) {
        for (float coordinate : contact) {
            if (!std::isfinite(coordinate) ||
                std::fabs(coordinate) > kContactLimitMetres) return result;
        }
    }
    std::array<std::array<int64_t, 3>, 4> sums{};
    for (size_t sampleIndex = 0u; sampleIndex < measurementCount;
         ++sampleIndex) {
        const CharacterWorkshopContactMeasurement &sample =
            measurements[sampleIndex];
        if ((sample.witnessMask & ~0xFu) != 0u) return result;
        if (sample.witnessMask != 0u) ++result.witnessSamples;
        for (size_t contact = 0u; contact < 4u; ++contact) {
            if ((sample.witnessMask & (1u << contact)) == 0u) continue;
            ++result.sampleCounts[contact];
            result.contactMask |= 1u << contact;
            for (size_t axis = 0u; axis < 3u; ++axis) {
                const int64_t target =
                    sample.targetMicrometres[contact][axis];
                const int64_t endpoint =
                    sample.endpointMicrometres[contact][axis];
                if (target < -kCoordinateLimitMicrometres ||
                    target > kCoordinateLimitMicrometres ||
                    endpoint < -kCoordinateLimitMicrometres ||
                    endpoint > kCoordinateLimitMicrometres) {
                    return CharacterWorkshopContactSuggestion{};
                }
                sums[contact][axis] += endpoint - target;
            }
        }
    }
    result.valid = true;
    result.available = result.contactMask != 0u;
    result.withinLimits = result.available;
    for (size_t contact = 0u; contact < 4u; ++contact) {
        if (result.sampleCounts[contact] == 0u) continue;
        for (size_t axis = 0u; axis < 3u; ++axis) {
            const double delta =
                static_cast<double>(sums[contact][axis]) /
                static_cast<double>(result.sampleCounts[contact]) /
                1000000.0;
            result.deltaMetres[contact][axis] =
                static_cast<float>(delta);
            result.adjustment |= std::fabs(delta) >= 0.0000005;
            const double proposed =
                static_cast<double>(currentContacts[contact][axis]) + delta;
            if (!std::isfinite(proposed) ||
                std::fabs(proposed) > kContactLimitMetres) {
                result.withinLimits = false;
            }
        }
    }
    return result;
}

CharacterWorkshopFitAssessment CharacterWorkshop_assessFit(
    const CharacterWorkshopFitMeasurement &measurement) {
    CharacterWorkshopFitAssessment result;
    const CharacterWorkshopFitSuggestion suggestion =
        CharacterWorkshop_suggestFit(measurement);
    if (!suggestion.available) return result;
    const int64_t widthMicrometres =
        measurement.boundsMaximumMicrometres[0] -
        measurement.boundsMinimumMicrometres[0];
    const int64_t depthMicrometres =
        measurement.boundsMaximumMicrometres[2] -
        measurement.boundsMinimumMicrometres[2];
    if (widthMicrometres < 0 || depthMicrometres < 0) return result;

    result.valid = true;
    result.vehicleContext = measurement.vehicleContext;
    result.datumErrorMetres = std::fabs(
        suggestion.measuredMinimumYMetres -
        suggestion.targetMinimumYMetres);
    const float datumReview = measurement.vehicleContext
        ? std::max(0.025f, suggestion.measuredHeightMetres * 0.05f)
        : 0.005f;
    const float datumCritical = measurement.vehicleContext
        ? std::max(0.075f, suggestion.measuredHeightMetres * 0.15f)
        : 0.025f;
    result.datum = result.datumErrorMetres <= datumReview
        ? CharacterWorkshopQualitySeverity::Nominal
        : result.datumErrorMetres <= datumCritical
            ? CharacterWorkshopQualitySeverity::Review
            : CharacterWorkshopQualitySeverity::Critical;

    const float forwardX =
        static_cast<float>(measurement.forwardMilli[0]);
    const float forwardZ =
        static_cast<float>(measurement.forwardMilli[2]);
    const float horizontalLength = std::hypot(forwardX, forwardZ);
    if (horizontalLength >= 500.0f) {
        result.facingMeasured = true;
        result.facingDegrees = std::acos(std::clamp(
            forwardZ / horizontalLength, -1.0f, 1.0f)) *
            57.295779513082320876f;
        result.facing = result.facingDegrees <= 5.0f
            ? CharacterWorkshopQualitySeverity::Nominal
            : result.facingDegrees <= 20.0f
                ? CharacterWorkshopQualitySeverity::Review
                : CharacterWorkshopQualitySeverity::Critical;
    } else {
        result.facing = CharacterWorkshopQualitySeverity::Review;
    }

    result.widthToHeight = static_cast<float>(widthMicrometres) /
        (suggestion.measuredHeightMetres * 1000000.0f);
    result.depthToHeight = static_cast<float>(depthMicrometres) /
        (suggestion.measuredHeightMetres * 1000000.0f);
    const float largestRatio = std::max(
        result.widthToHeight, result.depthToHeight);
    result.proportions = largestRatio <= 1.5f
        ? CharacterWorkshopQualitySeverity::Nominal
        : largestRatio <= 2.5f
            ? CharacterWorkshopQualitySeverity::Review
            : CharacterWorkshopQualitySeverity::Critical;
    return result;
}

CharacterWorkshopFitReviewDecision CharacterWorkshop_reviewFit(
    const CharacterWorkshopFitReviewFacts &facts) {
    CharacterWorkshopFitReviewDecision result;
    result.visibilityBlocksReview = facts.exactContract &&
        facts.opaqueVisibilityQualified &&
        facts.isolatedVisibleTiles == 0u;
    const bool visibilityWarning = facts.exactContract &&
        !result.visibilityBlocksReview &&
        (!facts.opaqueVisibilityQualified ||
         static_cast<uint64_t>(facts.sceneVisibleTiles) * 100u <
             static_cast<uint64_t>(facts.isolatedVisibleTiles) * 60u);
    result.warnings = facts.advisoryFitWarning || facts.cameraWarning ||
        facts.surfaceWarning || facts.attachmentVisibilityWarning ||
        visibilityWarning;
    result.ready = facts.exactContract &&
        !result.visibilityBlocksReview && facts.sceneReviewed &&
        (!facts.contactExceptionRequired ||
         facts.contactExceptionApproved);
    return result;
}

bool CharacterWorkshop_facingCorrectionDegrees(
    uint32_t sourceForward, float &outputDegrees) {
    static constexpr float kCorrections[] = {
        0.0f, 180.0f, -90.0f, 90.0f,
    };
    if (sourceForward >= std::size(kCorrections)) return false;
    outputDegrees = kCorrections[sourceForward];
    return true;
}

CharacterWorkshopSourceTransformReview CharacterWorkshop_reviewSourceTransform(
    const CharacterWorkshopSourceTransformFacts &facts) {
    CharacterWorkshopSourceTransformReview result;
    for (size_t axis = 0u; axis < 3u; ++axis) {
        if (!std::isfinite(facts.meshLocalMinimum[axis]) ||
            !std::isfinite(facts.meshLocalMaximum[axis]) ||
            !std::isfinite(facts.sceneWorldMinimum[axis]) ||
            !std::isfinite(facts.sceneWorldMaximum[axis]) ||
            facts.meshLocalMinimum[axis] > facts.meshLocalMaximum[axis] ||
            facts.sceneWorldMinimum[axis] > facts.sceneWorldMaximum[axis]) {
            return result;
        }
    }
    if (!std::isfinite(facts.targetHeightMetres) ||
        facts.targetHeightMetres < 0.1 || facts.targetHeightMetres > 10.0) {
        return result;
    }
    result.sceneWorldHeightMetres =
        facts.sceneWorldMaximum[1] - facts.sceneWorldMinimum[1];
    double localSpanSquared = 0.0;
    double worldSpanSquared = 0.0;
    for (size_t axis = 0u; axis < 3u; ++axis) {
        const double localExtent =
            facts.meshLocalMaximum[axis] - facts.meshLocalMinimum[axis];
        const double worldExtent =
            facts.sceneWorldMaximum[axis] - facts.sceneWorldMinimum[axis];
        localSpanSquared += localExtent * localExtent;
        worldSpanSquared += worldExtent * worldExtent;
    }
    result.meshLocalSpan = std::sqrt(localSpanSquared);
    result.sceneWorldSpanMetres = std::sqrt(worldSpanSquared);
    if (result.meshLocalSpan <= 1.0e-9 ||
        result.sceneWorldSpanMetres <= 1.0e-9 ||
        result.sceneWorldHeightMetres <= 1.0e-9) {
        return result;
    }
    result.valid = true;
    result.hierarchyScale =
        result.sceneWorldSpanMetres / result.meshLocalSpan;
    result.normalizeToOneMultiplier = 1.0 / result.sceneWorldHeightMetres;
    result.targetHeightMultiplier =
        facts.targetHeightMetres / result.sceneWorldHeightMetres;
    result.sceneGroundYMetres = facts.sceneWorldMinimum[1];
    result.widthToHeight =
        (facts.sceneWorldMaximum[0] - facts.sceneWorldMinimum[0]) /
        result.sceneWorldHeightMetres;
    result.depthToHeight =
        (facts.sceneWorldMaximum[2] - facts.sceneWorldMinimum[2]) /
        result.sceneWorldHeightMetres;
    result.suspiciousWorldHeight =
        result.sceneWorldHeightMetres < 0.25 ||
        result.sceneWorldHeightMetres > 4.0;
    result.suspiciousHierarchyScale =
        result.hierarchyScale < 0.1 || result.hierarchyScale > 10.0;
    result.unusualProportions =
        result.widthToHeight > 2.5 || result.depthToHeight > 2.5;
    const bool extreme = result.sceneWorldHeightMetres < 0.025 ||
        result.sceneWorldHeightMetres > 40.0 ||
        result.hierarchyScale < 0.001 || result.hierarchyScale > 1000.0 ||
        result.targetHeightMultiplier < 0.01 ||
        result.targetHeightMultiplier > 100.0;
    result.severity = extreme
        ? CharacterWorkshopTransformSeverity::Critical
        : result.suspiciousWorldHeight || result.suspiciousHierarchyScale
            ? CharacterWorkshopTransformSeverity::Review
            : CharacterWorkshopTransformSeverity::Nominal;
    return result;
}

CharacterWorkshopRigSuggestion CharacterWorkshop_suggestHumanoidRig(
    const std::vector<CharacterWorkshopRigJoint> &joints,
    uint32_t sourceForward) {
    CharacterWorkshopRigSuggestion result;
    if (joints.empty() || joints.size() > 256u || sourceForward > 3u) {
        return result;
    }
    for (size_t joint = 0u; joint < joints.size(); ++joint) {
        const int parent = joints[joint].parent;
        if (parent < -1 || parent >= static_cast<int>(joints.size()) ||
            parent == static_cast<int>(joint) ||
            !std::isfinite(joints[joint].bindPosition[0]) ||
            !std::isfinite(joints[joint].bindPosition[1]) ||
            !std::isfinite(joints[joint].bindPosition[2]) ||
            !std::all_of(joints[joint].bindRotation.begin(),
                         joints[joint].bindRotation.end(),
                         [](float value) { return std::isfinite(value); })) {
            return result;
        }
        std::vector<bool> seen(joints.size(), false);
        int current = static_cast<int>(joint);
        for (size_t depth = 0u; rigJointValid(joints, current) &&
             depth <= joints.size(); ++depth) {
            if (seen[static_cast<size_t>(current)]) return result;
            seen[static_cast<size_t>(current)] = true;
            current = joints[static_cast<size_t>(current)].parent;
        }
    }

    std::array<bool, 16> ambiguous{};
    for (size_t role = 0u; role < result.roles.size(); ++role) {
        float best = 0.0f;
        int bestJoint = -1;
        bool tied = false;
        for (size_t joint = 0u; joint < joints.size(); ++joint) {
            const float score = rigNameScore(
                role, normalizedRigName(joints[joint].name));
            if (score > best + 1.0e-6f) {
                best = score;
                bestJoint = static_cast<int>(joint);
                tied = false;
            } else if (score > 0.0f && std::fabs(score - best) <= 1.0e-6f) {
                tied = true;
            }
        }
        if (bestJoint >= 0 && !tied &&
            !rigSuggestionUses(result, role, bestJoint)) {
            result.roles[role] = {
                bestJoint, best, CharacterWorkshopRigEvidence::Name};
            ++result.namedRoles;
        } else {
            ambiguous[role] = tied;
        }
    }

    std::array<int, 16> geometryMapping{};
    if (proposeGeometryRigMap(joints, sourceForward, geometryMapping)) {
        for (size_t role = 0u; role < result.roles.size(); ++role) {
            if (result.roles[role].joint >= 0 ||
                rigSuggestionUses(result, role, geometryMapping[role])) {
                continue;
            }
            result.roles[role] = {
                geometryMapping[role], 0.72f,
                CharacterWorkshopRigEvidence::GeometrySymmetry};
            ambiguous[role] = false;
            ++result.geometryRoles;
        }
    }

    // A pelvis-shaped helper may be a sibling of the torso and legs. Repair
    // that common exporter pattern with the nearest skin-joint ancestor shared
    // by the three canonical branches; never force the name-only candidate.
    const int spine = result.roles[1].joint;
    const int leftLeg = result.roles[10].joint;
    const int rightLeg = result.roles[13].joint;
    if (spine >= 0 && leftLeg >= 0 && rightLeg >= 0) {
        const int hips = result.roles[0].joint;
        const bool namedHipsValid = hips >= 0 &&
            rigAncestorInclusive(joints, hips, spine) &&
            rigAncestorInclusive(joints, hips, leftLeg) &&
            rigAncestorInclusive(joints, hips, rightLeg);
        if (!namedHipsValid) {
            const int common = rigLowestCommonAncestor(
                joints, {spine, leftLeg, rightLeg});
            if (common >= 0 && !rigSuggestionUses(result, 0u, common)) {
                if (result.roles[0].joint >= 0) {
                    removeRigSuggestionEvidence(result, 0u);
                }
                result.roles[0] = {
                    common, 0.86f,
                    CharacterWorkshopRigEvidence::HierarchyCommonAncestor};
                ++result.hierarchyRoles;
                ++result.commonAncestorRepairs;
            }
        }
    }

    const auto fillParent = [&](size_t role, size_t childRole,
                                size_t ancestorRole) {
        if (result.roles[role].joint >= 0 || ambiguous[role]) return;
        const int child = result.roles[childRole].joint;
        const int ancestor = result.roles[ancestorRole].joint;
        if (!rigAncestorInclusive(joints, ancestor, child)) return;
        const int parent = rigJointValid(joints, child)
            ? joints[static_cast<size_t>(child)].parent : -1;
        if (parent != ancestor) {
            setHierarchySuggestion(
                result, role, parent, 0.62f,
                CharacterWorkshopRigEvidence::HierarchyChain);
        }
    };
    const auto fillChild = [&](size_t role, size_t ancestorRole,
                               size_t descendantRole) {
        if (result.roles[role].joint >= 0 || ambiguous[role]) return;
        const int candidate = rigChildOnPath(
            joints, result.roles[ancestorRole].joint,
            result.roles[descendantRole].joint);
        setHierarchySuggestion(
            result, role, candidate, 0.58f,
            CharacterWorkshopRigEvidence::HierarchyChain);
    };

    // Fill only a gap bracketed by already identified anatomy. These are
    // intentionally lower-confidence proposals because helper/twist joints
    // make a purely structural choice review-worthy.
    fillChild(1u, 0u, 2u);  // spine between hips and chest
    fillParent(2u, 3u, 1u); // chest below head and above spine
    fillParent(5u, 6u, 4u);
    fillParent(8u, 9u, 7u);
    fillParent(11u, 12u, 10u);
    fillParent(14u, 15u, 13u);
    fillChild(4u, 2u, 5u);
    fillChild(7u, 2u, 8u);
    fillChild(10u, 0u, 11u);
    fillChild(13u, 0u, 14u);

    result.complete = std::all_of(
        result.roles.begin(), result.roles.end(), [](const auto &role) {
            return role.joint >= 0;
        });
    static const size_t hierarchy[][2] = {
        {0u, 1u}, {1u, 2u}, {2u, 3u},
        {2u, 4u}, {4u, 5u}, {5u, 6u},
        {2u, 7u}, {7u, 8u}, {8u, 9u},
        {0u, 10u}, {10u, 11u}, {11u, 12u},
        {0u, 13u}, {13u, 14u}, {14u, 15u},
    };
    result.hierarchyValid = result.complete && std::all_of(
        std::begin(hierarchy), std::end(hierarchy), [&](const auto &edge) {
            return rigAncestorInclusive(
                joints, result.roles[edge[0]].joint,
                result.roles[edge[1]].joint);
        });
    proposeRigBases(joints, sourceForward, result);
    return result;
}

CharacterWorkshopRigSuggestion CharacterWorkshop_suggestHumanoidBases(
    const std::vector<CharacterWorkshopRigJoint> &joints,
    const std::array<int, 16> &roleJoints, uint32_t sourceForward) {
    CharacterWorkshopRigSuggestion result;
    if (joints.empty() || joints.size() > 256u || sourceForward > 3u) {
        return result;
    }
    for (size_t joint = 0u; joint < joints.size(); ++joint) {
        if (joints[joint].parent < -1 ||
            joints[joint].parent >= static_cast<int>(joints.size()) ||
            joints[joint].parent == static_cast<int>(joint) ||
            !std::all_of(joints[joint].bindPosition.begin(),
                         joints[joint].bindPosition.end(),
                         [](float value) { return std::isfinite(value); }) ||
            !std::all_of(joints[joint].bindRotation.begin(),
                         joints[joint].bindRotation.end(),
                         [](float value) { return std::isfinite(value); })) {
            return result;
        }
        std::vector<bool> seen(joints.size(), false);
        int current = static_cast<int>(joint);
        for (size_t depth = 0u; rigJointValid(joints, current) &&
             depth <= joints.size(); ++depth) {
            if (seen[static_cast<size_t>(current)]) return {};
            seen[static_cast<size_t>(current)] = true;
            current = joints[static_cast<size_t>(current)].parent;
        }
    }
    std::array<bool, 256> used{};
    for (size_t role = 0u; role < roleJoints.size(); ++role) {
        const int joint = roleJoints[role];
        if (joint < -1 || joint >= static_cast<int>(joints.size()) ||
            (joint >= 0 && used[static_cast<size_t>(joint)])) return {};
        result.roles[role].joint = joint;
        if (joint >= 0) used[static_cast<size_t>(joint)] = true;
    }
    result.complete = std::all_of(
        roleJoints.begin(), roleJoints.end(),
        [](int joint) { return joint >= 0; });
    static const size_t hierarchy[][2] = {
        {0u, 1u}, {1u, 2u}, {2u, 3u},
        {2u, 4u}, {4u, 5u}, {5u, 6u},
        {2u, 7u}, {7u, 8u}, {8u, 9u},
        {0u, 10u}, {10u, 11u}, {11u, 12u},
        {0u, 13u}, {13u, 14u}, {14u, 15u},
    };
    result.hierarchyValid = result.complete && std::all_of(
        std::begin(hierarchy), std::end(hierarchy), [&](const auto &edge) {
            return rigAncestorInclusive(
                joints, roleJoints[edge[0]], roleJoints[edge[1]]);
        });
    proposeRigBases(joints, sourceForward, result);
    return result;
}

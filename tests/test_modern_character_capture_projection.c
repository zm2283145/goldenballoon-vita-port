#include "modern_character_capture_projection.h"
#include "modern_character_limits.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "test_modern_character_capture_projection: %s\n", message);
    exit(1);
}

static void identity(float matrix[16]) {
    memset(matrix, 0, sizeof(float) * 16u);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

int main(void) {
    MdkrModernCharacterCaptureProjection projection;
    float camera[16];
    float target[16];
    float composed[16];
    int32_t pixel[2];
    int32_t depth;
    uint32_t flags;
    identity(camera);
    identity(target);
    target[12] = 0.25f;
    target[13] = -0.50f;
    require(mdkr_modern_character_capture_projection_compose(
                camera, target, composed),
            "identity composition was refused");
    require(memcmp(target, composed, sizeof(composed)) == 0,
            "column-major composition changed an identity product");
    identity(camera);
    identity(target);
    camera[0] = 2.0f;
    camera[12] = 1.0f;
    target[12] = 3.0f;
    require(mdkr_modern_character_capture_projection_compose(
                camera, target, composed) && composed[0] == 2.0f &&
                composed[12] == 7.0f,
            "composition did not preserve camera-times-target order");
    composed[0] = NAN;
    memset(camera, 0x5a, sizeof(camera));
    require(!mdkr_modern_character_capture_projection_compose(
                composed, target, camera),
            "non-finite composition was accepted");
    for (size_t index = 0u; index < sizeof(camera); ++index) {
        require(((unsigned char *)camera)[index] == 0x5a,
                "failed composition partially changed output");
    }

    memset(&projection, 0, sizeof(projection));
    projection.version = MDKR_MODERN_CHARACTER_CAPTURE_PROJECTION_VERSION;
    projection.valid = 1u;
    projection.subject_player = 0u;
    projection.primitive_draws = 3u;
    projection.output_width = 1280u;
    projection.output_height = 720u;
    projection.viewport[0] = 160;
    projection.viewport[1] = 90;
    projection.viewport[2] = 960;
    projection.viewport[3] = 540;
    projection.scissor[0] = 160;
    projection.scissor[1] = 90;
    projection.scissor[2] = 960;
    projection.scissor[3] = 540;
    identity(projection.target_to_clip);
    require(mdkr_modern_character_capture_projection_valid(&projection),
            "valid bounded projection was refused");
    {
        const float origin[3] = {0.0f, 0.0f, 0.5f};
        require(mdkr_modern_character_capture_project_point(
                    &projection, origin, pixel, &depth, &flags),
                "visible origin was refused");
        require(pixel[0] == 640000 && pixel[1] == 360000 &&
                    depth == 500000 && flags == 0u,
                "origin projection did not use top-left viewport pixels");
    }
    {
        const float clipped[3] = {2.0f, 2.0f, 1.5f};
        require(mdkr_modern_character_capture_project_point(
                    &projection, clipped, pixel, &depth, &flags),
                "positive-W clipped point was refused");
        require((flags & (MDKR_MODERN_CHARACTER_PROJECTION_CLIP_RIGHT |
                          MDKR_MODERN_CHARACTER_PROJECTION_CLIP_TOP |
                          MDKR_MODERN_CHARACTER_PROJECTION_CLIP_FAR |
                          MDKR_MODERN_CHARACTER_PROJECTION_CLIP_SCISSOR)) ==
                    (MDKR_MODERN_CHARACTER_PROJECTION_CLIP_RIGHT |
                     MDKR_MODERN_CHARACTER_PROJECTION_CLIP_TOP |
                     MDKR_MODERN_CHARACTER_PROJECTION_CLIP_FAR |
                     MDKR_MODERN_CHARACTER_PROJECTION_CLIP_SCISSOR),
                "clipped point did not preserve explicit refusal flags");
    }
    projection.target_to_clip[15] = -1.0f;
    {
        const float behind[3] = {0.0f, 0.0f, 0.0f};
        require(!mdkr_modern_character_capture_project_point(
                    &projection, behind, pixel, &depth, &flags),
                "behind-camera point was projected");
    }
    projection.target_to_clip[15] = 1.0f;
    projection.subject_player = MDKR_MODERN_CHARACTER_PLAYERS;
    require(!mdkr_modern_character_capture_projection_valid(&projection),
            "out-of-range subject was accepted");
    projection.subject_player = 0u;
    projection.viewport[2] = 0;
    require(!mdkr_modern_character_capture_projection_valid(&projection),
            "empty viewport was accepted");
    puts("test_modern_character_capture_projection: PASS");
    return 0;
}

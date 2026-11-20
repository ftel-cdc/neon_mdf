#include "neon_camera_3a.h"

#include <string.h>

static uint32_t clamp_u32(uint32_t value, uint32_t low, uint32_t high)
{ return value < low ? low : (value > high ? high : value); }

static uint16_t clamp_u16(uint32_t value, uint16_t low, uint16_t high)
{ return (uint16_t)clamp_u32(value, low, high); }

void neon_camera_3a_profile_default(neon_camera_3a_profile_t *profile)
{
    if (!profile) return;
    *profile = (neon_camera_3a_profile_t){ 110u, 25u, 20u, 4u, 4000u, 256u, 2048u, 2u };
}

void neon_camera_3a_init(neon_camera_3a_t *algorithm, const neon_camera_3a_profile_t *profile)
{
    neon_camera_3a_profile_t defaults;
    if (!algorithm) return;
    neon_camera_3a_profile_default(&defaults);
    memset(algorithm, 0, sizeof(*algorithm));
    algorithm->profile = profile ? *profile : defaults;
    algorithm->current.exposure_lines = 400u;
    algorithm->current.analogue_gain_q8 = 256u;
    algorithm->current.digital_gain_q8 = 256u;
    algorithm->current.wb_red_gain_q8 = 256u;
    algorithm->current.wb_blue_gain_q8 = 256u;
    algorithm->initialized = 1u;
}

static int load_profile(neon_camera_3a_t *algorithm, const void *profile, uint32_t bytes)
{
    if (!algorithm || !profile || bytes != sizeof(neon_camera_3a_profile_t)) return -1;
    neon_camera_3a_init(algorithm, profile);
    return 0;
}

static int process(neon_camera_3a_t *algorithm, const neon_isp_statistics_t *statistics,
                   neon_camera_3a_controls_t *controls)
{
    int32_t error;
    uint32_t correction, red_gain, blue_gain;

    if (!algorithm || !statistics || !controls || !algorithm->initialized) return -1;
    /* A mean of zero is a measurement, not a missing one: a scene too dark for the 8-bit mean (a covered
     * lens, the first frame at a short exposure) must make exposure go up. Only a result that the ISP marks
     * as absent is skipped. */
    algorithm->current.apply_exposure = 0u;
    algorithm->current.apply_white_balance = 0u;
    algorithm->current.apply_focus = 0u;
    algorithm->current.target_frame_id = statistics->frame_id +
        algorithm->profile.control_latency_frames;
    if (statistics->mean_luma == 0u && !statistics->aec.valid) goto white_balance;
    error = (int32_t)algorithm->profile.target_luma - statistics->mean_luma;
    correction = (uint32_t)(error < 0 ? -error : error) *
        algorithm->profile.ae_speed_percent / 100u + 1u;
    if (error >= 0) algorithm->current.exposure_lines += correction;
    else if (algorithm->current.exposure_lines > correction)
        algorithm->current.exposure_lines -= correction;
    else algorithm->current.exposure_lines = algorithm->profile.min_exposure_lines;
    algorithm->current.exposure_lines = clamp_u32(algorithm->current.exposure_lines,
        algorithm->profile.min_exposure_lines, algorithm->profile.max_exposure_lines);
    algorithm->current.apply_exposure = 1u;
white_balance:
    /* Gains are ratios to green: with a channel at zero there is nothing to divide by, so the gains in use
     * are kept and not written again. */
    if (!statistics->mean_red || !statistics->mean_green || !statistics->mean_blue) {
        *controls = algorithm->current;
        return 0;
    }
    red_gain = ((uint32_t)statistics->mean_green * 256u + statistics->mean_red / 2u) /
        statistics->mean_red;
    blue_gain = ((uint32_t)statistics->mean_green * 256u + statistics->mean_blue / 2u) /
        statistics->mean_blue;
    algorithm->current.wb_red_gain_q8 = clamp_u16(red_gain, 128u, 1024u);
    algorithm->current.wb_blue_gain_q8 = clamp_u16(blue_gain, 128u, 1024u);
    algorithm->current.apply_white_balance = 1u;
    *controls = algorithm->current;
    return 0;
}

const neon_camera_3a_ops_t *neon_camera_3a_reference_ops(void)
{
    static const neon_camera_3a_ops_t ops = { load_profile, process };
    return &ops;
}

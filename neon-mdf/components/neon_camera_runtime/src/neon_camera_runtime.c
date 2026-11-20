#include "neon_camera_runtime.h"

/* Stages that start() completes, in order. A rollback undoes every stage up to the one it is given. */
typedef enum {
    STAGE_NONE = 0,
    STAGE_MIPI,
    STAGE_CSI,
    STAGE_ISP,
    STAGE_SENSOR
} start_stage_t;

static int pipeline_complete(const neon_camera_pipeline_t *pipeline)
{
    return pipeline != 0 && pipeline->sensor != 0 && pipeline->sensor_ops != 0 &&
           pipeline->mipi != 0 && pipeline->csi != 0 && pipeline->isp != 0 &&
           pipeline->algorithm != 0 && pipeline->algorithm_ops != 0;
}

/* Reverse of start(). Keeps going after a failure so that later stages are still released. */
static int stop_stages(neon_camera_pipeline_t *pipeline, start_stage_t reached)
{
    int result = 0;

    if (reached >= STAGE_SENSOR && pipeline->sensor_ops->stop_stream(pipeline->sensor)) result = -1;
    if (reached >= STAGE_ISP && neon_isp_stop(pipeline->isp) != NEON_OK) result = -1;
    if (reached >= STAGE_CSI && neon_csi_stop(pipeline->csi) != NEON_OK) result = -1;
    if (reached >= STAGE_MIPI && neon_mipi_release(pipeline->mipi) != NEON_OK) result = -1;
    return result;
}

static int reference_start(neon_camera_pipeline_t *pipeline)
{
    uint32_t latency;

    if (!pipeline_complete(pipeline) || pipeline->csi->mipi != pipeline->mipi ||
        pipeline->mipi_ready_timeout_polls == 0u) return -1;

    if (pipeline->sensor_ops->probe(pipeline->sensor) ||
        pipeline->sensor_ops->configure_mode(pipeline->sensor, pipeline->sensor_mode_id)) return -1;

    /* A refused acquire means somebody else holds the link: nothing of ours to undo. */
    if (neon_mipi_acquire(pipeline->mipi, &pipeline->mipi_config) != NEON_OK) return -1;
    if (neon_mipi_wait_ready(pipeline->mipi, pipeline->mipi_ready_timeout_polls) != NEON_OK ||
        neon_csi_configure(pipeline->csi, &pipeline->csi_config) != NEON_OK) {
        (void)stop_stages(pipeline, STAGE_MIPI);
        return -1;
    }
    if (neon_csi_start(pipeline->csi) != NEON_OK) {
        (void)stop_stages(pipeline, STAGE_MIPI);
        return -1;
    }
    /* The tuning goes in first, while the ISP is stopped (the only frame-safe moment for every table), and
     * the configuration after it, so that the fields the HAL owns are the HAL's. */
    if ((pipeline->isp_profile != 0 &&
         neon_isp_load_profile(pipeline->isp, pipeline->isp_profile, pipeline->isp_profile_bytes, 0) != NEON_OK) ||
        neon_isp_configure(pipeline->isp, &pipeline->isp_config) != NEON_OK ||
        neon_isp_start(pipeline->isp) != NEON_OK) {
        (void)stop_stages(pipeline, STAGE_CSI);
        return -1;
    }
    if (pipeline->sensor_ops->start_stream(pipeline->sensor)) {
        (void)stop_stages(pipeline, STAGE_ISP);
        return -1;
    }

    latency = pipeline->sensor_ops->control_latency_frames(pipeline->sensor);
    if (latency == 0u) {
        (void)stop_stages(pipeline, STAGE_SENSOR);
        return -1;
    }
    pipeline->algorithm->profile.control_latency_frames = latency;
    pipeline->last_statistics_frame_id = 0u; /* The ISP counts frames from 1 again. */
    return 0;
}

static int reference_on_statistics(neon_camera_pipeline_t *pipeline,
                                   const neon_isp_statistics_t *statistics)
{
    neon_isp_statistics_t local_statistics;
    neon_camera_3a_controls_t controls;
    neon_isp_dynamic_controls_t isp_controls;
    const neon_isp_statistics_t *input = statistics;

    if (pipeline == 0 || pipeline->sensor_ops == 0 || pipeline->isp == 0 ||
        pipeline->algorithm_ops == 0) return -1;
    if (input == 0) {
        const neon_status_t status = neon_isp_read_statistics(pipeline->isp, &local_statistics);

        if (status == NEON_BUSY) return 1; /* No frame has finished since the last call. */
        if (status != NEON_OK) return -1;
        /* Only another generator moved on: the exposure result was used already. */
        if (local_statistics.frame_id == pipeline->last_statistics_frame_id) return 1;
        pipeline->last_statistics_frame_id = local_statistics.frame_id;
        input = &local_statistics;
    }
    if (pipeline->algorithm_ops->process(pipeline->algorithm, input, &controls)) return -1;
    if (controls.apply_exposure &&
        (pipeline->sensor_ops->set_exposure_lines(pipeline->sensor, controls.exposure_lines,
                                                  controls.target_frame_id) ||
         pipeline->sensor_ops->set_analogue_gain_q8(pipeline->sensor, controls.analogue_gain_q8,
                                                    controls.target_frame_id) ||
         pipeline->sensor_ops->set_digital_gain_q8(pipeline->sensor, controls.digital_gain_q8,
                                                    controls.target_frame_id))) return -1;
    if (!controls.apply_white_balance) return 0;
    isp_controls.target_frame_id = controls.target_frame_id;
    isp_controls.wb_red_gain_q8 = controls.wb_red_gain_q8;
    isp_controls.wb_blue_gain_q8 = controls.wb_blue_gain_q8;
    return neon_isp_apply_dynamic_controls(pipeline->isp, &isp_controls, 0) == NEON_OK ? 0 : -1;
}

static int reference_stop(neon_camera_pipeline_t *pipeline)
{
    if (!pipeline_complete(pipeline)) return -1;
    return stop_stages(pipeline, STAGE_SENSOR);
}

const neon_camera_runtime_ops_t *neon_camera_runtime_reference_ops(void)
{
    static const neon_camera_runtime_ops_t ops = {
        .start = reference_start,
        .on_statistics = reference_on_statistics,
        .stop = reference_stop,
    };
    return &ops;
}

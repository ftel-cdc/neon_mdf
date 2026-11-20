#ifndef NEON_CAMERA_RUNTIME_H
#define NEON_CAMERA_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include "csi/neon_csi.h"
#include "mipi/neon_mipi.h"
#include "isp/neon_isp.h"
#include "neon_camera_3a.h"
#include "neon_camera_sensor_hal.h"

typedef struct {
    void *sensor;
    const neon_camera_sensor_hal_ops_t *sensor_ops;
    /* The link, the receiver and the ISP are used directly. csi must have been bound to mipi by neon_csi_init(). */
    neon_mipi_handle_t *mipi;
    neon_mipi_config_t mipi_config;
    uint32_t mipi_ready_timeout_polls; /* Status polls to wait for Stop State; at least one. */
    neon_csi_handle_t *csi;
    neon_csi_config_t csi_config;
    neon_isp_handle_t *isp;
    neon_isp_config_t isp_config;
    /*
     * Optional ISP tuning profile (format in neon_isp.h), or NULL. start() writes it into the stopped ISP
     * before configuring it, every time, so that it must stay valid while the pipeline is used.
     */
    const void *isp_profile;
    uint32_t isp_profile_bytes;
    neon_camera_3a_t *algorithm;
    const neon_camera_3a_ops_t *algorithm_ops;
    uint32_t sensor_mode_id;
    /* Kept by the runtime: frame_id of the statistics the 3A loop last used. Reset by start(). */
    uint32_t last_statistics_frame_id;
} neon_camera_pipeline_t;

/*
 * start() brings the pipeline up in the order that the MIPI CSI-2 MAS (9.1) and the ISP MAS require: sensor
 * probe and mode, MIPI link ready, CSI configured and started, ISP tuning profile written (if any), ISP
 * configured and started, and only then the sensor streams. A failure undoes the stages that had completed. stop() runs the same stages in reverse.
 *
 * The runtime does not move frames: it brings the stages up and runs the 3A loop. Handing a frame that the
 * receiver completed to neon_isp_queue_input(), and taking the NV12 frame back out, is the caller's job
 * until a frame loop is added here.
 */
typedef struct {
    int (*start)(neon_camera_pipeline_t *pipeline);
    /*
     * One turn of the 3A loop. statistics may be NULL, in which case the newest result is read from the ISP.
     * Returns 0 when the controls were applied, 1 when the ISP has no result that an earlier call did not
     * already use (nothing is changed), and -1 on an error. The statistics generators publish separately, so
     * a newer focus or white balance result alone does not run the loop again on an exposure result it
     * has already used: the loop runs once per statistics frame_id.
     */
    int (*on_statistics)(neon_camera_pipeline_t *pipeline, const neon_isp_statistics_t *statistics);
    int (*stop)(neon_camera_pipeline_t *pipeline);
} neon_camera_runtime_ops_t;

const neon_camera_runtime_ops_t *neon_camera_runtime_reference_ops(void);

/*
 * Gathered configuration. A caller describes the camera once, and neon_camera_runtime_init() derives the
 * per-module configurations from it, so that the receiver and the ISP agree by construction: the ISP input
 * stride is the receiver's line pitch, the receiver's line padding makes that pitch a multiple of the
 * DMA alignment, and the NV12 planes use that same alignment.
 */
typedef struct {
    /* Sensor, selected by the caller (see neon_camera_sensor_hal.h). */
    void *sensor;
    const neon_camera_sensor_hal_ops_t *sensor_ops;
    uint32_t sensor_mode_id;

    /* What the sensor sends. */
    uint32_t width;                       /* Active pixels per line. */
    uint32_t height;
    neon_isp_raw_depth_t raw_depth;
    neon_isp_bayer_pattern_t bayer_pattern;

    /* MIPI link. */
    uint8_t lane_count;
    uint32_t lane_rate_bps;               /* Line rate per data lane. */
    bool continuous_clock;
    uint32_t mipi_ready_timeout_polls;    /* 0 selects NEON_CAMERA_RUNTIME_DEFAULT_READY_POLLS. */

    /*
     * One DDR region for every buffer. init() carves it, in this order and each piece rounded up to the DMA
     * alignment: the receiver's RAW buffers, then for each ISP output buffer its Y plane and its UV plane.
     * neon_camera_runtime_memory_bytes() tells how much it needs.
     */
    uint32_t memory_base;
    uint32_t memory_bytes;

    /* 3A. NULL selects the reference algorithm and its default profile. */
    const neon_camera_3a_ops_t *algorithm_ops;
    const neon_camera_3a_profile_t *profile;

    /*
     * Tuning binaries, each optional (NULL pointer and zero size). They are typically embedded in flash or
     * read from a firmware partition by the caller; the runtime never opens a file.
     *
     * isp_profile: register image for neon_isp_load_profile(). init() checks every entry against the chip's
     * register map, so a wrong file is refused before any hardware is touched; start() writes it. It must
     * stay valid while the pipeline is used.
     *
     * algorithm_tuning: the 3A tuning, in the format of algorithm_ops->load_profile() (for the reference
     * algorithm, a neon_camera_3a_profile_t). init() hands it over once. It replaces profile: give one or
     * the other.
     */
    const void *isp_profile;
    uint32_t isp_profile_bytes;
    const void *algorithm_tuning;
    uint32_t algorithm_tuning_bytes;
} neon_camera_runtime_config_t;

#define NEON_CAMERA_RUNTIME_DEFAULT_READY_POLLS 100U

/* Everything one camera pipeline owns. The caller allocates it; init() fills it. */
typedef struct {
    neon_mipi_handle_t mipi;
    neon_csi_handle_t csi;
    neon_isp_handle_t isp;
    neon_camera_3a_t algorithm;
    neon_camera_pipeline_t pipeline;      /* Ready for neon_camera_runtime_reference_ops()->start(). */
    neon_isp_profile_info_t isp_profile_info; /* What init() found in isp_profile; zero without one. */
} neon_camera_runtime_t;

/**
 * @brief Fill a configuration with portable defaults: RAW12, RGGB, continuous clock, the default ready
 * timeout and the reference 3A. The sensor, geometry, lanes and memory are left at zero for the caller.
 * @param[out] config Configuration.
 */
void neon_camera_runtime_get_default_config(neon_camera_runtime_config_t *config);

/**
 * @brief Bytes of DDR that the configuration needs, for a base address that is already a multiple of the
 * DMA alignment. Uses only the geometry and format fields; needs no hardware.
 * @param[in] config Configuration.
 * @param[out] bytes Size of the region to give as memory_bytes.
 * @return NEON_OK, NEON_INVALID_PARAM for a bad geometry or depth, or NEON_NOT_SUPPORTED for a frame the
 * receiver cannot write, or an input that the ISP cannot read (RAW10 until the receiver can left-align).
 */
neon_status_t neon_camera_runtime_memory_bytes(const neon_camera_runtime_config_t *config, uint32_t *bytes);

/**
 * @brief Initialize the link, the receiver, the ISP and the 3A algorithm, and derive their configurations.
 * @details
 * Touches no hardware. It checks what it needs for the derivation (geometry and format, buffer sizes, that
 * the region is big enough); the rules of each module (lane count, ISP size limits, bursts) are checked
 * by that module when start() configures it, and start() rolls back on a refusal. After init the
 * pipeline's configuration fields are plain data: change one (for example a buffer address) before start()
 * if the default is not wanted.
 * @param[out] runtime Storage, overwritten. Must not hold a running pipeline.
 * @param[in] config Configuration.
 * @return NEON_OK, NEON_INVALID_PARAM (NULL pointer, no sensor, no lanes, bad geometry or depth, region too
 * small, a tuning binary with a pointer but no size or the reverse, an ISP profile entry the chip refuses,
 * both profile and algorithm_tuning, or a 3A tuning the algorithm refuses), or NEON_NOT_SUPPORTED (also
 * for algorithm_tuning given to an algorithm that cannot load one). On failure nothing is left
 * initialized.
 */
neon_status_t neon_camera_runtime_init(neon_camera_runtime_t *runtime, const neon_camera_runtime_config_t *config);

/**
 * @brief Release the handles of a pipeline that is stopped.
 * @param[in,out] runtime Pipeline.
 * @return NEON_OK, NEON_BUSY if the ISP or the receiver runs or the link is acquired (nothing is changed),
 * or NEON_INVALID_PARAM for a NULL pointer.
 */
neon_status_t neon_camera_runtime_deinit(neon_camera_runtime_t *runtime);

#endif

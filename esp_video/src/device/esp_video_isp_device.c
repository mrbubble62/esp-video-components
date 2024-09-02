/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "driver/isp_ccm.h"
#include "driver/isp_bf.h"
#include "driver/isp_gamma.h"
#include "driver/isp_ae.h"
#include "driver/isp_hist.h"
#include "driver/isp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_video.h"
#include "esp_video_device.h"
#include "esp_video_isp_ioctl.h"
#include "esp_video_device_internal.h"

/**
 * IDF-9706
 */
#include "soc/isp_struct.h"

#define ISP_NAME                   "ISP"

#define ISP_DMA_ALIGN_BYTES         4
#define ISP_MEM_CAPS                MALLOC_CAP_8BIT

#define ISP_INPUT_DATA_SRC          ISP_INPUT_DATA_SOURCE_CSI

/* AEG-1489 */
#define ISP_CLK_SRC                 ISP_CLK_SRC_DEFAULT
#define ISP_CLK_FREQ_HZ             (80 * 1000 * 1000)

#if CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE
#define ISP_LOCK(i)                 xSemaphoreTake((i)->mutex, portMAX_DELAY)
#define ISP_UNLOCK(i)               xSemaphoreGive((i)->mutex)
#else
#define ISP_LOCK(i)
#define ISP_UNLOCK(i)
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x)               sizeof(x) / sizeof((x)[0])
#endif

#define ISP_REGION_START            (0.2)
#define ISP_REGION_END              (0.8)

#define ISP_RAW_GG                  (2.0)
#define ISP_RAW_RG                  (1.0 / ISP_RAW_GG)
#define ISP_RAW_BG                  (1.0 / ISP_RAW_GG)

#define ISP_RGB_RG_S                (0.15)
#define ISP_RGB_RG_L                (ISP_RAW_RG - ISP_RGB_RG_S)
#define ISP_RGB_RG_H                (ISP_RAW_RG + ISP_RGB_RG_S)

#define ISP_RGB_BG_S                (0.15)
#define ISP_RGB_BG_L                (ISP_RAW_RG - ISP_RGB_BG_S)
#define ISP_RGB_BG_H                (ISP_RAW_RG + ISP_RGB_BG_S)

#define ISP_AWB_MAX_GLUM            220
#define ISP_AWB_MIN_GLUM            110

#define ISP_AWB_MAX_LUM             (ISP_AWB_MAX_GLUM * (1 + ISP_RGB_RG_H + ISP_RGB_BG_H))
#define ISP_AWB_MIN_LUM             (ISP_AWB_MIN_GLUM * (1 + ISP_RGB_RG_L + ISP_RGB_BG_L))

#define ISP_STARTED(iv)             ((iv)->isp_proc != NULL)

#if 0

#if 1
#define ISP_STATS_AWB_FLAG          IPA_STATS_FLAGS_AE
#else
#define ISP_STATS_AWB_FLAG          0
#endif

#if 1
#define ISP_STATS_AE_FLAG           IPA_STATS_FLAGS_AWB
#else
#define ISP_STATS_AE_FLAG           0
#endif

#if 1
#define ISP_STATS_HIST_FLAG         IPA_STATS_FLAGS_HIST
#else
#define ISP_STATS_HIST_FLAG         0
#endif

#define ISP_STATS_FLAGS             (ISP_STATS_AE_FLAG | ISP_STATS_AWB_FLAG | ISP_STATS_HIST_FLAG)
#else
#define ISP_STATS_FLAGS             0
#endif

enum isp_module {
    ISP_MODULE_AWB = 0,
    ISP_MODULE_AE,
    ISP_MODULE_HIST
};

struct isp_video {
    isp_proc_handle_t isp_proc;

#if CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE
    struct esp_video *video;

#if ISP_STATS_AWB_FLAG
    isp_awb_ctlr_t awb_ctlr;
#endif

#if ISP_STATS_AE_FLAG
    isp_ae_ctlr_t ae_ctlr;
#endif

#if ISP_STATS_HIST_FLAG
    isp_hist_ctlr_t hist_ctlr;
#endif

    portMUX_TYPE spinlock;
    SemaphoreHandle_t mutex;

    /* AWB configuration */

    uint32_t pts_count;
    float red_balance_gain;
    float blue_balance_gain;

    /* CCM configuration */

    uint8_t denoising_level;
    uint8_t bf_matrix[ISP_BF_TEMPLATE_X_NUMS][ISP_BF_TEMPLATE_Y_NUMS];

    /* CCM configuration */

    float ccm_matrix[ISP_CCM_DIMENSION][ISP_CCM_DIMENSION];

    /* Sharpen Configuration */

    uint8_t h_thresh;
    uint8_t l_thresh;

    float h_coeff;
    float m_coeff;

    uint8_t sharpen_matrix[ISP_SHARPEN_TEMPLATE_X_NUMS][ISP_SHARPEN_TEMPLATE_Y_NUMS];

    /* GAMMA Configuration */

    esp_video_isp_gamma_point_t gamma_points[ISP_GAMMA_CURVE_POINTS_NUM];

    /* Application command target */

    uint8_t red_balance_enable      : 1;
    uint8_t blue_balance_enable     : 1;
    uint8_t bf_enable               : 1;
    uint8_t ccm_enable              : 1;
    uint8_t sharpen_enable          : 1;
    uint8_t gamma_enable            : 1;

    /* ISP pipeline state */

    uint8_t bf_started              : 1;
    uint8_t ccm_started             : 1;
    uint8_t sharpen_started         : 1;
    uint8_t gamma_started           : 1;

#if ISP_STATS_FLAGS
    /* Meta capture state */

    bool capture_meta;

    /* Statistics data */

    uint64_t seq;
    esp_ipa_stats_t *stats_buffer;
#endif
#endif
};

static const struct v4l2_query_ext_ctrl s_isp_qctrl[] = {
    {
        .id = V4L2_CID_RED_BALANCE,
        .type = V4L2_CTRL_TYPE_INTEGER,
        .maximum = 7999,
        .minimum = 1,
        .step = 1,
        .elems = sizeof(uint32_t),
        .nr_of_dims = 1,
        .default_value = 0,
        .name = "red balance",
    },
    {
        .id = V4L2_CID_BLUE_BALANCE,
        .type = V4L2_CTRL_TYPE_INTEGER,
        .maximum = 7999,
        .minimum = 1,
        .step = 1,
        .elems = sizeof(uint32_t),
        .nr_of_dims = 1,
        .default_value = 0,
        .name = "blue balance",
    },
    {
        .id = V4L2_CID_USER_ESP_ISP_BF,
        .type = V4L2_CTRL_TYPE_U8,
        .maximum = UINT8_MAX,
        .minimum = 0,
        .step = 1,
        .elems = sizeof(esp_video_isp_bf_t),
        .nr_of_dims = 1,
        .default_value = 0,
        .name = "bayer filter",
    },
    {
        .id = V4L2_CID_USER_ESP_ISP_CCM,
        .type = V4L2_CTRL_TYPE_U8,
        .maximum = UINT8_MAX,
        .minimum = 0,
        .step = 1,
        .elems = sizeof(esp_video_isp_ccm_t),
        .nr_of_dims = 1,
        .default_value = 0,
        .name = "color correction matrix",
    },
    {
        .id = V4L2_CID_USER_ESP_ISP_SHARPEN,
        .type = V4L2_CTRL_TYPE_U8,
        .maximum = UINT8_MAX,
        .minimum = 0,
        .step = 1,
        .elems = sizeof(esp_video_isp_sharpen_t),
        .nr_of_dims = 1,
        .default_value = 0,
        .name = "sharpen",
    },
    {
        .id = V4L2_CID_USER_ESP_ISP_SHARPEN,
        .type = V4L2_CTRL_TYPE_U8,
        .maximum = UINT8_MAX,
        .minimum = 0,
        .step = 1,
        .elems = sizeof(esp_video_isp_gamma_t),
        .nr_of_dims = 1,
        .default_value = 0,
        .name = "gamma",
    },
};
static const char *TAG = "isp_video";

static struct isp_video s_isp_video;

static esp_err_t isp_get_input_frame_type(cam_ctlr_color_t ctlr_color, isp_color_t *isp_color)
{
    esp_err_t ret = ESP_OK;

    switch (ctlr_color) {
    case CAM_CTLR_COLOR_RAW8:
        *isp_color = ISP_COLOR_RAW8;
        break;
    case CAM_CTLR_COLOR_RAW10:
        *isp_color = ISP_COLOR_RAW10;
        break;
    case CAM_CTLR_COLOR_RAW12:
        *isp_color = ISP_COLOR_RAW12;
        break;
    default:
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
    }

    return ret;
}

static esp_err_t isp_get_output_frame_type(cam_ctlr_color_t ctlr_color, isp_color_t *isp_color)
{
    esp_err_t ret = ESP_OK;

    switch (ctlr_color) {
    case CAM_CTLR_COLOR_RAW8:
        *isp_color = ISP_COLOR_RAW8;
        break;
    case CAM_CTLR_COLOR_RGB565:
        *isp_color = ISP_COLOR_RGB565;
        break;
    case CAM_CTLR_COLOR_RGB888:
        *isp_color = ISP_COLOR_RGB888;
        break;
    case CAM_CTLR_COLOR_YUV420:
        *isp_color = ISP_COLOR_YUV420;
        break;
    case CAM_CTLR_COLOR_YUV422:
        *isp_color = ISP_COLOR_YUV422;
        break;
    default:
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
    }

    return ret;
}

#if CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE

#if ISP_STATS_FLAGS
static esp_err_t isp_stats_done(struct isp_video *isp_video, const void *buffer, enum isp_module module)
{
    esp_err_t ret = ESP_OK;

    if (!isp_video->capture_meta) {
        return false;
    }

    portENTER_CRITICAL(&isp_video->spinlock);
    if (!isp_video->stats_buffer) {
        struct esp_video_buffer_element *element;

        element = META_VIDEO_GET_QUEUED_ELEMENT(isp_video->video);
        if (!element) {
            ret = ESP_ERR_NO_MEM;
            goto exit;
        }

        isp_video->stats_buffer = (esp_ipa_stats_t *)element->buffer;
        isp_video->stats_buffer->flags = 0;
    }

    switch (module) {
#if ISP_STATS_AWB_FLAG
    case ISP_MODULE_AWB: {
        const esp_isp_awb_evt_data_t *edata = (const esp_isp_awb_evt_data_t *)buffer;
        esp_ipa_stats_awb_t *awb_stats = isp_video->stats_buffer->awb_stats;

        awb_stats->counted = edata->awb_result.white_patch_num;
        awb_stats->uncounted = isp_video->pts_count - awb_stats->counted;
        awb_stats->sum_r = edata->awb_result.sum_r;
        awb_stats->sum_g = edata->awb_result.sum_g;
        awb_stats->sum_b = edata->awb_result.sum_b;

        isp_video->stats_buffer->flags |= IPA_STATS_FLAGS_AWB;
        break;
    }
#endif

#if ISP_STATS_AE_FLAG
    case ISP_MODULE_AE: {
        int ae_off = 0;
        const esp_isp_ae_env_detector_evt_data_t *edata = (const esp_isp_ae_env_detector_evt_data_t *)buffer;
        esp_ipa_stats_ae_t *ae_stats = isp_video->stats_buffer->ae_stats;

        for (int i = 0; i < SOC_ISP_AE_BLOCK_X_NUMS; i++) {
            for (int j = 0; j < SOC_ISP_AE_BLOCK_Y_NUMS; j++) {
                ae_stats[ae_off++].luminance = edata->ae_result.luminance[i][j];
            }
        }

        isp_video->stats_buffer->flags |= IPA_STATS_FLAGS_AE;
        break;
    }
#endif

#if ISP_STATS_HIST_FLAG
    case ISP_MODULE_HIST: {
        const esp_isp_hist_evt_data_t *edata = (const esp_isp_hist_evt_data_t *)buffer;
        esp_ipa_stats_hist_t *hist_stats = isp_video->stats_buffer->hist_stats;

        for (int i = 0; i < ISP_HIST_SEGMENT_NUMS; i++) {
            hist_stats[i].value = edata->hist_result.hist_value[i];
        }

        isp_video->stats_buffer->flags |= IPA_STATS_FLAGS_HIST;
        break;
    }
#endif

    default:
        ESP_EARLY_LOGE(TAG, "module=%d is not supported", module);
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    if ((isp_video->stats_buffer->flags & ISP_STATS_FLAGS) == ISP_STATS_FLAGS) {
        isp_video->stats_buffer->seq = isp_video->seq++;
        META_VIDEO_DONE_BUF(isp_video->video, isp_video->stats_buffer, sizeof(esp_ipa_stats_t));
        isp_video->stats_buffer = NULL;
    }

exit:
    portEXIT_CRITICAL(&isp_video->spinlock);
    return ret;
}
#endif

#if ISP_STATS_HIST_FLAG
static bool isp_hist_stats_done(isp_hist_ctlr_t hist_ctlr, const esp_isp_hist_evt_data_t *edata, void *user_data)
{
    esp_err_t ret;
    struct isp_video *isp_video = (struct isp_video *)user_data;

    ret = isp_stats_done(isp_video, edata, ISP_MODULE_HIST);

    return ret == ESP_OK ? true : false;
}

static esp_err_t isp_start_hist(struct isp_video *isp_video)
{
    esp_err_t ret;
    uint32_t width = META_VIDEO_GET_FORMAT_WIDTH(isp_video->video);
    uint32_t height = META_VIDEO_GET_FORMAT_HEIGHT(isp_video->video);
    esp_isp_hist_config_t hist_config = {
        .window = {
            .top_left = {.x = width * ISP_REGION_START, .y = height * ISP_REGION_START},
            .btm_right = {.x = width * ISP_REGION_END, .y = height * ISP_REGION_END},
        },
        .hist_mode = ISP_HIST_SAMPLING_RGB,
        .rgb_coefficient = {
            .coeff_b = {{85, 0}},
            .coeff_g = {{85, 0}},
            .coeff_r = {{85, 0}},
        },
        .window_weight = {
            {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0},
            {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 11, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0},
            {.decimal = 10, .integer = 0}, {.decimal = 11, .integer = 0}, {.decimal = 12, .integer = 0}, {.decimal = 11, .integer = 0}, {.decimal = 10, .integer = 0},
            {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 11, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0},
            {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0}, {.decimal = 10, .integer = 0},
        },
        .segment_threshold = {16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240},
    };
    esp_isp_hist_cbs_t hist_cb = {
        .on_statistics_done = isp_hist_stats_done,
    };

    ESP_RETURN_ON_ERROR(esp_isp_new_hist_controller(isp_video->isp_proc, &hist_config, &isp_video->hist_ctlr), TAG, "failed to new histogram");

    ESP_GOTO_ON_ERROR(esp_isp_hist_register_event_callbacks(isp_video->hist_ctlr, &hist_cb, isp_video), fail_0, TAG, "failed to register histogram callback");
    ESP_GOTO_ON_ERROR(esp_isp_hist_controller_enable(isp_video->hist_ctlr), fail_0, TAG, "failed to enable histogram");
    ESP_GOTO_ON_ERROR(esp_isp_hist_controller_start_continuous_statistics(isp_video->hist_ctlr), fail_1, TAG, "failed to start histogram");

    return ESP_OK;

fail_1:
    esp_isp_hist_controller_disable(isp_video->hist_ctlr);
fail_0:
    esp_isp_del_hist_controller(isp_video->hist_ctlr);
    isp_video->hist_ctlr = NULL;
    return ret;
}

static esp_err_t isp_stop_hist(struct isp_video *isp_video)
{
    ESP_RETURN_ON_ERROR(esp_isp_hist_controller_stop_continuous_statistics(isp_video->hist_ctlr), TAG, "failed to stop histogram");
    ESP_RETURN_ON_ERROR(esp_isp_hist_controller_disable(isp_video->hist_ctlr), TAG, "failed to disable histogram");
    ESP_RETURN_ON_ERROR(esp_isp_del_hist_controller(isp_video->hist_ctlr), TAG, "failed to delete histogram");

    isp_video->hist_ctlr = NULL;

    return ESP_OK;
}
#endif

#if ISP_STATS_AWB_FLAG
static bool isp_awb_stats_done(isp_awb_ctlr_t awb_ctlr, const esp_isp_awb_evt_data_t *edata, void *user_data)
{
    esp_err_t ret;
    struct isp_video *isp_video = (struct isp_video *)user_data;

    ret = isp_stats_done(isp_video, edata, ISP_MODULE_AWB);

    return ret == ESP_OK ? true : false;
}

static esp_err_t isp_start_awb(struct isp_video *isp_video)
{
    esp_err_t ret;
    uint32_t width = META_VIDEO_GET_FORMAT_WIDTH(isp_video->video);
    uint32_t height = META_VIDEO_GET_FORMAT_HEIGHT(isp_video->video);
    esp_isp_awb_config_t awb_config = {
        .sample_point = ISP_AWB_SAMPLE_POINT_BEFORE_CCM,
        .window = {
            .top_left = {.x = width * ISP_REGION_START, .y = height * ISP_REGION_START},
            .btm_right = {.x = width * ISP_REGION_END, .y = height * ISP_REGION_END},
        },
        .white_patch = {
            .luminance = {.min = ISP_AWB_MIN_LUM, .max = ISP_AWB_MAX_LUM},
            .red_green_ratio = {.min = ISP_RGB_RG_L, .max = ISP_RGB_RG_H},
            .blue_green_ratio = {.min = ISP_RGB_BG_L, .max = ISP_RGB_BG_H},
        },
    };
    esp_isp_awb_cbs_t awb_cb = {
        .on_statistics_done = isp_awb_stats_done,
    };

    ESP_RETURN_ON_ERROR(esp_isp_new_awb_controller(isp_video->isp_proc, &awb_config, &isp_video->awb_ctlr), TAG, "failed to new AWB");

    isp_video->pts_count = (awb_config.window.btm_right.x - awb_config.window.top_left.x + 1) *
                           (awb_config.window.btm_right.y - awb_config.window.top_left.y + 1);

    ESP_GOTO_ON_ERROR(esp_isp_awb_register_event_callbacks(isp_video->awb_ctlr, &awb_cb, isp_video), fail_0, TAG, "failed to register AWB callback");
    ESP_GOTO_ON_ERROR(esp_isp_awb_controller_enable(isp_video->awb_ctlr), fail_0, TAG, "failed to enable AWB");
    ESP_GOTO_ON_ERROR(esp_isp_awb_controller_start_continuous_statistics(isp_video->awb_ctlr), fail_1, TAG, "failed to start AWB");

    return ESP_OK;

fail_1:
    esp_isp_awb_controller_disable(isp_video->awb_ctlr);
fail_0:
    esp_isp_del_awb_controller(isp_video->awb_ctlr);
    isp_video->awb_ctlr = NULL;
    return ret;
}

static esp_err_t isp_stop_awb(struct isp_video *isp_video)
{
    ESP_RETURN_ON_ERROR(esp_isp_awb_controller_stop_continuous_statistics(isp_video->awb_ctlr), TAG, "failed to stop AWB");
    ESP_RETURN_ON_ERROR(esp_isp_awb_controller_disable(isp_video->awb_ctlr), TAG, "failed to disable AWB");
    ESP_RETURN_ON_ERROR(esp_isp_del_awb_controller(isp_video->awb_ctlr), TAG, "failed to delete AWB");

    isp_video->awb_ctlr = NULL;

    return ESP_OK;
}
#endif

static esp_err_t isp_start_bf(struct isp_video *isp_video)
{
    if (isp_video->bf_started) {
        return ESP_OK;
    }

    esp_isp_bf_config_t bf_config = {
        .denoising_level = isp_video->denoising_level,
        .padding_mode = ISP_BF_EDGE_PADDING_MODE_SRND_DATA,
        .padding_line_tail_valid_start_pixel = 0,
        .padding_line_tail_valid_end_pixel = 0,
    };

    memcpy(bf_config.bf_template, isp_video->bf_matrix, sizeof(bf_config.bf_template));

    ESP_RETURN_ON_ERROR(esp_isp_bf_configure(isp_video->isp_proc, &bf_config), TAG, "failed to configure BF");
    ESP_RETURN_ON_ERROR(esp_isp_bf_enable(isp_video->isp_proc), TAG, "failed to enable BF");
    isp_video->bf_started = true;

    return ESP_OK;
}

static esp_err_t isp_stop_bf(struct isp_video *isp_video)
{
    if (!isp_video->bf_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_isp_bf_disable(isp_video->isp_proc), TAG, "failed to disable BF");
    isp_video->bf_started = false;

    return ESP_OK;
}

static esp_err_t isp_start_ccm(struct isp_video *isp_video)
{
    if (isp_video->ccm_started) {
        return ESP_OK;
    }

    esp_isp_ccm_config_t ccm_config = {
        .saturation = true,
    };

    if (isp_video->ccm_enable) {
        memcpy(ccm_config.matrix, isp_video->ccm_matrix, sizeof(ccm_config.matrix));

        /* Apply red and blue balance */
        for (int i = 0; i < ISP_CCM_DIMENSION; i++) {
            if (isp_video->red_balance_enable) {
                ccm_config.matrix[i][0] *= isp_video->red_balance_gain;
            }

            if (isp_video->blue_balance_enable) {
                ccm_config.matrix[i][2] *= isp_video->blue_balance_gain;
            }
        }
    } else {
        if (isp_video->red_balance_enable) {
            ccm_config.matrix[0][0] = isp_video->red_balance_gain;
        } else {
            ccm_config.matrix[0][0] = 1.0;
        }

        ccm_config.matrix[1][1] = 1.0;

        if (isp_video->blue_balance_enable) {
            ccm_config.matrix[2][2] = isp_video->blue_balance_gain;
        } else {
            ccm_config.matrix[2][2] = 1.0;
        }
    }

    ESP_RETURN_ON_ERROR(esp_isp_ccm_configure(isp_video->isp_proc, &ccm_config), TAG, "failed to configure CCM");
    ESP_RETURN_ON_ERROR(esp_isp_ccm_enable(isp_video->isp_proc), TAG, "failed to enable CCM");
    isp_video->ccm_started = true;

    return ESP_OK;
}

static esp_err_t isp_stop_ccm(struct isp_video *isp_video)
{
    if (!isp_video->ccm_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_isp_ccm_disable(isp_video->isp_proc), TAG, "failed to disable CCM");
    isp_video->ccm_started = false;

    return ESP_OK;
}

#if ISP_STATS_AE_FLAG
static bool isp_ae_stats_done(isp_ae_ctlr_t ae_ctlr, const esp_isp_ae_env_detector_evt_data_t *edata, void *user_data)
{
    esp_err_t ret;
    struct isp_video *isp_video = (struct isp_video *)user_data;

    ret = isp_stats_done(isp_video, edata, ISP_MODULE_AE);

    return ret == ESP_OK ? true : false;
}

static esp_err_t isp_start_ae(struct isp_video *isp_video)
{
    uint32_t width = META_VIDEO_GET_FORMAT_WIDTH(isp_video->video);
    uint32_t height = META_VIDEO_GET_FORMAT_HEIGHT(isp_video->video);
    esp_isp_ae_config_t ae_config = {
        .sample_point = ISP_AE_SAMPLE_POINT_AFTER_DEMOSAIC,
        .window = {
            .top_left = {.x = width * ISP_REGION_START, .y = height * ISP_REGION_START},
            .btm_right = {.x = width * ISP_REGION_END, .y = height * ISP_REGION_END},
        },
        .intr_priority = 0,
    };
    esp_isp_ae_env_detector_evt_cbs_t cbs = {
        .on_env_statistics_done = isp_ae_stats_done,
    };

    ESP_ERROR_CHECK(esp_isp_new_ae_controller(isp_video->isp_proc, &ae_config, &isp_video->ae_ctlr));

    ESP_ERROR_CHECK(esp_isp_ae_env_detector_register_event_callbacks(isp_video->ae_ctlr, &cbs, isp_video));
    ESP_ERROR_CHECK(esp_isp_ae_controller_enable(isp_video->ae_ctlr));
    ESP_ERROR_CHECK(esp_isp_ae_controller_start_continuous_statistics(isp_video->ae_ctlr));

    return ESP_OK;
}

static esp_err_t isp_stop_ae(struct isp_video *isp_video)
{
    ESP_ERROR_CHECK(esp_isp_ae_controller_stop_continuous_statistics(isp_video->ae_ctlr));
    ESP_ERROR_CHECK(esp_isp_ae_controller_disable(isp_video->ae_ctlr));
    ESP_ERROR_CHECK(esp_isp_del_ae_controller(isp_video->ae_ctlr));
    isp_video->ae_ctlr = NULL;

    return ESP_OK;
}
#endif

static esp_err_t isp_start_sharpen(struct isp_video *isp_video)
{
    if (isp_video->sharpen_started) {
        return ESP_OK;
    }

    uint8_t h_amount = 1 << ISP_SHARPEN_H_FREQ_COEF_DEC_BITS;
    uint8_t m_amount = 1 << ISP_SHARPEN_M_FREQ_COEF_DEC_BITS;
    uint8_t h_integer = (uint8_t)(isp_video->h_coeff * h_amount);
    uint8_t m_integer = (uint8_t)(isp_video->m_coeff * m_amount);

    esp_isp_sharpen_config_t sharpen_config = {
        .h_freq_coeff = {
            .integer = h_integer / h_amount,
            .decimal = h_integer % h_amount
        },
        .m_freq_coeff = {
            .integer = m_integer / m_amount,
            .decimal = m_integer % m_amount
        },
        .h_thresh = isp_video->h_thresh,
        .l_thresh = isp_video->l_thresh,
        .padding_mode = ISP_SHARPEN_EDGE_PADDING_MODE_SRND_DATA,
    };

    for (int i = 0; i < ISP_SHARPEN_TEMPLATE_X_NUMS; i++) {
        for (int j = 0; j < ISP_SHARPEN_TEMPLATE_Y_NUMS; j++) {
            sharpen_config.sharpen_template[i][j] = isp_video->sharpen_matrix[i][j];
        }
    }

    ESP_RETURN_ON_ERROR(esp_isp_sharpen_configure(isp_video->isp_proc, &sharpen_config), TAG, "failed to configure sharpen");
    ESP_RETURN_ON_ERROR(esp_isp_sharpen_enable(isp_video->isp_proc), TAG, "failed to enable sharpen");
    isp_video->sharpen_started = true;

    return ESP_OK;
}

static esp_err_t isp_stop_sharpen(struct isp_video *isp_video)
{
    if (!isp_video->sharpen_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_isp_sharpen_disable(isp_video->isp_proc), TAG, "failed to disable sharpen");
    isp_video->sharpen_started = false;

    return ESP_OK;
}

static esp_err_t isp_start_gamma(struct isp_video *isp_video)
{
    if (isp_video->gamma_started) {
        return ESP_OK;
    }

    isp_gamma_curve_points_t gamma_config;

    for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
        gamma_config.pt[i].x = isp_video->gamma_points[i].x;
        gamma_config.pt[i].y = isp_video->gamma_points[i].y;
    }

    ESP_RETURN_ON_ERROR(esp_isp_gamma_configure(isp_video->isp_proc, COLOR_COMPONENT_R, &gamma_config), TAG, "failed to configure R GAMMA");
    ESP_RETURN_ON_ERROR(esp_isp_gamma_configure(isp_video->isp_proc, COLOR_COMPONENT_G, &gamma_config), TAG, "failed to configure G GAMMA");
    ESP_RETURN_ON_ERROR(esp_isp_gamma_configure(isp_video->isp_proc, COLOR_COMPONENT_B, &gamma_config), TAG, "failed to configure B GAMMA");
    ESP_RETURN_ON_ERROR(esp_isp_gamma_enable(isp_video->isp_proc), TAG, "failed to enable GAMMA");
    isp_video->gamma_started = true;

    return ESP_OK;
}

static esp_err_t isp_stop_gamma(struct isp_video *isp_video)
{
    if (!isp_video->gamma_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_isp_gamma_disable(isp_video->isp_proc), TAG, "failed to disable GAMMA");
    isp_video->gamma_started = false;

    return ESP_OK;
}

static esp_err_t isp_start_pipeline(struct isp_video *isp_video)
{
    esp_err_t ret;

    if (isp_video->ccm_enable || isp_video->red_balance_enable || isp_video->blue_balance_enable) {
        ESP_RETURN_ON_ERROR(isp_start_ccm(isp_video), TAG, "failed to start CCM");
    }
    if (isp_video->bf_enable) {
        ESP_GOTO_ON_ERROR(isp_start_bf(isp_video), fail_0, TAG, "failed to start BF");
    }

#if ISP_STATS_AWB_FLAG
    ESP_GOTO_ON_ERROR(isp_start_awb(isp_video), fail_1, TAG, "failed to start AWB");
#endif

#if ISP_STATS_AE_FLAG
    ESP_GOTO_ON_ERROR(isp_start_ae(isp_video), fail_2, TAG, "failed to start AE");
#endif

#if ISP_STATS_HIST_FLAG
    ESP_GOTO_ON_ERROR(isp_start_hist(isp_video), fail_3, TAG, "failed to start histogram");
#endif

    if (isp_video->sharpen_enable) {
        ESP_GOTO_ON_ERROR(isp_start_sharpen(isp_video), fail_4, TAG, "failed to start sharpen");
    }


    if (isp_video->gamma_enable) {
        ESP_GOTO_ON_ERROR(isp_start_gamma(isp_video), fail_5, TAG, "failed to start GAMMA");
    }

    return ESP_OK;

fail_5:
    isp_stop_sharpen(isp_video);
fail_4:
#if ISP_STATS_HIST_FLAG
    isp_stop_hist(isp_video);
fail_3:
#endif
#if ISP_STATS_AE_FLAG
    isp_stop_ae(isp_video);
fail_2:
#endif
#if ISP_STATS_AWB_FLAG
    isp_stop_awb(isp_video);
fail_1:
#endif
    isp_stop_bf(isp_video);
fail_0:
    isp_stop_ccm(isp_video);
    return ret;
}

static esp_err_t isp_stop_pipeline(struct isp_video *isp_video)
{
    ESP_RETURN_ON_ERROR(isp_stop_gamma(isp_video), TAG, "failed to stop GAMMA");

    ESP_RETURN_ON_ERROR(isp_stop_sharpen(isp_video), TAG, "failed to stop sharpen");

#if ISP_STATS_HIST_FLAG
    ESP_RETURN_ON_ERROR(isp_stop_hist(isp_video), TAG, "failed to stop histogram");
#endif

#if ISP_STATS_AE_FLAG
    ESP_RETURN_ON_ERROR(isp_stop_ae(isp_video), TAG, "failed to stop AE");
#endif

#if ISP_STATS_AWB_FLAG
    ESP_RETURN_ON_ERROR(isp_stop_awb(isp_video), TAG, "failed to stop AWB");
#endif

    ESP_RETURN_ON_ERROR(isp_stop_bf(isp_video), TAG, "failed to stop BF");
    ESP_RETURN_ON_ERROR(isp_stop_ccm(isp_video), TAG, "failed to stop CCM");

    return ESP_OK;
}

static esp_err_t isp_video_init(struct esp_video *video)
{
#if ISP_STATS_FLAGS
    uint32_t buf_size = sizeof(esp_ipa_stats_t);

    META_VIDEO_SET_BUF_INFO(video, buf_size, ISP_DMA_ALIGN_BYTES, ISP_MEM_CAPS);
#endif

    return ESP_OK;
}

static esp_err_t isp_video_deinit(struct esp_video *video)
{
    return ESP_OK;
}

static esp_err_t isp_video_start(struct esp_video *video, uint32_t type)
{
    esp_err_t ret = ESP_OK;
#if ISP_STATS_FLAGS
    struct isp_video *isp_video = VIDEO_PRIV_DATA(struct isp_video *, video);

    ISP_LOCK(isp_video);

    if (type == V4L2_BUF_TYPE_META_CAPTURE) {
        isp_video->capture_meta = true;
    }

    ISP_UNLOCK(isp_video);
#endif
    return ret;
}

static esp_err_t isp_video_stop(struct esp_video *video, uint32_t type)
{
    esp_err_t ret = ESP_OK;
#if ISP_STATS_FLAGS
    struct isp_video *isp_video = VIDEO_PRIV_DATA(struct isp_video *, video);

    ISP_LOCK(isp_video);

    if (type == V4L2_BUF_TYPE_META_CAPTURE) {
        isp_video->capture_meta = false;
    }

    ISP_UNLOCK(isp_video);
#endif
    return ret;
}

static esp_err_t isp_video_enum_format(struct esp_video *video, uint32_t type, uint32_t index, uint32_t *pixel_format)
{
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;

    if (type == V4L2_BUF_TYPE_META_CAPTURE) {
        switch (index) {
        case 0:
            *pixel_format = V4L2_META_FMT_ESP_ISP_STATS;
            ret = ESP_OK;
            break;
        default:
            break;
        }
    }

    return ret;
}

static esp_err_t isp_video_set_format(struct esp_video *video, uint32_t type, const struct esp_video_format *format)
{
    if (format->width != META_VIDEO_GET_FORMAT_WIDTH(video) ||
            format->height != META_VIDEO_GET_FORMAT_HEIGHT(video) ||
            format->pixel_format != META_VIDEO_GET_FORMAT_PIXEL_FORMAT(video)) {
        ESP_LOGE(TAG, "width or height or format is not supported");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t isp_video_notify(struct esp_video *video, enum esp_video_event event, void *arg)
{
    return ESP_OK;
}

static esp_err_t isp_video_set_ext_ctrl(struct esp_video *video, const struct v4l2_ext_controls *ctrls)
{
    esp_err_t ret = ESP_OK;
    struct isp_video *isp_video = VIDEO_PRIV_DATA(struct isp_video *, video);

    ISP_LOCK(isp_video);

    for (int i = 0; i < ctrls->count; i++) {
        struct v4l2_ext_control *ctrl = &ctrls->controls[i];

        switch (ctrl->id) {
        case V4L2_CID_USER_ESP_ISP_BF: {
            const esp_video_isp_bf_t *bf = (const esp_video_isp_bf_t *)ctrl->p_u8;

            isp_video->bf_enable = bf->enable;
            if (bf->enable) {
                isp_video->denoising_level = bf->level;
                for (int i = 0; i < ISP_BF_TEMPLATE_X_NUMS; i++) {
                    for (int j = 0; j < ISP_BF_TEMPLATE_Y_NUMS; j++) {
                        isp_video->bf_matrix[i][j] = bf->matrix[i][j];
                    }
                }

                if (ISP_STARTED(isp_video)) {
                    ESP_GOTO_ON_ERROR(isp_stop_bf(isp_video), exit, TAG, "failed to stop BF");
                    ESP_GOTO_ON_ERROR(isp_start_bf(isp_video), exit, TAG, "failed to start BF");
                }
            } else {
                if (ISP_STARTED(isp_video)) {
                    ESP_GOTO_ON_ERROR(isp_stop_bf(isp_video), exit, TAG, "failed to stop BF");
                }
            }
            break;
        }
        case V4L2_CID_USER_ESP_ISP_CCM: {
            const esp_video_isp_ccm_t *ccm = (const esp_video_isp_ccm_t *)ctrl->p_u8;

            isp_video->ccm_enable = ccm->enable;
            if (ccm->enable) {
                for (int i = 0; i < ISP_CCM_DIMENSION; i++) {
                    for (int j = 0; j < ISP_CCM_DIMENSION; j++) {
                        isp_video->ccm_matrix[i][j] = ccm->matrix[i][j];
                    }
                }

                if (ISP_STARTED(isp_video)) {
                    ESP_GOTO_ON_ERROR(isp_stop_ccm(isp_video), exit, TAG, "failed to stop CCM");
                    ESP_GOTO_ON_ERROR(isp_start_ccm(isp_video), exit, TAG, "failed to start CCM");
                }
            } else {
                if (ISP_STARTED(isp_video)) {
                    ESP_GOTO_ON_ERROR(isp_stop_ccm(isp_video), exit, TAG, "failed to stop CCM");
                }
            }
            break;
        }
        case V4L2_CID_RED_BALANCE:
            if (ctrl->value > 0) {
                isp_video->red_balance_gain = *(float *)ctrl->ptr;
                isp_video->red_balance_enable = true;
            } else {
                isp_video->red_balance_enable = false;
            }

            if (ISP_STARTED(isp_video)) {
                ESP_GOTO_ON_ERROR(isp_stop_ccm(isp_video), exit, TAG, "failed to stop CCM");
                ESP_GOTO_ON_ERROR(isp_start_ccm(isp_video), exit, TAG, "failed to start CCM");
            }
            break;
        case V4L2_CID_BLUE_BALANCE:
            if (ctrl->value > 0) {
                isp_video->blue_balance_gain = *(float *)ctrl->ptr;
                isp_video->blue_balance_enable = true;
            } else {
                isp_video->blue_balance_enable = false;
            }

            if (ISP_STARTED(isp_video)) {
                ESP_GOTO_ON_ERROR(isp_stop_ccm(isp_video), exit, TAG, "failed to stop CCM");
                ESP_GOTO_ON_ERROR(isp_start_ccm(isp_video), exit, TAG, "failed to start CCM");
            }
            break;
        case V4L2_CID_USER_ESP_ISP_SHARPEN: {
            const esp_video_isp_sharpen_t *sharpen = (const esp_video_isp_sharpen_t *)ctrl->p_u8;

            isp_video->sharpen_enable = sharpen->enable;
            if (sharpen->enable) {
                isp_video->h_thresh = sharpen->h_thresh;
                isp_video->l_thresh = sharpen->l_thresh;
                isp_video->h_coeff = sharpen->h_coeff;
                isp_video->m_coeff = sharpen->m_coeff;
                for (int i = 0; i < ISP_SHARPEN_TEMPLATE_X_NUMS; i++) {
                    for (int j = 0; j < ISP_SHARPEN_TEMPLATE_Y_NUMS; j++) {
                        isp_video->sharpen_matrix[i][j] = sharpen->matrix[i][j];
                    }
                }

                if (ISP_STARTED(isp_video)) {
                    ESP_GOTO_ON_ERROR(isp_stop_sharpen(isp_video), exit, TAG, "failed to stop BF");
                    ESP_GOTO_ON_ERROR(isp_start_sharpen(isp_video), exit, TAG, "failed to start BF");
                }
            } else {
                if (ISP_STARTED(isp_video)) {
                    ESP_GOTO_ON_ERROR(isp_stop_sharpen(isp_video), exit, TAG, "failed to stop BF");
                }
            }
            break;
        }
        case V4L2_CID_USER_ESP_ISP_GAMMA: {
            const esp_video_isp_gamma_t *gamma = (const esp_video_isp_gamma_t *)ctrl->p_u8;

            isp_video->gamma_enable = gamma->enable;
            if (gamma->enable) {
                for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
                    isp_video->gamma_points[i].x = gamma->points[i].x;
                    isp_video->gamma_points[i].y = gamma->points[i].y;
                }

                if (ISP_STARTED(isp_video)) {
                    ESP_GOTO_ON_ERROR(isp_stop_gamma(isp_video), exit, TAG, "failed to stop GAMMA");
                    ESP_GOTO_ON_ERROR(isp_start_gamma(isp_video), exit, TAG, "failed to start GAMMA");
                }
            } else {
                if (ISP_STARTED(isp_video)) {
                    ESP_GOTO_ON_ERROR(isp_stop_gamma(isp_video), exit, TAG, "failed to stop GAMMA");
                }
            }
            break;
        }
        default:
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
        }

        if (ret != ESP_OK) {
            break;
        }
    }

exit:
    ISP_UNLOCK(isp_video);
    return ret;
}

static esp_err_t isp_video_get_ext_ctrl(struct esp_video *video, struct v4l2_ext_controls *ctrls)
{
    esp_err_t ret = ESP_OK;
    struct isp_video *isp_video = VIDEO_PRIV_DATA(struct isp_video *, video);

    ISP_LOCK(isp_video);

    for (int i = 0; i < ctrls->count; i++) {
        struct v4l2_ext_control *ctrl = &ctrls->controls[i];

        switch (ctrl->id) {
        case V4L2_CID_USER_ESP_ISP_BF: {
            esp_video_isp_bf_t *bf = (esp_video_isp_bf_t *)ctrl->p_u8;

            bf->enable = isp_video->bf_enable;
            bf->level = isp_video->denoising_level;
            for (int i = 0; i < ISP_BF_TEMPLATE_X_NUMS; i++) {
                for (int j = 0; j < ISP_BF_TEMPLATE_Y_NUMS; j++) {
                    bf->matrix[i][j] = isp_video->bf_matrix[i][j];
                }
            }
            break;
        }
        case V4L2_CID_USER_ESP_ISP_CCM: {
            esp_video_isp_ccm_t *ccm = (esp_video_isp_ccm_t *)ctrl->p_u8;

            ccm->enable = isp_video->ccm_enable;
            for (int i = 0; i < ISP_CCM_DIMENSION; i++) {
                for (int j = 0; j < ISP_CCM_DIMENSION; j++) {
                    ccm->matrix[i][j] = isp_video->ccm_matrix[i][j];
                }
            }
            break;
        }
        case V4L2_CID_RED_BALANCE:
            *((float *)ctrl->ptr) = isp_video->red_balance_gain;
            break;
        case V4L2_CID_BLUE_BALANCE:
            *((float *)ctrl->ptr) = isp_video->blue_balance_gain;
            break;
        case V4L2_CID_USER_ESP_ISP_SHARPEN: {
            esp_video_isp_sharpen_t *sharpen = (esp_video_isp_sharpen_t *)ctrl->p_u8;

            sharpen->enable = isp_video->sharpen_enable;
            sharpen->h_thresh = isp_video->h_thresh;
            sharpen->l_thresh = isp_video->l_thresh;
            sharpen->h_coeff = isp_video->h_coeff;
            sharpen->m_coeff = isp_video->m_coeff;
            for (int i = 0; i < ISP_SHARPEN_TEMPLATE_X_NUMS; i++) {
                for (int j = 0; j < ISP_SHARPEN_TEMPLATE_Y_NUMS; j++) {
                    sharpen->matrix[i][j] = isp_video->sharpen_matrix[i][j];
                }
            }
            break;
        }
        case V4L2_CID_USER_ESP_ISP_GAMMA: {
            esp_video_isp_gamma_t *gamma = (esp_video_isp_gamma_t *)ctrl->p_u8;

            gamma->enable = isp_video->gamma_enable;
            for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
                gamma->points[i].x = isp_video->gamma_points[i].x;
                gamma->points[i].y = isp_video->gamma_points[i].y;
            }
            break;
        }
        default:
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
        }

        if (ret != ESP_OK) {
            break;
        }
    }

    ISP_UNLOCK(isp_video);
    return ret;
}

static esp_err_t isp_video_query_ext_ctrl(struct esp_video *video, struct v4l2_query_ext_ctrl *qctrl)
{
    int num = -1;
    int id = qctrl->id;
    int isp_qctrl_cnt = ARRAY_SIZE(s_isp_qctrl);
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;

    if (id & V4L2_CTRL_FLAG_NEXT_CTRL) {
        int new_id = -1;

        id &= ~V4L2_CTRL_FLAG_NEXT_CTRL;
        if (id == 0) {
            new_id = s_isp_qctrl[0].id;
            num = 0;
        } else {
            for (int i = 0; i < isp_qctrl_cnt; i++) {
                if (id == s_isp_qctrl[i].id) {
                    if (i < (isp_qctrl_cnt - 1)) {
                        new_id = s_isp_qctrl[i + 1].id;
                        num = i + 1;
                        break;
                    }
                }
            }
        }

        if (new_id < 0) {
            return ESP_ERR_NOT_SUPPORTED;
        }

        qctrl->id = new_id;
    } else {
        for (int i = 0; i < isp_qctrl_cnt; i++) {
            if (id == s_isp_qctrl[i].id) {
                num = i;
                break;
            }
        }
    }

    if (num >= 0) {
        memcpy(qctrl, &s_isp_qctrl[num], sizeof(struct v4l2_query_ext_ctrl));
        ret = ESP_OK;
    }

    return ret;
}

static const struct esp_video_ops s_isp_video_ops = {
    .init           = isp_video_init,
    .deinit         = isp_video_deinit,
    .start          = isp_video_start,
    .stop           = isp_video_stop,
    .enum_format    = isp_video_enum_format,
    .set_format     = isp_video_set_format,
    .notify         = isp_video_notify,
    .set_ext_ctrl   = isp_video_set_ext_ctrl,
    .get_ext_ctrl   = isp_video_get_ext_ctrl,
    .query_ext_ctrl = isp_video_query_ext_ctrl,
};

/**
 * @brief Create ISP video device
 *
 * @param None
 *
 * @return
 *      - ESP_OK on success
 *      - Others if failed
 */
esp_err_t esp_video_create_isp_video_device(void)
{
    uint32_t device_caps = V4L2_CAP_EXT_PIX_FORMAT | V4L2_CAP_STREAMING;
    uint32_t caps = device_caps | V4L2_CAP_DEVICE_CAPS;

    s_isp_video.mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_isp_video.mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_isp_video.spinlock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;

    s_isp_video.video = esp_video_create(ISP_NAME, ESP_VIDEO_ISP1_DEVICE_ID, &s_isp_video_ops, &s_isp_video, caps, device_caps);
    if (!s_isp_video.video) {
        vSemaphoreDelete(s_isp_video.mutex);
        return ESP_FAIL;
    }

    s_isp_video.red_balance_gain = 1.0;
    s_isp_video.blue_balance_gain = 1.0;
    s_isp_video.ccm_matrix[0][0] = 1.0;
    s_isp_video.ccm_matrix[1][1] = 1.0;
    s_isp_video.ccm_matrix[2][2] = 1.0;

    return ESP_OK;
}
#endif

/**
 * @brief Start ISP process
 *
 * @param bypass    true: bypass ISP and MIPI-CSI output sensor original data, false: ISP process image
 * @param in_color  ISP input image color type
 * @param out_color ISP putput image color type
 * @param line_sync true: sensor data has no sync signal, false: sensor data has no sync signal
 * @param width     Image width
 * @param height    Image height
 *
 * @return
 *      - ESP_OK on success
 *      - Others if failed
 */
esp_err_t esp_video_isp_start(bool bypass, cam_ctlr_color_t in_color, cam_ctlr_color_t out_color,
                              bool line_sync, uint32_t width, uint32_t height)
{
    esp_err_t ret;
    isp_color_t isp_in_color;
    isp_color_t isp_out_color;
    struct isp_video *isp_video = &s_isp_video;

    if (bypass) {
        isp_in_color = ISP_COLOR_RAW8;
        isp_out_color = ISP_COLOR_RGB565;
    } else {
        ESP_RETURN_ON_ERROR(isp_get_input_frame_type(in_color, &isp_in_color), TAG, "invalid ISP in format");
        ESP_RETURN_ON_ERROR(isp_get_output_frame_type(out_color, &isp_out_color), TAG, "invalid ISP out format");
    }

    esp_isp_processor_cfg_t isp_config = {
        .clk_src = ISP_CLK_SRC,
        .input_data_source = ISP_INPUT_DATA_SRC,
        .has_line_start_packet = line_sync,
        .has_line_end_packet = line_sync,
        .h_res = width,
        .v_res = height,
        .clk_hz = ISP_CLK_FREQ_HZ,
        .input_data_color_type = isp_in_color,
        .output_data_color_type = isp_out_color,
    };

    ISP_LOCK(isp_video);

    ESP_GOTO_ON_ERROR(esp_isp_new_processor(&isp_config, &isp_video->isp_proc), fail_0, TAG, "failed to new ISP");

    if (bypass) {
        /**
         * IDF-9706
         */

        ISP.frame_cfg.hadr_num = ceil((float)(isp_config.h_res * 16) / 32.0) - 1;
        ISP.frame_cfg.vadr_num = isp_config.v_res - 1;
        ISP.cntl.isp_en = 0;
    } else {
        ESP_GOTO_ON_ERROR(esp_isp_enable(isp_video->isp_proc), fail_1, TAG, "failed to enable ISP");
    }

    META_VIDEO_SET_FORMAT(isp_video->video, width, height, V4L2_META_FMT_ESP_ISP_STATS);

    if (!bypass) {
        ESP_GOTO_ON_ERROR(isp_start_pipeline(isp_video), fail_2, TAG, "failed to start ISP pipeline");
    }

    ISP_UNLOCK(isp_video);
    return ESP_OK;

fail_2:
    esp_isp_disable(isp_video->isp_proc);
fail_1:
    esp_isp_del_processor(isp_video->isp_proc);
    isp_video->isp_proc = NULL;
fail_0:
    ISP_UNLOCK(isp_video);
    return ret;
}

/**
 * @brief Stop ISP process
 *
 * @param bypass true: bypass ISP and MIPI-CSI output sensor original data, false: ISP process image
 *
 * @return
 *      - ESP_OK on success
 *      - Others if failed
 */
esp_err_t esp_video_isp_stop(bool bypass)
{
    esp_err_t ret = ESP_OK;
    struct isp_video *isp_video = &s_isp_video;

    ISP_LOCK(isp_video);

    if (!bypass) {
        ESP_GOTO_ON_ERROR(isp_stop_pipeline(isp_video), exit, TAG, "failed to stop ISP pipeline");
    }

    ESP_GOTO_ON_ERROR(esp_isp_disable(isp_video->isp_proc), exit, TAG, "failed to disable ISP");
    ESP_GOTO_ON_ERROR(esp_isp_del_processor(isp_video->isp_proc), exit, TAG, "failed to delete ISP");
    isp_video->isp_proc = NULL;

exit:
    ISP_UNLOCK(isp_video);
    return ret;
}

/**
 * @brief Enumerate ISP supported output pixel format
 *
 * @param index        Enumerated number index
 * @param pixel_format Supported output pixel format
 *
 * @return
 *      - ESP_OK on success
 *      - Others if failed
 */
esp_err_t esp_video_isp_enum_format(uint32_t index, uint32_t *pixel_format)
{
    static const uint32_t isp_isp_format[] = {
        V4L2_PIX_FMT_SBGGR8,
        V4L2_PIX_FMT_RGB565,
        V4L2_PIX_FMT_RGB24,
        V4L2_PIX_FMT_YUV420,
        V4L2_PIX_FMT_YUV422P,
    };

    if (index >= ARRAY_SIZE(isp_isp_format)) {
        return ESP_ERR_INVALID_ARG;
    }

    *pixel_format = isp_isp_format[index];

    return ESP_OK;
}
/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_sccb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Perform a write transaction.
 *        The transaction will be undergoing until it finishes or it reaches
 *        the timeout provided.
 *
 * @param[in] handle SCCB IO handle
 * @param[in] write_buffer Data bytes to send on the sccb bus.
 * @param[in] write_size Size, in bytes, of the write buffer.
 * @param[in] xfer_timeout_ms Wait timeout, in ms. Note: -1 means wait forever.
 * @return
 *      - ESP_OK: sccb transmit success
 *      - ESP_ERR_INVALID_ARG: sccb transmit parameter invalid.
 *      - ESP_ERR_TIMEOUT: Operation timeout(larger than xfer_timeout_ms) because the bus is busy or hardware crash.
 */
esp_err_t esp_sccb_transmit(sccb_io_handle_t io_handle, const uint8_t *write_buffer, size_t write_size, int xfer_timeout_ms);

/**
 * @brief Perform a read transaction
 *        The transaction will be undergoing until it finishes or it reaches
 *        the timeout provided.
 *
 * @param[in] handle SCCB IO handle
 * @param[out] read_buffer Data bytes received from sccb bus.
 * @param[in] read_size Size, in bytes, of the read buffer.
 * @param[in] xfer_timeout_ms Wait timeout, in ms. Note: -1 means wait forever.
 * @return
 *      - ESP_OK: sccb receive success
 *      - ESP_ERR_INVALID_ARG: sccb receive parameter invalid.
 *      - ESP_ERR_TIMEOUT: Operation timeout(larger than xfer_timeout_ms) because the bus is busy or hardware crash.
 */
esp_err_t esp_sccb_receive(sccb_io_handle_t io_handle, uint8_t *read_buffer, size_t read_size, int xfer_timeout_ms);

/**
 * @brief Perform a write-read transaction
 *        The transaction will be undergoing until it finishes or it reaches
 *        the timeout provided.
 *
 * @param[in] handle SCCB IO handle
 * @param[in] write_buffer Data bytes to send on the sccb bus.
 * @param[in] write_size Size, in bytes, of the write buffer.
 * @param[out] read_buffer Data bytes received from sccb bus.
 * @param[in] read_size Size, in bytes, of the read buffer.
 * @param[in] xfer_timeout_ms Wait timeout, in ms. Note: -1 means wait forever.
 * @return
 *      - ESP_OK: sccb transmit-receive success
 *      - ESP_ERR_INVALID_ARG: sccb transmit parameter invalid.
 *      - ESP_ERR_TIMEOUT: Operation timeout(larger than xfer_timeout_ms) because the bus is busy or hardware crash.
 */
esp_err_t esp_sccb_transmit_receive(sccb_io_handle_t io_handle, const uint8_t *write_buffer, size_t write_size, uint8_t *read_buffer, size_t read_size, int xfer_timeout_ms);

/**
 * @brief Delete sccb controller
 *
 * @param[in] handle SCCB IO handle
 * @return
 *        - ESP_OK: If controller is successfully deleted.
 */
esp_err_t esp_sccb_del_ctlr(sccb_io_handle_t io_handle);

#ifdef __cplusplus
}
#endif

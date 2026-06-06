/**
 * @file audio_http_stream.cpp
 * @brief HTTP audio streaming implementation
 */

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"

#include "audio_http_stream.h"
#include "audio_log.h"

#if CONFIG_AUDIO_PLAYER_ENABLE_HTTP_STREAM
#include "esp_http_client.h"
#endif

static const char *TAG = "audio_http_stream";

typedef struct audio_http_stream {
    audio_http_stream_config_t cfg;
    audio_http_stream_state_t state;
    audio_http_stream_event_cb_t event_cb;
    void *event_cb_ctx;

    RingbufHandle_t ringbuf;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    SemaphoreHandle_t task_done;  // given by the task right before it deletes itself
    bool task_running;
    bool paused;
    bool eof_reached;
    bool error_occurred;
    bool id3_tag_skipped;

    size_t total_bytes_downloaded;
    size_t bytes_available;
    int content_length;  // Total content length from HTTP headers, -1 if unknown

    audio_stream_io_handle_t io_handle;

} audio_http_stream_t;

/* ================= Stream I/O callbacks for HTTP stream ================= */

static size_t http_stream_read(void *ctx, void *buf, size_t size) {
    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(ctx);
    if (!stream || !stream->ringbuf) return 0;

    size_t total_read = 0;
    uint8_t *dst = static_cast<uint8_t*>(buf);

    // Read from ring buffer
    // Retry a few times if ringbuffer is temporarily empty to avoid premature EOF
    int retry_count = 0;
    const int max_retries = 10;

    while (total_read < size && retry_count < max_retries) {
        // If EOF or error, don't wait for more data
        if (stream->eof_reached || stream->error_occurred) {
            break;
        }

        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(stream->ringbuf, &item_size,
                                              pdMS_TO_TICKS(200),
                                              size - total_read);

        if (item && item_size > 0) {
            memcpy(dst + total_read, item, item_size);
            total_read += item_size;
            vRingbufferReturnItem(stream->ringbuf, item);
            retry_count = 0;  // Reset retry count on successful read
        } else if (item) {
            vRingbufferReturnItem(stream->ringbuf, item);
        } else {
            // Ringbuffer empty - increment retry count and yield to allow download task to fill buffer
            retry_count++;
            if (retry_count < max_retries) {
                ESP_LOGD(TAG, "ringbuf empty, retry %d/%d", retry_count, max_retries);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    }

    if (retry_count >= max_retries && total_read == 0) {
        ESP_LOGW(TAG, "http_stream_read: gave up after %d retries, returning 0", max_retries);
    }

    return total_read;
}

static int http_stream_seek(void *ctx, long offset, int whence) {
    // Seek not supported for HTTP stream
    // Just return error to indicate seek is not possible
    return -1;
}

static long http_stream_tell(void *ctx) {
    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(ctx);
    if (!stream) return -1;
    return static_cast<long>(stream->total_bytes_downloaded - stream->bytes_available);
}

static int http_stream_eof(void *ctx) {
    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(ctx);
    if (!stream) return 1;
    return stream->eof_reached && stream->bytes_available == 0;
}

static void http_stream_close(void *ctx) {
    (void)ctx;
}

static const audio_stream_io_ops_t http_stream_io_ops = {
    .read = http_stream_read,
    .seek = http_stream_seek,
    .tell = http_stream_tell,
    .eof = http_stream_eof,
    .close = http_stream_close
};

/* ================= Event dispatch ================= */

static void dispatch_event(audio_http_stream_t *stream, audio_http_stream_event_t event) {
    if (stream->event_cb) {
        stream->event_cb(event, stream->event_cb_ctx);
    }
}

/* ================= HTTP download task ================= */

#if CONFIG_AUDIO_PLAYER_ENABLE_HTTP_STREAM

/* Write a chunk of received body into the ring buffer, applying back-pressure
 * (block-with-timeout) when the consumer is slow. Updates counters and triggers
 * the BUFFERING -> PLAYING transition once the high watermark is reached. */
static void http_feed_ringbuf(audio_http_stream_t *stream, const uint8_t *src, size_t len) {
    if (!stream || !stream->ringbuf || !src || len == 0) return;

    size_t remaining = len;
    while (remaining > 0 && stream->task_running) {
        // RINGBUF_TYPE_BYTEBUF send is all-or-nothing for the requested size.
        BaseType_t res = xRingbufferSend(stream->ringbuf, src, remaining, pdMS_TO_TICKS(100));
        if (res == pdTRUE) {
            stream->total_bytes_downloaded += remaining;
            remaining = 0;
        } else {
            // Ring buffer full: consumer is behind, wait and retry (TCP back-pressure).
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    UBaseType_t items_waiting = 0;
    vRingbufferGetInfo(stream->ringbuf, NULL, NULL, NULL, NULL, &items_waiting);
    stream->bytes_available = items_waiting;

    if (stream->state == AUDIO_HTTP_STREAM_STATE_BUFFERING &&
        stream->bytes_available >= stream->cfg.high_watermark) {
        stream->state = AUDIO_HTTP_STREAM_STATE_PLAYING;
        dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_BUFFER_READY);
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            // Body is read explicitly via esp_http_client_read() in the download
            // task (not through this event), so nothing to do here.
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

static void http_download_task(void *arg) {
    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(arg);

    stream->state = AUDIO_HTTP_STREAM_STATE_CONNECTING;

    while (stream->task_running) {
        ESP_LOGI(TAG, "Creating HTTP client for URL: %s", stream->cfg.url);

        esp_http_client_config_t config = {};
        config.url = stream->cfg.url;
        config.event_handler = http_event_handler;
        config.user_data = stream;            // delivered to http_event_handler
        config.timeout_ms = stream->cfg.read_timeout_ms;
        config.keep_alive_enable = false;
        config.skip_cert_common_name_check = true;
        config.buffer_size = 1024;            // HTTP rx buffer size

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client for URL: %s", stream->cfg.url);
            stream->state = AUDIO_HTTP_STREAM_STATE_ERROR;
            stream->error_occurred = true;
            dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_ERROR);

            if (stream->cfg.enable_auto_reconnect && stream->task_running) {
                vTaskDelay(pdMS_TO_TICKS(stream->cfg.reconnect_timeout_ms));
                continue;
            }
            break;
        }

        stream->state = AUDIO_HTTP_STREAM_STATE_BUFFERING;
        stream->error_occurred = false;
        dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_CONNECTED);

        // Open the connection and read the body with esp_http_client_open() +
        // esp_http_client_read() — the same path the (reliable) /api/esp fetch
        // uses. The server returns a finite track with Content-Length, so read()
        // returns 0 only once the whole song has been received.
        ESP_LOGI(TAG, "Opening HTTP connection...");
        bool got_error = false;
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            got_error = true;
        } else {
            int content_length = esp_http_client_fetch_headers(client);
            stream->content_length = content_length;
            ESP_LOGI(TAG, "Content length: %d", content_length);

            if (content_length < 0) {
                // Negative = header fetch failed (e.g. timeout/EAGAIN).
                got_error = true;
            } else {
                uint8_t *buf = static_cast<uint8_t*>(malloc(1024));
                if (!buf) {
                    ESP_LOGE(TAG, "read buffer alloc failed");
                    got_error = true;
                } else {
                    while (stream->task_running) {
                        int r = esp_http_client_read(client, reinterpret_cast<char*>(buf), 1024);
                        if (r > 0) {
                            http_feed_ringbuf(stream, buf, (size_t)r);
                        } else if (r == 0) {
                            // read()==0 means either the whole body arrived OR the
                            // connection was closed early (common on a weak link).
                            // Only treat it as end-of-track when ALL Content-Length
                            // bytes were actually received; otherwise it's a dropped
                            // connection → retry instead of falsely "finishing".
                            if (esp_http_client_is_complete_data_received(client)) {
                                break;  // genuine end of track
                            }
                            ESP_LOGW(TAG, "connection closed early (%d/%d bytes) — will retry",
                                     (int)stream->total_bytes_downloaded, content_length);
                            got_error = true;
                            break;
                        } else {
                            ESP_LOGW(TAG, "HTTP read error: %d", r);
                            got_error = true;
                            break;
                        }
                    }
                    free(buf);
                }
            }
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (!stream->task_running) {
            break;
        }

        if (!got_error) {
            // Whole track received. Signal FINISHED and exit so the consumer can
            // pick the next song — do NOT reconnect (that would replay this URL).
            ESP_LOGI(TAG, "HTTP stream complete");
            stream->eof_reached = true;
            stream->state = AUDIO_HTTP_STREAM_STATE_FINISHED;
            dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_FINISHED);
            break;
        }

        // Transfer error / timeout. Retry the same URL when auto-reconnect is
        // enabled, otherwise report an error and stop.
        if (stream->cfg.enable_auto_reconnect) {
            ESP_LOGI(TAG, "Reconnecting in %d ms...", stream->cfg.reconnect_timeout_ms);
            dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_DISCONNECTED);
            vTaskDelay(pdMS_TO_TICKS(stream->cfg.reconnect_timeout_ms));
            continue;
        }

        stream->error_occurred = true;
        stream->state = AUDIO_HTTP_STREAM_STATE_ERROR;
        dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_ERROR);
        break;
    }

    // Signal clean exit AFTER all access to `stream` is done, so close() can
    // safely free the stream only once the task is guaranteed out of perform().
    stream->task = NULL;
    if (stream->task_done) xSemaphoreGive(stream->task_done);
    vTaskDelete(NULL);
}

#else

static void http_download_task(void *arg) {
    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(arg);
    ESP_LOGE(TAG, "HTTP streaming not enabled. Set CONFIG_AUDIO_PLAYER_ENABLE_HTTP_STREAM=y");
    stream->state = AUDIO_HTTP_STREAM_STATE_ERROR;
    stream->error_occurred = true;
    dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_ERROR);
    stream->task = NULL;
    if (stream->task_done) xSemaphoreGive(stream->task_done);
    vTaskDelete(NULL);
}

#endif

/* ================= Public API ================= */

audio_http_stream_handle_t audio_http_stream_open(const audio_http_stream_config_t *cfg) {
    ESP_RETURN_ON_FALSE(cfg != NULL, NULL, TAG, "cfg is NULL");
    ESP_RETURN_ON_FALSE(cfg->url != NULL, NULL, TAG, "url is NULL");

    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(calloc(1, sizeof(audio_http_stream_t)));
    ESP_RETURN_ON_FALSE(stream != NULL, NULL, TAG, "allocation failed");

    // Copy the URL string since caller may free it
    char *url_copy = strdup(cfg->url);
    if (!url_copy) {
        ESP_LOGE(TAG, "Failed to copy URL string");
        free(stream);
        return NULL;
    }

    stream->cfg = *cfg;
    stream->cfg.url = url_copy;

    if (stream->cfg.buffer_size == 0) {
        stream->cfg.buffer_size = 32 * 1024;
    }
    if (stream->cfg.low_watermark == 0) {
        stream->cfg.low_watermark = 8 * 1024;
    }
    if (stream->cfg.high_watermark == 0) {
        stream->cfg.high_watermark = 24 * 1024;
    }
    if (stream->cfg.low_watermark >= stream->cfg.high_watermark) {
        stream->cfg.low_watermark = stream->cfg.high_watermark / 2;
    }
    if (stream->cfg.task_stack_size == 0) {
        stream->cfg.task_stack_size = 4096;
    }
    if (stream->cfg.task_priority == 0) {
        stream->cfg.task_priority = tskIDLE_PRIORITY + 1;
    }

    stream->ringbuf = xRingbufferCreate(stream->cfg.buffer_size, RINGBUF_TYPE_BYTEBUF);
    if (!stream->ringbuf) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        free(url_copy);
        free(stream);
        return NULL;
    }

    stream->mutex = xSemaphoreCreateMutex();
    if (!stream->mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        vRingbufferDelete(stream->ringbuf);
        free(url_copy);
        free(stream);
        return NULL;
    }

    stream->task_done = xSemaphoreCreateBinary();
    if (!stream->task_done) {
        ESP_LOGE(TAG, "Failed to create task_done semaphore");
        vSemaphoreDelete(stream->mutex);
        vRingbufferDelete(stream->ringbuf);
        free(url_copy);
        free(stream);
        return NULL;
    }

    stream->state = AUDIO_HTTP_STREAM_STATE_IDLE;
    stream->task_running = true;

    stream->io_handle = audio_stream_io_create(&http_stream_io_ops, stream);
    if (!stream->io_handle) {
        ESP_LOGE(TAG, "Failed to create stream I/O");
        vSemaphoreDelete(stream->task_done);
        vSemaphoreDelete(stream->mutex);
        vRingbufferDelete(stream->ringbuf);
        free(url_copy);
        free(stream);
        return NULL;
    }

    BaseType_t res = xTaskCreatePinnedToCore(
        http_download_task,
        "audio_http",
        stream->cfg.task_stack_size,
        stream,
        stream->cfg.task_priority,
        &stream->task,
        stream->cfg.task_core_id
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create download task");
        audio_stream_io_close(stream->io_handle);
        vSemaphoreDelete(stream->task_done);
        vSemaphoreDelete(stream->mutex);
        vRingbufferDelete(stream->ringbuf);
        free(url_copy);
        free(stream);
        return NULL;
    }

    return stream;
}

esp_err_t audio_http_stream_get_io(audio_http_stream_handle_t h, audio_stream_io_handle_t *io_out) {
    ESP_RETURN_ON_FALSE(h != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(io_out != NULL, ESP_ERR_INVALID_ARG, TAG, "io_out is NULL");

    *io_out = h->io_handle;
    return ESP_OK;
}

esp_err_t audio_http_stream_register_cb(audio_http_stream_handle_t h,
                                          audio_http_stream_event_cb_t cb,
                                          void *user_ctx) {
    ESP_RETURN_ON_FALSE(h != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    if (h->mutex) xSemaphoreTake(h->mutex, portMAX_DELAY);
    h->event_cb = cb;
    h->event_cb_ctx = user_ctx;
    if (h->mutex) xSemaphoreGive(h->mutex);

    return ESP_OK;
}

audio_http_stream_state_t audio_http_stream_get_state(audio_http_stream_handle_t h) {
    if (!h) return AUDIO_HTTP_STREAM_STATE_ERROR;
    return h->state;
}

size_t audio_http_stream_get_buffered_bytes(audio_http_stream_handle_t h) {
    if (!h) return 0;

    UBaseType_t items_waiting = 0;
    if (h->ringbuf) {
        vRingbufferGetInfo(h->ringbuf, NULL, NULL, NULL, NULL, &items_waiting);
    }
    return items_waiting;
}

size_t audio_http_stream_get_total_bytes(audio_http_stream_handle_t h) {
    if (!h) return 0;
    return h->total_bytes_downloaded;
}

int audio_http_stream_get_content_length(audio_http_stream_handle_t h) {
    if (!h) return -1;
    return h->content_length;
}

esp_err_t audio_http_stream_pause(audio_http_stream_handle_t h) {
    ESP_RETURN_ON_FALSE(h != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    h->paused = true;
    if (h->state == AUDIO_HTTP_STREAM_STATE_PLAYING ||
        h->state == AUDIO_HTTP_STREAM_STATE_BUFFERING) {
        h->state = AUDIO_HTTP_STREAM_STATE_PAUSED;
    }

    return ESP_OK;
}

esp_err_t audio_http_stream_resume(audio_http_stream_handle_t h) {
    ESP_RETURN_ON_FALSE(h != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    h->paused = false;
    if (h->state == AUDIO_HTTP_STREAM_STATE_PAUSED) {
        h->state = AUDIO_HTTP_STREAM_STATE_BUFFERING;
    }

    return ESP_OK;
}

esp_err_t audio_http_stream_close(audio_http_stream_handle_t h) {
    if (!h) return ESP_OK;

    h->task_running = false;

    /* Drain the ring buffer so the download task unblocks from xRingbufferSend()
     * and can check task_running. Without this, a full ringbuf (no consumer)
     * causes the task to spin for up to 100ms per chunk and take seconds to exit,
     * or worse: the 15s timeout fires and the socket leaks. */
    if (h->ringbuf) {
        size_t item_size = 0;
        void *item;
        while ((item = xRingbufferReceive(h->ringbuf, &item_size, 0)) != NULL) {
            vRingbufferReturnItem(h->ringbuf, item);
        }
    }

    /* Wait for the download task to fully exit before freeing anything — do NOT
     * vTaskDelete it. The task may be blocked inside esp_http_client_perform()'s
     * lwIP socket call; killing it there corrupts lwIP/FreeRTOS state. The task
     * gives task_done as its very last action (after all use of the stream and
     * its ring buffer), so this Take guarantees the task is out of perform().
     *
     * If the task does NOT signal in time (e.g. perform() is wedged in a long
     * TCP connect), we DELIBERATELY LEAK the stream instead of freeing it: a
     * one-off leak is recoverable, but freeing memory still in use by the live
     * task causes a use-after-free crash (writing the freed ring buffer). */
    if (h->task_done) {
        if (xSemaphoreTake(h->task_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
            ESP_LOGE(TAG, "download task did not exit in time; leaking stream to avoid use-after-free");
            return ESP_ERR_TIMEOUT;
        }
        vSemaphoreDelete(h->task_done);
        h->task_done = NULL;
    }
    h->task = NULL;

    if (h->io_handle) {
        // Don't close io_handle here - it's owned and closed by the audio player
        h->io_handle = NULL;
    }

    if (h->ringbuf) {
        vRingbufferDelete(h->ringbuf);
        h->ringbuf = NULL;
    }

    if (h->mutex) {
        vSemaphoreDelete(h->mutex);
        h->mutex = NULL;
    }

    if (h->cfg.url) {
        free((void*)h->cfg.url);
        h->cfg.url = NULL;
    }

    free(h);
    return ESP_OK;
}

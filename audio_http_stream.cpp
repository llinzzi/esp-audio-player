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
    bool task_running;
    bool paused;
    bool eof_reached;
    bool error_occurred;

    size_t total_bytes_downloaded;
    size_t bytes_available;

    audio_stream_io_handle_t io_handle;
} audio_http_stream_t;

/* ================= Stream I/O callbacks for HTTP stream ================= */

static size_t http_stream_read(void *ctx, void *buf, size_t size) {
    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(ctx);
    if (!stream || !stream->ringbuf) return 0;

    size_t total_read = 0;
    uint8_t *dst = static_cast<uint8_t*>(buf);

    while (total_read < size) {
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(stream->ringbuf, &item_size,
                                              pdMS_TO_TICKS(100),
                                              size - total_read);

        if (item && item_size > 0) {
            memcpy(dst + total_read, item, item_size);
            total_read += item_size;
            vRingbufferReturnItem(stream->ringbuf, item);
        } else if (item) {
            vRingbufferReturnItem(stream->ringbuf, item);
        } else {
            if (stream->eof_reached) {
                break;
            }
            if (stream->error_occurred) {
                break;
            }
        }
    }

    return total_read;
}

static int http_stream_seek(void *ctx, long offset, int whence) {
    (void)ctx;
    (void)offset;
    (void)whence;
    ESP_LOGW(TAG, "seek not supported on HTTP streams");
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
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}

static void http_download_task(void *arg) {
    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(arg);
    esp_http_client_handle_t client = NULL;
    esp_err_t err;

    stream->state = AUDIO_HTTP_STREAM_STATE_CONNECTING;

    while (stream->task_running) {
        if (stream->paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!client) {
            esp_http_client_config_t config = {};
            config.url = stream->cfg.url;
            config.event_handler = http_event_handler;
            config.timeout_ms = stream->cfg.read_timeout_ms;
            config.keep_alive_enable = true;

            client = esp_http_client_init(&config);
            if (!client) {
                ESP_LOGE(TAG, "Failed to initialize HTTP client");
                stream->state = AUDIO_HTTP_STREAM_STATE_ERROR;
                stream->error_occurred = true;
                dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_ERROR);

                if (stream->cfg.enable_auto_reconnect) {
                    vTaskDelay(pdMS_TO_TICKS(stream->cfg.reconnect_timeout_ms));
                    continue;
                }
                break;
            }

            err = esp_http_client_open(client, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
                esp_http_client_cleanup(client);
                client = NULL;
                stream->state = AUDIO_HTTP_STREAM_STATE_ERROR;
                dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_DISCONNECTED);

                if (stream->cfg.enable_auto_reconnect) {
                    vTaskDelay(pdMS_TO_TICKS(stream->cfg.reconnect_timeout_ms));
                    continue;
                }
                break;
            }

            stream->state = AUDIO_HTTP_STREAM_STATE_BUFFERING;
            dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_CONNECTED);

            int content_length = esp_http_client_fetch_headers(client);
            ESP_LOGI(TAG, "Content length: %d", content_length);
        }

        uint8_t *buf = static_cast<uint8_t*>(malloc(1024));
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate read buffer");
            break;
        }

        int read_len = esp_http_client_read(client, reinterpret_cast<char*>(buf), 1024);

        if (read_len > 0) {
            size_t written = 0;
            while (written < static_cast<size_t>(read_len)) {
                size_t to_write = read_len - written;
                BaseType_t res = xRingbufferSend(stream->ringbuf, buf + written, to_write, pdMS_TO_TICKS(100));
                if (res == pdTRUE) {
                    written += to_write;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }

            stream->total_bytes_downloaded += read_len;

            UBaseType_t items_waiting = 0;
            vRingbufferGetInfo(stream->ringbuf, NULL, NULL, NULL, NULL, &items_waiting);
            stream->bytes_available = items_waiting;

            if (stream->state == AUDIO_HTTP_STREAM_STATE_BUFFERING &&
                stream->bytes_available >= stream->cfg.high_watermark) {
                stream->state = AUDIO_HTTP_STREAM_STATE_PLAYING;
                dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_BUFFER_READY);
            }
        } else if (read_len == 0) {
            ESP_LOGI(TAG, "End of HTTP stream");
            stream->eof_reached = true;
            stream->state = AUDIO_HTTP_STREAM_STATE_FINISHED;
            dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_FINISHED);
            break;
        } else {
            ESP_LOGE(TAG, "HTTP read error: %d", read_len);
            esp_http_client_cleanup(client);
            client = NULL;
            stream->state = AUDIO_HTTP_STREAM_STATE_ERROR;
            stream->error_occurred = true;
            dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_ERROR);

            if (stream->cfg.enable_auto_reconnect) {
                vTaskDelay(pdMS_TO_TICKS(stream->cfg.reconnect_timeout_ms));
                stream->error_occurred = false;
                continue;
            }
            break;
        }

        free(buf);

        if (stream->state == AUDIO_HTTP_STREAM_STATE_PLAYING &&
            stream->bytes_available < stream->cfg.low_watermark) {
            stream->state = AUDIO_HTTP_STREAM_STATE_BUFFERING;
            dispatch_event(stream, AUDIO_HTTP_STREAM_EVENT_BUFFERING);
        }
    }

    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    stream->task = NULL;
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
    vTaskDelete(NULL);
}

#endif

/* ================= Public API ================= */

audio_http_stream_handle_t audio_http_stream_open(const audio_http_stream_config_t *cfg) {
    ESP_RETURN_ON_FALSE(cfg != NULL, NULL, TAG, "cfg is NULL");
    ESP_RETURN_ON_FALSE(cfg->url != NULL, NULL, TAG, "url is NULL");

    audio_http_stream_t *stream = static_cast<audio_http_stream_t*>(calloc(1, sizeof(audio_http_stream_t)));
    ESP_RETURN_ON_FALSE(stream != NULL, NULL, TAG, "allocation failed");

    stream->cfg = *cfg;

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
        free(stream);
        return NULL;
    }

    stream->mutex = xSemaphoreCreateMutex();
    if (!stream->mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        vRingbufferDelete(stream->ringbuf);
        free(stream);
        return NULL;
    }

    stream->state = AUDIO_HTTP_STREAM_STATE_IDLE;
    stream->task_running = true;

    stream->io_handle = audio_stream_io_create(&http_stream_io_ops, stream);
    if (!stream->io_handle) {
        ESP_LOGE(TAG, "Failed to create stream I/O");
        vSemaphoreDelete(stream->mutex);
        vRingbufferDelete(stream->ringbuf);
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
        vSemaphoreDelete(stream->mutex);
        vRingbufferDelete(stream->ringbuf);
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

    if (h->task) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (h->io_handle) {
        audio_stream_io_close(h->io_handle);
    }

    if (h->ringbuf) {
        vRingbufferDelete(h->ringbuf);
    }

    if (h->mutex) {
        vSemaphoreDelete(h->mutex);
    }

    free(h);
    return ESP_OK;
}

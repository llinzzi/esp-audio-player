# HTTP 网络音频流使用示例

## 配置

在 `sdkconfig` 中启用 HTTP 流支持：

```
CONFIG_AUDIO_PLAYER_ENABLE_HTTP_STREAM=y
```

## 快速开始

### 最简单用法

```c
#include "audio_stream.h"

// 初始化混音器
audio_mixer_config_t mixer_cfg = {
    .write_fn = bsp_i2s_write,
    .clk_set_fn = bsp_i2s_reconfig_clk,
    .priority = 5,
    .coreID = 0,
    .i2s_format = {
        .sample_rate = 44100,
        .bits_per_sample = 16,
        .channels = 2
    }
};
audio_mixer_init(&mixer_cfg);

// 创建音频流
audio_stream_config_t stream_cfg = DEFAULT_AUDIO_STREAM_CONFIG("network");
audio_stream_handle_t stream = audio_stream_new(&stream_cfg);

// 播放网络音频
audio_stream_play_url(stream, "http://example.com/audio.mp3");
```

### 带配置的用法

```c
#include "audio_stream.h"
#include "audio_http_stream.h"

// 自定义配置
audio_http_stream_config_t http_cfg = DEFAULT_AUDIO_HTTP_STREAM_CONFIG(
    "http://example.com/audio.mp3"
);

// 增大缓冲区
http_cfg.buffer_size = 64 * 1024;        // 64KB 缓冲区
http_cfg.low_watermark = 16 * 1024;       // 16KB 低水位
http_cfg.high_watermark = 48 * 1024;      // 48KB 高水位
http_cfg.read_timeout_ms = 30000;          // 30秒读取超时

// 带配置播放
audio_stream_play_url_with_config(stream, "http://example.com/audio.mp3", &http_cfg);
```

### 使用事件回调

```c
#include "audio_http_stream.h"

static void http_stream_event_handler(audio_http_stream_event_t event, void *user_ctx) {
    switch (event) {
        case AUDIO_HTTP_STREAM_EVENT_CONNECTED:
            ESP_LOGI(TAG, "已连接到服务器");
            break;
        case AUDIO_HTTP_STREAM_EVENT_BUFFERING:
            ESP_LOGI(TAG, "缓冲中...");
            break;
        case AUDIO_HTTP_STREAM_EVENT_BUFFER_READY:
            ESP_LOGI(TAG, "缓冲完成，开始播放");
            break;
        case AUDIO_HTTP_STREAM_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "连接断开");
            break;
        case AUDIO_HTTP_STREAM_EVENT_ERROR:
            ESP_LOGE(TAG, "播放错误");
            break;
        case AUDIO_HTTP_STREAM_EVENT_FINISHED:
            ESP_LOGI(TAG, "播放完成");
            break;
    }
}

// 直接使用 HTTP 流 API
audio_http_stream_config_t cfg = DEFAULT_AUDIO_HTTP_STREAM_CONFIG(url);
audio_http_stream_handle_t http_stream = audio_http_stream_open(&cfg);

// 注册回调
audio_http_stream_register_cb(http_stream, http_stream_event_handler, NULL);

// 获取 stream_io 并播放
audio_stream_io_handle_t io;
audio_http_stream_get_io(http_stream, &io);
audio_stream_play_io(stream, io);
```

### 暂停和恢复下载

```c
// 暂停下载（继续播放已缓冲内容）
audio_http_stream_pause(http_stream);

// 恢复下载
audio_http_stream_resume(http_stream);
```

### 查询状态

```c
// 获取当前状态
audio_http_stream_state_t state = audio_http_stream_get_state(http_stream);

// 获取已缓冲字节数
size_t buffered = audio_http_stream_get_buffered_bytes(http_stream);
ESP_LOGI(TAG, "已缓冲: %u 字节", buffered);

// 获取总下载字节数
size_t total = audio_http_stream_get_total_bytes(http_stream);
ESP_LOGI(TAG, "总下载: %u 字节", total);
```

## 完整示例

```c
#include <stdio.h>
#include "audio_player.h"
#include "audio_mixer.h"
#include "audio_stream.h"
#include "audio_http_stream.h"

// I2S 配置（根据您的硬件修改）
#define BSP_I2S_GPIO_CFG { ... }

static i2s_chan_handle_t i2s_tx_chan;

static esp_err_t bsp_i2s_write(void *audio_buffer, size_t len,
                                 size_t *bytes_written, uint32_t timeout_ms) {
    return i2s_channel_write(i2s_tx_chan, audio_buffer, len, bytes_written, timeout_ms);
}

static esp_err_t bsp_i2s_reconfig_clk(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch) {
    // 重新配置 I2S 时钟
    return ESP_OK;
}

static void http_event_callback(audio_http_stream_event_t event, void *user_ctx) {
    switch (event) {
        case AUDIO_HTTP_STREAM_EVENT_CONNECTED:
            ESP_LOGI("APP", "连接成功");
            break;
        case AUDIO_HTTP_STREAM_EVENT_BUFFERING:
            ESP_LOGI("APP", "正在缓冲...");
            break;
        case AUDIO_HTTP_STREAM_EVENT_BUFFER_READY:
            ESP_LOGI("APP", "开始播放");
            break;
        case AUDIO_HTTP_STREAM_EVENT_ERROR:
            ESP_LOGE("APP", "播放出错");
            break;
        default:
            break;
    }
}

void app_main(void) {
    // 1. 初始化 I2S
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(0, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, &i2s_tx_chan, NULL);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = BSP_I2S_GPIO_CFG,
    };
    i2s_channel_init_std_mode(i2s_tx_chan, &std_cfg);
    i2s_channel_enable(i2s_tx_chan);

    // 2. 初始化混音器
    audio_mixer_config_t mixer_cfg = {
        .write_fn = bsp_i2s_write,
        .clk_set_fn = bsp_i2s_reconfig_clk,
        .priority = 5,
        .coreID = 0,
        .i2s_format = {
            .sample_rate = 44100,
            .bits_per_sample = 16,
            .channels = 2
        }
    };
    audio_mixer_init(&mixer_cfg);

    // 3. 创建音频流
    audio_stream_config_t stream_cfg = DEFAULT_AUDIO_STREAM_CONFIG("radio");
    audio_stream_handle_t stream = audio_stream_new(&stream_cfg);

    // 4. 播放网络音频
    const char *url = "http://example.com/music.mp3";
    ESP_LOGI("APP", "开始播放: %s", url);

    audio_http_stream_config_t http_cfg = DEFAULT_AUDIO_HTTP_STREAM_CONFIG(url);
    audio_stream_play_url_with_config(stream, url, &http_cfg);
}
```

## 注意事项

1. **HTTPS 支持**: 如果使用 HTTPS，需要确保已加载 CA 证书
2. **内存使用**: 增大缓冲区会增加内存占用
3. **网络稳定性**: 弱网环境建议增大缓冲区和超时时间
4. **Task 优先级**: HTTP 下载任务优先级应足够高以防止缓冲区欠载

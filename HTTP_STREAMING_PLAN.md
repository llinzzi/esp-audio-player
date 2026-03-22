# HTTP 网络音频流播放实现规划

## 概述

本规划文档描述如何为 esp-audio-player 项目添加 HTTP 网络音频流播放功能。

## 现状分析

### 当前架构

```
应用层
    ├─ audio_player_* (单流 API)
    └─ audio_stream_* (多流混音 API)
          ↓
核心层 (audio_instance / audio_mixer)
          ↓
解码层 (audio_mp3, audio_wav)
          ↓
硬件抽象层 (I2S 回调)
```

### 当前数据流程

1. 从 `FILE*` 读取数据（本地文件系统）
2. 解码器逐帧解码
3. PCM 数据写入 I2S 或混音器环形缓冲区

### 关键限制

- 解码器直接使用 `FILE*` 进行文件操作
- 没有抽象的输入流接口
- 不支持网络数据源

## 设计方案

### 方案概述

创建一个抽象的输入流接口，允许从不同数据源（文件、HTTP、内存等）读取数据。

### 架构设计

```
┌─────────────────────────────────────────────────────────┐
│                   应用层 API                              │
│  audio_stream_play_url() / audio_http_stream_play()    │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│              HTTP Stream 模块 (新增)                      │
│  - HTTP 连接管理                                          │
│  - 数据缓冲环形缓冲区                                      │
│  - 下载任务                                              │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│           输入流抽象层 (audio_stream_io) (新增)           │
│  ┌───────────────────────────────────────────────────┐  │
│  │ typedef struct {                                    │  │
│  │     size_t (*read)(void *ctx, void *buf, size_t);│  │
│  │     int    (*seek)(void *ctx, long offset, int);  │  │
│  │     long   (*tell)(void *ctx);                     │  │
│  │     int    (*eof)(void *ctx);                      │  │
│  │     void   (*close)(void *ctx);                    │  │
│  │     void   *ctx;                                    │  │
│  │ } audio_stream_io_t;                                │  │
│  └───────────────────────────────────────────────────┘  │
│  - 文件流实现 (fopen/fread 封装)                         │
│  - HTTP 流实现 (环形缓冲区读取)                          │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│                 解码器修改 (audio_mp3/audio_wav)        │
│  - 接受 audio_stream_io_t 而非 FILE*                    │
│  - 保持原有解码逻辑不变                                   │
└────────────────────┬────────────────────────────────────┘
                     │
              现有混音/输出架构
```

## 实现步骤

### 阶段 1: 输入流抽象层 (基础)

**文件**: `audio_stream_io.h`, `audio_stream_io.cpp`

1. 定义 `audio_stream_io_t` 结构体
2. 实现文件流包装器 (`audio_stream_io_from_file()`)
3. 更新解码器接口以支持 `audio_stream_io_t`
4. 保持向后兼容（仍然接受 `FILE*`）

### 阶段 2: HTTP 流模块

**文件**: `audio_http_stream.h`, `audio_http_stream.cpp`

1. HTTP 流配置结构体
   ```c
   typedef struct {
       const char *url;
       size_t buffer_size;           // 环形缓冲区大小 (默认: 32KB)
       uint32_t reconnect_timeout_ms;
       uint32_t read_timeout_ms;
       // 可选: 自定义 HTTP 头
   } audio_http_stream_config_t;
   ```

2. HTTP 流句柄和状态
   ```c
   typedef struct audio_http_stream* audio_http_stream_handle_t;

   typedef enum {
       AUDIO_HTTP_STREAM_STATE_IDLE,
       AUDIO_HTTP_STREAM_STATE_CONNECTING,
       AUDIO_HTTP_STREAM_STATE_BUFFERING,
       AUDIO_HTTP_STREAM_STATE_PLAYING,
       AUDIO_HTTP_STREAM_STATE_PAUSED,
       AUDIO_HTTP_STREAM_STATE_ERROR,
       AUDIO_HTTP_STREAM_STATE_DISCONNECTED
   } audio_http_stream_state_t;
   ```

3. HTTP 下载任务
   - 使用 ESP-IDF `esp_http_client`
   - 数据写入环形缓冲区
   - 处理重连、缓冲不足等情况

4. HTTP 流回调事件
   ```c
   typedef enum {
       AUDIO_HTTP_STREAM_EVENT_CONNECTED,
       AUDIO_HTTP_STREAM_EVENT_BUFFERING,        // 缓冲中 (低于低水位)
       AUDIO_HTTP_STREAM_EVENT_BUFFER_READY,     // 缓冲足够 (高于高水位)
       AUDIO_HTTP_STREAM_EVENT_DISCONNECTED,
       AUDIO_HTTP_STREAM_EVENT_ERROR,
       AUDIO_HTTP_STREAM_EVENT_FINISHED
   } audio_http_stream_event_t;
   ```

### 阶段 3: 集成到现有 API

**修改文件**: `audio_stream.h`, `audio_mixer.cpp`

1. 新增 API 函数
   ```c
   // 直接从 URL 播放（创建流 + HTTP 客户端 + 解码）
   esp_err_t audio_stream_play_url(audio_stream_handle_t h, const char *url);

   // 或者更细粒度的控制：
   audio_http_stream_handle_t audio_http_stream_open(audio_http_stream_config_t *cfg);
   esp_err_t audio_http_stream_get_stream_io(audio_http_stream_handle_t h, audio_stream_io_t **io);
   esp_err_t audio_stream_play_from_io(audio_stream_handle_t h, audio_stream_io_t *io);
   ```

2. 更新 `audio_instance_t` 以支持 `audio_stream_io_t`

### 阶段 4: 配置和依赖

**修改文件**: `CMakeLists.txt`, `Kconfig` (可选)

1. 添加 HTTP 支持的可选配置
   ```cmake
   if(CONFIG_AUDIO_PLAYER_ENABLE_HTTP_STREAM)
       list(APPEND srcs "audio_http_stream.cpp")
       list(APPEND requires "esp_http_client")
   endif()
   ```

2. Kconfig 选项
   - `CONFIG_AUDIO_PLAYER_ENABLE_HTTP_STREAM`
   - `CONFIG_AUDIO_PLAYER_HTTP_BUFFER_SIZE`
   - `CONFIG_AUDIO_PLAYER_HTTP_LOW_WATERMARK`
   - `CONFIG_AUDIO_PLAYER_HTTP_HIGH_WATERMARK`

## 数据流程详情

### HTTP 播放流程

```
应用调用 audio_stream_play_url(stream, "http://example.com/audio.mp3")
    │
    ├─ 创建 HTTP 流句柄
    ├─ 创建环形缓冲区 (例如 32KB)
    ├─ 启动 HTTP 下载任务
    │       │
    │       ├─ 连接 HTTP 服务器
    │       ├─ 接收 HTTP 响应头
    │       ├─ 下载数据 → 写入环形缓冲区
    │       └─ 持续下载...
    │
    ├─ 等待缓冲达到高水位 (例如 50%)
    │
    ├─ 创建 audio_stream_io_t 绑定到 HTTP 流
    │
    └─ 调用 audio_instance_play_from_io(instance, io)
            │
            └─ 解码器从 io->read() 读取数据
                    │
                    └─ 从环形缓冲区读取
                            │
                            └─ 如果缓冲区空，解码器阻塞等待
```

### 缓冲策略

```
              高水位 (开始播放) -> [===========]
              低水位 (暂停播放) -> [====       ]

状态机:
    IDLE → CONNECTING → BUFFERING → (缓冲≥高水位) → PLAYING
                                                     ↓
                                            (缓冲≤低水位)
                                                     ↓
                                              BUFFERING_WAIT
                                                     ↓
                                            (缓冲≥高水位)
                                                     ↓
                                                  PLAYING
```

## API 设计

### 高层 API (简单使用)

```c
/**
 * @brief 从 URL 播放音频
 *
 * @param h 音频流句柄
 * @param url HTTP/HTTPS URL
 * @return esp_err_t
 */
esp_err_t audio_stream_play_url(audio_stream_handle_t h, const char *url);

/**
 * @brief 从 URL 播放音频（带配置）
 *
 * @param h 音频流句柄
 * @param url HTTP/HTTPS URL
 * @param cfg HTTP 流配置（可为 NULL 使用默认）
 * @return esp_err_t
 */
esp_err_t audio_stream_play_url_with_config(audio_stream_handle_t h, const char *url,
                                              audio_http_stream_config_t *cfg);
```

### 低层 API (高级控制)

```c
/**
 * @brief 打开 HTTP 流
 *
 * @param cfg 配置
 * @return HTTP 流句柄，失败返回 NULL
 */
audio_http_stream_handle_t audio_http_stream_open(audio_http_stream_config_t *cfg);

/**
 * @brief 获取流 IO 接口
 *
 * @param h HTTP 流句柄
 * @param io_out 输出 IO 接口指针
 * @return esp_err_t
 */
esp_err_t audio_http_stream_get_io(audio_http_stream_handle_t h, audio_stream_io_t **io_out);

/**
 * @brief 注册 HTTP 流事件回调
 */
typedef void (*audio_http_stream_event_cb_t)(audio_http_stream_event_t event, void *user_ctx);
esp_err_t audio_http_stream_register_cb(audio_http_stream_handle_t h,
                                          audio_http_stream_event_cb_t cb,
                                          void *user_ctx);

/**
 * @brief 获取当前缓冲级别
 *
 * @param h HTTP 流句柄
 * @return 已缓冲字节数
 */
size_t audio_http_stream_get_buffered_bytes(audio_http_stream_handle_t h);

/**
 * @brief 关闭 HTTP 流
 */
esp_err_t audio_http_stream_close(audio_http_stream_handle_t h);

/**
 * @brief 从流 IO 播放（兼容文件/HTTP/内存流）
 */
esp_err_t audio_stream_play_from_io(audio_stream_handle_t h, audio_stream_io_t *io);
```

## 文件清单

### 新增文件

```
include/
  ├─ audio_stream_io.h         # 输入流抽象接口
  └─ audio_http_stream.h       # HTTP 流公共 API

audio_stream_io.cpp            # 输入流抽象实现 + 文件流
audio_http_stream.cpp          # HTTP 流实现
```

### 修改文件

```
include/
  ├─ audio_player.h            # 添加 play_from_io 声明
  └─ audio_stream.h            # 添加 play_url 声明

audio_player.cpp               # 修改解码器以使用 audio_stream_io_t
audio_mixer.cpp                # 添加 HTTP 流集成
audio_mp3.cpp                  # 修改以使用 audio_stream_io_t
audio_wav.cpp                  # 修改以使用 audio_stream_io_t
audio_decode_types.h           # 可能需要修改
audio_instance.h               # 添加 stream_io 支持

CMakeLists.txt                 # 添加 HTTP 流源文件和依赖
Kconfig                        # 添加配置选项（可选）
```

## 依赖项

- ESP-IDF 组件: `esp_http_client`
- 可选 HTTPS 支持: `esp-tls`

## 测试计划

1. 单元测试: `audio_stream_io` 的文件流读写
2. 集成测试: 播放本地 MP3 通过 stream_io 接口
3. 集成测试: 播放 HTTP MP3 流
4. 网络异常测试: 中断连接、超时等恢复测试

## 向后兼容性

- 保持所有现有 API 不变
- `FILE*` 参数在内部转换为 `audio_stream_io_t`
- 现有代码无需修改

## 风险和注意事项

1. **内存使用**: 环形缓冲区会增加内存占用
   - 缓解: 可配置缓冲区大小，默认合理值

2. **网络延迟**: 初始缓冲会导致播放延迟
   - 缓解: 提供缓冲事件回调，用户可显示"缓冲中"

3. **HTTPS 证书**: HTTPS 流需要证书管理
   - 缓解: 文档说明如何配置证书

4. **解码器不支持流式**: 某些解码器可能需要 seek
   - 缓解: MP3/WAV 都支持流式，seek 可返回错误

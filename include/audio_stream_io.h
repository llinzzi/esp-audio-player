/**
 * @file audio_stream_io.h
 * @brief Abstract stream I/O interface for audio data sources
 *
 * This header provides an abstract interface for reading audio data
 * from various sources (files, HTTP streams, memory buffers, etc.).
 */
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle for a stream I/O context
 */
typedef struct audio_stream_io* audio_stream_io_handle_t;

/**
 * @brief Seek origin values (matches stdio.h constants)
 */
#define AUDIO_STREAM_SEEK_SET  0   /**< Seek from beginning of file */
#define AUDIO_STREAM_SEEK_CUR  1   /**< Seek from current position */
#define AUDIO_STREAM_SEEK_END  2   /**< Seek from end of file */

/**
 * @brief Stream I/O operations table
 *
 * All function pointers are optional except read. If a function is
 * not implemented, it should be set to NULL.
 */
typedef struct {
    /**
     * @brief Read data from the stream
     * @param ctx User context pointer
     * @param buf Buffer to read into
     * @param size Number of bytes to read
     * @return Number of bytes actually read, 0 on EOF or error
     */
    size_t (*read)(void *ctx, void *buf, size_t size);

    /**
     * @brief Seek to a position in the stream
     * @param ctx User context pointer
     * @param offset Offset in bytes
     * @param whence Seek origin (AUDIO_STREAM_SEEK_*)
     * @return 0 on success, -1 on error
     */
    int (*seek)(void *ctx, long offset, int whence);

    /**
     * @brief Get current position in the stream
     * @param ctx User context pointer
     * @return Current position, -1 on error
     */
    long (*tell)(void *ctx);

    /**
     * @brief Check if end of stream has been reached
     * @param ctx User context pointer
     * @return Non-zero if at EOF, 0 otherwise
     */
    int (*eof)(void *ctx);

    /**
     * @brief Close the stream and free resources
     * @param ctx User context pointer
     */
    void (*close)(void *ctx);
} audio_stream_io_ops_t;

/**
 * @brief Create a stream I/O handle with custom operations
 *
 * @param ops Stream operations table
 * @param ctx User context passed to operations
 * @return Stream handle, or NULL on failure
 */
audio_stream_io_handle_t audio_stream_io_create(const audio_stream_io_ops_t *ops, void *ctx);

/**
 * @brief Create a stream I/O handle from a FILE*
 *
 * The FILE* will be closed when the stream is closed.
 *
 * @param fp File pointer (must be opened for reading)
 * @return Stream handle, or NULL on failure
 */
audio_stream_io_handle_t audio_stream_io_from_file(FILE *fp);

/**
 * @brief Create a stream I/O handle from a FILE* without taking ownership
 *
 * The FILE* will NOT be closed when the stream is closed.
 *
 * @param fp File pointer (must be opened for reading)
 * @return Stream handle, or NULL on failure
 */
audio_stream_io_handle_t audio_stream_io_from_file_no_close(FILE *fp);

/**
 * @brief Create a stream I/O handle from a memory buffer
 *
 * @param buf Pointer to data buffer
 * @param size Size of data in bytes
 * @param copy If true, the buffer is copied; otherwise, the caller retains ownership
 * @return Stream handle, or NULL on failure
 */
audio_stream_io_handle_t audio_stream_io_from_memory(const void *buf, size_t size, bool copy);

/**
 * @brief Read data from the stream
 *
 * @param h Stream handle
 * @param buf Buffer to read into
 * @param size Number of bytes to read
 * @return Number of bytes actually read, 0 on EOF or error
 */
size_t audio_stream_io_read(audio_stream_io_handle_t h, void *buf, size_t size);

/**
 * @brief Seek to a position in the stream
 *
 * @param h Stream handle
 * @param offset Offset in bytes
 * @param whence Seek origin (AUDIO_STREAM_SEEK_*)
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if seek not implemented, other error on failure
 */
esp_err_t audio_stream_io_seek(audio_stream_io_handle_t h, long offset, int whence);

/**
 * @brief Get current position in the stream
 *
 * @param h Stream handle
 * @param pos Output: current position
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if tell not implemented
 */
esp_err_t audio_stream_io_tell(audio_stream_io_handle_t h, long *pos);

/**
 * @brief Check if end of stream has been reached
 *
 * @param h Stream handle
 * @return true if at EOF or error, false otherwise
 */
bool audio_stream_io_eof(audio_stream_io_handle_t h);

/**
 * @brief Close the stream and free all resources
 *
 * @param h Stream handle (can be NULL)
 */
void audio_stream_io_close(audio_stream_io_handle_t h);

/**
 * @brief Get the user context from a stream
 *
 * @param h Stream handle
 * @return User context pointer
 */
void* audio_stream_io_get_context(audio_stream_io_handle_t h);

#ifdef __cplusplus
}
#endif

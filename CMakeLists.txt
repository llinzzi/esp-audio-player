
set(srcs
    "audio_player.cpp"
    "audio_mixer.cpp"
    "audio_stream_io.cpp"
)

set(includes
    "include"
)

set(requires)
set(priv_requires)

if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
    list(APPEND requires esp_driver_i2s esp_ringbuf)
else()
    list(APPEND requires driver)
endif()

if(CONFIG_AUDIO_PLAYER_ENABLE_MP3)
    list(APPEND srcs "audio_mp3.cpp")
endif()

# TODO: move inside of the 'if(CONFIG_AUDIO_PLAYER_ENABLE_MP3)' when everything builds correctly
list(APPEND requires "esp-libhelix-mp3")

if(CONFIG_AUDIO_PLAYER_ENABLE_WAV)
    list(APPEND srcs "audio_wav.cpp")
endif()

if(CONFIG_AUDIO_PLAYER_ENABLE_HTTP_STREAM)
    list(APPEND srcs "audio_http_stream.cpp")
    list(APPEND requires esp_http_client)
endif()

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "${includes}"
                       REQUIRES "${requires}"
                       PRIV_REQUIRES "${priv_requires}"
)

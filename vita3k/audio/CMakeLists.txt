set(AUDIO_SOURCES
	src/audio.cpp
	src/impl/sdl_audio.cpp)

if(NOT VITA3K_BUILD_IOS_UPSTREAM_CORE)
	list(APPEND AUDIO_SOURCES src/impl/cubeb_audio.cpp)
endif()

add_library(audio STATIC ${AUDIO_SOURCES})

target_include_directories(audio PUBLIC include)
target_link_libraries(audio PRIVATE util SDL3::SDL3)

if(NOT VITA3K_BUILD_IOS_UPSTREAM_CORE)
	target_link_libraries(audio PRIVATE cubeb)
endif()

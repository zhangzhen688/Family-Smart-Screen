# GUI application — LVGL frontend
# Links: lvgl, cjson, SDL2, FreeType, pthread

find_package(SDL2 REQUIRED)
find_package(SDL2_image QUIET)

set(GUI_SRCS
    main.c
    rpc_client.c
    xiaozhi_ipc.c
    camera_reader.c
    ui/ui_main.c
    ui/ui_camera.c
    ui/ui_album.c
    ui/ui_device.c
    ui/ui_voice.c
    ui/ui_settings.c
    ui/ui_scenes.c
)

add_executable(smart_gui ${GUI_SRCS})

target_include_directories(smart_gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SDL2_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
)

set(GUI_LIBS lvgl cjson ${SDL2_LIBRARIES} ${FREETYPE_LIBRARIES} pthread m rt jpeg)
if(SDL2_image_FOUND)
    target_include_directories(smart_gui PRIVATE ${SDL2_IMAGE_INCLUDE_DIRS})
    list(APPEND GUI_LIBS ${SDL2_IMAGE_LIBRARIES})
endif()

target_link_libraries(smart_gui ${GUI_LIBS})

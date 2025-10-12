cmake_minimum_required(VERSION 3.5)

project(Player VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ======================================================
# Qt 模块：去掉 OpenGLWidgets / OpenGL 配置
# ======================================================
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Multimedia LinguistTools)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Multimedia LinguistTools)

set(TS_FILES Player_zh_CN.ts)

set(PROJECT_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    ${TS_FILES}
)

# ======================================================
# Qt 可执行文件
# ======================================================
if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(Player
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        resources.qrc
        pathsel.h pathsel.cpp
        videofile.h videofile.cpp
        videomanager.h videomanager.cpp
        videoplayer.h videoplayer.cpp
        fullscreentool.h
        Player.rc
        README.md
    )
    qt_create_translation(QM_FILES ${CMAKE_SOURCE_DIR} ${TS_FILES})
else()
    if(ANDROID)
        add_library(Player SHARED ${PROJECT_SOURCES})
    else()
        add_executable(Player ${PROJECT_SOURCES})
    endif()
    qt5_create_translation(QM_FILES ${CMAKE_SOURCE_DIR} ${TS_FILES})
endif()

# ======================================================
# 链接 Qt 模块（已移除 OpenGLWidgets / OpenGL）
# ======================================================
target_link_libraries(Player PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Multimedia
)

# ======================================================
# FFmpeg 配置
# ======================================================
set(FFMPEG_DIR "D:/ffmpeg")  # 修改为你自己的路径

target_include_directories(Player PRIVATE ${FFMPEG_DIR}/include)

if(MINGW)
    target_link_directories(Player PRIVATE ${FFMPEG_DIR}/lib)
    target_link_libraries(Player PRIVATE
        avcodec
        avformat
        avutil
        swscale
        swresample
    )
elseif(MSVC)
    target_link_directories(Player PRIVATE ${FFMPEG_DIR}/lib)
    target_link_libraries(Player PRIVATE
        avcodec.lib
        avformat.lib
        avutil.lib
        swscale.lib
        swresample.lib
    )
endif()

# ======================================================
# 平台设置
# ======================================================
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.Player)
endif()

set_target_properties(Player PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS Player
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(Player)
endif()

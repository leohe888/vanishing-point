cmake_minimum_required(VERSION 3.19)
project(VanisingPoint LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Test)

qt_standard_project_setup()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(VanishingCore STATIC
    projectivemapping.cpp projectivemapping.h
    planemath.cpp planemath.h
    canvasdocument.cpp canvasdocument.h
    floatingimagemath.cpp floatingimagemath.h
    imagegeometry.cpp imagegeometry.h
    paintengine.cpp paintengine.h
    clonestampengine.cpp clonestampengine.h
    clonetool.cpp clonetool.h
    imagetransformtool.cpp imagetransformtool.h
    planeedittool.cpp planeedittool.h
    scenerenderer.cpp scenerenderer.h
    scenecontentcache.cpp scenecontentcache.h
)
target_include_directories(VanishingCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(VanishingCore PUBLIC Qt::Core Qt::Gui)

add_library(VanishingWidgets STATIC
    mainwindow.cpp mainwindow.h
    perspectivecanvas.cpp perspectivecanvas.h
)
target_link_libraries(VanishingWidgets PUBLIC VanishingCore Qt::Widgets)

qt_add_executable(VanisingPoint WIN32 MACOSX_BUNDLE main.cpp)
target_link_libraries(VanisingPoint PRIVATE VanishingWidgets)

qt_add_resources(VanisingPoint app_resources
    PREFIX "/icons"
    FILES app.ico
)

install(TARGETS VanisingPoint
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET VanisingPoint
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

# —— 单元测试 ——
enable_testing()
add_subdirectory(tests)
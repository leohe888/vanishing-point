# 消失点（Vanishing Point）透视绘画工具 - CMake 构建脚本
cmake_minimum_required(VERSION 3.19)
project(VanisingPoint LANGUAGES CXX)

# 查找 Qt6（>= 6.5），需要 Core 与 Widgets 模块
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets)

qt_standard_project_setup()

# 核心逻辑只依赖 Core/Gui，界面层依赖 Widgets；应用和测试共享已编译库。
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
qt_add_resources(VanisingPoint app_resources
    PREFIX "/icons"
    FILES app.ico
)
if(WIN32)
    target_sources(VanisingPoint PRIVATE app.rc)
endif()
target_link_libraries(VanisingPoint PRIVATE VanishingWidgets)

option(VP_BUILD_TESTS "Build editor and architecture regression tests" OFF)
if(VP_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    qt_add_executable(CloneStampTests tests/clonestamptests.cpp)
    target_link_libraries(CloneStampTests PRIVATE VanishingWidgets Qt::Test)
    qt_add_executable(ArchitectureTests tests/architecturetests.cpp)
    target_link_libraries(ArchitectureTests PRIVATE VanishingWidgets Qt::Test)
    set_target_properties(CloneStampTests ArchitectureTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if(WIN32)
        get_filename_component(VP_COMPILER_DIRECTORY "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(VP_TEST_RUNTIME_PATH "$<TARGET_FILE_DIR:Qt6::Core>;${VP_COMPILER_DIRECTORY};$ENV{PATH}")
        add_test(NAME EditorRegressionTests COMMAND ${CMAKE_COMMAND} -E env
            "PATH=${VP_TEST_RUNTIME_PATH}" $<TARGET_FILE:CloneStampTests> -platform offscreen)
        add_test(NAME ArchitectureTests COMMAND ${CMAKE_COMMAND} -E env
            "PATH=${VP_TEST_RUNTIME_PATH}" $<TARGET_FILE:ArchitectureTests> -platform offscreen)
    else()
        add_test(NAME EditorRegressionTests COMMAND CloneStampTests -platform offscreen)
        add_test(NAME ArchitectureTests COMMAND ArchitectureTests -platform offscreen)
    endif()
    set_tests_properties(EditorRegressionTests ArchitectureTests PROPERTIES TIMEOUT 60)
    qt_add_executable(PerpendicularPlaneTests tests/perpendicularplanetests.cpp)
    target_link_libraries(PerpendicularPlaneTests PRIVATE VanishingCore Qt::Test)
    set_target_properties(PerpendicularPlaneTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if(WIN32)
        add_test(NAME PerpendicularPlaneTests COMMAND ${CMAKE_COMMAND} -E env
            "PATH=${VP_TEST_RUNTIME_PATH}" $<TARGET_FILE:PerpendicularPlaneTests>)
    else()
        add_test(NAME PerpendicularPlaneTests COMMAND PerpendicularPlaneTests)
    endif()
    set_tests_properties(PerpendicularPlaneTests PROPERTIES TIMEOUT 60)
endif()

# 安装规则：Bundle 安装到根目录，其余平台按标准目录安装
install(TARGETS VanisingPoint
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# 生成各平台的部署脚本（打包所需的 Qt 运行时依赖）
qt_generate_deploy_app_script(
    TARGET VanisingPoint
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

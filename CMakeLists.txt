# 消失点（Vanishing Point）透视绘画工具 - CMake 构建脚本
cmake_minimum_required(VERSION 3.19)
project(VanisingPoint LANGUAGES CXX)

# 查找 Qt6（>= 6.5），需要 Core 与 Widgets 模块
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets)

qt_standard_project_setup()

# 可执行目标：Windows 下为无控制台窗口的 GUI 程序，macOS 下为应用 Bundle
# 模块划分：几何算法(planemath)、文档模型(canvasdocument)、笔刷引擎(paintengine)、
# 场景渲染(scenerenderer)、交互控件(perspectivecanvas)
qt_add_executable(VanisingPoint
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    planemath.cpp
    planemath.h
    canvasdocument.cpp
    canvasdocument.h
    paintengine.cpp
    paintengine.h
    scenerenderer.cpp
    scenerenderer.h
    perspectivecanvas.cpp
    perspectivecanvas.h
)

# 链接 Qt 核心与控件库
target_link_libraries(VanisingPoint
    PRIVATE
        Qt::Core
        Qt::Widgets
)

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

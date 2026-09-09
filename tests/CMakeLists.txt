# 测试目标：每个 .cpp 编译为一个独立可执行，链接 VanishingCore（纯逻辑）与 Qt::Test。
# 纯逻辑测试用 QTEST_APPLESS_MAIN，不依赖 GUI，可在命令行 / CI 直接运行。

qt_add_executable(tst_projectivemapping tst_projectivemapping.cpp)
target_link_libraries(tst_projectivemapping PRIVATE VanishingCore Qt::Test)

qt_add_executable(tst_planemath tst_planemath.cpp)
target_link_libraries(tst_planemath PRIVATE VanishingCore Qt::Test)

qt_add_executable(tst_canvasdocument tst_canvasdocument.cpp)
target_link_libraries(tst_canvasdocument PRIVATE VanishingCore Qt::Test)

add_test(NAME projectivemapping COMMAND tst_projectivemapping)
add_test(NAME planemath COMMAND tst_planemath)
add_test(NAME canvasdocument COMMAND tst_canvasdocument)

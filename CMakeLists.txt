cmake_minimum_required(VERSION 3.19)
project(AOSimulator LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network LinguistTools Test)

qt_standard_project_setup()

qt_add_executable(AOSimulator
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    simulator.cpp
    simulator.h
)

qt_add_translations(
    TARGETS AOSimulator
    TS_FILES AOSimulator_zh_CN.ts
)

target_link_libraries(AOSimulator
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Network
)

# ---------- 单元测试（Qt Test，走真实 TCP 环回） ----------
# 测试目标是控制台程序（不带 WIN32），ctest 能正常捕获输出。
enable_testing()
qt_add_executable(aosimulator_tests
    tests/test_simulator.cpp
    simulator.cpp
    simulator.h
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
)
target_link_libraries(aosimulator_tests
    PRIVATE
        Qt6::Core
        Qt6::Widgets
        Qt6::Network
        Qt6::Test
)
target_include_directories(aosimulator_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME aosimulator_tests COMMAND aosimulator_tests)

# ---------- Windows：每次构建后把 Qt 运行库拷到 exe 旁 ----------
# 为什么必须这么做（真实事故）：本机系统 PATH 里有 "C:\Program Files\010 Editor"，
# 该软件自带一份 Qt 6.4.3 的 Qt6Core.dll，且系统 PATH 恒排在用户 PATH 之前。
# exe 旁边没有 DLL 时，Windows 沿 PATH 先命中 6.4.3，加载时报
#   "无法定位程序输入点 _Z11qt_assert_xPKcS0_S0_i 于动态链接库 AOSimulator.exe 上"
# （6.4.3 与 6.8.3 的导出符号不同）。exe 所在目录的搜索优先级最高，
# 把 DLL 部署到 exe 旁就能彻底摆脱这种"别的软件抢 DLL"的问题，也方便整目录拷走。
if(WIN32)
    get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}" NO_DEFAULT_PATH)
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET AOSimulator POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                    --no-translations          # 本工程不依赖 .qm
                    --no-opengl-sw             # 用系统 OpenGL，不拷软件渲染器
                    --no-system-d3d-compiler
                    "$<TARGET_FILE:AOSimulator>"
            COMMENT "windeployqt: 把 Qt 运行库部署到 exe 旁（防止 PATH 上其他软件的 Qt 抢占）"
            VERBATIM)
    else()
        message(WARNING "未找到 windeployqt，exe 运行时将依赖 PATH 上的 Qt DLL")
    endif()

    # 部署会往 exe 旁写 platforms\qwindows.dll，而 app 目录的 platforms\ 搜索优先级最高：
    # ctest 用 offscreen 平台跑测试时就找不到 qoffscreen.dll，直接 fail-fast（0xc0000502）。
    # 这里把插件搜索路径补上完整目录，测试仍可用 offscreen，也不影响双击运行的 exe。
    if(WINDEPLOYQT_EXECUTABLE)
        get_filename_component(_qt_plugin_dir "${_qt_bin_dir}/../plugins" REALPATH)
        set_tests_properties(aosimulator_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_qt_plugin_dir}")
    endif()
endif()

# ---------- 覆盖率（与 AOMonitor 同一套路，见其 QTest执行手顺.md） ----------
option(ENABLE_COVERAGE "Enable gcov coverage instrumentation for tests" OFF)
if(ENABLE_COVERAGE)
    target_compile_options(aosimulator_tests PRIVATE --coverage -fno-inline -fno-inline-small-functions)
    target_link_options(aosimulator_tests PRIVATE --coverage)
endif()

install(TARGETS AOSimulator
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET AOSimulator
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

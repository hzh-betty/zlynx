# 每个模块独立 install/find_package 的公共封装。
#
# 设计目标：
# - 每个模块生成自己的 <module>Config.cmake 和 <module>Targets.cmake；
# - 安装后通过 find_package(zlog CONFIG REQUIRED) + zlog::zlog 使用；
# - 依赖使用 find_dependency() 转发，避免消费者手动重复查找；
# - headers/library/export/version 文件统一安装到 GNUInstallDirs 标准路径。

include_guard(GLOBAL)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

function(zlynx_install_module module_name target_name)
    set(options)
    set(one_value_args VERSION)
    set(multi_value_args DEPENDENCIES)
    cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ARG_VERSION)
        set(ARG_VERSION ${PROJECT_VERSION})
    endif()

    # 包配置文件安装到 lib/cmake/<module>，这是 CMake CONFIG 包的常见布局。
    set(package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/${module_name}")
    set(config_build_dir "${CMAKE_CURRENT_BINARY_DIR}/${module_name}-package")
    set(config_file "${config_build_dir}/${module_name}Config.cmake")
    set(version_file "${config_build_dir}/${module_name}ConfigVersion.cmake")
    set(dependencies "${ARG_DEPENDENCIES}")

    install(TARGETS ${target_name}
        EXPORT ${module_name}Targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )

    # EXPORT_NAME 由各模块目标属性控制，导出后形成 <module>::<target>。
    install(EXPORT ${module_name}Targets
        NAMESPACE ${module_name}::
        DESTINATION "${package_dir}"
    )

    # configure_package_config_file 会生成可重定位的包配置，不把构建机路径写死。
    configure_package_config_file(
        "${ZLYNX_CMAKE_DIR}/templates/ModuleConfig.cmake.in"
        "${config_file}"
        INSTALL_DESTINATION "${package_dir}"
    )

    # SameMajorVersion：同一主版本内允许兼容查找，避免 patch/minor 升级破坏消费方。
    write_basic_package_version_file(
        "${version_file}"
        VERSION "${ARG_VERSION}"
        COMPATIBILITY SameMajorVersion
    )

    install(FILES "${config_file}" "${version_file}"
        DESTINATION "${package_dir}"
    )
endfunction()

function(zlynx_install_extra_target module_name target_name)
    # 额外目标用于 zmalloc_override 这类可选产物：进入同一 export set，
    # 但不复制头文件，也不单独生成包配置。
    install(TARGETS ${target_name}
        EXPORT ${module_name}Targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endfunction()

# KNRL: deterministic simulation engine + OVERKNRL virtual-system layer.
# Shell (OVERSHELL) depends on this library; never the reverse.
add_library(knrl STATIC
    src/account.cpp
    src/achievement.cpp
    src/event.cpp
    src/node.cpp
    src/clock.cpp
    src/network.cpp
    src/history.cpp
    src/system.cpp
    src/system_procs.cpp
    src/system_faults.cpp
    src/system_net.cpp
    src/system_observe.cpp
    src/system_time.cpp
    src/system_services.cpp
    src/system_vfs.cpp
    src/process.cpp
    src/host.cpp
    src/fault.cpp
    src/vfs.cpp
    src/service.cpp
    src/hardware.cpp
    src/system_hardware.cpp
    src/system_serialize.cpp
    src/system_recovery.cpp
    src/system_packages.cpp
    src/worlds.cpp
    src/packages.cpp
    src/progress.cpp
)
target_include_directories(knrl PUBLIC include)
target_link_libraries(knrl PUBLIC ordc)
if(MSVC)
    target_compile_options(knrl PRIVATE /W4 /permissive-)
else()
    target_compile_options(knrl PRIVATE -Wall -Wextra -Wpedantic)
endif()

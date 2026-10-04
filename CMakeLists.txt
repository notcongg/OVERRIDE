cmake_minimum_required(VERSION 3.20)
project(OVERRIDE VERSION 0.1.0 LANGUAGES CXX)

# C++17 only: the codebase must not require newer standards (verified by CI
# builds; see the stdlib-only dependency rule in README).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Layout: OVERSHELL/ (shell frontend) + KNRL/ (simulation engine + OVERKNRL)
# + ORDC/ (deterministic .ord persistence documents).
# Only root metadata files live here; all code/data is under those trees.
enable_testing()
add_subdirectory(ORDC)
add_subdirectory(KNRL)
add_subdirectory(OVERSHELL)

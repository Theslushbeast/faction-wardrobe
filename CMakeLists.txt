cmake_minimum_required(VERSION 3.25)

project(FactionWardrobe VERSION 0.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# CommonLibSSE-NG: use the published prebuilt library when it matches this compiler, otherwise build it once from source.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(COMMONLIB_PREBUILT ON CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)

find_package(nlohmann_json CONFIG REQUIRED)

add_commonlibsse_plugin(
	FactionWardrobe
	NAME "FactionWardrobe"
	AUTHOR "Clyde"
	VERSION ${PROJECT_VERSION}
	SOURCES src/main.cpp
)

target_compile_features(FactionWardrobe PRIVATE cxx_std_23)
target_include_directories(FactionWardrobe PRIVATE src)
target_link_libraries(FactionWardrobe PRIVATE nlohmann_json::nlohmann_json)
target_precompile_headers(FactionWardrobe PRIVATE src/PCH.h)
if(MSVC)
	target_compile_options(FactionWardrobe PRIVATE /utf-8 /permissive- /W3)
endif()

# Rule-selection tests (no game needed): ctest -C Release --test-dir build
option(FACTIONWARDROBE_TESTS "Build the rule-selection tests" ON)
if(FACTIONWARDROBE_TESTS)
	enable_testing()
	add_executable(FactionWardrobeRulesTests tests/tests.cpp)
	target_include_directories(FactionWardrobeRulesTests PRIVATE src)
	add_test(NAME FactionWardrobeRules COMMAND FactionWardrobeRulesTests)
endif()

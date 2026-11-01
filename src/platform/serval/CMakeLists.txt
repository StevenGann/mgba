# serval-emu: the debugger's emulator for Serval Engine games (SERVAL.md).
if(APPLE)
	# mGBA's top level sets 10.6; SDL 2 needs 10.7, and Apple Silicon 11.0.
	set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0")
endif()
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
	find_package(SDL2 REQUIRED)
endif()

add_executable(serval-emu main.c link.c)
if(TARGET SDL2::SDL2)
	target_link_libraries(serval-emu SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static) # a static SDL2 (the release builds')
	target_link_libraries(serval-emu SDL2::SDL2-static)
else()
	target_include_directories(serval-emu PRIVATE ${SDL2_INCLUDE_DIRS})
	target_link_libraries(serval-emu ${SDL2_LIBRARIES})
endif()
target_link_libraries(serval-emu ${PLATFORM_LIBRARY} ${BINARY_NAME})
if(WIN32)
	target_link_libraries(serval-emu ws2_32)
endif()
# A console program on every platform (SDL_MAIN_HANDLED: no SDL2main), so
# Studio Advance reads its output.
target_compile_definitions(serval-emu PRIVATE "${OS_DEFINES};${FEATURE_DEFINES};${FUNCTION_DEFINES}" SDL_MAIN_HANDLED)
install(TARGETS serval-emu DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT serval-emu)

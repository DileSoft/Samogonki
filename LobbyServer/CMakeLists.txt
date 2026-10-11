# The lobby server is a plain console program, not the game: no SDL, no
# renderer, no game objects. It shares only the socket layer and the wire
# protocol header with the game, which is why net_socket.cpp is listed here
# rather than linked from the game target.

add_executable(moonshine-lobby
    main.cpp
    lobby_server.cpp
    ../MechoSoma/RUNTIME/net_socket.cpp)

target_include_directories(moonshine-lobby PRIVATE
    .
    ../MechoSoma/RUNTIME)

if (WIN32)
    target_link_libraries(moonshine-lobby PRIVATE ws2_32)
endif()

# Same standalone treatment as the game on MinGW: no libstdc++/libgcc DLLs to
# ship beside it, only the winpthread the standard thread support needs.
if (MINGW)
    target_link_options(moonshine-lobby PRIVATE -static-libgcc -static-libstdc++)
    get_filename_component(TOOLCHAIN_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET moonshine-lobby POST_BUILD
            COMMAND cp -v "${TOOLCHAIN_BIN_DIR}/libwinpthread-1.dll" "${CMAKE_CURRENT_BINARY_DIR}")
endif()

set_target_properties(moonshine-lobby PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO)

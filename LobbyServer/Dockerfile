# Lobby server image.
#
# It builds only the lobby server, not the game: three translation units and
# two headers, no SDL, no renderer, no 3rdparty. The same files the CMake
# target moonshine-lobby lists, compiled straight with g++ so the image does
# not drag the game's CMake tree in.
#
# Build from the repository root (the context needs LobbyServer/ and
# MechoSoma/RUNTIME/):
#
#   docker build -f LobbyServer/Dockerfile -t moonshine-lobby .
#
# See LobbyServer/README.md.

# ------------------------------- build stage -------------------------------
FROM debian:bookworm-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Only what net_socket.cpp and the protocol header are; the rest of the game
# is deliberately absent.
COPY LobbyServer/main.cpp LobbyServer/lobby_server.cpp LobbyServer/lobby_server.h LobbyServer/
COPY MechoSoma/RUNTIME/net_socket.cpp \
     MechoSoma/RUNTIME/net_socket.h \
     MechoSoma/RUNTIME/net_lobby_protocol.h \
     MechoSoma/RUNTIME/

# -pthread: std::thread is used for the console loop's sleep. The static
# libstdc++/libgcc keeps the runtime image from needing a compiler-era
# toolchain - only libc and libpthread remain, and both come with the base.
RUN g++ -std=c++17 -O2 -Wall -pthread -static-libgcc -static-libstdc++ \
        -ILobbyServer -IMechoSoma/RUNTIME \
        LobbyServer/main.cpp LobbyServer/lobby_server.cpp MechoSoma/RUNTIME/net_socket.cpp \
        -o /out/moonshine-lobby

# ------------------------------ runtime stage ------------------------------
FROM debian:bookworm-slim

# Not root: the server listens on an unprivileged port and touches nothing.
RUN useradd --system --uid 10001 --no-create-home lobby

COPY --from=build /out/moonshine-lobby /usr/local/bin/moonshine-lobby

# The control channel is TCP and the rendezvous channel is UDP, on the same
# number. Publishing only one of them is the classic way to make this look
# broken: see the README.
EXPOSE 8510/tcp
EXPOSE 8510/udp

USER lobby

ENTRYPOINT ["moonshine-lobby"]
CMD ["--port", "8510"]

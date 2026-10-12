#ifdef GPX
#include <c/gamepix.h>
#else
#define SDL_MAIN_HANDLED
#endif
#include <SDL2/SDL.h>
#include "filesystem.h"

#include <cstdio>

#ifdef EMSCRIPTEN
#include <emscripten.h>
#define SOKOL_GLES3
#else
#define SOKOL_GLCORE33
#endif

#ifdef STEAM_VERSION
#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include <steam/steam_api.h>
#endif

#define SOKOL_GFX_IMPL
#define SOKOL_GLCORE
#include <sokol_gfx.h>

#include "xtool.h"
#include "mechosoma.h"

int xtSysQuantDisabled = 0;

// Implemented in RUNTIME/net_selftest.cpp
int net_selftest(void);

namespace {

    int run_net_selftest() {
        const int result = net_selftest();
        if (result) {
            // OutputDebugString is Windows-only; the other platforms write the
            // same message to stderr so CI logs still show the failure.
#ifdef _WIN32
            OutputDebugStringA("moonshine: network transport self test FAILED, see nettest.log\n");
#else
            fprintf(stderr, "moonshine: network transport self test FAILED, see nettest.log\n");
#endif
        }
        return result;
    }

}

namespace {

    struct LoopState {
        int prevId;
        int id;
        bool xObjInit;
        XRuntimeObject *XObj;
        int clockCnt;
    };

    LoopState state;

    void loop() {
        if (state.xObjInit) {
            state.XObj->Init(state.prevId);
            state.prevId = state.id;
            state.id = 0;
            state.xObjInit = false;
            state.clockCnt = clocki();
        } else if (!state.id) {
            if (state.XObj->Timer) {
                int clockNow = clocki();
                int clockDelta = clockNow - state.clockCnt;
                if (clockDelta >= state.XObj->Timer) {
                    state.clockCnt = clockNow - (clockDelta - state.XObj->Timer) % state.XObj->Timer;
                    state.id = state.XObj->Quant();
                }
            } else {
                state.id = state.XObj->Quant();
            }

            if (!xtSysQuantDisabled) {
                XRec.Quant();
            }

            if (xtNeedExit()) {
                ErrH.Exit();
            }
        } else {
            state.XObj->Finit();
            state.XObj = xtGetRuntimeObject(state.id);
            state.xObjInit = true;
#ifdef EMSCRIPTEN
            EM_ASM((
                if (Module.layers) {
                    Module.layers.gameQuantId($0);
                }
            ), state.id);
#endif
        }

#ifdef GPX
        gpx()->async()->runNextTask();
#endif
    };
}

#ifdef GPX

int game_main(int argc, char const *argv[]) {
  localization::setLanguage(gpx()->sys()->getLanguage().c_str());
#else
int main(int argc, char const *argv[]) {
  for (int i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "-nettest") == 0) {
          return run_net_selftest();
      }
      // -nethost runs this copy as the session host, -netjoin <address> points
      // it at one. The race itself still starts from the menus: only the mode
      // is armed here, so a race started by hand on both machines arrives at
      // the same branch. Skipping the menus would need a lobby screen to wait
      // in, which the menu phase (6) adds.
      if (strcmp(argv[i], "-nethost") == 0) {
          mchNetGame = 1;
          mchNetHost = 1;
      }
      if (strcmp(argv[i], "-netjoin") == 0 && i + 1 < argc) {
          mchNetGame = 1;
          mchNetHost = 0;
          strncpy(mchNetJoinAddr, argv[i + 1], sizeof(mchNetJoinAddr) - 1);
          mchNetJoinAddr[sizeof(mchNetJoinAddr) - 1] = '\0';
      }
      // -netrace is the entry point for a run with nobody at the keyboard,
      // which is what a test is: the menus need a human, a race does not.
      // Use it with -nethost on one machine and -netjoin on the other.
      if (strcmp(argv[i], "-netrace") == 0) {
          mchNetGame = 1;
          mchNetRaceAuto = 1;
      }
      // -netdrive makes the client steer itself. Test scaffolding: it exercises
      // the client-sends / host-applies input path with no keyboard involved.
      if (strcmp(argv[i], "-netdrive") == 0) {
          mchNetAutoDrive = 1;
      }
      // -netdump writes a per-tick CSV of all networked objects (racers and
      // replicated bodies) so a host trace and a client trace can be compared.
      if (strcmp(argv[i], "-netdump") == 0) {
          mchNetTrace = 1;
      }
  }
  #ifdef STEAM_VERSION
  if (!SteamAPI_Init()) {
    return 1;
  }

  using LanguagePair = std::pair<std::string_view, std::string_view>;
  constexpr std::array<LanguagePair, 6> map{
    LanguagePair("czech", "cz"),
    LanguagePair("english", "en"),
    LanguagePair("french", "fr"),
    LanguagePair("italian", "it"),
    LanguagePair("lithuanian", "lt"),
    LanguagePair("russian", "ru")
  };

  auto language = SteamApps()->GetCurrentGameLanguage();
  const auto p = std::find_if(map.begin(), map.end(), [&language](const auto &e) {
    return e.first == language;
  });
  if (p == map.end()) {
    return 1;
  }

  localization::setLanguage(p->second.data());
  #else
  for (int i = 1; i < argc - 1; ++i) {
      if (strcmp(argv[i], "-lang") == 0 && strlen(argv[i + 1]) == 2) {
          localization::setLanguage(argv[i + 1]);
          break;
      }
  }
  #endif
#endif
  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK);

  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

#ifdef EMSCRIPTEN
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_SetHint("SDL_EMSCRIPTEN_ASYNCIFY", "0");
  SDL_SetHint("SDL_HINT_TOUCH_MOUSE_EVENTS", "1");
  SDL_SetHint("SDL_HINT_MOUSE_TOUCH_EVENTS", "0");
#else
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif

  initclock();

  state.prevId = 0;
  state.id = xtInitApplication();
  state.xObjInit = true;
  state.XObj = xtGetRuntimeObject(state.id);

#ifdef EMSCRIPTEN
  emscripten_set_main_loop(loop, 0, true);
#else
    while (state.XObj) {
        loop();
    }
#endif

  xtDoneApplication();

  SDL_Quit();

  return 0;
}

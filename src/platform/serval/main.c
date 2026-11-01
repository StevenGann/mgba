/* Copyright (c) 2026 Steven Gann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// serval-emu: mGBA's GBA core in a small SDL2 window, with the endpoint of
// Serval Engine's debug link (link.c). Studio Advance starts it as a process
// of its own:
//
//   serval-emu --link 127.0.0.1:PORT --link-token TOKEN game.gba
//
// It runs one GBA game. The emulation runs on this thread, one frame at a
// time, paced to the GBA's 59.73 frames a second; the link is serviced
// between the core's run loops, and a parked agent stops the machine at once
// (the doorbell's ring), with the window still responsive.

#include "link.h"

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/version.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/audio-buffer.h>

#include <SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVAL_EMU_VERSION "0.1.0"

#define GBA_FPS (16777216.0 / 280896.0) // 59.7275
#define AUDIO_RATE 48000
#define AUDIO_MAX_QUEUED_SECONDS 0.12

struct Options {
	const char* rom;
	const char* link;
	const char* token;
	int scale;
	bool headless;
	bool unpaced;
	long frames; // 0: until the window closes
	const char* keys; // scripted buttons, FRAME:KEYS:LENGTH,... (Serval's web test mode's)
};

// Scripted buttons (--keys), as Serval Engine's web pages take them in their
// test mode: from frame FRAME, for LENGTH frames (1), the buttons KEYS, one
// letter each: A, B, s (Select), S (Start), R, L, U, D, r (right), l (left).
struct ScriptedKeys {
	long from;
	long to;
	uint32_t mask;
};

static struct ScriptedKeys* _scripted;
static size_t _scriptedCount;

static bool _parseKeys(const char* text) {
	size_t count = 1;
	for (const char* c = text; *c; ++c) {
		count += *c == ',';
	}
	_scripted = calloc(count, sizeof(*_scripted));
	_scriptedCount = 0;
	const char* p = text;
	while (*p) {
		char* end;
		long from = strtol(p, &end, 10);
		if (end == p || *end != ':') {
			return false;
		}
		p = end + 1;
		uint32_t mask = 0;
		for (; *p && *p != ':' && *p != ','; ++p) {
			switch (*p) {
			case 'A': mask |= 1 << GBA_KEY_A; break;
			case 'B': mask |= 1 << GBA_KEY_B; break;
			case 's': mask |= 1 << GBA_KEY_SELECT; break;
			case 'S': mask |= 1 << GBA_KEY_START; break;
			case 'R': mask |= 1 << GBA_KEY_R; break;
			case 'L': mask |= 1 << GBA_KEY_L; break;
			case 'U': mask |= 1 << GBA_KEY_UP; break;
			case 'D': mask |= 1 << GBA_KEY_DOWN; break;
			case 'r': mask |= 1 << GBA_KEY_RIGHT; break;
			case 'l': mask |= 1 << GBA_KEY_LEFT; break;
			default: return false;
			}
		}
		long length = 1;
		if (*p == ':') {
			length = strtol(p + 1, &end, 10);
			p = end;
		}
		_scripted[_scriptedCount++] = (struct ScriptedKeys) { from, from + length, mask };
		if (*p == ',') {
			++p;
		} else if (*p) {
			return false;
		}
	}
	return true;
}

static uint32_t _scriptedAt(long frame) {
	uint32_t mask = 0;
	for (size_t i = 0; i < _scriptedCount; ++i) {
		if (frame >= _scripted[i].from && frame < _scripted[i].to) {
			mask |= _scripted[i].mask;
		}
	}
	return mask;
}

static struct ServalLink _link;
static bool _linkReady = false;

// --- Logs ---------------------------------------------------------------------

static int _linkLevel(enum mLogLevel level) {
	switch (level) {
	case mLOG_FATAL:
	case mLOG_ERROR:
		return SERVAL_LINK_LOG_ERROR;
	case mLOG_WARN:
		return SERVAL_LINK_LOG_WARNING;
	default:
		return SERVAL_LINK_LOG_INFO;
	}
}

// mGBA's log: the game's debug_log() lines (mGBA's debug registers, every
// build) and the emulator's warnings and errors. Printed, and sent as LOG.
static void _log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	bool game = category == _mLOG_CAT_GBA_DEBUG;
	if (!game && !(level & (mLOG_FATAL | mLOG_ERROR | mLOG_WARN))) {
		return;
	}
	char text[512];
	vsnprintf(text, sizeof(text), format, args);
	int source = SERVAL_LINK_SOURCE_EMULATOR;
	if (game) {
		source = strncmp(text, "serval:", 7) == 0 ? SERVAL_LINK_SOURCE_ENGINE : SERVAL_LINK_SOURCE_GAME;
	}
	const char* levelName = level & (mLOG_FATAL | mLOG_ERROR) ? "error" : level & mLOG_WARN ? "warning" : "info";
	if (game) {
		printf("[%s] %s\n", levelName, text);
	} else {
		printf("[%s] %s: %s\n", levelName, mLogCategoryName(category), text);
	}
	fflush(stdout);
	if (_linkReady) {
		ServalLinkLog(&_link, _linkLevel(level), source, text);
	}
}

static struct mLogger _logger = { .log = _log };

// --- Options --------------------------------------------------------------------

static void _usage(const char* program) {
	printf("usage: %s [options] game.gba\n"
	       "\n"
	       "  --link HOST:PORT     dial the debugger (Studio Advance) at HOST:PORT\n"
	       "  --link-token TOKEN   the debug session's token, sent in HELLO\n"
	       "  --scale N            the window's size, N times the GBA's screen (default 3)\n"
	       "  --headless           no window and no sound (for tests)\n"
	       "  --unpaced            run as fast as possible, not at the GBA's speed\n"
	       "  --frames N           quit after N frames\n"
	       "  --keys K             scripted buttons, FRAME:KEYS:LENGTH,... (A B s S R L U D r l)\n"
	       "  --version            print the version and quit\n"
	       "\n"
	       "Keys: arrows, X = A, Z = B, A = L, S = R, Enter = Start, Backspace = Select.\n",
	       program);
}

static bool _parse(int argc, char** argv, struct Options* options) {
	*options = (struct Options) { .scale = 3 };
	for (int i = 1; i < argc; ++i) {
		const char* arg = argv[i];
		bool hasValue = i + 1 < argc;
		if (!strcmp(arg, "--link") && hasValue) {
			options->link = argv[++i];
		} else if (!strcmp(arg, "--link-token") && hasValue) {
			options->token = argv[++i];
		} else if (!strcmp(arg, "--scale") && hasValue) {
			options->scale = atoi(argv[++i]);
			if (options->scale < 1 || options->scale > 10) {
				options->scale = 3;
			}
		} else if (!strcmp(arg, "--frames") && hasValue) {
			options->frames = atol(argv[++i]);
		} else if (!strcmp(arg, "--keys") && hasValue) {
			options->keys = argv[++i];
			if (!_parseKeys(options->keys)) {
				fprintf(stderr, "serval-emu: --keys wants FRAME:KEYS:LENGTH,..., not %s\n", options->keys);
				return false;
			}
		} else if (!strcmp(arg, "--headless")) {
			options->headless = true;
		} else if (!strcmp(arg, "--unpaced")) {
			options->unpaced = true;
		} else if (!strcmp(arg, "--version")) {
			printf("serval-emu %s (mGBA %s)\n", SERVAL_EMU_VERSION, projectVersion);
			exit(0);
		} else if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
			_usage(argv[0]);
			exit(0);
		} else if (arg[0] == '-' && arg[1]) {
			fprintf(stderr, "serval-emu: unknown option %s\n", arg);
			return false;
		} else if (!options->rom) {
			options->rom = arg;
		} else {
			fprintf(stderr, "serval-emu: one game at a time\n");
			return false;
		}
	}
	if (!options->rom) {
		fprintf(stderr, "serval-emu: no game given\n");
		return false;
	}
	return true;
}

// --- Input ----------------------------------------------------------------------

static uint32_t _keyBit(SDL_Keycode key) {
	switch (key) {
	case SDLK_x:
		return 1 << GBA_KEY_A;
	case SDLK_z:
		return 1 << GBA_KEY_B;
	case SDLK_BACKSPACE:
		return 1 << GBA_KEY_SELECT;
	case SDLK_RETURN:
		return 1 << GBA_KEY_START;
	case SDLK_RIGHT:
		return 1 << GBA_KEY_RIGHT;
	case SDLK_LEFT:
		return 1 << GBA_KEY_LEFT;
	case SDLK_UP:
		return 1 << GBA_KEY_UP;
	case SDLK_DOWN:
		return 1 << GBA_KEY_DOWN;
	case SDLK_s:
		return 1 << GBA_KEY_R;
	case SDLK_a:
		return 1 << GBA_KEY_L;
	default:
		return 0;
	}
}

static uint32_t _buttonBit(int button) {
	switch (button) {
	case SDL_CONTROLLER_BUTTON_B: // east: the GBA's A, on the right
		return 1 << GBA_KEY_A;
	case SDL_CONTROLLER_BUTTON_A: // south
		return 1 << GBA_KEY_B;
	case SDL_CONTROLLER_BUTTON_BACK:
		return 1 << GBA_KEY_SELECT;
	case SDL_CONTROLLER_BUTTON_START:
		return 1 << GBA_KEY_START;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
		return 1 << GBA_KEY_RIGHT;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
		return 1 << GBA_KEY_LEFT;
	case SDL_CONTROLLER_BUTTON_DPAD_UP:
		return 1 << GBA_KEY_UP;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
		return 1 << GBA_KEY_DOWN;
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
		return 1 << GBA_KEY_R;
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
		return 1 << GBA_KEY_L;
	default:
		return 0;
	}
}

// --- The window -----------------------------------------------------------------

struct Window {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* texture;
	SDL_AudioDeviceID audio;
	SDL_AudioStream* stream;
	char title[96];
	bool showingPaused;
};

static bool _openWindow(struct Window* w, const struct Options* options, struct mCore* core, unsigned width, unsigned height) {
	SDL_SetMainReady();
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
		fprintf(stderr, "serval-emu: SDL: %s\n", SDL_GetError());
		return false;
	}
	struct mGameInfo info;
	core->getGameInfo(core, &info);
	snprintf(w->title, sizeof(w->title), "%s - serval-emu", info.title[0] ? info.title : "GBA");
	w->window = SDL_CreateWindow(w->title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int) width * options->scale, (int) height * options->scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!w->window) {
		fprintf(stderr, "serval-emu: SDL: %s\n", SDL_GetError());
		return false;
	}
	w->renderer = SDL_CreateRenderer(w->window, -1, 0);
	if (!w->renderer) {
		fprintf(stderr, "serval-emu: SDL: %s\n", SDL_GetError());
		return false;
	}
	SDL_RenderSetLogicalSize(w->renderer, (int) width, (int) height);
	SDL_RenderSetIntegerScale(w->renderer, SDL_TRUE);
	w->texture = SDL_CreateTexture(w->renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, (int) width, (int) height);

	SDL_AudioSpec want = { 0 }, have;
	want.freq = AUDIO_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	w->audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (w->audio) {
		w->stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, (int) core->audioSampleRate(core), have.format, have.channels, have.freq);
		SDL_PauseAudioDevice(w->audio, 0);
	} else {
		fprintf(stderr, "serval-emu: no sound: %s\n", SDL_GetError());
	}
	for (int i = 0; i < SDL_NumJoysticks(); ++i) {
		if (SDL_IsGameController(i)) {
			SDL_GameControllerOpen(i);
		}
	}
	return true;
}

static void _present(struct Window* w, const mColor* pixels, unsigned width, bool paused) {
	if (!w->window) {
		return;
	}
	SDL_UpdateTexture(w->texture, NULL, pixels, (int) (width * sizeof(mColor)));
	SDL_RenderClear(w->renderer);
	SDL_RenderCopy(w->renderer, w->texture, NULL, NULL);
	SDL_RenderPresent(w->renderer);
	if (paused != w->showingPaused) {
		char title[128];
		snprintf(title, sizeof(title), paused ? "%s (paused by the debugger)" : "%s", w->title);
		SDL_SetWindowTitle(w->window, title);
		w->showingPaused = paused;
	}
}

static void _playAudio(struct Window* w, struct mCore* core) {
	struct mAudioBuffer* buffer = core->getAudioBuffer(core);
	int16_t samples[4096 * 2];
	size_t available = mAudioBufferAvailable(buffer);
	while (available) {
		size_t count = available < 4096 ? available : 4096;
		count = mAudioBufferRead(buffer, samples, count);
		if (!count) {
			break;
		}
		if (w->stream) {
			SDL_AudioStreamPut(w->stream, samples, (int) (count * 2 * sizeof(int16_t)));
		}
		available -= count;
	}
	if (!w->stream) {
		return;
	}
	uint8_t converted[16384];
	int got;
	double maxQueued = AUDIO_MAX_QUEUED_SECONDS * AUDIO_RATE * 4;
	while ((got = SDL_AudioStreamGet(w->stream, converted, sizeof(converted))) > 0) {
		if (SDL_GetQueuedAudioSize(w->audio) < maxQueued) { // running behind: drop rather than lag
			SDL_QueueAudio(w->audio, converted, (Uint32) got);
		}
	}
}

static void _closeWindow(struct Window* w) {
	if (w->stream) {
		SDL_FreeAudioStream(w->stream);
	}
	if (w->audio) {
		SDL_CloseAudioDevice(w->audio);
	}
	if (w->texture) {
		SDL_DestroyTexture(w->texture);
	}
	if (w->renderer) {
		SDL_DestroyRenderer(w->renderer);
	}
	if (w->window) {
		SDL_DestroyWindow(w->window);
	}
	SDL_Quit();
}

// --- Main -------------------------------------------------------------------------

int main(int argc, char** argv) {
	struct Options options;
	if (!_parse(argc, argv, &options)) {
		_usage(argv[0]);
		return 2;
	}
	mLogSetDefaultLogger(&_logger);

	struct mCore* core = mCoreFind(options.rom);
	if (!core || core->platform(core) != mPLATFORM_GBA) {
		fprintf(stderr, "serval-emu: %s is not a GBA game\n", options.rom);
		return 1;
	}
	core->init(core);
	// mGBA's own config file is not read: the user's mGBA settings (a BIOS,
	// a frameskip) don't change how the debugger's emulator runs a game.
	// No BIOS is loaded: mGBA's built-in replacement runs the BIOS calls.
	mCoreInitConfig(core, NULL);
	mCoreConfigSetDefaultIntValue(&core->config, "useBios", 0);
	mCoreLoadForeignConfig(core, &core->config);

	unsigned width, height;
	core->baseVideoSize(core, &width, &height);
	mColor* pixels = calloc((size_t) width * height, sizeof(mColor));
	core->setVideoBuffer(core, pixels, width);
	core->setAudioBufferSize(core, 2048);

	if (!mCoreLoadFile(core, options.rom)) {
		fprintf(stderr, "serval-emu: can't load %s\n", options.rom);
		return 1;
	}
	mCoreAutoloadSave(core); // the game's save beside it, as mGBA keeps it

	char version[64];
	snprintf(version, sizeof(version), "serval-emu %s (mGBA %s)", SERVAL_EMU_VERSION, projectVersion);
	ServalLinkInit(&_link, core, version);
	_linkReady = true;
	if (options.link && !ServalLinkConnect(&_link, options.link, options.token)) {
		fprintf(stderr, "serval-emu: running without a debugger\n");
	}
	core->reset(core);

	struct Window window = { 0 };
	if (options.headless) {
		SDL_SetMainReady();
		SDL_Init(SDL_INIT_TIMER);
	} else if (!_openWindow(&window, &options, core, width, height)) {
		return 1;
	}

	Uint64 frequency = SDL_GetPerformanceFrequency();
	double frameTicks = (double) frequency / GBA_FPS;
	double nextFrame = (double) SDL_GetPerformanceCounter();
	uint32_t keys = 0;
	uint32_t padKeys = 0;
	long framesRun = 0;
	bool quit = false;
	while (!quit) {
		SDL_Event event;
		while (!options.headless && SDL_PollEvent(&event)) {
			switch (event.type) {
			case SDL_QUIT:
				quit = true;
				break;
			case SDL_KEYDOWN:
				if (!event.key.repeat) {
					keys |= _keyBit(event.key.keysym.sym);
				}
				break;
			case SDL_KEYUP:
				keys &= ~_keyBit(event.key.keysym.sym);
				break;
			case SDL_CONTROLLERBUTTONDOWN:
				padKeys |= _buttonBit(event.cbutton.button);
				break;
			case SDL_CONTROLLERBUTTONUP:
				padKeys &= ~_buttonBit(event.cbutton.button);
				break;
			case SDL_CONTROLLERDEVICEADDED:
				SDL_GameControllerOpen(event.cdevice.which);
				break;
			default:
				break;
			}
		}
		core->setKeys(core, keys | padKeys | _scriptedAt(framesRun));

		ServalLinkService(&_link, 0);
		if (!ServalLinkMachineRuns(&_link)) {
			// Paused or parked: keep the window drawn and wait for the debugger.
			_present(&window, pixels, width, true);
			ServalLinkService(&_link, 15);
			nextFrame = (double) SDL_GetPerformanceCounter();
			continue;
		}

		uint32_t frame = core->frameCounter(core);
		while (core->frameCounter(core) == frame && ServalLinkMachineRuns(&_link)) {
			core->runLoop(core);
		}
		if (core->frameCounter(core) == frame) {
			continue; // parked inside the frame
		}

		_present(&window, pixels, width, false);
		if (!options.headless) {
			_playAudio(&window, core);
		} else {
			mAudioBufferClear(core->getAudioBuffer(core));
		}
		++framesRun;
		if (options.frames && framesRun >= options.frames) {
			break;
		}
		if (!options.unpaced) {
			nextFrame += frameTicks;
			double now = (double) SDL_GetPerformanceCounter();
			if (nextFrame > now) {
				Uint32 ms = (Uint32) ((nextFrame - now) * 1000.0 / (double) frequency);
				if (ms > 1) {
					SDL_Delay(ms - 1);
				}
				while ((double) SDL_GetPerformanceCounter() < nextFrame) {
				}
			} else if (now - nextFrame > frameTicks * 4) {
				nextFrame = now; // far behind (a breakpoint, a slow host): don't rush to catch up
			}
		}
	}

	_linkReady = false;
	ServalLinkDeinit(&_link);
	core->unloadROM(core);
	core->deinit(core);
	free(pixels);
	if (options.headless) {
		SDL_Quit();
	} else {
		_closeWindow(&window);
	}
	return 0;
}

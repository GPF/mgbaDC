/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "main.h"

#include <stdio.h>

#include <mgba/internal/debugger/cli-debugger.h>

#ifdef USE_GDB_STUB
#include <mgba/internal/debugger/gdb-stub.h>
#endif
#ifdef USE_EDITLINE
#include "feature/editline/cli-el-backend.h"
#endif
#ifdef ENABLE_SCRIPTING
#include <mgba/core/scripting.h>

#ifdef ENABLE_PYTHON
#include "platform/python/engine.h"
#endif
#endif

#include <mgba/core/cheats.h>
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/input.h>
#include <mgba/core/serialize.h>
#include <mgba/core/thread.h>
#include <mgba/internal/gba/input.h>

#include <mgba/feature/commandline.h>
#include <mgba-util/vfs.h>

#include <SDL.h>

#include <errno.h>
#include <signal.h>

#ifdef __DREAMCAST__
#include <arch/arch.h>
#include <arch/gdb.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#endif

#define PORT "sdl"

static void mSDLDeinit(struct mSDLRenderer* renderer);

static int mSDLRun(struct mSDLRenderer* renderer, struct mArguments* args);

#ifdef __DREAMCAST__
static void _mSDLDreamcastExit(uint8_t addr, uint32_t buttons) {
	UNUSED(addr);
	UNUSED(buttons);
	exit(EXIT_SUCCESS);
}

static void _mSDLDreamcastRemapPath(char** path, const char* root) {
	if (!*path || strncmp(*path, "/pc/", 4) != 0 || strcmp(root, "/pc") == 0) {
		return;
	}
	size_t suffixLength = strlen(*path) - 3;
	char* remapped = malloc(strlen(root) + suffixLength + 1);
	if (remapped) {
		sprintf(remapped, "%s/%s", root, *path + 4);
		free(*path);
		*path = remapped;
	}
}
#endif

static struct mStandardLogger _logger;

static struct VFile* _state = NULL;

static void _loadState(struct mCoreThread* thread) {
	mCoreLoadStateNamed(thread->core, _state, SAVESTATE_RTC);
}

int main(int argc, char** argv) {
#ifdef _WIN32
	AttachConsole(ATTACH_PARENT_PROCESS);
#endif
#ifdef __DREAMCAST__
	/* dc-load's console redirect goes through ordinary buffered stdio --
	 * the DC_CHECKPOINT printf()s sprinkled through core->init() (gba/
	 * core.c, gba/gba.c) were silently sitting in this buffer and never
	 * appearing before a hang/crash, making every checkpoint before the
	 * fault look like it never ran. Force unbuffered stdout so every
	 * printf is visible immediately, before anything else in main(). */
	setvbuf(stdout, NULL, _IONBF, 0);
#endif
#ifdef __DREAMCAST__
#ifdef DREAMCAST_GDB
	/* Routes SH-4 exceptions into kos-tool's GDB stub instead of KOS's
	 * default abort handler, so a fault traps into a live backtrace
	 * instead of just printing "arch: aborting the system" and dying.
	 * NOT safe to leave compiled in for a normal (non -g) launch: with
	 * this active, a fault waits forever for a GDB connection that isn't
	 * there instead of dying or letting the controller exit-chord below
	 * recover it -- confirmed on hardware, needed a physical reboot.
	 * Only build with -DDREAMCAST_GDB when actually launching with
	 * `kos-tool -g` to attach a debugger. */
	gdb_init();
#endif

	/* No terminal on this target to Ctrl-C out of a hang -- a real
	 * controller exit path is load-bearing, not optional. Register it
	 * before anything else in main() so it's live even if init below
	 * fails or hangs. */
	cont_btn_callback(0,
		CONT_START | CONT_A | CONT_B | CONT_X | CONT_Y,
		_mSDLDreamcastExit);
#endif
	struct mSDLRenderer renderer = {0};

	struct mCoreOptions opts = {
		.useBios = true,
#ifdef __DREAMCAST__
		/* A 600-frame rewind ring is too large for the Dreamcast's 16 MiB
		 * heap once the GBA core, SDL/PVR, thread, and audio are live. */
		.rewindEnable = false,
		.rewindBufferCapacity = 0,
#else
		.rewindEnable = true,
		.rewindBufferCapacity = 600,
#endif
		.audioBuffers = 1024,
		.videoSync = false,
		.audioSync = true,
		.volume = 0x100,
		.logLevel = mLOG_WARN | mLOG_ERROR | mLOG_FATAL,
	};

	struct mArguments args;
	struct mGraphicsOpts graphicsOpts;

	struct mSubParser subparser;

#ifdef __DREAMCAST__
	printf("mgba-dc: main entered, argc=%d argv0=%s\n", argc, argc > 0 && argv[0] ? argv[0] : "(null)");
	printf("mgba-dc: layout PATH_MAX=%u sizeof(mCore)=%u init_offset=%u\n",
		(unsigned)PATH_MAX, (unsigned)sizeof(struct mCore),
		(unsigned)offsetof(struct mCore, init));
	if (argc > 1) {
		printf("mgba-dc: argv1=%s\n", argv[1] ? argv[1] : "(null)");
	}
#endif

	mSubParserGraphicsInit(&subparser, &graphicsOpts);
	bool parsed = mArgumentsParse(&args, argc, argv, &subparser, 1);
#ifdef __DREAMCAST__
	/* KOS's dc-load-ip loader does not actually forward argc/argv into
	 * main() here (confirmed on hardware: argc==0 even when `kos-tool -x
	 * ... -- <path>` reports sending them) -- there is no argv-based ROM
	 * path on this platform, only /pc/ (host-mounted via kos-tool -m) and
	 * /cd/ (burned disc), same as gpSP's Dreamcast port. Fall back to a
	 * fixed path rather than falling through to usage()'s NULL argv[0]
	 * dereference (the actual cause of the first hardware crash here).
	 * TODO: replace with a directory scan once boot is otherwise proven;
	 * hardcoded to match what's staged in cd/roms/ for now. */
	if (!args.fname) {
		free(args.fname);
		args.fname = strdup("/pc/roms/DangerousXmas.gba");
	}
#endif
	if (!args.fname && !args.showVersion) {
		parsed = false;
	}
#ifdef __DREAMCAST__
	printf("mgba-dc: parsed=%d fname=%s showVersion=%d showHelp=%d\n",
		parsed, args.fname ? args.fname : "(null)", args.showVersion, args.showHelp);
#endif
	if (!parsed || args.showHelp) {
		usage(argv[0], NULL, NULL, &subparser, 1);
		mArgumentsDeinit(&args);
		return !parsed;
	}
	if (args.showVersion) {
		version(argv[0]);
		mArgumentsDeinit(&args);
		return 0;
	}

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("Could not initialize video: %s\n", SDL_GetError());
		mArgumentsDeinit(&args);
		return 1;
	}

#ifdef __DREAMCAST__
	mCoreConfigSetDreamcastMediaRoot("/pc");
	printf("mgba-dc: calling mCoreFind(%s)\n", args.fname);
	struct mCore* dbgcore = mCoreFind(args.fname);
	if (!dbgcore && strcmp(args.fname, "/pc/roms/DangerousXmas.gba") == 0) {
		/* dc-load-ip exposes the host mapping as /pc, while a Flycast or
		 * burned-disc launch exposes the same staged files as /cd. */
		printf("mgba-dc: /pc ROM unavailable, trying /cd/roms/DangerousXmas.gba\n");
		mCoreConfigSetDreamcastMediaRoot("/cd");
		free(args.fname);
		args.fname = strdup("/cd/roms/DangerousXmas.gba");
		_mSDLDreamcastRemapPath(&args.patch, "/cd");
		_mSDLDreamcastRemapPath(&args.cheatsFile, "/cd");
		_mSDLDreamcastRemapPath(&args.savestate, "/cd");
		_mSDLDreamcastRemapPath(&args.bios, "/cd");
		dbgcore = mCoreFind(args.fname);
	}
	printf("mgba-dc: immediately on return, dbgcore=%p dbgcore->init=%p\n",
		(void*)dbgcore, dbgcore ? (void*)dbgcore->init : NULL);
	renderer.core = dbgcore;
	printf("mgba-dc: after storing into renderer.core, renderer.core=%p init=%p\n",
		(void*)renderer.core, renderer.core ? (void*)renderer.core->init : NULL);
#else
	renderer.core = mCoreFind(args.fname);
#endif
	if (!renderer.core) {
		printf("Could not run game. Are you sure the file exists and is a compatible game?\n");
		mArgumentsDeinit(&args);
		return 1;
	}

	if (!renderer.core->init(renderer.core)) {
		mArgumentsDeinit(&args);
		return 1;
	}
#ifdef __DREAMCAST__
	printf("mgba-dc: core->init() OK\n");
#endif

	renderer.core->desiredVideoDimensions(renderer.core, &renderer.width, &renderer.height);
	renderer.ratio = graphicsOpts.multiplier;
	if (renderer.ratio == 0) {
		renderer.ratio = 1;
	}
	opts.width = renderer.width * renderer.ratio;
	opts.height = renderer.height * renderer.ratio;

	struct mCheatDevice* device = NULL;
	if (args.cheatsFile && (device = renderer.core->cheatDevice(renderer.core))) {
		struct VFile* vf = VFileOpen(args.cheatsFile, O_RDONLY);
		if (vf) {
			mCheatDeviceClear(device);
			mCheatParseFile(device, vf);
			vf->close(vf);
		}
	}

	mInputMapInit(&renderer.core->inputMap, &GBAInputInfo);
	mCoreInitConfig(renderer.core, PORT);
	mArgumentsApply(&args, &subparser, 1, &renderer.core->config);

	mCoreConfigSetDefaultIntValue(&renderer.core->config, "logToStdout", true);
	mCoreConfigLoadDefaults(&renderer.core->config, &opts);
	mCoreLoadConfig(renderer.core);

	renderer.viewportWidth = renderer.core->opts.width;
	renderer.viewportHeight = renderer.core->opts.height;
	renderer.player.fullscreen = renderer.core->opts.fullscreen;
	renderer.player.windowUpdated = 0;

	renderer.lockAspectRatio = renderer.core->opts.lockAspectRatio;
	renderer.lockIntegerScaling = renderer.core->opts.lockIntegerScaling;
	renderer.interframeBlending = renderer.core->opts.interframeBlending;
	renderer.filter = renderer.core->opts.resampleVideo;

#ifdef BUILD_GL
	if (mSDLGLCommonInit(&renderer)) {
		mSDLGLCreate(&renderer);
	} else
#elif defined(BUILD_GLES2) || defined(USE_EPOXY)
#ifdef BUILD_RASPI
	mRPIGLCommonInit(&renderer);
#else
	if (mSDLGLCommonInit(&renderer))
#endif
	{
		mSDLGLES2Create(&renderer);
	} else
#endif
	{
		mSDLSWCreate(&renderer);
	}

#ifdef __DREAMCAST__
	printf("mgba-dc: calling renderer.init() (window/renderer/texture create)\n");
#endif
	if (!renderer.init(&renderer)) {
		mArgumentsDeinit(&args);
		mCoreConfigDeinit(&renderer.core->config);
		renderer.core->deinit(renderer.core);
		return 1;
	}
#ifdef __DREAMCAST__
	printf("mgba-dc: renderer.init() OK\n");
#endif

	renderer.player.bindings = &renderer.core->inputMap;
	mSDLInitBindingsGBA(&renderer.core->inputMap);
	mSDLInitEvents(&renderer.events);
	mSDLEventsLoadConfig(&renderer.events, mCoreConfigGetInput(&renderer.core->config));
	mSDLAttachPlayer(&renderer.events, &renderer.player);
	mSDLPlayerLoadConfig(&renderer.player, mCoreConfigGetInput(&renderer.core->config));

#if SDL_VERSION_ATLEAST(2, 0, 0)
	renderer.core->setPeripheral(renderer.core, mPERIPH_RUMBLE, &renderer.player.rumble.d);
#endif

	int ret;

	// TODO: Use opts and config
	mStandardLoggerInit(&_logger);
	mStandardLoggerConfig(&_logger, &renderer.core->config);
	ret = mSDLRun(&renderer, &args);
	mSDLDetachPlayer(&renderer.events, &renderer.player);
	mInputMapDeinit(&renderer.core->inputMap);

	if (device) {
		mCheatDeviceDestroy(device);
	}

	mSDLDeinit(&renderer);
	mStandardLoggerDeinit(&_logger);

	mArgumentsDeinit(&args);
	mCoreConfigFreeOpts(&opts);
	mCoreConfigDeinit(&renderer.core->config);
	renderer.core->deinit(renderer.core);

	return ret;
}

#if defined(_WIN32) && !defined(_UNICODE)
#include <mgba-util/string.h>

int wmain(int argc, wchar_t** argv) {
	char** argv8 = malloc(sizeof(char*) * argc);
	int i;
	for (i = 0; i < argc; ++i) {
		argv8[i] = utf16to8((uint16_t*) argv[i], wcslen(argv[i]) * 2);
	}
	__argv = argv8;
	int ret = main(argc, argv8);
	for (i = 0; i < argc; ++i) {
		free(argv8[i]);
	}
	free(argv8);
	return ret;
}
#endif

int mSDLRun(struct mSDLRenderer* renderer, struct mArguments* args) {
	struct mCoreThread thread = {
		.core = renderer->core
	};
#ifdef __DREAMCAST__
	printf("mgba-dc: calling mCoreLoadFile(%s)\n", args->fname);
#endif
	if (!mCoreLoadFile(renderer->core, args->fname)) {
		return 1;
	}
#ifdef __DREAMCAST__
	printf("mgba-dc: mCoreLoadFile OK, autoloading save/cheats\n");
#endif
	mCoreAutoloadSave(renderer->core);
	mCoreAutoloadCheats(renderer->core);
#ifdef __DREAMCAST__
	printf("mgba-dc: autoload done, starting core thread\n");
#endif
#ifdef ENABLE_SCRIPTING
	struct mScriptBridge* bridge = mScriptBridgeCreate();
#ifdef ENABLE_PYTHON
	mPythonSetup(bridge);
#endif
#ifdef USE_DEBUGGERS
	CLIDebuggerScriptEngineInstall(bridge);
#endif
#endif

#ifdef USE_DEBUGGERS
	struct mDebugger* debugger = mDebuggerCreate(args->debuggerType, renderer->core);
	if (debugger) {
#ifdef USE_EDITLINE
		if (args->debuggerType == DEBUGGER_CLI) {
			struct CLIDebugger* cliDebugger = (struct CLIDebugger*) debugger;
			CLIDebuggerAttachBackend(cliDebugger, CLIDebuggerEditLineBackendCreate());
		}
#endif
		mDebuggerAttach(debugger, renderer->core);
		mDebuggerEnter(debugger, DEBUGGER_ENTER_MANUAL, NULL);
#ifdef ENABLE_SCRIPTING
		mScriptBridgeSetDebugger(bridge, debugger);
#endif
	}
#endif

	if (args->patch) {
		struct VFile* patch = VFileOpen(args->patch, O_RDONLY);
		if (patch) {
			renderer->core->loadPatch(renderer->core, patch);
		}
	} else {
		mCoreAutoloadPatch(renderer->core);
	}

	renderer->audio.samples = renderer->core->opts.audioBuffers;
	renderer->audio.sampleRate = 44100;
	thread.logger.logger = &_logger.d;

	bool didFail = !mCoreThreadStart(&thread);

	if (!didFail) {
#if SDL_VERSION_ATLEAST(2, 0, 0)
		renderer->core->desiredVideoDimensions(renderer->core, &renderer->width, &renderer->height);
		unsigned width = renderer->width * renderer->ratio;
		unsigned height = renderer->height * renderer->ratio;
		if (width != (unsigned) renderer->viewportWidth && height != (unsigned) renderer->viewportHeight) {
			SDL_SetWindowSize(renderer->window, width, height);
			renderer->player.windowUpdated = 1;
		}
		mSDLSetScreensaverSuspendable(&renderer->events, renderer->core->opts.suspendScreensaver);
		mSDLSuspendScreensaver(&renderer->events);
#endif
		if (mSDLInitAudio(&renderer->audio, &thread)) {
			if (args->savestate) {
				struct VFile* state = VFileOpen(args->savestate, O_RDONLY);
				if (state) {
					_state = state;
					mCoreThreadRunFunction(&thread, _loadState);
					_state = NULL;
					state->close(state);
				}
			}
			renderer->runloop(renderer, &thread);
			mSDLPauseAudio(&renderer->audio);
			if (mCoreThreadHasCrashed(&thread)) {
				didFail = true;
				printf("The game crashed!\n");
				mCoreThreadEnd(&thread);
			}
		} else {
			didFail = true;
			printf("Could not initialize audio.\n");
		}
#if SDL_VERSION_ATLEAST(2, 0, 0)
		mSDLResumeScreensaver(&renderer->events);
		mSDLSetScreensaverSuspendable(&renderer->events, false);
#endif

		mCoreThreadJoin(&thread);
	} else {
		printf("Could not run game. Are you sure the file exists and is a compatible game?\n");
	}
	renderer->core->unloadROM(renderer->core);

#ifdef ENABLE_SCRIPTING
	mScriptBridgeDestroy(bridge);
#endif

	return didFail;
}

static void mSDLDeinit(struct mSDLRenderer* renderer) {
	mSDLDeinitEvents(&renderer->events);
	mSDLDeinitAudio(&renderer->audio);
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_DestroyWindow(renderer->window);
#endif

	renderer->deinit(renderer);

	SDL_Quit();
}

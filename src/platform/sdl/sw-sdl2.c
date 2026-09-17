/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "main.h"

#include <mgba/core/core.h>
#include <mgba/core/thread.h>
#include <mgba/core/version.h>

#ifdef __DREAMCAST__
#include <string.h>
#endif

static bool mSDLSWInit(struct mSDLRenderer* renderer);
static void mSDLSWRunloop(struct mSDLRenderer* renderer, void* user);
static void mSDLSWDeinit(struct mSDLRenderer* renderer);

void mSDLSWCreate(struct mSDLRenderer* renderer) {
	renderer->init = mSDLSWInit;
	renderer->deinit = mSDLSWDeinit;
	renderer->runloop = mSDLSWRunloop;
}

#ifdef __DREAMCAST__
static int _findPVRRenderer(void) {
	SDL_RendererInfo info;
	int i;
	for (i = 0; i < SDL_GetNumRenderDrivers(); ++i) {
		if (SDL_GetRenderDriverInfo(i, &info) == 0 && info.name && !strcmp(info.name, "Dreamcast PVR")) {
			return i;
		}
	}
	return -1;
}
#endif

bool mSDLSWInit(struct mSDLRenderer* renderer) {
	unsigned width, height;
	renderer->core->desiredVideoDimensions(renderer->core, &width, &height);
	/* SDL_WINDOW_OPENGL asks SDL for a GL-backed window; KOS's Dreamcast SDL2
	 * port only implements the "Dreamcast PVR" 2D render driver (no GL
	 * context), same reasoning as cannonball's src/main/sdl2/rendersurface.cpp. */
#ifdef __DREAMCAST__
	renderer->window = SDL_CreateWindow(projectName, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, renderer->viewportWidth, renderer->viewportHeight, SDL_WINDOW_FULLSCREEN_DESKTOP * renderer->player.fullscreen);
#else
	renderer->window = SDL_CreateWindow(projectName, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, renderer->viewportWidth, renderer->viewportHeight, SDL_WINDOW_OPENGL | (SDL_WINDOW_FULLSCREEN_DESKTOP * renderer->player.fullscreen));
#endif
	SDL_GetWindowSize(renderer->window, &renderer->viewportWidth, &renderer->viewportHeight);
	renderer->player.window = renderer->window;
#ifdef __DREAMCAST__
	renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, _findPVRRenderer(), SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
#else
	renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
#endif
#ifdef COLOR_16_BIT
#ifdef COLOR_5_6_5
	renderer->sdlTex = SDL_CreateTexture(renderer->sdlRenderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, width, height);
#else
	renderer->sdlTex = SDL_CreateTexture(renderer->sdlRenderer, SDL_PIXELFORMAT_ABGR1555, SDL_TEXTUREACCESS_STREAMING, width, height);
#endif
#else
	renderer->sdlTex = SDL_CreateTexture(renderer->sdlRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, width, height);
#endif

	int stride;
	SDL_LockTexture(renderer->sdlTex, 0, (void**) &renderer->outputBuffer, &stride);
	renderer->core->setVideoBuffer(renderer->core, renderer->outputBuffer, stride / BYTES_PER_PIXEL);

	return true;
}

void mSDLSWRunloop(struct mSDLRenderer* renderer, void* user) {
	struct mCoreThread* context = user;
	SDL_Event event;

	while (mCoreThreadIsActive(context)) {
		while (SDL_PollEvent(&event)) {
			mSDLHandleEvent(context, &renderer->player, &event);
		}

		if (mCoreSyncWaitFrameStart(&context->impl->sync)) {
			SDL_UnlockTexture(renderer->sdlTex);
			SDL_RenderCopy(renderer->sdlRenderer, renderer->sdlTex, 0, 0);
			SDL_RenderPresent(renderer->sdlRenderer);
			int stride;
			SDL_LockTexture(renderer->sdlTex, 0, (void**) &renderer->outputBuffer, &stride);
			renderer->core->setVideoBuffer(renderer->core, renderer->outputBuffer, stride / BYTES_PER_PIXEL);
		}
		mCoreSyncWaitFrameEnd(&context->impl->sync);
	}
}

void mSDLSWDeinit(struct mSDLRenderer* renderer) {
	if (renderer->ratio > 1) {
		free(renderer->outputBuffer);
	}
}

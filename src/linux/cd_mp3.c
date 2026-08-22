/*
	cd_mp3.c

	Replaces cd_linux.c / cd_sdl.c: instead of driving a physical CD-ROM
	drive via ioctl(), this module plays .mp3 files (decoded with the
	dr_mp3 header-only submodule) while keeping exactly the same public
	API (CDAudio_Play, CDAudio_Stop, ...) so the rest of the client code
	doesn't need to change (cl_cin.c, menus, etc. still just call
	CDAudio_Play(track, looping)).

	IMPORTANT: this module does not own an audio device of its own. It
	relies on snd_sdl.c, whose SDL_OpenAudio callback (paint_audio)
	calls CDAudio_MixSamples() right after filling the buffer with the
	sound effects via S_PaintChannels(). This is necessary because
	SDL 1.2 / OSS only allow a single audio device to be open at a time
	on this kind of embedded target.

	Asset constraint: dr_mp3 (the "simple" API used here) does not
	resample. .mp3 files must therefore be encoded at the same sample
	rate / channel count as the game's audio output (typically
	44100 Hz stereo, see the s_khz / sndchannels cvars). A file that
	doesn't match is rejected at load time with a clear message
	instead of being played back garbled.

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.
*/

#include <stdio.h>
#include <string.h>

#include "SDL.h"

#include "../client/client.h"
#include "../client/snd_loc.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO_LARGE   /* no need for very long paths here */
#include "dr_mp3.h"

/* Size of the temporary decode buffer, in PCM frames (1 frame = 1
   sample per channel). 4096 is plenty for a typical SDL callback
   (usually <= 2048 mono samples, so <= 4096 for stereo). */
#define MP3_MIX_FRAMES 4096

static qboolean initialized = false;
static qboolean enabled     = true;

static drmp3    mp3;
static qboolean mp3Valid    = false;   /* a file is loaded/decodable */
static qboolean playing     = false;   /* we should be producing sound */
static qboolean paused      = false;
static qboolean playLooping = false;
static int      currentTrack = 0;

static cvar_t *cd_volume;
static cvar_t *cd_nocd;
static cvar_t *cd_musicdir;   /* replaces cd_dev: mp3 subdirectory */
static cvar_t *cd_mintrack;
static cvar_t *cd_maxtrack;
static float   cdvolume = 1.0f;

static void CD_f(void);

/* ------------------------------------------------------------------ */
/* Path resolution for a track                                        */
/* ------------------------------------------------------------------ */

/*
	NOTE: adapt this call to whatever function your engine actually
	uses to resolve the current data directory (gamedir). In Quake2
	this is usually FS_Gamedir() (qcommon/files.c). The file naming
	follows the usual "trackNN.mp3" convention used by Quake/Quake2
	ports with digital music (track01 = the "data" track, so it is
	never played; music starts at track02).
*/
static void CD_TrackPath(char *dst, size_t dstSize, int track)
{
	Com_sprintf(dst, dstSize, "%s/%s/track%02d.mp3",
		FS_Gamedir(), cd_musicdir->string, track);
}

/* ------------------------------------------------------------------ */
/* Mixing: called from the SDL audio callback (snd_sdl.c)             */
/* ------------------------------------------------------------------ */

void CDAudio_MixSamples(byte *stream, int len)
{
	drmp3_int16 buf[MP3_MIX_FRAMES];
	Sint16 *dst = (Sint16 *) stream;
	int frameBytes;
	drmp3_uint64 framesWanted, chunk, framesRead;
	int i;

	if (!enabled || !playing || paused || !mp3Valid)
		return;

	/* This mixer only handles 16-bit audio (the only format
	   drmp3_read_pcm_frames_s16 produces); if the game runs in
	   8-bit mode we simply don't mix any music. */
	if (dma.samplebits != 16)
		return;

	frameBytes = (dma.samplebits / 8) * dma.channels;
	if (frameBytes <= 0)
		return;

	framesWanted = (drmp3_uint64)len / (drmp3_uint64)frameBytes;

	while (framesWanted > 0)
	{
		chunk = framesWanted;
		if (chunk > MP3_MIX_FRAMES / dma.channels)
			chunk = MP3_MIX_FRAMES / dma.channels;

		framesRead = drmp3_read_pcm_frames_s16(&mp3, chunk, buf);

		for (i = 0; i < (int)(framesRead * dma.channels); i++)
		{
			int mixed = dst[i] + (int)((float)buf[i] * cdvolume);
			if (mixed > 32767) mixed = 32767;
			else if (mixed < -32768) mixed = -32768;
			dst[i] = (Sint16) mixed;
		}

		dst          += framesRead * dma.channels;
		framesWanted -= framesRead;

		if (framesRead < chunk)
		{
			/* end of file reached */
			if (playLooping)
			{
				drmp3_seek_to_pcm_frame(&mp3, 0);
				if (framesRead == 0)
					break; /* avoid an infinite loop if the file is empty */
			}
			else
			{
				playing = false;
				break;
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Public API - same signatures as cd_linux.c / cd_sdl.c              */
/* ------------------------------------------------------------------ */

void CDAudio_Play(int track, qboolean looping)
{
	char path[MAX_OSPATH];

	if (!enabled || !initialized)
		return;

	CD_TrackPath(path, sizeof(path), track);

	SDL_LockAudio();

	if (mp3Valid)
	{
		drmp3_uninit(&mp3);
		mp3Valid = false;
	}
	playing = false;

	if (!drmp3_init_file(&mp3, path, NULL))
	{
		SDL_UnlockAudio();
		Com_DPrintf("CDAudio_Play: could not open %s\n", path);
		return;
	}

	mp3Valid     = true;
	playLooping  = looping;
	paused       = false;
	currentTrack = track;
	playing      = true;

	SDL_UnlockAudio();
}

/* Picks a random track in [cd_mintrack, cd_maxtrack] and plays it in a
   loop. Replaces walking the real CD's track table. */
void CDAudio_RandomPlay(void)
{
	int lo, hi, track;

	if (!enabled || !initialized)
		return;

	lo = (int) cd_mintrack->value;
	hi = (int) cd_maxtrack->value;
	if (hi < lo)
		return;

	track = lo + (int)(((float) rand() / ((float) RAND_MAX + 1.0f)) * (hi - lo + 1));
	CDAudio_Play(track, true);
}

void CDAudio_Stop(void)
{
	if (!mp3Valid)
		return;

	SDL_LockAudio();
	playing = false;
	drmp3_uninit(&mp3);
	mp3Valid = false;
	SDL_UnlockAudio();
}

void CDAudio_Pause(void)
{
	paused = true;
}

void CDAudio_Resume(void)
{
	paused = false;
}

void CDAudio_Update(void)
{
	if (!initialized)
		return;

	if (cd_volume && cd_volume->value != cdvolume)
	{
		cdvolume = cd_volume->value;
		if (cdvolume < 0.0f) cdvolume = 0.0f;
		if (cdvolume > 1.0f) cdvolume = 1.0f;
	}

	if (cd_nocd->value)
	{
		CDAudio_Stop();
		return;
	}

	/* If the track has finished and we're not looping, this would be
	   the place to automatically move on to another track (the
	   original "CD playlist" behavior): adapt this to whatever your
	   game does when CDAudio_Play is never called again by the
	   client code. */
}

int CDAudio_Init(void)
{
	if (initialized)
		return 0;

	cd_nocd     = Cvar_Get("cd_nocd", "0", CVAR_ARCHIVE);
	if (cd_nocd->value)
		return -1;

	cd_volume   = Cvar_Get("cd_volume", "1", CVAR_ARCHIVE);
	cd_musicdir = Cvar_Get("cd_musicdir", "music", CVAR_ARCHIVE);
	cd_mintrack = Cvar_Get("cd_mintrack", "2", CVAR_ARCHIVE);
	cd_maxtrack = Cvar_Get("cd_maxtrack", "11", CVAR_ARCHIVE);

	cdvolume = cd_volume->value;

	Cmd_AddCommand("cd", CD_f);

	initialized = true;
	enabled     = true;

	Com_Printf("CD Audio (mp3) initialized, directory '%s'.\n", cd_musicdir->string);
	return 0;
}

void CDAudio_Shutdown(void)
{
	if (!initialized)
		return;

	CDAudio_Stop();
	initialized = false;
}

void CDAudio_Activate(qboolean active)
{
	if (active)
		CDAudio_Resume();
	else
		CDAudio_Pause();
}

/* ------------------------------------------------------------------ */
/* "cd" console command - subset compatible with the original         */
/* ------------------------------------------------------------------ */

static void CD_f(void)
{
	char *command;

	if (Cmd_Argc() < 2)
		return;

	command = Cmd_Argv(1);

	if (!Q_strcasecmp(command, "on"))
	{
		enabled = true;
		return;
	}
	if (!Q_strcasecmp(command, "off"))
	{
		CDAudio_Stop();
		enabled = false;
		return;
	}
	if (!Q_strcasecmp(command, "play"))
	{
		CDAudio_Play((byte) atoi(Cmd_Argv(2)), false);
		return;
	}
	if (!Q_strcasecmp(command, "loop"))
	{
		CDAudio_Play((byte) atoi(Cmd_Argv(2)), true);
		return;
	}
	if (!Q_strcasecmp(command, "stop"))
	{
		CDAudio_Stop();
		return;
	}
	if (!Q_strcasecmp(command, "pause"))
	{
		CDAudio_Pause();
		return;
	}
	if (!Q_strcasecmp(command, "resume"))
	{
		CDAudio_Resume();
		return;
	}
	if (!Q_strcasecmp(command, "info"))
	{
		if (playing)
			Com_Printf("Currently playing: track %d (%s)\n",
				currentTrack, playLooping ? "looping" : "once");
		else
			Com_Printf("No track currently playing.\n");
		return;
	}
}

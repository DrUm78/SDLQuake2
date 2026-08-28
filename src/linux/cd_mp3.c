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

	Asset constraint: none anymore regarding sample rate - CDAudio_Play()
	accepts any mono or stereo MP3 and CDAudio_MixSamples() resamples
	it on the fly (simple linear interpolation) to match the game's
	audio output format (dma.speed / dma.channels), and remaps mono
	<-> stereo as needed. This keeps memory use low (dr_mp3's
	streaming API itself never resamples - only its "decode the whole
	file into RAM" functions can, which isn't practical here for
	continuously-looping background music on an embedded target).

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.
*/

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include "SDL.h"

#include "../client/client.h"
#include "../client/snd_loc.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO_LARGE   /* no need for very long paths here */
#include "dr_mp3.h"

/* Number of interleaved source-format PCM frames kept buffered for
   resampling. 8192 frames * up to 2 channels * 2 bytes = 32 KB, cheap
   enough to keep static on an embedded target. */
#define MP3_SRC_BUF_FRAMES 8192

static qboolean initialized = false;
static qboolean enabled     = true;

static drmp3    mp3;
static qboolean mp3Valid    = false;   /* a file is loaded/decodable */
static qboolean playing     = false;   /* we should be producing sound */
static qboolean paused      = false;
static qboolean playLooping = false;
static int      currentTrack = 0;
static float    mp3Ratio    = 1.0f;    /* source sampleRate / dma.speed */

/* Small resampler state: a buffer of already-decoded source-format
   frames plus a fractional read cursor into it. Reset on every
   CDAudio_Play(). See MP3_Refill() / CDAudio_MixSamples(). */
typedef struct
{
	drmp3_int16 buf[MP3_SRC_BUF_FRAMES * 2]; /* interleaved, up to 2 src channels */
	int         avail; /* valid frames currently in buf, starting at index 0 */
	float       pos;   /* fractional read position, in source frames, into buf */
	qboolean    eof;   /* true once the decoder has nothing left to give and we're not looping */
} mp3_resample_t;

static mp3_resample_t mp3rs;

/* Maximum number of tracks CD_ScanMusicDir() will collect. 256 is
   generous for an embedded target's music folder. */
#define MP3_MAX_TRACKS 256

static cvar_t *cd_volume;
static cvar_t *cd_nocd;
static cvar_t *cd_musicdir;   /* replaces cd_dev: mp3 subdirectory */
static cvar_t *cd_mintrack;   /* floor filter for CDAudio_RandomPlay, default skips the data track */
static float   cdvolume = 1.0f;

static void CD_f(void);

/* ------------------------------------------------------------------ */
/* Path resolution for a track                                        */
/* ------------------------------------------------------------------ */

/*
	NOTE: adapt this call to whatever function your engine actually
	uses to resolve the current data directory (gamedir). In Quake2
	this is usually FS_Gamedir() (qcommon/files.c).
*/

/* Creates the music directory if it doesn't exist yet. Best-effort:
   if it fails (permissions, read-only filesystem...), CD_TrackPath()
   below will simply fail to find any file in it and CDAudio_Play()
   will log that and do nothing, same as a missing file today. */
static void CD_EnsureMusicDir(void)
{
	char dir[MAX_OSPATH];
	struct stat st;

	Com_sprintf(dir, sizeof(dir), "%s/%s", FS_Gamedir(), cd_musicdir->string);

	if (stat(dir, &st) == 0)
		return; /* already there (file or dir, either way don't touch it) */

	if (mkdir(dir, 0755) != 0)
		Com_DPrintf("CD_EnsureMusicDir: could not create %s\n", dir);
}

/* Resolves the file for a given track number, accepting both naming
   conventions: "trackNN.mp3" (classic Quake/Quake2 CD-rip convention)
   and the shorter "NN.mp3". Tries trackNN.mp3 first. Returns true and
   fills dst with the path that was actually found; returns false if
   neither form exists (dst is still filled with the trackNN.mp3 form,
   handy for the "file not found" log message). */
static qboolean CD_TrackPath(char *dst, size_t dstSize, int track)
{
	char alt[MAX_OSPATH];

	CD_EnsureMusicDir();

	Com_sprintf(dst, dstSize, "%s/%s/track%02d.mp3",
		FS_Gamedir(), cd_musicdir->string, track);
	if (access(dst, F_OK) == 0)
		return true;

	Com_sprintf(alt, sizeof(alt), "%s/%s/%02d.mp3",
		FS_Gamedir(), cd_musicdir->string, track);
	if (access(alt, F_OK) == 0)
	{
		Com_sprintf(dst, dstSize, "%s", alt);
		return true;
	}

	return false;
}

/* Scans the music directory for files matching either "trackNN.mp3"
   or "NN.mp3" and collects the track numbers found into 'tracks'
   (deduplicated - if both forms exist for the same number it's only
   counted once). Tracks below cd_mintrack are skipped (default 2,
   to skip a would-be "data track" the way real Quake2 CDs do).
   Returns the number of tracks found. */
static int CD_ScanMusicDir(int *tracks, int maxTracks)
{
	char dir[MAX_OSPATH];
	DIR *d;
	struct dirent *entry;
	int count = 0;
	int i;

	CD_EnsureMusicDir();
	Com_sprintf(dir, sizeof(dir), "%s/%s", FS_Gamedir(), cd_musicdir->string);

	d = opendir(dir);
	if (!d)
		return 0;

	while (count < maxTracks && (entry = readdir(d)) != NULL)
	{
		int track, chars;
		qboolean matched = false;
		size_t nameLen = strlen(entry->d_name);

		if (sscanf(entry->d_name, "track%d.mp3%n", &track, &chars) == 1 &&
			(size_t)chars == nameLen)
			matched = true;
		else if (sscanf(entry->d_name, "%d.mp3%n", &track, &chars) == 1 &&
			(size_t)chars == nameLen)
			matched = true;

		if (!matched || track < (int)cd_mintrack->value)
			continue;

		for (i = 0; i < count; i++)
			if (tracks[i] == track)
				break;
		if (i < count)
			continue; /* already have this track number */

		tracks[count++] = track;
	}

	closedir(d);
	return count;
}

/* ------------------------------------------------------------------ */
/* Resampling / mixing: called from the SDL audio callback (snd_sdl.c) */
/* ------------------------------------------------------------------ */

/* Compacts already-consumed frames out of mp3rs.buf and decodes more
   source PCM to top it back up. Handles looping transparently: if the
   decoder runs dry and playLooping is set, it seeks back to frame 0
   and keeps filling. Leaves mp3rs.eof set once there is truly nothing
   left to decode (non-looping track that reached its end). */
static void MP3_Refill(void)
{
	int srcCh = (int) mp3.channels;
	int keep;
	drmp3_uint64 framesRead;

	if (mp3rs.eof)
		return;

	keep = (int) mp3rs.pos;
	if (keep > 0 && keep < mp3rs.avail)
	{
		int remain = mp3rs.avail - keep;
		memmove(mp3rs.buf, mp3rs.buf + keep * srcCh, remain * srcCh * sizeof(drmp3_int16));
		mp3rs.avail = remain;
		mp3rs.pos  -= (float) keep;
	}
	else if (keep >= mp3rs.avail)
	{
		mp3rs.avail = 0;
		mp3rs.pos   = 0.0f;
	}

	if (mp3rs.avail >= MP3_SRC_BUF_FRAMES - 1)
		return; /* still enough buffered, nothing to do */

	framesRead = drmp3_read_pcm_frames_s16(&mp3,
		MP3_SRC_BUF_FRAMES - mp3rs.avail, mp3rs.buf + mp3rs.avail * srcCh);

	if (framesRead == 0)
	{
		if (playLooping)
		{
			drmp3_seek_to_pcm_frame(&mp3, 0);
			framesRead = drmp3_read_pcm_frames_s16(&mp3,
				MP3_SRC_BUF_FRAMES - mp3rs.avail, mp3rs.buf + mp3rs.avail * srcCh);
		}

		if (framesRead == 0)
		{
			mp3rs.eof = true;
			return;
		}
	}

	mp3rs.avail += (int) framesRead;
}

/* Converts one already-resampled source-format sample (srcCh values)
   to the destination channel count (1 or 2). Anything other than a
   1<->2 conversion (shouldn't happen for MP3 in practice) falls back
   to silence rather than reading out of bounds. */
static void MP3_RemapChannels(const float *src, int srcCh, float *dst, int destCh)
{
	int c;

	if (srcCh == destCh)
	{
		for (c = 0; c < destCh; c++)
			dst[c] = src[c];
	}
	else if (srcCh == 1 && destCh == 2)
	{
		dst[0] = dst[1] = src[0];
	}
	else if (srcCh == 2 && destCh == 1)
	{
		dst[0] = (src[0] + src[1]) * 0.5f;
	}
	else
	{
		for (c = 0; c < destCh; c++)
			dst[c] = 0.0f;
	}
}

void CDAudio_MixSamples(byte *stream, int len)
{
	Sint16 *dst = (Sint16 *) stream;
	int destCh, srcCh;
	int frameBytes, framesWanted, i;

	if (!enabled || !playing || paused || !mp3Valid)
		return;

	if (dma.samplebits != 16)
		return;

	destCh = dma.channels;
	srcCh  = (int) mp3.channels;
	frameBytes = (dma.samplebits / 8) * destCh;
	if (frameBytes <= 0)
		return;

	framesWanted = len / frameBytes;

	for (i = 0; i < framesWanted; i++)
	{
		int srcFrame;
		float frac;
		float srcSample[2];
		float outSample[2];
		int c;

		srcFrame = (int) mp3rs.pos;

		if (srcFrame + 1 >= mp3rs.avail)
		{
			break;
		}

		frac = mp3rs.pos - (float) srcFrame;

		for (c = 0; c < srcCh; c++)
		{
			drmp3_int16 a = mp3rs.buf[srcFrame * srcCh + c];
			drmp3_int16 b = mp3rs.buf[(srcFrame + 1) * srcCh + c];
			srcSample[c] = (float) a + (float) (b - a) * frac;
		}

		MP3_RemapChannels(srcSample, srcCh, outSample, destCh);

		for (c = 0; c < destCh; c++)
		{
			int mixed = dst[i * destCh + c] + (int) (outSample[c] * cdvolume);
			if (mixed > 32767) mixed = 32767;
			else if (mixed < -32768) mixed = -32768;
			dst[i * destCh + c] = (Sint16) mixed;
		}

		mp3rs.pos += mp3Ratio;
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

	if (!CD_TrackPath(path, sizeof(path), track))
	{
		Com_DPrintf("CDAudio_Play: no file found for track %d "
			"(tried track%02d.mp3 and %02d.mp3 in '%s')\n",
			track, track, track, cd_musicdir->string);
		return;
	}

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

	if (mp3.channels < 1 || mp3.channels > 2)
	{
		Com_Printf("CDAudio_Play: %s has %u channels, only mono/stereo "
			"MP3 is supported.\n", path, mp3.channels);
		drmp3_uninit(&mp3);
		SDL_UnlockAudio();
		return;
	}

	mp3Ratio = (float) mp3.sampleRate / (float) dma.speed;
	memset(&mp3rs, 0, sizeof(mp3rs));

	Com_DPrintf("CDAudio_Play: %s is %u Hz / %u channels, converting to "
		"%d Hz / %d channels on the fly.\n",
		path, mp3.sampleRate, mp3.channels, dma.speed, dma.channels);

	mp3Valid     = true;
	playLooping  = looping;
	paused       = false;
	currentTrack = track;
	playing      = true;

	SDL_UnlockAudio();
}

/* Picks a random track among the .mp3 files actually present in the
   music directory and plays it in a loop. Replaces walking the real
   CD's track table. */
void CDAudio_RandomPlay(void)
{
	int tracks[MP3_MAX_TRACKS];
	int count, idx;

	if (!enabled || !initialized)
		return;

	count = CD_ScanMusicDir(tracks, MP3_MAX_TRACKS);
	if (count == 0)
	{
		Com_DPrintf("CDAudio_RandomPlay: no track*.mp3 / *.mp3 files "
			"found in '%s'\n", cd_musicdir->string);
		return;
	}

	idx = (int)(((float) rand() / ((float) RAND_MAX + 1.0f)) * count);
	CDAudio_Play(tracks[idx], true);
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

	if (playing && mp3Valid && !paused)
	{
		SDL_LockAudio();
		if (mp3rs.avail - (int) mp3rs.pos < MP3_SRC_BUF_FRAMES / 4)
			MP3_Refill();
		SDL_UnlockAudio();
	}
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

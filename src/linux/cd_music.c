/*
	cd_music.c

	Replaces cd_linux.c / cd_sdl.c (formerly cd_mp3.c): instead of
	driving a physical CD-ROM drive via ioctl(), this module plays
	background music files from disk (MP3 via dr_mp3, FLAC via
	dr_flac - both header-only) while keeping exactly the same public
	API (CDAudio_Play, CDAudio_Stop, ...) so the rest of the client
	code doesn't need to change (cl_cin.c, menus, etc. still just call
	CDAudio_Play(track, looping)).

	IMPORTANT: this module does not own an audio device of its own. It
	relies on snd_sdl.c, whose SDL_OpenAudio callback (paint_audio)
	calls CDAudio_MixSamples() right after filling the buffer with the
	sound effects via S_PaintChannels(). This is necessary because
	SDL 1.2 / OSS only allow a single audio device to be open at a time
	on this kind of embedded target.

	Format support: a track can be provided as MP3 or FLAC (see
	CD_TrackPath() for the exact filenames tried, in order). Whichever
	is found is opened with the matching decoder; from there on, both
	formats are handled identically by a small format-agnostic
	interface (Music_ReadFrames / Music_SeekToStart / Music_Close, see
	below) so the resampling/mixing code never needs to know which
	decoder produced the samples.

	Asset constraint: none regarding sample rate or channel count -
	CDAudio_Play() accepts any mono or stereo MP3/FLAC and
	CDAudio_MixSamples() resamples it on the fly (simple linear
	interpolation) to match the game's audio output format
	(dma.speed / dma.channels), and remaps mono <-> stereo as needed.
	This keeps memory use low (neither dr_mp3 nor dr_flac resample in
	their streaming APIs - only their "decode the whole file into RAM"
	functions can, which isn't practical here for continuously-looping
	background music on an embedded target).

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

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

/* Number of interleaved source-format PCM frames kept buffered for
   resampling. 8192 frames * up to 2 channels * 2 bytes = 32 KB, cheap
   enough to keep static on an embedded target. */
#define MUSIC_SRC_BUF_FRAMES 8192

/* Maximum number of tracks CD_ScanMusicDir() will collect. 256 is
   generous for an embedded target's music folder. */
#define MUSIC_MAX_TRACKS 256

typedef enum
{
	MUSIC_FMT_NONE = 0,
	MUSIC_FMT_MP3,
	MUSIC_FMT_FLAC
} musicFormat_t;

static qboolean initialized = false;
static qboolean enabled     = true;

/* Decoder state. Only one of these is active at a time, selected by
   curFormat - see the Music_* wrappers below. drmp3 is a value type
   you own (drmp3_init_file fills it in place); drflac is opaque and
   heap-allocated by the library itself (drflac_open_file returns a
   pointer), hence the two different storage shapes. */
static musicFormat_t curFormat    = MUSIC_FMT_NONE;
static drmp3         mp3;
static drflac        *flac        = NULL;

static qboolean mp3Valid    = false;   /* a file is loaded/decodable (either format) */
static qboolean playing     = false;   /* we should be producing sound */
static qboolean paused      = false;
static qboolean playLooping = false;
static int      currentTrack = 0;

/* Cached once at CDAudio_Play() time so the resampler/mixer never
   needs to know which decoder is active to know the source format. */
static int          srcChannels   = 2;
static drmp3_uint32  srcSampleRate = 44100;
static float         musicRatio    = 1.0f;   /* srcSampleRate / dma.speed */

/* Small resampler state: a buffer of already-decoded source-format
   frames plus a fractional read cursor into it. Reset on every
   CDAudio_Play(). See Music_Refill() / CDAudio_MixSamples(). */
typedef struct
{
	drmp3_int16 buf[MUSIC_SRC_BUF_FRAMES * 2]; /* interleaved, up to 2 src channels */
	int         avail; /* valid frames currently in buf, starting at index 0 */
	float       pos;   /* fractional read position, in source frames, into buf */
	qboolean    eof;   /* true once the decoder has nothing left to give and we're not looping */
} music_resample_t;

static music_resample_t musicrs;

static cvar_t *cd_volume;
static cvar_t *cd_nocd;
static cvar_t *cd_musicdir;   /* replaces cd_dev: music subdirectory */
static cvar_t *cd_mintrack;   /* floor filter for CDAudio_RandomPlay, default skips the data track */
static float   cdvolume = 1.0f;

static void CD_f(void);

/* ------------------------------------------------------------------ */
/* Format-agnostic decoder interface                                  */
/* ------------------------------------------------------------------ */

static drmp3_uint64 Music_ReadFrames(drmp3_uint64 frames, drmp3_int16 *buf)
{
	switch (curFormat)
	{
		case MUSIC_FMT_MP3:  return drmp3_read_pcm_frames_s16(&mp3, frames, buf);
		case MUSIC_FMT_FLAC: return drflac_read_pcm_frames_s16(flac, frames, buf);
		default:             return 0;
	}
}

static void Music_SeekToStart(void)
{
	switch (curFormat)
	{
		case MUSIC_FMT_MP3:  drmp3_seek_to_pcm_frame(&mp3, 0);  break;
		case MUSIC_FMT_FLAC: drflac_seek_to_pcm_frame(flac, 0); break;
		default: break;
	}
}

static void Music_Close(void)
{
	switch (curFormat)
	{
		case MUSIC_FMT_MP3:
			drmp3_uninit(&mp3);
			break;
		case MUSIC_FMT_FLAC:
			drflac_close(flac);
			flac = NULL;
			break;
		default:
			break;
	}
	curFormat = MUSIC_FMT_NONE;
}

/* Tries to open 'path' with the decoder matching its extension.
   Fills srcChannels / srcSampleRate and sets curFormat on success. */
static qboolean Music_Open(const char *path)
{
	size_t len = strlen(path);

	if (len >= 5 && Q_strcasecmp(path + len - 5, ".flac") == 0)
	{
		flac = drflac_open_file(path, NULL);
		if (!flac)
			return false;

		curFormat     = MUSIC_FMT_FLAC;
		srcChannels   = (int) flac->channels;
		srcSampleRate = flac->sampleRate;
		return true;
	}

	/* default: MP3 */
	if (!drmp3_init_file(&mp3, path, NULL))
		return false;

	curFormat     = MUSIC_FMT_MP3;
	srcChannels   = (int) mp3.channels;
	srcSampleRate = mp3.sampleRate;
	return true;
}

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

/* Resolves the file for a given track number, trying in order:
   trackNN.mp3, NN.mp3, trackNN.flac, NN.flac (first match wins - MP3
   is tried first purely for backward compatibility with existing
   music folders; swap the order below if you'd rather prefer FLAC
   when both exist). Returns true and fills dst with the path that was
   actually found; returns false if none of the four forms exist (dst
   is still filled with the trackNN.mp3 form, handy for the "file not
   found" log message). */
static qboolean CD_TrackPath(char *dst, size_t dstSize, int track)
{
	static const char *patterns[] = {
		"%s/%s/track%02d.mp3",
		"%s/%s/%02d.mp3",
		"%s/%s/track%02d.flac",
		"%s/%s/%02d.flac",
	};
	int i;

	CD_EnsureMusicDir();

	Com_sprintf(dst, dstSize, patterns[0], FS_Gamedir(), cd_musicdir->string, track);

	for (i = 0; i < (int)(sizeof(patterns) / sizeof(patterns[0])); i++)
	{
		char candidate[MAX_OSPATH];
		Com_sprintf(candidate, sizeof(candidate), patterns[i],
			FS_Gamedir(), cd_musicdir->string, track);
		if (access(candidate, F_OK) == 0)
		{
			Com_sprintf(dst, dstSize, "%s", candidate);
			return true;
		}
	}

	return false;
}

/* Scans the music directory for files matching "trackNN.mp3",
   "NN.mp3", "trackNN.flac" or "NN.flac" and collects the track
   numbers found into 'tracks' (deduplicated - if several forms exist
   for the same number it's only counted once). Tracks below
   cd_mintrack are skipped (default 2, to skip a would-be "data track"
   the way real Quake2 CDs do). Returns the number of tracks found. */
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
		else if (sscanf(entry->d_name, "track%d.flac%n", &track, &chars) == 1 &&
			(size_t)chars == nameLen)
			matched = true;
		else if (sscanf(entry->d_name, "%d.flac%n", &track, &chars) == 1 &&
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

/* Compacts already-consumed frames out of musicrs.buf and decodes
   more source PCM to top it back up. Handles looping transparently:
   if the decoder runs dry and playLooping is set, it seeks back to
   frame 0 and keeps filling. Leaves musicrs.eof set once there is
   truly nothing left to decode (non-looping track that reached its
   end). Format-agnostic: goes through Music_ReadFrames/SeekToStart. */
static void Music_Refill(void)
{
	int keep;
	drmp3_uint64 framesRead;

	if (musicrs.eof)
		return;

	keep = (int) musicrs.pos;
	if (keep > 0 && keep < musicrs.avail)
	{
		int remain = musicrs.avail - keep;
		memmove(musicrs.buf, musicrs.buf + keep * srcChannels,
			remain * srcChannels * sizeof(drmp3_int16));
		musicrs.avail = remain;
		musicrs.pos  -= (float) keep;
	}
	else if (keep >= musicrs.avail)
	{
		musicrs.avail = 0;
		musicrs.pos   = 0.0f;
	}

	if (musicrs.avail >= MUSIC_SRC_BUF_FRAMES - 1)
		return; /* still enough buffered, nothing to do */

	framesRead = Music_ReadFrames(MUSIC_SRC_BUF_FRAMES - musicrs.avail,
		musicrs.buf + musicrs.avail * srcChannels);

	if (framesRead == 0)
	{
		if (playLooping)
		{
			Music_SeekToStart();
			framesRead = Music_ReadFrames(MUSIC_SRC_BUF_FRAMES - musicrs.avail,
				musicrs.buf + musicrs.avail * srcChannels);
		}

		if (framesRead == 0)
		{
			musicrs.eof = true;
			return;
		}
	}

	musicrs.avail += (int) framesRead;
}

/* Converts one already-resampled source-format sample (srcCh values)
   to the destination channel count (1 or 2). Anything other than a
   1<->2 conversion (shouldn't happen for MP3/FLAC in practice) falls
   back to silence rather than reading out of bounds. */
static void Music_RemapChannels(const float *src, int srcCh, float *dst, int destCh)
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
	int destCh;
	int frameBytes, framesWanted, i;

	if (!enabled || !playing || paused || !mp3Valid)
		return;

	if (dma.samplebits != 16)
		return;

	destCh = dma.channels;
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

		srcFrame = (int) musicrs.pos;

		if (srcFrame + 1 >= musicrs.avail)
		{
			break;
		}

		frac = musicrs.pos - (float) srcFrame;

		for (c = 0; c < srcChannels; c++)
		{
			drmp3_int16 a = musicrs.buf[srcFrame * srcChannels + c];
			drmp3_int16 b = musicrs.buf[(srcFrame + 1) * srcChannels + c];
			srcSample[c] = (float) a + (float) (b - a) * frac;
		}

		Music_RemapChannels(srcSample, srcChannels, outSample, destCh);

		for (c = 0; c < destCh; c++)
		{
			int mixed = dst[i * destCh + c] + (int) (outSample[c] * cdvolume);
			if (mixed > 32767) mixed = 32767;
			else if (mixed < -32768) mixed = -32768;
			dst[i * destCh + c] = (Sint16) mixed;
		}

		musicrs.pos += musicRatio;
	}
}

/* ------------------------------------------------------------------ */
/* Public API - same signatures as cd_linux.c / cd_sdl.c / cd_mp3.c   */
/* ------------------------------------------------------------------ */

void CDAudio_Play(int track, qboolean looping)
{
	char path[MAX_OSPATH];

	if (!enabled || !initialized)
		return;

	if (!CD_TrackPath(path, sizeof(path), track))
	{
		Com_DPrintf("CDAudio_Play: no file found for track %d "
			"(tried track%02d.mp3, %02d.mp3, track%02d.flac and "
			"%02d.flac in '%s')\n",
			track, track, track, track, track, cd_musicdir->string);
		return;
	}

	SDL_LockAudio();

	if (mp3Valid)
	{
		Music_Close();
		mp3Valid = false;
	}
	playing = false;

	if (!Music_Open(path))
	{
		SDL_UnlockAudio();
		Com_DPrintf("CDAudio_Play: could not open %s\n", path);
		return;
	}

	if (srcChannels < 1 || srcChannels > 2)
	{
		Com_Printf("CDAudio_Play: %s has %d channels, only mono/stereo "
			"is supported.\n", path, srcChannels);
		Music_Close();
		SDL_UnlockAudio();
		return;
	}

	musicRatio = (float) srcSampleRate / (float) dma.speed;
	memset(&musicrs, 0, sizeof(musicrs));

	Com_DPrintf("CDAudio_Play: %s (%s) is %u Hz / %d channels, "
		"converting to %d Hz / %d channels on the fly.\n",
		path, curFormat == MUSIC_FMT_FLAC ? "flac" : "mp3",
		srcSampleRate, srcChannels, dma.speed, dma.channels);

	mp3Valid     = true;
	playLooping  = looping;
	paused       = false;
	currentTrack = track;
	playing      = true;

	SDL_UnlockAudio();
}

/* Picks a random track among the music files actually present in the
   music directory and plays it in a loop. Replaces walking the real
   CD's track table. */
void CDAudio_RandomPlay(void)
{
	int tracks[MUSIC_MAX_TRACKS];
	int count, idx;

	if (!enabled || !initialized)
		return;

	count = CD_ScanMusicDir(tracks, MUSIC_MAX_TRACKS);
	if (count == 0)
	{
		Com_DPrintf("CDAudio_RandomPlay: no track*.mp3 / *.mp3 / "
			"track*.flac / *.flac files found in '%s'\n", cd_musicdir->string);
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
	Music_Close();
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
		if (musicrs.avail - (int) musicrs.pos < MUSIC_SRC_BUF_FRAMES / 4)
			Music_Refill();
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

	Com_Printf("CD Audio (mp3/flac) initialized, directory '%s'.\n", cd_musicdir->string);
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
			Com_Printf("Currently playing: track %d, %s (%s)\n",
				currentTrack, curFormat == MUSIC_FMT_FLAC ? "flac" : "mp3",
				playLooping ? "looping" : "once");
		else
			Com_Printf("No track currently playing.\n");
		return;
	}
}

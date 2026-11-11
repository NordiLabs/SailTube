// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Pure audio-track policy: ranking of audio-only formats (SEL-04) and the dub
// picker with its remembered language (SEL-05). Inputs are formats the caller
// already found playable (StreamSelection's playableAudioFormats()).
//
// Language classes follow yt-dlp's language_preference numbers, carried in
// MediaFormat::languagePreference: original +10, default +5, dub -1,
// descriptive -10 (and -1 when the field is absent).

#include "MediaFormat.h"
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace media {

// SEL-04: true when a is the better audio track. Keys, each breaking only the
// ties left by the one before: (1) language preference, higher first
// (original/default, then dubs, then descriptive); (2) Opus before AAC, even
// at a lower bitrate; (3) non-DRC before DRC; (4) bitrate, higher first.
// Equal on every key falls back to the format id, so the order is total.
bool audioRanksBefore(const MediaFormat &a, const MediaFormat &b);

// SEL-04: the formats sorted best first (stable on the keys above).
std::vector<MediaFormat> rankAudio(std::vector<MediaFormat> formats);

// SEL-04: true for a described-video (audio description) track.
bool descriptiveTrack(const MediaFormat &);

// SEL-05: lower-case, '_' to '-' ("pt_BR" -> "pt-br"); base is the part
// before the first '-' ("pt-br" -> "pt").
std::string normalizedLanguage(const std::string &);
std::string baseLanguage(const std::string &);

// SEL-04 + SEL-05: the best track for a remembered language preference.
// Empty preference = "no preference" = the original/default track. Otherwise
// an exact BCP-47 match first, then the base language, then the original.
// Descriptive tracks never match a remembered language.
// nullptr only when the pool is empty.
const MediaFormat *bestAudioTrack(const std::vector<MediaFormat> &playableAudio,
                                  const std::string &preferredLanguage);

// SEL-05: one dub-picker entry per language (descriptive tracks of a language
// get their own entry, marked). The original comes first and is marked.
struct AudioTrackOption {
    std::string language;               // normalized BCP-47 tag, may be empty
    std::string formatId;               // best track of this language (SEL-04)
    std::vector<std::string> formatIds; // every track of this entry, to match the playing one
    bool original = false;
    bool descriptive = false;
    std::string label;                  // language tag plus " (original)" / " (described)"
};

// SEL-05: entries ordered original first, then dubs by preference and tag,
// then descriptive. The UI shows the picker only when there is more than one
// entry and the play plan has a separate audio branch.
std::vector<AudioTrackOption> audioTrackOptions(const std::vector<MediaFormat> &playableAudio);

// SEL-05: index of the entry holding the given (playing) audio format id, -1 if none.
int audioTrackIndex(const std::vector<AudioTrackOption> &, const std::string &formatId);

// Stable volume (the player's "stable volume" switch, SEL-04 key 3 on request): the best track of
// one dub-picker entry, i.e. the tracks whose normalizedLanguage() equals `language` and whose
// descriptive class equals `descriptive`. preferDrc: a DRC ("stable volume") track wins over every
// non-DRC one when the entry has one; otherwise the SEL-04 order (non-DRC first). Then tracks of
// `codec` come first (AudioCodec::Unknown = any codec), then the SEL-04 keys. nullptr when the
// entry has no track in the pool.
const MediaFormat *bestEntryTrack(const std::vector<MediaFormat> &playableAudio, const std::string &language,
                                  bool descriptive, bool preferDrc, AudioCodec codec = AudioCodec::Unknown);

// SEL-05: the language preference to store after the user picks an entry.
// Picking the original stores "" (no preference), never its language, so a
// later video in another original language does not start dubbed.
std::string storedLanguagePreference(const AudioTrackOption &chosen);

}}}

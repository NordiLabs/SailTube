// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AudioTrackPolicy.h"

#include <algorithm>

namespace nordtile { namespace video { namespace media {

namespace {

const int originalPreference = 10;    // yt-dlp: original track
const int descriptivePreference = -10; // yt-dlp: audio description

int codecRank(AudioCodec c) { return c == AudioCodec::Opus ? 0 : c == AudioCodec::Aac ? 1 : 2; }

double kbps(const MediaFormat &f) { return f.audioKbps > 0 ? f.audioKbps : f.totalKbps; }

struct Entry {
    AudioTrackOption option;
    const MediaFormat *best = nullptr;
};

} // namespace

bool descriptiveTrack(const MediaFormat &f) { return f.languagePreference <= descriptivePreference; }

bool audioRanksBefore(const MediaFormat &a, const MediaFormat &b) {
    if (a.languagePreference != b.languagePreference) return a.languagePreference > b.languagePreference;
    const int ra = codecRank(a.audioCodec), rb = codecRank(b.audioCodec);
    if (ra != rb) return ra < rb;
    if (a.dynamicRangeCompressed != b.dynamicRangeCompressed) return !a.dynamicRangeCompressed;
    const double ka = kbps(a), kb = kbps(b);
    if (ka != kb) return ka > kb;
    return a.id < b.id;
}

std::vector<MediaFormat> rankAudio(std::vector<MediaFormat> formats) {
    std::stable_sort(formats.begin(), formats.end(), audioRanksBefore);
    return formats;
}

std::string normalizedLanguage(const std::string &tag) {
    std::string out(tag);
    for (char &c : out) {
        if (c == '_') c = '-';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string baseLanguage(const std::string &tag) {
    const std::string n = normalizedLanguage(tag);
    return n.substr(0, n.find('-'));
}

const MediaFormat *bestAudioTrack(const std::vector<MediaFormat> &pool, const std::string &preferredLanguage) {
    const MediaFormat *best = nullptr, *exact = nullptr, *base = nullptr;
    const std::string want = normalizedLanguage(preferredLanguage);
    const std::string wantBase = baseLanguage(preferredLanguage);
    for (const MediaFormat &f : pool) {
        if (!best || audioRanksBefore(f, *best)) best = &f;
        // A remembered dub never selects an audio-description track.
        if (want.empty() || f.language.empty() || descriptiveTrack(f)) continue;
        const std::string have = normalizedLanguage(f.language);
        if (have == want && (!exact || audioRanksBefore(f, *exact))) exact = &f;
        if (baseLanguage(have) == wantBase && (!base || audioRanksBefore(f, *base))) base = &f;
    }
    if (exact) return exact;
    if (base) return base;
    return best;
}

std::vector<AudioTrackOption> audioTrackOptions(const std::vector<MediaFormat> &pool) {
    std::vector<Entry> entries;
    for (const MediaFormat &f : pool) {
        const std::string language = normalizedLanguage(f.language);
        const bool descriptive = descriptiveTrack(f);
        std::vector<Entry>::iterator it = entries.begin();
        while (it != entries.end() && !(it->option.language == language && it->option.descriptive == descriptive)) ++it;
        if (it == entries.end()) {
            Entry e;
            e.option.language = language;
            e.option.descriptive = descriptive;
            entries.push_back(e);
            it = entries.end() - 1;
        }
        it->option.formatIds.push_back(f.id);
        if (!it->best || audioRanksBefore(f, *it->best)) it->best = &f;
    }
    if (entries.empty()) return std::vector<AudioTrackOption>();

    // The original is the entry flagged by yt-dlp; without a flag, the top-ranked entry.
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.option.descriptive != b.option.descriptive) return !a.option.descriptive;
        if (a.best->languagePreference != b.best->languagePreference)
            return a.best->languagePreference > b.best->languagePreference;
        return a.option.language < b.option.language;
    });
    bool flagged = false;
    for (Entry &e : entries) {
        if (!e.option.descriptive && e.best->languagePreference >= originalPreference && !flagged) {
            e.option.original = true;
            flagged = true;
        }
    }
    if (!flagged && !entries.front().option.descriptive) entries.front().option.original = true;

    std::vector<AudioTrackOption> out;
    for (Entry &e : entries) {
        e.option.formatId = e.best->id;
        e.option.label = e.option.language.empty() ? std::string("default") : e.option.language;
        if (e.option.original) e.option.label += " (original)";
        if (e.option.descriptive) e.option.label += " (described)";
        out.push_back(e.option);
    }
    // Keep the original first even if a non-flagged entry outranked it.
    std::stable_partition(out.begin(), out.end(), [](const AudioTrackOption &o) { return o.original; });
    return out;
}

int audioTrackIndex(const std::vector<AudioTrackOption> &options, const std::string &formatId) {
    for (std::size_t i = 0; i < options.size(); ++i)
        if (std::find(options[i].formatIds.begin(), options[i].formatIds.end(), formatId) != options[i].formatIds.end())
            return static_cast<int>(i);
    return -1;
}

const MediaFormat *bestEntryTrack(const std::vector<MediaFormat> &pool, const std::string &language, bool descriptive,
                                  bool preferDrc, AudioCodec codec) {
    const std::string want = normalizedLanguage(language);
    std::vector<const MediaFormat *> entry;
    bool anyDrc = false;
    for (const MediaFormat &f : pool) {
        if (normalizedLanguage(f.language) != want || descriptiveTrack(f) != descriptive) continue;
        entry.push_back(&f);
        anyDrc = anyDrc || f.dynamicRangeCompressed;
    }
    // Stable volume first (when asked and offered), then the codec, then SEL-04.
    if (preferDrc && anyDrc)
        entry.erase(std::remove_if(entry.begin(), entry.end(), [](const MediaFormat *f) { return !f->dynamicRangeCompressed; }),
                    entry.end());
    if (codec != AudioCodec::Unknown &&
        std::any_of(entry.begin(), entry.end(), [codec](const MediaFormat *f) { return f->audioCodec == codec; }))
        entry.erase(std::remove_if(entry.begin(), entry.end(), [codec](const MediaFormat *f) { return f->audioCodec != codec; }),
                    entry.end());
    const MediaFormat *best = nullptr;
    for (const MediaFormat *f : entry)
        if (!best || audioRanksBefore(*f, *best)) best = f;
    return best;
}

std::string storedLanguagePreference(const AudioTrackOption &chosen) {
    return chosen.original ? std::string() : chosen.language;
}

}}}

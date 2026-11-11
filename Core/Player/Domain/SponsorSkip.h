// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// SB-02 SponsorBlock skip decisions. Qt-free.
//
// Call evaluate() on every position update. It never seeks itself: a
// returned seek is a DEFERRED seek that the caller posts to the next
// event-loop turn (QTimer::singleShot(0, ...) / Qt.callLater is not in Qt
// 5.6), never executed inside the position-change handler, which caused a
// binding loop in Qt 5.6 (SB-02, PLT-05).
//
// The single table sponsorCategories() is the source of the UI rows, the
// defaults and the fetched category list (NFR-TST-03: every fetched category
// has a UI row).

#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace player {

enum class SponsorCategory {
    Sponsor, SelfPromo, Interaction, Intro, Outro, Preview, Filler, MusicOfftopic,
    PoiHighlight // "jump to highlight" (SHOULD): a point, never auto-skipped
};
const int kSponsorCategoryCount = 9;

enum class SkipMode { Off, AutoSkip, ShowButton };

struct SponsorCategoryInfo {
    SponsorCategory category;
    const char *apiName;     // SponsorBlock API name, also the settings key suffix
    const char *label;       // English fallback; QML shows a qsTr() label keyed by apiName
    SkipMode defaultMode;    // SB-02 defaults
    bool highlight;          // poi_highlight: offers "jump to highlight", no AutoSkip
};

// SB-02: every category, in UI order. Defaults: AutoSkip for sponsor,
// selfpromo, interaction; Off for the rest.
const std::vector<SponsorCategoryInfo> &sponsorCategories();
// SB-01: the names to request (categories=[...]); derived from the table.
std::vector<std::string> sponsorFetchCategories();
const SponsorCategoryInfo &sponsorCategoryInfo(SponsorCategory category);
bool sponsorCategoryFromApiName(const std::string &apiName, SponsorCategory *out);
SkipMode defaultSkipMode(SponsorCategory category);

const bool kSponsorBlockEnabledDefault = true; // SB-02 master switch

struct SponsorSettings {
    bool enabled = kSponsorBlockEnabledDefault;
    SkipMode modes[kSponsorCategoryCount];
    SponsorSettings(); // the defaults table
    SkipMode mode(SponsorCategory c) const { return modes[static_cast<int>(c)]; }
    void setMode(SponsorCategory c, SkipMode m) { modes[static_cast<int>(c)] = m; }
};

struct SponsorSegment {
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;  // == startMs for poi_highlight
    SponsorCategory category = SponsorCategory::Sponsor;
    SponsorSegment() {}
    SponsorSegment(std::int64_t s, std::int64_t e, SponsorCategory c) : startMs(s), endMs(e), category(c) {}
};

struct SkipDecision {
    // Auto-skip: post a DEFERRED seek to seekToMs (see the header comment).
    bool seek = false;
    std::int64_t seekToMs = 0;
    // Notice naming the skipped category, shown for noticeMs (1.6 s).
    bool notice = false;
    SponsorCategory noticeCategory = SponsorCategory::Sponsor;
    int noticeMs = 0;
    // Manual "skip <category>" button.
    bool showButton = false;
    SponsorCategory buttonCategory = SponsorCategory::Sponsor;
    std::int64_t buttonTargetMs = 0;
    // "Jump to highlight" (poi_highlight with mode != Off).
    bool showHighlight = false;
    std::int64_t highlightMs = 0;
};

class SponsorSkipper {
public:
    static const int kEarlyEndMs = 500;  // SB-02: a segment ends 0.5 s early (no skip loops)
    static const int kNoticeMs = 1600;   // SB-02: 1.6 s notice

    void setSettings(const SponsorSettings &settings) { m_settings = settings; }
    const SponsorSettings &settings() const { return m_settings; }
    // New segments (new video or reclaim). Re-arms everything.
    void setSegments(const std::vector<SponsorSegment> &segments);
    void clear() { setSegments(std::vector<SponsorSegment>()); }

    // SB-02 decision for a position update.
    // - nothing unless owner (PLY-10) and the master switch is on;
    // - inside = start <= position < end - 0.5 s;
    // - AutoSkip fires only while playing, once per pass: a segment re-arms
    //   when the position goes back before its start (the user sought back);
    //   overlapping/adjacent auto segments are chained into one seek;
    // - AutoSkip beats a button: inside an armed auto segment (even paused)
    //   no button is shown;
    // - a ShowButton segment shows the manual button (the longest one when
    //   several overlap). An auto segment already skipped this pass shows
    //   nothing, also while the deferred seek is still landing.
    SkipDecision evaluate(std::int64_t positionMs, bool playing, bool owner);

    // The manual button was tapped: the deferred seek target, or -1 when no
    // button applies at this position. Marks the segment as passed.
    std::int64_t manualSkip(std::int64_t positionMs, bool owner);

private:
    bool inside(const SponsorSegment &s, std::int64_t positionMs) const;
    int buttonSegment(std::int64_t positionMs) const;

    SponsorSettings m_settings;
    std::vector<SponsorSegment> m_segments;
    std::vector<bool> m_fired;
};

} } // namespace nordtile::player

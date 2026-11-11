// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SponsorSkip.h"

namespace nordtile { namespace player {

const int SponsorSkipper::kEarlyEndMs;
const int SponsorSkipper::kNoticeMs;

static std::vector<SponsorCategoryInfo> buildTable() {
    // The one defaults table (SET-03). Order = UI order.
    static const SponsorCategoryInfo rows[] = {
        { SponsorCategory::Sponsor, "sponsor", "Sponsor", SkipMode::AutoSkip, false },
        { SponsorCategory::SelfPromo, "selfpromo", "Unpaid/self promotion", SkipMode::AutoSkip, false },
        { SponsorCategory::Interaction, "interaction", "Interaction reminder", SkipMode::AutoSkip, false },
        { SponsorCategory::Intro, "intro", "Intermission/intro", SkipMode::Off, false },
        { SponsorCategory::Outro, "outro", "Endcards/credits", SkipMode::Off, false },
        { SponsorCategory::Preview, "preview", "Preview/recap", SkipMode::Off, false },
        { SponsorCategory::Filler, "filler", "Filler tangent", SkipMode::Off, false },
        { SponsorCategory::MusicOfftopic, "music_offtopic", "Non-music section", SkipMode::Off, false },
        { SponsorCategory::PoiHighlight, "poi_highlight", "Highlight", SkipMode::Off, true },
    };
    return std::vector<SponsorCategoryInfo>(rows, rows + sizeof(rows) / sizeof(rows[0]));
}

const std::vector<SponsorCategoryInfo> &sponsorCategories() {
    static const std::vector<SponsorCategoryInfo> table = buildTable();
    return table;
}

std::vector<std::string> sponsorFetchCategories() {
    std::vector<std::string> names;
    const std::vector<SponsorCategoryInfo> &t = sponsorCategories();
    for (std::size_t i = 0; i < t.size(); ++i) names.push_back(t[i].apiName);
    return names;
}

const SponsorCategoryInfo &sponsorCategoryInfo(SponsorCategory category) {
    const std::vector<SponsorCategoryInfo> &t = sponsorCategories();
    for (std::size_t i = 0; i < t.size(); ++i)
        if (t[i].category == category) return t[i];
    return t[0]; // unreachable: the table covers every enum value (tested)
}

bool sponsorCategoryFromApiName(const std::string &apiName, SponsorCategory *out) {
    const std::vector<SponsorCategoryInfo> &t = sponsorCategories();
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (apiName == t[i].apiName) {
            if (out) *out = t[i].category;
            return true;
        }
    }
    return false;
}

SkipMode defaultSkipMode(SponsorCategory category) { return sponsorCategoryInfo(category).defaultMode; }

SponsorSettings::SponsorSettings() {
    for (int i = 0; i < kSponsorCategoryCount; ++i) modes[i] = SkipMode::Off;
    const std::vector<SponsorCategoryInfo> &t = sponsorCategories();
    for (std::size_t i = 0; i < t.size(); ++i) modes[static_cast<int>(t[i].category)] = t[i].defaultMode;
}

void SponsorSkipper::setSegments(const std::vector<SponsorSegment> &segments) {
    m_segments = segments;
    m_fired.assign(m_segments.size(), false);
}

bool SponsorSkipper::inside(const SponsorSegment &s, std::int64_t pos) const {
    return pos >= s.startMs && pos < s.endMs - kEarlyEndMs;
}

int SponsorSkipper::buttonSegment(std::int64_t pos) const {
    int best = -1;
    for (std::size_t i = 0; i < m_segments.size(); ++i) {
        const SponsorSegment &s = m_segments[i];
        if (sponsorCategoryInfo(s.category).highlight || !inside(s, pos)) continue;
        if (m_settings.mode(s.category) != SkipMode::ShowButton) continue;
        if (best < 0 || s.endMs > m_segments[best].endMs) best = static_cast<int>(i);
    }
    return best;
}

SkipDecision SponsorSkipper::evaluate(std::int64_t pos, bool playing, bool owner) {
    SkipDecision d;
    if (!owner || !m_settings.enabled) return d;

    for (std::size_t i = 0; i < m_segments.size(); ++i)
        if (m_fired[i] && pos < m_segments[i].startMs) m_fired[i] = false; // sought back: re-arm

    int autoIdx = -1;
    for (std::size_t i = 0; i < m_segments.size(); ++i) {
        const SponsorSegment &s = m_segments[i];
        if (sponsorCategoryInfo(s.category).highlight) continue;
        if (m_settings.mode(s.category) == SkipMode::AutoSkip && !m_fired[i] && inside(s, pos)) {
            autoIdx = static_cast<int>(i);
            break;
        }
    }

    if (autoIdx >= 0) {
        if (playing) {
            std::int64_t target = m_segments[autoIdx].endMs;
            m_fired[autoIdx] = true;
            bool extended = true;
            while (extended) { // chain overlapping auto segments into one seek
                extended = false;
                for (std::size_t i = 0; i < m_segments.size(); ++i) {
                    const SponsorSegment &s = m_segments[i];
                    if (m_fired[i] || sponsorCategoryInfo(s.category).highlight) continue;
                    if (m_settings.mode(s.category) != SkipMode::AutoSkip || !inside(s, target)) continue;
                    m_fired[i] = true;
                    if (s.endMs > target) target = s.endMs;
                    extended = true;
                }
            }
            d.seek = true;
            d.seekToMs = target;
            d.notice = true;
            d.noticeCategory = m_segments[autoIdx].category;
            d.noticeMs = kNoticeMs;
        }
        // Paused inside an armed auto segment: it fires on play; no button.
    } else {
        int b = buttonSegment(pos);
        if (b >= 0) {
            d.showButton = true;
            d.buttonCategory = m_segments[b].category;
            d.buttonTargetMs = m_segments[b].endMs;
        }
    }

    if (m_settings.mode(SponsorCategory::PoiHighlight) != SkipMode::Off) {
        for (std::size_t i = 0; i < m_segments.size(); ++i) {
            const SponsorSegment &s = m_segments[i];
            if (s.category == SponsorCategory::PoiHighlight && pos < s.startMs) {
                d.showHighlight = true;
                d.highlightMs = s.startMs;
                break;
            }
        }
    }
    return d;
}

std::int64_t SponsorSkipper::manualSkip(std::int64_t pos, bool owner) {
    if (!owner || !m_settings.enabled) return -1;
    int b = buttonSegment(pos);
    if (b < 0) return -1;
    m_fired[b] = true;
    return m_segments[b].endMs;
}

} } // namespace nordtile::player

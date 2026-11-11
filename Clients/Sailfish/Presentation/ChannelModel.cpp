// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ChannelModel.h"
#include "Core/Catalog/Domain/LocalizedText.h"
#include <QDateTime>
#include "WatchedRows.h"
#include "ChannelAvatars.h"
#include "LargeThumbnails.h"
#include "Core/Catalog/Domain/ChannelReference.h"
#include "Core/Catalog/Infrastructure/ChannelService.h"
#include "Core/Catalog/Infrastructure/ShortsStore.h"

#include <QCoreApplication>
#include <set>

namespace nordtile { namespace video { namespace sailfish {

using catalog::ChannelPage;
using catalog::Item;
using catalog::ItemKind;
using catalog::Outcome;
using catalog::ShortEvidence;

namespace {

QPointer<catalog::ChannelService> &defaultService() {
    static QPointer<catalog::ChannelService> service;
    return service;
}

const int kMaxAutoPages = 5;

std::string rowKey(const Item &item) {
    const int kind = static_cast<int>(item.kind == ItemKind::Short ? ItemKind::Video : item.kind);
    return std::to_string(kind) + ":" + item.id;
}

QString kindName(ItemKind kind) {
    switch (kind) {
    case ItemKind::Video: return QStringLiteral("video");
    case ItemKind::Short: return QStringLiteral("short");
    case ItemKind::Channel: return QStringLiteral("channel");
    case ItemKind::Playlist: return QStringLiteral("playlist");
    }
    return QString();
}

}  // namespace

QString catalogOutcomeText(Outcome outcome) {
    switch (outcome) {
    case Outcome::Ok:
    case Outcome::ValidEmpty: return QString();
    case Outcome::ShapeDrift: return QCoreApplication::translate("SailTubeCatalog", "YouTube changed this page. SailTube cannot read it yet.");
    case Outcome::Http4xx: return QCoreApplication::translate("SailTubeCatalog", "YouTube refused the request. Try again later.");
    case Outcome::RateLimited: return QCoreApplication::translate("SailTubeCatalog", "YouTube is limiting requests. Try again later.");
    case Outcome::Http5xx: return QCoreApplication::translate("SailTubeCatalog", "YouTube is not responding. Try again later.");
    case Outcome::Network: return QCoreApplication::translate("SailTubeCatalog", "YouTube could not be reached. Check the connection.");
    case Outcome::Unavailable: return QCoreApplication::translate("SailTubeCatalog", "This is not available on YouTube.");
    }
    return QString();
}

QString catalogDurationText(const Item &item) {
    const std::int64_t seconds = item.durationSeconds;
    if (seconds < 0) return QString::fromStdString(item.durationText);
    const std::int64_t h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

void ChannelModel::setDefaultService(catalog::ChannelService *service) { defaultService() = service; }

ChannelModel::ChannelModel(QObject *parent) : ChannelModel(defaultService().data(), parent) {}

ChannelModel::ChannelModel(catalog::ChannelService *service, QObject *parent)
    : QAbstractListModel(parent), service_(service), tab_(QStringLiteral("videos")) {
    if (service && service->shorts())
        storeConnection_ = connect(service->shorts(), &catalog::ShortsStore::changed, this, [this] { sync(); });
    connect(WatchedRows::instance(), &WatchedRows::changed, this, [this] { sync(); });   // LIB-07
    ChannelAvatars::follow(this, ChannelAvatarRole);
    LargeThumbnails::follow(this, ThumbnailLargeRole);
}

// ---- list model ----------------------------------------------------------------------------

int ChannelModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

QVariant ChannelModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(visible_.size())) return QVariant();
    const Item &v = visible_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ItemIdRole: return QString::fromStdString(v.id);
    case VideoIdRole: return catalog::isVideoLike(v) ? QString::fromStdString(v.id) : QString();
    case KindRole: return kindName(v.kind);
    case TitleRole: return QString::fromStdString(v.title);
    case ChannelRole: return QString::fromStdString(v.channel);
    case ChannelIdRole: return QString::fromStdString(v.channelId);
    case DurationRole: return v.liveStatus == media::LiveStatus::IsLive ? QString() : catalogDurationText(v);
    case DurationSecondsRole: return static_cast<double>(v.durationSeconds);
    case ViewCountRole: return static_cast<double>(v.viewCount);
    case ViewTextRole: return QString::fromStdString(v.viewText);
    case PublishedRole:
        // YouTube's own text when the row has one ("hace 3 días"), else the age of an RSS row in
        // the same language.
        if (!v.publishedText.empty() || v.publishedEpoch < 0) return QString::fromStdString(v.publishedText);
        return QString::fromStdString(catalog::formatRelativeAge(
            qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() / 1000 - v.publishedEpoch), catalog::displayLanguage()));
    case ThumbnailRole: return QString::fromStdString(v.thumbnailUrl);
    case LiveRole: return v.liveStatus == media::LiveStatus::IsLive;
    case UpcomingRole: return v.liveStatus == media::LiveStatus::Upcoming;
    case MembersOnlyRole: return v.membersOnly;
    case RestrictedRole: return v.restricted;
    case ShortRole: return catalog::isConfirmedShort(v.shortEvidence);
    case ShortCandidateRole: return v.shortEvidence == ShortEvidence::Candidate;
    case VideoCountRole: return static_cast<double>(v.videoCount);
    case VideoCountTextRole: return QString::fromStdString(v.videoCountText);
    case PlaylistIdRole: return v.kind == ItemKind::Playlist ? QString::fromStdString(v.id) : QString::fromStdString(v.playlistId);
    case ThumbnailLargeRole: return LargeThumbnails::forThumbnail(v.thumbnailUrl);
    case ChannelAvatarRole: {
        const QString own = ChannelAvatars::forRow(v);
        // The rows of a channel page are the channel's: its header avatar stands in.
        return own.isEmpty() && (v.channelId.empty() || QString::fromStdString(v.channelId) == channelId_) ? avatarUrl_ : own;
    }
    default: return QVariant();
    }
}

QHash<int, QByteArray> ChannelModel::roleNames() const {
    QHash<int, QByteArray> r;
    r.insert(ItemIdRole, "itemId");
    r.insert(VideoIdRole, "videoId");
    r.insert(KindRole, "kind");
    r.insert(TitleRole, "title");
    r.insert(ChannelRole, "channel");
    r.insert(ChannelIdRole, "channelId");
    r.insert(DurationRole, "duration");
    r.insert(DurationSecondsRole, "durationSeconds");
    r.insert(ViewCountRole, "viewCount");
    r.insert(ViewTextRole, "viewText");
    r.insert(PublishedRole, "published");
    r.insert(ThumbnailRole, "thumbnail");
    r.insert(LiveRole, "live");
    r.insert(UpcomingRole, "upcoming");
    r.insert(MembersOnlyRole, "membersOnly");
    r.insert(RestrictedRole, "restricted");
    r.insert(ShortRole, "isShort");
    r.insert(ShortCandidateRole, "shortCandidate");
    r.insert(VideoCountRole, "videoCount");
    r.insert(VideoCountTextRole, "videoCountText");
    r.insert(PlaylistIdRole, "playlistId");
    r.insert(ChannelAvatarRole, "channelAvatar");
    r.insert(ThumbnailLargeRole, "thumbnailLarge");
    return r;
}

QVariantMap ChannelModel::get(int row) const {
    QVariantMap m;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(index(row, 0), it.key()));
    return m;
}

// ---- state ---------------------------------------------------------------------------------

QString ChannelModel::normalizeTab(const QString &tab) { return catalog::ChannelService::normalizeTab(tab); }

bool ChannelModel::supportedTab(const QString &tab) {
    static const QStringList tabs = QStringList() << QStringLiteral("videos") << QStringLiteral("shorts")
                                                  << QStringLiteral("streams") << QStringLiteral("playlists")
                                                  << QStringLiteral("releases");
    return tabs.contains(tab);
}

const ChannelModel::TabState *ChannelModel::currentState() const {
    auto it = states_.constFind(tab_);
    return it == states_.constEnd() ? nullptr : &it.value();
}

bool ChannelModel::busy() const {
    if (resolving_) return true;
    const TabState *s = currentState();
    return s && s->loading;
}

bool ChannelModel::canLoadMore() const {
    const TabState *s = currentState();
    return s && !s->loading && !s->continuation.isEmpty() && !channelId_.isEmpty();
}

QString ChannelModel::errorText() const {
    if (catalog::isFailure(resolveOutcome_)) return catalogOutcomeText(resolveOutcome_);
    const TabState *s = currentState();
    return s ? catalogOutcomeText(s->outcome) : QString();
}

QString ChannelModel::outcome() const {
    if (catalog::isFailure(resolveOutcome_)) return QString::fromLatin1(catalog::outcomeName(resolveOutcome_));
    const TabState *s = currentState();
    return s ? QString::fromLatin1(catalog::outcomeName(s->outcome)) : QString();
}

bool ChannelModel::empty() const {
    const TabState *s = currentState();
    return s && s->loaded && !s->loading && s->rows.empty() && (s->outcome == Outcome::ValidEmpty || s->missing);
}

bool ChannelModel::rssPainted() const {
    auto it = states_.constFind(QStringLiteral("videos"));
    return it != states_.constEnd() && it.value().rssPainted;
}

void ChannelModel::setHideShorts(bool on) {
    if (settings_.hideShorts == on) return;
    settings_.hideShorts = on;
    emit settingsChanged();
    sync();
}

void ChannelModel::setHideMembersOnly(bool on) {
    if (settings_.hideMembersOnly == on) return;
    settings_.hideMembersOnly = on;
    emit settingsChanged();
    sync();
}

void ChannelModel::setHideRestricted(bool on) {
    if (settings_.hideRestricted == on) return;
    settings_.hideRestricted = on;
    emit settingsChanged();
    sync();
}

void ChannelModel::reset() {
    ++generation_;
    beginResetModel();
    visible_.clear();
    endResetModel();
    states_.clear();
    hidden_ = catalog::RowFilterResult();
    channelId_.clear();
    title_.clear(); handle_.clear(); avatarUrl_.clear(); subscriberText_.clear(); videoCountText_.clear(); description_.clear();
    subscriberCount_ = -1;
    videoCount_ = -1;
    tabs_.clear();
    fallbackTab_ = false;
    resolving_ = false;
    resolveOutcome_ = Outcome::Ok;
}

// ---- flow ----------------------------------------------------------------------------------

void ChannelModel::open(const QString &reference) {
    const QString ref = reference.trimmed();
    reset();
    reference_ = ref;
    const catalog::ChannelReference parsed = catalog::parseChannelReference(ref.toStdString());
    const QString tab = normalizeTab(QString::fromStdString(parsed.tab));
    tab_ = supportedTab(tab) ? tab : QStringLiteral("videos");
    emit referenceChanged();
    emit headerChanged();
    if (!service_ || ref.isEmpty()) {
        resolveOutcome_ = Outcome::Unavailable;
        emit stateChanged();
        return;
    }
    resolving_ = true;
    emit stateChanged();
    const quint64 generation = generation_;
    service_->resolve(ref, this, [this, generation](Outcome o, QString channelId) {
        if (generation != generation_) return;
        resolving_ = false;
        resolveOutcome_ = o;
        if (o != Outcome::Ok) { emit stateChanged(); return; }
        channelId_ = channelId;
        emit headerChanged();
        startTab(tab_);
        emit stateChanged();
    });
}

void ChannelModel::refresh() {
    if (reference_.isEmpty()) return;
    const QString tab = tab_;
    open(reference_);
    setTab(tab);
}

void ChannelModel::startTab(const QString &tab) {
    if (!service_ || channelId_.isEmpty()) return;
    TabState &s = state(tab);
    if (s.requested) return;
    s.requested = true;
    s.loading = true;
    const quint64 generation = generation_;
    if (tab == QLatin1String("videos") && service_->hasRss() && !s.rssRequested) {
        s.rssRequested = true;
        service_->rss(channelId_, this, [this, generation](Outcome o, std::vector<Item> rows) {
            onRss(generation, o, std::move(rows));
        });
    }
    service_->browseTab(channelId_, tab, this, [this, generation, tab](ChannelPage page) {
        onFirstPage(generation, tab, page);
    });
}

void ChannelModel::onRss(quint64 generation, Outcome outcome, std::vector<Item> rows) {
    if (generation != generation_ || outcome != Outcome::Ok) return;
    TabState &s = state(QStringLiteral("videos"));
    // RSS /shorts/ entries belong on the Shorts tab (their evidence is already stored).
    std::vector<Item> uploads;
    for (Item &row : rows) {
        if (catalog::isConfirmedShort(row.shortEvidence)) continue;
        if (row.channel.empty()) row.channel = title_.toStdString();
        if (row.channelId.empty()) row.channelId = channelId_.toStdString();
        uploads.push_back(row);
    }
    if (s.loaded) {
        // InnerTube was first: only enrich the rows already shown (exact dates and views).
        for (const Item &row : uploads) {
            auto it = s.index.find(rowKey(row));
            if (it == s.index.end()) continue;
            Item &shown = s.rows[it->second];
            if (shown.publishedEpoch < 0) shown.publishedEpoch = row.publishedEpoch;
        }
    } else {
        merge(s, uploads);
        s.rssPainted = !s.rows.empty();
    }
    if (tab_ == QLatin1String("videos")) sync();
    emit stateChanged();
}

void ChannelModel::applyHeader(const catalog::ChannelHeader &h) {
    if (h.isEmpty()) return;
    if (!h.title.isEmpty()) title_ = h.title;
    if (!h.handle.isEmpty()) handle_ = h.handle;
    if (!h.avatarUrl.isEmpty()) avatarUrl_ = h.avatarUrl;
    if (!h.subscriberText.isEmpty()) { subscriberText_ = h.subscriberText; subscriberCount_ = h.subscriberCount; }
    if (!h.videoCountText.isEmpty()) { videoCountText_ = h.videoCountText; videoCount_ = h.videoCount; }
    if (!h.description.isEmpty()) description_ = h.description;
    emit headerChanged();
}

void ChannelModel::applyTabs(const ChannelPage &page) {
    if (page.tabs.isEmpty()) return;
    static const QStringList order = QStringList() << QStringLiteral("videos") << QStringLiteral("shorts") << QStringLiteral("streams")
                                                   << QStringLiteral("playlists") << QStringLiteral("releases");
    QStringList tabs;
    for (const QString &key : order)
        if (page.hasTab(key)) tabs << key;
    tabs_ = tabs;
}

void ChannelModel::onFirstPage(quint64 generation, const QString &tab, const ChannelPage &page) {
    if (generation != generation_) return;
    TabState &s = state(tab);
    s.loading = false;
    s.loaded = true;
    applyHeader(page.header);
    applyTabs(page);
    if (page.tabMissing) {
        s.missing = true;
        s.outcome = Outcome::ValidEmpty;
        if (tab == QLatin1String("videos") && !s.rssPainted) {
            // A channel without a Videos tab opens its Playlists, else its Releases.
            const QString fallback = page.hasTab(QStringLiteral("playlists")) ? QStringLiteral("playlists")
                                   : page.hasTab(QStringLiteral("releases")) ? QStringLiteral("releases") : QString();
            if (!fallback.isEmpty()) {
                fallbackTab_ = true;
                tab_ = fallback;
                startTab(fallback);
            }
        }
        sync();
        emit stateChanged();
        return;
    }
    if (catalog::isFailure(page.outcome)) {
        s.outcome = page.outcome;     // RSS rows (if any) stay on screen
        sync();
        emit stateChanged();
        return;
    }
    merge(s, page.items);
    s.continuation = page.continuation;
    s.outcome = s.rows.empty() ? Outcome::ValidEmpty : Outcome::Ok;
    if (tab == tab_) sync();
    emit stateChanged();
}

void ChannelModel::setTab(const QString &name) {
    const QString t = normalizeTab(name);
    if (!supportedTab(t) || t == tab_) return;
    tab_ = t;
    sync();
    if (!channelId_.isEmpty()) startTab(t);
    emit stateChanged();
}

void ChannelModel::loadMore() {
    if (!canLoadMore() || !service_) return;
    TabState &s = state(tab_);
    s.loading = true;
    const quint64 generation = generation_;
    const QString tab = tab_;
    service_->browseMore(channelId_, tab, s.continuation, this, [this, generation, tab](ChannelPage page) {
        onMore(generation, tab, page);
    });
    emit stateChanged();
}

void ChannelModel::onMore(quint64 generation, const QString &tab, const ChannelPage &page) {
    if (generation != generation_) return;
    TabState &s = state(tab);
    s.loading = false;
    if (catalog::isFailure(page.outcome)) {
        // Shown inline; the token stays so a tap or scroll can retry (never automatically).
        s.outcome = page.outcome;
        s.autoPages = 0;
        emit stateChanged();
        return;
    }
    const int before = tab == tab_ ? count() : -1;
    merge(s, page.items);
    s.continuation = page.continuation;
    s.outcome = Outcome::Ok;
    if (tab != tab_) { emit stateChanged(); return; }
    sync();
    if (count() == before && !s.continuation.isEmpty() && s.autoPages < kMaxAutoPages) {
        ++s.autoPages;
        emit stateChanged();
        loadMore();
        return;
    }
    s.autoPages = 0;
    emit stateChanged();
}

void ChannelModel::merge(TabState &s, const std::vector<Item> &items) {
    for (const Item &item : items) {
        const std::string key = rowKey(item);
        auto it = s.index.find(key);
        if (it == s.index.end()) {
            s.index[key] = s.rows.size();
            s.rows.push_back(item);
            continue;
        }
        // Same row seen again (RSS first, then InnerTube): update in place and keep the
        // exact values RSS knows (timestamp, view count).
        Item &shown = s.rows[it->second];
        Item updated = item;
        if (updated.publishedEpoch < 0) updated.publishedEpoch = shown.publishedEpoch;
        if (shown.viewCount >= 0 && shown.viewText.empty()) { updated.viewCount = shown.viewCount; updated.viewText.clear(); }
        if (updated.thumbnailUrl.empty()) updated.thumbnailUrl = shown.thumbnailUrl;
        if (catalog::isConfirmedShort(shown.shortEvidence) && !catalog::isConfirmedShort(updated.shortEvidence)) {
            updated.shortEvidence = shown.shortEvidence;
            updated.kind = shown.kind;
        }
        shown = updated;
    }
}

void ChannelModel::sync() {
    const TabState *s = currentState();
    std::vector<Item> rows = s ? s->rows : std::vector<Item>();
    catalog::RowFilterSettings settings = settings_;
    if (tab_ == QLatin1String("shorts")) settings.hideShorts = false;   // the Shorts tab shows Shorts
    settings.hideWatched = WatchedRows::instance()->forContext(QStringLiteral("channel"));
    catalog::ShortsLookup lookup;
    catalog::ShortsStore *store = service_ ? service_->shorts() : nullptr;
    if (store) {
        store->knowledge().fillDurations(rows);
        lookup = store->lookup();
    }
    std::set<std::string> seen;
    catalog::RowFilterResult result = catalog::filterRows(rows, settings, &seen, lookup);
    std::vector<Item> next = std::move(result.visible);
    result.visible.clear();
    hidden_ = result;

    // Apply with stable positions: rows that stay keep their place (removed ones go, in place
    // updates emit dataChanged, new ones are appended). Anything else resets the list.
    std::set<std::string> nextKeys, oldKeys;
    for (const Item &i : next) nextKeys.insert(rowKey(i));
    for (const Item &i : visible_) oldKeys.insert(rowKey(i));
    std::vector<std::string> kept;
    for (const Item &i : visible_) if (nextKeys.count(rowKey(i))) kept.push_back(rowKey(i));
    bool stable = !visible_.empty() && kept.size() <= next.size();
    for (std::size_t k = 0; stable && k < kept.size(); ++k) stable = rowKey(next[k]) == kept[k];
    for (std::size_t k = kept.size(); stable && k < next.size(); ++k) stable = !oldKeys.count(rowKey(next[k]));
    if (!stable) {
        if (!visible_.empty() || !next.empty()) {
            beginResetModel();
            visible_ = next;
            endResetModel();
        }
        emit stateChanged();
        return;
    }
    for (int row = static_cast<int>(visible_.size()) - 1; row >= 0; --row) {
        if (nextKeys.count(rowKey(visible_[static_cast<std::size_t>(row)]))) continue;
        beginRemoveRows(QModelIndex(), row, row);
        visible_.erase(visible_.begin() + row);
        endRemoveRows();
    }
    for (std::size_t k = 0; k < visible_.size(); ++k) visible_[k] = next[k];
    if (!visible_.empty()) emit dataChanged(index(0, 0), index(static_cast<int>(visible_.size()) - 1, 0));
    if (next.size() > visible_.size()) {
        const int first = static_cast<int>(visible_.size());
        beginInsertRows(QModelIndex(), first, static_cast<int>(next.size()) - 1);
        visible_.insert(visible_.end(), next.begin() + first, next.end());
        endInsertRows();
    }
    emit stateChanged();
}

}}}

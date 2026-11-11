<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="ja">
    <context>
        <name>AboutPage</name>
        <message>
            <source>A native YouTube video client for Sailfish OS. Video data is resolved on the device by the packaged yt-dlp; no account is needed.</source>
            <extracomment>Short description on the about page</extracomment>
            <translation>Sailfish OS 向けのネイティブ YouTube 動画クライアントです。動画データは同梱の yt-dlp により端末上で取得されます。アカウントは不要です。</translation>
        </message>
        <message>
            <source>SailTube is offered under the PolyForm Noncommercial License 1.0.0. Bundled components keep their own licenses; their notices are installed in /usr/share/harbour-sailtube/licenses.</source>
            <extracomment>License notice on the about page</extracomment>
            <translation>SailTube は PolyForm Noncommercial License 1.0.0 の下で提供されています。同梱コンポーネントにはそれぞれのライセンスが適用され、その告知は /usr/share/harbour-sailtube/licenses にインストールされています。</translation>
        </message>
        <message>
            <source>SailTube is an independent project and is not affiliated with or endorsed by YouTube or Google.</source>
            <extracomment>Disclaimer on the about page</extracomment>
            <translation>SailTube は独立したプロジェクトであり、YouTube や Google とは提携しておらず、承認も受けていません。</translation>
        </message>
        <message>
            <source>About this beta</source>
            <extracomment>About page title in beta builds</extracomment>
            <translation>このベータ版について</translation>
        </message>
        <message>
            <source>Version</source>
            <extracomment>About page detail row label: the app version</extracomment>
            <translation>バージョン</translation>
        </message>
        <message>
            <source>Ends</source>
            <extracomment>About page detail row label: the date this beta stops working</extracomment>
            <translation>終了日</translation>
        </message>
        <message>
            <source>This is a test version. After the end date it opens only a page that lets you export a backup; your subscriptions, playlists, history and downloads are never deleted or changed by it.</source>
            <extracomment>About page paragraph in beta builds</extracomment>
            <translation>これはテスト版です。終了日を過ぎると、バックアップをエクスポートできるページだけが開きます。登録チャンネル、プレイリスト、履歴、ダウンロードが削除・変更されることはありません。</translation>
        </message>
        <message>
            <source>Updates: install the next beta or the stable version from the place you got this one. SailTube does not update itself and sends nothing about you or your use.</source>
            <extracomment>About page paragraph in beta builds</extracomment>
            <translation>アップデート: 次のベータ版または安定版を、このバージョンを入手した場所からインストールしてください。SailTube は自動で更新されず、あなたや利用状況に関する情報を一切送信しません。</translation>
        </message>
        <message>
            <source>Open the download page</source>
            <extracomment>About page button opening the download page in the browser</extracomment>
            <translation>ダウンロードページを開く</translation>
        </message>
    </context>
    <context>
        <name>AccountPage</name>
        <message>
            <source>Sign out of SailTube</source>
            <extracomment>Menu entry removing the imported session from SailTube (the browser stays signed in)</extracomment>
            <translation>SailTube からログアウト</translation>
        </message>
        <message>
            <source>Signing out</source>
            <extracomment>Remorse text while the session is being removed</extracomment>
            <translation>ログアウト中</translation>
        </message>
        <message>
            <source>Check now</source>
            <extracomment>Menu entry checking the imported session now</extracomment>
            <translation>今すぐ確認</translation>
        </message>
        <message>
            <source>YouTube account</source>
            <extracomment>Title of the account page</extracomment>
            <translation>YouTube アカウント</translation>
        </message>
        <message>
            <source>Import again</source>
            <extracomment>Button importing the session again after it expired</extracomment>
            <translation>もう一度インポート</translation>
        </message>
        <message>
            <source>Secure storage is not available, so the session is kept in a file only SailTube can read.</source>
            <extracomment>One-time notice: without the Sailfish Secrets service the session is kept in a private file</extracomment>
            <translation>セキュアストレージを利用できないため、セッションは SailTube だけが読み取れるファイルに保存されます。</translation>
        </message>
        <message>
            <source>OK</source>
            <extracomment>Button dismissing the storage notice</extracomment>
            <translation>OK</translation>
        </message>
        <message>
            <source>Channels</source>
            <extracomment>Section listing the logins and brand channels of the session</extracomment>
            <translation>チャンネル</translation>
        </message>
        <message>
            <source>Brand channel %1</source>
            <extracomment>Marks a brand channel in the channel list, %1 is the handle</extracomment>
            <translation>ブランドチャンネル %1</translation>
        </message>
        <message>
            <source>Sign in</source>
            <extracomment>Section listing the ways to sign in</extracomment>
            <translation>ログイン</translation>
        </message>
        <message>
            <source>Signing in is optional. Playback, search, the feed and downloads work without it.</source>
            <extracomment>Explains that signing in is optional</extracomment>
            <translation>ログインは任意です。再生、検索、フィード、ダウンロードはログインなしで使えます。</translation>
        </message>
        <message>
            <source>Import from the Browser</source>
            <extracomment>Sign-in method: copy the session from the Sailfish Browser</extracomment>
            <translation>ブラウザからインポート</translation>
        </message>
        <message>
            <source>Paste or load cookies</source>
            <extracomment>Sign-in method: paste cookies or load a cookies file</extracomment>
            <translation>Cookie を貼り付けまたは読み込み</translation>
        </message>
        <message>
            <source>Sign in at youtube.com in a normal Browser tab, not a private one, then import.</source>
            <extracomment>Hint of the Browser import</extracomment>
            <translation>プライベートタブではなく通常のブラウザタブで youtube.com にログインしてから、インポートしてください。</translation>
        </message>
        <message>
            <source>A Cookie header, copied request headers, a "Copy as cURL" command or a cookies.txt file.</source>
            <extracomment>Hint of the paste import</extracomment>
            <translation>Cookie ヘッダー、コピーしたリクエストヘッダー、「Copy as cURL」コマンド、または cookies.txt ファイル。</translation>
        </message>
    </context>
    <context>
        <name>AddToPlaylistDialog</name>
        <message>
            <source>Add to playlist</source>
            <extracomment>Title of the page choosing a playlist for a video</extracomment>
            <translation>再生リストに追加</translation>
        </message>
        <message>
            <source>New playlist and add</source>
            <extracomment>Creates a playlist and adds the video to it</extracomment>
            <translation>新しい再生リストを作成して追加</translation>
        </message>
        <message>
            <source>New playlist</source>
            <extracomment>Heading of the dialog naming a new playlist</extracomment>
            <translation>新しい再生リスト</translation>
        </message>
        <message>
            <source>Watch later</source>
            <extracomment>Name of the built-in watch later playlist</extracomment>
            <translation>後で見る</translation>
        </message>
    </context>
    <context>
        <name>BetaBanner</name>
        <message numerus="yes">
            <source>This beta ends in %n day(s), on %1.</source>
            <extracomment>Beta warning banner; %n is the number of days left, %1 the end date ("30 Dec 2026")</extracomment>
            <translation>
                <numerusform>このベータ版はあと %n 日、%1 に終了します。</numerusform>
            </translation>
        </message>
        <message>
            <source>Dismiss</source>
            <extracomment>Accessible name of the button that hides the beta warning for today</extracomment>
            <translation>閉じる</translation>
        </message>
    </context>
    <context>
        <name>BetaEnded</name>
        <message>
            <source>This beta has ended</source>
            <extracomment>Title of the page shown when the beta build has expired</extracomment>
            <translation>このベータ版は終了しました</translation>
        </message>
        <message>
            <source>This beta of SailTube ended on %1. Install the current version to keep using it; your library, history and settings stay as they are.</source>
            <extracomment>%1 is the end date ("30 Dec 2026")</extracomment>
            <translation>SailTube のこのベータ版は %1 に終了しました。引き続き使うには最新版をインストールしてください。ライブラリ、履歴、設定はそのまま残ります。</translation>
        </message>
        <message>
            <source>Build %1</source>
            <extracomment>%1 is the build, e.g. 0.1.0-0.beta1</extracomment>
            <translation>ビルド %1</translation>
        </message>
        <message>
            <source>Export backup</source>
            <extracomment>Button on the expired-beta page; saves a backup of the library</extracomment>
            <translation>バックアップをエクスポート</translation>
        </message>
        <message>
            <source>Saved to %1</source>
            <extracomment>%1 is the file the backup was written to</extracomment>
            <translation>%1 に保存しました</translation>
        </message>
        <message>
            <source>Save backup in</source>
            <extracomment>Title of the folder picker for the backup on the expired-beta page</extracomment>
            <translation>バックアップの保存先</translation>
        </message>
    </context>
    <context>
        <name>CapabilitiesPage</name>
        <message>
            <source>Hardware</source>
            <extracomment>A codec decoded by the device's hardware decoder</extracomment>
            <translation>ハードウェア</translation>
        </message>
        <message>
            <source>Software</source>
            <extracomment>A codec decoded in software on the CPU</extracomment>
            <translation>ソフトウェア</translation>
        </message>
        <message>
            <source>Not available</source>
            <extracomment>A codec the device cannot decode</extracomment>
            <translation>利用不可</translation>
        </message>
        <message>
            <source>Yes</source>
            <translation>はい</translation>
        </message>
        <message>
            <source>No</source>
            <translation>いいえ</translation>
        </message>
        <message>
            <source>Video capabilities</source>
            <extracomment>Title of the page listing the device's decoders</extracomment>
            <translation>動画機能</translation>
        </message>
        <message>
            <source>GStreamer not inspected; using safe defaults</source>
            <extracomment>GStreamer could not be inspected</extracomment>
            <translation>GStreamer を検査できませんでした。安全な既定値を使用します</translation>
        </message>
        <message>
            <source>Live streams</source>
            <extracomment>Whether live (HLS) streams can be played</extracomment>
            <translation>ライブ配信</translation>
        </message>
        <message>
            <source>Adaptive streams</source>
            <extracomment>Whether separate audio and video streams can be combined</extracomment>
            <translation>アダプティブストリーム</translation>
        </message>
        <message>
            <source>JavaScript runtime</source>
            <extracomment>Whether the JavaScript runtime for the full resolver profile is packaged</extracomment>
            <translation>JavaScript ランタイム</translation>
        </message>
        <message>
            <source>Elements</source>
            <extracomment>Section listing the GStreamer elements found on the device</extracomment>
            <translation>エレメント</translation>
        </message>
        <message>
            <source>Hardware decoder formats</source>
            <extracomment>Section showing the raw capabilities of the hardware decoder</extracomment>
            <translation>ハードウェアデコーダーの形式</translation>
        </message>
        <message>
            <source>Hardware decoder</source>
            <extracomment>Whether the hardware decoder was switched off in the settings</extracomment>
            <translation>ハードウェアデコーダー</translation>
        </message>
        <message>
            <source>Off</source>
            <translation>オフ</translation>
        </message>
        <message>
            <source>For adaptive streams</source>
            <extracomment>playbin3 is used only for adaptive streams</extracomment>
            <translation>アダプティブストリーム用</translation>
        </message>
    </context>
    <context>
        <name>CaptionsPage</name>
        <message>
            <source>Captions</source>
            <extracomment>Title of the captions page
----------
Title of the page choosing the caption track</extracomment>
            <translation>字幕</translation>
        </message>
        <message>
            <source>Off</source>
            <extracomment>Caption choice that turns captions off</extracomment>
            <translation>オフ</translation>
        </message>
        <message>
            <source>Translations</source>
            <extracomment>Section of machine-translated caption languages</extracomment>
            <translation>自動翻訳</translation>
        </message>
        <message>
            <source>Search languages</source>
            <extracomment>Placeholder of the field filtering caption translations by name or code</extracomment>
            <translation>言語を検索</translation>
        </message>
        <message>
            <source>No captions for this video</source>
            <extracomment>Shown when a video has no captions</extracomment>
            <translation>この動画には字幕がありません</translation>
        </message>
        <message>
            <source>Retry</source>
            <extracomment>Menu entry asking again for the caption list after it failed to load</extracomment>
            <translation>再試行</translation>
        </message>
        <message>
            <source>This track could not be loaded</source>
            <extracomment>Shown under the title when the chosen caption track could not be loaded</extracomment>
            <translation>このトラックを読み込めませんでした</translation>
        </message>
        <message>
            <source>Loading captions</source>
            <extracomment>Shown while the caption list of a video is being loaded</extracomment>
            <translation>字幕を読み込み中</translation>
        </message>
        <message>
            <source>Captions could not be loaded</source>
            <extracomment>Shown when the caption list of a video could not be loaded</extracomment>
            <translation>字幕を読み込めませんでした</translation>
        </message>
        <message>
            <source>Pull down to try again.</source>
            <extracomment>Hint under the message that the caption list could not be loaded</extracomment>
            <translation>下に引いて再試行してください。</translation>
        </message>
    </context>
    <context>
        <name>ChannelPage</name>
        <message>
            <source>Videos</source>
            <extracomment>Channel tab with the regular uploads</extracomment>
            <translation>動画</translation>
        </message>
        <message>
            <source>Shorts</source>
            <extracomment>Channel tab with the channel's Shorts</extracomment>
            <translation>ショート</translation>
        </message>
        <message>
            <source>Live</source>
            <extracomment>Channel tab with live and past streams</extracomment>
            <translation>ライブ</translation>
        </message>
        <message>
            <source>Playlists</source>
            <extracomment>Channel tab with the channel's playlists</extracomment>
            <translation>再生リスト</translation>
        </message>
        <message>
            <source>Releases</source>
            <extracomment>Channel tab with albums and releases (music channels)</extracomment>
            <translation>リリース</translation>
        </message>
        <message>
            <source>Refresh</source>
            <extracomment>Menu entry reloading the channel page</extracomment>
            <translation>更新</translation>
        </message>
        <message>
            <source>Show Shorts</source>
            <extracomment>Menu entry showing Shorts again in the channel lists</extracomment>
            <translation>ショートを表示</translation>
        </message>
        <message>
            <source>Hide Shorts</source>
            <extracomment>Menu entry hiding confirmed Shorts from the channel lists</extracomment>
            <translation>ショートを非表示</translation>
        </message>
        <message>
            <source>Show members-only videos</source>
            <extracomment>Menu entry showing members-only videos again</extracomment>
            <translation>メンバー限定動画を表示</translation>
        </message>
        <message>
            <source>Hide members-only videos</source>
            <extracomment>Menu entry hiding members-only videos</extracomment>
            <translation>メンバー限定動画を非表示</translation>
        </message>
        <message>
            <source>Channel</source>
            <extracomment>Fallback title of a channel page while it loads</extracomment>
            <translation>チャンネル</translation>
        </message>
        <message>
            <source>%1 subscribers</source>
            <extracomment>Subscriber count on the channel header, %1 is a compact number such as 1.2M</extracomment>
            <translation>登録者 %1 人</translation>
        </message>
        <message>
            <source>%1 videos</source>
            <extracomment>Video count on the channel header, %1 is a compact number such as 6.2K</extracomment>
            <translation>動画 %1 本</translation>
        </message>
        <message>
            <source>This channel has no Videos tab.</source>
            <extracomment>Note on a channel page that opened its playlists because the channel has no Videos tab</extracomment>
            <translation>このチャンネルには「動画」タブがありません。</translation>
        </message>
        <message numerus="yes">
            <source>%n videos</source>
            <extracomment>Number of videos in a playlist on its thumbnail</extracomment>
            <translation>
                <numerusform>%n 本の動画</numerusform>
            </translation>
        </message>
        <message>
            <source>%1 views</source>
            <extracomment>Number of views of a video, %1 is a compact number such as 1.2M</extracomment>
            <translation>%1 回視聴</translation>
        </message>
        <message>
            <source>Members only</source>
            <extracomment>Badge of a video only channel members can watch</extracomment>
            <translation>メンバー限定</translation>
        </message>
        <message>
            <source>Restricted</source>
            <extracomment>Badge of an age-restricted or otherwise restricted video</extracomment>
            <translation>制限付き</translation>
        </message>
        <message>
            <source>Upcoming</source>
            <extracomment>Badge of a scheduled live stream or premiere</extracomment>
            <translation>公開予定</translation>
        </message>
        <message>
            <source>Short</source>
            <extracomment>Badge of a confirmed YouTube Short</extracomment>
            <translation>ショート</translation>
        </message>
        <message numerus="yes">
            <source>%n hidden by filters</source>
            <extracomment>Footer of a list: how many rows the Shorts / members-only filters hid</extracomment>
            <translation>
                <numerusform>フィルターで %n 件を非表示</numerusform>
            </translation>
        </message>
        <message>
            <source>%1 Tap to retry.</source>
            <extracomment>Inline error at the end of a list, %1 is the error text</extracomment>
            <translation>%1 タップして再試行。</translation>
        </message>
        <message>
            <source>Everything here is hidden by filters</source>
            <extracomment>Shown when every row of a channel tab was hidden by the filters</extracomment>
            <translation>ここにあるものはすべてフィルターで非表示になっています</translation>
        </message>
        <message>
            <source>No playlists</source>
            <extracomment>Shown on a channel tab without playlists</extracomment>
            <translation>再生リストはありません</translation>
        </message>
        <message>
            <source>No videos</source>
            <extracomment>Shown on a channel tab without videos</extracomment>
            <translation>動画はありません</translation>
        </message>
        <message>
            <source>Pull down to refresh.</source>
            <extracomment>Hint under a channel page error</extracomment>
            <translation>下に引いて更新してください。</translation>
        </message>
    </context>
    <context>
        <name>ChaptersPage</name>
        <message>
            <source>Chapters</source>
            <extracomment>Title of the chapter list page</extracomment>
            <translation>チャプター</translation>
        </message>
        <message>
            <source>No chapters</source>
            <extracomment>Shown when a video has no chapters</extracomment>
            <translation>チャプターはありません</translation>
        </message>
    </context>
    <context>
        <name>CommentsModel</name>
        <message>
            <source>%1 of %2</source>
            <extracomment>Footer under the comments: "&lt;shown&gt; of &lt;total&gt;", e.g. "20 of 7.7K"</extracomment>
            <translation>%1 / %2 件</translation>
        </message>
        <message>
            <source>%1 shown</source>
            <extracomment>Footer under the comments when the total is unknown</extracomment>
            <translation>%1 件を表示</translation>
        </message>
        <message>
            <source>Comments are turned off.</source>
            <extracomment>Shown instead of comments when the uploader turned them off</extracomment>
            <translation>コメントはオフになっています。</translation>
        </message>
        <message>
            <source>No comments yet.</source>
            <extracomment>Shown when a video has no comments</extracomment>
            <translation>まだコメントはありません。</translation>
        </message>
        <message>
            <source>Comments could not be loaded. Check the connection.</source>
            <extracomment>Inline error in the comments section</extracomment>
            <translation>コメントを読み込めませんでした。接続を確認してください。</translation>
        </message>
        <message>
            <source>YouTube is limiting requests. Try again later.</source>
            <translation>YouTube がリクエストを制限しています。後でもう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube did not answer. Try again.</source>
            <translation>YouTube から応答がありませんでした。もう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube sent comments in an unknown format.</source>
            <translation>YouTube から不明な形式のコメントが送られてきました。</translation>
        </message>
        <message>
            <source>Comments are not available for this video.</source>
            <translation>この動画ではコメントを利用できません。</translation>
        </message>
    </context>
    <context>
        <name>CommentsPage</name>
        <message>
            <source>Retry</source>
            <extracomment>Menu entry retrying the comments after an error</extracomment>
            <translation>再試行</translation>
        </message>
        <message>
            <source>Sort by top</source>
            <extracomment>Menu entry sorting comments by YouTube's top comments</extracomment>
            <translation>評価順で並べ替え</translation>
        </message>
        <message>
            <source>Sort by newest</source>
            <extracomment>Menu entry sorting comments newest first</extracomment>
            <translation>新しい順で並べ替え</translation>
        </message>
        <message>
            <source>Comments</source>
            <extracomment>Title of the comments page</extracomment>
            <translation>コメント</translation>
        </message>
        <message>
            <source>%1 · %2</source>
            <extracomment>Subtitle of the comments page: the sort and the total</extracomment>
            <translation>%1 · %2</translation>
        </message>
        <message>
            <source>Newest</source>
            <translation>新しい順</translation>
        </message>
        <message>
            <source>Top</source>
            <translation>評価順</translation>
        </message>
    </context>
    <context>
        <name>CommentsSection</name>
        <message>
            <source>Comments %1</source>
            <extracomment>Comments section header with the total count, e.g. "Comments 7.7K"</extracomment>
            <translation>コメント %1</translation>
        </message>
        <message>
            <source>Comments</source>
            <extracomment>Comments section header without a known count</extracomment>
            <translation>コメント</translation>
        </message>
        <message>
            <source>Show comments</source>
            <extracomment>Button loading the comments of the video</extracomment>
            <translation>コメントを表示</translation>
        </message>
        <message>
            <source>Sort</source>
            <extracomment>Label of the comment sort choice</extracomment>
            <translation>並べ替え</translation>
        </message>
        <message>
            <source>Top</source>
            <extracomment>Comment sort: YouTube's top comments</extracomment>
            <translation>評価順</translation>
        </message>
        <message>
            <source>Newest</source>
            <extracomment>Comment sort: newest first</extracomment>
            <translation>新しい順</translation>
        </message>
        <message>
            <source>%1 Tap to retry.</source>
            <extracomment>Inline comments error followed by the retry hint</extracomment>
            <translation>%1 タップして再試行。</translation>
        </message>
        <message>
            <source>Loading replies…</source>
            <extracomment>Row under a comment's replies while they load</extracomment>
            <translation>返信を読み込んでいます…</translation>
        </message>
        <message>
            <source>More replies</source>
            <extracomment>Row loading further replies of a comment</extracomment>
            <translation>他の返信</translation>
        </message>
        <message>
            <source>Pinned</source>
            <extracomment>Marks the comment the uploader pinned</extracomment>
            <translation>固定済み</translation>
        </message>
        <message>
            <source>%1 likes</source>
            <extracomment>Like count of a comment, e.g. "1.3K likes"</extracomment>
            <translation>高評価 %1 件</translation>
        </message>
        <message>
            <source>Hide replies</source>
            <extracomment>Toggle hiding a comment's replies</extracomment>
            <translation>返信を非表示</translation>
        </message>
        <message numerus="yes">
            <source>%n replies</source>
            <extracomment>Toggle showing a comment's replies, e.g. "66 replies"</extracomment>
            <translation>
                <numerusform>%n 件の返信</numerusform>
            </translation>
        </message>
    </context>
    <context>
        <name>CoverPage</name>
        <message>
            <source>Continue watching</source>
            <extracomment>Cover label above the last video, which can be resumed from the cover</extracomment>
            <translation>続きを見る</translation>
        </message>
    </context>
    <context>
        <name>DislikeInfo</name>
        <message>
            <source>Estimated dislikes</source>
            <translation>低評価数の推定値</translation>
        </message>
        <message>
            <source>Dislike counts come from the third-party Return YouTube Dislike service and are estimates.</source>
            <translation>低評価数はサードパーティの Return YouTube Dislike サービスによる推定値です。</translation>
        </message>
    </context>
    <context>
        <name>DownloadBadge</name>
        <message>
            <source>Saved</source>
            <extracomment>Badge on the thumbnail of a downloaded video</extracomment>
            <translation>保存済み</translation>
        </message>
    </context>
    <context>
        <name>DownloadFolderPage</name>
        <message>
            <source>Download folder</source>
            <extracomment>Title of the download folder page</extracomment>
            <translation>ダウンロード先フォルダー</translation>
        </message>
        <message>
            <source>Downloads are not available in this build.</source>
            <extracomment>Shown on the download folder page in a build without downloads</extracomment>
            <translation>このビルドではダウンロードを利用できません。</translation>
        </message>
        <message>
            <source>And what is saved with each download</source>
            <extracomment>Subtitle of the download folder page, which also holds the offline extras</extracomment>
            <translation>各ダウンロードと一緒に保存するもの</translation>
        </message>
    </context>
    <context>
        <name>DownloadNotifier</name>
        <message numerus="yes">
            <source>Downloading %n video(s)</source>
            <extracomment>System notification summary while downloads run, %n is the number of active downloads</extracomment>
            <translation>
                <numerusform>%n 本の動画をダウンロード中</numerusform>
            </translation>
        </message>
        <message>
            <source>Download complete</source>
            <extracomment>System notification when a download completed</extracomment>
            <translation>ダウンロード完了</translation>
        </message>
        <message>
            <source>Download failed</source>
            <extracomment>System notification when a download failed</extracomment>
            <translation>ダウンロード失敗</translation>
        </message>
    </context>
    <context>
        <name>DownloadQualityDialog</name>
        <message>
            <source>Download</source>
            <extracomment>Accept button of the download dialog
----------
Download dialog: audio or video</extracomment>
            <translation>ダウンロード</translation>
        </message>
        <message>
            <source>Audio only</source>
            <extracomment>Download kind: the audio track only</extracomment>
            <translation>音声のみ</translation>
        </message>
        <message>
            <source>Video</source>
            <extracomment>Download kind: video with sound</extracomment>
            <translation>動画</translation>
        </message>
        <message>
            <source>Quality</source>
            <extracomment>Download dialog: the video quality</extracomment>
            <translation>画質</translation>
        </message>
        <message>
            <source>The best available quality up to this height.</source>
            <extracomment>Explanation under the download quality choice</extracomment>
            <translation>この高さまでの最高画質。</translation>
        </message>
    </context>
    <context>
        <name>DownloadSettingsSection</name>
        <message>
            <source>Downloads</source>
            <extracomment>Settings section about downloads</extracomment>
            <translation>ダウンロード</translation>
        </message>
        <message>
            <source>Only on unmetered connections</source>
            <extracomment>Setting: download only on Wi-Fi or other unmetered connections</extracomment>
            <translation>従量制でない接続時のみ</translation>
        </message>
        <message>
            <source>Download folder</source>
            <extracomment>Setting: where downloads are saved</extracomment>
            <translation>ダウンロード先フォルダー</translation>
        </message>
        <message>
            <source>App folder</source>
            <extracomment>Download folder choice: the app's private folder
----------
Download folder choice: the app's own folder</extracomment>
            <translation>アプリのフォルダー</translation>
        </message>
        <message>
            <source>Save captions with downloads</source>
            <extracomment>Setting: save the captions of a video together with its download</extracomment>
            <translation>ダウンロード時に字幕を保存</translation>
        </message>
        <message>
            <source>Captions show when the video plays offline</source>
            <extracomment>Description of the setting that saves captions with downloads</extracomment>
            <translation>オフライン再生時に字幕を表示します</translation>
        </message>
        <message>
            <source>Save SponsorBlock segments with downloads</source>
            <extracomment>Setting: save the SponsorBlock segments of a video together with its download</extracomment>
            <translation>ダウンロード時に SponsorBlock の区間を保存</translation>
        </message>
        <message>
            <source>Segments are skipped when the video plays offline</source>
            <extracomment>Description of the setting that saves SponsorBlock segments with downloads</extracomment>
            <translation>オフライン再生時に区間をスキップします</translation>
        </message>
    </context>
    <context>
        <name>DownloadsPage</name>
        <message>
            <source>Downloads</source>
            <extracomment>Title of the downloads page</extracomment>
            <translation>ダウンロード</translation>
        </message>
    </context>
    <context>
        <name>DownloadsView</name>
        <message>
            <source>Downloads</source>
            <extracomment>Title of the downloads page</extracomment>
            <translation>ダウンロード</translation>
        </message>
        <message>
            <source>Downloads run only while SailTube is open.</source>
            <extracomment>Note on the downloads page of the store build (DL-09)</extracomment>
            <translation>ダウンロードは SailTube を開いている間のみ実行されます。</translation>
        </message>
        <message>
            <source>Pause</source>
            <extracomment>Context menu entry pausing a download</extracomment>
            <translation>一時停止</translation>
        </message>
        <message>
            <source>Resume</source>
            <extracomment>Context menu entry resuming a paused download</extracomment>
            <translation>再開</translation>
        </message>
        <message>
            <source>Retry</source>
            <extracomment>Context menu entry retrying a failed download</extracomment>
            <translation>再試行</translation>
        </message>
        <message>
            <source>Delete</source>
            <extracomment>Context menu entry deleting a downloaded file</extracomment>
            <translation>削除</translation>
        </message>
        <message>
            <source>Cancel</source>
            <extracomment>Context menu entry cancelling a download</extracomment>
            <translation>キャンセル</translation>
        </message>
        <message>
            <source>Removing</source>
            <extracomment>Remorse text while a download is removed</extracomment>
            <translation>削除中</translation>
        </message>
        <message>
            <source>Audio</source>
            <extracomment>Kind of a download: the audio track only</extracomment>
            <translation>音声</translation>
        </message>
        <message>
            <source>Video</source>
            <translation>動画</translation>
        </message>
        <message>
            <source>No downloads</source>
            <extracomment>Empty downloads page</extracomment>
            <translation>ダウンロードはありません</translation>
        </message>
        <message>
            <source>Download from a video's pull-down menu or by long pressing a video.</source>
            <extracomment>Hint on the empty downloads page</extracomment>
            <translation>動画のプルダウンメニューから、または動画を長押ししてダウンロードできます。</translation>
        </message>
    </context>
    <context>
        <name>FeedModel</name>
        <message>
            <source>Scheduled for %1</source>
            <extracomment>Start time of an upcoming live stream or premiere, %1 is a date and time</extracomment>
            <translation>%1 に公開予定</translation>
        </message>
        <message>
            <source>Upcoming</source>
            <translation>公開予定</translation>
        </message>
        <message>
            <source>The feed could not be refreshed. Check the connection.</source>
            <translation>フィードを更新できませんでした。接続を確認してください。</translation>
        </message>
        <message>
            <source>Recommendations could not be loaded. Try again.</source>
            <translation>おすすめを読み込めませんでした。もう一度お試しください。</translation>
        </message>
    </context>
    <context>
        <name>FeedView</name>
        <message>
            <source>About SailTube</source>
            <extracomment>Menu entry opening the about page</extracomment>
            <translation>SailTube について</translation>
        </message>
        <message>
            <source>Settings</source>
            <extracomment>Menu entry opening the application settings</extracomment>
            <translation>設定</translation>
        </message>
        <message>
            <source>Recommended</source>
            <extracomment>Menu entry opening the recommendations of the signed-in account</extracomment>
            <translation>おすすめ</translation>
        </message>
        <message>
            <source>Search</source>
            <extracomment>Menu entry opening the search page</extracomment>
            <translation>検索</translation>
        </message>
        <message>
            <source>Refresh</source>
            <extracomment>Menu entry refreshing every subscribed channel</extracomment>
            <translation>更新</translation>
        </message>
        <message>
            <source>Subscriptions</source>
            <extracomment>Title of the subscriptions feed page</extracomment>
            <translation>登録チャンネル</translation>
        </message>
        <message>
            <source>No subscriptions</source>
            <extracomment>Shown on the feed page without subscriptions</extracomment>
            <translation>登録チャンネルはありません</translation>
        </message>
        <message>
            <source>No videos</source>
            <extracomment>Shown when the subscribed channels have no videos</extracomment>
            <translation>動画はありません</translation>
        </message>
        <message>
            <source>Subscribe to channels to see their new videos here.</source>
            <extracomment>Hint on the feed page without subscriptions</extracomment>
            <translation>チャンネルを登録すると、ここに新しい動画が表示されます。</translation>
        </message>
        <message>
            <source>Pull down to refresh.</source>
            <extracomment>Hint on an empty or failed feed</extracomment>
            <translation>下に引いて更新してください。</translation>
        </message>
    </context>
    <context>
        <name>HistoryPage</name>
        <message>
            <source>Resume history</source>
            <extracomment>Menu entry turning history recording back on</extracomment>
            <translation>履歴の記録を再開</translation>
        </message>
        <message>
            <source>Pause history</source>
            <extracomment>Menu entry stopping history recording</extracomment>
            <translation>履歴を一時停止</translation>
        </message>
        <message>
            <source>Clear history</source>
            <extracomment>Menu entry deleting the whole watch history</extracomment>
            <translation>履歴を消去</translation>
        </message>
        <message>
            <source>Clearing history</source>
            <extracomment>Remorse text while the history is being cleared</extracomment>
            <translation>履歴を消去中</translation>
        </message>
        <message>
            <source>History</source>
            <extracomment>Title of the history page</extracomment>
            <translation>履歴</translation>
        </message>
        <message>
            <source>Paused</source>
            <extracomment>Shown under the history title while recording is paused</extracomment>
            <translation>一時停止中</translation>
        </message>
        <message>
            <source>Remove from history</source>
            <extracomment>Context menu entry removing one video from the history</extracomment>
            <translation>履歴から削除</translation>
        </message>
        <message>
            <source>Removing</source>
            <extracomment>Remorse text while a history entry is being removed</extracomment>
            <translation>削除中</translation>
        </message>
        <message>
            <source>No history</source>
            <extracomment>Empty history page</extracomment>
            <translation>履歴はありません</translation>
        </message>
        <message>
            <source>History is paused.</source>
            <translation>履歴は一時停止中です。</translation>
        </message>
    </context>
    <context>
        <name>HomeSettings</name>
        <message>
            <source>Subscriptions</source>
            <extracomment>Landing page choice: the subscriptions feed</extracomment>
            <translation>登録チャンネル</translation>
        </message>
        <message>
            <source>Recommended</source>
            <extracomment>Landing page choice: recommendations of the signed-in account</extracomment>
            <translation>おすすめ</translation>
        </message>
        <message>
            <source>Search</source>
            <extracomment>Landing page choice: the search page</extracomment>
            <translation>検索</translation>
        </message>
    </context>
    <context>
        <name>ImportExportPage</name>
        <message>
            <source>Choose a file to import</source>
            <extracomment>Title of the file picker for an import</extracomment>
            <translation>インポートするファイルを選択</translation>
        </message>
        <message>
            <source>Save backup in</source>
            <extracomment>Title of the folder picker for a backup export</extracomment>
            <translation>バックアップの保存先</translation>
        </message>
        <message>
            <source>Import and export</source>
            <extracomment>Title of the import and export page</extracomment>
            <translation>インポートとエクスポート</translation>
        </message>
        <message>
            <source>Working…</source>
            <extracomment>Shown while an import or export runs</extracomment>
            <translation>処理中…</translation>
        </message>
        <message>
            <source>Import</source>
            <extracomment>Section of the import page</extracomment>
            <translation>インポート</translation>
        </message>
        <message>
            <source>NewPipe or PipePipe backup</source>
            <extracomment>Imports a NewPipe or PipePipe backup file</extracomment>
            <translation>NewPipe または PipePipe のバックアップ</translation>
        </message>
        <message>
            <source>Google Takeout subscriptions</source>
            <extracomment>Imports a Google Takeout subscriptions.csv file</extracomment>
            <translation>Google Takeout の登録チャンネル</translation>
        </message>
        <message>
            <source>OPML feed list</source>
            <extracomment>Imports an OPML list of channel feeds</extracomment>
            <translation>OPML フィードリスト</translation>
        </message>
        <message>
            <source>SailTube backup</source>
            <extracomment>Section of the import page about SailTube's own backup</extracomment>
            <translation>SailTube のバックアップ</translation>
        </message>
        <message>
            <source>Export backup</source>
            <extracomment>Exports subscriptions, playlists, history and settings to a file</extracomment>
            <translation>バックアップをエクスポート</translation>
        </message>
        <message>
            <source>Restore backup</source>
            <extracomment>Restores a SailTube backup file</extracomment>
            <translation>バックアップを復元</translation>
        </message>
        <message>
            <source>Also restore settings</source>
            <extracomment>Option of the backup restore</extracomment>
            <translation>設定も復元</translation>
        </message>
        <message>
            <source>Imports never replace what is already in your library. Running one again adds nothing twice. Backups contain no sign-in data.</source>
            <extracomment>Explanation on the import page</extracomment>
            <translation>インポートでライブラリの既存の内容が置き換えられることはありません。もう一度実行しても重複して追加されません。バックアップにサインイン情報は含まれません。</translation>
        </message>
        <message>
            <source>From the YouTube account</source>
            <extracomment>Imports the subscriptions and playlists of the signed-in YouTube account</extracomment>
            <translation>YouTube アカウントから</translation>
        </message>
    </context>
    <context>
        <name>LibraryPage</name>
        <message>
            <source>Library</source>
            <extracomment>Title of the library page</extracomment>
            <translation>ライブラリ</translation>
        </message>
        <message>
            <source>Subscriptions</source>
            <extracomment>Library entry: list of subscribed channels</extracomment>
            <translation>登録チャンネル</translation>
        </message>
        <message>
            <source>Watch later</source>
            <extracomment>Library entry: the built-in Watch later list</extracomment>
            <translation>後で見る</translation>
        </message>
        <message>
            <source>Playlists</source>
            <extracomment>Library entry: local and saved playlists</extracomment>
            <translation>再生リスト</translation>
        </message>
        <message>
            <source>History</source>
            <extracomment>Library entry: watch history</extracomment>
            <translation>履歴</translation>
        </message>
        <message>
            <source>Downloads</source>
            <extracomment>Library entry: downloaded videos</extracomment>
            <translation>ダウンロード</translation>
        </message>
        <message>
            <source>Import and export</source>
            <extracomment>Library entry: import and export of the library</extracomment>
            <translation>インポートとエクスポート</translation>
        </message>
        <message>
            <source>YouTube Watch later</source>
            <extracomment>Library entry: the Watch later list of the signed-in YouTube account</extracomment>
            <translation>YouTube の後で見る</translation>
        </message>
        <message>
            <source>Liked videos</source>
            <extracomment>Library entry: the liked videos of the signed-in YouTube account</extracomment>
            <translation>高く評価した動画</translation>
        </message>
    </context>
    <context>
        <name>LibrarySection</name>
        <message>
            <source>Playlists</source>
            <extracomment>Library row: the user's playlists</extracomment>
            <translation>再生リスト</translation>
        </message>
        <message>
            <source>Saved and local playlists</source>
            <translation>保存済みとローカルの再生リスト</translation>
        </message>
        <message>
            <source>Watch later</source>
            <extracomment>Library row: videos saved to watch later</extracomment>
            <translation>後で見る</translation>
        </message>
        <message>
            <source>Videos you saved for later</source>
            <translation>後で見るために保存した動画</translation>
        </message>
        <message>
            <source>History</source>
            <extracomment>Library row: watch history</extracomment>
            <translation>履歴</translation>
        </message>
        <message>
            <source>Videos you watched, with resume points</source>
            <translation>視聴した動画と再開位置</translation>
        </message>
        <message>
            <source>Channels</source>
            <extracomment>Library row: subscribed channels</extracomment>
            <translation>チャンネル</translation>
        </message>
        <message>
            <source>Your subscriptions</source>
            <translation>登録チャンネル</translation>
        </message>
        <message>
            <source>Downloads</source>
            <extracomment>Library row: downloaded videos</extracomment>
            <translation>ダウンロード</translation>
        </message>
        <message>
            <source>Videos and audio saved on this device</source>
            <translation>この端末に保存した動画と音声</translation>
        </message>
        <message>
            <source>Settings</source>
            <extracomment>Menu entry opening the application settings</extracomment>
            <translation>設定</translation>
        </message>
        <message>
            <source>About SailTube</source>
            <extracomment>Menu entry opening the about page</extracomment>
            <translation>SailTube について</translation>
        </message>
        <message>
            <source>Not available yet</source>
            <extracomment>Shown under a library row whose page is not in this build yet</extracomment>
            <translation>未対応</translation>
        </message>
        <message>
            <source>Import and export</source>
            <extracomment>Library row: import from other apps and back up the library</extracomment>
            <translation>インポートとエクスポート</translation>
        </message>
        <message>
            <source>NewPipe, Takeout, OPML and SailTube backups</source>
            <translation>NewPipe、Takeout、OPML、SailTube のバックアップ</translation>
        </message>
        <message>
            <source>From your YouTube account</source>
            <extracomment>Detail line of the Library rows that open a list of the signed-in YouTube account</extracomment>
            <translation>YouTube アカウントから</translation>
        </message>
    </context>
    <context>
        <name>MainPage</name>
        <message>
            <source>Home</source>
            <extracomment>Main page section with recommended or trending videos</extracomment>
            <translation>ホーム</translation>
        </message>
        <message>
            <source>Subscriptions</source>
            <extracomment>Main page section with the feed of subscribed channels</extracomment>
            <translation>登録チャンネル</translation>
        </message>
        <message>
            <source>Library</source>
            <extracomment>Main page section with playlists, history, watch later and downloads</extracomment>
            <translation>ライブラリ</translation>
        </message>
        <message>
            <source>Search</source>
            <extracomment>Main page section for searching videos</extracomment>
            <translation>検索</translation>
        </message>
    </context>
    <context>
        <name>MiniPlayer</name>
        <message>
            <source>Pause</source>
            <extracomment>Accessible name of the pause button</extracomment>
            <translation>一時停止</translation>
        </message>
        <message>
            <source>Play</source>
            <extracomment>Accessible name of the play button</extracomment>
            <translation>再生</translation>
        </message>
        <message>
            <source>Next video</source>
            <extracomment>Accessible name of the button playing the next video of the queue</extracomment>
            <translation>次の動画</translation>
        </message>
        <message>
            <source>Close player</source>
            <extracomment>Accessible name of the button closing the mini-player and stopping playback</extracomment>
            <translation>プレーヤーを閉じる</translation>
        </message>
    </context>
    <context>
        <name>PasteSessionDialog</name>
        <message>
            <source>Choose a cookies file</source>
            <extracomment>Title of the file picker for a cookies file</extracomment>
            <translation>Cookie ファイルを選択</translation>
        </message>
        <message>
            <source>Import</source>
            <extracomment>Accept button of the paste dialog</extracomment>
            <translation>インポート</translation>
        </message>
        <message>
            <source>Paste cookies here</source>
            <extracomment>Placeholder of the paste field</extracomment>
            <translation>ここに Cookie を貼り付け</translation>
        </message>
        <message>
            <source>Cookies</source>
            <extracomment>Label of the paste field</extracomment>
            <translation>Cookie</translation>
        </message>
        <message>
            <source>Load from a file</source>
            <extracomment>Button loading cookies from a file</extracomment>
            <translation>ファイルから読み込む</translation>
        </message>
    </context>
    <context>
        <name>PlaceholderButtons</name>
        <message>
            <source>Open downloads</source>
            <extracomment>Button under a failed list (usually offline) that opens the downloaded videos</extracomment>
            <translation>ダウンロードを開く</translation>
        </message>
    </context>
    <context>
        <name>PlayerControls</name>
        <message>
            <source>LIVE</source>
            <extracomment>Shown instead of the duration for a live stream</extracomment>
            <translation>ライブ</translation>
        </message>
        <message>
            <source>Pause</source>
            <extracomment>Accessible name of the pause button</extracomment>
            <translation>一時停止</translation>
        </message>
        <message>
            <source>Play</source>
            <extracomment>Accessible name of the play button</extracomment>
            <translation>再生</translation>
        </message>
        <message>
            <source>Exit full screen</source>
            <extracomment>Accessible name of the button leaving full screen</extracomment>
            <translation>全画面表示を終了</translation>
        </message>
        <message>
            <source>Full screen</source>
            <extracomment>Accessible name of the full screen button</extracomment>
            <translation>全画面表示</translation>
        </message>
    </context>
    <context>
        <name>PlayerOverlay</name>
        <message>
            <source>Minimize player</source>
            <extracomment>Accessible name of the button that leaves the watch page for the mini-player</extracomment>
            <translation>プレーヤーを最小化</translation>
        </message>
        <message>
            <source>Previous video</source>
            <extracomment>Accessible name of the button playing the previous video of the queue</extracomment>
            <translation>前の動画</translation>
        </message>
        <message>
            <source>Replay</source>
            <extracomment>Accessible name of the button playing the video again from the start</extracomment>
            <translation>もう一度再生</translation>
        </message>
    </context>
    <context>
        <name>PlayerSettings</name>
        <message>
            <source>Normal</source>
            <extracomment>Normal playback speed (1×)</extracomment>
            <translation>標準</translation>
        </message>
        <message>
            <source>Playback speed</source>
            <extracomment>Player settings row: playback speed</extracomment>
            <translation>再生速度</translation>
        </message>
        <message>
            <source>Audio track</source>
            <extracomment>Player settings row: the audio track (original, dubbed, descriptive)</extracomment>
            <translation>音声トラック</translation>
        </message>
        <message>
            <source>Loop video</source>
            <extracomment>Player settings row: play the video again when it ends</extracomment>
            <translation>ループ再生</translation>
        </message>
        <message>
            <source>Sleep timer</source>
            <extracomment>Player settings row: stop playback after a while</extracomment>
            <translation>スリープタイマー</translation>
        </message>
        <message>
            <source>Stable volume</source>
            <extracomment>Player settings row: even out loud and quiet videos</extracomment>
            <translation>音量の安定化</translation>
        </message>
        <message>
            <source>Off</source>
            <extracomment>Sleep timer choice: no timer</extracomment>
            <translation>オフ</translation>
        </message>
        <message>
            <source>End of video</source>
            <extracomment>Sleep timer choice: stop when the current video ends</extracomment>
            <translation>動画の終了時</translation>
        </message>
        <message numerus="yes">
            <source>%n minute(s)</source>
            <extracomment>Sleep timer choice, %n is a number of minutes</extracomment>
            <translation>
                <numerusform>%n 分</numerusform>
            </translation>
        </message>
        <message>
            <source>%1 left</source>
            <extracomment>Sleep timer value in the settings sheet, %1 is the time left such as 12:30</extracomment>
            <translation>残り %1</translation>
        </message>
        <message>
            <source>Lock screen</source>
            <extracomment>Player settings row: ignore touches on the video in full screen until it is unlocked</extracomment>
            <translation>画面をロック</translation>
        </message>
    </context>
    <context>
        <name>PlaylistNameDialog</name>
        <message>
            <source>Playlist name</source>
            <extracomment>Placeholder of the playlist name field</extracomment>
            <translation>再生リスト名</translation>
        </message>
    </context>
    <context>
        <name>PlaylistPage</name>
        <message>
            <source>Refresh from YouTube</source>
            <extracomment>Menu entry fetching a saved YouTube playlist again</extracomment>
            <translation>YouTube から更新</translation>
        </message>
        <message>
            <source>Download all</source>
            <extracomment>Menu entry downloading every video of a playlist that is not downloaded yet</extracomment>
            <translation>すべてダウンロード</translation>
        </message>
        <message>
            <source>Shuffle</source>
            <extracomment>Menu entry playing a playlist in random order</extracomment>
            <translation>シャッフル</translation>
        </message>
        <message>
            <source>Play all</source>
            <extracomment>Menu entry playing a playlist from the start</extracomment>
            <translation>すべて再生</translation>
        </message>
        <message>
            <source>Watch later</source>
            <extracomment>Name of the built-in watch later playlist</extracomment>
            <translation>後で見る</translation>
        </message>
        <message numerus="yes">
            <source>%n video(s)</source>
            <extracomment>Number of videos in a playlist</extracomment>
            <translation>
                <numerusform>%n 本の動画</numerusform>
            </translation>
        </message>
        <message>
            <source>Remove from playlist</source>
            <extracomment>Context menu entry removing a video from a playlist</extracomment>
            <translation>再生リストから削除</translation>
        </message>
        <message>
            <source>Removing</source>
            <extracomment>Remorse text while a video is being removed from a playlist</extracomment>
            <translation>削除中</translation>
        </message>
        <message>
            <source>Move up</source>
            <extracomment>Playlist item menu: move the video one place up</extracomment>
            <translation>上へ移動</translation>
        </message>
        <message>
            <source>Move down</source>
            <extracomment>Context menu entry moving a video one place down in a playlist</extracomment>
            <translation>下へ移動</translation>
        </message>
        <message>
            <source>Not fetched yet</source>
            <extracomment>Empty saved YouTube playlist that has not been fetched yet</extracomment>
            <translation>まだ取得していません</translation>
        </message>
        <message>
            <source>No videos</source>
            <extracomment>Empty playlist</extracomment>
            <translation>動画はありません</translation>
        </message>
        <message>
            <source>Long press a video anywhere to add it.</source>
            <extracomment>Hint on an empty local playlist</extracomment>
            <translation>動画を長押しすると追加できます。</translation>
        </message>
    </context>
    <context>
        <name>PlaylistPreviewPage</name>
        <message>
            <source>Open channel</source>
            <extracomment>Menu entry opening the channel that owns the playlist</extracomment>
            <translation>チャンネルを開く</translation>
        </message>
        <message>
            <source>Playlist</source>
            <extracomment>Fallback title of a playlist preview while it loads</extracomment>
            <translation>再生リスト</translation>
        </message>
        <message numerus="yes">
            <source>%n videos</source>
            <extracomment>Number of videos in a playlist</extracomment>
            <translation>
                <numerusform>%n 本の動画</numerusform>
            </translation>
        </message>
        <message>
            <source>Members only · %1</source>
            <extracomment>Prefix of the channel line of a members-only video, %1 is the channel</extracomment>
            <translation>メンバー限定 · %1</translation>
        </message>
        <message>
            <source>Restricted · %1</source>
            <extracomment>Prefix of the channel line of a restricted video, %1 is the channel</extracomment>
            <translation>制限付き · %1</translation>
        </message>
        <message>
            <source>This playlist is empty</source>
            <extracomment>Shown for a playlist without videos</extracomment>
            <translation>この再生リストは空です</translation>
        </message>
        <message>
            <source>Open in library</source>
            <extracomment>Menu entry opening the saved copy of a YouTube playlist in the library</extracomment>
            <translation>ライブラリで開く</translation>
        </message>
        <message>
            <source>Save to library</source>
            <extracomment>Menu entry saving a YouTube playlist to the library</extracomment>
            <translation>ライブラリに保存</translation>
        </message>
    </context>
    <context>
        <name>PlaylistsPage</name>
        <message>
            <source>New playlist</source>
            <extracomment>Menu entry creating a new local playlist
----------
Header of the dialog creating a playlist</extracomment>
            <translation>新しい再生リスト</translation>
        </message>
        <message>
            <source>Playlists</source>
            <extracomment>Title of the playlists page</extracomment>
            <translation>再生リスト</translation>
        </message>
        <message>
            <source>Rename</source>
            <extracomment>Context menu entry renaming a playlist</extracomment>
            <translation>名前を変更</translation>
        </message>
        <message>
            <source>Rename playlist</source>
            <extracomment>Header of the dialog renaming a playlist</extracomment>
            <translation>再生リスト名を変更</translation>
        </message>
        <message>
            <source>Move up</source>
            <extracomment>Context menu entry moving a playlist one place up</extracomment>
            <translation>上へ移動</translation>
        </message>
        <message>
            <source>Remove from library</source>
            <extracomment>Context menu entry removing a saved YouTube playlist from the library</extracomment>
            <translation>ライブラリから削除</translation>
        </message>
        <message>
            <source>Delete</source>
            <extracomment>Context menu entry deleting a local playlist</extracomment>
            <translation>削除</translation>
        </message>
        <message>
            <source>Deleting</source>
            <extracomment>Remorse text while a playlist is being deleted</extracomment>
            <translation>削除中</translation>
        </message>
        <message>
            <source>Watch later</source>
            <extracomment>Title of the built-in Watch later list</extracomment>
            <translation>後で見る</translation>
        </message>
        <message numerus="yes">
            <source>%n video(s)</source>
            <extracomment>Number of videos in a playlist</extracomment>
            <translation>
                <numerusform>%n 本の動画</numerusform>
            </translation>
        </message>
    </context>
    <context>
        <name>QGuiApplication</name>
        <message>
            <source>QT_LAYOUT_DIRECTION</source>
            <extracomment>Layout direction of the language. Translate exactly as RTL for right-to-left languages (Arabic), LTR otherwise.</extracomment>
            <translation>LTR</translation>
        </message>
    </context>
    <context>
        <name>QualityPage</name>
        <message>
            <source>Quality</source>
            <extracomment>Title of the page choosing the video quality</extracomment>
            <translation>画質</translation>
        </message>
        <message>
            <source>Automatic</source>
            <extracomment>Automatic quality choice in the quality list</extracomment>
            <translation>自動</translation>
        </message>
    </context>
    <context>
        <name>RecommendedView</name>
        <message>
            <source>Popular</source>
            <extracomment>Title of Home while signed out: the most viewed videos of this week in the user's region</extracomment>
            <translation>人気</translation>
        </message>
        <message>
            <source>About SailTube</source>
            <extracomment>Menu entry opening the about page</extracomment>
            <translation>SailTube について</translation>
        </message>
        <message>
            <source>Settings</source>
            <extracomment>Menu entry opening the application settings</extracomment>
            <translation>設定</translation>
        </message>
        <message>
            <source>Subscriptions</source>
            <extracomment>Menu entry opening the subscriptions feed</extracomment>
            <translation>登録チャンネル</translation>
        </message>
        <message>
            <source>Search</source>
            <extracomment>Menu entry opening the search page</extracomment>
            <translation>検索</translation>
        </message>
        <message>
            <source>Refresh</source>
            <extracomment>Menu entry reloading the recommendations</extracomment>
            <translation>更新</translation>
        </message>
        <message>
            <source>Recommended</source>
            <extracomment>Title of the recommendations page</extracomment>
            <translation>おすすめ</translation>
        </message>
        <message>
            <source>Sign in for recommendations</source>
            <extracomment>Shown on the recommendations page while signed out</extracomment>
            <translation>ログインするとおすすめが表示されます</translation>
        </message>
        <message>
            <source>No recommendations</source>
            <extracomment>Shown when the account has no recommendations</extracomment>
            <translation>おすすめはありません</translation>
        </message>
        <message>
            <source>Recommendations come from a signed-in YouTube account.</source>
            <extracomment>Hint on the recommendations page while signed out</extracomment>
            <translation>おすすめはログインした YouTube アカウントから取得されます。</translation>
        </message>
        <message>
            <source>Pull down to refresh.</source>
            <translation>下に引いて更新してください。</translation>
        </message>
    </context>
    <context>
        <name>RelatedPage</name>
        <message>
            <source>Related videos</source>
            <extracomment>Title of the related videos page</extracomment>
            <translation>関連動画</translation>
        </message>
        <message>
            <source>No related videos</source>
            <extracomment>Shown when a video has no related videos</extracomment>
            <translation>関連動画はありません</translation>
        </message>
    </context>
    <context>
        <name>ResumeOffer</name>
        <message>
            <source>Resume at %1 (%2)</source>
            <extracomment>Resume prompt; %1 is a time such as 12:34, %2 the seconds before it resumes by itself</extracomment>
            <translation>%1 から再開 (%2)</translation>
        </message>
        <message>
            <source>Start over</source>
            <extracomment>Resume prompt button that plays the video from the beginning</extracomment>
            <translation>最初から再生</translation>
        </message>
    </context>
    <context>
        <name>SailTubeCatalog</name>
        <message>
            <source>Sign in to see this list.</source>
            <translation>このリストを見るにはログインしてください。</translation>
        </message>
        <message>
            <source>YouTube changed this page. SailTube cannot read it yet.</source>
            <translation>YouTube がこのページを変更しました。SailTube はまだ読み取れません。</translation>
        </message>
        <message>
            <source>YouTube refused the request. Try again later.</source>
            <translation>YouTube がリクエストを拒否しました。後でもう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube is limiting requests. Try again later.</source>
            <translation>YouTube がリクエストを制限しています。後でもう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube is not responding. Try again later.</source>
            <translation>YouTube が応答していません。後でもう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube could not be reached. Check the connection.</source>
            <translation>YouTube に接続できませんでした。接続を確認してください。</translation>
        </message>
        <message>
            <source>This is not available on YouTube.</source>
            <translation>これは YouTube では利用できません。</translation>
        </message>
        <message>
            <source>This playlist does not exist or is private.</source>
            <translation>この再生リストは存在しないか、非公開です。</translation>
        </message>
    </context>
    <context>
        <name>SailTubeErrors</name>
        <message>
            <source>The request is not valid.</source>
            <translation>リクエストが無効です。</translation>
        </message>
        <message>
            <source>The packaged YouTube resolver is not available. Reinstall SailTube or check that PyOtherSide and Python 3.11 are installed.</source>
            <translation>同梱の YouTube リゾルバーを利用できません。SailTube を再インストールするか、PyOtherSide と Python 3.11 がインストールされているか確認してください。</translation>
        </message>
        <message>
            <source>The resolver is busy. Try again.</source>
            <translation>リゾルバーが処理中です。もう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube took too long to answer. Try again.</source>
            <translation>YouTube の応答に時間がかかりすぎました。もう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube could not be reached. Check the connection.</source>
            <translation>YouTube に接続できませんでした。接続を確認してください。</translation>
        </message>
        <message>
            <source>YouTube asked to confirm that this is not a bot. Try again later or from another network.</source>
            <translation>YouTube からボットではないことの確認を求められました。後でもう一度試すか、別のネットワークからお試しください。</translation>
        </message>
        <message>
            <source>This video is age-restricted and needs a signed-in account.</source>
            <translation>この動画は年齢制限があり、ログインしたアカウントが必要です。</translation>
        </message>
        <message>
            <source>This video is private.</source>
            <translation>この動画は非公開です。</translation>
        </message>
        <message>
            <source>This video is not available.</source>
            <translation>この動画は利用できません。</translation>
        </message>
        <message>
            <source>This live event has not started yet.</source>
            <translation>このライブイベントはまだ始まっていません。</translation>
        </message>
        <message>
            <source>The resolver returned an unexpected answer.</source>
            <translation>リゾルバーから予期しない応答がありました。</translation>
        </message>
        <message>
            <source>None of the offered streams can be played on this device.</source>
            <translation>提供されたストリームはいずれもこの端末で再生できません。</translation>
        </message>
        <message>
            <source>The resolver failed. Try again.</source>
            <translation>リゾルバーでエラーが発生しました。もう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube is limiting requests from this network. Try again later.</source>
            <translation>YouTube がこのネットワークからのリクエストを制限しています。後でもう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube is having problems. Try again later.</source>
            <translation>YouTube で問題が発生しています。後でもう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube rejected the search. Try again later.</source>
            <translation>YouTube が検索を拒否しました。後でもう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube answered in a form SailTube cannot read yet. Try again later or update SailTube.</source>
            <translation>YouTube から SailTube がまだ読み取れない形式で応答がありました。後でもう一度試すか、SailTube を更新してください。</translation>
        </message>
        <message>
            <source>Search is not available right now.</source>
            <translation>現在、検索は利用できません。</translation>
        </message>
        <message>
            <source>This video needs a signed-in account.</source>
            <translation>この動画にはログインしたアカウントが必要です。</translation>
        </message>
        <message>
            <source>This video has been removed.</source>
            <translation>この動画は削除されました。</translation>
        </message>
        <message>
            <source>This video is not available in your country.</source>
            <translation>この動画はお住まいの国では利用できません。</translation>
        </message>
        <message>
            <source>This stream has just ended and is still being processed.</source>
            <translation>このライブ配信は終了したばかりで、まだ処理中です。</translation>
        </message>
        <message>
            <source>The account that posted this video has been terminated.</source>
            <translation>この動画を投稿したアカウントは停止されています。</translation>
        </message>
        <message>
            <source>This is not a valid video link.</source>
            <translation>有効な動画のリンクではありません。</translation>
        </message>
        <message>
            <source>The video could not be extracted. Try again; if it keeps failing, SailTube's resolver may need an update.</source>
            <extracomment>RES-09: the extraction failed (not a device limit); a later yt-dlp update may fix it</extracomment>
            <translation>動画を抽出できませんでした。もう一度お試しください。失敗が続く場合は、SailTube のリゾルバーの更新が必要かもしれません。</translation>
        </message>
        <message>
            <source>YouTube changed how this video is delivered. Try again later; SailTube's resolver may need an update.</source>
            <extracomment>RES-09: the answer had no usable stream addresses (SABR-only formats, a failed player challenge)</extracomment>
            <translation>YouTube がこの動画の配信方法を変更しました。後でもう一度お試しください。SailTube のリゾルバーの更新が必要かもしれません。</translation>
        </message>
        <message>
            <source>YouTube is limiting requests right now. Try again in a minute.</source>
            <extracomment>RES-09: YouTube is rate limiting (HTTP 429, "try again later")</extracomment>
            <translation>YouTube が現在リクエストを制限しています。1 分後にもう一度お試しください。</translation>
        </message>
    </context>
    <context>
        <name>SailTubeWatch</name>
        <message>
            <source>YouTube could not be reached. Check the connection.</source>
            <translation>YouTube に接続できませんでした。接続を確認してください。</translation>
        </message>
        <message>
            <source>YouTube is limiting requests. Try again later.</source>
            <translation>YouTube がリクエストを制限しています。後でもう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube had a temporary problem. Try again.</source>
            <translation>YouTube で一時的な問題が発生しました。もう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube refused the request. Try again.</source>
            <translation>YouTube がリクエストを拒否しました。もう一度お試しください。</translation>
        </message>
        <message>
            <source>YouTube changed its page layout; this part of the page cannot be read.</source>
            <translation>YouTube がページのレイアウトを変更したため、ページのこの部分を読み取れません。</translation>
        </message>
        <message>
            <source>This video is not available.</source>
            <translation>この動画は利用できません。</translation>
        </message>
    </context>
    <context>
        <name>SearchFilterPage</name>
        <message>
            <source>Reset filters</source>
            <extracomment>Resets every search filter to its default</extracomment>
            <translation>フィルターをリセット</translation>
        </message>
        <message>
            <source>Apply</source>
            <extracomment>Accept button of the search filter dialog</extracomment>
            <translation>適用</translation>
        </message>
        <message>
            <source>Sort by</source>
            <extracomment>Search filter: result order</extracomment>
            <translation>並べ替え</translation>
        </message>
        <message>
            <source>Relevance</source>
            <extracomment>Search sort order: most relevant first</extracomment>
            <translation>関連度順</translation>
        </message>
        <message>
            <source>Upload date</source>
            <extracomment>Search sort order: newest uploads first</extracomment>
            <translation>アップロード日</translation>
        </message>
        <message>
            <source>View count</source>
            <extracomment>Search sort order: most viewed first</extracomment>
            <translation>視聴回数</translation>
        </message>
        <message>
            <source>Rating</source>
            <extracomment>Search sort order: best rated first</extracomment>
            <translation>評価</translation>
        </message>
        <message>
            <source>Uploaded</source>
            <extracomment>Search filter: when the video was uploaded</extracomment>
            <translation>アップロード時期</translation>
        </message>
        <message>
            <source>Any time</source>
            <extracomment>Upload date filter: no limit</extracomment>
            <translation>指定なし</translation>
        </message>
        <message>
            <source>Last hour</source>
            <extracomment>Upload date filter: the last hour</extracomment>
            <translation>1 時間以内</translation>
        </message>
        <message>
            <source>Today</source>
            <extracomment>Upload date filter: today</extracomment>
            <translation>今日</translation>
        </message>
        <message>
            <source>This week</source>
            <extracomment>Upload date filter: this week</extracomment>
            <translation>今週</translation>
        </message>
        <message>
            <source>This month</source>
            <extracomment>Upload date filter: this month</extracomment>
            <translation>今月</translation>
        </message>
        <message>
            <source>This year</source>
            <extracomment>Upload date filter: this year</extracomment>
            <translation>今年</translation>
        </message>
        <message>
            <source>Type</source>
            <extracomment>Search filter: kind of result</extracomment>
            <translation>種類</translation>
        </message>
        <message>
            <source>Any</source>
            <extracomment>Result type filter: any type</extracomment>
            <translation>すべて</translation>
        </message>
        <message>
            <source>Videos</source>
            <extracomment>Result type filter: videos</extracomment>
            <translation>動画</translation>
        </message>
        <message>
            <source>Channels</source>
            <extracomment>Result type filter: channels</extracomment>
            <translation>チャンネル</translation>
        </message>
        <message>
            <source>Playlists</source>
            <extracomment>Result type filter: playlists</extracomment>
            <translation>再生リスト</translation>
        </message>
        <message>
            <source>Length</source>
            <extracomment>Search filter: video length</extracomment>
            <translation>長さ</translation>
        </message>
        <message>
            <source>Any length</source>
            <extracomment>Duration filter: any length</extracomment>
            <translation>指定なし</translation>
        </message>
        <message>
            <source>Under 4 minutes</source>
            <extracomment>Duration filter: under 4 minutes</extracomment>
            <translation>4 分未満</translation>
        </message>
        <message>
            <source>4-20 minutes</source>
            <extracomment>Duration filter: 4 to 20 minutes</extracomment>
            <translation>4〜20 分</translation>
        </message>
        <message>
            <source>Over 20 minutes</source>
            <extracomment>Duration filter: over 20 minutes</extracomment>
            <translation>20 分以上</translation>
        </message>
        <message>
            <source>Live now</source>
            <extracomment>Search filter: only live streams</extracomment>
            <translation>ライブ配信中</translation>
        </message>
        <message>
            <source>Mixed results</source>
            <extracomment>Search option: channels and playlists among the results when no type is chosen</extracomment>
            <translation>混在した結果</translation>
        </message>
        <message>
            <source>Show channels and playlists together with videos.</source>
            <translation>チャンネルと再生リストも動画と一緒に表示します。</translation>
        </message>
        <message>
            <source>Hide</source>
            <extracomment>Section of the search filter dialog about hidden rows</extracomment>
            <translation>非表示</translation>
        </message>
        <message>
            <source>Hide Shorts</source>
            <extracomment>Search filter switch: leave Shorts out of the results</extracomment>
            <translation>ショートを非表示</translation>
        </message>
        <message>
            <source>Hide members-only videos</source>
            <extracomment>Search filter switch: leave members-only videos out of the results</extracomment>
            <translation>メンバー限定動画を非表示</translation>
        </message>
        <message>
            <source>Search history</source>
            <extracomment>Section of the search filter dialog about the local search history</extracomment>
            <translation>検索履歴</translation>
        </message>
        <message>
            <source>Pause search history</source>
            <extracomment>Stops recording new searches in the local history</extracomment>
            <translation>検索履歴を一時停止</translation>
        </message>
        <message>
            <source>Clear search history</source>
            <extracomment>Deletes the local search history</extracomment>
            <translation>検索履歴を消去</translation>
        </message>
    </context>
    <context>
        <name>SearchView</name>
        <message>
            <source>Rating</source>
            <extracomment>Search filter summary: sorted by rating</extracomment>
            <translation>評価</translation>
        </message>
        <message>
            <source>Upload date</source>
            <extracomment>Search filter summary: sorted by upload date</extracomment>
            <translation>アップロード日</translation>
        </message>
        <message>
            <source>View count</source>
            <extracomment>Search filter summary: sorted by view count</extracomment>
            <translation>視聴回数</translation>
        </message>
        <message>
            <source>Last hour</source>
            <extracomment>Search filter summary: uploaded in the last hour</extracomment>
            <translation>1 時間以内</translation>
        </message>
        <message>
            <source>Today</source>
            <extracomment>Search filter summary: uploaded today</extracomment>
            <translation>今日</translation>
        </message>
        <message>
            <source>This week</source>
            <extracomment>Search filter summary: uploaded this week</extracomment>
            <translation>今週</translation>
        </message>
        <message>
            <source>This month</source>
            <extracomment>Search filter summary: uploaded this month</extracomment>
            <translation>今月</translation>
        </message>
        <message>
            <source>This year</source>
            <extracomment>Search filter summary: uploaded this year</extracomment>
            <translation>今年</translation>
        </message>
        <message>
            <source>Videos</source>
            <extracomment>Search filter summary: only videos</extracomment>
            <translation>動画</translation>
        </message>
        <message>
            <source>Channels</source>
            <extracomment>Search filter summary: only channels</extracomment>
            <translation>チャンネル</translation>
        </message>
        <message>
            <source>Playlists</source>
            <extracomment>Search filter summary: only playlists</extracomment>
            <translation>再生リスト</translation>
        </message>
        <message>
            <source>Under 4 minutes</source>
            <extracomment>Search filter summary: videos under 4 minutes</extracomment>
            <translation>4 分未満</translation>
        </message>
        <message>
            <source>Over 20 minutes</source>
            <extracomment>Search filter summary: videos over 20 minutes</extracomment>
            <translation>20 分以上</translation>
        </message>
        <message>
            <source>4-20 minutes</source>
            <extracomment>Search filter summary: videos of 4 to 20 minutes</extracomment>
            <translation>4〜20 分</translation>
        </message>
        <message>
            <source>Live</source>
            <extracomment>Search filter summary: only live streams</extracomment>
            <translation>ライブ</translation>
        </message>
        <message>
            <source>Search</source>
            <extracomment>Title of the search page</extracomment>
            <translation>検索</translation>
        </message>
        <message>
            <source>Search YouTube</source>
            <extracomment>Placeholder of the search field</extracomment>
            <translation>YouTube を検索</translation>
        </message>
        <message>
            <source>Filters: %1</source>
            <extracomment>Search filter entry while filters are active, %1 lists them</extracomment>
            <translation>フィルター: %1</translation>
        </message>
        <message>
            <source>Filters</source>
            <extracomment>Search filter entry when no filter is active</extracomment>
            <translation>フィルター</translation>
        </message>
        <message>
            <source>Remove from history</source>
            <extracomment>Removes one entry from the search history</extracomment>
            <translation>履歴から削除</translation>
        </message>
        <message>
            <source>Library</source>
            <extracomment>Menu entry opening the local library</extracomment>
            <translation>ライブラリ</translation>
        </message>
        <message>
            <source>Settings</source>
            <extracomment>Menu entry opening the application settings</extracomment>
            <translation>設定</translation>
        </message>
        <message>
            <source>About SailTube</source>
            <extracomment>Menu entry opening the about page</extracomment>
            <translation>SailTube について</translation>
        </message>
        <message>
            <source>Search filters</source>
            <extracomment>Menu entry opening the search filters</extracomment>
            <translation>検索フィルター</translation>
        </message>
        <message>
            <source>Now playing</source>
            <extracomment>Menu entry returning to the video that is playing</extracomment>
            <translation>再生中</translation>
        </message>
        <message>
            <source>Members only · %1</source>
            <extracomment>Prefix of a members-only row, %1 is the channel name</extracomment>
            <translation>メンバー限定 · %1</translation>
        </message>
        <message>
            <source>UPCOMING</source>
            <extracomment>Badge on the thumbnail of a scheduled live stream or premiere</extracomment>
            <translation>公開予定</translation>
        </message>
        <message>
            <source>More results</source>
            <extracomment>Loads the next page of search results</extracomment>
            <translation>その他の結果</translation>
        </message>
        <message numerus="yes">
            <source>%n result(s) hidden (Shorts, members-only)</source>
            <extracomment>Footer of the search results, %n is the number of hidden Shorts and members-only rows</extracomment>
            <translation>
                <numerusform>%n 件の結果を非表示 (ショート、メンバー限定)</numerusform>
            </translation>
        </message>
        <message>
            <source>All results are hidden</source>
            <extracomment>Shown when every search result was hidden by the Shorts / members-only settings</extracomment>
            <translation>すべての結果が非表示になっています</translation>
        </message>
        <message>
            <source>No results found</source>
            <extracomment>Shown when a search found nothing</extracomment>
            <translation>結果が見つかりません</translation>
        </message>
        <message>
            <source>Find videos</source>
            <extracomment>Shown on the empty search page</extracomment>
            <translation>動画を探す</translation>
        </message>
        <message>
            <source>Search again to retry.</source>
            <translation>もう一度検索して再試行してください。</translation>
        </message>
        <message>
            <source>Type a search above and press Enter.</source>
            <translation>上に検索語を入力して Enter を押してください。</translation>
        </message>
        <message>
            <source>Try fewer filters.</source>
            <extracomment>Hint when a filtered search found nothing</extracomment>
            <translation>フィルターを減らしてみてください。</translation>
        </message>
        <message>
            <source>All</source>
            <extracomment>Search filter chip: every kind of result</extracomment>
            <translation>すべて</translation>
        </message>
    </context>
    <context>
        <name>SeekBar</name>
        <message>
            <source>Seek</source>
            <extracomment>Accessible name of the seek bar</extracomment>
            <translation>再生位置</translation>
        </message>
    </context>
    <context>
        <name>SettingLink</name>
        <message>
            <source>Not available yet</source>
            <extracomment>Shown under a settings row whose page is not in this build yet</extracomment>
            <translation>未対応</translation>
        </message>
    </context>
    <context>
        <name>SettingsPage</name>
        <message>
            <source>No limit</source>
            <extracomment>Maximum quality on mobile data without a limit</extracomment>
            <translation>制限なし</translation>
        </message>
        <message>
            <source>Settings</source>
            <extracomment>Settings
----------
Title of the settings page</extracomment>
            <translation>設定</translation>
        </message>
        <message>
            <source>Content</source>
            <extracomment>Settings section about what lists show</extracomment>
            <translation>コンテンツ</translation>
        </message>
        <message>
            <source>Start page</source>
            <extracomment>Setting: which main page section opens at start</extracomment>
            <translation>起動時のページ</translation>
        </message>
        <message>
            <source>Subscriptions</source>
            <extracomment>Start page choice: the subscriptions feed</extracomment>
            <translation>登録チャンネル</translation>
        </message>
        <message>
            <source>Recommended</source>
            <extracomment>Start page choice: recommended videos (needs an account)</extracomment>
            <translation>おすすめ</translation>
        </message>
        <message>
            <source>Search</source>
            <extracomment>Start page choice: search</extracomment>
            <translation>検索</translation>
        </message>
        <message>
            <source>Hide Shorts</source>
            <extracomment>Setting: leave Shorts out of lists</extracomment>
            <translation>ショートを非表示</translation>
        </message>
        <message>
            <source>In search, channel lists and the feed.</source>
            <extracomment>Explanation of the hide Shorts setting</extracomment>
            <translation>検索、チャンネル一覧、フィードで適用されます。</translation>
        </message>
        <message>
            <source>Hide watched videos</source>
            <extracomment>Setting: leave watched videos out of the feed</extracomment>
            <translation>視聴済みの動画を非表示</translation>
        </message>
        <message>
            <source>In the feed and channel lists.</source>
            <extracomment>Explanation of the hide watched setting</extracomment>
            <translation>フィードとチャンネル一覧で適用されます。</translation>
        </message>
        <message>
            <source>History</source>
            <extracomment>Settings section about the watch history</extracomment>
            <translation>履歴</translation>
        </message>
        <message>
            <source>History size</source>
            <extracomment>Setting: how many videos the watch history keeps</extracomment>
            <translation>履歴の件数</translation>
        </message>
        <message>
            <source>Pause history</source>
            <extracomment>Setting: stop recording watched videos</extracomment>
            <translation>履歴を一時停止</translation>
        </message>
        <message>
            <source>Playback</source>
            <extracomment>Settings section about video playback</extracomment>
            <translation>再生</translation>
        </message>
        <message>
            <source>Resume videos</source>
            <extracomment>Setting: continue a video where it was left</extracomment>
            <translation>動画の再開</translation>
        </message>
        <message>
            <source>Always</source>
            <extracomment>Resume choice: always continue from the saved position
----------
Comment loading choice: as soon as the video opens</extracomment>
            <translation>常に</translation>
        </message>
        <message>
            <source>Ask</source>
            <extracomment>Resume choice: ask each time</extracomment>
            <translation>確認する</translation>
        </message>
        <message>
            <source>Never</source>
            <extracomment>Resume choice: always start from the beginning</extracomment>
            <translation>しない</translation>
        </message>
        <message>
            <source>Keep display on</source>
            <extracomment>Setting: keep the display on while a video plays</extracomment>
            <translation>画面を点灯したままにする</translation>
        </message>
        <message>
            <source>While a video plays in the foreground.</source>
            <extracomment>Explanation of the keep display on setting</extracomment>
            <translation>動画をフォアグラウンドで再生している間。</translation>
        </message>
        <message>
            <source>Background playback</source>
            <extracomment>Setting: keep the sound playing after leaving the video</extracomment>
            <translation>バックグラウンド再生</translation>
        </message>
        <message>
            <source>Keeps the sound playing when you leave the video or lock the screen.</source>
            <extracomment>Explanation of the background playback setting</extracomment>
            <translation>動画から離れたり画面をロックしたりしても音声の再生を続けます。</translation>
        </message>
        <message>
            <source>Vertical videos in fullscreen</source>
            <extracomment>Setting: how vertical videos and Shorts go fullscreen</extracomment>
            <translation>縦型動画の全画面表示</translation>
        </message>
        <message>
            <source>Upright</source>
            <extracomment>Vertical-video fullscreen choice: stay upright</extracomment>
            <translation>縦向き</translation>
        </message>
        <message>
            <source>Sideways</source>
            <extracomment>Vertical-video fullscreen choice: turn sideways like other videos</extracomment>
            <translation>横向き</translation>
        </message>
        <message>
            <source>Edge glow on the video</source>
            <extracomment>Setting: the glowing page indicator at the screen edge on the watch page</extracomment>
            <translation>動画上のエッジグロー</translation>
        </message>
        <message>
            <source>Automatic</source>
            <extracomment>Maximum quality choice: chosen by the app for this device and decoder
----------
Edge glow choice: hide it while watching, show it when paused
----------
Codec preference: let the application decide
----------
Adaptive streams chosen automatically
----------
Player client choice: automatic</extracomment>
            <translation>自動</translation>
        </message>
        <message>
            <source>Always hidden</source>
            <extracomment>Edge glow choice: never show it on the watch page</extracomment>
            <translation>常に非表示</translation>
        </message>
        <message>
            <source>Swiping back works either way.</source>
            <extracomment>Explanation of the edge glow setting</extracomment>
            <translation>どちらの場合もスワイプで戻れます。</translation>
        </message>
        <message>
            <source>Autoplay next video</source>
            <extracomment>Setting: play a related video after the current one ends</extracomment>
            <translation>次の動画を自動再生</translation>
        </message>
        <message>
            <source>Starts after a short countdown that you can cancel.</source>
            <extracomment>Explanation of the autoplay setting</extracomment>
            <translation>キャンセルできる短いカウントダウンの後に始まります。</translation>
        </message>
        <message>
            <source>Double-tap seek</source>
            <extracomment>Setting: how far a double tap on the video seeks</extracomment>
            <translation>ダブルタップでシーク</translation>
        </message>
        <message>
            <source>%1 s</source>
            <extracomment>Seek step in seconds, %1 is a number</extracomment>
            <translation>%1 秒</translation>
        </message>
        <message>
            <source>Load comments</source>
            <extracomment>Setting: when comments are loaded on the watch page</extracomment>
            <translation>コメントの読み込み</translation>
        </message>
        <message>
            <source>When near</source>
            <extracomment>Comment loading choice: when scrolled near them</extracomment>
            <translation>近づいたとき</translation>
        </message>
        <message>
            <source>On request</source>
            <extracomment>Comment loading choice: only on request</extracomment>
            <translation>リクエスト時</translation>
        </message>
        <message>
            <source>Quality</source>
            <extracomment>Settings section about video quality and decoding</extracomment>
            <translation>画質</translation>
        </message>
        <message>
            <source>Maximum quality on Wi-Fi</source>
            <extracomment>Setting: tallest picture chosen automatically on Wi-Fi</extracomment>
            <translation>Wi-Fi での最高画質</translation>
        </message>
        <message>
            <source>Maximum quality on mobile data</source>
            <extracomment>Setting: tallest picture chosen automatically on mobile data</extracomment>
            <translation>モバイルデータでの最高画質</translation>
        </message>
        <message>
            <source>Hardware decoding</source>
            <extracomment>Setting: decode video with the device's hardware decoder</extracomment>
            <translation>ハードウェアデコード</translation>
        </message>
        <message>
            <source>No hardware decoder was found on this device.</source>
            <extracomment>Shown when the device has no usable hardware decoder</extracomment>
            <translation>この端末にはハードウェアデコーダーが見つかりませんでした。</translation>
        </message>
        <message>
            <source>Preferred codec</source>
            <extracomment>Setting: which video codec is preferred when several are playable</extracomment>
            <translation>優先コーデック</translation>
        </message>
        <message>
            <source>Automatic prefers codecs that this device decodes in hardware.</source>
            <extracomment>Explanation under the codec preference setting</extracomment>
            <translation>「自動」では、この端末がハードウェアでデコードできるコーデックを優先します。</translation>
        </message>
        <message>
            <source>High frame rate</source>
            <extracomment>Setting: allow 50/60 frames per second streams</extracomment>
            <translation>高フレームレート</translation>
        </message>
        <message>
            <source>Audio only</source>
            <extracomment>Download quality choice: sound only</extracomment>
            <translation>音声のみ</translation>
        </message>
        <message>
            <source>Adaptive streams</source>
            <extracomment>Setting: how separate audio and video streams are combined</extracomment>
            <translation>アダプティブストリーム</translation>
        </message>
        <message>
            <source>Off</source>
            <extracomment>SponsorBlock category mode: never act on this category
----------
Adaptive streams disabled: only combined (progressive) streams</extracomment>
            <translation>オフ</translation>
        </message>
        <message>
            <source>Always use playbin3</source>
            <extracomment>Adaptive streams: always use the newer GStreamer pipeline (diagnostics)</extracomment>
            <translation>常に playbin3 を使用</translation>
        </message>
        <message>
            <source>Higher qualities need separate audio and video streams. Off limits playback to combined streams (usually 360p).</source>
            <extracomment>Explanation of the adaptive streams setting</extracomment>
            <translation>高画質には音声と映像が別々のストリームが必要です。オフにすると、再生は結合済みストリーム（通常 360p）に限られます。</translation>
        </message>
        <message>
            <source>Sound and captions</source>
            <extracomment>Settings section about sound</extracomment>
            <translation>音声と字幕</translation>
        </message>
        <message>
            <source>Equalizer</source>
            <extracomment>Settings row opening the equalizer</extracomment>
            <translation>イコライザー</translation>
        </message>
        <message>
            <source>Volume boost</source>
            <extracomment>Setting: amplify quiet videos</extracomment>
            <translation>音量ブースト</translation>
        </message>
        <message>
            <source>Loudness normalisation</source>
            <extracomment>Setting: even out loudness between videos</extracomment>
            <translation>ラウドネスの正規化</translation>
        </message>
        <message>
            <source>Caption style</source>
            <extracomment>Settings row opening the caption appearance page</extracomment>
            <translation>字幕のスタイル</translation>
        </message>
        <message>
            <source>Community services</source>
            <extracomment>Settings section about community services</extracomment>
            <translation>コミュニティサービス</translation>
        </message>
        <message>
            <source>SponsorBlock</source>
            <extracomment>Settings section about SponsorBlock segment skipping
----------
Setting: skip or mark community-submitted segments such as sponsors</extracomment>
            <translation>SponsorBlock</translation>
        </message>
        <message>
            <source>Show dislike estimates</source>
            <extracomment>Setting: show estimated dislike counts from a community service</extracomment>
            <translation>低評価数の推定値を表示</translation>
        </message>
        <message>
            <source>Asks the Return YouTube Dislike service for each video you open.</source>
            <extracomment>Explanation of the dislike estimate setting</extracomment>
            <translation>開いた動画ごとに Return YouTube Dislike サービスに問い合わせます。</translation>
        </message>
        <message>
            <source>Downloads</source>
            <extracomment>Settings section about downloads</extracomment>
            <translation>ダウンロード</translation>
        </message>
        <message>
            <source>Download folder</source>
            <extracomment>Settings row choosing where downloads are saved</extracomment>
            <translation>ダウンロード先フォルダー</translation>
        </message>
        <message>
            <source>Download quality</source>
            <extracomment>Setting: quality of downloaded videos</extracomment>
            <translation>ダウンロード画質</translation>
        </message>
        <message>
            <source>Download on Wi-Fi only</source>
            <extracomment>Setting: download only on Wi-Fi or other unmetered networks</extracomment>
            <translation>Wi-Fi 接続時のみダウンロード</translation>
        </message>
        <message>
            <source>Providers</source>
            <extracomment>Settings section about the video resolver</extracomment>
            <translation>プロバイダー</translation>
        </message>
        <message>
            <source>Resolver updates</source>
            <extracomment>Setting: which yt-dlp release channel updates come from</extracomment>
            <translation>リゾルバーの更新</translation>
        </message>
        <message>
            <source>Stable</source>
            <extracomment>Resolver update channel: stable releases</extracomment>
            <translation>安定版</translation>
        </message>
        <message>
            <source>Nightly</source>
            <extracomment>Resolver update channel: nightly builds</extracomment>
            <translation>ナイトリー</translation>
        </message>
        <message>
            <source>Proof-of-origin tokens</source>
            <extracomment>Setting: use a proof-of-origin token provider</extracomment>
            <translation>Proof-of-origin トークン</translation>
        </message>
        <message>
            <source>Diagnostics</source>
            <extracomment>Settings section with diagnostic options</extracomment>
            <translation>診断</translation>
        </message>
        <message>
            <source>Player client</source>
            <extracomment>Setting: force a specific YouTube client for diagnostics</extracomment>
            <translation>プレーヤークライアント</translation>
        </message>
        <message>
            <source>For diagnostics only.</source>
            <extracomment>Explanation of the player client setting</extracomment>
            <translation>診断専用です。</translation>
        </message>
        <message>
            <source>Timing log</source>
            <extracomment>Setting: write a timing log for performance measurements</extracomment>
            <translation>タイミングログ</translation>
        </message>
        <message>
            <source>Records start-up and playback timings on this device. Never uploaded.</source>
            <extracomment>Explanation of the timing log setting</extracomment>
            <translation>この端末での起動と再生のタイミングを記録します。アップロードされることはありません。</translation>
        </message>
        <message>
            <source>Video capabilities</source>
            <extracomment>Opens the page listing the device's decoders</extracomment>
            <translation>動画機能</translation>
        </message>
        <message>
            <source>Uses the phone's video decoder. Turn off if videos stay black or stall.</source>
            <extracomment>Explanation of the hardware decoding setting</extracomment>
            <translation>端末の動画デコーダーを使用します。動画が黒いままになったり止まったりする場合はオフにしてください。</translation>
        </message>
        <message>
            <source>Restart SailTube to apply the decoder settings.</source>
            <extracomment>Shown after changing a setting that applies at start-up</extracomment>
            <translation>デコーダーの設定を適用するには SailTube を再起動してください。</translation>
        </message>
        <message>
            <source>Also in search results</source>
            <extracomment>Setting: also hide watched videos from search results</extracomment>
            <translation>検索結果でも適用</translation>
        </message>
        <message>
            <source>App folder</source>
            <extracomment>Download folder row: the app's private folder is used</extracomment>
            <translation>アプリのフォルダー</translation>
        </message>
        <message>
            <source>YouTube account</source>
            <extracomment>Settings entry opening the optional YouTube account page</extracomment>
            <translation>YouTube アカウント</translation>
        </message>
        <message>
            <source>Skip</source>
            <extracomment>SponsorBlock category mode: skip the segment automatically</extracomment>
            <translation>スキップ</translation>
        </message>
        <message>
            <source>Show button</source>
            <extracomment>SponsorBlock category mode: show a skip button during the segment</extracomment>
            <translation>ボタンを表示</translation>
        </message>
        <message>
            <source>Keep playing after leaving the video</source>
            <extracomment>Setting: keep the video loaded after leaving the watch page</extracomment>
            <translation>動画から離れても再生を続ける</translation>
        </message>
        <message>
            <source>The mini-player stays at the bottom of the app.</source>
            <extracomment>Explanation of the keep session setting</extracomment>
            <translation>ミニプレーヤーがアプリの下部に表示されたままになります。</translation>
        </message>
        <message>
            <source>Resume after calls and alarms</source>
            <extracomment>Setting: continue playing after a call or an alarm</extracomment>
            <translation>通話やアラームの後に再開</translation>
        </message>
        <message>
            <source>Sponsor</source>
            <extracomment>SponsorBlock category: paid promotion inside a video</extracomment>
            <translation>スポンサー</translation>
        </message>
        <message>
            <source>Unpaid or self promotion</source>
            <extracomment>SponsorBlock category: unpaid or self promotion</extracomment>
            <translation>無償の宣伝または自己宣伝</translation>
        </message>
        <message>
            <source>Interaction reminder</source>
            <extracomment>SponsorBlock category: reminder to like, subscribe or follow</extracomment>
            <translation>インタラクションの呼びかけ</translation>
        </message>
        <message>
            <source>Intermission or intro</source>
            <extracomment>SponsorBlock category: intermission or intro animation</extracomment>
            <translation>休憩またはイントロ</translation>
        </message>
        <message>
            <source>End cards and credits</source>
            <extracomment>SponsorBlock category: end cards and credits</extracomment>
            <translation>エンドカードとクレジット</translation>
        </message>
        <message>
            <source>Preview or recap</source>
            <extracomment>SponsorBlock category: preview or recap of other content</extracomment>
            <translation>予告または振り返り</translation>
        </message>
        <message>
            <source>Non-music section</source>
            <extracomment>SponsorBlock category: non-music part of a music video</extracomment>
            <translation>音楽以外の部分</translation>
        </message>
        <message>
            <source>Filler tangent</source>
            <extracomment>SponsorBlock category: tangent or joke that is not needed</extracomment>
            <translation>本筋から外れた話</translation>
        </message>
        <message>
            <source>Highlight</source>
            <extracomment>SponsorBlock category: the highlight of a video, offered as a jump</extracomment>
            <translation>ハイライト</translation>
        </message>
        <message>
            <source>Show jump button</source>
            <extracomment>SponsorBlock highlight mode: offer a "jump to highlight" button</extracomment>
            <translation>ジャンプボタンを表示</translation>
        </message>
        <message>
            <source>Uses segments that viewers submit to the SponsorBlock community.</source>
            <extracomment>Explanation of the SponsorBlock master switch</extracomment>
            <translation>視聴者が SponsorBlock コミュニティに投稿した区間を使用します。</translation>
        </message>
        <message>
            <source>Video on the cover</source>
            <extracomment>Setting: show the playing video live on the app cover in the home screen</extracomment>
            <translation>カバーに動画を表示</translation>
        </message>
        <message>
            <source>While a video plays, the app cover shows it live. Uses more battery while the home screen is visible.</source>
            <extracomment>Explanation of the cover video setting</extracomment>
            <translation>動画の再生中、アプリのカバーにライブで表示します。ホーム画面が表示されている間はバッテリーを多く使います。</translation>
        </message>
        <message>
            <source>MiB</source>
            <extracomment>Unit after the memory limit value, mebibytes</extracomment>
            <translation>MiB</translation>
        </message>
        <message>
            <source>Token helper memory limit</source>
            <extracomment>Setting: the most memory the proof-of-origin token helper may use</extracomment>
            <translation>トークンヘルパーのメモリ上限</translation>
        </message>
        <message>
            <source>Start at low quality, then switch up</source>
            <extracomment>Setting (experiment): start videos at a low quality, then switch to the chosen one</extracomment>
            <translation>低画質で開始してから切り替える</translation>
        </message>
        <message>
            <source>A start-up experiment: the picture may appear sooner, then sharpens once.</source>
            <extracomment>Explanation of the low-quality start experiment</extracomment>
            <translation>起動の実験：映像が早く表示され、その後一度だけ鮮明になります。</translation>
        </message>
        <message>
            <source>Stable volume</source>
            <extracomment>Setting: prefer the audio version that keeps the volume steady within a video</extracomment>
            <translation>音量の安定化</translation>
        </message>
        <message>
            <source>Uses the steadier audio version of a video when YouTube offers one.</source>
            <extracomment>Explanation of the stable volume setting</extracomment>
            <translation>YouTube が提供している場合、音量が安定した音声を使います。</translation>
        </message>
    </context>
    <context>
        <name>ShortsView</name>
        <message>
            <source>No Shorts</source>
            <extracomment>Shown in the Shorts section when the subscribed channels have no Shorts</extracomment>
            <translation>ショートはありません</translation>
        </message>
        <message>
            <source>Shorts from your subscriptions appear here.</source>
            <extracomment>Hint in the empty Shorts section</extracomment>
            <translation>登録チャンネルのショートがここに表示されます。</translation>
        </message>
        <message>
            <source>Like</source>
            <extracomment>Accessible name of the like button of a Short that shows no count</extracomment>
            <translation>高評価</translation>
        </message>
        <message>
            <source>Liking videos is not available yet</source>
            <extracomment>Shown over a Short when its like button is tapped: SailTube cannot like videos yet</extracomment>
            <translation>高評価はまだ使えません</translation>
        </message>
    </context>
    <context>
        <name>SubscriptionsPage</name>
        <message>
            <source>Sort by latest upload</source>
            <extracomment>Menu entry sorting subscriptions by their newest upload</extracomment>
            <translation>新着アップロード順</translation>
        </message>
        <message>
            <source>Sort A–Z</source>
            <extracomment>Menu entry sorting subscriptions alphabetically</extracomment>
            <translation>名前順 (A–Z)</translation>
        </message>
        <message>
            <source>Subscriptions</source>
            <extracomment>Title of the subscriptions page</extracomment>
            <translation>登録チャンネル</translation>
        </message>
        <message numerus="yes">
            <source>%n channel(s) waiting to be looked up</source>
            <extracomment>%n channels imported by handle that still have to be looked up online</extracomment>
            <translation>
                <numerusform>%n 件のチャンネルが照会待ちです</numerusform>
            </translation>
        </message>
        <message>
            <source>Unsubscribe</source>
            <extracomment>Context menu entry removing a channel subscription</extracomment>
            <translation>登録解除</translation>
        </message>
        <message>
            <source>Unsubscribing</source>
            <extracomment>Remorse text while a subscription is being removed</extracomment>
            <translation>登録解除中</translation>
        </message>
        <message>
            <source>No subscriptions</source>
            <extracomment>Empty subscriptions page</extracomment>
            <translation>登録チャンネルはありません</translation>
        </message>
        <message>
            <source>Subscribe from a channel or video page, or import a backup.</source>
            <extracomment>Hint on the empty subscriptions page</extracomment>
            <translation>チャンネルや動画のページから登録するか、バックアップをインポートしてください。</translation>
        </message>
    </context>
    <context>
        <name>VideoDetailsPage</name>
        <message>
            <source>Open in browser</source>
            <extracomment>Menu entry opening the in-browser YouTube page of the video</extracomment>
            <translation>ブラウザで開く</translation>
        </message>
        <message>
            <source>Copy link</source>
            <extracomment>Button copying the video link to the clipboard</extracomment>
            <translation>リンクをコピー</translation>
        </message>
        <message>
            <source>Share</source>
            <extracomment>Button sharing the video link through the system share sheet</extracomment>
            <translation>共有</translation>
        </message>
        <message>
            <source>Play</source>
            <extracomment>Menu entry starting playback of the video shown in the details view
----------
Accessible name of the play button</extracomment>
            <translation>再生</translation>
        </message>
        <message>
            <source>%1 subscribers</source>
            <extracomment>Subscriber count of a channel, %1 is a compact number such as 1.2M</extracomment>
            <translation>登録者 %1 人</translation>
        </message>
        <message>
            <source>Live now</source>
            <extracomment>Shown for a live stream instead of a view count</extracomment>
            <translation>ライブ配信中</translation>
        </message>
        <message>
            <source>%1 views</source>
            <extracomment>View count of a video, %1 is a compact number such as 1.2M</extracomment>
            <translation>%1 回視聴</translation>
        </message>
        <message>
            <source>%1 likes</source>
            <extracomment>Like count of a video, %1 is a compact number</extracomment>
            <translation>高評価 %1 件</translation>
        </message>
        <message>
            <source>Description</source>
            <extracomment>Section header above the video description</extracomment>
            <translation>説明</translation>
        </message>
        <message>
            <source>Show more</source>
            <extracomment>Expands the collapsed video description</extracomment>
            <translation>もっと見る</translation>
        </message>
        <message>
            <source>Show less</source>
            <extracomment>Collapses the video description to 5 lines</extracomment>
            <translation>一部を表示</translation>
        </message>
        <message numerus="yes">
            <source>%n chapter(s)</source>
            <extracomment>Number of chapters of a video</extracomment>
            <translation>
                <numerusform>%n 個のチャプター</numerusform>
            </translation>
        </message>
        <message>
            <source>Related videos</source>
            <extracomment>Section header above the related videos</extracomment>
            <translation>関連動画</translation>
        </message>
    </context>
    <context>
        <name>VideoListItem</name>
        <message>
            <source>LIVE</source>
            <extracomment>Badge on the thumbnail of a live stream</extracomment>
            <translation>ライブ</translation>
        </message>
        <message>
            <source>%1 views</source>
            <extracomment>Number of views of a video, %1 is a compact number such as 1.2M</extracomment>
            <translation>%1 回視聴</translation>
        </message>
    </context>
    <context>
        <name>VideoMenuItems</name>
        <message>
            <source>Add to playlist</source>
            <extracomment>Context menu entry adding a video to a playlist</extracomment>
            <translation>再生リストに追加</translation>
        </message>
        <message>
            <source>Remove from Watch later</source>
            <extracomment>Context menu entry removing a video from the Watch later list</extracomment>
            <translation>「後で見る」から削除</translation>
        </message>
        <message>
            <source>Watch later</source>
            <extracomment>Context menu entry adding a video to the Watch later list</extracomment>
            <translation>後で見る</translation>
        </message>
        <message>
            <source>Mark as not watched</source>
            <extracomment>Context menu entry clearing the watched mark of a video</extracomment>
            <translation>未視聴にする</translation>
        </message>
        <message>
            <source>Mark as watched</source>
            <extracomment>Context menu entry marking a video as watched</extracomment>
            <translation>視聴済みにする</translation>
        </message>
        <message>
            <source>Unsubscribe from %1</source>
            <extracomment>Context menu entry unsubscribing from the channel of a video</extracomment>
            <translation>%1 の登録を解除</translation>
        </message>
        <message>
            <source>Subscribe to %1</source>
            <extracomment>Context menu entry subscribing to the channel of a video</extracomment>
            <translation>%1 を登録</translation>
        </message>
        <message>
            <source>Download audio</source>
            <extracomment>Context menu entry downloading a video's audio track</extracomment>
            <translation>音声をダウンロード</translation>
        </message>
        <message>
            <source>Download video</source>
            <extracomment>Context menu entry downloading a video</extracomment>
            <translation>動画をダウンロード</translation>
        </message>
        <message>
            <source>Removing from Watch later</source>
            <extracomment>Remorse text while a video is being removed from the Watch later list</extracomment>
            <translation>「後で見る」から削除しています</translation>
        </message>
        <message>
            <source>Unsubscribing</source>
            <extracomment>Remorse text while a subscription is being removed</extracomment>
            <translation>登録解除中</translation>
        </message>
        <message>
            <source>Details</source>
            <extracomment>Context menu entry opening a video's details without playing it</extracomment>
            <translation>詳細</translation>
        </message>
    </context>
    <context>
        <name>VideoPage</name>
        <message>
            <source>Open setting</source>
            <extracomment>Button under a playback error that opens the setting the error refers to</extracomment>
            <translation>設定を開く</translation>
        </message>
        <message>
            <source>Retry</source>
            <extracomment>Button under a playback error that tries the video again
----------
Menu entry retrying a failed video</extracomment>
            <translation>再試行</translation>
        </message>
        <message>
            <source>Play video</source>
            <extracomment>Menu entry switching from audio-only back to video</extracomment>
            <translation>動画を再生</translation>
        </message>
        <message>
            <source>Quality</source>
            <extracomment>Menu entry opening the video quality choice</extracomment>
            <translation>画質</translation>
        </message>
        <message>
            <source>Live now</source>
            <extracomment>Shown for a live stream instead of a view count</extracomment>
            <translation>ライブ配信中</translation>
        </message>
        <message>
            <source>%1 views</source>
            <extracomment>View count of a video, %1 is a compact number such as 1.2M</extracomment>
            <translation>%1 回視聴</translation>
        </message>
        <message>
            <source>%1 likes</source>
            <extracomment>Like count of a video, %1 is a compact number</extracomment>
            <translation>高評価 %1 件</translation>
        </message>
        <message>
            <source>Show less</source>
            <extracomment>Collapses the video description to 5 lines</extracomment>
            <translation>一部を表示</translation>
        </message>
        <message>
            <source>Show more</source>
            <extracomment>Expands the collapsed video description</extracomment>
            <translation>もっと見る</translation>
        </message>
        <message>
            <source>Chapters</source>
            <extracomment>Section header above the list of video chapters</extracomment>
            <translation>チャプター</translation>
        </message>
        <message>
            <source>Add to playlist</source>
            <extracomment>Menu entry adding the current video to a playlist</extracomment>
            <translation>再生リストに追加</translation>
        </message>
        <message>
            <source>Download</source>
            <extracomment>Menu entry opening the download choice for the current video</extracomment>
            <translation>ダウンロード</translation>
        </message>
        <message>
            <source>Subscribed</source>
            <extracomment>Button state of a channel the user is subscribed to</extracomment>
            <translation>登録済み</translation>
        </message>
        <message>
            <source>Subscribe</source>
            <extracomment>Button subscribing to the channel of the current video</extracomment>
            <translation>登録</translation>
        </message>
        <message>
            <source>Downloaded</source>
            <extracomment>Watch-page note for a video that is downloaded</extracomment>
            <translation>ダウンロード済み</translation>
        </message>
        <message>
            <source>Downloading %1 %</source>
            <extracomment>Watch-page note while a download runs, %1 is a percentage</extracomment>
            <translation>ダウンロード中 %1 %</translation>
        </message>
        <message>
            <source>%1 subscribers</source>
            <extracomment>Subscriber count of a channel, %1 is a compact number such as 1.2M</extracomment>
            <translation>登録者 %1 人</translation>
        </message>
        <message>
            <source>Share</source>
            <extracomment>Button sharing the video link through the system share sheet</extracomment>
            <translation>共有</translation>
        </message>
        <message>
            <source>Copy link</source>
            <extracomment>Button copying the video link to the clipboard</extracomment>
            <translation>リンクをコピー</translation>
        </message>
        <message>
            <source>Open in browser</source>
            <extracomment>Button opening the video on youtube.com in the web browser</extracomment>
            <translation>ブラウザで開く</translation>
        </message>
        <message>
            <source>Link at the current time</source>
            <extracomment>Switch adding the current playback time to shared and copied links</extracomment>
            <translation>現在の再生位置からのリンク</translation>
        </message>
        <message numerus="yes">
            <source>%n chapter(s) · now: %1</source>
            <extracomment>Chapter entry on the watch page: chapter count and the chapter playing now</extracomment>
            <translation>
                <numerusform>%n 個のチャプター · 再生中: %1</numerusform>
            </translation>
        </message>
        <message numerus="yes">
            <source>%n chapter(s)</source>
            <translation>
                <numerusform>%n 個のチャプター</numerusform>
            </translation>
        </message>
        <message>
            <source>Related videos</source>
            <extracomment>Section header above the related videos</extracomment>
            <translation>関連動画</translation>
        </message>
        <message>
            <source>More related videos</source>
            <extracomment>Opens the full list of related videos</extracomment>
            <translation>他の関連動画</translation>
        </message>
        <message>
            <source>Unsubscribing</source>
            <extracomment>Remorse text while a subscription is being removed</extracomment>
            <translation>登録解除中</translation>
        </message>
        <message>
            <source>Skipped: %1</source>
            <extracomment>Notice after a SponsorBlock segment was skipped, %1 is the category name</extracomment>
            <translation>スキップしました: %1</translation>
        </message>
        <message>
            <source>Skip %1</source>
            <extracomment>Button skipping a SponsorBlock segment, %1 is the category name</extracomment>
            <translation>%1 をスキップ</translation>
        </message>
        <message>
            <source>Up next in %1 s</source>
            <extracomment>Autoplay countdown, %1 is the number of seconds before the next video starts</extracomment>
            <translation>%1 秒後に次の動画</translation>
        </message>
        <message>
            <source>Cancel</source>
            <extracomment>Cancels the autoplay countdown</extracomment>
            <translation>キャンセル</translation>
        </message>
        <message>
            <source>Playing downloaded file</source>
            <extracomment>Watch-page note while the video plays from its downloaded file</extracomment>
            <translation>ダウンロードしたファイルを再生中</translation>
        </message>
        <message>
            <source>Stream instead</source>
            <extracomment>Button on the watch page that streams a video instead of playing its downloaded file</extracomment>
            <translation>ストリーミングで再生</translation>
        </message>
        <message>
            <source>%1 s</source>
            <extracomment>Running total of a double-tap seek, %1 is a signed number of seconds such as +20</extracomment>
            <translation>%1 秒</translation>
        </message>
        <message>
            <source>Unlock</source>
            <extracomment>Button ending the touch lock of the full screen player</extracomment>
            <translation>ロック解除</translation>
        </message>
        <message>
            <source>About %1 dislikes (estimate)</source>
            <extracomment>Estimated dislike count of a video, %1 is a compact number such as 1.2K</extracomment>
            <translation>低評価 約 %1 件（推定）</translation>
        </message>
    </context>
    <context>
        <name>WatchActions</name>
        <message>
            <source>Link copied</source>
            <extracomment>Feedback after the video link was copied to the clipboard</extracomment>
            <translation>リンクをコピーしました</translation>
        </message>
        <message>
            <source>Share link</source>
            <extracomment>Title of the system share sheet for a video link</extracomment>
            <translation>リンクを共有</translation>
        </message>
        <message>
            <source>Sharing is not available; link copied</source>
            <extracomment>Feedback when the share sheet is unavailable and the link was copied instead</extracomment>
            <translation>共有を利用できないため、リンクをコピーしました</translation>
        </message>
    </context>
    <context>
        <name>WatchOverlay</name>
        <message>
            <source>Watched</source>
            <extracomment>Badge on the thumbnail of a video the user has watched</extracomment>
            <translation>視聴済み</translation>
        </message>
    </context>
    <context>
        <name>nordtile::video::sailfish::AccountController</name>
        <message>
            <source>Not signed in</source>
            <extracomment>Account state: no session imported (the normal state; everything works without one)</extracomment>
            <translation>ログインしていません</translation>
        </message>
        <message>
            <source>Checking the session…</source>
            <extracomment>Account state while the session is being verified</extracomment>
            <translation>セッションを確認しています…</translation>
        </message>
        <message>
            <source>Signed in</source>
            <extracomment>Account state when the account name is not known</extracomment>
            <translation>ログイン済み</translation>
        </message>
        <message>
            <source>Signed in as %1</source>
            <extracomment>Account state, %1 is the account name</extracomment>
            <translation>%1 としてログイン中</translation>
        </message>
        <message>
            <source>The session has expired. Import it again.</source>
            <extracomment>Account state: YouTube no longer accepts the imported session</extracomment>
            <translation>セッションの有効期限が切れました。もう一度インポートしてください。</translation>
        </message>
        <message>
            <source>Could not check the session. It will be checked again.</source>
            <extracomment>Account state: the session could not be checked (offline or server error)</extracomment>
            <translation>セッションを確認できませんでした。後で再確認します。</translation>
        </message>
        <message>
            <source>No YouTube sign-in was found. Sign in at youtube.com first.</source>
            <extracomment>Import error: the text or browser data holds no YouTube sign-in</extracomment>
            <translation>YouTube のログイン情報が見つかりません。先に youtube.com にログインしてください。</translation>
        </message>
        <message>
            <source>Secure storage is not available on this device, so the session cannot be kept.</source>
            <extracomment>Import error (Harbour build): the Sailfish Secrets service is unavailable, so nothing is stored</extracomment>
            <translation>このデバイスではセキュアストレージを利用できないため、セッションを保存できません。</translation>
        </message>
        <message>
            <source>The session could not be saved.</source>
            <extracomment>Import error: the session could not be saved</extracomment>
            <translation>セッションを保存できませんでした。</translation>
        </message>
        <message>
            <source>The Browser has no saved sign-in. Open youtube.com in the Browser and sign in.</source>
            <extracomment>Import error: the Sailfish Browser has no cookie database yet</extracomment>
            <translation>ブラウザに保存されたログイン情報がありません。ブラウザで youtube.com を開いてログインしてください。</translation>
        </message>
        <message>
            <source>The Browser data could not be read. Close the Browser and try again.</source>
            <extracomment>Import error: the Browser's cookie database could not be read</extracomment>
            <translation>ブラウザのデータを読み取れませんでした。ブラウザを閉じてもう一度お試しください。</translation>
        </message>
        <message>
            <source>No YouTube sign-in was found in the Browser. Sign in at youtube.com in a normal tab, not a private one.</source>
            <extracomment>Import error: no YouTube sign-in in the Browser; private tabs keep none</extracomment>
            <translation>ブラウザに YouTube のログイン情報が見つかりません。プライベートタブではなく通常のタブで youtube.com にログインしてください。</translation>
        </message>
        <message>
            <source>Importing pasted sessions is not available yet.</source>
            <extracomment>Import error: pasting a session is not available in this build yet</extracomment>
            <translation>貼り付けたセッションのインポートはまだ利用できません。</translation>
        </message>
        <message>
            <source>The file could not be read.</source>
            <extracomment>Import error: the chosen file could not be read or is too large</extracomment>
            <translation>ファイルを読み取れませんでした。</translation>
        </message>
        <message>
            <source>These cookies hold no YouTube sign-in. Copy them while signed in at youtube.com.</source>
            <extracomment>Import error: the pasted cookies hold no YouTube sign-in</extracomment>
            <translation>この Cookie には YouTube のログイン情報がありません。youtube.com にログインした状態でコピーしてください。</translation>
        </message>
        <message>
            <source>These cookies have expired. Sign in again and copy them anew.</source>
            <extracomment>Import error: every cookie in the pasted cookies.txt has expired</extracomment>
            <translation>この Cookie は期限切れです。もう一度ログインしてコピーし直してください。</translation>
        </message>
        <message>
            <source>This is too large to be a cookie export.</source>
            <extracomment>Import error: the pasted text or file is too large</extracomment>
            <translation>Cookie のエクスポートとしては大きすぎます。</translation>
        </message>
        <message>
            <source>This text holds no cookies. Paste a Cookie header, copied request headers, a cURL command or a cookies.txt.</source>
            <extracomment>Import error: the pasted text is not a cookie header, request headers, cURL command or cookies.txt</extracomment>
            <translation>このテキストには Cookie が含まれていません。Cookie ヘッダー、コピーしたリクエストヘッダー、cURL コマンド、または cookies.txt を貼り付けてください。</translation>
        </message>
    </context>
    <context>
        <name>nordtile::video::sailfish::DownloadsModel</name>
        <message>
            <source>Queued</source>
            <extracomment>Download state</extracomment>
            <translation>待機中</translation>
        </message>
        <message>
            <source>Preparing</source>
            <extracomment>Download state while the stream address is fetched</extracomment>
            <translation>準備中</translation>
        </message>
        <message>
            <source>Downloading</source>
            <extracomment>Download state shown while a download runs
----------
Download state</extracomment>
            <translation>ダウンロード中</translation>
        </message>
        <message>
            <source>Paused</source>
            <extracomment>Download state</extracomment>
            <translation>一時停止中</translation>
        </message>
        <message>
            <source>Waiting for Wi-Fi</source>
            <extracomment>Download state when only unmetered connections may be used</extracomment>
            <translation>Wi-Fi を待機中</translation>
        </message>
        <message>
            <source>Joining tracks</source>
            <extracomment>Download state while the video and audio tracks are joined</extracomment>
            <translation>トラックを結合中</translation>
        </message>
        <message>
            <source>Downloaded</source>
            <extracomment>Download state</extracomment>
            <translation>ダウンロード済み</translation>
        </message>
        <message>
            <source>The download folder is unavailable. Saving in the app's own folder instead.</source>
            <extracomment>Notice when the chosen download folder (e.g. an SD card) is missing</extracomment>
            <translation>ダウンロード先フォルダーを使用できません。代わりにアプリのフォルダーに保存します。</translation>
        </message>
        <message>
            <source>Video and audio could not be joined; both tracks were kept.</source>
            <extracomment>Notice when the video and audio of a download could not be joined</extracomment>
            <translation>動画と音声を結合できなかったため、両方のトラックを残しました。</translation>
        </message>
        <message>
            <source>No downloadable stream</source>
            <translation>ダウンロードできるストリームがありません</translation>
        </message>
        <message>
            <source>Download failed: size changed</source>
            <extracomment>Download failure when the file size changed between attempts</extracomment>
            <translation>ダウンロード失敗: サイズが変わりました</translation>
        </message>
        <message>
            <source>Download refused by YouTube</source>
            <translation>YouTube にダウンロードを拒否されました</translation>
        </message>
        <message>
            <source>Network error</source>
            <translation>ネットワークエラー</translation>
        </message>
        <message>
            <source>Could not write the file</source>
            <extracomment>Download failure when the file could not be written</extracomment>
            <translation>ファイルを書き込めませんでした</translation>
        </message>
        <message>
            <source>File missing</source>
            <translation>ファイルがありません</translation>
        </message>
        <message>
            <source>No writable download folder</source>
            <translation>書き込み可能なダウンロード先フォルダーがありません</translation>
        </message>
        <message>
            <source>This folder cannot be written to</source>
            <extracomment>Shown when a chosen download folder cannot be used</extracomment>
            <translation>このフォルダーには書き込めません</translation>
        </message>
        <message>
            <source>Download failed</source>
            <extracomment>Generic download failure</extracomment>
            <translation>ダウンロード失敗</translation>
        </message>
        <message>
            <source>The server did not send a video file</source>
            <extracomment>Download failure when the server sent a web page instead of the video or audio</extracomment>
            <translation>サーバーから動画ファイルが送られませんでした</translation>
        </message>
        <message>
            <source>Not enough free space</source>
            <extracomment>Download failure when the storage is too full for the rest of the download</extracomment>
            <translation>空き容量が足りません</translation>
        </message>
    </context>
    <context>
        <name>nordtile::video::sailfish::ImportController</name>
        <message>
            <source>Backup saved to %1</source>
            <translation>バックアップを %1 に保存しました</translation>
        </message>
        <message>
            <source>Nothing new to import.</source>
            <extracomment>Import summary when everything was already in the library</extracomment>
            <translation>新しくインポートするものはありません。</translation>
        </message>
        <message>
            <source>%1 subscriptions</source>
            <extracomment>Import summary part, %1 is a number</extracomment>
            <translation>登録チャンネル %1 件</translation>
        </message>
        <message>
            <source>%1 channels to look up</source>
            <extracomment>Import summary part, %1 is a number of channels to look up online</extracomment>
            <translation>照会するチャンネル %1 件</translation>
        </message>
        <message>
            <source>%1 history entries</source>
            <extracomment>Import summary part, %1 is a number</extracomment>
            <translation>履歴 %1 件</translation>
        </message>
        <message>
            <source>%1 resume points</source>
            <extracomment>Import summary part, %1 is a number</extracomment>
            <translation>再開位置 %1 件</translation>
        </message>
        <message>
            <source>%1 playlists</source>
            <extracomment>Import summary part, %1 is a number</extracomment>
            <translation>再生リスト %1 件</translation>
        </message>
        <message>
            <source>%1 saved YouTube playlists</source>
            <extracomment>Import summary part, %1 is a number</extracomment>
            <translation>保存済みの YouTube 再生リスト %1 件</translation>
        </message>
        <message>
            <source>%1 playlists skipped (same name)</source>
            <extracomment>Import summary part, %1 is a number of playlists skipped because a list of that name exists</extracomment>
            <translation>再生リスト %1 件をスキップ (同じ名前)</translation>
        </message>
        <message>
            <source>Imported: %1</source>
            <extracomment>Import summary, %1 is a comma-separated list of what was added</extracomment>
            <translation>インポート完了: %1</translation>
        </message>
        <message>
            <source>Could not read the account. Check the connection and try again.</source>
            <extracomment>Import error: reading the subscriptions and playlists of the signed-in account failed</extracomment>
            <translation>アカウントを読み取れませんでした。接続を確認してもう一度お試しください。</translation>
        </message>
    </context>
    <context>
        <name>nordtile::video::sailfish::PlaybackSession</name>
        <message>
            <source>Resolving video again…</source>
            <translation>動画を再取得しています…</translation>
        </message>
        <message>
            <source>Loading video…</source>
            <translation>動画を読み込んでいます…</translation>
        </message>
        <message>
            <source>Starting playback…</source>
            <translation>再生を開始しています…</translation>
        </message>
        <message>
            <source>Downloaded file</source>
            <extracomment>Stream label of a downloaded video played from the device</extracomment>
            <translation>ダウンロードしたファイル</translation>
        </message>
        <message>
            <source>Reconnecting…</source>
            <extracomment>Status while playback recovers automatically after an error (PLY-18)</extracomment>
            <translation>再接続しています…</translation>
        </message>
        <message>
            <source>Reconnects when SailTube is shown again</source>
            <extracomment>Status of a playback error that is retried when the app is shown again</extracomment>
            <translation>SailTube を再表示すると再接続します</translation>
        </message>
        <message>
            <source>Playback stopped after repeated errors.</source>
            <extracomment>Shown with a Retry button after automatic recovery gave up (PLY-18)</extracomment>
            <translation>エラーが繰り返されたため再生を停止しました。</translation>
        </message>
        <message>
            <source>Scheduled to start %1.</source>
            <extracomment>RES-19: appended to "not started yet"; %1 is the scheduled start (date and time)</extracomment>
            <translation>開始予定：%1。</translation>
        </message>
        <message>
            <source>Streaming is not available right now.</source>
            <extracomment>Shown for a few seconds when switching a downloaded video to its stream failed; the file plays on</extracomment>
            <translation>現在ストリーミングは利用できません。</translation>
        </message>
        <message>
            <source>Default</source>
            <extracomment>Label of the untagged or default audio track in the audio-track menu</extracomment>
            <translation>デフォルト</translation>
        </message>
        <message>
            <source>%1 (original)</source>
            <extracomment>Audio track menu: %1 is the language name of the original audio track</extracomment>
            <translation>%1（オリジナル）</translation>
        </message>
        <message>
            <source>%1 (audio description)</source>
            <extracomment>Audio track menu: %1 is the language name of a descriptive-audio track</extracomment>
            <translation>%1（音声解説）</translation>
        </message>
        <message>
            <source>YouTube changed how this video is delivered. Retrying with the full resolver…</source>
            <extracomment>Status line while the video is retried with the full resolver</extracomment>
            <translation>YouTube がこの動画の配信方法を変更しました。フルリゾルバーで再試行しています…</translation>
        </message>
        <message>
            <source>YouTube is limiting requests. Trying again shortly…</source>
            <extracomment>Status line while waiting to retry after YouTube limited requests</extracomment>
            <translation>YouTube がリクエストを制限しています。まもなく再試行します…</translation>
        </message>
        <message>
            <source>Playback stopped responding.</source>
            <extracomment>Shown with a Retry button when the stream stopped delivering and the automatic recovery steps did not bring it back</extracomment>
            <translation>再生が応答しなくなりました。</translation>
        </message>
    </context>
</TS>

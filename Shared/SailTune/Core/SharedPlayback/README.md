# SharedPlayback

One self-contained directory: the part of playback that turns a video id into directly playable
media addresses, quickly and anonymously. Qt Core and Qt Network only; no QML, no QtMultimedia, no
application types. SailTune uses it; SailTube can pin a copy of this directory and its tests
(`tests/sharedplayback/`) and must not edit it.

Behavioural source: `docs/fin_analysis/INSTANT_PLAYBACK_SPEC.md` (a clean-room description of how
other applications reach first audio in about two seconds) and the public client table of yt-dlp
(Unlicense). No GPL code was read.

## What is here

| Part | File | Purpose |
| --- | --- | --- |
| Vocabulary | `Domain/StreamTypes.h` | `StreamRequest` (audio/video, ceilings, codecs, dub, hardware decode: all in the cache key), `StreamFormat`, `StreamSet`, `ResolveOutcome`, failure and path labels |
| Request shape | `Domain/InnerTubeClient.h` | The pinned player request for a client that needs no key, cookies, PO token or player script (VISIONOS, id 101, from yt-dlp's table), visitor id request, media request headers |
| Ladder | `Domain/FormatLadder.h` | Language, then codec (Opus before AAC by default), regular before "stable volume", bitrate; deduplication; video ladder with a `video-only formats wanted` mode; muxed last resort |
| Verdicts | `Domain/Playability.h` | `playabilityStatus` to bot wall / unplayable / final; extractor error text to final or not |
| Breaker | `Domain/CircuitBreaker.h` | Two strikes in ten minutes or two bot walls in a row switch the fast path off for thirty minutes |
| Cache | `Domain/ResolveCache.h` | 24 entries, valid until expiry minus two minutes (at most twenty), five minutes without an expiry |
| Wire | `Domain/HttpResponseParser.h` | Bounded incremental HTTP/1.1 response parser |
| Transport | `Infrastructure/HttpsConnection.*` | Keep-alive HTTPS/1.1 connection, IPv4 on request, exact headers, one retry on a dead pooled connection |
| Parser | `Infrastructure/PlayerResponseParser.*` | Player answer to a ranked `StreamSet`; keeps plain addresses on the media host only |
| Resolver | `Infrastructure/FastStreamResolver.*` | Visitor id, player request, ladder, probe with playback's exact first request, cache, single flight, breaker, counters |
| Media source | `Infrastructure/MediaProxy.*` | Loopback HTTP source for the audio backend (`http://127.0.0.1:<port>/s/<token>`): bounded 10 MiB `Range` chunks over IPv4, resume at the same byte after a refusal, an expiring address or a dropped connection (one re-resolve of the same format through a host callback), flow control, a pre-buffer of the stream's start, `reuse()` of a still-open stream for a second playback of the same bytes, `Counters` |

## Interface for a host

```
FastStreamResolver resolver(options);
resolver.warmUp();                               // visitor id + connection, once the first screen is up
resolver.resolve(videoId, StreamRequest, cb);    // cb(ResolveOutcome): ok -> outcome.streams (ranked audio/video/muxed)
resolver.invalidate(videoId);                    // drop cached answers of a video
resolver.reportRefused(videoId, msLeft);         // a source was refused downstream (breaker strike if >= 60 s were left)
```

`ResolveOutcome.fallback` tells the host to try its slower extractor. Nothing here logs on its own:
`Options::log` receives one line of fixed numbers and class names per phase, never an address, title
or identifier.

## Rules that are pinned by tests

- The player request carries no `gl`, no visitor data in the body, no key, no cookies, no PO token.
- Only plain addresses on `*.googlevideo.com` over HTTPS are kept.
- The probe is the first request playback makes: same headers, `Range: bytes=0-(min(10 MiB, length) - 1)`.
  An HTTP refusal rejects and counts a strike; a network failure is no verdict.
- A bot answer discards the visitor id; an age gate does not.
- A removed or unavailable-in-region video is final: no fallback.

## The media source

```
MediaProxy proxy(options);                                   // ipv4Only, chunkBytes, log, ...
QUrl local = proxy.open(videoId, Source{url, userAgent, contentLength, mimeType, expiresAtUnixMs}, refresher);
//   hand `local` to the audio backend instead of the provider address
proxy.closeVideo(videoId);  proxy.wasServed(videoId);  proxy.counters();
```

`refresher(deliver)` is the host's way to get a new address of the same format (`deliver(FreshSource)`
exactly once); the proxy checks length and type before it splices, so two different files are never
joined. Rules pinned by tests: GET and HEAD only, 200/206/416/404/405/503, `Connection: close`,
`application/octet-stream`; only addresses registered through `open()` are served, under a random
64-character token; only HTTPS addresses on `*.googlevideo.com` are fetched, without cookies or
redirects; a freshly issued address that is refused is retried in place every 500 ms for up to 6 s
before it is replaced; three deaths in a row without progress end the stream; at most eight
re-resolves a minute per stream; nothing is taken from the network while the client's queue is above 2 MiB (at most the chunk in flight, 10 MiB, waits in memory).

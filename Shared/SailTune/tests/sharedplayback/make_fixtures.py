#!/usr/bin/env python3
"""Regenerates fixtures/*.json: trimmed copies of the shapes the player endpoint returned for the
VISIONOS client (recorded 2026-11-08 against two public Topic-channel tracks), with every address,
identifier and signature replaced by synthetic values. Run: python3 make_fixtures.py"""
import json, os

HERE = os.path.dirname(os.path.abspath(__file__))
EXPIRE = 4102444800  # 2100-01-01
UA = ('Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) '
      'Version/26.0 Safari/605.1.15')


def address(itag, host='rr1---sn-test.googlevideo.com', expire=EXPIRE):
    return ('https://%s/videoplayback?expire=%d&ei=SYNTHETIC&ip=0.0.0.0&id=o-SYNTHETIC&itag=%d&source=youtube&sig=SYNTHETIC'
            % (host, expire, itag))


def audio(itag, mime, bitrate, length, **extra):
    f = {'itag': itag, 'url': address(itag), 'mimeType': mime, 'bitrate': bitrate, 'averageBitrate': bitrate - 1000,
         'contentLength': str(length), 'approxDurationMs': '211000', 'audioQuality': 'AUDIO_QUALITY_MEDIUM',
         'audioSampleRate': '44100', 'audioChannels': 2, 'loudnessDb': None}
    f.pop('loudnessDb')
    f.update(extra)
    return f


def video(itag, mime, height, bitrate):
    return {'itag': itag, 'url': address(itag), 'mimeType': mime, 'bitrate': bitrate, 'averageBitrate': bitrate - 500,
            'contentLength': str(bitrate * 26), 'width': height, 'height': height, 'fps': 25, 'quality': 'hd%d' % height,
            'approxDurationMs': '211000'}


def base(video_id='AAAAAAAAAAA', status='OK', reason=None, formats=None, extra_details=None):
    playability = {'status': status}
    if reason:
        playability['reason'] = reason
    doc = {'responseContext': {'visitorData': 'SYNTHETICVISITORDATA'}, 'playabilityStatus': playability}
    details = {'videoId': video_id, 'title': 'Synthetic Song', 'lengthSeconds': '211', 'author': 'Synthetic Artist - Topic',
               'thumbnail': {'thumbnails': [{'url': 'https://i.ytimg.com/vi/%s/default.jpg' % video_id, 'width': 120, 'height': 90},
                                            {'url': 'https://i.ytimg.com/vi/%s/hqdefault.jpg' % video_id, 'width': 480, 'height': 360}]}}
    details.update(extra_details or {})
    doc['videoDetails'] = details
    if formats is not None:
        doc['streamingData'] = {'expiresInSeconds': '21540', 'adaptiveFormats': formats,
                                'hlsManifestUrl': 'https://manifest.googlevideo.com/api/manifest/hls_variant/synthetic/file/index.m3u8'}
        doc['playerConfig'] = {'audioConfig': {'perceptualLoudnessDb': -10.55, 'enablePerFormatLoudness': True,
                                               'trackAbsoluteLoudnessLkfs': -10.55, 'loudnessTargetLkfs': -14}}
    return doc


def write(name, doc):
    with open(os.path.join(HERE, 'fixtures', name), 'w') as handle:
        json.dump(doc, handle, indent=1, sort_keys=True)
        handle.write('\n')


ok_formats = [
    video(137, 'video/mp4; codecs="avc1.640020"', 1080, 64628),
    video(248, 'video/webm; codecs="vp9"', 1080, 172516),
    video(399, 'video/mp4; codecs="av01.0.08M.08"', 1080, 154426),
    video(136, 'video/mp4; codecs="avc1.4d401f"', 720, 40000),
    video(247, 'video/webm; codecs="vp9"', 720, 90000),
    audio(139, 'audio/mp4; codecs="mp4a.40.5"', 50375, 2891798),
    audio(140, 'audio/mp4; codecs="mp4a.40.2"', 130905, 7672924),
    audio(249, 'audio/webm; codecs="opus"', 59071, 3136699),
    audio(250, 'audio/webm; codecs="opus"', 77730, 4161704),
    audio(251, 'audio/webm; codecs="opus"', 152672, 8296263),
    audio(774, 'audio/webm; codecs="opus"', 258000, 9000000, isDrc=True),
    # Not kept: a ciphered format, a DRM format, a segmented one, a foreign host, an insecure address.
    {'itag': 18, 'signatureCipher': 's=SYNTHETIC&sp=sig&url=https%3A%2F%2Frr1.googlevideo.com%2Fvideoplayback', 'mimeType': 'video/mp4; codecs="avc1.42001E, mp4a.40.2"',
     'bitrate': 500000, 'width': 640, 'height': 360},
    dict(audio(141, 'audio/mp4; codecs="mp4a.40.2"', 256000, 1), drmFamilies=['WIDEVINE']),
    dict(audio(142, 'audio/mp4; codecs="mp4a.40.2"', 256000, 1), targetDurationSec=5),
    dict(audio(143, 'audio/mp4; codecs="mp4a.40.2"', 256000, 1), url='https://evil.example.com/videoplayback?expire=%d' % EXPIRE),
    dict(audio(144, 'audio/mp4; codecs="mp4a.40.2"', 256000, 1), url='http://rr1.googlevideo.com/videoplayback?expire=%d' % EXPIRE),
]
write('player_ok.json', base(formats=ok_formats))

dub = [
    audio(251, 'audio/webm; codecs="opus"', 150000, 8000000, audioTrack={'id': 'en.4', 'displayName': 'English (original)', 'audioIsDefault': True}),
    audio(139, 'audio/mp4; codecs="mp4a.40.5"', 50000, 2000000, audioTrack={'id': 'en.4', 'displayName': 'English (original)', 'audioIsDefault': True}),
    dict(audio(251, 'audio/webm; codecs="opus"', 150000, 8000000, audioTrack={'id': 'es.3', 'displayName': 'Spanish', 'audioIsDefault': False}), url=address(2510)),
    dict(audio(251, 'audio/webm; codecs="opus"', 151000, 8000000, audioTrack={'id': 'fr.3', 'displayName': 'French descriptive', 'audioIsDefault': False}), url=address(2511)),
]
write('player_dub.json', base(formats=dub))

write('player_bot.json', base(status='LOGIN_REQUIRED', reason='Sign in to confirm you’re not a bot'))
write('player_age.json', base(status='LOGIN_REQUIRED', reason='Sign in to confirm your age'))
write('player_removed.json', base(status='UNPLAYABLE', reason='This video has been removed by the uploader'))
write('player_try_later.json', base(status='ERROR', reason='This content is unavailable. Please try again later'))
write('player_ciphered.json', base(formats=[
    {'itag': 140, 'signatureCipher': 's=SYNTHETIC&sp=sig&url=https%3A%2F%2Frr1.googlevideo.com%2Fvideoplayback', 'mimeType': 'audio/mp4; codecs="mp4a.40.2"', 'bitrate': 130000}]))
write('player_live.json', base(formats=ok_formats, extra_details={'isLive': True}))
write('player_other_video.json', base(video_id='BBBBBBBBBBB', formats=ok_formats))
write('player_short_lived.json', base(formats=[audio(251, 'audio/webm; codecs="opus"', 152672, 8296263)]))
print('fixtures written')

# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Playback component build fragment, included once from sailtube.pro.

# Stream selection additions (Core/Media is owned by the playback component).
HEADERS += Core/Media/Domain/AudioTrackPolicy.h \
           Core/Media/Domain/LivePolicy.h
SOURCES += Core/Media/Domain/AudioTrackPolicy.cpp \
           Core/Media/Domain/LivePolicy.cpp

# Player (playback component): Qt-free policies, shareable with SailTune (no SailTube types).
HEADERS += Core/Player/Domain/AudioChain.h \
           Core/Player/Domain/AudioResourcePolicy.h \
           Core/Player/Domain/AutoplayPolicy.h \
           Core/Player/Domain/BufferingPolicy.h \
           Core/Player/Domain/FreezePolicy.h \
           Core/Player/Domain/KeepOnPolicy.h \
           Core/Player/Domain/MediaControlsPolicy.h \
           Core/Player/Domain/PipelinePlan.h \
           Core/Player/Domain/PlayerTypes.h \
           Core/Player/Domain/PositionPolicy.h \
           Core/Player/Domain/PrerollBarrier.h \
           Core/Player/Domain/RecoveryPolicy.h \
           Core/Player/Domain/ResolveCache.h \
           Core/Player/Domain/ResumePolicy.h \
           Core/Player/Domain/SeekPolicy.h \
           Core/Player/Domain/SessionArbiter.h \
           Core/Player/Domain/SpeedPolicy.h \
           Core/Player/Domain/SplitSeekGuard.h \
           Core/Player/Domain/SponsorSkip.h \
           Core/Player/Domain/StallPolicy.h \
           Core/Player/Domain/SwitchPlan.h \
           Core/Player/Domain/TapGestures.h \
           Core/Player/Domain/MemoryTrimPolicy.h \
           Core/Player/Domain/TempDirPolicy.h
SOURCES += Core/Player/Domain/AudioChain.cpp \
           Core/Player/Domain/AudioResourcePolicy.cpp \
           Core/Player/Domain/AutoplayPolicy.cpp \
           Core/Player/Domain/BufferingPolicy.cpp \
           Core/Player/Domain/FreezePolicy.cpp \
           Core/Player/Domain/MediaControlsPolicy.cpp \
           Core/Player/Domain/PipelinePlan.cpp \
           Core/Player/Domain/PositionPolicy.cpp \
           Core/Player/Domain/PrerollBarrier.cpp \
           Core/Player/Domain/RecoveryPolicy.cpp \
           Core/Player/Domain/ResolveCache.cpp \
           Core/Player/Domain/SeekPolicy.cpp \
           Core/Player/Domain/SessionArbiter.cpp \
           Core/Player/Domain/SplitSeekGuard.cpp \
           Core/Player/Domain/SponsorSkip.cpp \
           Core/Player/Domain/StallPolicy.cpp \
           Core/Player/Domain/SwitchPlan.cpp \
           Core/Player/Domain/TapGestures.cpp

# Player infrastructure (playback component): QML-facing pieces, Qt only, no SailTube types.
# link_pkgconfig comes from sailfishapp.prf (adding it here first would skip sailfishapp).
sailtube_store {
    # STORE (Harbour), E9: the validator rejects linking GStreamer, gst-app, gst-video,
    # gstnemointerfaces and audioresource-qt. The engine is a stub (GstEngineStore.cpp) that
    # fails every load with "only in the full build" until the reduced Qt Multimedia path
    # exists; the audio resource gate is compiled without libaudioresource-qt.
    DEFINES += NORDTILE_NO_AUDIORESOURCE NORDTILE_STORE_PLAYER_STUB
    PKGCONFIG += egl glesv2
} else {
    PKGCONFIG += audioresource-qt gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0
    # PLY-20 zero-copy hardware rendering: NemoGstVideoTexture / EGL-image memory
    # (droideglsink), EGLImage -> GL_TEXTURE_EXTERNAL_OES.
    PKGCONFIG += nemo-gstreamer-interfaces-1.0 egl glesv2
}
HEADERS += Core/Player/Infrastructure/AudioResourceGate.h \
           Core/Player/Infrastructure/GlResources.h \
           Core/Player/Infrastructure/HardwareFrameSource.h \
           Core/Player/Infrastructure/HardwareVideoNode.h \
           Core/Player/Infrastructure/GstEngine.h \
           Core/Player/Infrastructure/VideoPlayer.h \
           Core/Player/Infrastructure/OverlayController.h \
           Core/Player/Infrastructure/VideoFrameMailbox.h \
           Core/Player/Infrastructure/VideoSurfaceItem.h \
           Core/Player/Infrastructure/CoverVideoItem.h
SOURCES += Core/Player/Infrastructure/AudioResourceGate.cpp \
           Core/Player/Infrastructure/GlResources.cpp \
           Core/Player/Infrastructure/HardwareVideoNode.cpp \
           Core/Player/Infrastructure/VideoPlayer.cpp \
           Core/Player/Infrastructure/OverlayController.cpp \
           Core/Player/Infrastructure/VideoFrameMailbox.cpp \
           Core/Player/Infrastructure/VideoSurfaceItem.cpp \
           Core/Player/Infrastructure/CoverVideoItem.cpp

# Shared instant-playback interface (SailTube-authored proposal, docs/SHARED_PLAYBACK.md) and the
# temporary adapter over core's yt-dlp runner, until SailTune's component is pinned.
HEADERS += Core/Player/Infrastructure/YtDlpStreamMapping.h \
           Core/Player/Infrastructure/YtDlpStreamResolver.h \
           Core/Player/Infrastructure/SharedStreamSource.h \
           Core/Player/Infrastructure/SharedStreamSourceLadder.h \
           Core/Player/Infrastructure/SharedStreamSourceSailTune.h \
           Core/Player/Infrastructure/SharedStreamSourceThreaded.h
SOURCES += Core/Player/Infrastructure/YtDlpStreamMapping.cpp \
           Core/Player/Infrastructure/YtDlpStreamResolver.cpp \
           Core/Player/Infrastructure/SharedStreamSource.cpp \
           Core/Player/Infrastructure/SharedStreamSourceLadder.cpp \
           Core/Player/Infrastructure/SharedStreamSourceThreaded.cpp

# App-side playback glue (batch 2): decision controller and the process-wide timing clock.
HEADERS += Clients/Sailfish/Presentation/PlaybackController.h \
           Clients/Sailfish/Presentation/PlaybackTiming.h
SOURCES += Clients/Sailfish/Presentation/PlaybackController.cpp

# GStreamer engine: real in FULL, stub in STORE (E9).
sailtube_store {
    SOURCES += Core/Player/Infrastructure/GstEngineStore.cpp
} else {
    HEADERS += Core/Player/Infrastructure/EglSinkBridge.h
    SOURCES += Core/Player/Infrastructure/EglSinkBridge.cpp \
               Core/Player/Infrastructure/GstEngine.cpp
}

# RES-14 [FULL]: optional PO-token helper manager (compiled out of STORE).
!sailtube_store {
    HEADERS += Core/Resolver/Domain/PoTokenPolicy.h Core/Resolver/Infrastructure/PoTokenProvider.h
    SOURCES += Core/Resolver/Domain/PoTokenPolicy.cpp Core/Resolver/Infrastructure/PoTokenProvider.cpp
}

# PLY-20 per-build hardware-decode probe and PLT-09 flash TMPDIR (both builds; the GStreamer
# probe runner is FULL only).
HEADERS += Core/Player/Domain/HardwareProbe.h \
           Core/Player/Infrastructure/HardwareDecodeGate.h \
           Core/Player/Infrastructure/HardwareProbeClip.h \
           Core/Player/Infrastructure/MemoryTrim.h \
           Core/Player/Infrastructure/TempDirSetup.h \
           Clients/Sailfish/Presentation/MemoryTrimScheduler.h
SOURCES += Core/Player/Domain/HardwareProbe.cpp \
           Core/Player/Infrastructure/HardwareDecodeGate.cpp \
           Core/Player/Infrastructure/HardwareProbeClip.cpp \
           Core/Player/Infrastructure/MemoryTrim.cpp \
           Core/Player/Infrastructure/TempDirSetup.cpp \
           Clients/Sailfish/Presentation/MemoryTrimScheduler.cpp
!sailtube_store {
    HEADERS += Core/Player/Infrastructure/HardwareProbeRunner.h
    SOURCES += Core/Player/Infrastructure/HardwareProbeRunner.cpp
}

# Player overlay API (batch 4a): captions seam over the catalog component's caption models.
HEADERS += Clients/Sailfish/Presentation/PlaybackCaptions.h
SOURCES += Clients/Sailfish/Presentation/PlaybackCaptions.cpp

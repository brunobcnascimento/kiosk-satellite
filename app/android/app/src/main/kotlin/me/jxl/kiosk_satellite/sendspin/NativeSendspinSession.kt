package me.jxl.kiosk_satellite.sendspin

import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.os.SystemClock
import android.util.Log
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean

/**
 * One native-engine SendSpin session: a sendspin-cpp client (protocol, time
 * sync, decode, scheduling) plus the Android audio output and the feedback
 * loop that drives the library's synchronization.
 *
 * The engine setting decides whether SendspinBridge builds this or the
 * classic Kotlin pipeline; both feed the same bridge events, so Dart cannot
 * tell them apart.
 */
class NativeSendspinSession(
    context: Context,
    playerName: String,
    clientId: String,
    preferredCodec: String,
    softwareVersion: String,
    initialSyncOffsetMs: Long,
    initialDuckFactor: Float,
    initialMediaGain: Float,
    private val events: Events,
) {
    /** Bridge-facing events. All fire on native threads, never main. */
    interface Events {
        fun onConnectionChanged(connected: Boolean, serverName: String?)
        fun onPlaybackStateChanged(state: String?)
        fun onMetadata(
            title: String?,
            artist: String?,
            album: String?,
            artworkUrl: String?,
            positionMs: Long,
            durationMs: Long,
        )
        fun onPositionUpdate(positionMs: Long)
        fun onServerVolume(volume: Int)
        fun onServerMuted(muted: Boolean)
        /** The server's controller state: what it accepts, and the shuffle and repeat it is in. */
        fun onControllerState(commands: List<String>, shuffle: Boolean, repeat: String)
        fun onStreamActiveChanged(active: Boolean)
        /** The server's cover frame changed; see [NativeSendspinCallbacks.onArtwork]. */
        fun onArtwork(seq: Int)
    }

    companion object {
        private const val TAG = "NativeSendspin"

        // Off the protocol default (8928) so a co-installed sendspinlite or
        // other client can keep it; retried upward on bind failure.
        private const val BASE_SERVER_PORT = 8930
        private const val PORT_ATTEMPTS = 4

        private const val AUDIO_BUFFER_CAPACITY = 1_000_000

        private const val FEEDBACK_INTERVAL_MS = 5L
        private const val POSITION_PUSH_INTERVAL_MS = 5_000L

        // How long a stream start waits for its music to play, and a seek
        // for its stream to restart, before the position runs regardless.
        private const val AUDIO_START_TIMEOUT_MS = 5_000L

        // Shell-level reconnect: the library reports the connected edge but
        // does not redial on its own.
        private val RECONNECT_DELAYS_MS = longArrayOf(1_000, 2_000, 4_000, 8_000, 15_000, 30_000)
        private const val RECONNECT_PARKED_MS = 60_000L

        private const val PREFS = "sendspin_native"
        private const val PREF_STATIC_DELAY = "static_delay_ms"
    }

    private val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val output = NativeAudioOutput()

    private val controlThread = HandlerThread("SendspinNativeControl").also { it.start() }
    private val controlHandler = Handler(controlThread.looper)

    private val destroyed = AtomicBoolean(false)
    @Volatile private var handle = 0L
    @Volatile private var serverUrl: String? = null
    @Volatile private var connected = false
    @Volatile private var syncOffsetMs = initialSyncOffsetMs
    @Volatile private var streamActive = false
    private var reconnectAttempt = 0

    @Volatile private var feedbackThread: Thread? = null

    val isConnected: Boolean get() = connected
    val isTimeSynced: Boolean get() = handle != 0L && NativeSendspin.nativeIsTimeSynced(handle)
    val trackProgressMs: Int get() = if (handle != 0L) NativeSendspin.nativeGetTrackProgressMs(handle) else 0

    private val reconnectRunnable = Runnable {
        val url = serverUrl
        if (destroyed.get() || connected || url == null) return@Runnable
        Log.i(TAG, "Reconnecting (attempt ${reconnectAttempt + 1})")
        NativeSendspin.nativeConnect(handle, url)
        reconnectAttempt++
        scheduleReconnect()
    }

    init {
        output.setDuckGain(initialDuckFactor)
        output.setMediaGain(initialMediaGain)

        // Preference order is what the server picks from; preferred codec
        // first, PCM always last as the universal fallback.
        val codecs = linkedSetOf(
            when (preferredCodec) {
                "opus" -> NativeSendspin.CODEC_OPUS
                "pcm" -> NativeSendspin.CODEC_PCM
                else -> NativeSendspin.CODEC_FLAC
            },
            NativeSendspin.CODEC_FLAC,
            NativeSendspin.CODEC_OPUS,
            NativeSendspin.CODEC_PCM,
        )
        // Covers come at the screen's short side, which is as large as Now
        // Playing ever draws one.
        val metrics = context.resources.displayMetrics
        val artworkSize = minOf(metrics.widthPixels, metrics.heightPixels)
        val formats = ArrayList<Int>()
        for (codec in codecs) {
            for (channels in intArrayOf(2, 1)) {
                formats.addAll(listOf(codec, channels, 48_000, 16))
            }
        }

        handle = NativeSendspin.nativeCreate(
            callbacks = NativeCallbacks(),
            clientId = clientId,
            name = playerName,
            productName = "Kiosk Satellite",
            manufacturer = "Kiosk Satellite",
            softwareVersion = softwareVersion,
            serverPort = BASE_SERVER_PORT,
            audioBufferCapacity = AUDIO_BUFFER_CAPACITY,
            fixedDelayUs = 0,
            initialStaticDelayMs = prefs.getInt(PREF_STATIC_DELAY, 0),
            formats = formats.toIntArray(),
            artworkSize = artworkSize,
        )
        if (handle == 0L) {
            Log.e(TAG, "nativeCreate failed")
        } else {
            var startedNative = NativeSendspin.nativeStart(handle)
            var attempt = 1
            while (!startedNative && attempt < PORT_ATTEMPTS) {
                // Bind failure: recreate on the next port up.
                NativeSendspin.nativeDestroy(handle)
                handle = NativeSendspin.nativeCreate(
                    callbacks = NativeCallbacks(),
                    clientId = clientId,
                    name = playerName,
                    productName = "Kiosk Satellite",
                    manufacturer = "Kiosk Satellite",
                    softwareVersion = softwareVersion,
                    serverPort = BASE_SERVER_PORT + attempt,
                    audioBufferCapacity = AUDIO_BUFFER_CAPACITY,
                    fixedDelayUs = 0,
                    initialStaticDelayMs = prefs.getInt(PREF_STATIC_DELAY, 0),
                    formats = formats.toIntArray(),
                    artworkSize = artworkSize,
                )
                startedNative = handle != 0L && NativeSendspin.nativeStart(handle)
                attempt++
            }
            if (!startedNative) {
                Log.e(TAG, "Native engine failed to start on any port")
                if (handle != 0L) NativeSendspin.nativeDestroy(handle)
                handle = 0L
            }
        }

        startFeedbackLoop()
        controlHandler.postDelayed(::pushPosition, POSITION_PUSH_INTERVAL_MS)
    }

    fun connect(url: String) {
        if (destroyed.get() || handle == 0L) return
        serverUrl = url
        reconnectAttempt = 0
        controlHandler.removeCallbacks(reconnectRunnable)
        Log.i(TAG, "Connecting to $url")
        NativeSendspin.nativeConnect(handle, url)
        scheduleReconnect()
    }

    /**
     * Live sync offset, no restart. Implemented as a bias on the playback
     * feedback timestamp: reporting audio as finishing later than it does
     * makes the library schedule everything correspondingly earlier, so a
     * negative offset plays earlier, matching the classic engine's setting.
     */
    fun setSyncOffset(ms: Long) {
        syncOffsetMs = ms
    }

    fun setDuckFactor(factor: Float) = output.setDuckGain(factor)

    fun setMediaGain(gain: Float) = output.setMediaGain(gain)

    fun publishVolume(volumePct: Int, muted: Boolean) {
        if (handle == 0L) return
        NativeSendspin.nativeUpdateVolume(handle, volumePct)
        NativeSendspin.nativeUpdateMuted(handle, muted)
    }

    /**
     * A position adopted ahead of the server: Music Assistant restarts the
     * stream for a seek or a previous but sends no fresh metadata progress
     * for it, so the engine keeps extrapolating from the report before and
     * the UI would show the old place while the audio plays the new one.
     * Carried by every position push until the server's next real metadata
     * report, which re-bases everything.
     */
    private val anchor = PositionAnchor()

    /** The current track's duration from the last metadata report, 0 when unknown. */
    @Volatile private var durationMs = 0L

    /**
     * Whether the current stream's music has started playing. A stream
     * opens with the engine's silence until its first chunk's server
     * timestamp, a second or two after a seek or a restart, and an adopted
     * position only runs from the music on.
     */
    private val audible = AtomicBoolean(false)

    /** When a seek was sent, while its stream restart is still to come; 0 otherwise. */
    @Volatile private var seekSentAt = 0L

    private val audioStartTimeout = Runnable { onAudioStartTimeout() }

    /** When the current stream started, for the log of how long its music took. */
    @Volatile private var streamStartedAt = 0L

    /** [arg] is the command's value where it takes one: the seek position in ms. */
    fun sendCommand(command: String, arg: Long = 0L): Boolean {
        if (handle == 0L) return false
        val sent = NativeSendspin.nativeSendCommand(handle, command, arg)
        if (sent && command == "seek") seekSent(arg)
        if (sent && command == "previous") anchor.previousSent(SystemClock.elapsedRealtime())
        return sent
    }

    /**
     * Music Assistant answers a seek with a stream restart at the target.
     * The target stands still until the new stream's music plays, and only
     * then goes to the UI, so the bar does not run ahead through the
     * silence before it.
     */
    private fun seekSent(positionMs: Long) {
        val now = SystemClock.elapsedRealtime()
        anchor.set(positionMs, now, running = false)
        seekSentAt = now
        armAudioStartTimeout()
    }

    private fun armAudioStartTimeout() {
        controlHandler.removeCallbacks(audioStartTimeout)
        controlHandler.postDelayed(audioStartTimeout, AUDIO_START_TIMEOUT_MS)
    }

    /** Playback reached the stream's first music, at [at] (elapsed realtime). */
    private fun onAudible(at: Long) {
        if (!streamActive || !audible.compareAndSet(false, true)) return
        controlHandler.removeCallbacks(audioStartTimeout)
        Log.i(TAG, "Music started ${at - streamStartedAt}ms after the stream start")
        seekSentAt = 0L
        anchor.setRunning(true, at)
        events.onStreamActiveChanged(true)
        anchor.current(at, durationMs)?.let { events.onPositionUpdate(it) }
    }

    /**
     * No music marker came, or no restart after a seek: run the position
     * anyway rather than hold it forever. A seek the server carried out
     * within the running stream counts from when it was sent.
     */
    private fun onAudioStartTimeout() {
        if (!streamActive) {
            seekSentAt = 0L
            return
        }
        val now = SystemClock.elapsedRealtime()
        if (!audible.get()) {
            Log.w(TAG, "No music marker within ${AUDIO_START_TIMEOUT_MS}ms of the stream start")
            onAudible(now)
            return
        }
        val sentAt = seekSentAt
        if (sentAt == 0L) return
        seekSentAt = 0L
        val target = anchor.current(now, 0L) ?: return
        anchor.set(target + (now - sentAt), now, running = true)
        events.onPositionUpdate(displayedProgressMs())
    }

    /**
     * Take [positionMs] as the track's position from now on: a seek just
     * sent, or the server's own queue time when the engine's metadata
     * progress has gone stale (Music Assistant leaves it behind after a
     * queue jump). Carried by every push until the next real report.
     */
    fun rebasePosition(positionMs: Long) {
        if (handle == 0L) return
        anchor.set(positionMs, SystemClock.elapsedRealtime(), streamActive && audible.get())
        events.onPositionUpdate(positionMs)
    }

    /** The adopted position while there is one, the engine's progress otherwise. */
    private fun displayedProgressMs(): Long =
        anchor.current(SystemClock.elapsedRealtime(), durationMs)
            ?: NativeSendspin.nativeGetTrackProgressMs(handle).toLong().coerceAtLeast(0L)

    fun onNetworkAvailable() {
        if (destroyed.get() || handle == 0L) return
        NativeSendspin.nativeSetNetworkReady(handle, true)
        if (!connected && serverUrl != null) {
            reconnectAttempt = 0
            controlHandler.removeCallbacks(reconnectRunnable)
            controlHandler.post(reconnectRunnable)
        }
    }

    /** The shown cover frame's bytes while [seq] is still current. */
    fun artwork(seq: Int): ByteArray? =
        if (handle != 0L) NativeSendspin.nativeGetArtwork(handle, seq) else null

    fun setNetworkReady(ready: Boolean) {
        if (handle != 0L) NativeSendspin.nativeSetNetworkReady(handle, ready)
    }

    fun buildStats(): Map<String, Any?> = mapOf(
        "engine" to "native",
        "outputQueueMs" to output.outputQueueMs(),
        "framesWritten" to output.totalFramesWritten(),
        "audioTrackUnderruns" to output.underrunCount(),
        "sinkLatencyMs" to output.sinkLatencyMs(),
        "timeSynced" to isTimeSynced,
        "progressMs" to trackProgressMs,
        "timing" to output.timingDiagnostics(),
    )

    fun destroy() {
        if (!destroyed.compareAndSet(false, true)) return
        controlHandler.removeCallbacksAndMessages(null)
        feedbackThread?.interrupt()
        feedbackThread = null
        val h = handle
        handle = 0L
        if (h != 0L) {
            NativeSendspin.nativeDisconnect(h, NativeSendspin.REASON_USER_REQUEST)
            NativeSendspin.nativeDestroy(h)
        }
        output.release()
        controlThread.quitSafely()
    }

    // ------------------------------------------------------------------

    private fun scheduleReconnect() {
        if (destroyed.get()) return
        val delay = RECONNECT_DELAYS_MS.getOrElse(reconnectAttempt) { RECONNECT_PARKED_MS }
        controlHandler.removeCallbacks(reconnectRunnable)
        controlHandler.postDelayed(reconnectRunnable, delay)
    }

    private fun startFeedbackLoop() {
        val thread = Thread {
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
            while (!destroyed.get()) {
                val h = handle
                if (h != 0L && output.isStarted) {
                    val progress = output.takePresentedFramesDelta()
                    if (output.takeAudioStarted()) {
                        val at = SystemClock.elapsedRealtime()
                        controlHandler.post { onAudible(at) }
                    }
                    if (progress != null) {
                        NativeSendspin.nativeNotifyAudioPlayed(
                            h,
                            progress.frames.toInt(),
                            progress.sampledAtUs - syncOffsetMs * 1000,
                        )
                    }
                }
                try {
                    Thread.sleep(FEEDBACK_INTERVAL_MS)
                } catch (_: InterruptedException) {
                    break
                }
            }
        }
        thread.name = "SendspinNativeFeedback"
        thread.isDaemon = true
        feedbackThread = thread
        thread.start()
    }

    private fun pushPosition() {
        if (destroyed.get()) return
        // Before the music plays, the position would only start the UI
        // counting through the silence.
        if (streamActive && audible.get() && seekSentAt == 0L && handle != 0L) {
            events.onPositionUpdate(displayedProgressMs())
        }
        controlHandler.postDelayed(::pushPosition, POSITION_PUSH_INTERVAL_MS)
    }

    // ------------------------------------------------------------------

    private inner class NativeCallbacks : NativeSendspinCallbacks {

        override fun onAudioWrite(buffer: ByteBuffer, length: Int, timeoutMs: Int): Int =
            output.write(buffer, length, timeoutMs)

        override fun onAudioStart() = output.markAudioStart()

        override fun onStreamStart(sampleRate: Int, channels: Int, bitDepth: Int) {
            Log.i(TAG, "Stream start sr=$sampleRate ch=$channels bd=$bitDepth")
            output.start(sampleRate, channels, bitDepth)
            audible.set(false)
            streamActive = true
            streamStartedAt = SystemClock.elapsedRealtime()
            if (anchor.onStreamStart(streamStartedAt)) {
                Log.i(TAG, "Previous restarted the track: position reset to 0")
            }
            // Playing as far as everything else is concerned once the
            // music does, so no surface counts through the silence.
            controlHandler.post { armAudioStartTimeout() }
        }

        override fun onStreamEnd() {
            Log.i(TAG, "Stream end")
            streamActive = false
            audible.set(false)
            anchor.setRunning(false, SystemClock.elapsedRealtime())
            output.pause()
            events.onStreamActiveChanged(false)
        }

        override fun onVolumeChanged(volume: Int) = events.onServerVolume(volume)

        override fun onMuteChanged(muted: Boolean) = events.onServerMuted(muted)

        override fun onStaticDelayChanged(delayMs: Int) {
            prefs.edit().putInt(PREF_STATIC_DELAY, delayMs).apply()
        }

        override fun onMetadata(
            title: String?,
            artist: String?,
            album: String?,
            albumArtist: String?,
            artworkUrl: String?,
            year: Int,
            track: Int,
            progressMs: Long,
            durationMs: Long,
            timestampUs: Long,
        ) {
            // A real server report: whatever it says about the position
            // outranks a seek the client carried on its own.
            if (progressMs >= 0) anchor.clear()
            this@NativeSendspinSession.durationMs = durationMs.coerceAtLeast(0L)
            events.onMetadata(title, artist, album, artworkUrl, progressMs, durationMs)
        }

        override fun onArtwork(seq: Int) = events.onArtwork(seq)

        override fun onGroupUpdate(playbackState: Int, groupId: String?, groupName: String?) {
            when (playbackState) {
                1 -> events.onPlaybackStateChanged("playing")
                0 -> events.onPlaybackStateChanged("stopped")
            }
        }

        override fun onControllerState(
            commands: String,
            volume: Int,
            muted: Boolean,
            repeat: String,
            shuffle: Boolean,
            seekMaxMs: Long,
        ) {
            events.onControllerState(
                commands.split(',').map { it.trim() }.filter { it.isNotEmpty() },
                shuffle,
                repeat,
            )
        }

        override fun onTimeSyncUpdated(errorUs: Float) {}

        override fun onConnectionChanged(connected: Boolean, serverName: String?) {
            this@NativeSendspinSession.connected = connected
            if (connected) {
                reconnectAttempt = 0
                controlHandler.removeCallbacks(reconnectRunnable)
            } else {
                streamActive = false
                audible.set(false)
                anchor.setRunning(false, SystemClock.elapsedRealtime())
                output.pause()
                events.onStreamActiveChanged(false)
                if (!destroyed.get() && serverUrl != null) {
                    controlHandler.post { scheduleReconnect() }
                }
            }
            events.onConnectionChanged(connected, serverName)
        }

        override fun onRequestHighPerformance() {
            Log.d(TAG, "High-performance networking requested")
        }

        override fun onReleaseHighPerformance() {
            Log.d(TAG, "High-performance networking released")
        }
    }
}

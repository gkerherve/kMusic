package uk.kherve.kmusic

import android.content.ComponentName
import android.graphics.Bitmap
import android.media.MediaMetadata
import android.media.Rating
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.service.notification.NotificationListenerService
import android.media.AudioManager

/**
 * Runs while notification access is granted. Follows the phone's active media
 * session (YouTube Music, Spotify, ...) and mirrors it to the kMusic watch app.
 */
class MusicService : NotificationListenerService() {

    private object K {   // must match watch/package.json messageKeys
        const val CMD = 0; const val TITLE = 1; const val ARTIST = 2; const val ALBUM = 3; const val STATE = 4
        const val POS = 5; const val DUR = 6; const val VOL = 7; const val PLAYER = 8; const val ART_W = 9
        const val ART_ROW = 10; const val ART_LEN = 11; const val ART_ID = 12; const val ART_OFF = 13
        const val ART_DATA = 14; const val ACCENT = 15; const val COLOR = 16; const val LIKED = 17; const val INFO = 18
    }
    private object Cmd { const val PLAYPAUSE = 1; const val NEXT = 2; const val PREV = 3; const val VOL_UP = 4; const val VOL_DOWN = 5; const val HELLO = 6; const val LIKE = 7; const val DISLIKE = 8 }

    companion object { @Volatile var running = false }

    private lateinit var link: PebbleLink
    private lateinit var msm: MediaSessionManager
    private val main = Handler(Looper.getMainLooper())
    private var ctrl: MediaController? = null

    // what the watch asked for
    private var artSize = 0; private var artRow = 0; private var artColor = true
    // what we last sent
    private var lastTrackKey = ""; private var artId = 0; private var liked = 0
    private var sentArt: Bitmap? = null   // cover last sent to the watch
    private var accent = 0xC0 or (1 shl 4) or 2   // default purple

    private val sessionsListener = MediaSessionManager.OnActiveSessionsChangedListener { pickController(it) }

    private val cb = object : MediaController.Callback() {
        override fun onMetadataChanged(m: MediaMetadata?) { pushAll(false) }
        override fun onPlaybackStateChanged(s: PlaybackState?) { pushState() }
        override fun onSessionDestroyed() { pickController(null) }
        override fun onQueueChanged(q: MutableList<android.media.session.MediaSession.QueueItem>?) { pushAll(false) }
    }

    override fun onListenerConnected() {
        running = true
        link = PebbleLink(this) { onWatch(it) }
        link.start()
        msm = getSystemService(MediaSessionManager::class.java)
        val me = ComponentName(this, MusicService::class.java)
        msm.addOnActiveSessionsChangedListener(sessionsListener, me, main)
        pickController(msm.getActiveSessions(me))
        prefs().registerOnSharedPreferenceChangeListener(prefListener)
    }

    private fun prefs() = getSharedPreferences("kmusic", MODE_PRIVATE)
    private val prefListener = android.content.SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
        if (key == "player") { ctrl?.unregisterCallback(cb); ctrl = null; pickController(null) }
    }

    override fun onListenerDisconnected() {
        running = false
        try { msm.removeOnActiveSessionsChangedListener(sessionsListener) } catch (_: Exception) {}
        prefs().unregisterOnSharedPreferenceChangeListener(prefListener)
        ctrl?.unregisterCallback(cb)
        link.stop()
    }

    private fun pickController(list: List<MediaController>?) {
        val all = list ?: try { msm.getActiveSessions(ComponentName(this, MusicService::class.java)) } catch (_: Exception) { emptyList() }
        // "player" = package chosen in the kMusic app, or empty for automatic (whatever is playing)
        val chosen = prefs().getString("player", "") ?: ""
        val pool = if (chosen.isEmpty()) all else all.filter { it.packageName == chosen }
        val best = pool.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING } ?: pool.firstOrNull()
        if (best?.sessionToken == ctrl?.sessionToken) return
        ctrl?.unregisterCallback(cb)
        ctrl = best
        best?.registerCallback(cb, main)
        pushAll(true)
    }

    // ------------------------------------------------------------------ watch -> phone
    private fun onWatch(m: Map<Int, Any>) {
        val c = ctrl
        when (m[K.CMD] as? Int) {
            Cmd.HELLO -> {
                artSize = (m[K.ART_W] as? Int) ?: 0
                artRow = (m[K.ART_ROW] as? Int) ?: 0
                artColor = ((m[K.COLOR] as? Int) ?: 1) != 0
                pushAll(true)
            }
            Cmd.PLAYPAUSE -> c?.transportControls?.let { if (c.playbackState?.state == PlaybackState.STATE_PLAYING) it.pause() else it.play() }
            Cmd.NEXT -> c?.transportControls?.skipToNext()
            Cmd.PREV -> c?.transportControls?.skipToPrevious()
            Cmd.VOL_UP -> changeVolume(AudioManager.ADJUST_RAISE)
            Cmd.VOL_DOWN -> changeVolume(AudioManager.ADJUST_LOWER)
            Cmd.LIKE -> rate(true)
            Cmd.DISLIKE -> rate(false)
        }
    }

    /**
     * Volume of the MUSIC stream only, one step at a time, never below 0 and never
     * touching the ringer (so the phone can't drop into silent / vibrate), no sound, no UI.
     * Casting (remote playback) uses the session's own volume instead.
     */
    private fun changeVolume(dir: Int) {
        val c = ctrl
        val remote = c?.playbackInfo?.playbackType == MediaController.PlaybackInfo.PLAYBACK_TYPE_REMOTE
        if (remote) c?.adjustVolume(dir, 0)
        else {
            val am = getSystemService(AudioManager::class.java)
            val cur = am.getStreamVolume(AudioManager.STREAM_MUSIC)
            val max = am.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
            val next = (cur + if (dir == AudioManager.ADJUST_RAISE) 1 else -1).coerceIn(0, max)
            if (next != cur) am.setStreamVolume(AudioManager.STREAM_MUSIC, next, 0)
        }
        link.send(mapOf(K.VOL to PebbleLink.V.I(volumePct(c))))
    }

    /** YouTube Music exposes thumbs up / down as custom actions; other players use ratings. */
    private fun rate(up: Boolean) {
        val c = ctrl ?: return
        val actions = c.playbackState?.customActions.orEmpty()
        fun matches(a: PlaybackState.CustomAction, words: List<String>) =
            words.any { w -> a.action.lowercase().contains(w) || a.name.toString().lowercase().contains(w) }
        val downWords = listOf("dislike", "thumbs_down", "thumb_down", "thumbsdown", "unlike")
        val upWords = listOf("thumbs_up", "thumb_up", "thumbsup", "like", "favorite", "favourite", "heart", "love")
        val act = if (up) actions.firstOrNull { matches(it, upWords) && !matches(it, downWords) }
                  else actions.firstOrNull { matches(it, downWords) }
        when {
            act != null -> c.transportControls.sendCustomAction(act, null)
            c.ratingType == Rating.RATING_THUMB_UP_DOWN -> c.transportControls.setRating(Rating.newThumbRating(up))
            c.ratingType == Rating.RATING_HEART -> if (up) c.transportControls.setRating(Rating.newHeartRating(true))
        }
        val v = if (up) 1 else -1
        liked = if (liked == v) 0 else v
    }

    // ------------------------------------------------------------------ phone -> watch
    private fun clean(s: CharSequence?, max: Int): String {
        var t = (s ?: "").toString()
            .replace(Regex("[‘’]"), "'").replace(Regex("[“”]"), "\"")
            .replace(Regex("[–—]"), "-").replace("…", "...")
            .replace(Regex("[^\u0000-ɏ]"), "").trim()
        while (t.toByteArray().size > max) t = t.dropLast(1)
        return t
    }

    private fun playerName(c: MediaController): String = try {
        packageManager.getApplicationLabel(packageManager.getApplicationInfo(c.packageName, 0)).toString()
    } catch (_: Exception) { c.packageName }

    private fun volumePct(c: MediaController?): Int {
        val pi = c?.playbackInfo
        if (pi != null && pi.playbackType == MediaController.PlaybackInfo.PLAYBACK_TYPE_REMOTE && pi.maxVolume > 0)
            return pi.currentVolume * 100 / pi.maxVolume
        val am = getSystemService(AudioManager::class.java)
        val max = am.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
        return if (max > 0) am.getStreamVolume(AudioManager.STREAM_MUSIC) * 100 / max else -1
    }

    private fun position(s: PlaybackState?): Int {
        if (s == null) return 0
        var p = s.position
        if (s.state == PlaybackState.STATE_PLAYING && s.lastPositionUpdateTime > 0)
            p += ((SystemClock.elapsedRealtime() - s.lastPositionUpdateTime) * s.playbackSpeed).toLong()
        return p.coerceAtLeast(0).toInt()
    }

    private fun info(c: MediaController, m: MediaMetadata?): String {
        val lines = ArrayList<String>()
        fun str(k: String) = m?.getString(k)?.takeIf { it.isNotBlank() }
        fun num(k: String) = m?.getLong(k)?.takeIf { it > 0 }
        str(MediaMetadata.METADATA_KEY_ARTIST)?.let { lines += it }
        str(MediaMetadata.METADATA_KEY_ALBUM)?.let { a -> lines += "Album: $a" + (num(MediaMetadata.METADATA_KEY_YEAR)?.let { " ($it)" } ?: "") }
        str(MediaMetadata.METADATA_KEY_ALBUM_ARTIST)?.takeIf { it != str(MediaMetadata.METADATA_KEY_ARTIST) }?.let { lines += "Album artist: $it" }
        str(MediaMetadata.METADATA_KEY_GENRE)?.let { lines += "Genre: $it" }
        num(MediaMetadata.METADATA_KEY_TRACK_NUMBER)?.let { n -> lines += "Track $n" + (num(MediaMetadata.METADATA_KEY_NUM_TRACKS)?.let { " of $it" } ?: "") }
        num(MediaMetadata.METADATA_KEY_DISC_NUMBER)?.takeIf { it > 1 }?.let { lines += "Disc $it" }
        str(MediaMetadata.METADATA_KEY_COMPOSER)?.let { lines += "Composer: $it" }
        str(MediaMetadata.METADATA_KEY_WRITER)?.let { lines += "Writer: $it" }
        num(MediaMetadata.METADATA_KEY_DURATION)?.let { lines += "Length: %d:%02d".format(it / 60000, it / 1000 % 60) }
        m?.getDescription()?.subtitle?.toString()?.takeIf { it.isNotBlank() && !lines.contains(it) }?.let { lines += it }
        val next = upNext(c)
        if (next.isNotEmpty()) { lines += ""; lines += "Up next:"; next.forEachIndexed { i, t -> lines += "${i + 1}. $t" } }
        lines += ""
        lines += "Playing on ${playerName(c)}"
        volumePct(c).takeIf { it >= 0 }?.let { lines += "Volume $it%" }
        return clean(lines.joinToString("\n"), 560)
    }

    /** The next few songs of the player's queue, "Title - Artist". */
    private fun upNext(c: MediaController): List<String> {
        val q = c.queue ?: return emptyList()
        if (q.isEmpty()) return emptyList()
        val activeId = c.playbackState?.activeQueueItemId ?: -1L
        var i = q.indexOfFirst { it.queueId == activeId }
        if (i < 0) {                                                     // no id: find the current title
            val t = c.metadata?.getString(MediaMetadata.METADATA_KEY_TITLE)
            i = q.indexOfFirst { it.description.title?.toString() == t }
        }
        return q.drop(i + 1).take(5).map { item ->
            val d = item.description
            val title = clean(d.title, 40)
            val by = clean(d.subtitle, 28)
            if (by.isNotEmpty()) "$title - $by" else title
        }.filter { it.isNotBlank() }
    }

    private fun pushState() {
        val c = ctrl ?: return
        val s = c.playbackState
        link.send(mapOf(
            K.STATE to PebbleLink.V.I(if (s?.state == PlaybackState.STATE_PLAYING) 1 else 0),
            K.POS to PebbleLink.V.I(position(s)),
            K.VOL to PebbleLink.V.I(volumePct(c))
        ))
    }

    private fun pushAll(force: Boolean) {
        val c = ctrl
        if (c == null) {
            link.send(mapOf(K.TITLE to PebbleLink.V.S(""), K.ARTIST to PebbleLink.V.S(""), K.STATE to PebbleLink.V.I(0)))
            return
        }
        val m = c.metadata
        val title = clean(m?.getString(MediaMetadata.METADATA_KEY_TITLE) ?: m?.description?.title, 64)
        val artist = clean(m?.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: m?.description?.subtitle, 52)
        val key = c.packageName + "|" + title + "|" + artist
        val newTrack = key != lastTrackKey
        if (newTrack) {
            lastTrackKey = key
            liked = m?.getRating(MediaMetadata.METADATA_KEY_USER_RATING)?.let { r ->
                if (r.isRated && r.ratingStyle == Rating.RATING_THUMB_UP_DOWN) (if (r.isThumbUp) 1 else -1)
                else if (r.isRated && r.ratingStyle == Rating.RATING_HEART && r.hasHeart()) 1 else 0
            } ?: 0
        }
        val art: Bitmap? = m?.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
            ?: m?.getBitmap(MediaMetadata.METADATA_KEY_ART)
            ?: m?.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON)
            ?: m?.description?.iconBitmap
        if (art != null) accent = ArtEncoder.accent(art)

        if (newTrack || force) link.clearQueue()
        val s = c.playbackState
        link.send(mapOf(
            K.TITLE to PebbleLink.V.S(title.ifEmpty { "Unknown" }),
            K.ARTIST to PebbleLink.V.S(artist),
            K.PLAYER to PebbleLink.V.S(clean(playerName(c), 22)),
            K.STATE to PebbleLink.V.I(if (s?.state == PlaybackState.STATE_PLAYING) 1 else 0),
            K.POS to PebbleLink.V.I(position(s)),
            K.DUR to PebbleLink.V.I((m?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L).toInt()),
            K.VOL to PebbleLink.V.I(volumePct(c)),
            K.ACCENT to PebbleLink.V.I(accent),
            K.LIKED to PebbleLink.V.I(liked),
            K.INFO to PebbleLink.V.S(info(c, m))
        ))
        // players often send the title first and the cover a moment later: send it when it shows up
        val artChanged = art != null && (sentArt == null || try { !art.sameAs(sentArt) } catch (_: Exception) { true })
        if (artSize > 0 && (newTrack || force || artChanged)) {
            sentArt = art
            sendArt(art)
        }
    }

    private fun sendArt(art: Bitmap?) {
        val id = ++artId and 0x7FFF
        if (art == null) { link.send(mapOf(K.ART_ID to PebbleLink.V.I(id), K.ART_LEN to PebbleLink.V.I(0))); return }
        val size = artSize; val row = artRow; val color = artColor
        Thread {
            val bytes = ArtEncoder.encode(ArtEncoder.square(art, size), color, row)
            main.post {
                if (id != (artId and 0x7FFF)) return@post          // a newer song arrived meanwhile
                link.send(mapOf(K.ART_ID to PebbleLink.V.I(id), K.ART_LEN to PebbleLink.V.I(bytes.size)))
                val chunk = 1500
                var o = 0
                while (o < bytes.size) {
                    val end = minOf(bytes.size, o + chunk)
                    link.send(mapOf(K.ART_ID to PebbleLink.V.I(id), K.ART_OFF to PebbleLink.V.I(o),
                                    K.ART_DATA to PebbleLink.V.B(bytes.copyOfRange(o, end))))
                    o = end
                }
            }
        }.start()
    }
}

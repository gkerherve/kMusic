package uk.kherve.kmusic

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

/**
 * Talks to the kMusic watch app through the Pebble phone app, using the
 * standard PebbleKit Android broadcast protocol (no library needed).
 * Messages are queued and sent one at a time: the next goes when the watch ACKs.
 */
class PebbleLink(private val ctx: Context, private val onMessage: (Map<Int, Any>) -> Unit) {

    companion object {
        val APP_UUID: UUID = UUID.fromString("a445a243-adbe-49b0-86cf-51b8aa7e59b2")
        private const val SEND = "com.getpebble.action.app.SEND"
        private const val RECEIVE = "com.getpebble.action.app.RECEIVE"
        private const val ACK = "com.getpebble.action.app.ACK"
        private const val RECEIVE_ACK = "com.getpebble.action.app.RECEIVE_ACK"
        private const val RECEIVE_NACK = "com.getpebble.action.app.RECEIVE_NACK"
    }

    sealed class V {
        data class I(val v: Int) : V()
        data class S(val v: String) : V()
        class B(val v: ByteArray) : V()
    }

    private val handler = Handler(Looper.getMainLooper())
    private val queue = ArrayDeque<Map<Int, V>>()
    private var inFlight: Int = -1
    private var nextTx = 1
    private var retries = 0
    private val timeout = Runnable { onFail() }

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            when (i.action) {
                RECEIVE -> {
                    val uuid = i.getSerializableExtra("uuid") as? UUID ?: return
                    if (uuid != APP_UUID) return
                    val tx = i.getIntExtra("transaction_id", -1)
                    broadcast(Intent(ACK).putExtra("transaction_id", tx))
                    val json = i.getStringExtra("msg_data") ?: return
                    onMessage(parse(json))
                }
                RECEIVE_ACK -> if (i.getIntExtra("transaction_id", -2) == inFlight) onAck()
                RECEIVE_NACK -> if (i.getIntExtra("transaction_id", -2) == inFlight) onFail()
            }
        }
    }

    fun start() {
        val f = IntentFilter().apply { addAction(RECEIVE); addAction(RECEIVE_ACK); addAction(RECEIVE_NACK) }
        if (Build.VERSION.SDK_INT >= 33) ctx.registerReceiver(receiver, f, Context.RECEIVER_EXPORTED)
        else ctx.registerReceiver(receiver, f)
    }

    fun stop() {
        try { ctx.unregisterReceiver(receiver) } catch (_: Exception) {}
        handler.removeCallbacksAndMessages(null)
        queue.clear(); inFlight = -1
    }

    /** Drop anything still waiting (e.g. the old song's cover) and send fresh. */
    fun clearQueue() { queue.clear() }

    fun send(msg: Map<Int, V>) { queue.addLast(msg); pump() }

    private fun pump() {
        if (inFlight != -1 || queue.isEmpty()) return
        val msg = queue.first()
        val tx = nextTx; nextTx = if (nextTx >= 250) 1 else nextTx + 1
        inFlight = tx
        val arr = JSONArray()
        for ((k, v) in msg) {
            val o = JSONObject().put("key", k)
            when (v) {
                is V.I -> o.put("type", "int").put("length", 4).put("value", v.v)
                is V.S -> o.put("type", "string").put("length", v.v.toByteArray().size + 1).put("value", v.v)
                is V.B -> o.put("type", "bytes").put("length", v.v.size).put("value", Base64.encodeToString(v.v, Base64.NO_WRAP))
            }
            arr.put(o)
        }
        broadcast(Intent(SEND).putExtra("uuid", APP_UUID).putExtra("transaction_id", tx).putExtra("msg_data", arr.toString()))
        handler.postDelayed(timeout, 3000)
    }

    private fun onAck() {
        handler.removeCallbacks(timeout)
        queue.removeFirstOrNull(); inFlight = -1; retries = 0
        pump()
    }

    private fun onFail() {
        handler.removeCallbacks(timeout)
        inFlight = -1
        if (++retries > 2) { queue.removeFirstOrNull(); retries = 0 }   // give up on this one
        handler.postDelayed({ pump() }, 400)
    }

    /** Send implicitly, plus explicitly to every installed Pebble app (Android 8+ blocks implicit to manifest receivers). */
    private fun broadcast(i: Intent) {
        ctx.sendBroadcast(i)
        val targets = ctx.packageManager.queryBroadcastReceivers(Intent(i.action), 0)
        for (r in targets.map { it.activityInfo.packageName }.toSet()) {
            if (r == ctx.packageName) continue
            ctx.sendBroadcast(Intent(i).setPackage(r))
        }
    }

    private fun parse(json: String): Map<Int, Any> {
        val out = HashMap<Int, Any>()
        val arr = JSONArray(json)
        for (n in 0 until arr.length()) {
            val o = arr.getJSONObject(n)
            out[o.getInt("key")] = when (o.optString("type")) {
                "string" -> o.getString("value")
                "bytes" -> Base64.decode(o.getString("value"), Base64.DEFAULT)
                else -> o.getLong("value").toInt()
            }
        }
        return out
    }
}

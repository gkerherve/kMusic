package uk.kherve.kmusic

import android.app.Activity
import android.content.ComponentName
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.provider.Settings
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

/** One screen: explains kMusic and asks for notification access (needed to read what's playing). */
class MainActivity : Activity() {

    private lateinit var status: TextView
    private lateinit var button: Button

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        window.statusBarColor = Color.parseColor("#2A1736")
        window.navigationBarColor = Color.parseColor("#1A0F22")
        val dp = resources.displayMetrics.density
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding((28 * dp).toInt(), (72 * dp).toInt(), (28 * dp).toInt(), (28 * dp).toInt())
            background = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                intArrayOf(Color.parseColor("#4B2A5C"), Color.parseColor("#1A0F22")))
        }
        root.addView(TextView(this).apply {
            text = "kMusic"; textSize = 44f; setTextColor(Color.WHITE); typeface = Typeface.DEFAULT_BOLD; gravity = Gravity.CENTER
        })
        root.addView(TextView(this).apply {
            text = "Now playing on your Pebble"; textSize = 18f; setTextColor(Color.parseColor("#D9C8E6")); gravity = Gravity.CENTER
            setPadding(0, (4 * dp).toInt(), 0, (36 * dp).toInt())
        })
        root.addView(TextView(this).apply {
            text = "Album art, song, artist and progress from YouTube Music (or any music app) on your watch.\n\n" +
                   "Up  -  previous\nSelect  -  play / pause\nDown  -  next\nHold Up / Down  -  volume\nHold Select  -  like, info, dislike"
            textSize = 16f; setTextColor(Color.parseColor("#EDE3F3")); setLineSpacing(0f, 1.25f)
        })
        status = TextView(this).apply {
            textSize = 16f; typeface = Typeface.DEFAULT_BOLD; gravity = Gravity.CENTER
            setPadding(0, (36 * dp).toInt(), 0, (14 * dp).toInt())
        }
        root.addView(status)
        button = Button(this).apply {
            textSize = 16f; isAllCaps = false; setTextColor(Color.parseColor("#2A1736"))
            background = GradientDrawable().apply { setColor(Color.WHITE); cornerRadius = 28 * dp }
            setPadding((24 * dp).toInt(), (14 * dp).toInt(), (24 * dp).toInt(), (14 * dp).toInt())
            setOnClickListener { startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)) }
        }
        root.addView(button, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        root.addView(TextView(this).apply {
            text = "Then open kMusic on your watch."
            textSize = 14f; setTextColor(Color.parseColor("#B9A5C7")); gravity = Gravity.CENTER
            setPadding(0, (18 * dp).toInt(), 0, 0)
        })
        setContentView(root)
    }

    override fun onResume() {
        super.onResume()
        val flat = Settings.Secure.getString(contentResolver, "enabled_notification_listeners") ?: ""
        val ok = flat.contains(ComponentName(this, MusicService::class.java).flattenToString())
        status.text = if (ok) "✓  Connected - music will show on your watch" else "Step 1: allow kMusic to see what's playing"
        status.setTextColor(Color.parseColor(if (ok) "#9BE7A8" else "#FFD27A"))
        button.text = if (ok) "Notification access settings" else "Allow notification access"
    }
}

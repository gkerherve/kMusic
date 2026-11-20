package uk.kherve.kmusic

import android.app.Activity
import android.content.ComponentName
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import java.util.Locale

/**
 * Setup screen. Sideloaded apps on Android 13+ get "Restricted setting" when asking for
 * notification access, so this walks the user through every step, with a button for each
 * and a green tick once it's done. French or English, following the phone's language.
 */
class MainActivity : Activity() {

    private val fr = Locale.getDefault().language == "fr"
    private fun t(en: String, frText: String) = if (fr) frText else en

    private lateinit var steps: LinearLayout
    private lateinit var status: TextView
    private val dp get() = resources.displayMetrics.density

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        window.statusBarColor = Color.parseColor("#2A1736")
        window.navigationBarColor = Color.parseColor("#1A0F22")
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(px(24), px(56), px(24), px(32))
        }
        root.addView(text("kMusic", 40f, Color.WHITE, bold = true, center = true))
        root.addView(text(t("Now playing on your Pebble", "La musique sur ta Pebble"), 17f, Color.parseColor("#D9C8E6"), center = true).also {
            it.setPadding(0, px(2), 0, px(20))
        })
        status = text("", 17f, Color.WHITE, bold = true, center = true).also { it.setPadding(px(12), px(14), px(12), px(14)) }
        root.addView(status)
        steps = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(steps)

        // ---- which app to control
        root.addView(text(t("Control", "Contrôler"), 20f, Color.WHITE, bold = true).also { it.setPadding(0, px(28), 0, px(4)) })
        val prefs = getSharedPreferences("kmusic", MODE_PRIVATE)
        val chosen = prefs.getString("player", "") ?: ""
        val group = RadioGroup(this)
        fun option(label: String, pkg: String) {
            group.addView(RadioButton(this).apply {
                text = label; textSize = 17f; setTextColor(Color.parseColor("#EDE3F3"))
                buttonTintList = ColorStateList.valueOf(Color.WHITE)
                setPadding(px(8), px(10), 0, px(10))
                id = View.generateViewId()
                isChecked = pkg == chosen
                setOnClickListener { prefs.edit().putString("player", pkg).apply() }
            })
        }
        option(t("Automatic  -  whatever is playing", "Automatique  -  ce qui joue"), "")
        for ((label, pkg) in musicApps()) option(label, pkg)
        root.addView(group)

        // ---- watch buttons
        root.addView(text(t("On the watch", "Sur la montre"), 20f, Color.WHITE, bold = true).also { it.setPadding(0, px(28), 0, px(6)) })
        root.addView(text(t(
            "Up  -  previous\nSelect  -  play / pause\nDown  -  next\nHold Up / Down  -  volume\nHold Select  -  like, info (up next), dislike",
            "Haut  -  précédent\nSelect  -  lecture / pause\nBas  -  suivant\nMaintenir Haut / Bas  -  volume\nMaintenir Select  -  j'aime, infos (à suivre), je n'aime pas"),
            16f, Color.parseColor("#EDE3F3")).also { it.setLineSpacing(0f, 1.3f) })

        setContentView(ScrollView(this).apply {
            isFillViewport = true
            background = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                intArrayOf(Color.parseColor("#4B2A5C"), Color.parseColor("#1A0F22")))
            addView(root)
        })
    }

    override fun onResume() { super.onResume(); buildSteps() }

    // ------------------------------------------------------------------ setup steps
    private fun listenerOn(): Boolean {
        val flat = Settings.Secure.getString(contentResolver, "enabled_notification_listeners") ?: ""
        return flat.contains(ComponentName(this, MusicService::class.java).flattenToString())
    }
    private fun batteryFree(): Boolean = getSystemService(PowerManager::class.java).isIgnoringBatteryOptimizations(packageName)

    private fun buildSteps() {
        steps.removeAllViews()
        val on = listenerOn()
        val restricted = Build.VERSION.SDK_INT >= 33            // Android 13+: sideloaded apps are restricted
        status.text = if (on) t("✓  Connected - your music shows on the watch", "✓  Connecté - ta musique s'affiche sur la montre")
                      else t("Setup needed - follow the steps below", "Réglage nécessaire - suis les étapes ci-dessous")
        status.setTextColor(Color.parseColor(if (on) "#9BE7A8" else "#FFD27A"))
        status.background = GradientDrawable().apply { setColor(Color.parseColor("#33FFFFFF")); cornerRadius = 16 * dp }

        if (!on && restricted) {
            step(1, false,
                t("Try once (it will be blocked)", "Essaie une fois (ce sera bloqué)"),
                t("Tap the button, find kMusic and tap it. Android shows \"Restricted setting\". That's normal: tap OK and come back here. This unlocks step 2.",
                  "Appuie sur le bouton, trouve kMusic et touche-le. Android affiche « Paramètre restreint ». C'est normal : appuie sur OK et reviens ici. Ça débloque l'étape 2."),
                t("Open notification access", "Ouvrir l'accès aux notifications")) { openListenerSettings() }
            step(2, false,
                t("Allow restricted settings", "Autoriser les paramètres restreints"),
                t("Tap the button to open kMusic's App info. Tap the ⋮ menu in the TOP-RIGHT corner, then \"Allow restricted settings\", and confirm with your PIN or fingerprint.\n\nNo ⋮ menu? Do step 1 first. On Samsung it can be under ⋮ too; on Xiaomi look for \"Allow restricted settings\" at the bottom of App info.",
                  "Appuie sur le bouton pour ouvrir les infos de kMusic. Touche le menu ⋮ en HAUT À DROITE, puis « Autoriser les paramètres restreints », et confirme avec ton code ou ton empreinte.\n\nPas de menu ⋮ ? Fais d'abord l'étape 1. Sur Samsung c'est aussi dans ⋮ ; sur Xiaomi cherche « Autoriser les paramètres restreints » en bas des infos de l'appli."),
                t("Open kMusic's App info", "Ouvrir les infos de kMusic")) { openAppInfo() }
        }
        step(if (restricted) 3 else 1, on,
            t("Turn on notification access", "Activer l'accès aux notifications"),
            t("Tap the button, tap kMusic, turn the switch ON and tap Allow. kMusic only reads what's playing; it never reads or sends your messages.",
              "Appuie sur le bouton, touche kMusic, active l'interrupteur et appuie sur Autoriser. kMusic lit seulement la musique en cours ; il ne lit ni n'envoie jamais tes messages."),
            t("Open notification access", "Ouvrir l'accès aux notifications")) { openListenerSettings() }
        step(if (restricted) 4 else 2, batteryFree(),
            t("Don't let Android stop kMusic (recommended)", "Empêcher Android d'arrêter kMusic (conseillé)"),
            t("Lets kMusic keep the watch updated when the screen is off. Tap the button and choose Allow (or \"Unrestricted\" in Battery).",
              "Permet à kMusic de garder la montre à jour écran éteint. Appuie sur le bouton et choisis Autoriser (ou « Sans restriction » dans Batterie)."),
            t("Remove battery limits", "Retirer les limites de batterie")) { openBattery() }
        step(if (restricted) 5 else 3, false,
            t("Open kMusic on the watch", "Ouvre kMusic sur la montre"),
            t("Play a song, then open kMusic on your Pebble. The cover appears after a few seconds.",
              "Lance une chanson, puis ouvre kMusic sur ta Pebble. La pochette arrive après quelques secondes."),
            null, null)
    }

    private fun step(n: Int, done: Boolean, title: String, body: String, button: String?, action: (() -> Unit)?) {
        val card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(px(16), px(14), px(16), px(16))
            background = GradientDrawable().apply {
                setColor(Color.parseColor(if (done) "#2A9BE7A8" else "#26FFFFFF")); cornerRadius = 18 * dp
                if (done) setStroke(px(1), Color.parseColor("#9BE7A8"))
            }
        }
        card.addView(text((if (done) "✓  " else "$n.  ") + title, 18f, if (done) Color.parseColor("#9BE7A8") else Color.WHITE, bold = true))
        if (!done) {
            card.addView(text(body, 15f, Color.parseColor("#E3D6EC")).also { it.setPadding(0, px(6), 0, 0); it.setLineSpacing(0f, 1.2f) })
            if (button != null && action != null) card.addView(Button(this).apply {
                text = button; textSize = 15f; isAllCaps = false; setTextColor(Color.parseColor("#2A1736"))
                background = GradientDrawable().apply { setColor(Color.WHITE); cornerRadius = 24 * dp }
                setPadding(px(18), px(10), px(18), px(10))
                setOnClickListener { action() }
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = px(12) })
        }
        steps.addView(card, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = px(12) })
    }

    private fun openListenerSettings() {
        val direct = Intent("android.settings.NOTIFICATION_LISTENER_DETAIL_SETTINGS")
            .putExtra("android.provider.extra.NOTIFICATION_LISTENER_COMPONENT_NAME", ComponentName(this, MusicService::class.java).flattenToString())
        try { startActivity(direct) } catch (_: Exception) { startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)) }
    }
    private fun openAppInfo() = startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName")))
    private fun openBattery() {
        try { startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName"))) }
        catch (_: Exception) { openAppInfo() }
    }

    // ------------------------------------------------------------------ helpers
    private fun px(v: Int) = (v * dp).toInt()
    private fun text(s: String, size: Float, color: Int, bold: Boolean = false, center: Boolean = false) = TextView(this).apply {
        text = s; textSize = size; setTextColor(color)
        if (bold) typeface = Typeface.DEFAULT_BOLD
        if (center) gravity = Gravity.CENTER
    }

    /** Installed apps that can play media (they offer a media browser or handle media buttons). */
    private fun musicApps(): List<Pair<String, String>> {
        val pm = packageManager
        val pkgs = LinkedHashSet<String>()
        pm.queryIntentServices(Intent("android.media.browse.MediaBrowserService"), 0).forEach { pkgs += it.serviceInfo.packageName }
        pm.queryBroadcastReceivers(Intent(Intent.ACTION_MEDIA_BUTTON), 0).forEach { pkgs += it.activityInfo.packageName }
        pkgs -= packageName
        return pkgs.mapNotNull { p ->
            try { pm.getApplicationLabel(pm.getApplicationInfo(p, 0)).toString() to p } catch (_: Exception) { null }
        }.filter { pm.getLaunchIntentForPackage(it.second) != null }
         .sortedBy { it.first.lowercase() }
    }
}

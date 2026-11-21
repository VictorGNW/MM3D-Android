package org.triaevum.android

import android.app.Activity
import android.app.ActivityManager
import android.app.AlertDialog
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.content.Intent

/**
 * Opens the game from the installer. The game runs once per process (SDL activity, title library,
 * guest memory): when its activity is already running, it comes to the front instead of a second
 * instance being started over it. Idea from Ocarina ReTuned's GameSession, reimplemented.
 */
object GameSession {
    /** Flags of the game intent: an existing game activity in this task is reordered, not duplicated. */
    const val FLAGS = Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_SINGLE_TOP

    private val GAME = TriAevumActivity::class.java.name

    private const val PROBE_FAILED = -1
    private const val UNSUPPORTED = 0
    private const val COMPATIBILITY = 1
    private const val STANDARD = 2
    private var launchPending = false

    fun open(activity: Activity) {
        // A game started in a task of its own (a developer launch, or a task the system split
        // off): bring that task forward.
        try {
            val tasks = activity.getSystemService(ActivityManager::class.java).appTasks
            for (task in tasks) {
                val info = task.taskInfo
                if (info.taskId == activity.taskId) continue
                if (info.baseActivity?.className == GAME || info.topActivity?.className == GAME) {
                    task.moveToFront()
                    return
                }
            }
        } catch (_: RuntimeException) {
            // A task that ended meanwhile: start the game as usual.
        }
        if (launchPending) return
        launchPending = true
        Thread({
            val tier = try {
                System.loadLibrary("TriAevum")
                NativeHostBridge.probeSystemVulkan()
            } catch (error: UnsatisfiedLinkError) {
                Log.e("TriAevum", "Cannot load Vulkan probe", error)
                PROBE_FAILED
            }
            activity.runOnUiThread {
                launchPending = false
                if (!activity.isFinishing && !activity.isDestroyed) {
                    when (tier) {
                        STANDARD -> activity.startActivity(
                            Intent(activity, TriAevumActivity::class.java).addFlags(FLAGS)
                        )
                        COMPATIBILITY -> AlertDialog.Builder(activity)
                            .setMessage(R.string.vulkan_compatibility_pending)
                            .setPositiveButton(android.R.string.ok, null)
                            .show()
                        UNSUPPORTED -> closeUnsupported(activity)
                        else -> AlertDialog.Builder(activity)
                            .setMessage(R.string.vulkan_probe_failed)
                            .setPositiveButton(android.R.string.ok, null)
                            .show()
                    }
                }
            }
        }, "Vulkan probe").start()
    }

    private fun closeUnsupported(activity: Activity) {
        val dialog = AlertDialog.Builder(activity)
            .setMessage(R.string.vulkan_unsupported)
            .setCancelable(false)
            .show()
        Handler(Looper.getMainLooper()).postDelayed({
            if (!activity.isFinishing && !activity.isDestroyed) {
                dialog.dismiss()
                activity.finish()
            }
        }, 3000L)
    }
}

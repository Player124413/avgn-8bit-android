package ru.avgn8bit

import android.app.Application

class AvgnApp : Application() {
    lateinit var settings: AppSettings
        private set

    override fun onCreate() {
        super.onCreate()
        settings = AppSettings(this)
        AppLog.init(GameStorage.logFile(this))
        Haptics.init(this)
        Haptics.configure(settings.haptics, settings.hapticsStrength)
        AppLog.write("AVGN 8-bit Android ${BuildConfig.VERSION_NAME} started (${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}, Android ${android.os.Build.VERSION.RELEASE})")
    }
}

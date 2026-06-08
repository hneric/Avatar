package com.esps31.companion.data

import android.content.Context
import java.security.SecureRandom

class BoundDeviceStore(context: Context) {
    private val prefs = context.getSharedPreferences("s31_bound_device", Context.MODE_PRIVATE)

    val address: String?
        get() = prefs.getString("address", null)

    val token: String?
        get() = prefs.getString("token", null)

    fun bind(address: String, token: String = newToken()) {
        prefs.edit()
            .putString("address", address)
            .putString("token", token)
            .apply()
    }

    fun clear() {
        prefs.edit().clear().apply()
    }

    private fun newToken(): String {
        val bytes = ByteArray(16)
        SecureRandom().nextBytes(bytes)
        return bytes.joinToString("") { "%02x".format(it) }
    }
}

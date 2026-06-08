package com.esps31.companion.data

data class S31Device(
    val name: String,
    val address: String,
    val rssi: Int,
    val serviceUuids: List<String>,
)

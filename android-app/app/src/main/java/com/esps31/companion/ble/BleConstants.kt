package com.esps31.companion.ble

import java.util.UUID

object BleConstants {
    const val DEVICE_PREFIX = "S31_"
    val PROVISIONING_SERVICE_UUIDS: Set<String> = setOf(
        "021a9031-0382-4aea-bff4-6b3f1c5adfb4",
        "b4df5a1c-3f6b-f4bf-ea4a-820331901a02",
    )
    val PRESENCE_SERVICE_UUID: UUID = UUID.fromString("53455250-3153-9991-6a44-b27d00005331")
    val PRESENCE_CHARACTERISTIC_UUID: UUID = UUID.fromString("53455250-3153-9991-6a44-b27d01005331")
}

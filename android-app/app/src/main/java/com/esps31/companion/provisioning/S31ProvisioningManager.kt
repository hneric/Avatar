package com.esps31.companion.provisioning

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import androidx.core.content.ContextCompat
import com.esps31.companion.ble.BleConstants
import com.esps31.companion.data.S31Device
import com.espressif.provisioning.DeviceConnectionEvent
import com.espressif.provisioning.ESPConstants
import com.espressif.provisioning.ESPDevice
import com.espressif.provisioning.ESPProvisionManager
import com.espressif.provisioning.WiFiAccessPoint
import com.espressif.provisioning.listeners.ProvisionListener
import com.espressif.provisioning.listeners.WiFiScanListener
import org.greenrobot.eventbus.EventBus
import org.greenrobot.eventbus.Subscribe
import org.greenrobot.eventbus.ThreadMode

data class S31WifiNetwork(
    val ssid: String,
    val rssi: Int,
    val security: Int,
)

class S31ProvisioningManager(
    private val context: Context,
    private val onStatus: (String) -> Unit,
    private val onDiagnostic: (String) -> Unit = {},
) {
    enum class Action {
        NONE,
        SCAN_WIFI,
        PROVISION,
    }

    private val manager = ESPProvisionManager.getInstance(context.applicationContext)
    private var pendingSsid: String? = null
    private var pendingPassword: String? = null
    private var pendingPop: String = "s31pop"
    private var targetDevice: S31Device? = null
    private var wifiResultCallback: ((List<S31WifiNetwork>) -> Unit)? = null
    private var espDevice: ESPDevice? = null
    private var connected = false
    private var action = Action.NONE

    @SuppressLint("MissingPermission")
    fun scanWifiNetworks(device: S31Device?, pop: String, onResult: (List<S31WifiNetwork>) -> Unit) {
        debug("scanWifiNetworks target=$device pop=$pop")
        if (!hasProvisioningPermissions()) {
            onStatus("请先允许蓝牙和位置信息权限")
            return
        }
        if (device == null) {
            onStatus("请先在附近设备列表中发现 S31 设备")
            return
        }
        if (action != Action.NONE) {
            onStatus("设备正在处理上一个操作")
            return
        }
        pendingPop = pop
        targetDevice = device
        wifiResultCallback = onResult
        action = Action.SCAN_WIFI
        ensureEventBus()
        onStatus("正在连接设备并扫描 WiFi")

        if (connected && espDevice != null) {
            scanNetworksOnConnectedDevice()
        } else {
            connectProvisioningDevice(device)
        }
    }

    @SuppressLint("MissingPermission")
    fun provision(device: S31Device?, ssid: String, password: String, pop: String) {
        debug("provision target=$device ssid=$ssid pop=$pop")
        if (!hasProvisioningPermissions()) {
            onStatus("请先允许蓝牙和位置信息权限")
            return
        }
        if (device == null && espDevice == null) {
            onStatus("请先扫描 WiFi 或选择一台 S31 设备")
            return
        }
        if (action != Action.NONE) {
            onStatus("设备正在处理上一个操作")
            return
        }
        if (ssid.isBlank()) {
            onStatus("请先选择或输入 WiFi 名称")
            return
        }

        pendingSsid = ssid
        pendingPassword = password
        pendingPop = pop
        targetDevice = device ?: targetDevice
        action = Action.PROVISION
        ensureEventBus()
        onStatus("正在准备发送 WiFi 信息")

        if (connected && espDevice != null) {
            provisionConnectedDevice()
        } else {
            connectProvisioningDevice(targetDevice)
        }
    }

    @SuppressLint("MissingPermission")
    private fun connectProvisioningDevice(target: S31Device?) {
        debug("connectProvisioningDevice target=$target")
        if (target == null) {
            failAndDisconnect("请先选择一台 S31 设备")
            return
        }
        val serviceUuid = target.provisioningServiceUuid()
        debug("provisioning service selected=$serviceUuid all=${target.serviceUuids}")
        if (serviceUuid.isNullOrBlank()) {
            failAndDisconnect("设备当前不是配网模式，请先擦除设备 WiFi 信息并确认屏幕显示蓝牙配网")
            return
        }
        try {
            manager.stopBleScan()
            val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter
            val bleDevice = adapter?.getRemoteDevice(target.address)
            if (bleDevice == null) {
                failAndDisconnect("无法获取蓝牙设备：${target.address}")
                return
            }
            onStatus("正在连接 ${target.name}")
            espDevice = manager.createESPDevice(
                ESPConstants.TransportType.TRANSPORT_BLE,
                ESPConstants.SecurityType.SECURITY_1,
            ).apply {
                setProofOfPossession(pendingPop)
                connectBLEDevice(bleDevice, serviceUuid)
            }
        } catch (e: Exception) {
            failAndDisconnect("蓝牙连接异常：${e.message ?: e.javaClass.simpleName}")
        }
    }

    @Subscribe(threadMode = ThreadMode.MAIN)
    fun onDeviceConnectionEvent(event: DeviceConnectionEvent) {
        debug("DeviceConnectionEvent type=${event.eventType} action=$action")
        when (event.eventType) {
            ESPConstants.EVENT_DEVICE_CONNECTED -> {
                connected = true
                when (action) {
                    Action.SCAN_WIFI -> scanNetworksOnConnectedDevice()
                    Action.PROVISION -> provisionConnectedDevice()
                    Action.NONE -> onStatus("设备已连接")
                }
            }

            ESPConstants.EVENT_DEVICE_CONNECTION_FAILED -> failAndDisconnect("设备连接失败")
            ESPConstants.EVENT_DEVICE_DISCONNECTED -> {
                connected = false
                if (action != Action.NONE) {
                    failAndDisconnect("设备连接已断开")
                }
            }
        }
    }

    private fun scanNetworksOnConnectedDevice() {
        debug("scanNetworksOnConnectedDevice")
        val device = espDevice
        if (device == null) {
            failAndDisconnect("设备连接状态异常")
            return
        }
        onStatus("正在扫描设备周围 WiFi")
        try {
            device.scanNetworks(object : WiFiScanListener {
                override fun onWifiListReceived(wifiList: ArrayList<WiFiAccessPoint>) {
                    debug("wifi list received count=${wifiList.size}")
                    val networks = wifiList
                        .mapNotNull { ap ->
                            val ssid = ap.wifiName ?: return@mapNotNull null
                            if (ssid.isBlank()) return@mapNotNull null
                            S31WifiNetwork(ssid = ssid, rssi = ap.rssi, security = ap.security)
                        }
                        .distinctBy { it.ssid }
                        .sortedByDescending { it.rssi }
                    wifiResultCallback?.invoke(networks)
                    action = Action.NONE
                    onStatus(if (networks.isEmpty()) "没有扫描到 WiFi" else "请选择 WiFi 并输入密码")
                }

                override fun onWiFiScanFailed(e: Exception) {
                    debug("wifi scan failed: ${e.message ?: e.javaClass.simpleName}")
                    Log.e(TAG, "wifi scan failed", e)
                    failAndDisconnect("WiFi 扫描失败：${e.message ?: e.javaClass.simpleName}")
                }
            })
        } catch (e: Exception) {
            debug("wifi scan exception: ${e.message ?: e.javaClass.simpleName}")
            Log.e(TAG, "wifi scan exception", e)
            failAndDisconnect("WiFi 扫描异常：${e.message ?: e.javaClass.simpleName}")
        }
    }

    private fun provisionConnectedDevice() {
        val device = espDevice
        val ssid = pendingSsid
        val password = pendingPassword
        if (device == null || ssid == null || password == null) {
            failAndDisconnect("配网状态异常")
            return
        }
        onStatus("正在发送 WiFi 名称和密码")
        try {
            device.provision(ssid, password, provisionListener)
        } catch (e: Exception) {
            debug("provision exception: ${e.message ?: e.javaClass.simpleName}")
            Log.e(TAG, "provision exception", e)
            failAndDisconnect("发送配网信息异常：${e.message ?: e.javaClass.simpleName}")
        }
    }

    private val provisionListener = object : ProvisionListener {
        override fun createSessionFailed(e: Exception) {
            debug("create session failed: ${e.message ?: e.javaClass.simpleName}")
            Log.e(TAG, "create session failed", e)
            failAndDisconnect("安全握手失败：${e.message ?: e.javaClass.simpleName}")
        }

        override fun wifiConfigSent() {
            onStatus("WiFi 信息已发送")
        }

        override fun wifiConfigFailed(e: Exception) {
            debug("wifi config failed: ${e.message ?: e.javaClass.simpleName}")
            Log.e(TAG, "wifi config failed", e)
            failAndDisconnect("WiFi 信息发送失败：${e.message ?: e.javaClass.simpleName}")
        }

        override fun wifiConfigApplied() {
            onStatus("设备正在连接 WiFi")
        }

        override fun wifiConfigApplyFailed(e: Exception) {
            debug("wifi config apply failed: ${e.message ?: e.javaClass.simpleName}")
            Log.e(TAG, "wifi config apply failed", e)
            failAndDisconnect("设备应用 WiFi 信息失败：${e.message ?: e.javaClass.simpleName}")
        }

        override fun provisioningFailedFromDevice(failureReason: ESPConstants.ProvisionFailureReason) {
            debug("device provisioning failed: $failureReason")
            Log.e(TAG, "device provisioning failed: $failureReason")
            failAndDisconnect("设备配网失败：$failureReason")
        }

        override fun deviceProvisioningSuccess() {
            finishAndDisconnect("WiFi 配网成功")
        }

        override fun onProvisioningFailed(e: Exception) {
            debug("provisioning failed: ${e.message ?: e.javaClass.simpleName}")
            Log.e(TAG, "provisioning failed", e)
            failAndDisconnect("配网失败：${e.message ?: e.javaClass.simpleName}")
        }
    }

    fun stop() {
        if (action != Action.NONE || connected || espDevice != null) {
            finishAndDisconnect("已停止配网")
        }
    }

    private fun ensureEventBus() {
        if (!EventBus.getDefault().isRegistered(this)) {
            EventBus.getDefault().register(this)
        }
    }

    private fun hasProvisioningPermissions(): Boolean {
        val permissions = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.ACCESS_FINE_LOCATION,
            )
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        return permissions.all {
            ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
        }
    }

    private fun S31Device.provisioningServiceUuid(): String? {
        return serviceUuids.firstOrNull { uuid ->
            uuid.lowercase() in BleConstants.PROVISIONING_SERVICE_UUIDS
        }
    }

    private fun failAndDisconnect(message: String) {
        finishAndDisconnect(message)
    }

    private fun finishAndDisconnect(message: String) {
        debug("finishAndDisconnect: $message")
        onStatus(message)
        try {
            manager.stopBleScan()
        } catch (_: Exception) {
        }
        action = Action.NONE
        connected = false
        try {
            espDevice?.disconnectDevice()
        } catch (_: Exception) {
        }
        espDevice = null
        pendingSsid = null
        pendingPassword = null
        pendingPop = "s31pop"
        targetDevice = null
        wifiResultCallback = null
        if (EventBus.getDefault().isRegistered(this)) {
            EventBus.getDefault().unregister(this)
        }
    }

    companion object {
        private const val TAG = "S31Provisioning"
    }

    private fun debug(message: String) {
        Log.i(TAG, message)
        onDiagnostic(message)
    }
}

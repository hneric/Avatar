package com.esps31.companion.ble

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import com.esps31.companion.data.S31Device

class BleScanner(
    private val context: Context,
    private val onDevice: (S31Device) -> Unit,
    private val onStatus: (String) -> Unit,
) {
    private val bluetoothManager = context.getSystemService(BluetoothManager::class.java)
    private val adapter: BluetoothAdapter? = bluetoothManager?.adapter
    private var scanning = false

    private val callback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device ?: return
            val name = result.scanRecord?.deviceName ?: safeName(device.name) ?: return
            if (!name.startsWith(BleConstants.DEVICE_PREFIX)) return
            val serviceUuids = result.scanRecord?.serviceUuids
                ?.map { it.uuid.toString().lowercase() }
                .orEmpty()
            onDevice(S31Device(name = name, address = device.address, rssi = result.rssi, serviceUuids = serviceUuids))
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            onStatus("蓝牙扫描失败：$errorCode")
        }
    }

    @SuppressLint("MissingPermission")
    fun start() {
        if (!hasScanPermission()) {
            onStatus("缺少蓝牙扫描权限")
            return
        }
        val scanner = adapter?.bluetoothLeScanner
        if (scanner == null) {
            onStatus("当前手机无法使用低功耗蓝牙扫描")
            return
        }
        if (scanning) return
        scanning = true
        scanner.startScan(callback)
        onStatus("正在扫描 S31 设备")
    }

    @SuppressLint("MissingPermission")
    fun stop() {
        if (!scanning || !hasScanPermission()) return
        adapter?.bluetoothLeScanner?.stopScan(callback)
        scanning = false
        onStatus("已停止扫描")
    }

    private fun hasScanPermission(): Boolean {
        val permission = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            Manifest.permission.BLUETOOTH_SCAN
        } else {
            Manifest.permission.ACCESS_FINE_LOCATION
        }
        return ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED
    }

    @Suppress("DEPRECATION")
    private fun safeName(name: String?): String? = name
}

package com.esps31.companion.ble

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat

class PresenceClient(
    private val context: Context,
    private val onStatus: (String) -> Unit,
) {
    private var gatt: BluetoothGatt? = null
    private var pendingPayload: String? = null

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                onStatus("已连接设备，正在查找问候服务")
                discoverServices(gatt)
            } else {
                onStatus("问候蓝牙连接已断开")
                close()
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            val service = gatt.getService(BleConstants.PRESENCE_SERVICE_UUID)
            val characteristic = service?.getCharacteristic(BleConstants.PRESENCE_CHARACTERISTIC_UUID)
            if (service == null || characteristic == null) {
                onStatus("未找到设备问候服务")
                disconnect()
                return
            }
            val payload = pendingPayload ?: return
            write(gatt, characteristic, payload)
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            onStatus(if (status == BluetoothGatt.GATT_SUCCESS) "问候指令已发送" else "问候指令发送失败：$status")
            disconnect()
        }
    }

    fun bind(address: String, token: String) {
        connectAndWrite(address, """{"event":"bind","token":"$token"}""")
    }

    fun nearby(address: String, token: String) {
        connectAndWrite(address, """{"event":"nearby","token":"$token"}""")
    }

    fun resetBinding(address: String, token: String) {
        connectAndWrite(address, """{"event":"reset_bind","token":"$token"}""")
    }

    @SuppressLint("MissingPermission")
    private fun connectAndWrite(address: String, payload: String) {
        if (!hasConnectPermission()) {
            onStatus("缺少蓝牙连接权限")
            return
        }
        val manager = context.getSystemService(BluetoothManager::class.java)
        val device: BluetoothDevice = manager.adapter.getRemoteDevice(address)
        pendingPayload = payload
        gatt = device.connectGatt(context, false, callback)
        onStatus("正在连接 $address")
    }

    @SuppressLint("MissingPermission")
    private fun discoverServices(gatt: BluetoothGatt) {
        if (hasConnectPermission()) gatt.discoverServices()
    }

    @SuppressLint("MissingPermission")
    private fun write(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, payload: String) {
        characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        val bytes = payload.toByteArray(Charsets.UTF_8)
        val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeCharacteristic(characteristic, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            characteristic.value = bytes
            @Suppress("DEPRECATION")
            gatt.writeCharacteristic(characteristic)
        }
        if (!ok) {
            onStatus("问候指令写入请求失败")
            disconnect()
        }
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        if (hasConnectPermission()) gatt?.disconnect()
    }

    @SuppressLint("MissingPermission")
    fun close() {
        if (hasConnectPermission()) gatt?.close()
        gatt = null
        pendingPayload = null
    }

    private fun hasConnectPermission(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return true
        return ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
    }
}

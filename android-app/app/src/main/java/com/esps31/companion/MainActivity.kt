package com.esps31.companion

import android.Manifest
import android.os.Build
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.esps31.companion.ble.BleConstants
import com.esps31.companion.ble.BleScanner
import com.esps31.companion.ble.PresenceClient
import com.esps31.companion.data.BoundDeviceStore
import com.esps31.companion.data.S31Device
import com.esps31.companion.provisioning.S31ProvisioningManager
import com.esps31.companion.provisioning.S31WifiNetwork

class MainActivity : ComponentActivity() {
    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestBlePermissions()

        setContent {
            MaterialTheme {
                val store = remember { BoundDeviceStore(this) }
                val devices = remember { mutableStateMapOf<String, S31Device>() }
                var status by remember { mutableStateOf("正在扫描附近设备") }
                var boundAddress by remember { mutableStateOf(store.address) }
                var boundToken by remember { mutableStateOf(store.token) }
                var lastNearbyAt by remember { mutableLongStateOf(0L) }
                var ssid by remember { mutableStateOf("") }
                var password by remember { mutableStateOf("") }
                var pop by remember { mutableStateOf("s31pop") }
                var wifiNetworks by remember { mutableStateOf(emptyList<S31WifiNetwork>()) }
                var wifiListVisible by remember { mutableStateOf(false) }
                val sortedDevices = devices.values.sortedByDescending { it.rssi }
                val provisioningTarget = sortedDevices
                    .filter { it.isProvisioningMode() }
                    .maxByOrNull { it.rssi }

                val setStatus: (String) -> Unit = { text ->
                    Log.i(TAG, text)
                    runOnUiThread { status = text }
                }
                val provisioning = remember { S31ProvisioningManager(this, setStatus) }
                val presenceClient = remember { PresenceClient(this, setStatus) }

                val scanner = remember {
                    BleScanner(
                        context = this,
                        onDevice = { device ->
                            runOnUiThread {
                                devices[device.address] = device
                                val token = boundToken
                                val now = System.currentTimeMillis()
                                if (device.address == boundAddress &&
                                    token != null &&
                                    device.rssi >= NEARBY_RSSI_THRESHOLD &&
                                    now - lastNearbyAt >= NEARBY_COOLDOWN_MS
                                ) {
                                    lastNearbyAt = now
                                    presenceClient.nearby(device.address, token)
                                }
                            }
                        },
                        onStatus = setStatus,
                    )
                }

                DisposableEffect(Unit) {
                    scanner.start()
                    onDispose {
                        scanner.stop()
                        presenceClient.close()
                        provisioning.stop()
                    }
                }

                S31App(
                    status = status,
                    boundAddress = boundAddress,
                    devices = sortedDevices,
                    provisioningTarget = provisioningTarget,
                    ssid = ssid,
                    password = password,
                    pop = pop,
                    wifiNetworks = wifiNetworks,
                    wifiListVisible = wifiListVisible,
                    onStartScan = scanner::start,
                    onStopScan = scanner::stop,
                    onBind = { device ->
                        val token = boundToken ?: newAppToken()
                        store.bind(device.address, token)
                        boundAddress = device.address
                        boundToken = token
                        presenceClient.bind(device.address, token)
                    },
                    onSsidChange = { ssid = it },
                    onPasswordChange = { password = it },
                    onPopChange = { pop = it },
                    onWifiSelected = { network ->
                        ssid = network.ssid
                        wifiListVisible = false
                        status = "已选择 WiFi：${network.ssid}"
                    },
                    onScanWifi = {
                        wifiListVisible = false
                        scanner.stop()
                        provisioning.scanWifiNetworks(provisioningTarget, pop) { networks ->
                            runOnUiThread {
                                wifiNetworks = networks
                                wifiListVisible = networks.isNotEmpty()
                                status = if (networks.isEmpty()) {
                                    "未扫描到 WiFi，请确认设备处于配网模式"
                                } else {
                                    "已扫描到 ${networks.size} 个 WiFi"
                                }
                            }
                        }
                    },
                    onProvision = {
                        wifiListVisible = false
                        scanner.stop()
                        provisioning.provision(provisioningTarget, ssid, password, pop)
                    },
                )
            }
        }
    }

    private fun requestBlePermissions() {
        val permissions = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.ACCESS_FINE_LOCATION,
            )
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        permissionLauncher.launch(permissions)
    }

    private fun newAppToken(): String {
        val bytes = ByteArray(16)
        java.security.SecureRandom().nextBytes(bytes)
        return bytes.joinToString("") { "%02x".format(it) }
    }

    companion object {
        private const val TAG = "S31Companion"
        private const val NEARBY_RSSI_THRESHOLD = -65
        private const val NEARBY_COOLDOWN_MS = 30_000L
    }
}

@androidx.compose.runtime.Composable
private fun S31App(
    status: String,
    boundAddress: String?,
    devices: List<S31Device>,
    provisioningTarget: S31Device?,
    ssid: String,
    password: String,
    pop: String,
    wifiNetworks: List<S31WifiNetwork>,
    wifiListVisible: Boolean,
    onStartScan: () -> Unit,
    onStopScan: () -> Unit,
    onBind: (S31Device) -> Unit,
    onSsidChange: (String) -> Unit,
    onPasswordChange: (String) -> Unit,
    onPopChange: (String) -> Unit,
    onWifiSelected: (S31WifiNetwork) -> Unit,
    onScanWifi: () -> Unit,
    onProvision: () -> Unit,
) {
    var wifiExpanded by remember { mutableStateOf(true) }

    Surface(color = PageBg) {
        Column(modifier = Modifier.fillMaxSize()) {
            LazyColumn(
                modifier = Modifier
                    .weight(1f)
                    .padding(horizontal = 18.dp, vertical = 18.dp),
                verticalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                item {
                    Header(hasProvisioningTarget = provisioningTarget != null)
                }
                item {
                    BindingCard(boundAddress = boundAddress)
                }
                item {
                    DeviceOverviewCard(
                        boundAddress = boundAddress,
                        devices = devices,
                        provisioningTarget = provisioningTarget,
                        onStartScan = onStartScan,
                        onStopScan = onStopScan,
                        onBind = onBind,
                    )
                }
                item {
                    WifiProvisionCard(
                        expanded = wifiExpanded,
                        onToggleExpanded = { wifiExpanded = !wifiExpanded },
                        provisioningTarget = provisioningTarget,
                        ssid = ssid,
                        password = password,
                        pop = pop,
                        wifiNetworks = wifiNetworks,
                        wifiListVisible = wifiListVisible,
                        onSsidChange = onSsidChange,
                        onPasswordChange = onPasswordChange,
                        onPopChange = onPopChange,
                        onWifiSelected = onWifiSelected,
                        onScanWifi = onScanWifi,
                        onProvision = onProvision,
                    )
                }
            }
            StatusBar(status = status)
        }
    }
}

@androidx.compose.runtime.Composable
private fun Header(hasProvisioningTarget: Boolean) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.Top,
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.weight(1f)) {
            Text(
                "S31 设备助手",
                style = MaterialTheme.typography.headlineLarge,
                fontSize = 30.sp,
                lineHeight = 34.sp,
                fontWeight = FontWeight.Bold,
                color = Ink,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                "蓝牙配网与设备绑定",
                style = MaterialTheme.typography.titleMedium,
                fontSize = 16.sp,
                color = Muted,
            )
        }
        StatusPill(
            text = if (hasProvisioningTarget) "蓝牙配网可用" else "正在扫描",
            active = hasProvisioningTarget,
        )
    }
}

@androidx.compose.runtime.Composable
private fun StatusPill(text: String, active: Boolean) {
    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(8.dp))
            .background(Color(0xFFEFF6FF))
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(7.dp),
    ) {
        IconBadge(iconRes = R.drawable.ic_ble, color = Primary, size = 26)
        Text(text, color = Primary, fontWeight = FontWeight.SemiBold, fontSize = 14.sp, maxLines = 1)
        Box(
            modifier = Modifier
                .size(8.dp)
                .background(if (active) Success else Color(0xFF94A3B8), CircleShape)
        )
    }
}

@androidx.compose.runtime.Composable
private fun BindingCard(boundAddress: String?) {
    SectionCard {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            IconBadge(iconRes = R.drawable.ic_check, color = Primary, size = 32)
            Text("已绑定：", color = Ink, fontWeight = FontWeight.SemiBold, fontSize = 16.sp)
            Text(
                boundAddress ?: "未绑定设备",
                modifier = Modifier.weight(1f),
                color = Ink,
                fontSize = 16.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            SmallIcon(iconRes = R.drawable.ic_chevron_right, tint = Color(0xFF94A3B8))
        }
    }
}

@androidx.compose.runtime.Composable
private fun DeviceOverviewCard(
    boundAddress: String?,
    devices: List<S31Device>,
    provisioningTarget: S31Device?,
    onStartScan: () -> Unit,
    onStopScan: () -> Unit,
    onBind: (S31Device) -> Unit,
) {
    SectionCard {
        CardHeader(iconRes = R.drawable.ic_scan, title = "附近设备", trailingIconRes = R.drawable.ic_refresh)
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Button(
                onClick = onStartScan,
                colors = ButtonDefaults.buttonColors(containerColor = Primary),
                shape = RoundedCornerShape(8.dp),
            ) {
                ButtonIcon(iconRes = R.drawable.ic_scan, tint = Color.White)
                Text("扫描设备", fontSize = 15.sp)
            }
            OutlinedButton(
                onClick = onStopScan,
                shape = RoundedCornerShape(8.dp),
            ) {
                ButtonIcon(iconRes = R.drawable.ic_stop, tint = Primary)
                Text("停止", fontSize = 15.sp)
            }
        }

        TargetRow(
            label = "配网目标",
            value = provisioningTarget?.let { "${it.name}  ${it.rssi} dBm" } ?: "未发现配网模式设备",
        )

        if (devices.isEmpty()) {
            EmptyState("暂未发现 S31 设备")
        } else {
            devices.take(5).forEach { device ->
                DeviceRow(
                    device = device,
                    isBound = device.address == boundAddress,
                    onBind = { onBind(device) },
                )
            }
        }
    }
}

@androidx.compose.runtime.Composable
private fun WifiProvisionCard(
    expanded: Boolean,
    onToggleExpanded: () -> Unit,
    provisioningTarget: S31Device?,
    ssid: String,
    password: String,
    pop: String,
    wifiNetworks: List<S31WifiNetwork>,
    wifiListVisible: Boolean,
    onSsidChange: (String) -> Unit,
    onPasswordChange: (String) -> Unit,
    onPopChange: (String) -> Unit,
    onWifiSelected: (S31WifiNetwork) -> Unit,
    onScanWifi: () -> Unit,
    onProvision: () -> Unit,
) {
    SectionCard {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clickable(onClick = onToggleExpanded),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            CardHeader(iconRes = R.drawable.ic_wifi, title = "WiFi 配网", trailingIconRes = null)
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                Text(if (expanded) "收起" else "展开", color = Muted, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
                SmallIcon(
                    iconRes = if (expanded) R.drawable.ic_chevron_up else R.drawable.ic_chevron_down,
                    tint = Muted,
                )
            }
        }

        StepRow()

        if (!expanded) {
            Text("点击展开后填写 WiFi 信息并开始配网", color = Muted, fontSize = 16.sp, lineHeight = 22.sp)
        } else {
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Button(
                    onClick = onScanWifi,
                    enabled = provisioningTarget != null,
                    colors = ButtonDefaults.buttonColors(containerColor = Primary),
                    shape = RoundedCornerShape(8.dp),
                ) {
                    ButtonIcon(iconRes = R.drawable.ic_wifi, tint = Color.White)
                    Text("扫描 WiFi", fontSize = 15.sp)
                }
                Button(
                    onClick = onProvision,
                    enabled = provisioningTarget != null && ssid.isNotBlank(),
                    colors = ButtonDefaults.buttonColors(containerColor = Success),
                    shape = RoundedCornerShape(8.dp),
                ) {
                    ButtonIcon(iconRes = R.drawable.ic_check, tint = Color.White)
                    Text("开始配网", fontSize = 15.sp)
                }
            }

            if (wifiListVisible) {
                Column(verticalArrangement = Arrangement.spacedBy(7.dp)) {
                    Text("选择 WiFi", fontWeight = FontWeight.SemiBold, color = Ink, fontSize = 16.sp)
                    wifiNetworks.take(12).forEach { network ->
                        WifiNetworkRow(
                            network = network,
                            selected = network.ssid == ssid,
                            onClick = { onWifiSelected(network) },
                        )
                    }
                }
            }

            OutlinedTextField(
                value = ssid,
                onValueChange = onSsidChange,
                modifier = Modifier.fillMaxWidth(),
                label = { Text("WiFi 名称") },
                singleLine = true,
                shape = RoundedCornerShape(8.dp),
            )
            OutlinedTextField(
                value = password,
                onValueChange = onPasswordChange,
                modifier = Modifier.fillMaxWidth(),
                label = { Text("WiFi 密码") },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                shape = RoundedCornerShape(8.dp),
            )
            OutlinedTextField(
                value = pop,
                onValueChange = onPopChange,
                modifier = Modifier.fillMaxWidth(),
                label = { Text("配网验证码 PoP") },
                singleLine = true,
                shape = RoundedCornerShape(8.dp),
            )
        }
    }
}

@androidx.compose.runtime.Composable
private fun CardHeader(iconRes: Int, title: String, trailingIconRes: Int?) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        IconBadge(iconRes = iconRes, color = Primary, size = 38)
        Text(
            title,
            style = MaterialTheme.typography.titleLarge,
            fontSize = 22.sp,
            lineHeight = 26.sp,
            fontWeight = FontWeight.Bold,
            color = Ink,
            maxLines = 1,
        )
        if (trailingIconRes != null) {
            Image(
                painter = painterResource(trailingIconRes),
                contentDescription = null,
                modifier = Modifier.size(22.dp),
                colorFilter = ColorFilter.tint(Color(0xFF94A3B8)),
            )
        }
    }
}

@androidx.compose.runtime.Composable
private fun StepRow() {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(7.dp),
    ) {
        StepChip("1", "扫描设备", Modifier.weight(1f))
        StepChip("2", "选择 WiFi", Modifier.weight(1f))
        StepChip("3", "开始配网", Modifier.weight(1f))
    }
}

@androidx.compose.runtime.Composable
private fun StepChip(number: String, text: String, modifier: Modifier = Modifier) {
    Row(
        modifier = modifier
            .clip(RoundedCornerShape(8.dp))
            .background(Color(0xFFF1F5F9))
            .padding(horizontal = 7.dp, vertical = 7.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        NumberBadge(text = number, color = Primary, size = 18)
        Text(
            text,
            color = Ink,
            fontWeight = FontWeight.SemiBold,
            fontSize = 12.sp,
            lineHeight = 14.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@androidx.compose.runtime.Composable
private fun DeviceRow(
    device: S31Device,
    isBound: Boolean,
    onBind: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(8.dp))
            .background(Color(0xFFFAFCFF))
            .padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        IconBadge(iconRes = R.drawable.ic_ble, color = Primary, size = 46, soft = true)
        Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(5.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    device.name,
                    modifier = Modifier.weight(1f),
                    fontSize = 18.sp,
                    lineHeight = 22.sp,
                    fontWeight = FontWeight.Bold,
                    color = Ink,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                if (isBound) {
                    BoundBadge()
                } else if (!device.isProvisioningMode()) {
                    OutlinedButton(onClick = onBind, shape = RoundedCornerShape(8.dp)) {
                        Text("绑定", fontSize = 14.sp)
                    }
                }
            }
            Text(
                "${device.address}  |  ${device.rssi} dBm",
                color = Muted,
                fontSize = 14.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(device.modeLabel(), color = Muted, fontSize = 14.sp, maxLines = 1)
        }
    }
}

@androidx.compose.runtime.Composable
private fun BoundBadge() {
    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(8.dp))
            .background(Color(0xFFEAFBF3))
            .padding(horizontal = 10.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        IconBadge(iconRes = R.drawable.ic_check, color = Success, size = 20)
        Text("已绑定", color = Success, fontWeight = FontWeight.SemiBold, fontSize = 14.sp, maxLines = 1)
    }
}

@androidx.compose.runtime.Composable
private fun WifiNetworkRow(
    network: S31WifiNetwork,
    selected: Boolean,
    onClick: () -> Unit,
) {
    val bg = if (selected) Color(0xFFEFF6FF) else Color(0xFFFAFCFF)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(8.dp))
            .background(bg)
            .clickable(onClick = onClick)
            .padding(12.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(network.ssid, fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal, color = Ink)
        Text("${network.rssi} dBm", color = Muted)
    }
}

@androidx.compose.runtime.Composable
private fun TargetRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(8.dp))
            .background(Color(0xFFF3F7FC))
            .padding(14.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
            IconBadge(iconRes = R.drawable.ic_target, color = Primary, size = 28, soft = true)
            Text(label, color = Ink, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
        }
        Text(value, color = Ink, fontSize = 14.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@androidx.compose.runtime.Composable
private fun SectionCard(
    content: @androidx.compose.runtime.Composable ColumnScope.() -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = Color.White),
        elevation = CardDefaults.cardElevation(defaultElevation = 3.dp),
        shape = RoundedCornerShape(8.dp),
    ) {
        Column(
            modifier = Modifier.padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            content()
        }
    }
}

@androidx.compose.runtime.Composable
private fun EmptyState(text: String) {
    Text(
        text = text,
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(8.dp))
            .background(Color(0xFFFAFCFF))
            .padding(14.dp),
        color = Muted,
    )
}

@androidx.compose.runtime.Composable
private fun NumberBadge(
    text: String,
    color: Color,
    size: Int,
    soft: Boolean = false,
) {
    Box(
        modifier = Modifier
            .size(size.dp)
            .background(if (soft) Color(0xFFEFF6FF) else color, CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = text,
            color = if (soft) color else Color.White,
            fontWeight = FontWeight.Bold,
            fontSize = 12.sp,
        )
    }
}

@androidx.compose.runtime.Composable
private fun IconBadge(
    iconRes: Int,
    color: Color,
    size: Int,
    soft: Boolean = false,
) {
    Box(
        modifier = Modifier
            .size(size.dp)
            .background(if (soft) Color(0xFFEFF6FF) else color, CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        Image(
            painter = painterResource(iconRes),
            contentDescription = null,
            modifier = Modifier.size((size * 0.52f).dp),
            colorFilter = ColorFilter.tint(if (soft) color else Color.White),
        )
    }
}

@androidx.compose.runtime.Composable
private fun ButtonIcon(iconRes: Int, tint: Color) {
    Image(
        painter = painterResource(iconRes),
        contentDescription = null,
        modifier = Modifier
            .padding(end = 6.dp)
            .size(18.dp),
        colorFilter = ColorFilter.tint(tint),
    )
}

@androidx.compose.runtime.Composable
private fun SmallIcon(iconRes: Int, tint: Color) {
    Image(
        painter = painterResource(iconRes),
        contentDescription = null,
        modifier = Modifier.size(22.dp),
        colorFilter = ColorFilter.tint(tint),
    )
}

@androidx.compose.runtime.Composable
private fun StatusBar(status: String) {
    Surface(color = Color.White, tonalElevation = 2.dp) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 18.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            IconBadge(iconRes = R.drawable.ic_link, color = Success, size = 32)
            Text("状态", color = Ink, fontWeight = FontWeight.Bold, fontSize = 16.sp)
            Text("|", color = Color(0xFFCBD5E1))
            Text(status, color = Muted, fontSize = 15.sp, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

private fun S31Device.isProvisioningMode(): Boolean {
    return serviceUuids.any { it.lowercase() in BleConstants.PROVISIONING_SERVICE_UUIDS }
}

private fun S31Device.modeLabel(): String {
    return when {
        isProvisioningMode() -> "当前模式：WiFi 配网"
        serviceUuids.any { it.equals(BleConstants.PRESENCE_SERVICE_UUID.toString(), ignoreCase = true) } -> "当前模式：已绑定服务"
        else -> "当前模式：未知"
    }
}

private val PageBg = Color(0xFFF7FAFE)
private val Primary = Color(0xFF1D63E9)
private val Success = Color(0xFF18B26B)
private val Ink = Color(0xFF0F1B35)
private val Muted = Color(0xFF748094)

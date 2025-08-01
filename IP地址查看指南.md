# 📱 如何查看设备IP地址 - 简单指南

## 🔍 什么是IP地址？

IP地址就像设备在网络中的"门牌号"，是一串数字，例如：`192.168.1.100`

通过这个"门牌号"，您的手机或电脑就能找到TV-PRO设备，并访问它的设置页面。

## 🎯 最简单的查看方法

### 方法1：设备启动时查看（推荐新手）

1. **重启设备**
   - 断电后重新插电，或在网页点击"重启设备"

2. **观看屏幕**
   - 设备连接WiFi成功后，屏幕会显示5秒钟IP地址信息
   - 显示格式：
     ```
     System Ready!
     
     Device IP:
     192.168.1.100    ← 这就是IP地址
     
     Web Settings:
     http://192.168.1.100
     ```

3. **记录IP地址**
   - 用手机拍照记录，或用笔记下来

### 方法2：按键查看（最便捷）

1. **按右键4次**
   - 在设备屏幕上按右键（→）4次
   - 屏幕会切换到"网络信息页面"

2. **查看IP地址**
   - 屏幕显示：
     ```
     Network Info
     ─────────────
     WiFi: Connected
     
     Device IP:
      192.168.1.100   ← 大字显示
     
     Network: 您的WiFi名称
     Signal: -45 dBm (强)
     
     Web Settings:
     http://192.168.1.100
     ```

## 📱 使用IP地址访问设置页面

1. **打开手机浏览器**
   - 任何浏览器都可以（Safari、Chrome、微信内置浏览器等）

2. **输入地址**
   - 在地址栏输入：`http://192.168.1.100`
   - 注意：把`192.168.1.100`替换为您设备显示的实际IP地址

3. **访问设置页面**
   - 页面打开后，您就可以设置AM/PM格式和农历显示了

## 🔧 其他查看方法

### 方法3：路由器管理页面

1. **打开路由器管理页面**
   - 通常是：`192.168.1.1` 或 `192.168.0.1`

2. **登录路由器**
   - 输入管理员账号密码

3. **查看连接设备**
   - 找到"已连接设备"或"设备管理"
   - 寻找名为"TV-PRO"或包含"ESP"的设备

### 方法4：网络扫描工具

1. **下载网络扫描APP**
   - 安卓：Fing、IP Scanner等
   - iOS：Network Scanner、LanScan等

2. **扫描局域网**
   - 打开APP，点击扫描
   - 查找设备名称包含"TV-PRO"或"ESP"的设备

## ✅ 常见IP地址格式

设备IP地址通常是以下格式之一：
- `192.168.1.xxx`（如：192.168.1.100）
- `192.168.0.xxx`（如：192.168.0.100）  
- `10.0.0.xxx`（如：10.0.0.100）
- `172.16.0.xxx`（如：172.16.0.100）

其中`xxx`是1-254之间的数字。

## 🚨 常见问题

### ❓ 设备显示"WiFi: Disconnected"
**解决方法：**
1. 长按设备中键2秒，进入配置模式
2. 手机连接"TV-PRO"热点
3. 访问`192.168.4.1`重新配置WiFi

### ❓ 找不到IP地址或无法访问
**检查清单：**
1. ✅ 设备和手机连接到同一个WiFi网络
2. ✅ IP地址输入正确（包括所有点和数字）
3. ✅ 地址栏输入`http://`开头
4. ✅ 网络连接正常

### ❓ 浏览器显示"无法连接"
**解决方法：**
1. 检查设备是否正常显示时间和天气
2. 重启路由器和设备
3. 尝试不同的浏览器
4. 清除浏览器缓存

## 📞 快速帮助

**记住这个简单步骤：**
1. 📺 按右键4次 → 看屏幕显示的IP地址
2. 📱 打开手机浏览器 → 输入`http://IP地址`
3. ⚙️ 设置页面打开 → 配置AM/PM和农历显示

**示例完整流程：**
1. 设备屏幕显示IP：`192.168.1.100`
2. 手机浏览器输入：`http://192.168.1.100`
3. 打开设置页面，勾选需要的功能

就是这么简单！ 
<template>
  <div>
    <!-- Status bar -->
    <div class="status-bar" v-if="status">
      <span>设备IP：<strong>{{ status.ip }}</strong></span>
      <span>SIM：{{ status.simInitialized ? '✅ 已就绪' : (status.simPresent ? '⏳ 初始化中' : '❌ 未插入') }}</span>
      <span>本机号码：{{ status.devicePhone || '未知' }}</span>
    </div>

    <div v-if="saveMsg" class="alert" :class="saveMsg.ok ? 'alert-success' : 'alert-error'"
         style="margin-bottom:16px">
      {{ saveMsg.ok ? '✅' : '❌' }} {{ saveMsg.text }}
    </div>

    <!-- Web admin -->
    <div class="card">
      <div class="card-title">🔐 Web管理账号</div>
      <div class="alert alert-warning" style="margin-bottom:14px">
        ⚠️ 首次使用请修改默认密码！默认账号：{{ esp32.DEFAULT_WEB_USER }}，默认密码：{{ esp32.DEFAULT_WEB_PASS }}
      </div>
      <div class="form-group">
        <label>管理账号</label>
        <input type="text" v-model="form.webUser" placeholder="admin">
      </div>
      <div class="form-group">
        <label>管理密码</label>
        <input type="password" v-model="form.webPass" placeholder="请设置复杂密码">
      </div>
    </div>

    <!-- WiFi -->
    <div class="card">
      <div class="card-title">📡 WiFi 设置</div>
      <p style="font-size:13px;color:#888;margin:0 0 14px">
        修改WiFi配置保存后设备将自动重启并连接新WiFi。若连接失败将开启热点 <strong>SMS-Forwarder-AP</strong>。
      </p>
      <div class="form-group">
        <label>WiFi 名称 (SSID)</label>
        <input type="text" v-model="form.wifiSSID" placeholder="请输入WiFi名称">
      </div>
      <div class="form-group">
        <label>WiFi 密码</label>
        <input type="password" v-model="form.wifiPass" placeholder="请输入WiFi密码">
      </div>
    </div>

    <!-- Scheduled reboot -->
    <div class="card">
      <div class="card-title">⏰ 计划任务</div>
      <div class="form-group" style="display:flex;align-items:center;gap:10px">
        <input type="checkbox" id="autoRebootEnabled" v-model="form.autoRebootEnabled"
               style="width:auto;margin:0">
        <label for="autoRebootEnabled" style="margin:0;font-weight:normal;cursor:pointer">
          启用每日定时重启
        </label>
      </div>
      <div class="form-group">
        <label>重启时间（24小时制，北京时间）</label>
        <input type="time" v-model="form.autoRebootTime">
        <p style="font-size:12px;color:#888;margin:4px 0 0">
          建议设置在深夜（如 03:00），重启时将同时复位 ESP32 和 4G 模组。
        </p>
      </div>
    </div>

    <!-- Email -->
    <div class="card">
      <div class="card-title">📧 邮件通知设置</div>
      <div class="form-group">
        <label>SMTP服务器</label>
        <input type="text" v-model="form.smtpServer" placeholder="smtp.qq.com">
      </div>
      <div class="form-group">
        <label>SMTP端口</label>
        <input type="number" v-model.number="form.smtpPort" placeholder="465">
      </div>
      <div class="form-group">
        <label>邮箱账号</label>
        <input type="text" v-model="form.smtpUser" placeholder="your@qq.com">
      </div>
      <div class="form-group">
        <label>邮箱密码 / 授权码</label>
        <input type="password" v-model="form.smtpPass" placeholder="授权码">
      </div>
      <div class="form-group">
        <label>接收邮件地址</label>
        <input type="text" v-model="form.smtpSendTo" placeholder="receiver@example.com">
      </div>
    </div>

    <!-- Push channels -->
    <div class="card">
      <div class="card-title">🔗 HTTP推送通道设置</div>
      <p style="font-size:13px;color:#888;margin:0 0 14px">
        可同时启用多个推送通道，支持多种推送方式。
      </p>
      <PushChannelEditor
        v-for="(ch, i) in form.pushChannels"
        :key="i"
        :index="i"
        v-model="form.pushChannels[i]"
      />
    </div>

    <!-- Blacklist -->
    <div class="card">
      <div class="card-title">🚫 号码黑名单</div>
      <p style="font-size:13px;color:#888;margin:0 0 14px">
        每行一个号码，来自黑名单号码的短信将被忽略（不转发、不执行命令）。
      </p>
      <div class="form-group">
        <label>黑名单号码</label>
        <textarea v-model="form.numberBlackList" rows="5" placeholder="每行一个手机号"></textarea>
      </div>
    </div>

    <!-- Admin phone -->
    <div class="card">
      <div class="card-title">👤 管理员设置</div>
      <div class="form-group">
        <label>管理员手机号</label>
        <input type="text" v-model="form.adminPhone" placeholder="13800138000">
        <p style="font-size:12px;color:#888;margin:4px 0 0">管理员可通过短信发送 SMS:号码:内容 或 RESET 指令</p>
      </div>
    </div>

    <!-- Save button -->
    <button class="btn btn-primary btn-block" :disabled="saving" @click="save">
      {{ saving ? '⏳ 保存中...' : '💾 保存配置' }}
    </button>
    <div style="height:20px"></div>
  </div>
</template>

<script setup>
import { ref, onMounted, reactive } from 'vue'
import PushChannelEditor from '../components/PushChannelEditor.vue'
import { useApi } from '../composables/useApi.js'

const api   = useApi()
const esp32 = window.__ESP32_DATA__ || {}

const MAX_CH = esp32.MAX_PUSH_CHANNELS || 5

function emptyChannel(i) {
  return {
    enabled: false, type: 1, name: `通道${i+1}`,
    url: '', key1: '', key2: '', customBody: '', customCallBody: ''
  }
}

const status  = ref(null)
const saving  = ref(false)
const saveMsg = ref(null)

const form = reactive({
  webUser: '', webPass: '',
  wifiSSID: '', wifiPass: '',
  smtpServer: '', smtpPort: 465, smtpUser: '', smtpPass: '', smtpSendTo: '',
  adminPhone: '',
  numberBlackList: '',
  autoRebootEnabled: false,
  autoRebootTime: '03:00',
  pushChannels: Array.from({ length: MAX_CH }, (_, i) => emptyChannel(i))
})

onMounted(async () => {
  try {
    status.value = await api.getStatus()
    const cfg    = await api.getConfig()
    if (!cfg) return
      Object.assign(form, {
        webUser: cfg.webUser || '', webPass: cfg.webPass || '',
        wifiSSID: cfg.wifiSSID || '', wifiPass: cfg.wifiPass || '',
        smtpServer: cfg.smtpServer || '', smtpPort: cfg.smtpPort || 465,
        smtpUser: cfg.smtpUser || '', smtpPass: cfg.smtpPass || '',
        smtpSendTo: cfg.smtpSendTo || '',
        adminPhone: cfg.adminPhone || '',
        numberBlackList: cfg.numberBlackList || '',
        autoRebootEnabled: cfg.autoRebootEnabled || false,
        autoRebootTime: cfg.autoRebootTime || '03:00',
      })
    if (cfg.pushChannels?.length) {
      form.pushChannels = cfg.pushChannels.map((c, i) => ({
        ...emptyChannel(i), ...c
      }))
    }
  } catch (e) {
    console.error('加载配置失败', e)
  }
})

async function save() {
  saving.value  = true
  saveMsg.value = null
  try {
    const data = {
      webUser: form.webUser, webPass: form.webPass,
      wifiSSID: form.wifiSSID, wifiPass: form.wifiPass,
      smtpServer: form.smtpServer, smtpPort: form.smtpPort,
      smtpUser: form.smtpUser, smtpPass: form.smtpPass,
      smtpSendTo: form.smtpSendTo,
      adminPhone: form.adminPhone,
      numberBlackList: form.numberBlackList,
      autoRebootEnabled: form.autoRebootEnabled ? 'true' : 'false',
      autoRebootTime: form.autoRebootTime,
    }
    form.pushChannels.forEach((ch, i) => {
      data[`push${i}en`]    = ch.enabled ? 'on' : ''
      data[`push${i}type`]  = ch.type
      data[`push${i}name`]  = ch.name
      data[`push${i}url`]   = ch.url
      data[`push${i}key1`]  = ch.key1
      data[`push${i}key2`]  = ch.key2
      data[`push${i}body`]  = ch.customBody
      data[`push${i}cbody`] = ch.customCallBody
    })
    const res = await api.saveConfig(data)
    saveMsg.value = { ok: res?.success, text: res?.message || '保存成功' }
    if (res?.wifiChanged) {
      saveMsg.value.text += '（WiFi已修改，设备将重启…）'
    }
  } catch (e) {
    saveMsg.value = { ok: false, text: '请求失败: ' + e.message }
  } finally {
    saving.value = false
    window.scrollTo(0, 0)
  }
}
</script>


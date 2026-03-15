<template>
  <div>
    <!-- ── Firmware Info ─────────────────────────────────────────────────── -->
    <div class="card">
      <div class="card-title">📋 固件信息</div>
      <div class="info-table-wrap">
        <table class="info-table">
          <tr>
            <td>项目名称</td>
            <td>低成本短信转发器</td>
          </tr>
          <tr>
            <td>作者</td>
            <td><a :href="meta.repoUrl" target="_blank" rel="noopener">{{ meta.author }}</a></td>
          </tr>
          <tr>
            <td>GitHub</td>
            <td><a :href="meta.repoUrl" target="_blank" rel="noopener">{{ meta.repoUrl }}</a></td>
          </tr>
          <tr>
            <td>Commit ID</td>
            <td><code>{{ meta.commitId }}</code></td>
          </tr>
          <tr>
            <td>编译时间</td>
            <td>{{ meta.buildTime }}</td>
          </tr>
        </table>
      </div>
    </div>

    <!-- ── MCU Info ──────────────────────────────────────────────────────── -->
    <div class="card">
      <div class="card-title">🖥️ MCU 信息</div>
      <button class="btn btn-blue btn-sm" :disabled="sysInfoLoading" @click="loadSysInfo">
        {{ sysInfoLoading ? '⏳ 获取中...' : '🔄 刷新信息' }}
      </button>
      <div v-if="sysInfoError" class="alert alert-error" style="margin-top:10px">{{ sysInfoError }}</div>
      <div v-if="sysInfo" style="margin-top:12px">
        <div class="info-table-wrap">
          <table class="info-table">
            <tr>
              <td>芯片型号</td>
              <td>{{ sysInfo.chipModel }} (Rev {{ sysInfo.chipRevision }})</td>
            </tr>
            <tr>
              <td>唯一识别码</td>
              <td><code>{{ sysInfo.chipId }}</code></td>
            </tr>
            <tr>
              <td>CPU 主频</td>
              <td>{{ sysInfo.cpuFreqMHz }} MHz</td>
            </tr>
            <tr>
              <td>运行时长</td>
              <td>{{ sysInfo.uptime }}</td>
            </tr>
            <tr>
              <td>内存使用</td>
              <td>
                <div class="usage-bar-wrap">
                  <div class="usage-bar" :style="{ width: ramPct + '%', background: usageColor(ramPct) }"></div>
                </div>
                <span class="usage-label">{{ fmtKB(sysInfo.usedHeap) }} / {{ fmtKB(sysInfo.totalHeap) }} KB（{{ ramPct }}%）</span>
              </td>
            </tr>
            <tr>
              <td>Flash 占用</td>
              <td>
                <div class="usage-bar-wrap">
                  <div class="usage-bar" :style="{ width: flashPct + '%', background: usageColor(flashPct) }"></div>
                </div>
                <span class="usage-label">{{ fmtKB(sysInfo.sketchSize) }} / {{ fmtKB(sysInfo.totalFlash) }} KB（{{ flashPct }}%）</span>
              </td>
            </tr>
          </table>
        </div>
      </div>
    </div>

    <!-- ── Config Import / Export ────────────────────────────────────────── -->
    <div class="card">
      <div class="card-title">💾 配置导入 / 导出</div>
      <p style="font-size:13px;color:#888;margin:0 0 12px">
        导出当前配置为 JSON 文件，或从 JSON 文件导入配置（含推送通道、WiFi、密码等所有设置）。
        导入后如 WiFi 信息发生变化，设备会自动重启。
      </p>
      <div style="display:flex;gap:8px;flex-wrap:wrap">
        <button class="btn btn-blue btn-sm" :disabled="cfgExporting" @click="exportConfig">
          {{ cfgExporting ? '⏳ 导出中...' : '📤 导出配置' }}
        </button>
        <label class="btn btn-ghost btn-sm" style="cursor:pointer">
          📥 导入配置
          <input type="file" accept=".json,application/json" style="display:none" @change="importConfig">
        </label>
      </div>
      <div v-if="cfgResult" class="alert" :class="cfgResult.ok ? 'alert-success' : 'alert-error'"
           style="margin-top:10px">
        {{ cfgResult.ok ? '✅' : '❌' }} {{ cfgResult.message }}
      </div>
    </div>

    <!-- ── Reboot ────────────────────────────────────────────────────────── -->
    <div class="card">
      <div class="card-title">🔁 重启设备</div>
      <p style="font-size:13px;color:#888;margin:0 0 12px">
        点击按钮后设备将立即重启，重启期间无法收发短信，约 10–20 秒后恢复。
      </p>
      <button class="btn btn-red btn-block" :disabled="rebooting" @click="confirmReboot">
        {{ rebooting ? '⏳ 正在重启...' : '⚡ 立即重启' }}
      </button>
      <div v-if="rebootResult" class="alert" :class="rebootResult.ok ? 'alert-success' : 'alert-error'"
           style="margin-top:10px">
        {{ rebootResult.ok ? '✅' : '❌' }} {{ rebootResult.message }}
      </div>
    </div>

    <!-- ── Module info query ─────────────────────────────────────────────── -->
    <div class="card">
      <div class="card-title">📊 模组信息查询</div>
      <div style="display:flex;gap:8px;flex-wrap:wrap">
        <button class="btn btn-ghost btn-sm" @click="query('ati')">📋 IOT 固件信息</button>
        <button class="btn btn-ghost btn-sm" @click="query('signal')">📶 信号质量</button>
        <button class="btn btn-ghost btn-sm" @click="query('siminfo')">💳 SIM卡信息</button>
        <button class="btn btn-ghost btn-sm" @click="query('network')">🌍 网络状态</button>
        <button class="btn btn-ghost btn-sm" @click="query('wifi')">📡 WiFi状态</button>
      </div>
      <div v-if="queryResult" class="alert" :class="queryResult.ok ? 'alert-info' : 'alert-error'"
           style="margin-top:12px" v-html="queryResult.message"></div>
    </div>

    <!-- ── Send SMS ──────────────────────────────────────────────────────── -->
    <div class="card">
      <div class="card-title">📤 发送短信</div>
      <div class="form-group">
        <label>目标号码</label>
        <input type="text" v-model="smsForm.phone"
               placeholder='填写手机号，国际号码用 "+国家码" 前缀，如 +8612345678900'>
      </div>
      <div class="form-group">
        <label>短信内容（已输入 {{ smsForm.content.length }} 字符）</label>
        <textarea v-model="smsForm.content" rows="4" placeholder="请输入短信内容..."></textarea>
      </div>
      <button class="btn btn-blue btn-block" :disabled="smsSending" @click="sendSms">
        {{ smsSending ? '⏳ 发送中...' : '📨 发送短信' }}
      </button>
      <div v-if="smsResult" class="alert" :class="smsResult.ok ? 'alert-success' : 'alert-error'"
           style="margin-top:10px">
        {{ smsResult.ok ? '✅' : '❌' }} {{ smsResult.message }}
      </div>
    </div>

    <!-- ── Network test ──────────────────────────────────────────────────── -->
    <div class="card">
      <div class="card-title">🌐 网络测试（Ping）</div>
      <p style="font-size:13px;color:#888;margin:0 0 12px">
        将向 8.8.8.8 发起 Ping，一次性消耗极少流量。
      </p>
      <button class="btn btn-orange btn-block" :disabled="pinging" @click="confirmPing">
        {{ pinging ? '⏳ Ping 中（最多 35 秒）...' : '📡 开始 Ping 测试' }}
      </button>
      <div v-if="pingResult" class="alert" :class="pingResult.ok ? 'alert-success' : 'alert-error'"
           style="margin-top:10px">
        {{ pingResult.ok ? '✅' : '❌' }} {{ pingResult.message }}
      </div>
    </div>

    <!-- ── Flight mode ───────────────────────────────────────────────────── -->
    <div class="card">
      <div class="card-title">✈️ 模组控制（飞行模式）</div>
      <p style="font-size:13px;color:#888;margin:0 0 12px">
        飞行模式开启后模组将关闭射频，无法收发短信。
      </p>
      <div style="display:flex;gap:8px">
        <button class="btn btn-blue btn-sm" @click="flightQuery">🔍 查询状态</button>
        <button class="btn btn-red  btn-sm" @click="flightToggle">✈️ 切换飞行模式</button>
      </div>
      <div v-if="flightResult" class="alert" :class="flightResult.ok ? 'alert-info' : 'alert-error'"
           style="margin-top:10px" v-html="flightResult.message"></div>
    </div>

    <!-- ── AT debugger ───────────────────────────────────────────────────── -->
    <div class="card">
      <div class="card-title">💻 AT 指令调试</div>
      <div class="at-log" ref="atLogEl">
        <div v-for="(entry, i) in atLog" :key="i">
          <span :class="entry.type">{{ entry.prefix }}</span>{{ entry.text }}
        </div>
        <span v-if="!atLog.length" style="color:#555">等待输入指令...</span>
      </div>
      <div style="display:flex;gap:8px">
        <input type="text" v-model="atCmd" placeholder="输入 AT 指令，如: AT+CSQ"
               style="flex:1;font-family:monospace"
               @keydown.enter="sendAT">
        <button class="btn btn-blue btn-sm" :disabled="atBusy" @click="sendAT">
          {{ atBusy ? '...' : '发送' }}
        </button>
        <button class="btn btn-ghost btn-sm" @click="atLog = []">🧹 清空</button>
      </div>
    </div>
  </div>
</template>

<script setup>
/* global __ESP32_DATA__ */
import {ref, computed, nextTick} from 'vue'
import {useApi} from '../composables/useApi.js'

const api = useApi()

// ── Firmware metadata (injected at build time via Vite define) ────────────────
const _d = typeof __ESP32_DATA__ !== 'undefined' ? __ESP32_DATA__ : {}
const meta = {
  author: _d.AUTHOR || 'unknown',
  repoUrl: _d.REPO_URL || 'unknown',
  commitId: _d.GIT_COMMIT || 'unknown',
  buildTime: _d.BUILD_TIME || 'unknown',
}

// ── MCU Info ──────────────────────────────────────────────────────────────────
const sysInfo = ref(null)
const sysInfoLoading = ref(false)
const sysInfoError = ref(null)

const ramPct = computed(() => sysInfo.value
    ? Math.round(sysInfo.value.usedHeap / sysInfo.value.totalHeap * 100) : 0)
const flashPct = computed(() => sysInfo.value
    ? Math.round(sysInfo.value.sketchSize / sysInfo.value.totalFlash * 100) : 0)

function fmtKB(bytes) {
  return (bytes / 1024).toFixed(1)
}

function usageColor(pct) {
  if (pct >= 85) return '#f44336'
  if (pct >= 65) return '#FF9800'
  return '#4CAF50'
}

async function loadSysInfo() {
  sysInfoLoading.value = true;
  sysInfoError.value = null
  try {
    sysInfo.value = await api.getSysInfo()
  } catch (e) {
    sysInfoError.value = e.message
  } finally {
    sysInfoLoading.value = false
  }
}

// Load on mount
loadSysInfo()

// ── Reboot ────────────────────────────────────────────────────────────────────
const rebooting = ref(false)
const rebootResult = ref(null)

async function confirmReboot() {
  if (!confirm('确定要重启设备吗？\n重启期间无法收发短信，约 10–20 秒后恢复。')) return
  rebooting.value = true;
  rebootResult.value = null
  try {
    const r = await api.reboot()
    rebootResult.value = {ok: r?.success, message: r?.message || '重启指令已发送'}
  } catch (e) {
    rebootResult.value = {ok: false, message: e.message}
  } finally {
    rebooting.value = false
  }
}

// ── Config Export / Import ────────────────────────────────────────────────────
const cfgExporting = ref(false)
const cfgResult = ref(null)

async function exportConfig() {
  cfgExporting.value = true;
  cfgResult.value = null
  try {
    const data = await api.getConfig()
    if (!data) return
    const json = JSON.stringify(data, null, 2)
    const blob = new Blob([json], {type: 'application/json'})
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = 'sms_forwarding_config.json'
    a.click()
    URL.revokeObjectURL(url)
    cfgResult.value = {ok: true, message: '配置已导出'}
  } catch (e) {
    cfgResult.value = {ok: false, message: e.message}
  } finally {
    cfgExporting.value = false
  }
}

async function importConfig(evt) {
  const file = evt.target.files?.[0]
  evt.target.value = ''
  if (!file) return
  cfgResult.value = null
  try {
    const text = await file.text()
    const cfg = JSON.parse(text)

    // Flatten JSON config → form params expected by POST /api/config
    const fd = {}
    const topKeys = [
      'smtpServer', 'smtpPort', 'smtpUser', 'smtpPass', 'smtpSendTo',
      'adminPhone', 'webUser', 'webPass', 'wifiSSID', 'wifiPass',
      'numberBlackList', 'adminNote', 'deviceAlias', 'manualPhone',
      'autoRebootEnabled', 'autoRebootTime',
      'trafficKeepEnabled', 'trafficKeepIntervalHours', 'trafficKeepSizeKb',
    ]
    for (const k of topKeys) {
      if (cfg[k] !== undefined) fd[k] = cfg[k]
    }
    if (Array.isArray(cfg.pushChannels)) {
      cfg.pushChannels.forEach((ch, i) => {
        fd[`push${i}en`] = ch.enabled ? 'true' : 'false'
        fd[`push${i}type`] = ch.type ?? 0
        fd[`push${i}url`] = ch.url ?? ''
        fd[`push${i}name`] = ch.name ?? ''
        fd[`push${i}key1`] = ch.key1 ?? ''
        fd[`push${i}key2`] = ch.key2 ?? ''
        fd[`push${i}body`] = ch.customBody ?? ''
        fd[`push${i}cbody`] = ch.customCallBody ?? ''
      })
    }

    const r = await api.saveConfig(fd)
    cfgResult.value = {ok: r?.success, message: r?.message || '配置已导入'}
    if (r?.wifiChanged) {
      cfgResult.value.message += '（WiFi 已变更，设备将自动重启）'
    }
  } catch (e) {
    cfgResult.value = {ok: false, message: '文件解析失败：' + e.message}
  }
}

// ── Send SMS ──────────────────────────────────────────────────────────────────
const smsForm = ref({phone: '', content: ''})
const smsSending = ref(false)
const smsResult = ref(null)

async function sendSms() {
  if (!smsForm.value.phone || !smsForm.value.content) return
  smsSending.value = true;
  smsResult.value = null
  try {
    const r = await api.sendSms({phone: smsForm.value.phone, content: smsForm.value.content})
    smsResult.value = {ok: r?.success, message: r?.message}
  } catch (e) {
    smsResult.value = {ok: false, message: e.message}
  } finally {
    smsSending.value = false
  }
}

// ── Query ─────────────────────────────────────────────────────────────────────
const queryResult = ref(null)

async function query(type) {
  queryResult.value = null
  try {
    const r = await api.query(type)
    queryResult.value = {ok: r?.success, message: r?.message}
  } catch (e) {
    queryResult.value = {ok: false, message: e.message}
  }
}

// ── Ping ──────────────────────────────────────────────────────────────────────
const pinging = ref(false)
const pingResult = ref(null)

async function confirmPing() {
  if (!confirm('确定要执行 Ping 操作吗？\n这将消耗少量流量。')) return
  pinging.value = true;
  pingResult.value = null
  try {
    const r = await api.ping()
    pingResult.value = {ok: r?.success, message: r?.message}
  } catch (e) {
    pingResult.value = {ok: false, message: e.message}
  } finally {
    pinging.value = false
  }
}

// ── Flight mode ───────────────────────────────────────────────────────────────
const flightResult = ref(null)

async function flightQuery() {
  flightResult.value = null
  try {
    const r = await api.flight('query')
    flightResult.value = {ok: r?.success, message: r?.message}
  } catch (e) {
    flightResult.value = {ok: false, message: e.message}
  }
}

async function flightToggle() {
  if (!confirm('确定要切换飞行模式吗？\n开启后模组无法收发短信。')) return
  flightResult.value = null
  try {
    const r = await api.flight('toggle')
    flightResult.value = {ok: r?.success, message: r?.message}
  } catch (e) {
    flightResult.value = {ok: false, message: e.message}
  }
}

// ── AT debugger ───────────────────────────────────────────────────────────────
const atCmd = ref('')
const atBusy = ref(false)
const atLog = ref([])
const atLogEl = ref(null)

async function sendAT() {
  const cmd = atCmd.value.trim()
  if (!cmd) return
  atLog.value.push({type: 'at-cmd', prefix: '> ', text: cmd})
  atCmd.value = '';
  atBusy.value = true
  await nextTick()
  if (atLogEl.value) atLogEl.value.scrollTop = atLogEl.value.scrollHeight
  try {
    const r = await api.atCommand(cmd)
    atLog.value.push({
      type: r?.success ? 'at-resp' : 'at-err',
      prefix: '[RESP] ', text: r?.message || '无响应'
    })
  } catch (e) {
    atLog.value.push({type: 'at-err', prefix: '❌ ', text: e.message})
  } finally {
    atBusy.value = false
    await nextTick()
    if (atLogEl.value) atLogEl.value.scrollTop = atLogEl.value.scrollHeight
  }
}
</script>


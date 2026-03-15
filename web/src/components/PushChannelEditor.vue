<template>
  <div class="push-channel-card" :class="{ enabled: local.enabled }">
    <div class="push-channel-header" @click="local.enabled = !local.enabled">
      <input type="checkbox" :checked="local.enabled"
             @click.stop="local.enabled = !local.enabled" />
      <span class="channel-num">推送通道 {{ index + 1 }}</span>
      <span v-if="local.name" style="color:#888;font-size:13px">— {{ local.name }}</span>
    </div>

    <div v-if="local.enabled" class="push-channel-body">
      <!-- Channel name -->
      <div class="form-group">
        <label>通道名称</label>
        <input type="text" v-model="local.name" placeholder="自定义名称（便于识别）">
      </div>

      <!-- Push type selector -->
      <div class="form-group">
        <label>推送方式</label>
        <select v-model.number="local.type">
          <option v-for="pt in pushTypes" :key="pt.value" :value="pt.value">
            {{ pt.label }}
          </option>
        </select>
        <div v-if="typeInfo" class="push-type-hint">{{ typeInfo.hint }}</div>
      </div>

      <!-- URL (conditional label) -->
      <div v-if="typeInfo?.showUrl" class="form-group">
        <label>{{ typeInfo?.urlLabel || '推送URL/Webhook' }}</label>
        <input type="text" v-model="local.url"
               :placeholder="typeInfo?.urlPlaceholder || 'https://...'">
      </div>

      <!-- Key1 -->
      <div v-if="typeInfo?.showKey1" class="form-group">
        <label>{{ typeInfo?.key1Label || '参数1' }}</label>
        <input type="text" v-model="local.key1"
               :placeholder="typeInfo?.key1Placeholder || ''">
      </div>

      <!-- Key2 -->
      <div v-if="typeInfo?.showKey2" class="form-group">
        <label>{{ typeInfo?.key2Label || '参数2' }}</label>
        <input type="text" v-model="local.key2"
               :placeholder="typeInfo?.key2Placeholder || ''">
      </div>

      <!-- Custom body -->
      <div v-if="typeInfo?.showCustomBody" class="form-group">
        <label>请求体模板（使用 {sender} {sender_fmt} {message} {timestamp} {device} 占位符）</label>
        <textarea v-model="local.customBody" rows="4"
                  style="font-family:monospace;font-size:13px"
                  placeholder='{"key":"{sender}","value":"{message}"}'></textarea>
      </div>

      <!-- Call notification template (available for all types) -->
      <div class="form-group" style="margin-top:10px">
        <details>
          <summary style="cursor:pointer;color:#666;font-size:13px;user-select:none">
            📞 来电通知独立模板（可选，点击展开）
          </summary>
          <div style="margin-top:8px">
            <p style="font-size:12px;color:#888;margin:0 0 6px">
              留空则使用各推送类型内置格式。占位符：
              <code>{caller}</code> <code>{caller_fmt}</code>
              <code>{timestamp}</code> <code>{receiver}</code>
            </p>
            <textarea v-model="local.customCallBody" rows="3"
                      style="font-family:monospace;font-size:12px;width:100%"
                      placeholder='{"title":"📞来电通知","body":"来电：{caller_fmt}\n时间：{timestamp}"}'></textarea>
          </div>
        </details>
      </div>

      <!-- Test button -->
      <div style="display:flex;align-items:center;gap:12px;margin-top:8px">
        <button class="btn btn-orange btn-sm" :disabled="testing" @click="testChannel">
          {{ testing ? '⏳ 测试中...' : '🧪 测试推送' }}
        </button>
      </div>
      <div v-if="testResult" class="alert"
           :class="testResult.ok ? 'alert-success' : 'alert-error'"
           style="margin-top:8px">
        {{ testResult.ok ? '✅' : '❌' }} {{ testResult.message }}
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, watch } from 'vue'
import { useApi } from '../composables/useApi.js'

const props = defineProps({
  modelValue: { type: Object, required: true },
  index:      { type: Number, required: true }
})
const emit = defineEmits(['update:modelValue'])

const esp32 = window.__ESP32_DATA__ || {}

// ── UI metadata for each push type ───────────────────────────────────────────
// Keyed by the C++ enum key string from PushTypeMeta.def.
// esp32_config.json only carries raw {key, value} pairs — all display /
// field-visibility logic lives here in the frontend.
const PUSH_TYPE_UI = {
  PUSH_TYPE_NONE: {
    label: '禁用',
    hint: '',
    showUrl: false, showKey1: false, showKey2: false, showCustomBody: false,
  },
  PUSH_TYPE_POST_JSON: {
    label: 'POST JSON（通用格式）',
    hint: 'POST JSON格式：{"sender":"发送者号码","message":"短信内容","timestamp":"时间","device":"本机号码"}',
    showUrl: true,  urlLabel: '推送URL/Webhook',
    showKey1: false, showKey2: false, showCustomBody: false,
  },
  PUSH_TYPE_BARK: {
    label: 'Bark（iOS推送）',
    hint: 'Bark格式：POST {"title":"发送者号码","body":"短信内容"}',
    showUrl: true,  urlLabel: 'Bark推送URL',
    showKey1: false, showKey2: false, showCustomBody: false,
  },
  PUSH_TYPE_GET: {
    label: 'GET请求（参数在URL中）',
    hint: 'GET请求格式：URL?sender=xxx&message=xxx&timestamp=xxx&device=xxx',
    showUrl: true,  urlLabel: 'GET请求URL',
    showKey1: false, showKey2: false, showCustomBody: false,
  },
  PUSH_TYPE_DINGTALK: {
    label: '钉钉机器人',
    hint: '填写Webhook地址，如需加签请填Secret（key1）',
    showUrl: true,  urlLabel: 'Webhook地址',
    showKey1: true, key1Label: 'Secret（加签密钥，可选）', key1Placeholder: 'SEC...',
    showKey2: false, showCustomBody: false,
  },
  PUSH_TYPE_PUSHPLUS: {
    label: 'PushPlus',
    hint: '填写Token，URL留空使用默认 http://www.pushplus.plus/send',
    showUrl: true,  urlLabel: '推送URL（可留空）',
    showKey1: true, key1Label: 'Token',
    showKey2: true, key2Label: '群组编码（可选）',
    showCustomBody: false,
  },
  PUSH_TYPE_SERVERCHAN: {
    label: 'Server酱',
    hint: '填写SendKey（SCKEY）',
    showUrl: false,
    showKey1: true, key1Label: 'SendKey',
    showKey2: false, showCustomBody: false,
  },
  PUSH_TYPE_CUSTOM: {
    label: '自定义请求体',
    hint: '自定义POST请求，Body模板支持 {sender} {message} {timestamp} {device} 占位符',
    showUrl: true,  urlLabel: '推送URL/Webhook',
    showKey1: false, showKey2: false, showCustomBody: true,
  },
  PUSH_TYPE_FEISHU: {
    label: '飞书机器人',
    hint: '填写Webhook地址，如需加签请填Secret（key1）',
    showUrl: true,  urlLabel: 'Webhook地址',
    showKey1: true, key1Label: 'Secret（加签密钥，可选）', key1Placeholder: '',
    showKey2: false, showCustomBody: false,
  },
  PUSH_TYPE_GOTIFY: {
    label: 'Gotify',
    hint: '填写Gotify服务器地址及应用Token',
    showUrl: true,  urlLabel: 'Gotify服务器URL',
    showKey1: true, key1Label: 'Application Token',
    showKey2: false, showCustomBody: false,
  },
  PUSH_TYPE_TELEGRAM: {
    label: 'Telegram Bot',
    hint: '填写Bot Token和Chat ID',
    showUrl: false,
    showKey1: true, key1Label: 'Bot Token',
    showKey2: true, key2Label: 'Chat ID',
    showCustomBody: false,
  },
  PUSH_TYPE_WORK_WEIXIN: {
    label: '企业微信机器人',
    hint: '填写企业微信机器人Webhook地址',
    showUrl: true,  urlLabel: 'Webhook地址',
    showKey1: false, showKey2: false, showCustomBody: false,
  },
  PUSH_TYPE_SMS: {
    label: '短信转发（发送SMS）',
    hint: '填写目标手机号，将短信转发给指定号码',
    showUrl: false,
    showKey1: true, key1Label: '目标手机号',
    showKey2: false, showCustomBody: false,
  },
}

// Merge raw enum data from C++ with local UI metadata
const pushTypes = (esp32.PUSH_TYPES || [])
  .filter(pt => pt.key !== 'PUSH_TYPE_NONE')   // hide "none" from selector
  .map(pt => ({ ...pt, ...(PUSH_TYPE_UI[pt.key] || { label: pt.key }) }))

// Local copy so edits are reactive
const local = ref({ customCallBody: '', ...props.modelValue })

// When local changes, emit to parent
watch(local, v => emit('update:modelValue', { ...v }), { deep: true })

// When parent changes, sync to local — but only when values actually differ
// to avoid an infinite watch cycle: local → emit → props → local → emit → ...
watch(() => props.modelValue, v => {
  if (JSON.stringify(v) !== JSON.stringify(local.value)) {
    local.value = { ...v }
  }
}, { deep: true })

const typeInfo = computed(() => pushTypes.find(pt => pt.value === local.value.type))

// Test
const api     = useApi()
const testing    = ref(false)
const testResult = ref(null)

async function testChannel() {
  testing.value    = true
  testResult.value = null
  try {
    const data = {
      type: local.value.type,
      url:  local.value.url  || '',
      key1: local.value.key1 || '',
      key2: local.value.key2 || '',
      body: local.value.customBody || ''
    }
    const res = await api.testPush(data)
    testResult.value = { ok: res?.success, message: res?.message || '未知结果' }
  } catch (e) {
    testResult.value = { ok: false, message: '请求失败: ' + e.message }
  } finally {
    testing.value = false
  }
}
</script>

